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
	if w, ok := m.(*wire.Which); ok {
		endo := s.which(w.Lookup)
		if endo == nil {
			s.logEvent("refused", logFields{kind: "client", src: c.addr, reason: reasonNoUnit})
			s.refuse(c)
			return
		}
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
	phone := wire.Fingerprint(login.SPKI)
	if !wire.VerifySPKI(login.SPKI, wire.ClientLoginMsg(c.nonce, s.keyHash, login.DeviceID), login.Sig) {
		s.logEvent("refused", logFields{kind: "client", src: c.addr, reason: reasonLogin})
		s.refuse(c)
		return
	}
	s.markAuthed(c)
	sp, old := s.admitSplice(login.DeviceID, phone, req.Slot)
	for _, o := range old {
		o.end(reasonReplaced, false)
	}
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
	defer sp.end(reasonClosed, false)
	sp.client = c
	f.via = sp.via
	sp.ctl.send(&wire.Invite{Cookie: sp.cookie, Client: phone, Via: sp.via})
	t := time.NewTimer(s.t.join)
	select {
	case <-sp.joined:
	case <-sp.done:
	case <-t.C:
		sp.end(reasonNoJoin, true)
	case <-s.stop:
		sp.end(reasonShutdown, false)
	}
	t.Stop()
	if !sp.isJoined() {
		f.reason = sp.why()
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

// runSplice copies bytes both ways until either side ends, the splice is
// ended (replacement, removal, the unit going offline), or no byte moved
// for the session idle time. It never looks at the bytes.
func (s *Server) runSplice(sp *splice, f logFields) {
	c, d := sp.client, sp.data
	defer c.raw.Close()
	defer d.raw.Close()
	now := time.Now()
	for _, x := range []*rconn{d, c} {
		x.raw.SetReadDeadline(time.Time{})
		x.raw.SetWriteDeadline(now.Add(s.t.write))
		if _, err := x.tc.Write(ready); err != nil {
			sp.end(reasonWrite, false)
			return
		}
		x.raw.SetWriteDeadline(time.Time{})
	}
	s.logEvent("session up", f)

	base := time.Now()
	var last, up, down atomic.Int64
	var wg sync.WaitGroup
	pump := func(dst io.Writer, src io.Reader, n *atomic.Int64) {
		defer wg.Done()
		buf := make([]byte, 16<<10)
		for {
			k, err := src.Read(buf)
			if k > 0 {
				last.Store(int64(time.Since(base)))
				if _, werr := dst.Write(buf[:k]); werr != nil {
					sp.end(reasonClosed, false)
					return
				}
				n.Add(int64(k))
				last.Store(int64(time.Since(base)))
			}
			if err != nil {
				sp.end(reasonClosed, false)
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
			sp.end(reasonShutdown, false)
		case <-timer.C:
			quiet := time.Since(base) - time.Duration(last.Load())
			if quiet >= idle {
				sp.end(reasonIdle, false)
			} else {
				timer.Reset(idle - quiet)
			}
		}
	}
	timer.Stop()
	c.raw.Close()
	d.raw.Close()
	wg.Wait()
	f.reason, f.in, f.out, f.dur = sp.why(), up.Load(), down.Load(), time.Since(base)
	s.logEvent("session down", f)
}
