// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

// T1-T18 of the resolution of the Codex review (RESOLUTION-review-1.md
// §11). The race points are held with hooks and an injected clock, never
// with sleeps that hope.

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/liar"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// gate stops the relay once at a named point each time it is armed.
type gate struct {
	point   string
	mu      sync.Mutex
	armed   bool
	reached chan any
	release chan struct{}
}

func newGate(point string) *gate {
	return &gate{point: point, reached: make(chan any, 1)}
}

func (g *gate) arm() {
	g.mu.Lock()
	g.armed, g.release = true, make(chan struct{})
	g.mu.Unlock()
}

func (g *gate) take() (chan struct{}, bool) {
	g.mu.Lock()
	defer g.mu.Unlock()
	if !g.armed {
		return nil, false
	}
	g.armed = false
	return g.release, true
}

func (g *gate) wait(t *testing.T) any {
	t.Helper()
	select {
	case v := <-g.reached:
		return v
	case <-time.After(10 * time.Second):
		t.Fatalf("the relay never reached %q", g.point)
	}
	return nil
}

func (g *gate) open() {
	g.mu.Lock()
	close(g.release)
	g.mu.Unlock()
}

func hooks(gs []*gate, also func(string, any)) func(string, any) {
	return func(p string, v any) {
		if also != nil {
			also(p, v)
		}
		for _, g := range gs {
			if p != g.point {
				continue
			}
			if rel, ok := g.take(); ok {
				g.reached <- v
				<-rel
			}
		}
	}
}

// slowTiming keeps raw test controls from going idle.
func slowTiming() *timing {
	tm := fastTiming()
	tm.ctlIdle, tm.ping, tm.auth, tm.join = 30*time.Second, 20*time.Second, 30*time.Second, 5*time.Second
	return &tm
}

// rawRelay is a relay with n enrolled units whose control connections the
// test drives by hand.
func rawRelay(t *testing.T, o opts, n int) (*env, *target, []*fake.Unit) {
	t.Helper()
	ln := listen(t)
	to := newTarget(ln.Addr().String())
	var us []*fake.Unit
	for i := 0; i < n; i++ {
		u := fake.NewUnit(relayName, 443, to.dial)
		us = append(us, u)
		o.units = append(o.units, u.ID)
	}
	if o.timing == nil {
		o.timing = slowTiming()
	}
	e := start(t, ln, o)
	for _, u := range us {
		u.Set(func(x *fake.Unit) {
			a := e.s.KeyHash()
			x.A, x.ConfigID, x.Seq = &a, 7, 1
		})
	}
	return e, to, us
}

// ctlLogin opens a control connection and logs in.
func ctlLogin(t *testing.T, to *target, u *fake.Unit) *fake.Conn {
	t.Helper()
	c := rawStart(t, to)
	sig, _ := wire.Sign(u.Key, wire.UnitLoginMsg(c.Nonce, c.Presented))
	if err := c.Send(&wire.UnitLogin{SPKI: u.SPKI, Sig: sig}); err != nil {
		t.Fatal(err)
	}
	return c
}

// ctlRegister logs in, registers with admit, and waits for `registered`.
func ctlRegister(t *testing.T, to *target, u *fake.Unit, admit ...[32]byte) *fake.Conn {
	t.Helper()
	c := ctlLogin(t, to, u)
	if err := c.Send(&wire.Register{Admit: admit, Endorsement: u.Endorse()}); err != nil {
		t.Fatal(err)
	}
	next(t, c, "registered")
	return c
}

// next reads frames, past the relay's pings, until one of type want.
func next(t *testing.T, c *fake.Conn, want string) wire.Msg {
	t.Helper()
	c.TLS.SetReadDeadline(time.Now().Add(10 * time.Second))
	for {
		m, err := wire.ReadMsg(c.BR)
		if err != nil {
			t.Fatalf("waiting for %s: %v", want, err)
		}
		if _, ping := m.(*wire.Ping); ping && want != "ping" {
			continue
		}
		if m.Type() != want {
			t.Fatalf("waiting for %s, got %s", want, m.Type())
		}
		return m
	}
}

var syncN atomic.Uint32

// syncCtl returns once the relay has handled every frame sent before it.
func syncCtl(t *testing.T, c *fake.Conn) {
	t.Helper()
	n := syncN.Add(1) + 1000
	c.Send(&wire.Ping{N: n})
	c.TLS.SetReadDeadline(time.Now().Add(10 * time.Second))
	for {
		m, err := wire.ReadMsg(c.BR)
		if err != nil {
			t.Fatal("sync:", err)
		}
		if p, ok := m.(*wire.Pong); ok && p.N == n {
			return
		}
	}
}

// phoneConnect sends a phone's login and connect; the answer is read later.
func phoneConnect(t *testing.T, to *target, p *fake.Phone, dev [32]byte, slot *[16]byte, extra []byte) *fake.Conn {
	t.Helper()
	c := rawStart(t, to)
	sig, _ := wire.Sign(p.Key, wire.ClientLoginMsg(c.Nonce, c.Presented, dev))
	b := wire.AppendFrame(nil, wire.Encode(&wire.ClientLogin{SPKI: p.SPKI, DeviceID: dev, Sig: sig}))
	b = wire.AppendFrame(b, wire.Encode(&wire.Connect{Slot: slot}))
	if _, err := c.TLS.Write(append(b, extra...)); err != nil {
		t.Fatal(err)
	}
	return c
}

// answer reads the relay's answer to a connect or a join.
func answer(t *testing.T, c *fake.Conn) wire.Msg {
	t.Helper()
	c.TLS.SetReadDeadline(time.Now().Add(10 * time.Second))
	m, err := wire.ReadMsg(c.BR)
	if err != nil {
		t.Fatal("answer:", err)
	}
	return m
}

// joinConn opens a data connection for cookie, sending extra after the
// join, and reads its answer.
func joinConn(t *testing.T, to *target, cookie [16]byte, extra []byte) (*fake.Conn, wire.Msg) {
	t.Helper()
	c := rawStart(t, to)
	if _, err := c.TLS.Write(append(wire.AppendFrame(nil, wire.Encode(&wire.Join{Cookie: cookie})), extra...)); err != nil {
		t.Fatal(err)
	}
	return c, answer(t, c)
}

// closedNow reports whether the relay has already closed c: a read ends
// with EOF or a reset, not a timeout.
func closedNow(c *fake.Conn, within time.Duration) bool {
	c.TLS.SetReadDeadline(time.Now().Add(within))
	_, err := c.BR.ReadByte()
	var ne net.Error
	return err != nil && !(errors.As(err, &ne) && ne.Timeout())
}

func readN(t *testing.T, c *fake.Conn, n int) string {
	t.Helper()
	c.TLS.SetReadDeadline(time.Now().Add(5 * time.Second))
	b := make([]byte, n)
	if _, err := io.ReadFull(c.BR, b); err != nil {
		t.Fatalf("reading %d bytes: %v", n, err)
	}
	return string(b)
}

func isType(m wire.Msg, typ string) bool { return m != nil && m.Type() == typ }

// runningSplice sets up a running splice of p at u through control k.
func runningSplice(t *testing.T, to *target, k *fake.Conn, p *fake.Phone, u *fake.Unit, slot *[16]byte) (pc, dc *fake.Conn) {
	t.Helper()
	pc = phoneConnect(t, to, p, u.ID, slot, nil)
	inv := next(t, k, "invite").(*wire.Invite)
	dc, m := joinConn(t, to, inv.Cookie, nil)
	if !isType(m, "ready") || !isType(answer(t, pc), "ready") {
		t.Fatal("no splice")
	}
	return pc, dc
}

func forwards(t *testing.T, from, to *fake.Conn, msg string) {
	t.Helper()
	if _, err := from.TLS.Write([]byte(msg)); err != nil {
		t.Fatal(err)
	}
	if got := readN(t, to, len(msg)); got != msg {
		t.Fatalf("forwarded %q", got)
	}
}

// T1: a join after the invitation's deadline, before the phone's timer
// has fired, is refused, and the phone is answered at once.
func TestT1JoinAfterDeadline(t *testing.T) {
	var skew atomic.Int64
	e, to, us := rawRelay(t, opts{now: func() time.Time { return time.Now().Add(time.Duration(skew.Load())) }}, 1)
	u, p := us[0], newPhone(e.to)
	k := ctlRegister(t, to, u, p.FP)
	pc := phoneConnect(t, to, p, u.ID, nil, nil)
	inv := next(t, k, "invite").(*wire.Invite)
	skew.Store(int64(6 * time.Second)) // past the 5 s deadline; the phone's real 5 s timer runs on
	t0 := time.Now()
	if got := joinRaw(t, to, inv.Cookie); !bytes.Equal(got, unavailable) {
		t.Fatalf("late join answered %q", got)
	}
	if !isType(answer(t, pc), "unavailable") {
		t.Fatal("phone not refused")
	}
	if d := time.Since(t0); d > 2*time.Second {
		t.Fatalf("the refusal waited for the phone's timer: %v", d)
	}
}

// T2: a join with a cookie issued on a superseded control is refused,
// also when it arrives inside the replacement (between its commit and
// its `registered`).
func TestT2JoinOnSupersededControl(t *testing.T) {
	g := newGate("register-committed")
	e, to, us := rawRelay(t, opts{hook: hooks([]*gate{g}, nil)}, 1)
	u, p := us[0], newPhone(e.to)
	k1 := ctlRegister(t, to, u, p.FP)
	pc := phoneConnect(t, to, p, u.ID, nil, nil)
	inv := next(t, k1, "invite").(*wire.Invite)
	k2 := ctlRegister(t, to, u, p.FP)
	// The replacement itself ended the invitation: its phone is answered
	// now, not by its 5 s timer, and before anyone tries the cookie.
	pc.TLS.SetReadDeadline(time.Now().Add(time.Second))
	if m, err := wire.ReadMsg(pc.BR); err != nil || !isType(m, "unavailable") {
		t.Fatalf("phone not refused at the replacement: %v", err)
	}
	if got := joinRaw(t, to, inv.Cookie); !bytes.Equal(got, unavailable) {
		t.Fatalf("join after the replacement: %q", got)
	}

	pc = phoneConnect(t, to, p, u.ID, nil, nil)
	inv = next(t, k2, "invite").(*wire.Invite)
	g.arm()
	k3 := ctlLogin(t, to, u)
	k3.Send(&wire.Register{Admit: [][32]byte{p.FP}, Endorsement: u.Endorse()})
	g.wait(t) // the replacement is committed; its effects are not yet sent
	if got := joinRaw(t, to, inv.Cookie); !bytes.Equal(got, unavailable) {
		t.Fatalf("join inside the replacement: %q", got)
	}
	g.open()
	next(t, k3, "registered")
	if !isType(answer(t, pc), "unavailable") {
		t.Fatal("phone not refused inside the replacement")
	}
}

// T3: a slot change and a slot clear end pending `pair` invitations; a
// running candidate splice survives a clear and ends at replacement.
func TestT3SlotChangeAndClear(t *testing.T) {
	e, to, us := rawRelay(t, opts{}, 1)
	u, q := us[0], newPhone(e.to)
	k := ctlRegister(t, to, u)
	slot := func(b byte) *[16]byte { return &[16]byte{b} }

	k.Send(&wire.PairSlot{Slot: slot(1)})
	syncCtl(t, k)
	pc := phoneConnect(t, to, q, u.ID, slot(1), nil)
	inv := next(t, k, "invite").(*wire.Invite)
	if inv.Via != wire.ViaPair {
		t.Fatalf("via %q", inv.Via)
	}
	t0 := time.Now()
	k.Send(&wire.PairSlot{Slot: slot(2)}) // a new window
	syncCtl(t, k)
	// Re-review F4 (mutation MD): the change itself ends the invitation and
	// answers the phone, before anyone tries the cookie and long before the
	// invitation's 5 s expiry; a join after it is refused.
	pc.TLS.SetReadDeadline(time.Now().Add(time.Second))
	if m, err := wire.ReadMsg(pc.BR); err != nil || !isType(m, "unavailable") {
		t.Fatalf("slot change kept the invitation (%v after the change): %v", time.Since(t0), err)
	}
	if got := joinRaw(t, to, inv.Cookie); !bytes.Equal(got, unavailable) {
		t.Fatalf("join after a slot change: %q", got)
	}

	pc = phoneConnect(t, to, q, u.ID, slot(2), nil)
	inv = next(t, k, "invite").(*wire.Invite)
	k.Send(&wire.PairSlot{}) // the window closes
	if !isType(answer(t, pc), "unavailable") {
		t.Fatal("slot clear kept the invitation")
	}
	if got := joinRaw(t, to, inv.Cookie); !bytes.Equal(got, unavailable) {
		t.Fatalf("join after a slot clear: %q", got)
	}

	k.Send(&wire.PairSlot{Slot: slot(3)})
	syncCtl(t, k)
	pc, dc := runningSplice(t, to, k, q, u, slot(3))
	k.Send(&wire.PairSlot{})
	syncCtl(t, k)
	forwards(t, pc, dc, "still the candidate's")
	forwards(t, dc, pc, "final response")
	ctlRegister(t, to, u)
	if !closedNow(dc, time.Second) || !closedNow(pc, time.Second) {
		t.Fatal("candidate splice outlived the replacement")
	}
}

// T4: revocation between `joined` and forwarding: `admitted` comes after
// both legs are closed, and no byte crosses.
func TestT4RevocationDuringReady(t *testing.T) {
	g := newGate("ready-sent")
	e, to, us := rawRelay(t, opts{hook: hooks([]*gate{g}, nil)}, 1)
	u, p := us[0], newPhone(e.to)
	k := ctlRegister(t, to, u, p.FP)
	g.arm()
	pc := phoneConnect(t, to, p, u.ID, nil, nil)
	inv := next(t, k, "invite").(*wire.Invite)
	dc, m := joinConn(t, to, inv.Cookie, nil)
	if !isType(m, "ready") || !isType(answer(t, pc), "ready") {
		t.Fatal("no ready")
	}
	g.wait(t) // ready is on both legs; forwarding has not started
	k.Send(&wire.Admit{N: 1})
	if a := next(t, k, "admitted").(*wire.Admitted); a.N != 1 {
		t.Fatalf("admitted %d", a.N)
	}
	pc.TLS.Write([]byte("after admitted"))
	if !closedNow(dc, 100*time.Millisecond) || !closedNow(pc, 100*time.Millisecond) {
		t.Fatal("a leg was open when admitted came")
	}
	g.open()
}

// T5: revocation while forwarding is blocked (the phone does not read, so
// a copy goroutine is stuck in Write): `admitted` comes only after both
// legs are closed, within a bound.
func TestT5RevocationWhileBlocked(t *testing.T) {
	e, to, us := rawRelay(t, opts{}, 1)
	u, p := us[0], newPhone(e.to)
	k := ctlRegister(t, to, u, p.FP)
	pc, dc := runningSplice(t, to, k, p, u, nil)
	stuck := make(chan struct{})
	go func() {
		buf := make([]byte, 64<<10)
		for {
			dc.TLS.SetWriteDeadline(time.Now().Add(300 * time.Millisecond))
			if _, err := dc.TLS.Write(buf); err != nil {
				close(stuck)
				return
			}
		}
	}()
	select {
	case <-stuck: // the buffers are full: the relay's pump to the phone is in Write
	case <-time.After(20 * time.Second):
		t.Fatal("never blocked")
	}
	t0 := time.Now()
	k.Send(&wire.Admit{N: 1})
	next(t, k, "admitted")
	if d := time.Since(t0); d > 2*time.Second {
		t.Fatalf("admitted after %v", d)
	}
	if !closedNow(dc, 100*time.Millisecond) {
		t.Fatal("data leg open at admitted")
	}
	pc.TLS.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := io.Copy(io.Discard, pc.BR); err != nil {
		var ne net.Error
		if errors.As(err, &ne) && ne.Timeout() {
			t.Fatal("client leg open at admitted")
		}
	}
}

// T6: an `admit` handled for a superseded control is not acknowledged;
// `admitted` carries its update's n; a non-increasing n closes.
func TestT6StaleAcknowledgements(t *testing.T) {
	g := newGate("admit-committed")
	acks := make(chan bool, 8)
	e, to, us := rawRelay(t, opts{hook: hooks([]*gate{g}, func(p string, v any) {
		if p == "admit-acked" {
			acks <- v.(bool)
		}
	})}, 1)
	u := us[0]
	_ = e
	k1 := ctlRegister(t, to, u)
	for _, n := range []uint32{5, 9} {
		k1.Send(&wire.Admit{N: n})
		if a := next(t, k1, "admitted").(*wire.Admitted); a.N != n {
			t.Fatalf("admitted %d for %d", a.N, n)
		}
		<-acks
	}
	g.arm()
	k1.Send(&wire.Admit{N: 10})
	g.wait(t) // committed on k1, not yet acknowledged
	k2 := ctlRegister(t, to, u)
	g.open()
	if <-acks {
		t.Fatal("an update of a superseded control was acknowledged")
	}
	k2.Send(&wire.Admit{N: 3}) // a new control may start anywhere
	next(t, k2, "admitted")
	k2.Send(&wire.Admit{N: 3})
	if !closedNow(k2, 2*time.Second) {
		t.Fatal("a non-increasing n did not close the connection")
	}
}

// T7: a decline from a superseded control is ignored.
func TestT7DeclineFromSupersededControl(t *testing.T) {
	uid, phone := [32]byte{1}, [32]byte{2}
	key := fake.NewKey()
	cfg := &Config{Name: relayName, Port: 443, Units: []string{wire.Hex(uid[:])}}
	cfg.Limits.fill()
	s := newServer(cfg, key, fake.SelfSigned(key, "doors-relay"), io.Discard, fastTiming())
	defer s.Close()
	un := s.units[uid]
	mk := func() *ctl {
		a, b := net.Pipe()
		t.Cleanup(func() { a.Close(); b.Close() })
		return &ctl{s: s, c: &rconn{raw: a}, u: un, out: make(chan []byte, 32),
			done: make(chan struct{}), writerDone: make(chan struct{})}
	}
	k1, k2 := mk(), mk()
	s.register(k1, [][32]byte{phone}, nil)
	s.register(k2, [][32]byte{phone}, nil)
	a, b := net.Pipe()
	defer a.Close()
	defer b.Close()
	sp, _ := s.admitSplice(uid, phone, nil, &rconn{raw: a})
	if sp == nil {
		t.Fatal("not admitted")
	}
	s.decline(k1, sp.cookie)
	if st, _ := sp.stateNow(); st != invited {
		t.Fatal("a superseded control's decline counted")
	}
	s.decline(k2, sp.cookie)
	if st, why := sp.stateNow(); st != stopped || why != reasonDeclined {
		t.Fatal("the current control's decline did not count")
	}
}

// T8: `registered` comes after the replacement's effects: the earlier
// control is closed, a listed splice survives if and only if its phone is
// on the new list, and a candidate's splice ends.
func TestT8Registered(t *testing.T) {
	g := newGate("register-effects-done")
	e, to, us := rawRelay(t, opts{hook: hooks([]*gate{g}, nil)}, 1)
	u, p, q := us[0], newPhone(e.to), newPhone(e.to)
	// held registers a new control and stops the relay just before it
	// sends `registered`: the effects must be done, and nothing sent yet.
	held := func(admit ...[32]byte) *fake.Conn {
		g.arm()
		k := ctlLogin(t, to, u)
		k.Send(&wire.Register{Admit: admit, Endorsement: u.Endorse()})
		g.wait(t)
		k.TLS.SetReadDeadline(time.Now().Add(100 * time.Millisecond))
		if m, err := wire.ReadMsg(k.BR); err == nil {
			t.Fatalf("%s sent before the replacement's effects", m.Type())
		}
		return k
	}
	release := func(k *fake.Conn) { g.open(); next(t, k, "registered") }

	k1 := ctlRegister(t, to, u, p.FP)
	pc, dc := runningSplice(t, to, k1, p, u, nil)
	k2 := held(p.FP)
	if !closedNow(k1, 100*time.Millisecond) {
		t.Fatal("earlier control open before registered")
	}
	release(k2)
	forwards(t, pc, dc, "survived")

	k3 := held(q.FP)
	if !closedNow(dc, 100*time.Millisecond) || !closedNow(pc, 100*time.Millisecond) {
		t.Fatal("splice of a phone off the new list open before registered")
	}
	release(k3)

	slot := &[16]byte{7}
	k3.Send(&wire.PairSlot{Slot: slot})
	syncCtl(t, k3)
	qc, qd := runningSplice(t, to, k3, q, u, slot)
	k4 := held(q.FP) // q is listed now, but its splice was a candidate's
	if !closedNow(qd, 100*time.Millisecond) || !closedNow(qc, 100*time.Millisecond) {
		t.Fatal("candidate splice open before registered")
	}
	release(k4)
}

// T9: through the real pairing flow (fake unit, fake phone, a relay that
// withholds `admitted`): pairing is durable and the outcome `pending`; a
// reconnect sends the snapshot and gets `admitted`; a retry makes no
// second entry; revocation and re-pairing without remote do not revive
// the grant; and the two-file rule's three start cases.
func TestT9PairingCommitPendingRecovery(t *testing.T) {
	lw := newLiarWorld(t, liar.Lies{WithholdAdmitted: true})
	u, l := lw.u, lw.l
	dir := t.TempDir()
	u.Dir = dir
	p := newPhone(lw.to)
	pairedKeys := func() []string {
		b, err := os.ReadFile(filepath.Join(dir, "paired.v1"))
		if err != nil {
			t.Fatal(err)
		}
		entries, err := fake.PairedFile(b)
		if err != nil {
			t.Fatal(err)
		}
		var keys []string
		for _, e := range entries {
			keys = append(keys, e.Key)
		}
		return keys
	}
	relayFile := func() string {
		b, _ := os.ReadFile(filepath.Join(dir, "relay.v1"))
		return string(b)
	}
	pFP := wire.Hex(p.FP[:])

	tok, _ := u.OpenWindow(true, true)
	slot := wire.SlotFromToken(tok)
	rec := firstPairing(u)
	s, err := p.Connect(ctx(t), rec, &slot)
	if err != nil || s.Role != "candidate" {
		t.Fatalf("candidate: %v", err)
	}
	t0 := time.Now()
	if got, err := s.Pair(tok); err != nil || got != "paired remote pending" {
		t.Fatalf("pairing without admitted: %q %v", got, err)
	}
	if d := time.Since(t0); d < 3*time.Second || d > 5*time.Second {
		t.Fatalf("waited %v for admitted", d)
	}
	s.Close()
	if keys := pairedKeys(); len(keys) != 1 || keys[0] != pFP {
		t.Fatalf("paired.v1: %v", keys)
	}
	if !strings.Contains(relayFile(), pFP) || u.RelayState(p.FP) != "pending" {
		t.Fatal("grant not durable, or not pending")
	}

	for i := 0; i < 2; i++ { // the app's retries: ordinary sessions of a paired key
		s, err = p.Connect(ctx(t), rec, nil)
		if err != nil || s.Role != "paired" || s.RelayState != "pending" {
			t.Fatalf("retry %d: %v %q %q", i, err, s.Role, s.RelayState)
		}
		s.Close()
	}
	if keys := pairedKeys(); len(keys) != 1 {
		t.Fatalf("a retry made a second entry: %v", keys)
	}

	l.SetWithholdAdmitted(false)
	u.Disconnect()
	if err := u.Connect(ctx(t)); err != nil { // registered, then the snapshot
		t.Fatal(err)
	}
	if u.RelayState(p.FP) != "admitted" {
		t.Fatal("snapshot after registered not admitted")
	}
	s, err = p.Connect(ctx(t), rec, nil)
	if err != nil || s.RelayState != "admitted" {
		t.Fatalf("after recovery: %v %q", err, s.RelayState)
	}
	s.Close()

	if ok, err := u.Revoke(p.FP); err != nil || !ok {
		t.Fatalf("revoke: %v", err)
	}
	tok, _ = u.OpenWindow(true, false)
	slot = wire.SlotFromToken(tok)
	rec = firstPairing(u)
	s, err = p.Connect(ctx(t), rec, &slot)
	if err != nil {
		t.Fatal(err)
	}
	if got, _ := s.Pair(tok); got != "paired" {
		t.Fatalf("re-pairing: %q", got)
	}
	s.Close()
	if u.RelayState(p.FP) != "off" || strings.Contains(relayFile(), pFP) {
		t.Fatal("revocation and re-pairing revived the grant")
	}
	if _, err := p.Connect(ctx(t), rec, nil); err == nil {
		t.Fatal("re-paired phone without remote reached the unit through the relay")
	}

	// The two-file rule's start cases (RELAY_PROPOSAL.md §6.1): "after"
	// uses the grants after; "before" means the change was interrupted and
	// did not happen, so the grants before; "neither" drops every grant.
	load := func() *fake.Unit {
		v := fake.NewUnit(relayName, 443, nil)
		v.Dir = dir
		if err := v.Load(); err != nil {
			t.Fatal(err)
		}
		return v
	}
	crashing := func(on bool) { u.Set(func(x *fake.Unit) { x.CrashAfterRelayWrite = on }) }
	pairAgain := func(ph *fake.Phone, rec *fake.Record, remote bool) (string, error) {
		tok, _ := u.OpenWindow(true, remote)
		slot := wire.SlotFromToken(tok)
		var s *fake.Session
		var err error
		if rec.Pin == nil {
			s, err = ph.Connect(ctx(t), rec, &slot)
		} else {
			s, err = ph.Connect(ctx(t), rec, nil) // a paired phone with remote
		}
		if err != nil {
			t.Fatal(err)
		}
		defer s.Close()
		return s.Pair(tok)
	}

	if ok, _ := u.SetRemote(p.FP, true); !ok {
		t.Fatal("grant not confirmed")
	}
	if v := load(); !v.Remote[p.FP] || !v.Paired[p.FP] || v.Notice != "" {
		t.Fatal(`"after": the grants after`)
	}

	crashing(true)
	q := newPhone(lw.to)
	if _, err := pairAgain(q, firstPairing(u), true); err == nil {
		t.Fatal("a crashed pairing answered")
	}
	if v := load(); v.Paired[q.FP] || v.Remote[q.FP] || !v.Remote[p.FP] || v.Notice != "" {
		t.Fatal(`"before", interrupted pairing: the grants before, nothing for the new key`)
	}
	// Fail-closed (re-review F5): an interrupted change keeps only grants
	// that were there both before and after it.
	u.Revoke(p.FP)
	if v := load(); !v.Paired[p.FP] || v.Remote[p.FP] || !strings.Contains(v.Notice, "interrupted") {
		t.Fatal(`"before", interrupted revocation: still paired, its grant taken away`)
	}
	if _, err := pairAgain(p, rec, false); err == nil {
		t.Fatal("a crashed pairing again answered")
	}
	if v := load(); !v.Paired[p.FP] || v.Remote[p.FP] {
		t.Fatal(`"before", interrupted pairing again of a known key without remote: no grant`)
	}
	crashing(false)
	if got, err := pairAgain(p, rec, false); err != nil || got != "paired" {
		t.Fatalf("pairing again without remote: %q %v", got, err)
	}
	if keys := pairedKeys(); len(keys) != 1 {
		t.Fatalf("pairing again made a second entry: %v", keys)
	}
	if v := load(); !v.Paired[p.FP] || v.Remote[p.FP] || v.Notice != "" {
		t.Fatal(`"after", pairing again without remote: no grant`)
	}

	if ok, _ := u.SetRemote(p.FP, true); !ok || !load().Remote[p.FP] {
		t.Fatal("grant not given back")
	}
	// Re-review F5: a grant counts only for a key in paired.v1, checked on
	// its own: relay.v1 (not covered by the seal) grants a key that is not
	// paired, and the seal is intact.
	rp := filepath.Join(dir, "relay.v1")
	rb, _ := os.ReadFile(rp)
	stranger := newPhone(lw.to).FP
	rb = bytes.Replace(rb, []byte(`"grants_after":[`), []byte(`"grants_after":["`+wire.Hex(stranger[:])+`",`), 1)
	os.WriteFile(rp, rb, 0o600)
	if v := load(); v.Remote[stranger] || !v.Remote[p.FP] || v.Notice != "" {
		t.Fatal("a grant for a key not in paired.v1 counted, or the seal broke")
	}

	// Re-review F5: a rollback gatewayd rewrites paired.v1 with the very
	// bytes recorded as "after" (revoke and pair again within one boot).
	// The digest matches; the seal does not, so every grant is dropped.
	pb, _ := os.ReadFile(filepath.Join(dir, "paired.v1"))
	tmp := filepath.Join(dir, ".rollback")
	os.WriteFile(tmp, pb, 0o600)
	os.Rename(tmp, filepath.Join(dir, "paired.v1"))
	if v := load(); len(v.Remote) != 0 || !v.Paired[p.FP] || !strings.Contains(v.Notice, "turned off for every phone") {
		t.Fatal(`byte-identical rewrite: every grant dropped`)
	}

	// A crash between paired.v1 and its seal: not provable, so fail-closed.
	if ok, _ := u.SetRemote(p.FP, true); !ok || !load().Remote[p.FP] {
		t.Fatal("grant not given back")
	}
	u.Set(func(x *fake.Unit) { x.CrashAfterPairedWrite = true })
	if _, err := pairAgain(newPhone(lw.to), firstPairing(u), false); err == nil {
		t.Fatal("a pairing that crashed before its seal answered")
	}
	u.Set(func(x *fake.Unit) { x.CrashAfterPairedWrite = false })
	if v := load(); len(v.Remote) != 0 || !strings.Contains(v.Notice, "turned off for every phone") {
		t.Fatal(`crash before the seal: every grant dropped`)
	}

	os.WriteFile(filepath.Join(dir, "paired.v1"), []byte(`{"paired":[{"key":"`+pFP+`","pairings":9},{"key":"`+wire.Hex(q.FP[:])+`","pairings":1}]}`+"\n"), 0o600)
	if v := load(); len(v.Remote) != 0 || !strings.Contains(v.Notice, "turned off for every phone") {
		t.Fatal(`"neither": every grant dropped`)
	}
}

// T10: a control replacement while an invitation waits ends it; one while
// a listed splice is in its ready transition keeps it.
func TestT10ReplacementWhileWaitingAndJoining(t *testing.T) {
	g := newGate("joined")
	e, to, us := rawRelay(t, opts{hook: hooks([]*gate{g}, nil)}, 1)
	u, p := us[0], newPhone(e.to)
	k1 := ctlRegister(t, to, u, p.FP)
	pc := phoneConnect(t, to, p, u.ID, nil, nil)
	inv := next(t, k1, "invite").(*wire.Invite)
	k2 := ctlRegister(t, to, u, p.FP)
	if !isType(answer(t, pc), "unavailable") {
		t.Fatal("a waiting invitation survived the replacement")
	}
	if got := joinRaw(t, to, inv.Cookie); !bytes.Equal(got, unavailable) {
		t.Fatalf("its cookie: %q", got)
	}

	g.arm()
	pc = phoneConnect(t, to, p, u.ID, nil, nil)
	inv = next(t, k2, "invite").(*wire.Invite)
	dcc := rawStart(t, to)
	dcc.Send(&wire.Join{Cookie: inv.Cookie})
	g.wait(t) // joined, not yet ready
	ctlRegister(t, to, u, p.FP)
	g.open()
	if !isType(answer(t, dcc), "ready") || !isType(answer(t, pc), "ready") {
		t.Fatal("a joined listed splice did not survive the replacement")
	}
	forwards(t, pc, dcc, "joined across a replacement")
}

// T11 (Q2): after `which` for unit U, a client naming another unit is refused.
func TestT11WhichThenOtherUnit(t *testing.T) {
	e, to, us := rawRelay(t, opts{}, 2)
	uu, vv, p := us[0], us[1], newPhone(e.to)
	ku := ctlRegister(t, to, uu, p.FP)
	ctlRegister(t, to, vv, p.FP)
	try := func(dev [32]byte) *fake.Conn {
		c := rawStart(t, to)
		c.Send(&wire.Which{Lookup: wire.Lookup(c.Presented, uu.ID)})
		if m, err := c.Recv(); err != nil || !isType(m, "endorsement") {
			t.Fatal("no endorsement")
		}
		sig, _ := wire.Sign(p.Key, wire.ClientLoginMsg(c.Nonce, c.Presented, dev))
		c.Send(&wire.ClientLogin{SPKI: p.SPKI, DeviceID: dev, Sig: sig})
		c.Send(&wire.Connect{})
		return c
	}
	if got := rest(try(vv.ID)); !bytes.Equal(got, unavailable) {
		t.Fatalf("which U, then client V: %q", got)
	}
	try(uu.ID)
	if inv := next(t, ku, "invite").(*wire.Invite); inv.Client != p.FP {
		t.Fatal("which U, then client U not invited")
	}
}

// T12 (Q4): bytes read ahead past `connect` and past `join` are forwarded
// first, in order.
func TestT12ReadAheadForwarded(t *testing.T) {
	e, to, us := rawRelay(t, opts{}, 1)
	u, p := us[0], newPhone(e.to)
	k := ctlRegister(t, to, u, p.FP)
	pc := phoneConnect(t, to, p, u.ID, nil, []byte("PHONE-EARLY|"))
	inv := next(t, k, "invite").(*wire.Invite)
	dc, m := joinConn(t, to, inv.Cookie, []byte("UNIT-EARLY|"))
	if !isType(m, "ready") || !isType(answer(t, pc), "ready") {
		t.Fatal("no ready")
	}
	pc.TLS.Write([]byte("PHONE-LATE"))
	dc.TLS.Write([]byte("UNIT-LATE"))
	if got := readN(t, dc, len("PHONE-EARLY|PHONE-LATE")); got != "PHONE-EARLY|PHONE-LATE" {
		t.Fatalf("unit got %q", got)
	}
	if got := readN(t, pc, len("UNIT-EARLY|UNIT-LATE")); got != "UNIT-EARLY|UNIT-LATE" {
		t.Fatalf("phone got %q", got)
	}
}

// blockWriter is a log output that never returns until released.
type blockWriter struct{ release chan struct{} }

func (b *blockWriter) Write(p []byte) (int, error) { <-b.release; return len(p), nil }

func dialFrom(t *testing.T, ip, addr string) net.Conn {
	t.Helper()
	d := net.Dialer{LocalAddr: &net.TCPAddr{IP: net.ParseIP(ip)}, Timeout: 2 * time.Second}
	c, err := d.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	return c
}

// T13: the source table never exceeds max_sources; a full table refuses a
// new source at accept with bounded work; a blocked log writer never
// stalls accept.
func TestT13SourceTableAndBlockedLog(t *testing.T) {
	bw := &blockWriter{release: make(chan struct{})}
	defer close(bw.release)
	tm := slowTiming()
	e, to, us := rawRelay(t, opts{timing: tm, logOut: bw,
		limits: Limits{PerSourceRate: 1000, PerSourceUnauth: 64, MaxSources: 16}}, 2)
	u, v, p := us[0], us[1], newPhone(to)
	k := ctlRegister(t, to, u, p.FP) // 127.0.0.1 is in the table
	for i := 2; i <= 16; i++ {
		dialFrom(t, "127.0.1."+strconv.Itoa(i), e.addr) // live, so not removable
	}
	waitFor(t, "16 sources", func() bool { return e.s.lim.size() == 16 })
	e.s.lim.mu.Lock()
	before := e.s.lim.examined
	e.s.lim.mu.Unlock()
	for i := 1; i <= 20; i++ {
		c := dialFrom(t, "127.0.2."+strconv.Itoa(i), e.addr)
		c.SetReadDeadline(time.Now().Add(2 * time.Second))
		if _, err := c.Read(make([]byte, 1)); err == nil {
			t.Fatal("a new source got bytes")
		} else if ne, ok := err.(net.Error); ok && ne.Timeout() {
			t.Fatalf("new source %d accepted into a full table", i)
		}
		if n := e.s.lim.size(); n > 16 {
			t.Fatalf("source table at %d", n)
		}
	}
	e.s.lim.mu.Lock()
	work := e.s.lim.examined - before
	e.s.lim.mu.Unlock()
	if work > 20*evictProbe {
		t.Fatalf("%d entries examined for 20 accepts", work)
	}
	if e.s.count.refused[refusedSources].Load() != 20 {
		t.Fatalf("refused_sources %d", e.s.count.refused[refusedSources].Load())
	}
	// The log writer has been blocked since the first line. Make far more
	// lines than the queue holds; accept and the relay go on.
	for i := 0; i < 200; i++ {
		c := ctlRegister(t, to, v)
		c.Close()
	}
	pc, dc := runningSplice(t, to, k, p, u, nil)
	forwards(t, pc, dc, "served with the log blocked")
	if e.s.lines.dropped.Load() == 0 {
		t.Fatal("no line dropped")
	}
}

// T15: one relay place per unit: a listed splice makes a candidate's
// connect unavailable and the other way round; a phone that holds the
// place replaces itself.
func TestT15OnePlace(t *testing.T) {
	e, to, us := rawRelay(t, opts{}, 1)
	u, p, q := us[0], newPhone(e.to), newPhone(e.to)
	k := ctlRegister(t, to, u, p.FP)
	slot := &[16]byte{5}
	k.Send(&wire.PairSlot{Slot: slot})
	syncCtl(t, k)
	pc, dc := runningSplice(t, to, k, p, u, nil)
	if !isType(answer(t, phoneConnect(t, to, q, u.ID, slot, nil)), "unavailable") {
		t.Fatal("a candidate got in beside a listed splice")
	}
	pc.Close()
	waitFor(t, "place free", func() bool { e.s.mu.Lock(); defer e.s.mu.Unlock(); return e.s.units[u.ID].place == nil })
	dc.Close()
	qc, qd := runningSplice(t, to, k, q, u, slot)
	if !isType(answer(t, phoneConnect(t, to, p, u.ID, nil, nil)), "unavailable") {
		t.Fatal("a listed phone got in beside a candidate's splice")
	}
	qc2, qd2 := runningSplice(t, to, k, q, u, slot) // the candidate replaces itself
	if !closedNow(qc, time.Second) || !closedNow(qd, time.Second) {
		t.Fatal("old candidate splice not replaced")
	}
	forwards(t, qc2, qd2, "replaced itself")
}

// rawE lays out endorsement bytes by hand, so that invalid ones can be made.
func rawE(cid, seq uint64, name string, port uint16, key [32]byte) []byte {
	b := append([]byte(wire.CtxEndorsement), 0)
	b = binary.BigEndian.AppendUint64(b, cid)
	b = binary.BigEndian.AppendUint64(b, seq)
	b = append(b, byte(len(name)))
	b = append(b, name...)
	b = binary.BigEndian.AppendUint16(b, port)
	return append(b, key[:]...)
}

// T17 (Q14): seq 0 and non-canonical names are refused at register.
func TestT17EndorsementBounds(t *testing.T) {
	e, to, us := rawRelay(t, opts{}, 1)
	u := us[0]
	key := e.s.KeyHash()
	for name, eb := range map[string][]byte{
		"seq 0":        rawE(7, 0, relayName, 443, key),
		"seq 2^63":     rawE(7, 1<<63, relayName, 443, key),
		"trailing dot": rawE(7, 1, relayName+".", 443, key),
		"upper case":   rawE(7, 1, strings.ToUpper(relayName), 443, key),
		"empty label":  rawE(7, 1, "relay..test", 443, key),
		"leading dash": rawE(7, 1, "-relay.test", 443, key),
	} {
		sig, _ := wire.Sign(u.Key, eb)
		c := ctlLogin(t, to, u)
		c.Send(&wire.Register{Endorsement: wire.SignedEndorsement{E: eb, Sig: sig}})
		if got := rest(c); len(got) != 0 {
			t.Errorf("%s: answered %q", name, got)
		}
		if e.online(u.ID) {
			t.Errorf("%s: registered", name)
		}
	}
	ctlRegister(t, to, u) // the canonical one is taken
}

// T18 (Q26): enroll refuses to write over a change made since it read.
func TestT18EnrollLostUpdate(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "relay.json")
	if err := os.WriteFile(path, []byte(`{"name":"relay.example.org","key_file":"k","units":[]}`), 0o600); err != nil {
		t.Fatal(err)
	}
	other := `{"name":"relay.example.org","key_file":"k","units":["` + strings.Repeat("cd", 32) + `"]}`
	enrollHook = func() { os.WriteFile(path, []byte(other), 0o600) }
	defer func() { enrollHook = nil }()
	if _, _, err := Enroll(path, strings.Repeat("ab", 32)); !errors.Is(err, ErrConfigChanged) {
		t.Fatalf("enroll over a concurrent change: %v", err)
	}
	if got, _ := os.ReadFile(path); string(got) != other {
		t.Fatal("the concurrent change was overwritten")
	}
	enrollHook = nil
	if _, _, err := Enroll(path, strings.Repeat("ab", 32)); err != nil {
		t.Fatal("enroll run again:", err)
	}
	c, _ := LoadConfig(path)
	if len(c.Units) != 2 {
		t.Fatalf("units %v", c.Units)
	}
}
