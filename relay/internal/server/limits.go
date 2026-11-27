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
	refusedSources // the source table is full
	nRefused
)

// evictProbe is the most source entries one accept examines for removal
// when the table is full: the work per accept is bounded (RESOLUTION §6).
const evictProbe = 8

// limiter holds the per-source and global connection limits. The source
// table has a hard cap; it is never scanned whole on an accept.
type limiter struct {
	mu         sync.Mutex
	rate       float64
	unauthMax  int
	maxConns   int
	maxSources int
	live       int
	srcs       map[netip.Prefix]*source
	examined   int64 // entries examined by accepts (tests)
}

func newLimiter(l Limits) *limiter {
	return &limiter{
		rate:       float64(l.PerSourceRate),
		unauthMax:  l.PerSourceUnauth,
		maxConns:   l.MaxConns,
		maxSources: l.MaxSources,
		srcs:       make(map[netip.Prefix]*source),
	}
}

func removable(s *source, now time.Time) bool {
	return s.live <= 0 && now.Sub(s.rate.last) >= time.Second
}

// admit counts a new connection, or says why it is refused.
func (l *limiter) admit(src netip.Prefix, now time.Time) (ok bool, why int) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.live >= l.maxConns {
		return false, refusedGlobal
	}
	s := l.srcs[src]
	if s == nil {
		if len(l.srcs) >= l.maxSources {
			n := 0
			for k, e := range l.srcs { // map order: a different start each time
				if n == evictProbe {
					break
				}
				n++
				if removable(e, now) {
					delete(l.srcs, k)
				}
			}
			l.examined += int64(n)
			if len(l.srcs) >= l.maxSources {
				return false, refusedSources
			}
		}
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

// sweep forgets sources with no connection whose bucket is full again. It
// runs every 10 s, over a table of at most max_sources entries.
func (l *limiter) sweep(now time.Time) {
	l.mu.Lock()
	defer l.mu.Unlock()
	for k, s := range l.srcs {
		if removable(s, now) {
			delete(l.srcs, k)
		}
	}
}

func (l *limiter) size() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.srcs)
}
