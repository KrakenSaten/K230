// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"bytes"
	"context"
	"crypto/ecdsa"
	"crypto/tls"
	"net"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

const relayName = "relay.test"

// syncBuf collects the relay's log for the tests that read it.
type syncBuf struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *syncBuf) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.Write(p)
}

func (s *syncBuf) String() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.String()
}

// target is a dial address that a test can move (relay restarts).
type target struct{ v atomic.Value }

func newTarget(addr string) *target {
	t := &target{}
	t.v.Store(addr)
	return t
}

func (t *target) set(addr string) { t.v.Store(addr) }

func (t *target) dial(ctx context.Context) (net.Conn, error) {
	return fake.DialTCP(t.v.Load().(string))(ctx)
}

func fastTiming() timing {
	return timing{
		auth: 3 * time.Second, join: 2 * time.Second, ctlIdle: 5 * time.Second,
		ping: time.Second, write: 3 * time.Second, sessionIdle: 5 * time.Second,
		linger: 100 * time.Millisecond,
	}
}

type opts struct {
	key    *ecdsa.PrivateKey
	cert   *tls.Certificate
	backup *ecdsa.PrivateKey // its hash goes into the configuration
	noBack bool
	limits Limits
	timing *timing
	units  [][32]byte
}

type env struct {
	t      *testing.T
	s      *Server
	addr   string
	key    *ecdsa.PrivateKey
	backup *ecdsa.PrivateKey
	logs   *syncBuf
	to     *target
}

func listen(t *testing.T) net.Listener {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	return ln
}

// start runs a relay on ln.
func start(t *testing.T, ln net.Listener, o opts) *env {
	t.Helper()
	if o.key == nil {
		o.key = fake.NewKey()
	}
	if o.backup == nil && !o.noBack {
		o.backup = fake.NewKey()
	}
	cfg := &Config{Listen: ln.Addr().String(), Name: relayName, Port: 443, Limits: o.limits}
	if o.backup != nil {
		h := KeyFingerprint(&o.backup.PublicKey)
		cfg.BackupHash = wire.Hex(h[:])
	}
	for _, u := range o.units {
		cfg.Units = append(cfg.Units, wire.Hex(u[:]))
	}
	if cfg.Limits.PerSourceRate == 0 {
		cfg.Limits.PerSourceRate = 1000 // every test client is 127.0.0.1
	}
	cfg.Limits.fill()
	tm := fastTiming()
	if o.timing != nil {
		tm = *o.timing
	}
	cert := fake.SelfSigned(o.key, "doors-relay")
	if o.cert != nil {
		cert = *o.cert
	}
	logs := &syncBuf{}
	s := newServer(cfg, o.key, cert, logs, tm)
	go s.Serve(ln)
	e := &env{t: t, s: s, addr: ln.Addr().String(), key: o.key, backup: o.backup, logs: logs, to: newTarget(ln.Addr().String())}
	t.Cleanup(func() { s.Close() })
	return e
}

func ctx(t *testing.T) context.Context {
	c, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	t.Cleanup(cancel)
	return c
}

func newUnit(t *testing.T, to *target) *fake.Unit {
	u := fake.NewUnit(relayName, 443, to.dial)
	t.Cleanup(u.Disconnect)
	return u
}

// setUp is rule 28's set-up: probe, compare, confirm, backup; then connect.
func (e *env) setUp(u *fake.Unit) {
	e.t.Helper()
	got, err := u.Probe(ctx(e.t))
	if err != nil {
		e.t.Fatal(err)
	}
	if got != e.s.KeyHash() {
		e.t.Fatalf("probe shows %s, relay is %s", wire.Short(got), wire.Short(e.s.KeyHash()))
	}
	if err := u.Setup(ctx(e.t), got); err != nil {
		e.t.Fatal("setup:", err)
	}
	if err := u.Connect(ctx(e.t)); err != nil {
		e.t.Fatal("connect:", err)
	}
	waitFor(e.t, "control up", func() bool { return e.online(u.ID) })
}

func (e *env) online(id [32]byte) bool {
	e.s.mu.Lock()
	defer e.s.mu.Unlock()
	u := e.s.units[id]
	return u != nil && u.ctl != nil
}

// pairLocally pairs p with u as if over the LAN, and returns what the
// phone learned in gateway.hello.
func pairLocally(t *testing.T, u *fake.Unit, p *fake.Phone, remote bool) *fake.Record {
	t.Helper()
	var rec *fake.Record
	u.Set(func(u *fake.Unit) {
		u.Paired[p.FP] = true
		if remote {
			u.Remote[p.FP] = true
		}
		pin, id := *u.A, u.ConfigID
		rec = &fake.Record{DeviceID: u.ID, Name: u.Name, Port: u.Port, Pin: &pin, ConfigID: &id, Seq: u.Seq}
	})
	if ok, err := u.SendAdmit(); err != nil || !ok {
		t.Fatalf("admit not confirmed: %v", err)
	}
	return rec
}

func newPhone(to *target) *fake.Phone { return fake.NewPhone(to.dial) }

func waitFor(t *testing.T, what string, f func() bool) {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for !f() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

// rawStart dials the relay with TLS and the HTTP start, accepting any key.
func rawStart(t *testing.T, to *target) *fake.Conn {
	t.Helper()
	c, err := fake.DialRelay(ctx(t), to.dial, relayName, nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := c.Start(relayName, 443); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	return c
}

// rest reads everything the relay sends until it closes (at most 5 s).
func rest(c *fake.Conn) []byte {
	c.TLS.SetReadDeadline(time.Now().Add(5 * time.Second))
	var b bytes.Buffer
	b.ReadFrom(c.BR)
	return b.Bytes()
}

func contains(s string, subs ...string) bool {
	for _, x := range subs {
		if !strings.Contains(s, x) {
			return false
		}
	}
	return true
}
