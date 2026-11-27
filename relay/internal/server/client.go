// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"io"
	"sync"
	"sync/atomic"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/wire"
)

var ready = wire.AppendFrame(nil, wire.Encode(&wire.Ready{}))

// serveClient handles a phone: on the provisional path `which` first, then
// `client` and `connect`; then the invitation, the unit's join, and the
// splice. Every refusal is the same `unavailable`.
func (s *Server) serveClient(c *rconn, first wire.Msg) {
	m := first
	var matched *unit
	if w, ok := m.(*wire.Which); ok {
		u, endo := s.which(w.Lookup)
		if endo == nil {
			s.logEvent("refused", logFields{kind: "client", src: c.addr, reason: reasonNoUnit})
			s.refuse(c)
			return
		}
		matched = u
		if _, err := c.tc.Write(endo); err != nil {
			return
		}
		var err error
		if m, err = wire.ReadMsg(c.br); err != nil {
			s.count.early.Add(1)
			return
		}
	}
	login, ok := m.(*wire.ClientLogin)
	if !ok {
		s.count.early.Add(1)
		return
	}
	m, err := wire.ReadMsg(c.br)
	if err != nil {
		s.count.early.Add(1)
		return
	}
	req, ok := m.(*wire.Connect)
	if !ok {
		s.count.early.Add(1)
		return
	}
	s.at("connect", time.Since(c.start))
	phone := wire.Fingerprint(login.SPKI)
	if !wire.VerifySPKI(login.SPKI, wire.ClientLoginMsg(c.nonce, s.keyHash, login.DeviceID), login.Sig) {
		s.logEvent("refused", logFields{kind: "client", src: c.addr, reason: reasonLogin})
		s.refuse(c)
		return
	}
	s.markAuthed(c)
	// Q2: a client that asked `which` continues with the unit it matched.
	if matched != nil && matched.id != login.DeviceID {
		s.logEvent("refused", logFields{kind: "client", src: c.addr, phone: &phone, reason: reasonOtherUnit})
		s.refuse(c)
		return
	}
	sp, old := s.admitSplice(login.DeviceID, phone, req.Slot, c)
	finish(old, false)
	f := logFields{kind: "client", src: c.addr, phone: &phone}
	if s.units[login.DeviceID] != nil {
		f.unit = &login.DeviceID
	}
	if sp == nil {
		f.reason = reasonRefused
		s.logEvent("refused", f)
		s.refuse(c)
		return
	}
	defer sp.stop(reasonClosed)
	f.via = sp.via
	sp.ctl.send(&wire.Invite{Cookie: sp.cookie, Client: phone, Via: sp.via})
	t := time.NewTimer(s.t.join)
	select {
	case <-sp.joinedCh:
	case <-sp.done:
	case <-t.C:
		sp.stopIfInvited(reasonNoJoin)
	case <-s.stop:
		sp.stop(reasonShutdown)
	}
	t.Stop()
	if st, why := sp.stateNow(); st != joined {
		f.reason = why
		s.logEvent("refused", f)
		s.refuse(c)
		return
	}
	s.runSplice(sp, f)
}

// serveData handles the unit's data connection: one `join`, then the
// splice, which the phone's goroutine runs.
func (s *Server) serveData(c *rconn, j *wire.Join) {
	sp := s.join(j.Cookie, c)
	if sp == nil {
		s.logEvent("refused", logFields{kind: "data", src: c.addr, reason: reasonRefused})
		s.refuse(c)
		return
	}
	s.markAuthed(c)
	<-sp.done
}

// runSplice takes a joined splice through `ready` to forwarding, and
// copies bytes both ways until either side ends, the splice is stopped
// (replacement, removal, the unit going offline), or no byte moved for the
// session idle time. It never looks at the bytes.
func (s *Server) runSplice(sp *splice, f logFields) {
	c, d := sp.client, sp.data
	defer c.raw.Close()
	defer d.raw.Close()
	s.at("joined", sp)
	now := time.Now()
	for _, x := range []*rconn{d, c} {
		x.raw.SetReadDeadline(time.Time{})
		x.raw.SetWriteDeadline(now.Add(s.t.write))
		if _, err := x.tc.Write(ready); err != nil {
			sp.stop(reasonWrite)
			return
		}
		x.raw.SetWriteDeadline(time.Time{})
	}
	s.mu.Lock()
	if sp.state != joined {
		s.mu.Unlock()
		return // stopped meanwhile: its stopper closed both legs
	}
	sp.state = readySent
	s.mu.Unlock()
	s.at("ready-sent", sp)
	s.mu.Lock()
	if sp.state != readySent {
		s.mu.Unlock()
		return
	}
	sp.state = forwarding
	s.mu.Unlock()
	s.logEvent("session up", f)

	base := time.Now()
	var last, up, down atomic.Int64
	var wg sync.WaitGroup
	// Q4: each pump reads through the leg's own buffered reader, so bytes
	// already read ahead past `ready` go first, in order.
	pump := func(dst io.Writer, src io.Reader, n *atomic.Int64) {
		defer wg.Done()
		buf := make([]byte, 16<<10)
		for {
			k, err := src.Read(buf)
			if k > 0 {
				last.Store(int64(time.Since(base)))
				if _, werr := dst.Write(buf[:k]); werr != nil {
					sp.stop(reasonClosed)
					return
				}
				n.Add(int64(k))
				last.Store(int64(time.Since(base)))
			}
			if err != nil {
				sp.stop(reasonClosed)
				return
			}
		}
	}
	wg.Add(2)
	go pump(d.tc, c.br, &up)   // phone to unit
	go pump(c.tc, d.br, &down) // unit to phone

	idle := s.t.sessionIdle
	timer := time.NewTimer(idle)
wait:
	for {
		select {
		case <-sp.done:
			break wait
		case <-s.stop:
			sp.stop(reasonShutdown)
		case <-timer.C:
			quiet := time.Since(base) - time.Duration(last.Load())
			if quiet >= idle {
				sp.stop(reasonIdle)
			} else {
				timer.Reset(idle - quiet)
			}
		}
	}
	timer.Stop()
	c.raw.Close()
	d.raw.Close()
	wg.Wait()
	close(sp.fwdDone)
	_, f.reason = sp.stateNow()
	f.in, f.out, f.dur = up.Load(), down.Load(), time.Since(base)
	s.logEvent("session down", f)
}
