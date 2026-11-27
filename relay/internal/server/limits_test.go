// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"net"
	"net/netip"
	"testing"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// closedAtOnce reports whether the relay closed a fresh TCP connection
// without a TLS handshake (refused at accept).
func closedAtOnce(t *testing.T, addr string) bool {
	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	_, err = c.Read(make([]byte, 1))
	ne, ok := err.(net.Error)
	return !(ok && ne.Timeout())
}

func TestPerSourceRate(t *testing.T) {
	ln := listen(t)
	e := start(t, ln, opts{limits: Limits{PerSourceRate: 10, PerSourceUnauth: 100}})
	var conns []net.Conn
	for i := 0; i < 10; i++ {
		c, err := net.Dial("tcp", e.addr)
		if err != nil {
			t.Fatal(err)
		}
		conns = append(conns, c)
	}
	defer func() {
		for _, c := range conns {
			c.Close()
		}
	}()
	time.Sleep(50 * time.Millisecond)
	if !closedAtOnce(t, e.addr) {
		t.Fatal("11th connection in a second accepted")
	}
	time.Sleep(1100 * time.Millisecond)
	if closedAtOnce(t, e.addr) {
		t.Fatal("refused after the bucket refilled")
	}
}

func TestPerSourceUnauthenticated(t *testing.T) {
	ln := listen(t)
	e := start(t, ln, opts{limits: Limits{PerSourceRate: 1000, PerSourceUnauth: 3}})
	var conns []net.Conn
	for i := 0; i < 3; i++ {
		c, _ := net.Dial("tcp", e.addr)
		conns = append(conns, c)
	}
	time.Sleep(50 * time.Millisecond)
	if !closedAtOnce(t, e.addr) {
		t.Fatal("4th unauthenticated connection accepted")
	}
	for _, c := range conns {
		c.Close()
	}
	waitFor(t, "release", func() bool { return !closedAtOnce(t, e.addr) })
}

func TestGlobalLimit(t *testing.T) {
	ln := listen(t)
	e := start(t, ln, opts{limits: Limits{PerSourceRate: 1000, PerSourceUnauth: 100, MaxConns: 2}})
	a, _ := net.Dial("tcp", e.addr)
	b, _ := net.Dial("tcp", e.addr)
	defer a.Close()
	defer b.Close()
	time.Sleep(50 * time.Millisecond)
	if !closedAtOnce(t, e.addr) {
		t.Fatal("3rd connection accepted")
	}
}

// An authenticated phone leaves the unauthenticated count: a source with a
// limit of 2 still opens new connections while two sessions run.
func TestAuthenticatedLeaveTheCount(t *testing.T) {
	w := newWorld(t, opts{limits: Limits{PerSourceRate: 1000, PerSourceUnauth: 4}})
	p := newPhone(w.to)
	rec := pairLocally(t, w.u, p, true)
	s, err := p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	// Control (1), the session's client and data (2): none count now.
	var held []net.Conn
	for i := 0; i < 4; i++ {
		c, _ := net.Dial("tcp", w.addr)
		held = append(held, c)
	}
	defer func() {
		for _, c := range held {
			c.Close()
		}
	}()
	time.Sleep(50 * time.Millisecond)
	if !closedAtOnce(t, w.addr) {
		t.Fatal("5th unauthenticated accepted")
	}
}

func TestSourceOf(t *testing.T) {
	for addr, want := range map[string]string{
		"192.0.2.7:443":                   "192.0.2.7/32",
		"[::ffff:192.0.2.7]:443":          "192.0.2.7/32",
		"[2001:db8:1:2:3:4:5:6]:443":      "2001:db8:1:2::/64",
		"[2001:db8:1:2:ffff:4:5:6]:40000": "2001:db8:1:2::/64",
	} {
		ap := netip.MustParseAddrPort(addr)
		got := sourceOf(net.TCPAddrFromAddrPort(ap))
		if got.String() != want {
			t.Errorf("%s: %s", addr, got)
		}
	}
}

// The unit's control connection: more than 10 frames a second, a frame
// above 4 KiB or a malformed one closes it; and login plus register must
// come within the first seconds.
func TestControlAbuse(t *testing.T) {
	tm := fastTiming()
	tm.auth = 700 * time.Millisecond
	w := newWorld(t, opts{timing: &tm})
	login := func(register bool) *fake.Conn {
		c := rawStart(t, w.to)
		sig, _ := wire.Sign(w.u.Key, wire.UnitLoginMsg(c.Nonce, c.Presented))
		c.Send(&wire.UnitLogin{SPKI: w.u.SPKI, Sig: sig})
		if register {
			c.Send(&wire.Register{Endorsement: w.u.Endorse()})
		}
		return c
	}
	closed := func(c *fake.Conn) bool {
		c.TLS.SetReadDeadline(time.Now().Add(3 * time.Second))
		for {
			if _, err := wire.ReadMsg(c.BR); err != nil {
				ne, ok := err.(net.Error)
				return !(ok && ne.Timeout())
			}
		}
	}
	c := login(true)
	for i := 0; i < 30; i++ {
		c.Send(&wire.Ping{N: uint32(i)})
	}
	if !closed(c) {
		t.Error("frame flood not closed")
	}
	c = login(true)
	big := make([]byte, wire.MaxFrame+1)
	c.TLS.Write(wire.AppendFrame(nil, big))
	if !closed(c) {
		t.Error("oversized frame not closed")
	}
	c = login(true)
	c.TLS.Write(wire.AppendFrame(nil, []byte(`{"type":"ping","n":1,"n":2}`)))
	if !closed(c) {
		t.Error("malformed frame not closed")
	}
	c = login(true)
	c.Send(&wire.Which{Lookup: [32]byte{}})
	if !closed(c) {
		t.Error("a client frame on control not closed")
	}
	c = login(false) // never registers
	if !closed(c) {
		t.Error("unregistered control connection not closed")
	}
	c = rawStart(t, w.to) // never logs in
	if !closed(c) {
		t.Error("silent connection not closed")
	}
}

// register carries an endorsement of this relay, as configured, for the
// key it presents, signed by the unit that logged in; anything else
// closes the connection, and the unit stays offline.
func TestRegisterRefusesForeignEndorsement(t *testing.T) {
	ln := listen(t)
	to := newTarget(ln.Addr().String())
	u := newUnit(t, to)
	e := start(t, ln, opts{units: [][32]byte{u.ID}})
	other := fake.NewUnit(relayName, 443, to.dial)
	var cid, seqN uint64 = 5, 1
	mk := func(signer *fake.Unit, f func(*wire.Endorsement)) wire.SignedEndorsement {
		en := wire.Endorsement{ConfigID: cid, Seq: seqN, Name: relayName, Port: 443, KeyHash: e.s.KeyHash()}
		f(&en)
		b, _ := en.Marshal()
		sig, _ := wire.Sign(signer.Key, b)
		return wire.SignedEndorsement{E: b, Sig: sig}
	}
	same := func(*wire.Endorsement) {}
	for name, se := range map[string]wire.SignedEndorsement{
		"other key":   mk(u, func(x *wire.Endorsement) { x.KeyHash = [32]byte{1} }),
		"other name":  mk(u, func(x *wire.Endorsement) { x.Name = "other.test" }),
		"other port":  mk(u, func(x *wire.Endorsement) { x.Port = 8443 }),
		"other unit":  mk(other, same),
		"not E bytes": {E: append([]byte("DOORS relay endorsement v2\x00"), make([]byte, 60)...), Sig: mk(u, same).Sig},
	} {
		c := rawStart(t, to)
		sig, _ := wire.Sign(u.Key, wire.UnitLoginMsg(c.Nonce, c.Presented))
		c.Send(&wire.UnitLogin{SPKI: u.SPKI, Sig: sig})
		c.Send(&wire.Register{Endorsement: se})
		if got := rest(c); len(got) != 0 {
			t.Errorf("%s: answered %q", name, got)
		}
		if e.online(u.ID) {
			t.Errorf("%s: unit online", name)
		}
	}
	c := rawStart(t, to)
	sig, _ := wire.Sign(u.Key, wire.UnitLoginMsg(c.Nonce, c.Presented))
	c.Send(&wire.UnitLogin{SPKI: u.SPKI, Sig: sig})
	c.Send(&wire.Register{Endorsement: mk(u, same)})
	waitFor(t, "registered", func() bool { return e.online(u.ID) })
}

// The relay pings an idle unit, and closes after the silence limit.
func TestControlPingAndSilence(t *testing.T) {
	tm := fastTiming()
	tm.ping, tm.ctlIdle = 200*time.Millisecond, 600*time.Millisecond
	w := newWorld(t, opts{timing: &tm})
	c := rawStart(t, w.to)
	sig, _ := wire.Sign(w.u.Key, wire.UnitLoginMsg(c.Nonce, c.Presented))
	c.Send(&wire.UnitLogin{SPKI: w.u.SPKI, Sig: sig})
	c.Send(&wire.Register{Endorsement: w.u.Endorse()})
	c.TLS.SetReadDeadline(time.Now().Add(2 * time.Second))
	m, err := wire.ReadMsg(c.BR)
	if _, ok := m.(*wire.Ping); err != nil || !ok {
		t.Fatalf("no ping: %T %v", m, err)
	}
	t0 := time.Now()
	for {
		if _, err := wire.ReadMsg(c.BR); err != nil {
			break
		}
	}
	if d := time.Since(t0); d > 1500*time.Millisecond {
		t.Fatalf("silent unit kept %v", d)
	}
}
