// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package fake

import (
	"bufio"
	"context"
	"crypto/ecdsa"
	"crypto/rand"
	"crypto/subtle"
	"crypto/tls"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// Unit stands in for gatewayd and its helper, as far as the relay sees
// them: relay.v1 (pins A and B, configuration id, sequence, the phones
// holding remote), set-up with the signed backup, one-way promotion after
// key_proof, unit login only for pinned keys, the control connection, and
// a tiny inner "gateway" on each data connection that applies the pins and
// the remote check of ADR-019 and its amendment 3.
type Unit struct {
	Key  *ecdsa.PrivateKey
	SPKI []byte
	ID   [32]byte // device_id
	Name string
	Port uint16
	Dial Dialer

	mu        sync.Mutex
	A, B      *[32]byte
	ConfigID  uint64
	Seq       uint64
	Paired    map[[32]byte]bool
	Remote    map[[32]byte]bool
	Decline   bool // decline every invitation
	Manual    bool // record invitations only; the test joins
	Promoted  int  // promotions done (at most one per configuration)
	token     []byte
	approveRm bool
	n         uint32
	ctl       *Conn
	ctlWmu    sync.Mutex
	admitted  chan uint32
	sessions  map[[32]byte]net.Conn
	invites   chan wire.Invite
	ctlDone   chan struct{}
}

// NewUnit makes a unit with a fresh device key.
func NewUnit(name string, port uint16, dial Dialer) *Unit {
	k := NewKey()
	spki := wire.SPKI(&k.PublicKey)
	return &Unit{
		Key: k, SPKI: spki, ID: wire.Fingerprint(spki), Name: name, Port: port, Dial: dial,
		Paired: map[[32]byte]bool{}, Remote: map[[32]byte]bool{},
		admitted: make(chan uint32, 8), sessions: map[[32]byte]net.Conn{},
		invites: make(chan wire.Invite, 16),
	}
}

// ErrBackup is a backup the unit does not accept.
var ErrBackup = errors.New("fake: backup refused")

// ErrProof is a key_proof the unit does not accept.
var ErrProof = errors.New("fake: key proof refused")

// Probe connects and reports the relay key presented (set-up step 2).
func (u *Unit) Probe(ctx context.Context) ([32]byte, error) {
	c, err := DialRelay(ctx, u.Dial, u.Name, nil)
	if err != nil {
		return [32]byte{}, err
	}
	defer c.Close()
	return c.Presented, nil
}

// Setup is set-up steps 3 to 6 once a person confirmed key confirmed: over
// a connection presenting it, the unit logs in and takes the backup pin,
// accepting it only with a valid signature by the confirmed key over its
// own nonce. A new configuration id is drawn and seq set to 1.
func (u *Unit) Setup(ctx context.Context, confirmed [32]byte) error {
	c, err := DialRelay(ctx, u.Dial, u.Name, func(h [32]byte) bool { return h == confirmed })
	if err != nil {
		return err
	}
	defer c.Close()
	if err := c.Start(u.Name, u.Port); err != nil {
		return err
	}
	if err := u.signLogin(c); err != nil { // c.Presented == confirmed
		return err
	}
	var nonce [32]byte
	rand.Read(nonce[:])
	if err := c.Send(&wire.GetBackup{Nonce: nonce}); err != nil {
		return err
	}
	m, err := c.Recv()
	if err != nil {
		return err
	}
	b, ok := m.(*wire.Backup)
	if !ok || wire.Fingerprint(b.SPKIA) != confirmed ||
		!wire.VerifySPKI(b.SPKIA, wire.BackupMsg(b.SPKIHashB, nonce), b.Sig) || b.SPKIHashB == confirmed {
		return ErrBackup
	}
	var id [8]byte
	rand.Read(id[:])
	u.mu.Lock()
	a, bb := confirmed, b.SPKIHashB
	u.A, u.B = &a, &bb
	u.ConfigID = binary.BigEndian.Uint64(id[:])
	u.Seq = 1
	u.Promoted = 0
	u.mu.Unlock()
	return nil
}

func (u *Unit) isPin(h [32]byte) bool {
	u.mu.Lock()
	defer u.mu.Unlock()
	return u.A != nil && *u.A == h || u.B != nil && *u.B == h
}

// signLogin signs a unit login for the key c presented. Callers make sure
// it is a pin, or, during set-up, the key the person confirmed (rule 31).
func (u *Unit) signLogin(c *Conn) error {
	sig, err := wire.Sign(u.Key, wire.UnitLoginMsg(c.Nonce, c.Presented))
	if err != nil {
		return err
	}
	return c.Send(&wire.UnitLogin{SPKI: u.SPKI, Sig: sig})
}

// Endorse returns the endorsement of the active pin at the current sequence.
func (u *Unit) Endorse() wire.SignedEndorsement {
	u.mu.Lock()
	e := wire.Endorsement{ConfigID: u.ConfigID, Seq: u.Seq, Name: u.Name, Port: u.Port, KeyHash: *u.A}
	u.mu.Unlock()
	b, err := e.Marshal()
	if err != nil {
		panic(err)
	}
	sig, err := wire.Sign(u.Key, b)
	if err != nil {
		panic(err)
	}
	return wire.SignedEndorsement{E: b, Sig: sig}
}

func (u *Unit) remoteList() [][32]byte {
	var l [][32]byte
	for fp := range u.Remote {
		if u.Paired[fp] {
			l = append(l, fp)
		}
	}
	return l
}

// Connect opens the control connection: the relay must present A or B.
// If it presents B, the unit asks for key_proof, and promotes B only when
// the proof verifies (once per configuration), then endorses it with
// seq+1. Then it logs in and registers.
func (u *Unit) Connect(ctx context.Context) error {
	c, err := DialRelay(ctx, u.Dial, u.Name, u.isPin)
	if err != nil {
		return err
	}
	if err := c.Start(u.Name, u.Port); err != nil {
		c.Close()
		return err
	}
	if err := u.loginPinned(c); err != nil {
		c.Close()
		return err
	}
	u.mu.Lock()
	isB := u.B != nil && *u.B == c.Presented
	u.mu.Unlock()
	if isB {
		if err := u.promote(c); err != nil {
			c.Close()
			return err
		}
	}
	u.mu.Lock()
	admit := u.remoteList()
	u.mu.Unlock()
	if err := c.Send(&wire.Register{Admit: admit, Endorsement: u.Endorse()}); err != nil {
		c.Close()
		return err
	}
	done := make(chan struct{})
	u.mu.Lock()
	u.ctl, u.ctlDone = c, done
	u.mu.Unlock()
	go u.control(c, done)
	return nil
}

func (u *Unit) loginPinned(c *Conn) error {
	if !u.isPin(c.Presented) {
		return ErrPin
	}
	return u.signLogin(c)
}

func (u *Unit) promote(c *Conn) error {
	var nonce [32]byte
	rand.Read(nonce[:])
	if err := c.Send(&wire.Prove{Nonce: nonce}); err != nil {
		return err
	}
	m, err := c.Recv()
	if err != nil {
		return err
	}
	p, ok := m.(*wire.KeyProof)
	u.mu.Lock()
	defer u.mu.Unlock()
	if !ok || u.B == nil || wire.Fingerprint(p.SPKI) != *u.B || u.Promoted > 0 ||
		!wire.VerifySPKI(p.SPKI, wire.KeyProofMsg(nonce), p.Sig) {
		return ErrProof
	}
	// One durable write: A := B, B := none, seq+1.
	b := *u.B
	u.A, u.B = &b, nil
	u.Seq++
	u.Promoted++
	return nil
}

func (u *Unit) send(m wire.Msg) error {
	u.mu.Lock()
	c := u.ctl
	u.mu.Unlock()
	if c == nil {
		return errors.New("fake: no control connection")
	}
	u.ctlWmu.Lock()
	defer u.ctlWmu.Unlock()
	return c.Send(m)
}

func (u *Unit) control(c *Conn, done chan struct{}) {
	defer close(done)
	defer c.Close()
	for {
		c.TLS.SetReadDeadline(time.Now().Add(2 * time.Minute))
		m, err := c.Recv()
		if err != nil {
			return
		}
		switch m := m.(type) {
		case *wire.Ping:
			u.ctlWmu.Lock()
			c.Send(&wire.Pong{N: m.N})
			u.ctlWmu.Unlock()
		case *wire.Pong:
		case *wire.Admitted:
			select {
			case u.admitted <- m.N:
			default:
			}
		case *wire.Invite:
			select {
			case u.invites <- *m:
			default:
			}
			go u.handleInvite(*m)
		default:
			return
		}
	}
}

// Invites delivers the invitations the unit received.
func (u *Unit) Invites() <-chan wire.Invite { return u.invites }

// Disconnect closes the control connection and waits for it to end.
func (u *Unit) Disconnect() {
	u.mu.Lock()
	c, done := u.ctl, u.ctlDone
	u.ctl = nil
	u.mu.Unlock()
	if c != nil {
		c.Close()
		<-done
	}
}

// ControlDone is closed when the current control connection ends.
func (u *Unit) ControlDone() <-chan struct{} {
	u.mu.Lock()
	defer u.mu.Unlock()
	return u.ctlDone
}

// SendAdmit sends the admission list and waits for the relay's
// confirmation (at most 3 s, as gatewayd waits).
func (u *Unit) SendAdmit() (bool, error) {
	u.mu.Lock()
	u.n++
	n := u.n
	l := u.remoteList()
	u.mu.Unlock()
	if err := u.send(&wire.Admit{Admit: l, N: n}); err != nil {
		return false, err
	}
	t := time.NewTimer(3 * time.Second)
	defer t.Stop()
	for {
		select {
		case got := <-u.admitted:
			if got == n {
				return true, nil
			}
		case <-t.C:
			return false, nil
		}
	}
}

// SetRemote gives or takes `remote`. Taking it closes the phone's session
// at the gateway as well as at the relay (rule 33).
func (u *Unit) SetRemote(phone [32]byte, on bool) (bool, error) {
	u.mu.Lock()
	if on {
		u.Remote[phone] = true
	} else {
		delete(u.Remote, phone)
	}
	s := u.sessions[phone]
	u.mu.Unlock()
	if !on && s != nil {
		s.Close()
	}
	return u.SendAdmit()
}

// OpenWindow opens a pairing window; with relay, the relay gets its slot.
// approveRemote stands for the person switching "Through the relay" on.
func (u *Unit) OpenWindow(relay, approveRemote bool) ([]byte, error) {
	tok := make([]byte, 16)
	rand.Read(tok)
	u.mu.Lock()
	u.token, u.approveRm = tok, approveRemote
	u.mu.Unlock()
	if relay {
		slot := wire.SlotFromToken(tok)
		if err := u.send(&wire.PairSlot{Slot: &slot}); err != nil {
			return nil, err
		}
	}
	return tok, nil
}

// CloseWindow ends the pairing window.
func (u *Unit) CloseWindow() error {
	u.mu.Lock()
	u.token = nil
	u.mu.Unlock()
	return u.send(&wire.PairSlot{})
}

// HasSession reports whether a phone has a live inner session.
func (u *Unit) HasSession(phone [32]byte) bool {
	u.mu.Lock()
	defer u.mu.Unlock()
	return u.sessions[phone] != nil
}

// ForgetRemote takes remote at the relay only, leaving the gateway session
// alone, so that a test sees what the relay does by itself.
func (u *Unit) ForgetRemote(phone [32]byte) (bool, error) {
	u.mu.Lock()
	delete(u.Remote, phone)
	u.mu.Unlock()
	return u.SendAdmit()
}

// Set changes the unit's state under its lock (tests).
func (u *Unit) Set(f func(u *Unit)) {
	u.mu.Lock()
	defer u.mu.Unlock()
	f(u)
}

// SendRaw sends any frame on the control connection (tests).
func (u *Unit) SendRaw(m wire.Msg) error { return u.send(m) }

func (u *Unit) handleInvite(inv wire.Invite) {
	u.mu.Lock()
	decline, manual := u.Decline, u.Manual
	u.mu.Unlock()
	if manual {
		return
	}
	if decline {
		u.send(&wire.Decline{Cookie: inv.Cookie})
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	d, err := DialRelay(ctx, u.Dial, u.Name, func(h [32]byte) bool {
		u.mu.Lock()
		defer u.mu.Unlock()
		return u.A != nil && *u.A == h
	})
	if err != nil {
		return
	}
	defer d.Close()
	if d.Start(u.Name, u.Port) != nil || d.Send(&wire.Join{Cookie: inv.Cookie}) != nil {
		return
	}
	if m, err := d.Recv(); err != nil {
		return
	} else if _, ok := m.(*wire.Ready); !ok {
		return
	}
	var phone [32]byte
	cfg := &tls.Config{
		Certificates: []tls.Certificate{SelfSigned(u.Key, "doors-gateway")},
		MinVersion:   tls.VersionTLS13,
		ClientAuth:   tls.RequireAnyClientCert,
		VerifyConnection: exactlyOnePin(func(h [32]byte) bool {
			u.mu.Lock()
			defer u.mu.Unlock()
			return u.Paired[h] || u.token != nil
		}, &phone),
		SessionTicketsDisabled: true,
	}
	inner := tls.Server(d.Stream(), cfg)
	inner.SetDeadline(time.Now().Add(10 * time.Second))
	if inner.Handshake() != nil {
		return
	}
	inner.SetDeadline(time.Time{})
	u.mu.Lock()
	role := ""
	switch {
	case u.Paired[phone] && u.Remote[phone]:
		role = "paired"
	case u.Paired[phone]:
		// A relay session of a paired phone without remote: whatever the
		// relay admitted, the gateway closes it (rule 32).
	case u.token != nil:
		role = "candidate"
	}
	if role != "" {
		if old := u.sessions[phone]; old != nil {
			old.Close() // a phone's new session replaces its older one
		}
		u.sessions[phone] = inner
	}
	u.mu.Unlock()
	if role == "" {
		inner.Close()
		return
	}
	defer func() {
		u.mu.Lock()
		if u.sessions[phone] == inner {
			delete(u.sessions, phone)
		}
		u.mu.Unlock()
		inner.Close()
	}()
	if _, err := inner.Write([]byte("hello " + role + "\n")); err != nil {
		return
	}
	r := bufio.NewReader(inner)
	for {
		line, err := readLine(r)
		if err != nil {
			return
		}
		switch {
		case strings.HasPrefix(line, "echo "):
			if _, err := inner.Write([]byte(line[5:] + "\n")); err != nil {
				return
			}
		case role == "candidate" && strings.HasPrefix(line, "pair "):
			tok, err := base64.RawURLEncoding.DecodeString(line[5:])
			u.mu.Lock()
			ok := err == nil && u.token != nil && subtle.ConstantTimeCompare(tok, u.token) == 1
			remote := u.approveRm
			if ok {
				u.Paired[phone] = true
				if remote {
					u.Remote[phone] = true
				}
				u.token = nil
			}
			u.mu.Unlock()
			if !ok {
				inner.Write([]byte("refused\n"))
				return
			}
			answer := "paired\n"
			if remote {
				// The phone is told it is paired with remote only after the
				// relay confirmed the new list.
				if ok, _ := u.SendAdmit(); ok {
					answer = "paired remote\n"
				}
			}
			u.send(&wire.PairSlot{})
			inner.Write([]byte(answer))
			return
		default:
			return
		}
	}
}
