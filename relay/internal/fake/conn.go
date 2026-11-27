// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

// Package fake holds reference units and phones for tests: Go stand-ins
// that make exactly the checks docs/api/relay.md requires of gatewayd, its
// helper and the app's RelayTransport. They are test code; the real ends
// are phases 2 and 3.
package fake

import (
	"bufio"
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"errors"
	"io"
	"math/big"
	"net"
	"sync"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// Dialer opens a TCP connection to the relay (directly, or through a proxy).
type Dialer func(ctx context.Context) (net.Conn, error)

// DialTCP is a Dialer for an address.
func DialTCP(addr string) Dialer {
	return func(ctx context.Context) (net.Conn, error) {
		var d net.Dialer
		return d.DialContext(ctx, "tcp", addr)
	}
}

var (
	// ErrPin is a relay whose leaf key is not one the caller accepts.
	ErrPin = errors.New("fake: relay key not accepted")
	// ErrUnavailable is the relay's refusal.
	ErrUnavailable = errors.New("fake: unavailable")
	// ErrUnexpected is any other frame than the one expected.
	ErrUnexpected = errors.New("fake: unexpected frame")
)

// Conn is one connection to a relay after its TLS handshake.
type Conn struct {
	TLS           *tls.Conn
	BR            *bufio.Reader
	Presented     [32]byte // SHA-256 of the leaf's SubjectPublicKeyInfo
	PresentedSPKI []byte
	Nonce         [32]byte // the relay's challenge

	mu      sync.Mutex
	written []byte // everything written through Send/Write, for tests
}

// leafCheck is the relay pin rule (rule 31): the leaf certificate alone
// decides. Its key must be ECDSA P-256 and accepted by accept; the rest of
// a chain, dates, names and issuers are ignored.
func leafCheck(accept func([32]byte) bool, out *Conn) func(tls.ConnectionState) error {
	return func(cs tls.ConnectionState) error {
		if len(cs.PeerCertificates) == 0 {
			return ErrPin
		}
		leaf := cs.PeerCertificates[0]
		pub, ok := leaf.PublicKey.(*ecdsa.PublicKey)
		if !ok || pub.Curve != elliptic.P256() {
			return ErrPin
		}
		h := sha256.Sum256(leaf.RawSubjectPublicKeyInfo)
		if accept != nil && !accept(h) {
			return ErrPin
		}
		out.Presented = h
		out.PresentedSPKI = append([]byte(nil), leaf.RawSubjectPublicKeyInfo...)
		return nil
	}
}

// DialRelay is TLS as the unit's helper and the phone dial a relay:
// TLS 1.3 only, SNI = name, ALPN http/1.1, and the leaf-only pin check.
// accept nil accepts any P-256 leaf (a probe, or the phone's provisional
// path).
func DialRelay(ctx context.Context, dial Dialer, name string, accept func([32]byte) bool) (*Conn, error) {
	raw, err := dial(ctx)
	if err != nil {
		return nil, err
	}
	c := &Conn{}
	cfg := &tls.Config{
		ServerName:             name,
		MinVersion:             tls.VersionTLS13,
		NextProtos:             []string{wire.ALPN},
		InsecureSkipVerify:     true, // replaced by the leaf-only pin check below
		VerifyConnection:       leafCheck(accept, c),
		SessionTicketsDisabled: true,
	}
	tc := tls.Client(raw, cfg)
	hctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	if err := tc.HandshakeContext(hctx); err != nil {
		raw.Close()
		return nil, err
	}
	if tc.ConnectionState().NegotiatedProtocol != wire.ALPN {
		raw.Close()
		return nil, errors.New("fake: no ALPN http/1.1")
	}
	c.TLS = tc
	c.BR = bufio.NewReaderSize(tc, 2*wire.MaxFrame)
	return c, nil
}

func (c *Conn) record(b []byte) {
	c.mu.Lock()
	c.written = append(c.written, b...)
	c.mu.Unlock()
}

// Written is a copy of everything written so far.
func (c *Conn) Written() []byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([]byte(nil), c.written...)
}

// Start sends the HTTP start and reads the 101 and the challenge.
func (c *Conn) Start(name string, port uint16) error {
	req := wire.StartRequest(name, port)
	c.record(req)
	c.TLS.SetDeadline(time.Now().Add(10 * time.Second))
	if _, err := c.TLS.Write(req); err != nil {
		return err
	}
	if err := wire.ReadSwitching(c.BR); err != nil {
		return err
	}
	m, err := c.Recv()
	if err != nil {
		return err
	}
	ch, ok := m.(*wire.Challenge)
	if !ok {
		return ErrUnexpected
	}
	c.Nonce = ch.Nonce
	return nil
}

// Send writes one frame.
func (c *Conn) Send(m wire.Msg) error {
	b := wire.AppendFrame(nil, wire.Encode(m))
	c.record(b)
	c.TLS.SetWriteDeadline(time.Now().Add(10 * time.Second))
	_, err := c.TLS.Write(b)
	return err
}

// Recv reads one frame; Unavailable becomes ErrUnavailable.
func (c *Conn) Recv() (wire.Msg, error) {
	m, err := wire.ReadMsg(c.BR)
	if err != nil {
		return nil, err
	}
	if _, ok := m.(*wire.Unavailable); ok {
		return m, ErrUnavailable
	}
	return m, nil
}

// Stream is the connection after `ready`, for the inner TLS.
func (c *Conn) Stream() net.Conn {
	c.TLS.SetDeadline(time.Time{})
	return &bufConn{Conn: c.TLS, r: c.BR, c: c}
}

// Close closes the connection.
func (c *Conn) Close() error { return c.TLS.Close() }

type bufConn struct {
	net.Conn
	r *bufio.Reader
	c *Conn
}

func (b *bufConn) Read(p []byte) (int, error) { return b.r.Read(p) }
func (b *bufConn) Write(p []byte) (int, error) {
	b.c.record(p)
	return b.Conn.Write(p)
}

// SelfSigned is a certificate container for a key, with the fixed fields
// of PAIRING.md §2.
func SelfSigned(key *ecdsa.PrivateKey, cn string) tls.Certificate {
	serial, _ := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 127))
	tmpl := &x509.Certificate{
		SerialNumber: serial,
		Subject:      pkix.Name{CommonName: cn},
		NotBefore:    time.Unix(0, 0).UTC(),
		NotAfter:     time.Date(9999, 12, 31, 23, 59, 59, 0, time.UTC),
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		panic(err)
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}
}

// NewKey makes a P-256 key.
func NewKey() *ecdsa.PrivateKey {
	k, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		panic(err)
	}
	return k
}

// exactlyOnePin is the gateway's rule (ADR-019 rule 11): one certificate,
// a P-256 key, whose fingerprint accept takes.
func exactlyOnePin(accept func([32]byte) bool, got *[32]byte) func(tls.ConnectionState) error {
	return func(cs tls.ConnectionState) error {
		if len(cs.PeerCertificates) != 1 {
			return ErrPin
		}
		pub, ok := cs.PeerCertificates[0].PublicKey.(*ecdsa.PublicKey)
		if !ok || pub.Curve != elliptic.P256() {
			return ErrPin
		}
		h := sha256.Sum256(cs.PeerCertificates[0].RawSubjectPublicKeyInfo)
		if !accept(h) {
			return ErrPin
		}
		if got != nil {
			*got = h
		}
		return nil
	}
}

// readLine reads one '\n'-terminated line of at most 256 bytes.
func readLine(r *bufio.Reader) (string, error) {
	var b []byte
	for len(b) <= 256 {
		c, err := r.ReadByte()
		if err != nil {
			return "", err
		}
		if c == '\n' {
			return string(b), nil
		}
		b = append(b, c)
	}
	return "", io.ErrShortBuffer
}
