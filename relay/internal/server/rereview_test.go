// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

// Regression tests for the independent re-review's findings F1, F2, F4 and
// F6 (RELAY_REVIEW_RESOLUTION.md §15.2, §17). As in review_test.go, race
// points are held with hooks and an injected clock.

import (
	"io"
	"net"
	"sync/atomic"
	"testing"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// noInvite reads k until the relay answers a ping of the test's and fails
// if an `invite` came before it.
func noInvite(t *testing.T, k *fake.Conn) {
	t.Helper()
	n := syncN.Add(1) + 1000
	k.Send(&wire.Ping{N: n})
	k.TLS.SetReadDeadline(time.Now().Add(10 * time.Second))
	for {
		m, err := wire.ReadMsg(k.BR)
		if err != nil {
			t.Fatal("control closed:", err)
		}
		switch m := m.(type) {
		case *wire.Invite:
			t.Fatal("an invitation came")
		case *wire.Pong:
			if m.N == n {
				return
			}
		}
	}
}

// T19 (F1): a control is routable only once `registered` is queued. A listed
// phone that connects between `register`'s commit and its `registered` is
// refused, and nothing reaches the control before `registered`; afterwards
// the phone is invited. Both for a first control and for a replacement.
func TestT19NoInviteBeforeRegistered(t *testing.T) {
	g := newGate("register-committed")
	e, to, us := rawRelay(t, opts{hook: hooks([]*gate{g}, nil)}, 1)
	u, p := us[0], newPhone(e.to)
	held := func() *fake.Conn {
		g.arm()
		k := ctlLogin(t, to, u)
		if err := k.Send(&wire.Register{Admit: [][32]byte{p.FP}, Endorsement: u.Endorse()}); err != nil {
			t.Fatal(err)
		}
		g.wait(t) // committed: the unit's control is k, `registered` not yet queued
		pc := phoneConnect(t, to, p, u.ID, nil, nil)
		if !isType(answer(t, pc), "unavailable") {
			t.Fatal("a phone was admitted before registered")
		}
		g.open()
		next(t, k, "registered") // fails on an `invite` queued first
		return k
	}
	for i, what := range []string{"first control", "replacement"} {
		k := held()
		pc := phoneConnect(t, to, p, u.ID, nil, nil)
		inv := next(t, k, "invite").(*wire.Invite)
		if inv.Client != p.FP {
			t.Fatalf("%s: invite for another phone", what)
		}
		k.Send(&wire.Decline{Cookie: inv.Cookie})
		if !isType(answer(t, pc), "unavailable") {
			t.Fatalf("%s: decline not answered", what)
		}
		_ = i
	}
}

// T20 (F2): at most one invitation per unit per second. Connects beyond it
// are refused before any invitation, so a flood can neither close the
// control by its frame limit nor fill its queue; one second later the
// phone is invited again.
func TestT20InvitationLimit(t *testing.T) {
	var skew atomic.Int64
	tm := slowTiming()
	tm.invite = time.Second
	e, to, us := rawRelay(t, opts{
		timing: tm,
		limits: Limits{PerSourceRate: 1000},
		now:    func() time.Time { return time.Now().Add(time.Duration(skew.Load())) },
	}, 1)
	u, p := us[0], newPhone(e.to)
	k := ctlRegister(t, to, u, p.FP)

	pc := phoneConnect(t, to, p, u.ID, nil, nil)
	inv := next(t, k, "invite").(*wire.Invite)
	k.Send(&wire.Decline{Cookie: inv.Cookie})
	if !isType(answer(t, pc), "unavailable") {
		t.Fatal("decline not answered")
	}
	// The re-review's probe: 12 quick attempts closed the control through
	// its declines. Now none of them is invited.
	for i := 0; i < 12; i++ {
		c := phoneConnect(t, to, p, u.ID, nil, nil)
		if !isType(answer(t, c), "unavailable") {
			t.Fatalf("attempt %d inside the second was admitted", i)
		}
	}
	noInvite(t, k)
	skew.Store(int64(1100 * time.Millisecond))
	pc = phoneConnect(t, to, p, u.ID, nil, nil)
	inv = next(t, k, "invite").(*wire.Invite)
	if inv.Client != p.FP {
		t.Fatal("invite for another phone")
	}
	// A refused connect does not take the budget: a candidate with a wrong
	// slot is refused before the limit is consulted.
	skew.Store(int64(2200 * time.Millisecond))
	k.Send(&wire.Decline{Cookie: inv.Cookie})
	answer(t, pc)
	bad := phoneConnect(t, to, newPhone(e.to), u.ID, &[16]byte{9}, nil)
	if !isType(answer(t, bad), "unavailable") {
		t.Fatal("a wrong slot was admitted")
	}
	pc = phoneConnect(t, to, p, u.ID, nil, nil)
	next(t, k, "invite")
}

// T21 (F4, mutation MP): an `admit` read from a superseded control changes
// neither the new control's list nor anything else, and is not
// acknowledged. White-box, as T7, so it does not depend on timing.
func TestT21AdmitFromSupersededControl(t *testing.T) {
	uid, p, q := [32]byte{1}, [32]byte{2}, [32]byte{3}
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
	s.register(k1, [][32]byte{p}, nil)
	s.register(k2, [][32]byte{p}, nil)
	queued := len(k1.out)
	if s.setAdmit(k1, [][32]byte{q}, 4) {
		t.Fatal("a superseded control's admit was applied")
	}
	if len(k1.out) != queued {
		t.Fatal("a superseded control's admit was acknowledged")
	}
	s.mu.Lock()
	gotP, gotQ := un.admit[p], un.admit[q]
	s.mu.Unlock()
	if !gotP || gotQ {
		t.Fatalf("the new control's list changed: p %v q %v", gotP, gotQ)
	}
	a, b := net.Pipe()
	defer a.Close()
	defer b.Close()
	if sp, _ := s.admitSplice(uid, q, nil, &rconn{raw: a}); sp != nil {
		t.Fatal("a phone only on the superseded control's list was admitted")
	}
	if sp, _ := s.admitSplice(uid, p, nil, &rconn{raw: a}); sp == nil {
		t.Fatal("a listed phone was refused")
	}
	// The current control's update is applied and acknowledged (F6: queued
	// under the lock that saw it current).
	n2 := len(k2.out)
	if !s.setAdmit(k2, [][32]byte{q}, 5) || len(k2.out) != n2+1 {
		t.Fatal("the current control's admit was not applied and acknowledged")
	}
}
