// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"bufio"
	"bytes"
	"context"
	"crypto/tls"
	"errors"
	"io"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// tlsTo opens TLS to the relay with the given SNI and ALPN.
func tlsTo(t *testing.T, to *target, sni string, alpn []string) (*tls.Conn, error) {
	raw, err := to.dial(ctx(t))
	if err != nil {
		t.Fatal(err)
	}
	tc := tls.Client(raw, &tls.Config{ServerName: sni, NextProtos: alpn, InsecureSkipVerify: true, MinVersion: tls.VersionTLS13})
	tc.SetDeadline(time.Now().Add(5 * time.Second))
	if err := tc.Handshake(); err != nil {
		raw.Close()
		return nil, err
	}
	t.Cleanup(func() { tc.Close() })
	return tc, nil
}

// Threat model §5.10 and rule 39: anything but the exact Upgrade request,
// with SNI and ALPN, gets the ordinary 404 and the end of the connection.
func TestNotFound(t *testing.T) {
	w := newWorld(t, opts{})
	start := string(wire.StartRequest(relayName, 443))
	cases := map[string]struct {
		sni  string
		alpn []string
		req  string
	}{
		"plain GET":     {relayName, []string{"http/1.1"}, "GET / HTTP/1.1\r\nHost: relay.test\r\n\r\n"},
		"favicon":       {relayName, []string{"http/1.1"}, "GET /favicon.ico HTTP/1.1\r\nHost: relay.test\r\nUser-Agent: x\r\n\r\n"},
		"POST":          {relayName, []string{"http/1.1"}, strings.Replace(start, "GET", "POST", 1)},
		"OPTIONS *":     {relayName, []string{"http/1.1"}, "OPTIONS * HTTP/1.1\r\nHost: relay.test\r\n\r\n"},
		"websocket":     {relayName, []string{"http/1.1"}, strings.Replace(start, "doors-relay/1\r\n\r\n", "websocket\r\n\r\n", 1)},
		"no SNI":        {"", []string{"http/1.1"}, start},
		"other SNI":     {"other.test", []string{"http/1.1"}, start},
		"no ALPN":       {relayName, nil, start},
		"pipelined":     {relayName, []string{"http/1.1"}, start + "\x00\x00\x00\x10{\"type\":\"ready\"}"},
		"too long":      {relayName, []string{"http/1.1"}, "GET /doors-relay/1 HTTP/1.1\r\nX: " + strings.Repeat("a", 2100) + "\r\n\r\n"},
		"HTTP/1.0":      {relayName, []string{"http/1.1"}, strings.Replace(start, "HTTP/1.1", "HTTP/1.0", 1)},
		"not HTTP":      {relayName, []string{"http/1.1"}, "\x16\x03\x01 hello\r\n\r\n"},
		"wrong Host":    {relayName, []string{"http/1.1"}, strings.Replace(start, "Host: relay.test", "Host: relay.test.evil", 1)},
		"extra header":  {relayName, []string{"http/1.1"}, strings.Replace(start, "\r\n\r\n", "\r\nCookie: a=b\r\n\r\n", 1)},
		"doubled start": {relayName, []string{"http/1.1"}, start + start},
	}
	for name, c := range cases {
		tc, err := tlsTo(t, w.to, c.sni, c.alpn)
		if err != nil {
			t.Fatalf("%s: handshake: %v", name, err)
		}
		tc.Write([]byte(c.req))
		got, _ := io.ReadAll(tc)
		if !bytes.Equal(got, wire.NotFound) {
			t.Errorf("%s: answered %q", name, got)
		}
	}
	// ALPN for HTTP/2 only fails the handshake; plain TCP gets no HTTP.
	if _, err := tlsTo(t, w.to, relayName, []string{"h2"}); err == nil {
		t.Error("h2-only ALPN accepted")
	}
	raw, _ := w.to.dial(ctx(t))
	defer raw.Close()
	raw.SetDeadline(time.Now().Add(5 * time.Second))
	raw.Write([]byte("GET / HTTP/1.1\r\nHost: relay.test\r\n\r\n"))
	got, _ := io.ReadAll(raw)
	if bytes.Contains(got, []byte("HTTP/1.1")) {
		t.Errorf("plain TCP answered %q", got)
	}
}

// connectProxy is an explicit HTTP proxy: CONNECT, then a TCP splice to
// the relay (any target name maps to it, as DNS would).
func connectProxy(t *testing.T, relayAddr string) string {
	ln := listen(t)
	t.Cleanup(func() { ln.Close() })
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func() {
				defer c.Close()
				br := bufio.NewReader(c)
				head, err := wire.ReadHead(br)
				if err != nil || !strings.HasPrefix(string(head), "CONNECT relay.test:443 HTTP/1.1\r\n") {
					return
				}
				up, err := net.Dial("tcp", relayAddr)
				if err != nil {
					return
				}
				defer up.Close()
				c.Write([]byte("HTTP/1.1 200 Connection established\r\n\r\n"))
				go io.Copy(up, br)
				io.Copy(c, up)
			}()
		}
	}()
	return ln.Addr().String()
}

func viaProxy(proxy string) fake.Dialer {
	return func(ctx context.Context) (net.Conn, error) {
		c, err := fake.DialTCP(proxy)(ctx)
		if err != nil {
			return nil, err
		}
		c.Write([]byte("CONNECT relay.test:443 HTTP/1.1\r\nHost: relay.test:443\r\n\r\n"))
		br := bufio.NewReader(c)
		head, err := wire.ReadHead(br)
		if err != nil || !strings.HasPrefix(string(head), "HTTP/1.1 200") || br.Buffered() != 0 {
			c.Close()
			return nil, errors.New("proxy refused")
		}
		return c, nil
	}
}

// Threat model §5.10: a phone behind an explicit HTTP proxy reaches its
// unit with CONNECT; the relay sees the proxy, the pins are unchanged.
func TestThroughConnectProxy(t *testing.T) {
	w := newWorld(t, opts{})
	p := fake.NewPhone(viaProxy(connectProxy(t, w.addr)))
	rec := pairLocally(t, w.u, p, true)
	s, err := p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if got, err := s.Echo("proxied"); err != nil || got != "proxied" {
		t.Fatal(err)
	}
}

// mitm terminates TLS with its own key and opens its own TLS to the relay,
// as an intercepting middlebox does.
func mitm(t *testing.T, relayAddr string) string {
	ln := listen(t)
	t.Cleanup(func() { ln.Close() })
	cert := fake.SelfSigned(fake.NewKey(), relayName)
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func() {
				defer c.Close()
				in := tls.Server(c, &tls.Config{Certificates: []tls.Certificate{cert}, NextProtos: []string{"http/1.1"}})
				if in.Handshake() != nil {
					return
				}
				up, err := tls.Dial("tcp", relayAddr, &tls.Config{ServerName: relayName, NextProtos: []string{"http/1.1"}, InsecureSkipVerify: true})
				if err != nil {
					return
				}
				defer up.Close()
				go io.Copy(up, in)
				io.Copy(in, up)
			}()
		}
	}()
	return ln.Addr().String()
}

// Threat model §5.10: interception fails closed. The phone sees a key it
// has not pinned, takes the provisional path, and gets no endorsement for
// the interceptor's key: nothing but the start and the bound lookup left
// it, and it does not connect.
func TestInterceptionFailsClosed(t *testing.T) {
	w := newWorld(t, opts{})
	p := fake.NewPhone(fake.DialTCP(mitm(t, w.addr)))
	rec := pairLocally(t, w.u, p, true)
	_, err := p.Connect(ctx(t), rec, nil)
	if !errors.Is(err, fake.ErrUnavailable) {
		t.Fatalf("through an interceptor: %v", err)
	}
	if *rec.Pin != w.s.KeyHash() {
		t.Fatal("the phone re-pinned")
	}
}
