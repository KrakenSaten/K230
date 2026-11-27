// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"errors"
	"testing"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/liar"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// liarWorld is a unit set up against a lying relay that holds a backup.
type liarWorld struct {
	l  *liar.Liar
	u  *fake.Unit
	to *target
}

func newLiarWorld(t *testing.T, lies liar.Lies) *liarWorld {
	t.Helper()
	h := KeyFingerprint(&fake.NewKey().PublicKey)
	lies.BackupHash = &h
	l, err := liar.Start("127.0.0.1:0", fake.NewKey(), lies)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(l.Close)
	to := newTarget(l.Addr())
	u := newUnit(t, to)
	got, err := u.Probe(ctx(t))
	if err != nil {
		t.Fatal(err)
	}
	if err := u.Setup(ctx(t), got); err != nil {
		t.Fatal(err)
	}
	if err := u.Connect(ctx(t)); err != nil {
		t.Fatal(err)
	}
	time.Sleep(50 * time.Millisecond)
	return &liarWorld{l: l, u: u, to: to}
}

// Threat model §5.5: a lying relay that passes a paired phone without
// remote through to the unit gets it nothing: the gateway closes it after
// the handshake. A stranger fails the gateway's pin.
func TestLyingRelayAdmitsAnyone(t *testing.T) {
	lw := newLiarWorld(t, liar.Lies{AdmitAnyone: true})
	p := newPhone(lw.to)
	rec := pairLocally(t, lw.u, p, false)
	if _, err := p.Connect(ctx(t), rec, nil); err == nil {
		t.Fatal("a paired phone without remote got a session")
	}
	stranger := newPhone(lw.to)
	srec := *rec
	if _, err := stranger.Connect(ctx(t), &srec, nil); err == nil {
		t.Fatal("a stranger got a session")
	}
}

// A lying relay that keeps a removed phone's splice: the gateway closes
// the session itself (rule 33).
func TestLyingRelayIgnoresRemovals(t *testing.T) {
	lw := newLiarWorld(t, liar.Lies{IgnoreRemovals: true})
	p := newPhone(lw.to)
	rec := pairLocally(t, lw.u, p, true)
	s, err := p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	lw.u.SetRemote(p.FP, false)
	if !s.Done(time.Second) {
		t.Fatal("the gateway did not close a removed phone's session")
	}
}

// A lying relay that withholds `admitted`: gatewayd waits at most 3 s and
// does not tell the phone it has remote (R-T24).
func TestLyingRelayWithholdsAdmitted(t *testing.T) {
	lw := newLiarWorld(t, liar.Lies{WithholdAdmitted: true})
	t0 := time.Now()
	ok, err := lw.u.SendAdmit()
	if err != nil || ok {
		t.Fatalf("confirmed: %v %v", ok, err)
	}
	if d := time.Since(t0); d < 3*time.Second || d > 5*time.Second {
		t.Fatalf("waited %v", d)
	}
}

// Threat model §5.3 with a lying relay: each wrong field of an endorsement
// is refused on the provisional path, and an old one after a new set-up
// (new configuration id) is refused although its sequence is higher.
func TestLyingEndorsements(t *testing.T) {
	ln := listen(t)
	to := newTarget(ln.Addr().String())
	u := newUnit(t, to)
	e := start(t, ln, opts{units: [][32]byte{u.ID}})
	e.setUp(u)
	p := newPhone(to)
	rec := pairLocally(t, u, p, true)
	relayKey := fake.NewKey() // the liar's key: the phone has not seen it
	liarHash := KeyFingerprint(&relayKey.PublicKey)

	signed := func(key *fake.Unit, mod func(*wire.Endorsement)) *wire.EndorsementFrame {
		var en wire.Endorsement
		key.Set(func(x *fake.Unit) {
			en = wire.Endorsement{ConfigID: x.ConfigID, Seq: x.Seq + 1, Name: x.Name, Port: x.Port, KeyHash: liarHash}
		})
		mod(&en)
		b, _ := en.Marshal()
		sig, _ := wire.Sign(key.Key, b)
		return &wire.EndorsementFrame{SPKI: key.SPKI, E: b, Sig: sig}
	}
	other := fake.NewUnit(relayName, 443, to.dial)
	other.Set(func(x *fake.Unit) { x.ConfigID, x.Seq = 1, 1 })
	badSig := signed(u, func(*wire.Endorsement) {})
	badSig.Sig = append([]byte(nil), badSig.Sig...)
	badSig.Sig[len(badSig.Sig)-1] ^= 1
	cases := map[string]*wire.EndorsementFrame{
		"unit key":  signed(other, func(*wire.Endorsement) {}),
		"signature": badSig,
		"name":      signed(u, func(e *wire.Endorsement) { e.Name = "other.test" }),
		"port":      signed(u, func(e *wire.Endorsement) { e.Port = 8443 }),
		"key":       signed(u, func(e *wire.Endorsement) { e.KeyHash = [32]byte{7} }),
		"config id": signed(u, func(e *wire.Endorsement) { e.ConfigID++; e.Seq += 100 }),
		"sequence":  signed(u, func(e *wire.Endorsement) { e.Seq = 0 }),
	}
	for field, ef := range cases {
		l, err := liar.Start("127.0.0.1:0", relayKey, liar.Lies{Endorsement: ef, AdmitAnyone: true})
		if err != nil {
			t.Fatal(err)
		}
		to.set(l.Addr())
		r := *rec
		var ee *fake.EndorsementError
		if _, err := p.Connect(ctx(t), &r, nil); !errors.As(err, &ee) || ee.Field != field {
			t.Errorf("%s: %v", field, err)
		}
		if seen := l.Seen(); len(seen) != 1 || seen[0] != "which" {
			t.Errorf("%s: phone sent %v", field, seen)
		}
		l.Close()
	}
}

// The liar's floods reach the unit as frames a reader must refuse; the
// fake unit's reader (like the helper's) drops the connection.
func TestLiarFloodsAreRefused(t *testing.T) {
	for name, lies := range map[string]liar.Lies{
		"oversize": {Oversize: true},
		"garbage":  {Garbage: true},
	} {
		lw := newLiarWorld(t, lies)
		select {
		case <-lw.u.ControlDone():
		case <-time.After(3 * time.Second):
			t.Errorf("%s: unit kept the connection", name)
		}
	}
	lw := newLiarWorld(t, liar.Lies{BogusInvite: true})
	inv := <-lw.u.Invites()
	time.Sleep(200 * time.Millisecond)
	if lw.u.HasSession(inv.Client) {
		t.Fatal("a bogus invitation made a session")
	}
}
