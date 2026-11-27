// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

// Package liar is a lying DOORS Relay: a small, separate relay that speaks
// the protocol well enough to be used, and lies in the ways a hostile or
// compromised relay could (threat model R1). Each lie is switched on by
// itself. The Go tests of this phase use it, and phases 2 and 3 can run it
// as cmd/lying-relay against gatewayd, its helper and the app.
//
// It is test code: it verifies no login and enrols every unit that logs in.
package liar

import (
	"bufio"
	"crypto/ecdsa"
	"crypto/rand"
	"crypto/subtle"
	"crypto/tls"
	"io"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// Lies are the ways the relay lies. The zero value is an honest (but
// unauthenticating) relay.
type Lies struct {
	// Cert replaces the self-signed certificate for Key: another key, an
	// RSA key, or a chain.
	Cert *tls.Certificate
	// TLS12 offers TLS 1.2 only.
	TLS12 bool

	// BackupHash is the backup it claims; nil answers get_backup unavailable.
	BackupHash *[32]byte
	// BackupSigner signs `backup` instead of Key.
	BackupSigner *ecdsa.PrivateKey
	// BackupOwnNonce signs `backup` over a nonce of its own.
	BackupOwnNonce bool
	// UnsolicitedBackup sends a `backup` right after the unit's login.
	UnsolicitedBackup bool
	// ProofSigner signs `key_proof` instead of Key.
	ProofSigner *ecdsa.PrivateKey
	// ProofSPKI is the key `key_proof` claims, instead of Key's.
	ProofSPKI []byte
	// ProofOwnNonce signs `key_proof` over a nonce of its own.
	ProofOwnNonce bool

	// WithholdAdmitted never confirms an `admit`.
	WithholdAdmitted bool
	// IgnoreRemovals keeps the session of a phone removed from the list.
	IgnoreRemovals bool
	// AdmitAnyone invites every phone that connects, listed or not, with
	// any slot or none.
	AdmitAnyone bool
	// Endorsement answers every `which` with this frame (forged, stale, or
	// another unit's), whatever the lookup.
	Endorsement *wire.EndorsementFrame

	// After `register`: a frame above 4 KiB, a frame that is not JSON, a
	// burst of pings, an invitation with a cookie nobody asked for.
	Oversize, Garbage bool
	PingFlood         int
	BogusInvite       bool
}

// Liar is one lying relay.
type Liar struct {
	Key  *ecdsa.PrivateKey
	Lies Lies

	keyHash [32]byte
	cfg     *tls.Config
	ln      net.Listener

	mu      sync.Mutex
	units   map[[32]byte]*lunit
	cookies map[[16]byte]*lsplice
	conns   map[net.Conn]struct{}
	closed  bool
	wg      sync.WaitGroup
	seen    []string

	withhold atomic.Bool // Lies.WithholdAdmitted, switchable while running
}

// SetWithholdAdmitted switches the withholding of `admitted` while the
// relay runs.
func (l *Liar) SetWithholdAdmitted(on bool) { l.withhold.Store(on) }

type lunit struct {
	id       [32]byte
	spki     []byte
	ctl      *lconn
	wmu      *sync.Mutex
	admit    map[[32]byte]bool
	endo     *wire.EndorsementFrame
	slot     *[16]byte
	sessions map[[32]byte]*lsplice
}

type lsplice struct {
	u      *lunit
	phone  [32]byte
	cookie [16]byte
	data   chan *lconn
	done   chan struct{}
	once   sync.Once
}

func (sp *lsplice) end() { sp.once.Do(func() { close(sp.done) }) }

type lconn struct {
	raw   net.Conn
	tc    *tls.Conn
	br    *bufio.Reader
	nonce [32]byte
}

func (c *lconn) send(m wire.Msg) error {
	c.raw.SetWriteDeadline(time.Now().Add(10 * time.Second))
	return wire.WriteMsg(c.tc, m)
}

// Start listens on addr ("127.0.0.1:0" for tests).
func Start(addr string, key *ecdsa.PrivateKey, lies Lies) (*Liar, error) {
	l := &Liar{
		Key: key, Lies: lies,
		units: map[[32]byte]*lunit{}, cookies: map[[16]byte]*lsplice{}, conns: map[net.Conn]struct{}{},
	}
	l.keyHash = wire.Fingerprint(wire.SPKI(&key.PublicKey))
	l.withhold.Store(lies.WithholdAdmitted)
	cert := fake.SelfSigned(key, "doors-relay")
	if lies.Cert != nil {
		cert = *lies.Cert
	}
	l.cfg = &tls.Config{
		Certificates: []tls.Certificate{cert},
		MinVersion:   tls.VersionTLS13,
		NextProtos:   []string{wire.ALPN},
	}
	if lies.TLS12 {
		l.cfg.MinVersion, l.cfg.MaxVersion = tls.VersionTLS12, tls.VersionTLS12
	}
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, err
	}
	l.ln = ln
	go l.serve()
	return l, nil
}

// Addr is the listening address.
func (l *Liar) Addr() string { return l.ln.Addr().String() }

// KeyHash is the SHA-256 of the SPKI of Key.
func (l *Liar) KeyHash() [32]byte { return l.keyHash }

// Seen lists the frame types clients sent, in order.
func (l *Liar) Seen() []string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return append([]string(nil), l.seen...)
}

// Close stops the relay and ends every connection.
func (l *Liar) Close() {
	l.mu.Lock()
	l.closed = true
	for c := range l.conns {
		c.Close()
	}
	l.mu.Unlock()
	l.ln.Close()
	l.wg.Wait()
}

func (l *Liar) serve() {
	for {
		raw, err := l.ln.Accept()
		if err != nil {
			return
		}
		l.mu.Lock()
		if l.closed {
			l.mu.Unlock()
			raw.Close()
			return
		}
		l.conns[raw] = struct{}{}
		l.wg.Add(1)
		l.mu.Unlock()
		go func() {
			defer l.wg.Done()
			defer func() {
				l.mu.Lock()
				delete(l.conns, raw)
				l.mu.Unlock()
				raw.Close()
			}()
			l.handle(raw)
		}()
	}
}

func (l *Liar) note(t string) {
	l.mu.Lock()
	l.seen = append(l.seen, t)
	l.mu.Unlock()
}

func (l *Liar) handle(raw net.Conn) {
	raw.SetDeadline(time.Now().Add(10 * time.Second))
	tc := tls.Server(raw, l.cfg)
	if tc.Handshake() != nil {
		return
	}
	c := &lconn{raw: raw, tc: tc, br: bufio.NewReaderSize(tc, 2*wire.MaxFrame)}
	if _, err := wire.ReadHead(c.br); err != nil {
		return
	}
	rand.Read(c.nonce[:])
	out := wire.AppendFrame(append([]byte(nil), wire.Switching...), wire.Encode(&wire.Challenge{Nonce: c.nonce}))
	if _, err := tc.Write(out); err != nil {
		return
	}
	m, err := wire.ReadMsg(c.br)
	if err != nil {
		return
	}
	l.note(m.Type())
	switch m := m.(type) {
	case *wire.UnitLogin:
		l.control(c, m)
	case *wire.Which, *wire.ClientLogin:
		l.client(c, m)
	case *wire.Join:
		l.join(c, m)
	}
}

func (l *Liar) control(c *lconn, login *wire.UnitLogin) {
	u := &lunit{id: wire.Fingerprint(login.SPKI), spki: login.SPKI, ctl: c, wmu: &sync.Mutex{},
		sessions: map[[32]byte]*lsplice{}}
	send := func(m wire.Msg) {
		u.wmu.Lock()
		defer u.wmu.Unlock()
		c.send(m)
	}
	key := l.Key
	if l.Lies.UnsolicitedBackup && l.Lies.BackupHash != nil {
		var n [32]byte
		rand.Read(n[:])
		sig, _ := wire.Sign(key, wire.BackupMsg(*l.Lies.BackupHash, n))
		send(&wire.Backup{SPKIA: wire.SPKI(&key.PublicKey), SPKIHashB: *l.Lies.BackupHash, Sig: sig})
	}
	for {
		c.raw.SetReadDeadline(time.Now().Add(2 * time.Minute))
		m, err := wire.ReadMsg(c.br)
		if err != nil {
			break
		}
		switch m := m.(type) {
		case *wire.Ping:
			send(&wire.Pong{N: m.N})
		case *wire.Prove:
			signer, nonce, spki := key, m.Nonce, wire.SPKI(&key.PublicKey)
			if l.Lies.ProofSigner != nil {
				signer = l.Lies.ProofSigner
			}
			if l.Lies.ProofOwnNonce {
				rand.Read(nonce[:])
			}
			if l.Lies.ProofSPKI != nil {
				spki = l.Lies.ProofSPKI
			}
			sig, _ := wire.Sign(signer, wire.KeyProofMsg(nonce))
			send(&wire.KeyProof{SPKI: spki, Sig: sig})
		case *wire.GetBackup:
			if l.Lies.BackupHash == nil {
				send(&wire.Unavailable{})
				return
			}
			signer, nonce := key, m.Nonce
			if l.Lies.BackupSigner != nil {
				signer = l.Lies.BackupSigner
			}
			if l.Lies.BackupOwnNonce {
				rand.Read(nonce[:])
			}
			sig, _ := wire.Sign(signer, wire.BackupMsg(*l.Lies.BackupHash, nonce))
			send(&wire.Backup{SPKIA: wire.SPKI(&key.PublicKey), SPKIHashB: *l.Lies.BackupHash, Sig: sig})
		case *wire.Register:
			l.mu.Lock()
			u.admit = map[[32]byte]bool{}
			for _, a := range m.Admit {
				u.admit[a] = true
			}
			u.endo = &wire.EndorsementFrame{SPKI: u.spki, E: m.Endorsement.E, Sig: m.Endorsement.Sig}
			l.units[u.id] = u
			l.mu.Unlock()
			send(&wire.Registered{})
			l.afterRegister(c, u)
		case *wire.Admit:
			l.mu.Lock()
			old := u.admit
			u.admit = map[[32]byte]bool{}
			for _, a := range m.Admit {
				u.admit[a] = true
			}
			var end []*lsplice
			if !l.Lies.IgnoreRemovals {
				for fp, sp := range u.sessions {
					if old[fp] && !u.admit[fp] {
						end = append(end, sp)
					}
				}
			}
			l.mu.Unlock()
			for _, sp := range end {
				sp.end()
			}
			if !l.withhold.Load() {
				send(&wire.Admitted{N: m.N})
			}
		case *wire.PairSlot:
			l.mu.Lock()
			u.slot = m.Slot
			l.mu.Unlock()
		case *wire.Decline:
			l.mu.Lock()
			sp := l.cookies[m.Cookie]
			l.mu.Unlock()
			if sp != nil {
				sp.end()
			}
		}
	}
	l.mu.Lock()
	if l.units[u.id] == u {
		delete(l.units, u.id)
	}
	l.mu.Unlock()
}

func (l *Liar) afterRegister(c *lconn, u *lunit) {
	u.wmu.Lock()
	defer u.wmu.Unlock()
	c.raw.SetWriteDeadline(time.Now().Add(10 * time.Second))
	if l.Lies.Oversize {
		big := make([]byte, wire.MaxFrame+904)
		for i := range big {
			big[i] = ' '
		}
		copy(big, `{"type":"ping","n":1}`)
		c.tc.Write(wire.AppendFrame(nil, big))
	}
	if l.Lies.Garbage {
		c.tc.Write(wire.AppendFrame(nil, []byte("{not json")))
	}
	for i := 0; i < l.Lies.PingFlood; i++ {
		wire.WriteMsg(c.tc, &wire.Ping{N: uint32(i)})
	}
	if l.Lies.BogusInvite {
		var ck [16]byte
		rand.Read(ck[:])
		wire.WriteMsg(c.tc, &wire.Invite{Cookie: ck, Client: [32]byte{1}, Via: wire.ViaAdmit})
	}
}

func (l *Liar) client(c *lconn, first wire.Msg) {
	m := first
	if w, ok := m.(*wire.Which); ok {
		ef := l.Lies.Endorsement
		if ef == nil {
			l.mu.Lock()
			for id, u := range l.units {
				if wire.Lookup(l.keyHash, id) == w.Lookup {
					ef = u.endo
				}
			}
			l.mu.Unlock()
		}
		if ef == nil {
			c.send(&wire.Unavailable{})
			return
		}
		if c.send(ef) != nil {
			return
		}
		var err error
		if m, err = wire.ReadMsg(c.br); err != nil {
			return
		}
		l.note(m.Type())
	}
	login, ok := m.(*wire.ClientLogin)
	if !ok {
		return
	}
	m, err := wire.ReadMsg(c.br)
	if err != nil {
		return
	}
	l.note(m.Type())
	req, ok := m.(*wire.Connect)
	if !ok {
		return
	}
	phone := wire.Fingerprint(login.SPKI)
	l.mu.Lock()
	u := l.units[login.DeviceID]
	admitted := u != nil && (l.Lies.AdmitAnyone ||
		req.Slot == nil && u.admit[phone] ||
		req.Slot != nil && u.slot != nil && subtle.ConstantTimeCompare(req.Slot[:], u.slot[:]) == 1)
	var sp *lsplice
	if admitted {
		sp = &lsplice{u: u, phone: phone, data: make(chan *lconn, 1), done: make(chan struct{})}
		rand.Read(sp.cookie[:])
		if old := u.sessions[phone]; old != nil {
			old.end()
		}
		u.sessions[phone] = sp
		l.cookies[sp.cookie] = sp
	}
	l.mu.Unlock()
	if !admitted {
		c.send(&wire.Unavailable{})
		return
	}
	defer func() {
		sp.end()
		l.mu.Lock()
		if u.sessions[phone] == sp {
			delete(u.sessions, phone)
		}
		delete(l.cookies, sp.cookie)
		l.mu.Unlock()
	}()
	via := wire.ViaAdmit
	if req.Slot != nil {
		via = wire.ViaPair
	}
	u.wmu.Lock()
	u.ctl.send(&wire.Invite{Cookie: sp.cookie, Client: phone, Via: via})
	u.wmu.Unlock()
	var d *lconn
	select {
	case d = <-sp.data:
	case <-sp.done:
	case <-time.After(5 * time.Second):
	}
	if d == nil {
		c.send(&wire.Unavailable{})
		return
	}
	if d.send(&wire.Ready{}) != nil || c.send(&wire.Ready{}) != nil {
		return
	}
	c.raw.SetDeadline(time.Time{})
	d.raw.SetDeadline(time.Time{})
	go func() { io.Copy(d.tc, c.br); sp.end() }()
	go func() { io.Copy(c.tc, d.br); sp.end() }()
	<-sp.done
	c.raw.Close()
	d.raw.Close()
}

func (l *Liar) join(c *lconn, j *wire.Join) {
	l.mu.Lock()
	sp := l.cookies[j.Cookie]
	delete(l.cookies, j.Cookie)
	l.mu.Unlock()
	if sp == nil {
		c.send(&wire.Unavailable{})
		return
	}
	sp.data <- c
	<-sp.done
}
