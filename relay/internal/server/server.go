// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

// Package server is the DOORS Relay (docs/api/relay.md): a blind pipe that
// splices a phone's TLS stream to a unit's, after both have proved their
// keys, and only for phones the unit has listed. It holds no key of a unit
// or a phone, and never sees the gateway's TLS from inside.
package server

import (
	"bufio"
	"crypto/ecdsa"
	"crypto/rand"
	"crypto/tls"
	"errors"
	"io"
	"net"
	"net/netip"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// Protocol timings (docs/api/relay.md §6). Only the session idle time is
// the operator's; tests shorten the others.
type timing struct {
	auth        time.Duration // accept to login (and, on control, register)
	join        time.Duration // invite to join: the cookie's life
	ctlIdle     time.Duration // silence that closes a control connection
	ping        time.Duration // the relay's ping interval on control
	write       time.Duration // one frame write
	sessionIdle time.Duration // a splice in which no byte moved
	linger      time.Duration // reading after a refusal, before closing
	invite      time.Duration // the shortest interval between one unit's invitations (F2)
}

func defaultTiming(l Limits) timing {
	return timing{
		auth:        10 * time.Second,
		join:        5 * time.Second,
		ctlIdle:     75 * time.Second,
		ping:        25 * time.Second,
		write:       10 * time.Second,
		sessionIdle: time.Duration(l.SessionIdleSeconds) * time.Second,
		linger:      500 * time.Millisecond,
		invite:      time.Second,
	}
}

// ctlFrameRate is the most frames per second a unit may send on its
// control connection (burst of the same size).
const ctlFrameRate = 10

// Server is one relay.
type Server struct {
	cfg     *Config
	t       timing
	key     *ecdsa.PrivateKey
	spki    []byte
	keyHash [32]byte
	cert    tls.Certificate
	backup  *[32]byte
	tlsCfg  *tls.Config
	units   map[[32]byte]*unit // enrolled; fixed at start
	lookups map[[32]byte]*unit // Lookup(keyHash, device_id); fixed at start
	lim     *limiter
	log     *logger
	lines   *lineQueue
	count   counters

	// now is the monotonic clock of invitation deadlines; hook, nil in
	// production, lets a test stop the relay at a named race point. Both
	// are set before Serve.
	now  func() time.Time
	hook func(point string, v any)

	mu      sync.Mutex // guards every unit's mutable state, splices and cookies
	cookies map[[16]byte]*splice

	cmu     sync.Mutex
	closed  bool
	lns     map[net.Listener]struct{}
	conns   map[net.Conn]struct{}
	wg      sync.WaitGroup
	stop    chan struct{}
	houseWG sync.WaitGroup
}

// at is a named race point (state.go, client.go): nothing in production.
func (s *Server) at(point string, v any) {
	if s.hook != nil {
		s.hook(point, v)
	}
}

// New loads the key (and chain) the configuration names. Logs go to logOut.
func New(cfg *Config, logOut io.Writer) (*Server, error) {
	data, err := os.ReadFile(cfg.KeyFile)
	if err != nil {
		return nil, err
	}
	key, err := ParseKeyPEM(data)
	if err != nil {
		return nil, err
	}
	var cert tls.Certificate
	if cfg.CertFile != "" {
		pemData, err := os.ReadFile(cfg.CertFile)
		if err != nil {
			return nil, err
		}
		if cert, err = ChainFor(pemData, key); err != nil {
			return nil, err
		}
	} else if cert, err = SelfSigned(key); err != nil {
		return nil, err
	}
	return newServer(cfg, key, cert, logOut, defaultTiming(cfg.Limits)), nil
}

func newServer(cfg *Config, key *ecdsa.PrivateKey, cert tls.Certificate, logOut io.Writer, t timing) *Server {
	s := &Server{
		cfg:     cfg,
		t:       t,
		key:     key,
		spki:    wire.SPKI(&key.PublicKey),
		cert:    cert,
		units:   make(map[[32]byte]*unit),
		lookups: make(map[[32]byte]*unit),
		lim:     newLimiter(cfg.Limits),
		now:     time.Now,
		cookies: make(map[[16]byte]*splice),
		lns:     make(map[net.Listener]struct{}),
		conns:   make(map[net.Conn]struct{}),
		stop:    make(chan struct{}),
	}
	s.lines = newLineQueue(logOut)
	s.log = newLogger(s.lines)
	s.keyHash = wire.Fingerprint(s.spki)
	if cfg.BackupHash != "" {
		b, _ := wire.ParseHex32(cfg.BackupHash)
		s.backup = &b
	}
	for _, h := range cfg.Units {
		id, _ := wire.ParseHex32(h)
		u := &unit{id: id}
		s.units[id] = u
		s.lookups[wire.Lookup(s.keyHash, id)] = u
	}
	s.tlsCfg = &tls.Config{
		Certificates:           []tls.Certificate{cert},
		MinVersion:             tls.VersionTLS13,
		MaxVersion:             tls.VersionTLS13,
		NextProtos:             []string{wire.ALPN},
		SessionTicketsDisabled: true,
		ClientAuth:             tls.NoClientCert,
	}
	s.logStart()
	s.houseWG.Add(1)
	go s.housekeeping()
	return s
}

// KeyHash is the SHA-256 of the SubjectPublicKeyInfo the relay presents.
func (s *Server) KeyHash() [32]byte { return s.keyHash }

func (s *Server) housekeeping() {
	defer s.houseWG.Done()
	sweep := time.NewTicker(10 * time.Second)
	summary := time.NewTicker(time.Minute)
	defer sweep.Stop()
	defer summary.Stop()
	for {
		select {
		case <-s.stop:
			return
		case now := <-sweep.C:
			s.lim.sweep(now)
		case <-summary.C:
			s.logSummary()
		}
	}
}

// ListenAndServe listens on the configured address.
func (s *Server) ListenAndServe() error {
	ln, err := net.Listen("tcp", s.cfg.Listen)
	if err != nil {
		return err
	}
	return s.Serve(ln)
}

// Serve accepts connections on ln until Close.
func (s *Server) Serve(ln net.Listener) error {
	s.cmu.Lock()
	if s.closed {
		s.cmu.Unlock()
		ln.Close()
		return net.ErrClosed
	}
	s.lns[ln] = struct{}{}
	s.cmu.Unlock()
	defer func() {
		s.cmu.Lock()
		delete(s.lns, ln)
		s.cmu.Unlock()
	}()
	var delay time.Duration
	for {
		c, err := ln.Accept()
		if err != nil {
			if s.isClosed() || errors.Is(err, net.ErrClosed) {
				return nil
			}
			s.logAcceptError(err)
			delay = min(max(2*delay, 5*time.Millisecond), time.Second)
			select {
			case <-s.stop:
				return nil
			case <-time.After(delay):
			}
			continue
		}
		delay = 0
		src := sourceOf(c.RemoteAddr())
		if ok, why := s.lim.admit(src, time.Now()); !ok {
			s.count.refused[why].Add(1)
			c.Close()
			continue
		}
		if !s.track(c) {
			s.lim.release(src, false)
			c.Close()
			continue
		}
		go s.handle(c, src)
	}
}

func (s *Server) isClosed() bool {
	s.cmu.Lock()
	defer s.cmu.Unlock()
	return s.closed
}

func (s *Server) track(c net.Conn) bool {
	s.cmu.Lock()
	defer s.cmu.Unlock()
	if s.closed {
		return false
	}
	s.conns[c] = struct{}{}
	s.wg.Add(1)
	return true
}

func (s *Server) untrack(c net.Conn) {
	s.cmu.Lock()
	delete(s.conns, c)
	s.cmu.Unlock()
	s.wg.Done()
}

// Close stops the listeners, ends every connection and waits for them.
func (s *Server) Close() error {
	s.cmu.Lock()
	if s.closed {
		s.cmu.Unlock()
		return nil
	}
	s.closed = true
	close(s.stop)
	for ln := range s.lns {
		ln.Close()
	}
	for c := range s.conns {
		c.Close()
	}
	s.cmu.Unlock()
	s.wg.Wait()
	s.houseWG.Wait()
	s.logStop()
	s.lines.close(time.Second)
	return nil
}

// rconn is one accepted connection after its HTTP start.
type rconn struct {
	raw    net.Conn
	tc     *tls.Conn
	br     *bufio.Reader
	src    netip.Prefix
	addr   net.Addr
	nonce  [32]byte
	start  time.Time
	authed bool
}

func (s *Server) markAuthed(c *rconn) {
	if !c.authed {
		c.authed = true
		s.lim.authed(c.src)
	}
}

func (s *Server) handle(raw net.Conn, src netip.Prefix) {
	c := &rconn{raw: raw, src: src, addr: raw.RemoteAddr(), start: time.Now()}
	defer s.untrack(raw)
	defer func() { s.lim.release(src, c.authed) }()
	defer raw.Close()

	raw.SetDeadline(c.start.Add(s.t.auth))
	c.tc = tls.Server(raw, s.tlsCfg)
	if err := c.tc.Handshake(); err != nil {
		s.count.tls.Add(1)
		return
	}
	c.br = bufio.NewReaderSize(c.tc, 2*wire.MaxFrame)
	head, err := wire.ReadHead(c.br)
	if err != nil {
		if errors.Is(err, wire.ErrHeadTooLong) || errors.Is(err, wire.ErrHeadMalformed) {
			s.count.notFound.Add(1)
			s.finish(c, wire.NotFound)
		} else {
			s.count.early.Add(1)
		}
		return
	}
	st := c.tc.ConnectionState()
	if c.br.Buffered() != 0 || st.NegotiatedProtocol != wire.ALPN ||
		!strings.EqualFold(st.ServerName, s.cfg.Name) ||
		!wire.CheckStart(head, s.cfg.Name, uint16(s.cfg.Port)) {
		s.count.notFound.Add(1)
		s.finish(c, wire.NotFound)
		return
	}
	if _, err := rand.Read(c.nonce[:]); err != nil {
		return
	}
	out := append([]byte(nil), wire.Switching...)
	out = wire.AppendFrame(out, wire.Encode(&wire.Challenge{Nonce: c.nonce}))
	if _, err := c.tc.Write(out); err != nil {
		s.count.early.Add(1)
		return
	}
	m, err := wire.ReadMsg(c.br)
	if err != nil {
		s.count.early.Add(1)
		return
	}
	switch m := m.(type) {
	case *wire.UnitLogin:
		s.serveControl(c, m)
	case *wire.Which, *wire.ClientLogin:
		s.serveClient(c, m)
	case *wire.Join:
		s.serveData(c, m)
	default:
		s.count.early.Add(1)
	}
}

var unavailable = wire.AppendFrame(nil, wire.Encode(&wire.Unavailable{}))

// refuse is the relay's one refusal: unavailable, then the end.
func (s *Server) refuse(c *rconn) { s.finish(c, unavailable) }

// finish writes b, closes the write side, and reads briefly so that the
// peer receives b before the connection is reset.
func (s *Server) finish(c *rconn, b []byte) {
	c.raw.SetWriteDeadline(time.Now().Add(s.t.write))
	if _, err := c.tc.Write(b); err != nil {
		return
	}
	c.tc.CloseWrite()
	c.raw.SetReadDeadline(time.Now().Add(s.t.linger))
	if c.br != nil {
		io.CopyN(io.Discard, c.br, 2*wire.MaxFrame)
	}
}
