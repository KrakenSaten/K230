// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"crypto/rand"
	"crypto/subtle"
	"net"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// unit is an enrolled unit. Everything but id is guarded by Server.mu and
// lives only as long as the unit's registered control connection: the
// relay never routes without the unit's current word (RESOLUTION §2).
type unit struct {
	id      [32]byte
	ctl     *ctl              // the registered control connection, or nil (offline)
	gen     uint64            // control generation: +1 at every register and at control loss
	admit   map[[32]byte]bool // admission list
	endo    []byte            // the endorsement frame for phones, encoded
	slot    *[16]byte         // pairing slot, while a window allows the relay
	slotGen uint64            // +1 at every change of the slot, and with gen
	place   *splice           // the one relay place: an invitation or a splice (RESOLUTION §1)
	invited time.Time         // monotonic: when the relay last invited this unit (F2)
}

// Splice states (RESOLUTION §4, the ready transition).
const (
	invited    = iota // the invitation is sent
	joined            // the cookie is consumed; the data leg is here
	readySent         // ready written to the data leg, then the client leg
	forwarding        // the copy goroutines run (entered only under the lock)
	stopped
)

// splice is one phone's use of the unit's relay place, from its admission
// to its end.
type splice struct {
	s        *Server
	u        *unit
	phone    [32]byte
	via      string
	ctl      *ctl      // the control connection the invitation was issued on
	gen      uint64    // its control generation
	slotGen  uint64    // the slot generation, for via "pair"
	cookie   [16]byte  // one-time
	deadline time.Time // monotonic: the invitation's end
	client   *rconn
	data     *rconn // set at join, under the lock

	state  int    // guarded by s.mu
	reason string // guarded by s.mu; set when state becomes stopped

	joinedCh chan struct{} // closed when the cookie is consumed
	done     chan struct{} // closed when stopped
	fwdDone  chan struct{} // closed when no copy goroutine runs, and none will
}

// Reasons in logs (constants only, rule 41).
const (
	reasonReplaced   = "replaced"
	reasonRemoved    = "removed from the admission list"
	reasonWindow     = "pairing window changed"
	reasonOffline    = "unit offline"
	reasonDeclined   = "declined"
	reasonNoJoin     = "no join"
	reasonIdle       = "idle"
	reasonClosed     = "closed"
	reasonShutdown   = "shutdown"
	reasonRefused    = "unavailable"
	reasonLogin      = "login refused"
	reasonNoUnit     = "unit unknown or offline"
	reasonOtherUnit  = "client names another unit than which"
	reasonSilent     = "silent"
	reasonOversized  = "frame too long"
	reasonMalformed  = "malformed frame"
	reasonOutOfOrder = "frame out of order"
	reasonNotRising  = "admit n not increasing"
	reasonFrameRate  = "frame rate"
	reasonEndorse    = "endorsement refused"
	reasonNoBackup   = "no backup key"
	reasonQueue      = "write queue full"
	reasonWrite      = "write failed"
)

// The writer side's reasons, addressable for ctl.ended.
var endQueue, endWrite = reasonQueue, reasonWrite

// stopLocked stops sp; s.mu is held. A stop is never only a flag: it
// returns both legs' sockets, which the caller closes at once after the
// lock (finish). An invitation has no data leg yet, and its phone's
// goroutine answers it unavailable.
func (sp *splice) stopLocked(reason string) []net.Conn {
	if sp.state == stopped {
		return nil
	}
	prev := sp.state
	sp.state, sp.reason = stopped, reason
	if sp.s.cookies[sp.cookie] == sp {
		delete(sp.s.cookies, sp.cookie)
	}
	if sp.u.place == sp {
		sp.u.place = nil
	}
	close(sp.done)
	if prev != forwarding {
		close(sp.fwdDone) // no copy goroutine started, and none will
	}
	if prev == invited {
		return nil
	}
	conns := []net.Conn{sp.client.raw}
	if sp.data != nil {
		conns = append(conns, sp.data.raw)
	}
	return conns
}

// stopping is a stopped splice whose sockets are still to close.
type stopping struct {
	sp    *splice
	conns []net.Conn
}

// finish closes the sockets of stopped splices and, with wait, returns
// only when none of them forwards any more. The wait is bounded: closing
// the sockets unblocks every read and write in progress.
func finish(st []stopping, wait bool) {
	for _, x := range st {
		for _, c := range x.conns {
			c.Close()
		}
	}
	if wait {
		for _, x := range st {
			<-x.sp.fwdDone
		}
	}
}

// stop stops sp and closes its sockets.
func (sp *splice) stop(reason string) {
	sp.s.mu.Lock()
	conns := sp.stopLocked(reason)
	sp.s.mu.Unlock()
	finish([]stopping{{sp, conns}}, false)
}

// stopIfInvited stops sp only while no data connection has joined.
func (sp *splice) stopIfInvited(reason string) {
	sp.s.mu.Lock()
	if sp.state == invited {
		sp.stopLocked(reason)
	}
	sp.s.mu.Unlock()
}

func (sp *splice) stateNow() (int, string) {
	sp.s.mu.Lock()
	defer sp.s.mu.Unlock()
	return sp.state, sp.reason
}

func toSet(l [][32]byte) map[[32]byte]bool {
	m := make(map[[32]byte]bool, len(l))
	for _, v := range l {
		m[v] = true
	}
	return m
}

// register makes k its unit's control connection at the next generation
// (RESOLUTION §2). In one critical section: the generation, the list, the
// endorsement, no slot, and the end of every pending invitation, of a
// candidate's splice, and of a listed splice whose phone is not on the new
// list. Then their sockets close, they stop forwarding, the earlier
// control closes, and only then is `registered` sent.
func (s *Server) register(k *ctl, admit [][32]byte, endo []byte) {
	s.at("register", k)
	s.mu.Lock()
	u := k.u
	old := u.ctl
	u.gen++
	k.gen = u.gen
	u.ctl, u.admit, u.endo = k, toSet(admit), endo
	u.slot = nil
	u.slotGen++
	var st []stopping
	if sp := u.place; sp != nil && (sp.state == invited || sp.via == wire.ViaPair || !u.admit[sp.phone]) {
		st = append(st, stopping{sp, sp.stopLocked(reasonReplaced)})
	}
	s.mu.Unlock()
	s.at("register-committed", k)
	finish(st, true)
	if old != nil {
		old.replaced.Store(true)
		old.close()
	}
	s.at("register-effects-done", k)
	// Routing starts only now (Q22, re-review F1): `registered` is queued
	// first, so no `invite` can be queued ahead of it on k.
	s.mu.Lock()
	k.send(&wire.Registered{})
	if u.ctl == k {
		k.routable = true
	}
	s.mu.Unlock()
}

// setAdmit replaces the admission list (RESOLUTION §4): under the lock,
// only for the current control; a dropped phone's invitation ends and its
// splice stops; then both legs close and forwarding ends; and only then,
// if k is still current, is `admitted` queued, in the same critical section
// as that check (re-review F6), so an update of a control replaced before
// it is neither applied nor acknowledged. It reports whether it was.
func (s *Server) setAdmit(k *ctl, admit [][32]byte, n uint32) bool {
	s.mu.Lock()
	u := k.u
	if u.ctl != k {
		s.mu.Unlock()
		return false
	}
	u.admit = toSet(admit)
	var st []stopping
	if sp := u.place; sp != nil && sp.via == wire.ViaAdmit && !u.admit[sp.phone] {
		st = append(st, stopping{sp, sp.stopLocked(reasonRemoved)})
	}
	s.mu.Unlock()
	s.at("admit-committed", k)
	finish(st, true)
	s.mu.Lock()
	defer s.mu.Unlock()
	if u.ctl != k {
		return false
	}
	k.send(&wire.Admitted{N: n})
	return true
}

// setSlot sets or clears the pairing slot. A change ends a pending `pair`
// invitation; a running candidate splice is gatewayd's to end (Q27).
func (s *Server) setSlot(k *ctl, slot *[16]byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	u := k.u
	if u.ctl != k {
		return
	}
	if u.slot == nil && slot == nil || u.slot != nil && slot != nil && *u.slot == *slot {
		return
	}
	if slot == nil {
		u.slot = nil
	} else {
		v := *slot
		u.slot = &v
	}
	u.slotGen++
	if sp := u.place; sp != nil && sp.via == wire.ViaPair && sp.state == invited {
		sp.stopLocked(reasonWindow)
	}
}

// decline counts only for a pending invitation issued on this control
// generation; anything else is a normal race and is ignored.
func (s *Server) decline(k *ctl, cookie [16]byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	sp := s.cookies[cookie]
	if sp != nil && sp.u == k.u && k.u.ctl == k && sp.gen == k.gen && sp.state == invited {
		sp.stopLocked(reasonDeclined)
	}
}

// ctlGone: the unit's registered control connection ended without a
// replacement. Under the lock the unit goes offline and forgets what it
// said; its invitation ends and its splice stops, with both legs closed at
// once.
func (s *Server) ctlGone(k *ctl) {
	s.mu.Lock()
	u := k.u
	if u.ctl != k {
		s.mu.Unlock()
		return
	}
	u.ctl, u.admit, u.endo, u.slot = nil, nil, nil, nil
	u.gen++
	u.slotGen++
	var st []stopping
	if sp := u.place; sp != nil {
		st = append(st, stopping{sp, sp.stopLocked(reasonOffline)})
	}
	s.mu.Unlock()
	finish(st, false)
}

// which returns the unit a lookup names and its endorsement frame, while
// the unit is registered.
func (s *Server) which(lookup [32]byte) (*unit, []byte) {
	u := s.lookups[lookup]
	if u == nil {
		return nil, nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if u.ctl == nil {
		return nil, nil
	}
	return u, u.endo
}

// admitSplice decides a phone's connect (RESOLUTION §1, §9). The unit has
// one relay place. A phone that holds it replaces itself, as a listed
// phone or as a candidate; another phone is refused while it is taken.
// It returns nil when the phone is refused, and the splice it replaces.
func (s *Server) admitSplice(deviceID, phone [32]byte, slot *[16]byte, c *rconn) (*splice, []stopping) {
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
	// Routable only once `registered` is queued on the control (F1).
	if u.ctl == nil || !u.ctl.routable {
		return nil, nil
	}
	via := wire.ViaAdmit
	if slot != nil {
		if u.slot == nil || subtle.ConstantTimeCompare(slot[:], u.slot[:]) != 1 {
			return nil, nil
		}
		via = wire.ViaPair
	} else if !u.admit[phone] {
		return nil, nil
	}
	if p := u.place; p != nil && p.phone != phone {
		return nil, nil
	}
	// At most one invitation per unit per s.t.invite (re-review F2, the
	// owner's decision: one a second). Below the control's frame limit, so
	// neither the invitations nor the unit's answers to them can close it.
	now := s.now()
	if s.t.invite > 0 && !u.invited.IsZero() && now.Sub(u.invited) < s.t.invite {
		return nil, nil
	}
	for _, ok := s.cookies[cookie]; ok; _, ok = s.cookies[cookie] {
		if _, err := rand.Read(cookie[:]); err != nil {
			return nil, nil
		}
	}
	var st []stopping
	if p := u.place; p != nil {
		st = append(st, stopping{p, p.stopLocked(reasonReplaced)})
	}
	sp := &splice{
		s: s, u: u, phone: phone, via: via, ctl: u.ctl, gen: u.gen, slotGen: u.slotGen,
		cookie: cookie, deadline: now.Add(s.t.join), client: c, state: invited,
		joinedCh: make(chan struct{}), done: make(chan struct{}), fwdDone: make(chan struct{}),
	}
	s.cookies[cookie] = sp
	u.place = sp
	u.invited = now
	return sp, st
}

// join decides a data connection in one critical section (RESOLUTION §3):
// the cookie exists and is unused, the monotonic now is before its
// deadline, its control generation is current, for `pair` its slot
// generation is current, and its phone still waits. The cookie is
// consumed here, whatever the answer.
func (s *Server) join(cookie [16]byte, d *rconn) *splice {
	s.at("join", nil)
	s.mu.Lock()
	defer s.mu.Unlock()
	sp := s.cookies[cookie]
	if sp == nil {
		return nil
	}
	delete(s.cookies, cookie)
	u := sp.u
	ok := sp.state == invited && s.now().Before(sp.deadline) && u.ctl != nil && sp.gen == u.gen &&
		(sp.via != wire.ViaPair || sp.slotGen == u.slotGen)
	if !ok {
		if sp.state == invited {
			sp.stopLocked(reasonNoJoin)
		}
		return nil
	}
	sp.state = joined
	sp.data = d
	close(sp.joinedCh)
	return sp
}
