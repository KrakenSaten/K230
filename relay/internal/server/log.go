// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"io"
	"log/slog"
	"net"
	"net/netip"
	"sync/atomic"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/wire"
)

type logger = slog.Logger

// newLogger writes key=value lines (slog's text format) to w.
func newLogger(w io.Writer) *logger { return slog.New(slog.NewTextHandler(w, nil)) }

// Logging (rule 41). Every log call of this package is in this file, and
// a test holds that (log_lint_test.go). What can be logged is fixed by the
// types below: connection lifecycle, fingerprint prefixes (16 hex digits),
// addresses truncated to /24 (IPv4) or /48 (IPv6), byte counts and
// durations. Never a payload, a nonce, a cookie, a slot, a signature, an
// endorsement or a full address.

// truncated is a source address cut to /24 or /48.
func truncated(a net.Addr) string {
	if a == nil {
		return "-"
	}
	ap, err := netip.ParseAddrPort(a.String())
	if err != nil {
		return "-"
	}
	ip := ap.Addr().Unmap()
	bits := 48
	if ip.Is4() {
		bits = 24
	}
	p, err := ip.Prefix(bits)
	if err != nil {
		return "-"
	}
	return p.String()
}

// logFields are the only values a log line may carry.
type logFields struct {
	kind   string // control, client, data
	src    net.Addr
	unit   *[32]byte
	phone  *[32]byte
	via    string
	reason string // a constant of this package
	in     int64
	out    int64
	dur    time.Duration
}

func (s *Server) logEvent(event string, f logFields) {
	attrs := make([]any, 0, 18)
	if f.kind != "" {
		attrs = append(attrs, "kind", f.kind)
	}
	if f.src != nil {
		attrs = append(attrs, "src", truncated(f.src))
	}
	if f.unit != nil {
		attrs = append(attrs, "unit", wire.Prefix(*f.unit))
	}
	if f.phone != nil {
		attrs = append(attrs, "phone", wire.Prefix(*f.phone))
	}
	if f.via != "" {
		attrs = append(attrs, "via", f.via)
	}
	if f.reason != "" {
		attrs = append(attrs, "reason", f.reason)
	}
	if f.in != 0 || f.out != 0 {
		attrs = append(attrs, "in", f.in, "out", f.out)
	}
	if f.dur != 0 {
		attrs = append(attrs, "dur", f.dur.Round(time.Millisecond).String())
	}
	s.log.Info(event, attrs...)
}

func (s *Server) logStart() {
	s.log.Info("relay up",
		"name", s.cfg.Name, "port", s.cfg.Port,
		"key", wire.Short(s.keyHash), "backup", s.backup != nil,
		"chain", len(s.cert.Certificate), "units", len(s.units))
}

func (s *Server) logStop() { s.log.Info("relay down") }

// counters are what is not worth a line per connection: refusals at accept,
// failed handshakes, 404s and connections that ended before a login. They
// are logged as one summary line a minute, when not all zero.
type counters struct {
	refused  [nRefused]atomic.Int64
	tls      atomic.Int64
	notFound atomic.Int64
	early    atomic.Int64
}

func (s *Server) logSummary() {
	c := &s.count
	g, r, u := c.refused[refusedGlobal].Swap(0), c.refused[refusedRate].Swap(0), c.refused[refusedUnauth].Swap(0)
	t, n, e := c.tls.Swap(0), c.notFound.Swap(0), c.early.Swap(0)
	if g+r+u+t+n+e == 0 {
		return
	}
	s.log.Info("summary", "refused_global", g, "refused_rate", r, "refused_unauth", u,
		"tls_failed", t, "not_found", n, "ended_before_login", e)
}

// logClosed is for errors of the listener itself.
func (s *Server) logAcceptError(err error) {
	s.log.Warn("accept", "error", err.Error())
}
