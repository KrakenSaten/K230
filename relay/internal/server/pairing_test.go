// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"bytes"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// firstPairing is what a phone knows from a pairing code with r: the
// unit's fingerprint and the relay's name and port, nothing else.
func firstPairing(u *fake.Unit) *fake.Record {
	return &fake.Record{DeviceID: u.ID, Name: u.Name, Port: u.Port}
}

// Threat model §5.6: first pairing through the relay, with remote switched
// on at the approval: the phone verifies the endorsement against the
// code's fp, arrives as the relay candidate through the slot, is told it is
// paired with remote only after the relay confirmed the list, and then
// reconnects as a paired phone.
func TestRelayPairingWithRemote(t *testing.T) {
	w := newWorld(t, opts{})
	p := newPhone(w.to)
	token, err := w.u.OpenWindow(true, true)
	if err != nil {
		t.Fatal(err)
	}
	rec := firstPairing(w.u)
	slot := wire.SlotFromToken(token)
	waitFor(t, "slot at the relay", func() bool {
		w.s.mu.Lock()
		defer w.s.mu.Unlock()
		return w.s.units[w.u.ID].slot != nil
	})
	s, err := p.Connect(ctx(t), rec, &slot)
	if err != nil {
		t.Fatal(err)
	}
	if s.Role != "candidate" || !s.Provisional {
		t.Fatalf("role %q provisional %v", s.Role, s.Provisional)
	}
	if rec.ConfigID == nil || rec.Pin == nil || *rec.Pin != w.s.KeyHash() {
		t.Fatal("first pairing did not store the relay's pin and configuration id")
	}
	if got, err := s.Pair(token); err != nil || got != "paired remote" {
		t.Fatalf("pair: %q %v", got, err)
	}
	s.Close()
	waitFor(t, "candidate released", func() bool {
		w.s.mu.Lock()
		defer w.s.mu.Unlock()
		return w.s.units[w.u.ID].candidate == nil
	})
	s, err = p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal("reconnect as paired:", err)
	}
	defer s.Close()
	if s.Role != "paired" || s.Provisional {
		t.Fatalf("after pairing: role %q provisional %v", s.Role, s.Provisional)
	}
}

// Threat model §5.6: approved without remote, the phone is not admitted
// through the relay until remote is given at the unit; then the person's
// tap gets it on.
func TestRelayPairingWithoutRemote(t *testing.T) {
	w := newWorld(t, opts{})
	p := newPhone(w.to)
	token, _ := w.u.OpenWindow(true, false)
	slot := wire.SlotFromToken(token)
	rec := firstPairing(w.u)
	waitFor(t, "slot", func() bool { w.s.mu.Lock(); defer w.s.mu.Unlock(); return w.s.units[w.u.ID].slot != nil })
	s, err := p.Connect(ctx(t), rec, &slot)
	if err != nil {
		t.Fatal(err)
	}
	if got, err := s.Pair(token); err != nil || got != "paired" {
		t.Fatalf("pair: %q %v", got, err)
	}
	s.Close()
	if _, err := p.Connect(ctx(t), rec, nil); !errors.Is(err, fake.ErrUnavailable) {
		t.Fatalf("paired without remote, reached the unit: %v", err)
	}
	if ok, err := w.u.SetRemote(p.FP, true); !ok || err != nil {
		t.Fatal("grant not confirmed")
	}
	s, err = p.Connect(ctx(t), rec, nil) // "Try through the relay"
	if err != nil {
		t.Fatal("after the grant:", err)
	}
	s.Close()
}

// Threat model §5.6: no slot without a relay window; a wrong slot, a
// cleared slot and a second candidate are refused; a candidate's slot is
// released when its session ends.
func TestPairingSlotRules(t *testing.T) {
	w := newWorld(t, opts{})
	p, q := newPhone(w.to), newPhone(w.to)
	rec := firstPairing(w.u)

	lanToken, _ := w.u.OpenWindow(false, false) // relay off: no slot sent
	slot := wire.SlotFromToken(lanToken)
	if _, err := p.Connect(ctx(t), rec, &slot); !errors.Is(err, fake.ErrUnavailable) {
		t.Fatalf("LAN-only window reachable through the relay: %v", err)
	}

	token, _ := w.u.OpenWindow(true, false)
	good := wire.SlotFromToken(token)
	waitFor(t, "slot", func() bool { w.s.mu.Lock(); defer w.s.mu.Unlock(); return w.s.units[w.u.ID].slot != nil })
	wrong := good
	wrong[0] ^= 1
	if _, err := p.Connect(ctx(t), rec, &wrong); !errors.Is(err, fake.ErrUnavailable) {
		t.Fatalf("wrong slot: %v", err)
	}
	s, err := p.Connect(ctx(t), rec, &good)
	if err != nil {
		t.Fatal(err)
	}
	recQ := firstPairing(w.u)
	if _, err := q.Connect(ctx(t), recQ, &good); !errors.Is(err, fake.ErrUnavailable) {
		t.Fatalf("second candidate: %v", err)
	}
	s.Close()
	waitFor(t, "candidate released", func() bool {
		w.s.mu.Lock()
		defer w.s.mu.Unlock()
		return w.s.units[w.u.ID].candidate == nil
	})
	s, err = q.Connect(ctx(t), recQ, &good)
	if err != nil {
		t.Fatal("slot not released:", err)
	}
	s.Close()

	if err := w.u.CloseWindow(); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "slot cleared", func() bool { w.s.mu.Lock(); defer w.s.mu.Unlock(); return w.s.units[w.u.ID].slot == nil })
	if _, err := q.Connect(ctx(t), recQ, &good); !errors.Is(err, fake.ErrUnavailable) {
		t.Fatalf("closed window: %v", err)
	}
}

// Threat model §5.6: the token never reaches the relay: not in what the
// phone sent outside the inner TLS, not in the relay's state, not in its
// log. The relay sees the slot only, and does not log it either.
func TestTokenNeverReachesRelay(t *testing.T) {
	w := newWorld(t, opts{})
	p := newPhone(w.to)
	token, _ := w.u.OpenWindow(true, true)
	slot := wire.SlotFromToken(token)
	waitFor(t, "slot", func() bool { w.s.mu.Lock(); defer w.s.mu.Unlock(); return w.s.units[w.u.ID].slot != nil })
	rec := firstPairing(w.u)
	s, err := p.Connect(ctx(t), rec, &slot)
	if err != nil {
		t.Fatal(err)
	}
	if got, _ := s.Pair(token); got != "paired remote" {
		t.Fatalf("pair: %q", got)
	}
	sent := s.Relay.Written()
	s.Close()
	time.Sleep(100 * time.Millisecond)
	logs := w.logs.String()
	forms := [][]byte{
		token,
		[]byte(hex.EncodeToString(token)),
		[]byte(base64.StdEncoding.EncodeToString(token)),
		[]byte(base64.RawURLEncoding.EncodeToString(token)),
	}
	for _, f := range forms {
		if bytes.Contains(sent, f) || strings.Contains(logs, string(f)) {
			t.Fatalf("token form %q seen by the relay", f)
		}
	}
	for _, f := range []string{base64.StdEncoding.EncodeToString(slot[:]), hex.EncodeToString(slot[:])} {
		if strings.Contains(logs, f) {
			t.Fatal("slot in the log")
		}
	}
}
