// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"crypto/rand"
	"crypto/subtle"

	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// unit is an enrolled unit. Everything but id is guarded by Server.mu and
// lives only as long as the unit's registered control connection: the
// relay never routes without the unit's current word.
type unit struct {
	id        [32]byte
	ctl       *ctl              // the registered control connection, or nil (offline)
	admit     map[[32]byte]bool // admission list
	endo      []byte            // the endorsement frame for phones, encoded
	slot      *[16]byte         // pairing slot, while a window allows the relay
	session   *splice           // the one session of a listed phone
	candidate *splice           // the one session of a pairing candidate
}

// Splice states.
const (
	pending = iota // invited; waiting for the unit's join
	joined         // the data connection arrived
	dead
)

// splice is one phone's session through the relay, from its admission
// until either side ends. Its fields after done are read only by its owner
// (the client connection's goroutine).
type splice struct {
	s      *Server
	u      *unit
	phone  [32]byte
	via    string
	ctl    *ctl
	cookie [16]byte
	client *rconn
	data   *rconn

	state  int    // guarded by s.mu
	reason string // guarded by s.mu; set when state becomes dead
	joined chan struct{}
	done   chan struct{}
}

// end ends sp; with pendingOnly, only while no data connection has joined.
// It reports whether this call ended it.
func (sp *splice) end(reason string, pendingOnly bool) bool {
	s := sp.s
	s.mu.Lock()
	if sp.state == dead || pendingOnly && sp.state != pending {
		s.mu.Unlock()
		return false
	}
	sp.state = dead
	sp.reason = reason
	if s.cookies[sp.cookie] == sp {
		delete(s.cookies, sp.cookie)
	}
	if sp.u.session == sp {
		sp.u.session = nil
	}
	if sp.u.candidate == sp {
		sp.u.candidate = nil
	}
	s.mu.Unlock()
	close(sp.done)
	return true
}

func (sp *splice) why() string {
	sp.s.mu.Lock()
	defer sp.s.mu.Unlock()
	return sp.reason
}

func (sp *splice) isJoined() bool {
	sp.s.mu.Lock()
	defer sp.s.mu.Unlock()
	return sp.state == joined
}

// Reasons in logs (constants only, rule 41).
const (
	reasonReplaced   = "replaced"
	reasonRemoved    = "removed from the admission list"
	reasonOffline    = "unit offline"
	reasonDeclined   = "declined"
	reasonNoJoin     = "no join"
	reasonIdle       = "idle"
	reasonClosed     = "closed"
	reasonShutdown   = "shutdown"
	reasonRefused    = "unavailable"
	reasonLogin      = "login refused"
	reasonNoUnit     = "unit unknown or offline"
	reasonSilent     = "silent"
	reasonOversized  = "frame too long"
	reasonMalformed  = "malformed frame"
	reasonOutOfOrder = "frame out of order"
	reasonFrameRate  = "frame rate"
	reasonEndorse    = "endorsement refused"
	reasonNoBackup   = "no backup key"
	reasonQueue      = "write queue full"
	reasonWrite      = "write failed"
)

func toSet(l [][32]byte) map[[32]byte]bool {
	m := make(map[[32]byte]bool, len(l))
	for _, v := range l {
		m[v] = true
	}
	return m
}

// register makes k its unit's control connection, with its admission list
// and endorsement, and replaces an earlier one. Invitations sent over the
// earlier connection end with it; a session survives only if its phone is
// on the new list (a candidate's session survives).
func (s *Server) register(k *ctl, admit [][32]byte, endo []byte) {
	s.mu.Lock()
	u := k.u
	old := u.ctl
	var end []*splice
	for _, sp := range []*splice{u.session, u.candidate} {
		if sp != nil && sp.state == pending && sp.ctl == old {
			end = append(end, sp)
		}
	}
	u.ctl, u.admit, u.endo, u.slot = k, toSet(admit), endo, nil
	if sp := u.session; sp != nil && !u.admit[sp.phone] {
		end = append(end, sp) // end is idempotent
	}
	s.mu.Unlock()
	for _, sp := range end {
		sp.end(reasonReplaced, false)
	}
	if old != nil {
		old.replaced.Store(true)
		old.close()
	}
}

// setAdmit replaces the admission list. A session of a phone no longer on
// it ends before the list is confirmed.
func (s *Server) setAdmit(k *ctl, admit [][32]byte) {
	s.mu.Lock()
	u := k.u
	if u.ctl != k {
		s.mu.Unlock()
		return
	}
	u.admit = toSet(admit)
	var end *splice
	if sp := u.session; sp != nil && !u.admit[sp.phone] {
		end = sp
	}
	s.mu.Unlock()
	if end != nil {
		end.end(reasonRemoved, false)
	}
}

func (s *Server) setSlot(k *ctl, slot *[16]byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if k.u.ctl != k {
		return
	}
	if slot == nil {
		k.u.slot = nil
		return
	}
	v := *slot
	k.u.slot = &v
}

func (s *Server) decline(k *ctl, cookie [16]byte) {
	s.mu.Lock()
	sp := s.cookies[cookie]
	s.mu.Unlock()
	if sp != nil && sp.u == k.u {
		sp.end(reasonDeclined, true)
	}
}

// ctlGone forgets everything the unit said, and ends its sessions, when
// its registered control connection ends without a replacement.
func (s *Server) ctlGone(k *ctl) {
	s.mu.Lock()
	u := k.u
	if u.ctl != k {
		s.mu.Unlock()
		return
	}
	u.ctl, u.admit, u.endo, u.slot = nil, nil, nil, nil
	end := []*splice{u.session, u.candidate}
	s.mu.Unlock()
	for _, sp := range end {
		if sp != nil {
			sp.end(reasonOffline, false)
		}
	}
}

// which returns the endorsement frame for a lookup, or nil.
func (s *Server) which(lookup [32]byte) []byte {
	u := s.lookups[lookup]
	if u == nil {
		return nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if u.ctl == nil {
		return nil
	}
	return u.endo
}

// admitSplice decides a phone's connect. A phone's own earlier session or
// candidacy at the unit is replaced, and does not count against the limit
// of one (rule 37); another phone's is not. It returns nil when the phone
// is refused, and the sessions it replaces.
func (s *Server) admitSplice(deviceID, phone [32]byte, slot *[16]byte) (*splice, []*splice) {
	u := s.units[deviceID]
	if u == nil {
		return nil, nil
	}
	var cookie [16]byte
	if _, err := rand.Read(cookie[:]); err != nil {
		return nil, nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if u.ctl == nil {
		return nil, nil
	}
	var place **splice
	via := wire.ViaAdmit
	if slot != nil {
		if u.slot == nil || subtle.ConstantTimeCompare(slot[:], u.slot[:]) != 1 {
			return nil, nil
		}
		if c := u.candidate; c != nil && c.phone != phone {
			return nil, nil
		}
		place, via = &u.candidate, wire.ViaPair
	} else {
		if !u.admit[phone] {
			return nil, nil
		}
		if c := u.session; c != nil && c.phone != phone {
			return nil, nil
		}
		place = &u.session
	}
	for _, ok := s.cookies[cookie]; ok; _, ok = s.cookies[cookie] {
		if _, err := rand.Read(cookie[:]); err != nil {
			return nil, nil
		}
	}
	var old []*splice
	for _, p := range []**splice{&u.session, &u.candidate} {
		if sp := *p; sp != nil && sp.phone == phone {
			old = append(old, sp)
			*p = nil
		}
	}
	sp := &splice{
		s: s, u: u, phone: phone, via: via, ctl: u.ctl, cookie: cookie,
		joined: make(chan struct{}), done: make(chan struct{}),
	}
	s.cookies[cookie] = sp
	*place = sp
	return sp, old
}

// join matches a data connection to its invitation; each cookie works once.
func (s *Server) join(cookie [16]byte, d *rconn) *splice {
	s.mu.Lock()
	sp := s.cookies[cookie]
	if sp == nil || sp.state != pending {
		s.mu.Unlock()
		return nil
	}
	delete(s.cookies, cookie)
	sp.state = joined
	sp.data = d
	s.mu.Unlock()
	close(sp.joined)
	return sp
}
