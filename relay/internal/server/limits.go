// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"net"
	"net/netip"
	"sync"
	"time"
)

// sourceOf is the unit a per-source limit counts: an IPv4 address, or an
// IPv6 /64 (one subscriber's usual allocation).
func sourceOf(a net.Addr) netip.Prefix {
	ap, err := netip.ParseAddrPort(a.String())
	if err != nil {
		return netip.Prefix{}
	}
	ip := ap.Addr().Unmap()
	if ip.Is4() {
		return netip.PrefixFrom(ip, 32)
	}
	p, _ := ip.Prefix(64)
	return p
}

// bucket is a token bucket: rate per second, burst of the same size.
type bucket struct {
	tokens float64
	last   time.Time
}

func (b *bucket) take(now time.Time, rate float64) bool {
	if b.last.IsZero() {
		b.tokens = rate
	} else {
		b.tokens += now.Sub(b.last).Seconds() * rate
		if b.tokens > rate {
			b.tokens = rate
		}
	}
	b.last = now
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

type source struct {
	rate   bucket
	unauth int
	live   int
}

// Why a connection was refused at accept.
const (
	refusedGlobal = iota
	refusedRate
	refusedUnauth
	nRefused
)

// limiter holds the per-source and global connection limits.
type limiter struct {
	mu        sync.Mutex
	rate      float64
	unauthMax int
	maxConns  int
	live      int
	srcs      map[netip.Prefix]*source
}

func newLimiter(l Limits) *limiter {
	return &limiter{
		rate:      float64(l.PerSourceRate),
		unauthMax: l.PerSourceUnauth,
		maxConns:  l.MaxConns,
		srcs:      make(map[netip.Prefix]*source),
	}
}

// admit counts a new connection, or says why it is refused.
func (l *limiter) admit(src netip.Prefix, now time.Time) (ok bool, why int) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.live >= l.maxConns {
		return false, refusedGlobal
	}
	if len(l.srcs) > 2*l.maxConns {
		l.sweepLocked(now)
	}
	s := l.srcs[src]
	if s == nil {
		s = &source{}
		l.srcs[src] = s
	}
	if !s.rate.take(now, l.rate) {
		return false, refusedRate
	}
	if s.unauth >= l.unauthMax {
		return false, refusedUnauth
	}
	s.unauth++
	s.live++
	l.live++
	return true, 0
}

// authed moves a connection out of its source's unauthenticated count.
func (l *limiter) authed(src netip.Prefix) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if s := l.srcs[src]; s != nil && s.unauth > 0 {
		s.unauth--
	}
}

// release ends a connection's count.
func (l *limiter) release(src netip.Prefix, wasAuthed bool) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.live--
	s := l.srcs[src]
	if s == nil {
		return
	}
	if !wasAuthed && s.unauth > 0 {
		s.unauth--
	}
	s.live--
}

// sweepLocked forgets sources with no connection whose bucket is full again.
func (l *limiter) sweepLocked(now time.Time) {
	for k, s := range l.srcs {
		if s.live <= 0 && now.Sub(s.rate.last) >= time.Second {
			delete(l.srcs, k)
		}
	}
}

func (l *limiter) sweep(now time.Time) {
	l.mu.Lock()
	l.sweepLocked(now)
	l.mu.Unlock()
}
