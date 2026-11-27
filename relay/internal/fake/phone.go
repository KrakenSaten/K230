// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package fake

import (
	"bufio"
	"context"
	"crypto/ecdsa"
	"crypto/tls"
	"encoding/base64"
	"fmt"
	"strings"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// Phone stands in for the app's RelayTransport (RELAY_PROPOSAL.md §4.2,
// §11): the pinned path when it knows the relay's key, the provisional path
// otherwise, the endorsement check, the client login, and the inner
// gateway TLS with its pin on the unit.
type Phone struct {
	Key  *ecdsa.PrivateKey
	SPKI []byte
	FP   [32]byte
	Dial Dialer
}

// NewPhone makes a phone with a fresh key.
func NewPhone(dial Dialer) *Phone {
	k := NewKey()
	spki := wire.SPKI(&k.PublicKey)
	return &Phone{Key: k, SPKI: spki, FP: wire.Fingerprint(spki), Dial: dial}
}

// Record is what the app stores per unit for the relay.
type Record struct {
	DeviceID [32]byte
	Name     string
	Port     uint16
	Pin      *[32]byte // the relay key hash, once known
	ConfigID *uint64   // nil until the first accepted endorsement
	Seq      uint64
}

// EndorsementError names the check an endorsement failed.
type EndorsementError struct{ Field string }

func (e *EndorsementError) Error() string { return "fake: endorsement refused: " + e.Field }

// CheckEndorsement is the provisional path's check (RELAY_PROPOSAL.md §4.2,
// step 2): the unit key is the pinned one, its signature verifies, the
// name and port are those dialled, the key is the presented leaf, the
// configuration id is the stored one (any at a first pairing) and the
// sequence is not lower than the stored one.
func CheckEndorsement(rec *Record, presented [32]byte, ef *wire.EndorsementFrame) (wire.Endorsement, error) {
	if wire.Fingerprint(ef.SPKI) != rec.DeviceID {
		return wire.Endorsement{}, &EndorsementError{"unit key"}
	}
	if !wire.VerifySPKI(ef.SPKI, ef.E, ef.Sig) {
		return wire.Endorsement{}, &EndorsementError{"signature"}
	}
	e, err := wire.ParseEndorsement(ef.E)
	switch {
	case err != nil:
		return e, &EndorsementError{"format"}
	case e.Name != rec.Name:
		return e, &EndorsementError{"name"}
	case e.Port != rec.Port:
		return e, &EndorsementError{"port"}
	case e.KeyHash != presented:
		return e, &EndorsementError{"key"}
	case rec.ConfigID != nil && *rec.ConfigID != e.ConfigID:
		return e, &EndorsementError{"config id"}
	case rec.ConfigID != nil && e.Seq < rec.Seq:
		return e, &EndorsementError{"sequence"}
	}
	return e, nil
}

// Session is a phone's gateway session through the relay.
type Session struct {
	Relay       *Conn
	Inner       *tls.Conn
	R           *bufio.Reader
	Role        string // "paired" or "candidate"
	Provisional bool
	// BeforeAccept is every byte the phone sent before it accepted the
	// endorsement, on the provisional path.
	BeforeAccept []byte
}

// Connect reaches rec's unit through the relay. slot is set by a pairing
// candidate. rec is updated as the app would store it.
func (p *Phone) Connect(ctx context.Context, rec *Record, slot *[16]byte) (*Session, error) {
	c, err := DialRelay(ctx, p.Dial, rec.Name, nil)
	if err != nil {
		return nil, err
	}
	ok := false
	defer func() {
		if !ok {
			c.Close()
		}
	}()
	pinned := rec.Pin != nil && *rec.Pin == c.Presented
	if err := c.Start(rec.Name, rec.Port); err != nil {
		return nil, err
	}
	s := &Session{Relay: c, Provisional: !pinned}
	if !pinned {
		if err := c.Send(&wire.Which{Lookup: wire.Lookup(c.Presented, rec.DeviceID)}); err != nil {
			return nil, err
		}
		m, err := c.Recv()
		if err != nil {
			return nil, err
		}
		ef, isE := m.(*wire.EndorsementFrame)
		if !isE {
			return nil, ErrUnexpected
		}
		e, err := CheckEndorsement(rec, c.Presented, ef)
		if err != nil {
			return nil, err
		}
		s.BeforeAccept = c.Written()
		pin, id := c.Presented, e.ConfigID
		rec.Pin, rec.ConfigID, rec.Seq = &pin, &id, e.Seq
	}
	sig, err := wire.Sign(p.Key, wire.ClientLoginMsg(c.Nonce, c.Presented, rec.DeviceID))
	if err != nil {
		return nil, err
	}
	if err := c.Send(&wire.ClientLogin{SPKI: p.SPKI, DeviceID: rec.DeviceID, Sig: sig}); err != nil {
		return nil, err
	}
	if err := c.Send(&wire.Connect{Slot: slot}); err != nil {
		return nil, err
	}
	c.TLS.SetReadDeadline(time.Now().Add(10 * time.Second))
	m, err := c.Recv()
	if err != nil {
		return nil, err
	}
	if _, isR := m.(*wire.Ready); !isR {
		return nil, ErrUnexpected
	}
	cfg := &tls.Config{
		Certificates:           []tls.Certificate{SelfSigned(p.Key, "doors-companion")},
		MinVersion:             tls.VersionTLS13,
		ServerName:             "doors-gateway",
		InsecureSkipVerify:     true, // replaced by the gateway pin check below
		VerifyConnection:       exactlyOnePin(func(h [32]byte) bool { return h == rec.DeviceID }, nil),
		SessionTicketsDisabled: true,
	}
	inner := tls.Client(c.Stream(), cfg)
	inner.SetDeadline(time.Now().Add(10 * time.Second))
	if err := inner.Handshake(); err != nil {
		return nil, err
	}
	r := bufio.NewReader(inner)
	line, err := readLine(r)
	if err != nil {
		return nil, err
	}
	inner.SetDeadline(time.Time{})
	if !strings.HasPrefix(line, "hello ") {
		return nil, ErrUnexpected
	}
	s.Inner, s.R, s.Role = inner, r, line[6:]
	ok = true
	return s, nil
}

// Echo sends a line through the gateway session and reads it back.
func (s *Session) Echo(text string) (string, error) {
	s.Inner.SetDeadline(time.Now().Add(10 * time.Second))
	defer s.Inner.SetDeadline(time.Time{})
	if _, err := fmt.Fprintf(s.Inner, "echo %s\n", text); err != nil {
		return "", err
	}
	return readLine(s.R)
}

// Pair presents the pairing token, as gateway.pair does, and returns the
// gateway's answer.
func (s *Session) Pair(token []byte) (string, error) {
	s.Inner.SetDeadline(time.Now().Add(10 * time.Second))
	if _, err := fmt.Fprintf(s.Inner, "pair %s\n", base64.RawURLEncoding.EncodeToString(token)); err != nil {
		return "", err
	}
	return readLine(s.R)
}

// Done reports whether the session has ended, waiting up to d.
func (s *Session) Done(d time.Duration) bool {
	s.Inner.SetReadDeadline(time.Now().Add(d))
	_, err := s.R.ReadByte()
	if err == nil {
		return false
	}
	if ne, ok := err.(interface{ Timeout() bool }); ok && ne.Timeout() {
		return false
	}
	return true
}

// Close ends the session.
func (s *Session) Close() {
	if s.Inner != nil {
		s.Inner.Close()
	}
	s.Relay.Close()
}
