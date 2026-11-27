// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"crypto/ecdsa"
	"errors"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// ctl is a unit's control connection.
type ctl struct {
	s    *Server
	c    *rconn
	u    *unit
	pub  *ecdsa.PublicKey
	spki []byte

	out        chan []byte // encoded frames; a nil entry closes after the ones before it
	done       chan struct{}
	writerDone chan struct{}
	once       sync.Once
	replaced   atomic.Bool
	in, sent   atomic.Int64

	registered, proved, backedUp bool // reader goroutine only
}

func (k *ctl) close() {
	k.once.Do(func() {
		close(k.done)
		k.c.raw.Close()
	})
}

// queue hands an encoded frame to the writer; a unit that does not read
// its control connection fills the queue and is disconnected.
func (k *ctl) queue(b []byte) {
	select {
	case <-k.done:
	case k.out <- b:
	default:
		k.close()
	}
}

func (k *ctl) send(m wire.Msg) { k.queue(wire.Encode(m)) }

func (k *ctl) write(b []byte) bool {
	k.c.raw.SetWriteDeadline(time.Now().Add(k.s.t.write))
	if err := wire.WriteFrame(k.c.tc, b); err != nil {
		return false
	}
	k.sent.Add(int64(4 + len(b)))
	return true
}

// writer sends queued frames, and a ping every 25 s.
func (k *ctl) writer() {
	defer close(k.writerDone)
	t := time.NewTicker(k.s.t.ping)
	defer t.Stop()
	var n uint32
	for {
		select {
		case <-k.done:
			return
		case <-t.C:
			n++
			if !k.write(wire.Encode(&wire.Ping{N: n})) {
				k.close()
				return
			}
		case b := <-k.out:
			if b == nil || !k.write(b) {
				k.close()
				return
			}
		}
	}
}

func (s *Server) serveControl(c *rconn, login *wire.UnitLogin) {
	pub, err := wire.ParseSPKI(login.SPKI)
	fp := wire.Fingerprint(login.SPKI)
	u := s.units[fp]
	if err != nil || u == nil || !wire.Verify(pub, wire.UnitLoginMsg(c.nonce, s.keyHash), login.Sig) {
		f := logFields{kind: "control", src: c.addr, reason: reasonLogin}
		if err == nil {
			f.unit = &fp
		}
		s.logEvent("refused", f)
		s.refuse(c)
		return
	}
	k := &ctl{
		s: s, c: c, u: u, pub: pub, spki: login.SPKI,
		out:        make(chan []byte, 32),
		done:       make(chan struct{}),
		writerDone: make(chan struct{}),
	}
	go k.writer()
	reason := k.readLoop()
	if reason != reasonNoBackup {
		k.close()
	}
	<-k.writerDone
	k.close()
	s.ctlGone(k)
	if k.replaced.Load() {
		reason = reasonReplaced
	}
	if k.registered {
		s.logEvent("control down", logFields{kind: "control", src: c.addr, unit: &fp, reason: reason,
			in: k.in.Load(), out: k.sent.Load(), dur: time.Since(c.start)})
	} else {
		s.logEvent("control ended before register", logFields{kind: "control", src: c.addr, unit: &fp, reason: reason})
	}
}

func readReason(err error) string {
	var ne net.Error
	switch {
	case errors.As(err, &ne) && ne.Timeout():
		return reasonSilent
	case errors.Is(err, wire.ErrFrame):
		return reasonOversized
	}
	return reasonClosed
}

// readLoop handles the unit's frames until the connection ends, and says why.
func (k *ctl) readLoop() string {
	s := k.s
	var rate bucket
	for {
		if k.registered {
			k.c.raw.SetReadDeadline(time.Now().Add(s.t.ctlIdle))
		} else {
			// Login and register within the first 10 s of the connection.
			k.c.raw.SetReadDeadline(k.c.start.Add(s.t.auth))
		}
		b, err := wire.ReadFrame(k.c.br)
		if err != nil {
			return readReason(err)
		}
		k.in.Add(int64(4 + len(b)))
		if !rate.take(time.Now(), ctlFrameRate) {
			return reasonFrameRate
		}
		m, err := wire.Decode(b)
		if err != nil {
			return reasonMalformed
		}
		switch m := m.(type) {
		case *wire.Ping:
			k.send(&wire.Pong{N: m.N})
		case *wire.Pong:
		case *wire.Prove:
			if k.registered || k.proved {
				return reasonOutOfOrder
			}
			k.proved = true
			sig, err := wire.Sign(s.key, wire.KeyProofMsg(m.Nonce))
			if err != nil {
				return reasonWrite
			}
			k.send(&wire.KeyProof{SPKI: s.spki, Sig: sig})
		case *wire.GetBackup:
			if k.registered || k.backedUp {
				return reasonOutOfOrder
			}
			k.backedUp = true
			if s.backup == nil {
				k.send(&wire.Unavailable{})
				k.queue(nil)
				return reasonNoBackup
			}
			sig, err := wire.Sign(s.key, wire.BackupMsg(*s.backup, m.Nonce))
			if err != nil {
				return reasonWrite
			}
			k.send(&wire.Backup{SPKIA: s.spki, SPKIHashB: *s.backup, Sig: sig})
		case *wire.Register:
			if k.registered {
				return reasonOutOfOrder
			}
			endo, ok := s.checkEndorsement(k, m.Endorsement)
			if !ok {
				return reasonEndorse
			}
			k.registered = true
			s.markAuthed(k.c)
			s.register(k, m.Admit, endo)
			s.logEvent("control up", logFields{kind: "control", src: k.c.addr, unit: &k.u.id})
		case *wire.Admit:
			if !k.registered {
				return reasonOutOfOrder
			}
			s.setAdmit(k, m.Admit)
			k.send(&wire.Admitted{N: m.N})
		case *wire.PairSlot:
			if !k.registered {
				return reasonOutOfOrder
			}
			s.setSlot(k, m.Slot)
		case *wire.Decline:
			if !k.registered {
				return reasonOutOfOrder
			}
			s.decline(k, m.Cookie)
		default:
			return reasonOutOfOrder
		}
	}
}

// checkEndorsement accepts only an endorsement of this relay, as configured,
// for the key it presents, signed by the unit that logged in. It returns the
// frame phones will be sent.
func (s *Server) checkEndorsement(k *ctl, se wire.SignedEndorsement) ([]byte, bool) {
	e, err := wire.ParseEndorsement(se.E)
	if err != nil || e.Name != s.cfg.Name || int(e.Port) != s.cfg.Port || e.KeyHash != s.keyHash ||
		!wire.Verify(k.pub, se.E, se.Sig) {
		return nil, false
	}
	return wire.AppendFrame(nil, wire.Encode(&wire.EndorsementFrame{SPKI: k.spki, E: se.E, Sig: se.Sig})), true
}
