// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package fake

import (
	"bufio"
	"bytes"
	"context"
	"crypto/ecdsa"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/tls"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// Unit stands in for gatewayd and its helper, as far as the relay sees
// them: relay.v1 (pins A and B, configuration id, sequence, the phones
// holding remote), set-up with the signed backup, one-way promotion after
// key_proof, unit login only for pinned keys, the control connection, and
// a tiny inner "gateway" on each data connection that applies the pins and
// the remote check of ADR-019 and its amendment 3.
//
// With Dir set it also keeps paired.v1 and relay.v1 on disk by the
// two-file rule of RESOLUTION §5, and Load reads them back as gatewayd
// does at start.
type Unit struct {
	Key  *ecdsa.PrivateKey
	SPKI []byte
	ID   [32]byte // device_id
	Name string
	Port uint16
	Dial Dialer

	// Dir, if set, holds paired.v1 and relay.v1.
	Dir string
	// CrashAfterRelayWrite stops a state change after its relay.v1 write,
	// as a crash would (tests of the two-file rule).
	CrashAfterRelayWrite bool
	// CrashAfterPairedWrite stops a change of paired.v1 after that write,
	// before relay.v1 seals it.
	CrashAfterPairedWrite bool
	// Notice is what Settings would say after Load dropped grants.
	Notice string

	mu        sync.Mutex
	A, B      *[32]byte
	ConfigID  uint64
	Seq       uint64
	Paired    map[[32]byte]bool
	Remote    map[[32]byte]bool // the remote grants
	pairings  map[[32]byte]int  // paired.v1's count of pairings per key
	lostGrant bool              // the last approval took a grant away
	Decline   bool              // decline every invitation
	Manual    bool              // record invitations only; the test joins
	Promoted  int               // promotions done (at most one per configuration)
	token     []byte
	approveRm bool
	n         uint32
	ctl       *Conn
	ctlGen    uint64 // +1 per registered control connection
	ackGen    uint64 // the generation the last confirmed snapshot was on
	acked     map[[32]byte]bool
	ctlWmu    sync.Mutex
	admitted  chan ack
	sessions  map[[32]byte]net.Conn
	invites   chan wire.Invite
	ctlDone   chan struct{}
}

type ack struct {
	gen uint64
	n   uint32
}

// NewUnit makes a unit with a fresh device key.
func NewUnit(name string, port uint16, dial Dialer) *Unit {
	k := NewKey()
	spki := wire.SPKI(&k.PublicKey)
	return &Unit{
		Key: k, SPKI: spki, ID: wire.Fingerprint(spki), Name: name, Port: port, Dial: dial,
		Paired: map[[32]byte]bool{}, Remote: map[[32]byte]bool{}, pairings: map[[32]byte]int{},
		admitted: make(chan ack, 8), sessions: map[[32]byte]net.Conn{},
		invites: make(chan wire.Invite, 16),
	}
}

// ErrBackup is a backup the unit does not accept.
var ErrBackup = errors.New("fake: backup refused")

// ErrProof is a key_proof the unit does not accept.
var ErrProof = errors.New("fake: key proof refused")

// ErrCrash is a state change stopped by CrashAfterRelayWrite or
// CrashAfterPairedWrite.
var ErrCrash = errors.New("fake: crashed during a two-file change")

// Probe connects and reports the relay key presented (set-up step 2).
func (u *Unit) Probe(ctx context.Context) ([32]byte, error) {
	c, err := DialRelay(ctx, u.Dial, u.Name, nil)
	if err != nil {
		return [32]byte{}, err
	}
	defer c.Close()
	return c.Presented, nil
}

// Setup is set-up steps 3 to 6 once a person confirmed key confirmed: over
// a fresh connection pinned to it, the unit logs in and takes the backup
// pin, accepting it only with a valid signature by the confirmed key over
// its own nonce. A new configuration id is drawn and seq set to 1.
func (u *Unit) Setup(ctx context.Context, confirmed [32]byte) error {
	c, err := DialRelay(ctx, u.Dial, u.Name, func(h [32]byte) bool { return h == confirmed })
	if err != nil {
		return err
	}
	defer c.Close()
	if err := c.Start(u.Name, u.Port); err != nil {
		return err
	}
	if err := u.signLogin(c); err != nil { // c.Presented == confirmed
		return err
	}
	var nonce [32]byte
	rand.Read(nonce[:])
	if err := c.Send(&wire.GetBackup{Nonce: nonce}); err != nil {
		return err
	}
	m, err := c.Recv()
	if err != nil {
		return err
	}
	b, ok := m.(*wire.Backup)
	if !ok || wire.Fingerprint(b.SPKIA) != confirmed ||
		!wire.VerifySPKI(b.SPKIA, wire.BackupMsg(b.SPKIHashB, nonce), b.Sig) || b.SPKIHashB == confirmed {
		return ErrBackup
	}
	var id [8]byte
	rand.Read(id[:])
	u.mu.Lock()
	a, bb := confirmed, b.SPKIHashB
	u.A, u.B = &a, &bb
	u.ConfigID = binary.BigEndian.Uint64(id[:])
	u.Seq = 1
	u.Promoted = 0
	u.mu.Unlock()
	return nil
}

func (u *Unit) isPin(h [32]byte) bool {
	u.mu.Lock()
	defer u.mu.Unlock()
	return u.A != nil && *u.A == h || u.B != nil && *u.B == h
}

// signLogin signs a unit login for the key c presented. Callers make sure
// it is a pin, or, during set-up, the key the person confirmed (rule 31).
func (u *Unit) signLogin(c *Conn) error {
	sig, err := wire.Sign(u.Key, wire.UnitLoginMsg(c.Nonce, c.Presented))
	if err != nil {
		return err
	}
	return c.Send(&wire.UnitLogin{SPKI: u.SPKI, Sig: sig})
}

// Endorse returns the endorsement of the active pin at the current sequence.
func (u *Unit) Endorse() wire.SignedEndorsement {
	u.mu.Lock()
	e := wire.Endorsement{ConfigID: u.ConfigID, Seq: u.Seq, Name: u.Name, Port: u.Port, KeyHash: *u.A}
	u.mu.Unlock()
	b, err := e.Marshal()
	if err != nil {
		panic(err)
	}
	sig, err := wire.Sign(u.Key, b)
	if err != nil {
		panic(err)
	}
	return wire.SignedEndorsement{E: b, Sig: sig}
}

// remoteList is the admission snapshot: paired keys with a grant.
func (u *Unit) remoteList() [][32]byte {
	var l [][32]byte
	for fp := range u.Remote {
		if u.Paired[fp] {
			l = append(l, fp)
		}
	}
	return l
}

// Connect opens the control connection: the relay must present A or B.
// If it presents B, the unit asks for key_proof, and promotes B only when
// the proof verifies (once per configuration), then endorses it with
// seq+1. Then it logs in and registers, and treats the relay as available
// only after `registered`. Its first act then is the full admission
// snapshot (RESOLUTION §5); whether the relay confirms it is recorded.
func (u *Unit) Connect(ctx context.Context) error {
	c, err := DialRelay(ctx, u.Dial, u.Name, u.isPin)
	if err != nil {
		return err
	}
	if err := c.Start(u.Name, u.Port); err != nil {
		c.Close()
		return err
	}
	if err := u.loginPinned(c); err != nil {
		c.Close()
		return err
	}
	u.mu.Lock()
	isB := u.B != nil && *u.B == c.Presented
	u.mu.Unlock()
	if isB {
		if err := u.promote(c); err != nil {
			c.Close()
			return err
		}
	}
	u.mu.Lock()
	admit := u.remoteList()
	u.mu.Unlock()
	if err := c.Send(&wire.Register{Admit: admit, Endorsement: u.Endorse()}); err != nil {
		c.Close()
		return err
	}
	for {
		m, err := c.Recv()
		if err != nil {
			c.Close()
			return err
		}
		if _, ok := m.(*wire.Registered); ok {
			break
		}
		if p, ok := m.(*wire.Ping); ok {
			c.Send(&wire.Pong{N: p.N})
			continue
		}
		c.Close()
		return ErrUnexpected
	}
	done := make(chan struct{})
	u.mu.Lock()
	u.ctlGen++
	gen := u.ctlGen
	u.ctl, u.ctlDone = c, done
	u.mu.Unlock()
	go u.control(c, gen, done)
	u.SendAdmit()
	return nil
}

func (u *Unit) loginPinned(c *Conn) error {
	if !u.isPin(c.Presented) {
		return ErrPin
	}
	return u.signLogin(c)
}

func (u *Unit) promote(c *Conn) error {
	var nonce [32]byte
	rand.Read(nonce[:])
	if err := c.Send(&wire.Prove{Nonce: nonce}); err != nil {
		return err
	}
	m, err := c.Recv()
	if err != nil {
		return err
	}
	p, ok := m.(*wire.KeyProof)
	u.mu.Lock()
	defer u.mu.Unlock()
	if !ok || u.B == nil || wire.Fingerprint(p.SPKI) != *u.B || u.Promoted > 0 ||
		!wire.VerifySPKI(p.SPKI, wire.KeyProofMsg(nonce), p.Sig) {
		return ErrProof
	}
	// One durable write: A := B, B := none, seq+1.
	b := *u.B
	u.A, u.B = &b, nil
	u.Seq++
	u.Promoted++
	return nil
}

func (u *Unit) send(m wire.Msg) error {
	u.mu.Lock()
	c := u.ctl
	u.mu.Unlock()
	if c == nil {
		return errors.New("fake: no control connection")
	}
	u.ctlWmu.Lock()
	defer u.ctlWmu.Unlock()
	return c.Send(m)
}

func (u *Unit) control(c *Conn, gen uint64, done chan struct{}) {
	defer close(done)
	defer c.Close()
	for {
		c.TLS.SetReadDeadline(time.Now().Add(2 * time.Minute))
		m, err := c.Recv()
		if err != nil {
			return
		}
		switch m := m.(type) {
		case *wire.Ping:
			u.ctlWmu.Lock()
			c.Send(&wire.Pong{N: m.N})
			u.ctlWmu.Unlock()
		case *wire.Pong:
		case *wire.Admitted:
			select {
			case u.admitted <- ack{gen, m.N}:
			default:
			}
		case *wire.Invite:
			select {
			case u.invites <- *m:
			default:
			}
			go u.handleInvite(*m)
		default:
			return
		}
	}
}

// Invites delivers the invitations the unit received.
func (u *Unit) Invites() <-chan wire.Invite { return u.invites }

// Disconnect closes the control connection and waits for it to end.
func (u *Unit) Disconnect() {
	u.mu.Lock()
	c, done := u.ctl, u.ctlDone
	u.ctl = nil
	u.mu.Unlock()
	if c != nil {
		c.Close()
		<-done
	}
}

// ControlDone is closed when the current control connection ends.
func (u *Unit) ControlDone() <-chan struct{} {
	u.mu.Lock()
	defer u.mu.Unlock()
	return u.ctlDone
}

// SendAdmit sends the full admission snapshot with the next n, and waits
// at most 3 s for `admitted` with that n on the same control generation;
// any other acknowledgement is ignored (RESOLUTION §5 step 3).
func (u *Unit) SendAdmit() (bool, error) {
	u.mu.Lock()
	u.n++
	n, gen, done := u.n, u.ctlGen, u.ctlDone
	l := u.remoteList()
	u.mu.Unlock()
	if err := u.send(&wire.Admit{Admit: l, N: n}); err != nil {
		return false, err
	}
	t := time.NewTimer(3 * time.Second)
	defer t.Stop()
	for {
		select {
		case a := <-u.admitted:
			if a.gen == gen && a.n == n {
				u.mu.Lock()
				if u.ctlGen == gen {
					u.ackGen, u.acked = gen, toSet(l)
				}
				u.mu.Unlock()
				return true, nil
			}
		case <-done:
			return false, nil
		case <-t.C:
			return false, nil
		}
	}
}

func toSet(l [][32]byte) map[[32]byte]bool {
	m := make(map[[32]byte]bool, len(l))
	for _, v := range l {
		m[v] = true
	}
	return m
}

// RelayState is what gateway.hello reports for a phone (RESOLUTION §5):
// "off" without a grant; "admitted" if the current relay confirmed it on
// the current control generation; else "pending".
func (u *Unit) RelayState(phone [32]byte) string {
	u.mu.Lock()
	defer u.mu.Unlock()
	return u.relayStateLocked(phone)
}

func (u *Unit) relayStateLocked(phone [32]byte) string {
	switch {
	case !u.Paired[phone] || !u.Remote[phone]:
		return "off"
	case u.ctl != nil && u.ackGen == u.ctlGen && u.acked[phone]:
		return "admitted"
	}
	return "pending"
}

// The two-file rule (RESOLUTION §5 as amended for re-review F5;
// RELAY_PROPOSAL.md §6.1). paired.v1 is the sorted list of paired keys,
// each with the count of its pairings, which stands for the name and scopes
// gatewayd rewrites when a known key pairs again. relay.v1 holds the grants
// before and after a change, the SHA-256 of paired.v1 before and after it,
// paired.v1's identity before the change and, once paired.v1 is written,
// the seal: its identity as written. Recovery fails closed: the grants
// after count only under an intact seal.

type relayFile struct {
	GrantsBefore []string `json:"grants_before"`
	GrantsAfter  []string `json:"grants_after"`
	Before       string   `json:"paired_before"`
	After        string   `json:"paired_after"`
	BeforeID     fileID   `json:"paired_before_id"`
	Seal         *fileID  `json:"paired_seal,omitempty"`
}

// fileID identifies one version of paired.v1: its content and the file
// that holds it. A rewrite, even of the same bytes, changes the change time
// (and, with gatewayd's temp-and-rename, the inode), so it does not match.
type fileID struct {
	Exists  bool   `json:"exists"`
	Digest  string `json:"digest,omitempty"`
	Dev     uint64 `json:"dev,omitempty"`
	Ino     uint64 `json:"ino,omitempty"`
	Size    int64  `json:"size,omitempty"`
	CtimeNs int64  `json:"ctime_ns,omitempty"`
	MtimeNs int64  `json:"mtime_ns,omitempty"`
}

// pairedID is paired.v1's identity now; ok is false when it cannot be
// told, and then nothing recorded matches it.
func (u *Unit) pairedID() (fileID, bool) {
	path := filepath.Join(u.Dir, "paired.v1")
	b, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return fileID{}, true
	}
	if err != nil {
		return fileID{}, false
	}
	fi, err := os.Stat(path)
	if err != nil {
		return fileID{}, false
	}
	id, ok := statID(fi)
	if !ok {
		return fileID{}, false
	}
	id.Exists, id.Digest = true, digest(b)
	return id, true
}

type pairedEntry struct {
	Key      string `json:"key"`
	Pairings int    `json:"pairings"`
}

func hexList(set map[[32]byte]bool) []string {
	l := []string{}
	for k := range set {
		l = append(l, hex.EncodeToString(k[:]))
	}
	sort.Strings(l)
	return l
}

func pairedBytes(set map[[32]byte]bool, pairings map[[32]byte]int) []byte {
	l := []pairedEntry{}
	for _, h := range hexList(set) {
		k, _ := wire.ParseHex32(h)
		l = append(l, pairedEntry{Key: h, Pairings: pairings[k]})
	}
	b, _ := json.Marshal(map[string][]pairedEntry{"paired": l})
	return append(b, '\n')
}

// PairedFile is paired.v1's content (tests).
func PairedFile(b []byte) ([]pairedEntry, error) {
	var f struct{ Paired []pairedEntry }
	err := json.Unmarshal(b, &f)
	return f.Paired, err
}

func digest(b []byte) string {
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}

// writeSync is temp, flush, rename, flush of the directory.
func writeSync(dir, name string, data []byte) error {
	f, err := os.CreateTemp(dir, "."+name+".*")
	if err != nil {
		return err
	}
	tmp := f.Name()
	defer os.Remove(tmp)
	if _, err := f.Write(data); err != nil {
		f.Close()
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmp, filepath.Join(dir, name)); err != nil {
		return err
	}
	d, err := os.Open(dir)
	if err != nil {
		return err
	}
	defer d.Close()
	return d.Sync()
}

func (u *Unit) readPaired() []byte {
	b, err := os.ReadFile(filepath.Join(u.Dir, "paired.v1"))
	if err != nil {
		return pairedBytes(nil, nil)
	}
	return b
}

// changeLocked moves to paired, grants and pairings. A change of grants
// alone is one relay.v1 write, sealed with paired.v1's identity as it is. A
// change of paired.v1 is three writes: relay.v1 unsealed (grants before and
// after, digests, paired.v1's identity before), paired.v1, then relay.v1
// sealed with paired.v1's new identity. A grant counts only for a paired
// key. u.mu is held.
func (u *Unit) changeLocked(paired, grants map[[32]byte]bool, pairings map[[32]byte]int) error {
	for k := range grants {
		if !paired[k] {
			delete(grants, k)
		}
	}
	if u.Dir != "" {
		before := u.readPaired()
		after := pairedBytes(paired, pairings)
		was := clone(u.Remote)
		for k := range was {
			if !u.Paired[k] {
				delete(was, k)
			}
		}
		bid, ok := u.pairedID()
		if !ok {
			return errors.New("fake: paired.v1 identity")
		}
		rf := relayFile{GrantsBefore: hexList(was), GrantsAfter: hexList(grants),
			Before: digest(before), After: digest(after), BeforeID: bid}
		changed := !bytes.Equal(before, after)
		if !changed {
			rf.Seal = &bid
		}
		b, _ := json.Marshal(rf)
		if err := writeSync(u.Dir, "relay.v1", b); err != nil {
			return err
		}
		if u.CrashAfterRelayWrite {
			return ErrCrash
		}
		if changed {
			if err := writeSync(u.Dir, "paired.v1", after); err != nil {
				return err
			}
			if u.CrashAfterPairedWrite {
				return ErrCrash
			}
			aid, ok := u.pairedID()
			if !ok {
				return errors.New("fake: paired.v1 identity")
			}
			rf.Seal = &aid
			b, _ := json.Marshal(rf)
			if err := writeSync(u.Dir, "relay.v1", b); err != nil {
				return err
			}
		}
	}
	u.Paired, u.Remote, u.pairings = paired, grants, pairings
	return nil
}

func clone(m map[[32]byte]bool) map[[32]byte]bool {
	c := make(map[[32]byte]bool, len(m))
	for k, v := range m {
		if v {
			c[k] = true
		}
	}
	return c
}

func cloneCounts(m map[[32]byte]int) map[[32]byte]int {
	c := make(map[[32]byte]int, len(m))
	for k, v := range m {
		c[k] = v
	}
	return c
}

// Load reads paired.v1 and relay.v1 as gatewayd does at start, failing
// closed (re-review F5):
//   - sealed, and paired.v1 is the file the seal names (same content,
//     device, inode, size and times): the grants after count;
//   - not sealed, and paired.v1 is still the file recorded before the
//     change: the change was interrupted before paired.v1 was written, and
//     only grants both before and after count (an interrupted pairing
//     grants nothing; an interrupted revocation takes the grant away);
//   - anything else (paired.v1 rewritten by a rollback gatewayd or an edit,
//     even byte for byte, or a crash between paired.v1 and the seal): every
//     grant is dropped and Notice says so.
//
// Grants count only for keys present in paired.v1.
func (u *Unit) Load() error {
	u.mu.Lock()
	defer u.mu.Unlock()
	pb := u.readPaired()
	entries, err := PairedFile(pb)
	if err != nil {
		return err
	}
	paired, pairings := map[[32]byte]bool{}, map[[32]byte]int{}
	for _, e := range entries {
		k, ok := wire.ParseHex32(e.Key)
		if !ok {
			return errors.New("fake: paired.v1")
		}
		paired[k], pairings[k] = true, e.Pairings
	}
	grants := map[[32]byte]bool{}
	u.Notice = ""
	if rb, err := os.ReadFile(filepath.Join(u.Dir, "relay.v1")); err == nil {
		var rf relayFile
		if err := json.Unmarshal(rb, &rf); err != nil {
			return err
		}
		now, known := u.pairedID()
		var use []string
		switch {
		case known && rf.Seal != nil && *rf.Seal == now && digest(pb) == rf.After:
			use = rf.GrantsAfter
		case known && rf.Seal == nil && rf.BeforeID == now && digest(pb) == rf.Before:
			after := map[string]bool{}
			for _, h := range rf.GrantsAfter {
				after[h] = true
			}
			for _, h := range rf.GrantsBefore {
				if after[h] {
					use = append(use, h)
				}
			}
			if len(use) != len(rf.GrantsBefore) {
				u.Notice = "Remote access was turned off for a phone whose change was interrupted"
			}
		default:
			u.Notice = "Remote access was turned off for every phone because the paired list changed"
		}
		for _, h := range use {
			if k, ok := wire.ParseHex32(h); ok && paired[k] {
				grants[k] = true
			}
		}
	}
	u.Paired, u.Remote, u.pairings = paired, grants, pairings
	return nil
}

// SetRemote gives or takes `remote` (relay.v1 only). Taking it closes the
// phone's session at the gateway itself before the snapshot (rule 33).
func (u *Unit) SetRemote(phone [32]byte, on bool) (bool, error) {
	u.mu.Lock()
	grants := clone(u.Remote)
	if on {
		grants[phone] = true
	} else {
		delete(grants, phone)
	}
	err := u.changeLocked(clone(u.Paired), grants, cloneCounts(u.pairings))
	s := u.sessions[phone]
	u.mu.Unlock()
	if err != nil {
		return false, err
	}
	if !on && s != nil {
		s.Close()
	}
	return u.SendAdmit()
}

// Revoke removes a phone: relay.v1 drops its grant, paired.v1 drops the
// key, every session of it closes, then the snapshot goes out.
func (u *Unit) Revoke(phone [32]byte) (bool, error) {
	u.mu.Lock()
	paired, grants, pairings := clone(u.Paired), clone(u.Remote), cloneCounts(u.pairings)
	delete(paired, phone)
	delete(grants, phone)
	delete(pairings, phone)
	err := u.changeLocked(paired, grants, pairings)
	s := u.sessions[phone]
	u.mu.Unlock()
	if err != nil {
		return false, err
	}
	if s != nil {
		s.Close()
	}
	return u.SendAdmit()
}

// OpenWindow opens a pairing window; with relay, the relay gets its slot.
// approveRemote stands for the person switching "Through the relay" on.
func (u *Unit) OpenWindow(relay, approveRemote bool) ([]byte, error) {
	tok := make([]byte, 16)
	rand.Read(tok)
	u.mu.Lock()
	u.token, u.approveRm = tok, approveRemote
	u.mu.Unlock()
	if relay {
		slot := wire.SlotFromToken(tok)
		if err := u.send(&wire.PairSlot{Slot: &slot}); err != nil {
			return nil, err
		}
	}
	return tok, nil
}

// CloseWindow ends the pairing window.
func (u *Unit) CloseWindow() error {
	u.mu.Lock()
	u.token = nil
	u.mu.Unlock()
	return u.send(&wire.PairSlot{})
}

// HasSession reports whether a phone has a live inner session.
func (u *Unit) HasSession(phone [32]byte) bool {
	u.mu.Lock()
	defer u.mu.Unlock()
	return u.sessions[phone] != nil
}

// ForgetRemote takes remote at the relay only, leaving the gateway session
// alone, so that a test sees what the relay does by itself.
func (u *Unit) ForgetRemote(phone [32]byte) (bool, error) {
	u.mu.Lock()
	delete(u.Remote, phone)
	u.mu.Unlock()
	return u.SendAdmit()
}

// Set changes the unit's state under its lock (tests).
func (u *Unit) Set(f func(u *Unit)) {
	u.mu.Lock()
	defer u.mu.Unlock()
	f(u)
}

// SendRaw sends any frame on the control connection (tests).
func (u *Unit) SendRaw(m wire.Msg) error { return u.send(m) }

// approve is the tap on ALLOW (RESOLUTION §5): the pairing is committed
// by the two-file rule, then the snapshot goes out, and the answer says
// whether the relay confirmed it in 3 s. A retry by the same key creates
// no second entry.
func (u *Unit) approve(phone [32]byte) (string, bool) {
	u.mu.Lock()
	remote := u.approveRm
	paired, grants, pairings := clone(u.Paired), clone(u.Remote), cloneCounts(u.pairings)
	paired[phone] = true
	pairings[phone]++ // a known key pairing again rewrites its entry too
	delete(grants, phone)
	if remote {
		grants[phone] = true
	}
	u.lostGrant = !remote && u.Remote[phone]
	err := u.changeLocked(paired, grants, pairings)
	if err == nil {
		u.token = nil
	}
	u.mu.Unlock()
	if err != nil {
		return "", false // a crash: no answer
	}
	if !remote {
		// Answered first (RELAY_PROPOSAL.md §6.1): a known key that loses
		// its grant here is dropped from the snapshot that follows, and the
		// relay then ends its splice.
		return "paired", true
	}
	confirmed, _ := u.SendAdmit()
	u.send(&wire.PairSlot{})
	if confirmed {
		return "paired remote admitted", true
	}
	return "paired remote pending", true
}

// afterApprove is what follows the answer of a pairing without remote:
// the window's slot is cleared, and a lost grant reaches the relay.
func (u *Unit) afterApprove(lostGrant bool) {
	u.send(&wire.PairSlot{})
	if lostGrant {
		u.SendAdmit()
	}
}

func (u *Unit) handleInvite(inv wire.Invite) {
	u.mu.Lock()
	decline, manual := u.Decline, u.Manual
	u.mu.Unlock()
	if manual {
		return
	}
	if decline {
		u.send(&wire.Decline{Cookie: inv.Cookie})
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	d, err := DialRelay(ctx, u.Dial, u.Name, func(h [32]byte) bool {
		u.mu.Lock()
		defer u.mu.Unlock()
		return u.A != nil && *u.A == h
	})
	if err != nil {
		return
	}
	defer d.Close()
	if d.Start(u.Name, u.Port) != nil || d.Send(&wire.Join{Cookie: inv.Cookie}) != nil {
		return
	}
	if m, err := d.Recv(); err != nil {
		return
	} else if _, ok := m.(*wire.Ready); !ok {
		return
	}
	var phone [32]byte
	cfg := &tls.Config{
		Certificates: []tls.Certificate{SelfSigned(u.Key, "doors-gateway")},
		MinVersion:   tls.VersionTLS13,
		ClientAuth:   tls.RequireAnyClientCert,
		VerifyConnection: exactlyOnePin(func(h [32]byte) bool {
			u.mu.Lock()
			defer u.mu.Unlock()
			return u.Paired[h] || u.token != nil
		}, &phone),
		SessionTicketsDisabled: true,
	}
	inner := tls.Server(d.Stream(), cfg)
	inner.SetDeadline(time.Now().Add(10 * time.Second))
	if inner.Handshake() != nil {
		return
	}
	inner.SetDeadline(time.Time{})
	u.mu.Lock()
	hello := ""
	switch {
	case u.Paired[phone] && u.Remote[phone]:
		hello = "hello paired relay=" + u.relayStateLocked(phone)
	case u.token != nil:
		// The relay candidate of a window that allows the relay: a new
		// key, or a known one pairing again.
		hello = "hello candidate"
	case u.Paired[phone]:
		// A relay session of a paired phone without remote: whatever the
		// relay admitted, the gateway closes it (rule 32).
	}
	mayPair := u.token != nil // a known key may pair again during a window
	if hello != "" {
		if old := u.sessions[phone]; old != nil {
			old.Close() // a phone's new session replaces its older one
		}
		u.sessions[phone] = inner
	}
	u.mu.Unlock()
	if hello == "" {
		inner.Close()
		return
	}
	defer func() {
		u.mu.Lock()
		if u.sessions[phone] == inner {
			delete(u.sessions, phone)
		}
		u.mu.Unlock()
		inner.Close()
	}()
	if _, err := inner.Write([]byte(hello + "\n")); err != nil {
		return
	}
	r := bufio.NewReader(inner)
	for {
		line, err := readLine(r)
		if err != nil {
			return
		}
		switch {
		case strings.HasPrefix(line, "echo "):
			if _, err := inner.Write([]byte(line[5:] + "\n")); err != nil {
				return
			}
		case mayPair && strings.HasPrefix(line, "pair "):
			tok, err := base64.RawURLEncoding.DecodeString(line[5:])
			u.mu.Lock()
			ok := err == nil && u.token != nil && subtle.ConstantTimeCompare(tok, u.token) == 1
			u.mu.Unlock()
			if !ok {
				inner.Write([]byte("refused\n"))
				return
			}
			answer, ok := u.approve(phone)
			if ok {
				inner.Write([]byte(answer + "\n"))
				if answer == "paired" {
					u.mu.Lock()
					lost := u.lostGrant
					u.mu.Unlock()
					u.afterApprove(lost)
				}
			}
			return
		default:
			return
		}
	}
}
