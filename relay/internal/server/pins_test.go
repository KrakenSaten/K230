// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"crypto/ecdsa"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"errors"
	"math/big"
	"strings"
	"testing"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/liar"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// caChain issues a leaf for key from a throwaway CA, with dates long past:
// the leaf-only pin must not care (rule 31, 39).
func caChain(t *testing.T, key *ecdsa.PrivateKey) tls.Certificate {
	t.Helper()
	caKey := fake.NewKey()
	old := time.Date(2001, 1, 1, 0, 0, 0, 0, time.UTC)
	ca := &x509.Certificate{
		SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "Test CA"},
		NotBefore: old, NotAfter: old.AddDate(1, 0, 0),
		IsCA: true, BasicConstraintsValid: true, KeyUsage: x509.KeyUsageCertSign,
	}
	caDER, err := x509.CreateCertificate(rand.Reader, ca, ca, &caKey.PublicKey, caKey)
	if err != nil {
		t.Fatal(err)
	}
	caCert, _ := x509.ParseCertificate(caDER)
	leaf := &x509.Certificate{
		SerialNumber: big.NewInt(2), Subject: pkix.Name{CommonName: relayName},
		DNSNames: []string{relayName}, NotBefore: old, NotAfter: old.AddDate(0, 3, 0),
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	leafDER, err := x509.CreateCertificate(rand.Reader, leaf, caCert, &key.PublicKey, caKey)
	if err != nil {
		t.Fatal(err)
	}
	var p []byte
	for _, d := range [][]byte{leafDER, caDER} {
		p = append(p, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: d})...)
	}
	c, err := ChainFor(p, key)
	if err != nil {
		t.Fatal(err)
	}
	return c
}

// Threat model §5.1 and §5.10: a CA-issued chain for the pinned key, with
// expired dates, passes the leaf-only pins of the unit and the phone.
func TestCAChainLeafOnly(t *testing.T) {
	key := fake.NewKey()
	chain := caChain(t, key)
	if len(chain.Certificate) != 2 {
		t.Fatal("not a chain")
	}
	w := newWorld(t, opts{key: key, cert: &chain})
	p := newPhone(w.to)
	rec := pairLocally(t, w.u, p, true)
	s, err := p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if s.Provisional {
		t.Fatal("a chain sent the phone onto the provisional path")
	}
	if got, err := s.Echo("chain"); err != nil || got != "chain" {
		t.Fatal(err)
	}
}

// Threat model §5.1: the relay speaks TLS 1.3 only, and a unit refuses a
// relay with TLS 1.2, an RSA key, or a key that is not a pin, before it
// sends anything: so it never signs a login for an unpinned key.
func TestUnitPins(t *testing.T) {
	w := newWorld(t, opts{})
	raw, err := w.to.dial(ctx(t))
	if err != nil {
		t.Fatal(err)
	}
	tc := tls.Client(raw, &tls.Config{ServerName: relayName, MaxVersion: tls.VersionTLS12,
		InsecureSkipVerify: true, NextProtos: []string{"http/1.1"}})
	if err := tc.Handshake(); err == nil {
		t.Fatal("the relay accepted TLS 1.2")
	}
	raw.Close()

	rsaKey, _ := rsa.GenerateKey(rand.Reader, 2048)
	tmpl := &x509.Certificate{SerialNumber: big.NewInt(1), NotBefore: time.Unix(0, 0), NotAfter: time.Unix(1<<40, 0)}
	der, _ := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &rsaKey.PublicKey, rsaKey)
	rsaCert := tls.Certificate{Certificate: [][]byte{der}, PrivateKey: rsaKey}

	for name, lies := range map[string]liar.Lies{
		"TLS 1.2":    {TLS12: true},
		"RSA key":    {Cert: &rsaCert},
		"other key":  {},
		"other cert": {Cert: func() *tls.Certificate { c := fake.SelfSigned(fake.NewKey(), "x"); return &c }()},
	} {
		l, err := liar.Start("127.0.0.1:0", fake.NewKey(), lies)
		if err != nil {
			t.Fatal(err)
		}
		w.to.set(l.Addr())
		if err := w.u.Connect(ctx(t)); err == nil {
			t.Errorf("%s: unit connected", name)
		}
		if seen := l.Seen(); len(seen) != 0 {
			t.Errorf("%s: unit sent %v", name, seen)
		}
		l.Close()
	}
}

// Threat model §5.1: the relay takes a unit login only for its own key and
// this connection's nonce, from an enrolled unit.
func TestUnitLoginChecks(t *testing.T) {
	w := newWorld(t, opts{})
	stranger := fake.NewUnit(relayName, 443, w.to.dial)
	try := func(key *ecdsa.PrivateKey, spki []byte, msg func(c *fake.Conn) []byte) error {
		c := rawStart(t, w.to)
		sig, _ := wire.Sign(key, msg(c))
		c.Send(&wire.UnitLogin{SPKI: spki, Sig: sig})
		c.Send(&wire.Register{Admit: nil, Endorsement: w.u.Endorse()})
		c.Send(&wire.Ping{N: 1})
		_, err := c.Recv()
		return err
	}
	other := [32]byte{9}
	var stale [32]byte
	stale[0] = 1
	cases := map[string]func(c *fake.Conn) []byte{
		"other relay key": func(c *fake.Conn) []byte { return wire.UnitLoginMsg(c.Nonce, other) },
		"other nonce":     func(c *fake.Conn) []byte { return wire.UnitLoginMsg(stale, c.Presented) },
		"client context":  func(c *fake.Conn) []byte { return wire.ClientLoginMsg(c.Nonce, c.Presented, w.u.ID) },
	}
	for name, msg := range cases {
		if err := try(w.u.Key, w.u.SPKI, msg); !errors.Is(err, fake.ErrUnavailable) {
			t.Errorf("%s: %v", name, err)
		}
	}
	good := func(c *fake.Conn) []byte { return wire.UnitLoginMsg(c.Nonce, c.Presented) }
	if err := try(stranger.Key, stranger.SPKI, good); !errors.Is(err, fake.ErrUnavailable) {
		t.Errorf("unenrolled unit: %v", err)
	}
	// The enrolled unit with a good login gets on: its ping is answered.
	c := rawStart(t, w.to)
	sig, _ := wire.Sign(w.u.Key, good(c))
	c.Send(&wire.UnitLogin{SPKI: w.u.SPKI, Sig: sig})
	c.Send(&wire.Register{Endorsement: w.u.Endorse()})
	c.Send(&wire.Ping{N: 7})
	for {
		m, err := c.Recv()
		if err != nil {
			t.Fatalf("good login: %v", err)
		}
		if p, ok := m.(*wire.Pong); ok && p.N == 7 {
			break
		}
	}
}

// The client login, for an admitted phone: the phone's key is public, so
// only a signature over this connection's nonce, this relay's key and
// this unit, by that key, gets the session.
func TestClientLoginChecks(t *testing.T) {
	w := newWorld(t, opts{})
	p := newPhone(w.to)
	pairLocally(t, w.u, p, true)
	thief := fake.NewKey()
	var stale [32]byte
	stale[0] = 1
	try := func(key *ecdsa.PrivateKey, msg func(c *fake.Conn) []byte) error {
		c := rawStart(t, w.to)
		sig, _ := wire.Sign(key, msg(c))
		c.Send(&wire.ClientLogin{SPKI: p.SPKI, DeviceID: w.u.ID, Sig: sig})
		c.Send(&wire.Connect{})
		c.TLS.SetReadDeadline(time.Now().Add(5 * time.Second))
		_, err := c.Recv()
		return err
	}
	for name, f := range map[string]struct {
		key *ecdsa.PrivateKey
		msg func(c *fake.Conn) []byte
	}{
		"another key":     {thief, func(c *fake.Conn) []byte { return wire.ClientLoginMsg(c.Nonce, c.Presented, w.u.ID) }},
		"other nonce":     {p.Key, func(c *fake.Conn) []byte { return wire.ClientLoginMsg(stale, c.Presented, w.u.ID) }},
		"other relay key": {p.Key, func(c *fake.Conn) []byte { return wire.ClientLoginMsg(c.Nonce, stale, w.u.ID) }},
		"other unit":      {p.Key, func(c *fake.Conn) []byte { return wire.ClientLoginMsg(c.Nonce, c.Presented, stale) }},
		"unit context":    {p.Key, func(c *fake.Conn) []byte { return wire.UnitLoginMsg(c.Nonce, c.Presented) }},
	} {
		if err := try(f.key, f.msg); !errors.Is(err, fake.ErrUnavailable) {
			t.Errorf("%s: %v", name, err)
		}
	}
	w.u.Set(func(u *fake.Unit) { u.Manual = true })
	if err := try(p.Key, func(c *fake.Conn) []byte { return wire.ClientLoginMsg(c.Nonce, c.Presented, w.u.ID) }); err != nil {
		// Manual: no join comes; the phone was admitted and invited.
		if !errors.Is(err, fake.ErrUnavailable) {
			t.Fatal(err)
		}
	}
	select {
	case inv := <-w.u.Invites():
		if inv.Client != p.FP {
			t.Fatal("invitation for another phone")
		}
	case <-time.After(time.Second):
		t.Fatal("the genuine login was not invited")
	}
}

// Threat model §5.2, the relay's part: `backup` is signed by the active key
// over the unit's nonce; with no backup configured the answer is
// unavailable; and `backup` and `prove` exist only on a control connection
// after login and before register, once each.
func TestBackupAndProve(t *testing.T) {
	w := newWorld(t, opts{})
	login := func() *fake.Conn {
		c := rawStart(t, w.to)
		sig, _ := wire.Sign(w.u.Key, wire.UnitLoginMsg(c.Nonce, c.Presented))
		c.Send(&wire.UnitLogin{SPKI: w.u.SPKI, Sig: sig})
		return c
	}
	c := login()
	var nonce [32]byte
	rand.Read(nonce[:])
	c.Send(&wire.GetBackup{Nonce: nonce})
	m, err := c.Recv()
	b, ok := m.(*wire.Backup)
	if err != nil || !ok {
		t.Fatalf("get_backup: %T %v", m, err)
	}
	want := KeyFingerprint(&w.backup.PublicKey)
	if wire.Fingerprint(b.SPKIA) != w.s.KeyHash() || b.SPKIHashB != want ||
		!wire.VerifySPKI(b.SPKIA, wire.BackupMsg(b.SPKIHashB, nonce), b.Sig) {
		t.Fatal("backup does not verify")
	}
	c.Send(&wire.Prove{Nonce: nonce})
	m, _ = c.Recv()
	kp, ok := m.(*wire.KeyProof)
	if !ok || wire.Fingerprint(kp.SPKI) != w.s.KeyHash() || !wire.VerifySPKI(kp.SPKI, wire.KeyProofMsg(nonce), kp.Sig) {
		t.Fatal("key_proof does not verify")
	}
	c.Send(&wire.GetBackup{Nonce: nonce}) // twice
	if len(rest(c)) != 0 {
		t.Fatal("second get_backup answered")
	}

	c = login()
	c.Send(&wire.Register{Endorsement: w.u.Endorse()})
	c.Send(&wire.GetBackup{Nonce: nonce}) // after register
	for {
		m, err := c.Recv()
		if err != nil {
			break
		}
		if _, ok := m.(*wire.Backup); ok {
			t.Fatal("backup answered after register")
		}
	}

	c = rawStart(t, w.to)
	c.Send(&wire.GetBackup{Nonce: nonce}) // before login
	if len(rest(c)) != 0 {
		t.Fatal("get_backup answered before login")
	}

	ln := listen(t)
	to := newTarget(ln.Addr().String())
	start(t, ln, opts{noBack: true, units: [][32]byte{w.u.ID}})
	c = rawStart(t, to)
	sig, _ := wire.Sign(w.u.Key, wire.UnitLoginMsg(c.Nonce, c.Presented))
	c.Send(&wire.UnitLogin{SPKI: w.u.SPKI, Sig: sig})
	c.Send(&wire.GetBackup{Nonce: nonce})
	if _, err := c.Recv(); !errors.Is(err, fake.ErrUnavailable) {
		t.Fatalf("no backup: %v", err)
	}
	if len(rest(c)) != 0 {
		t.Fatal("connection not ended after unavailable")
	}
}

// Threat model §5.2: a backup not signed by the confirmed key over the
// unit's own nonce, or offered unasked, is refused, and nothing is stored.
func TestLyingBackupRefused(t *testing.T) {
	h := KeyFingerprint(&fake.NewKey().PublicKey)
	for name, lies := range map[string]liar.Lies{
		"other signer": {BackupHash: &h, BackupSigner: fake.NewKey()},
		"own nonce":    {BackupHash: &h, BackupOwnNonce: true},
		"unsolicited":  {BackupHash: &h, UnsolicitedBackup: true},
	} {
		key := fake.NewKey()
		l, err := liar.Start("127.0.0.1:0", key, lies)
		if err != nil {
			t.Fatal(err)
		}
		u := fake.NewUnit(relayName, 443, fake.DialTCP(l.Addr()))
		got, err := u.Probe(ctx(t))
		if err != nil {
			t.Fatal(err)
		}
		if err := u.Setup(ctx(t), got); !errors.Is(err, fake.ErrBackup) {
			t.Errorf("%s: %v", name, err)
		}
		u.Set(func(u *fake.Unit) {
			if u.A != nil || u.B != nil {
				t.Errorf("%s: pins stored", name)
			}
		})
		l.Close()
	}
}

// Threat model §5.2 and §5.4: rotation with the phone away. The operator
// brings the backup online; the unit promotes it only after a valid
// key_proof, once; the phone follows by endorsement through the relay
// alone; then the old key, stolen with its old endorsement, gets nothing
// from either.
func TestRotationWithPhoneAway(t *testing.T) {
	keyA, keyB := fake.NewKey(), fake.NewKey()
	hashA, hashB := KeyFingerprint(&keyA.PublicKey), KeyFingerprint(&keyB.PublicKey)
	ln := listen(t)
	to := newTarget(ln.Addr().String())
	u := newUnit(t, to)
	e := start(t, ln, opts{key: keyA, backup: keyB, units: [][32]byte{u.ID}})
	e.setUp(u)
	u.Set(func(u *fake.Unit) {
		if *u.A != hashA || *u.B != hashB {
			t.Fatal("pins after set-up")
		}
	})
	p := newPhone(to)
	rec := pairLocally(t, u, p, true)
	stolen := u.Endorse()
	s, err := p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal(err)
	}
	s.Close()

	// The phone goes away. The relay is restarted with the backup key.
	u.Disconnect()
	e.s.Close()
	lnB := listen(t)
	to.set(lnB.Addr().String())
	eB := start(t, lnB, opts{key: keyB, noBack: true, units: [][32]byte{u.ID}})
	if err := u.Connect(ctx(t)); err != nil {
		t.Fatal("unit on the promoted relay:", err)
	}
	waitFor(t, "control up on B", func() bool { return eB.online(u.ID) })
	u.Set(func(u *fake.Unit) {
		if *u.A != hashB || u.B != nil || u.Seq != 2 || u.Promoted != 1 {
			t.Fatalf("after promotion: A=%x B=%v seq=%d promoted=%d", u.A[:4], u.B, u.Seq, u.Promoted)
		}
	})

	// The phone comes back: its pin fails, it follows the endorsement.
	s, err = p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal("phone after rotation:", err)
	}
	if !s.Provisional || *rec.Pin != hashB || rec.Seq != 2 {
		t.Fatalf("phone did not re-pin: provisional=%v seq=%d", s.Provisional, rec.Seq)
	}
	if got, err := s.Echo("rotated"); err != nil || got != "rotated" {
		t.Fatal(err)
	}
	s.Close()

	// Whoever holds the old key, with its old endorsement, gets nothing.
	thief, err := liar.Start("127.0.0.1:0", keyA, liar.Lies{
		Endorsement: &wire.EndorsementFrame{SPKI: u.SPKI, E: stolen.E, Sig: stolen.Sig},
		AdmitAnyone: true,
	})
	if err != nil {
		t.Fatal(err)
	}
	defer thief.Close()
	to.set(thief.Addr())
	var ee *fake.EndorsementError
	if _, err := p.Connect(ctx(t), rec, nil); !errors.As(err, &ee) || ee.Field != "sequence" {
		t.Fatalf("phone took the old key: %v", err)
	}
	if err := u.Connect(ctx(t)); !errors.Is(err, fake.ErrPin) && !strings.Contains(err.Error(), "not accepted") {
		t.Fatalf("unit took the old key: %v", err)
	}
	if seen := thief.Seen(); len(seen) != 1 || seen[0] != "which" {
		t.Fatalf("sent to the old key: %v", seen)
	}
}

// Threat model §5.2: a relay presenting the backup without a valid
// key_proof changes nothing.
func TestLyingProofRefused(t *testing.T) {
	keyA, keyB := fake.NewKey(), fake.NewKey()
	ln := listen(t)
	to := newTarget(ln.Addr().String())
	u := newUnit(t, to)
	e := start(t, ln, opts{key: keyA, backup: keyB, units: [][32]byte{u.ID}})
	e.setUp(u)
	u.Disconnect()
	var before fake.Unit
	u.Set(func(u *fake.Unit) { before.A, before.B, before.Seq = u.A, u.B, u.Seq })
	for name, lies := range map[string]liar.Lies{
		"other signer": {ProofSigner: fake.NewKey()},
		"own nonce":    {ProofOwnNonce: true},
		"other key":    {ProofSPKI: wire.SPKI(&fake.NewKey().PublicKey)},
	} {
		l, err := liar.Start("127.0.0.1:0", keyB, lies)
		if err != nil {
			t.Fatal(err)
		}
		to.set(l.Addr())
		if err := u.Connect(ctx(t)); !errors.Is(err, fake.ErrProof) {
			t.Errorf("%s: %v", name, err)
		}
		u.Set(func(u *fake.Unit) {
			if u.A != before.A || u.B != before.B || u.Seq != before.Seq || u.Promoted != 0 {
				t.Errorf("%s: state changed", name)
			}
		})
		l.Close()
	}
}
