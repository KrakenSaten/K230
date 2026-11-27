// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"bytes"
	"errors"
	"testing"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// world is a relay with one set-up unit.
type world struct {
	*env
	to *target
	u  *fake.Unit
}

func newWorld(t *testing.T, o opts) *world {
	ln := listen(t)
	to := newTarget(ln.Addr().String())
	u := newUnit(t, to)
	o.units = append(o.units, u.ID)
	e := start(t, ln, o)
	e.setUp(u)
	return &world{env: e, to: to, u: u}
}

// The splice carries the inner, mutually pinned TLS end to end.
func TestSessionEndToEnd(t *testing.T) {
	w := newWorld(t, opts{})
	p := newPhone(w.to)
	rec := pairLocally(t, w.u, p, true)
	s, err := p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if s.Role != "paired" || s.Provisional {
		t.Fatalf("role %q provisional %v", s.Role, s.Provisional)
	}
	for _, msg := range []string{"one", "two", "a longer line through the relay"} {
		if got, err := s.Echo(msg); err != nil || got != msg {
			t.Fatalf("echo %q: %q %v", msg, got, err)
		}
	}
	if !w.u.HasSession(p.FP) {
		t.Fatal("the unit has no session")
	}
}

// Threat model §5.3 (as far as the relay goes): on the provisional path
// nothing but the HTTP start and the bound lookup leaves the phone before
// the endorsement is accepted; afterwards the key is pinned.
func TestProvisionalPathSendsOnlyStartAndLookup(t *testing.T) {
	w := newWorld(t, opts{})
	p := newPhone(w.to)
	rec := pairLocally(t, w.u, p, true)
	rec.Pin = nil // a rotation not yet seen
	s, err := p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if !s.Provisional {
		t.Fatal("not on the provisional path")
	}
	lookup := wire.Lookup(w.s.KeyHash(), w.u.ID)
	want := append(wire.StartRequest(relayName, 443), wire.AppendFrame(nil, wire.Encode(&wire.Which{Lookup: lookup}))...)
	if !bytes.Equal(s.BeforeAccept, want) {
		t.Fatalf("sent before acceptance:\n%q\nwant\n%q", s.BeforeAccept, want)
	}
	if rec.Pin == nil || *rec.Pin != w.s.KeyHash() {
		t.Fatal("relay key not pinned after acceptance")
	}
	if bytes.Contains(s.BeforeAccept, []byte(wire.Hex(w.u.ID[:]))) || bytes.Contains(s.BeforeAccept, w.u.ID[:]) {
		t.Fatal("device_id sent before acceptance")
	}
}

// Threat model §5.5: an unknown unit, a phone not admitted and an offline
// unit all get exactly the same answer.
func TestUnavailableIsUniform(t *testing.T) {
	ln := listen(t)
	to := newTarget(ln.Addr().String())
	u := newUnit(t, to)
	offline := newUnit(t, to)
	e := start(t, ln, opts{units: [][32]byte{u.ID, offline.ID}})
	e.setUp(u)
	e.setUp(offline)
	p := newPhone(to)
	recU := pairLocally(t, u, p, false) // paired, no remote: not admitted
	recOff := pairLocally(t, offline, p, true)
	offline.Disconnect()
	waitFor(t, "offline", func() bool { return !e.online(offline.ID) })
	unknown := *recU
	unknown.DeviceID = fake.NewUnit(relayName, 443, to.dial).ID

	answer := func(rec *fake.Record) []byte {
		c := rawStart(t, to)
		sig, _ := wire.Sign(p.Key, wire.ClientLoginMsg(c.Nonce, c.Presented, rec.DeviceID))
		c.Send(&wire.ClientLogin{SPKI: p.SPKI, DeviceID: rec.DeviceID, Sig: sig})
		c.Send(&wire.Connect{})
		return rest(c)
	}
	want := wire.AppendFrame(nil, wire.Encode(&wire.Unavailable{}))
	for name, rec := range map[string]*fake.Record{"not admitted": recU, "offline": recOff, "unknown": &unknown} {
		if got := answer(rec); !bytes.Equal(got, want) {
			t.Errorf("%s: %q", name, got)
		}
	}
	// A bad login signature, and a lookup nobody matches, get the same.
	c := rawStart(t, to)
	c.Send(&wire.ClientLogin{SPKI: p.SPKI, DeviceID: u.ID, Sig: []byte{0x30, 0x06, 2, 1, 1, 2, 1, 1}})
	c.Send(&wire.Connect{})
	if got := rest(c); !bytes.Equal(got, want) {
		t.Errorf("bad login: %q", got)
	}
	c = rawStart(t, to)
	c.Send(&wire.Which{Lookup: wire.Lookup(e.s.KeyHash(), unknown.DeviceID)})
	if got := rest(c); !bytes.Equal(got, want) {
		t.Errorf("unknown lookup: %q", got)
	}
	c = rawStart(t, to)
	c.Send(&wire.Which{Lookup: wire.Lookup(e.s.KeyHash(), offline.ID)})
	if got := rest(c); !bytes.Equal(got, want) {
		t.Errorf("offline lookup: %q", got)
	}
}

// Threat model §5.5: taking a phone off the list ends its splice within a
// second, at the relay itself (the gateway's own close is left out here).
func TestRemovalEndsSpliceWithinASecond(t *testing.T) {
	w := newWorld(t, opts{})
	p := newPhone(w.to)
	rec := pairLocally(t, w.u, p, true)
	s, err := p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	t0 := time.Now()
	if ok, err := w.u.ForgetRemote(p.FP); !ok || err != nil {
		t.Fatalf("admit not confirmed: %v", err)
	}
	if !s.Done(time.Second) {
		t.Fatal("the splice outlived its phone's removal by a second")
	}
	if d := time.Since(t0); d > time.Second {
		t.Fatalf("took %v", d)
	}
	if _, err := p.Connect(ctx(t), rec, nil); !errors.Is(err, fake.ErrUnavailable) {
		t.Fatalf("removed phone reconnected: %v", err)
	}
}

// Rule 37 at the relay: a phone's new connect replaces its own splice, and
// is not refused by the limit of one session; another phone's is.
func TestReplacement(t *testing.T) {
	w := newWorld(t, opts{})
	p, q := newPhone(w.to), newPhone(w.to)
	rec := pairLocally(t, w.u, p, true)
	recQ := pairLocally(t, w.u, q, true)
	s1, err := p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer s1.Close()
	if _, err := q.Connect(ctx(t), recQ, nil); !errors.Is(err, fake.ErrUnavailable) {
		t.Fatalf("a second phone got the unit's one session: %v", err)
	}
	s2, err := p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatalf("returning phone refused: %v", err)
	}
	defer s2.Close()
	if !s1.Done(2 * time.Second) {
		t.Fatal("old splice not replaced")
	}
	if got, err := s2.Echo("x"); err != nil || got != "x" {
		t.Fatalf("new session: %q %v", got, err)
	}
	s2.Close()
	waitFor(t, "slot released", func() bool {
		w.s.mu.Lock()
		defer w.s.mu.Unlock()
		return w.s.units[w.u.ID].session == nil
	})
	s3, err := q.Connect(ctx(t), recQ, nil)
	if err != nil {
		t.Fatalf("slot not released: %v", err)
	}
	s3.Close()
}

// A new register replaces the unit's control connection; a listed phone's
// session survives it. The control connection ending without a
// replacement ends the unit's sessions and its admission state.
func TestControlReplacementAndLoss(t *testing.T) {
	w := newWorld(t, opts{})
	p := newPhone(w.to)
	rec := pairLocally(t, w.u, p, true)
	s, err := p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	first := w.u.ControlDone()
	if err := w.u.Connect(ctx(t)); err != nil {
		t.Fatal(err)
	}
	select {
	case <-first:
	case <-time.After(3 * time.Second):
		t.Fatal("earlier control connection not replaced")
	}
	if got, err := s.Echo("still"); err != nil || got != "still" {
		t.Fatalf("listed phone's session did not survive: %v", err)
	}
	w.u.Disconnect()
	if !s.Done(3 * time.Second) {
		t.Fatal("session outlived the unit's control connection")
	}
	if _, err := p.Connect(ctx(t), rec, nil); !errors.Is(err, fake.ErrUnavailable) {
		t.Fatalf("offline unit reachable: %v", err)
	}
}

func TestDeclineAndCookies(t *testing.T) {
	tm := fastTiming()
	tm.join = 500 * time.Millisecond
	w := newWorld(t, opts{timing: &tm})
	p := newPhone(w.to)
	rec := pairLocally(t, w.u, p, true)

	w.u.Set(func(u *fake.Unit) { u.Decline = true })
	if _, err := p.Connect(ctx(t), rec, nil); !errors.Is(err, fake.ErrUnavailable) {
		t.Fatalf("declined: %v", err)
	}

	// Manual joins: a cookie works once, and only for 500 ms here.
	w.u.Set(func(u *fake.Unit) { u.Decline, u.Manual = false, true })
	drain(w.u)
	res := make(chan error, 1)
	go func() {
		s, err := p.Connect(ctx(t), rec, nil)
		if err == nil {
			s.Close()
		}
		res <- err
	}()
	inv := <-w.u.Invites()
	if inv.Client != p.FP || inv.Via != wire.ViaAdmit {
		t.Fatalf("invite %+v", inv)
	}
	time.Sleep(700 * time.Millisecond)
	if err := <-res; !errors.Is(err, fake.ErrUnavailable) {
		t.Fatalf("phone after a missed join: %v", err)
	}
	if got := joinRaw(t, w.to, inv.Cookie); !bytes.Equal(got, unavailable) {
		t.Fatalf("late join: %q", got)
	}

	go func() {
		s, err := p.Connect(ctx(t), rec, nil)
		if err == nil {
			s.Close()
		}
		res <- err
	}()
	inv = <-w.u.Invites()
	c := rawStart(t, w.to)
	c.Send(&wire.Join{Cookie: inv.Cookie})
	if m, err := c.Recv(); err != nil {
		t.Fatalf("join: %v", err)
	} else if _, ok := m.(*wire.Ready); !ok {
		t.Fatalf("join answered %T", m)
	}
	if got := joinRaw(t, w.to, inv.Cookie); !bytes.Equal(got, unavailable) {
		t.Fatalf("second join with one cookie: %q", got)
	}
	if got := joinRaw(t, w.to, [16]byte{1, 2, 3}); !bytes.Equal(got, unavailable) {
		t.Fatalf("unknown cookie: %q", got)
	}
	c.Close()
	<-res
}

func drain(u *fake.Unit) {
	for {
		select {
		case <-u.Invites():
		default:
			return
		}
	}
}

func joinRaw(t *testing.T, to *target, cookie [16]byte) []byte {
	c := rawStart(t, to)
	c.Send(&wire.Join{Cookie: cookie})
	return rest(c)
}

func TestIdleSession(t *testing.T) {
	tm := fastTiming()
	tm.sessionIdle = 400 * time.Millisecond
	w := newWorld(t, opts{timing: &tm})
	p := newPhone(w.to)
	rec := pairLocally(t, w.u, p, true)
	s, err := p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	for i := 0; i < 4; i++ { // traffic keeps it alive
		time.Sleep(200 * time.Millisecond)
		if _, err := s.Echo("keep"); err != nil {
			t.Fatalf("active session ended: %v", err)
		}
	}
	if !s.Done(2 * time.Second) {
		t.Fatal("idle session not ended")
	}
}
