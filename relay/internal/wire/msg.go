// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package wire

import (
	"bytes"
	"encoding/asn1"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"math/big"
	"strconv"
	"unicode/utf8"
)

// MaxSig is the longest DER ECDSA P-256 signature.
const MaxSig = 72

// MaxAdmit is the most phones a unit may list (RELAY_PROPOSAL.md §5.2).
const MaxAdmit = 8

// Values of Invite.Via: why the relay connects this phone. A hint only
// (rule 30): gatewayd decides by the inner TLS key.
const (
	ViaAdmit = "admit" // listed in the unit's admission list
	ViaPair  = "pair"  // presented the unit's pairing slot
)

// ErrMalformed is any frame that is not exactly one of the types below.
var ErrMalformed = errors.New("wire: malformed frame")

// Msg is one decoded frame.
type Msg interface{ Type() string }

type (
	// Challenge is the relay's first frame on every connection.
	Challenge struct{ Nonce [32]byte }
	// UnitLogin opens a control connection.
	UnitLogin struct{ SPKI, Sig []byte }
	// Prove asks the relay to sign the unit's nonce with the key it presented.
	Prove struct{ Nonce [32]byte }
	// KeyProof answers Prove.
	KeyProof struct{ SPKI, Sig []byte }
	// GetBackup asks for the backup pin, signed by the active key.
	GetBackup struct{ Nonce [32]byte }
	// Backup answers GetBackup.
	Backup struct {
		SPKIA     []byte
		SPKIHashB [32]byte
		Sig       []byte
	}
	// SignedEndorsement is the endorsement bytes and the device key's signature.
	SignedEndorsement struct{ E, Sig []byte }
	// Register makes a control connection the unit's one.
	Register struct {
		Admit       [][32]byte
		Endorsement SignedEndorsement
	}
	// Registered confirms a Register: routing to the unit is available.
	Registered struct{}
	// Admit replaces the admission list; the relay confirms with Admitted.
	Admit struct {
		Admit [][32]byte
		N     uint32
	}
	// Admitted confirms an Admit.
	Admitted struct{ N uint32 }
	// PairSlot sets (Slot non-nil) or clears the unit's pairing slot.
	PairSlot struct{ Slot *[16]byte }
	// Invite tells the unit that a phone wants a session.
	Invite struct {
		Cookie [16]byte
		Client [32]byte
		Via    string
	}
	// Decline refuses an Invite.
	Decline struct{ Cookie [16]byte }
	// Ping asks for a Pong with the same N.
	Ping struct{ N uint32 }
	// Pong answers a Ping.
	Pong struct{ N uint32 }
	// Which asks for a unit's endorsement on the provisional path.
	Which struct{ Lookup [32]byte }
	// EndorsementFrame answers Which: the unit's key and its endorsement.
	EndorsementFrame struct{ SPKI, E, Sig []byte }
	// ClientLogin opens a client connection to one unit.
	ClientLogin struct {
		SPKI     []byte
		DeviceID [32]byte
		Sig      []byte
	}
	// Connect asks for a session; Slot is set by a pairing candidate.
	Connect struct{ Slot *[16]byte }
	// Ready: the splice begins with the next byte.
	Ready struct{}
	// Unavailable is the relay's one refusal; the connection then ends.
	Unavailable struct{}
	// Join opens a data connection for an Invite.
	Join struct{ Cookie [16]byte }
)

func (*Challenge) Type() string        { return "challenge" }
func (*UnitLogin) Type() string        { return "unit" }
func (*Prove) Type() string            { return "prove" }
func (*KeyProof) Type() string         { return "key_proof" }
func (*GetBackup) Type() string        { return "get_backup" }
func (*Backup) Type() string           { return "backup" }
func (*Register) Type() string         { return "register" }
func (*Registered) Type() string       { return "registered" }
func (*Admit) Type() string            { return "admit" }
func (*Admitted) Type() string         { return "admitted" }
func (*PairSlot) Type() string         { return "pair_slot" }
func (*Invite) Type() string           { return "invite" }
func (*Decline) Type() string          { return "decline" }
func (*Ping) Type() string             { return "ping" }
func (*Pong) Type() string             { return "pong" }
func (*Which) Type() string            { return "which" }
func (*EndorsementFrame) Type() string { return "endorsement" }
func (*ClientLogin) Type() string      { return "client" }
func (*Connect) Type() string          { return "connect" }
func (*Ready) Type() string            { return "ready" }
func (*Unavailable) Type() string      { return "unavailable" }
func (*Join) Type() string             { return "join" }

// members lists, per type, the required and the optional members besides
// "type". Any other member, a duplicate or a missing one is malformed.
var members = map[string]struct{ req, opt []string }{
	"challenge":   {req: []string{"nonce"}},
	"unit":        {req: []string{"spki", "sig"}},
	"prove":       {req: []string{"nonce"}},
	"key_proof":   {req: []string{"spki", "sig"}},
	"get_backup":  {req: []string{"nonce"}},
	"backup":      {req: []string{"spki_a", "spki_hash_b", "sig"}},
	"register":    {req: []string{"admit", "endorsement"}},
	"registered":  {},
	"admit":       {req: []string{"admit", "n"}},
	"admitted":    {req: []string{"n"}},
	"pair_slot":   {req: []string{"slot"}},
	"invite":      {req: []string{"cookie", "client", "via"}},
	"decline":     {req: []string{"cookie"}},
	"ping":        {req: []string{"n"}},
	"pong":        {req: []string{"n"}},
	"which":       {req: []string{"lookup"}},
	"endorsement": {req: []string{"spki", "e", "sig"}},
	"client":      {req: []string{"spki", "device_id", "sig"}},
	"connect":     {opt: []string{"slot"}},
	"ready":       {},
	"unavailable": {},
	"join":        {req: []string{"cookie"}},
}

// Encoding of binary values.
var b64 = base64.StdEncoding.Strict()

// B64 is the padded standard base64 of RFC 4648 §4.
func B64(b []byte) string { return b64.EncodeToString(b) }

// Hex is lower-case hex, used for every SHA-256 value (fingerprints,
// device_ids, key hashes, lookups).
func Hex(b []byte) string { return hex.EncodeToString(b) }

// ParseHex32 accepts exactly 64 lower-case hex digits.
func ParseHex32(s string) ([32]byte, bool) {
	var out [32]byte
	if len(s) != 64 {
		return out, false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return out, false
		}
	}
	_, err := hex.Decode(out[:], []byte(s))
	return out, err == nil
}

func hexList(l [][32]byte) []string {
	out := make([]string, len(l))
	for i, v := range l {
		out[i] = Hex(v[:])
	}
	return out
}

// Encode returns the JSON payload of m. Members come out in sorted order.
func Encode(m Msg) []byte {
	o := map[string]any{"type": m.Type()}
	switch m := m.(type) {
	case *Challenge:
		o["nonce"] = B64(m.Nonce[:])
	case *UnitLogin:
		o["spki"], o["sig"] = B64(m.SPKI), B64(m.Sig)
	case *Prove:
		o["nonce"] = B64(m.Nonce[:])
	case *KeyProof:
		o["spki"], o["sig"] = B64(m.SPKI), B64(m.Sig)
	case *GetBackup:
		o["nonce"] = B64(m.Nonce[:])
	case *Backup:
		o["spki_a"], o["spki_hash_b"], o["sig"] = B64(m.SPKIA), Hex(m.SPKIHashB[:]), B64(m.Sig)
	case *Register:
		o["admit"] = hexList(m.Admit)
		o["endorsement"] = map[string]any{"e": B64(m.Endorsement.E), "sig": B64(m.Endorsement.Sig)}
	case *Admit:
		o["admit"], o["n"] = hexList(m.Admit), m.N
	case *Admitted:
		o["n"] = m.N
	case *PairSlot:
		if m.Slot == nil {
			o["slot"] = nil
		} else {
			o["slot"] = B64(m.Slot[:])
		}
	case *Invite:
		o["cookie"], o["client"], o["via"] = B64(m.Cookie[:]), Hex(m.Client[:]), m.Via
	case *Decline:
		o["cookie"] = B64(m.Cookie[:])
	case *Ping:
		o["n"] = m.N
	case *Pong:
		o["n"] = m.N
	case *Which:
		o["lookup"] = Hex(m.Lookup[:])
	case *EndorsementFrame:
		o["spki"], o["e"], o["sig"] = B64(m.SPKI), B64(m.E), B64(m.Sig)
	case *ClientLogin:
		o["spki"], o["device_id"], o["sig"] = B64(m.SPKI), Hex(m.DeviceID[:]), B64(m.Sig)
	case *Connect:
		if m.Slot != nil {
			o["slot"] = B64(m.Slot[:])
		}
	case *Ready, *Unavailable, *Registered:
	case *Join:
		o["cookie"] = B64(m.Cookie[:])
	default:
		panic("wire: unknown message")
	}
	b, err := json.Marshal(o)
	if err != nil {
		panic("wire: " + err.Error())
	}
	return b
}

// parseObject accepts exactly one JSON object of well-formed UTF-8 with no
// duplicate member, and nothing after it.
func parseObject(b []byte) (map[string]json.RawMessage, error) {
	if !utf8.Valid(b) {
		return nil, ErrMalformed
	}
	dec := json.NewDecoder(bytes.NewReader(b))
	if t, err := dec.Token(); err != nil || t != json.Delim('{') {
		return nil, ErrMalformed
	}
	obj := make(map[string]json.RawMessage)
	for dec.More() {
		t, err := dec.Token()
		if err != nil {
			return nil, ErrMalformed
		}
		k, ok := t.(string)
		if !ok {
			return nil, ErrMalformed
		}
		if _, dup := obj[k]; dup {
			return nil, ErrMalformed
		}
		var raw json.RawMessage
		if err := dec.Decode(&raw); err != nil {
			return nil, ErrMalformed
		}
		obj[k] = raw
	}
	if t, err := dec.Token(); err != nil || t != json.Delim('}') {
		return nil, ErrMalformed
	}
	if _, err := dec.Token(); err != io.EOF {
		return nil, ErrMalformed
	}
	return obj, nil
}

func isNull(raw json.RawMessage) bool { return string(raw) == "null" }

type fields struct {
	obj map[string]json.RawMessage
	err bool
}

func (f *fields) str(k string) string {
	raw, ok := f.obj[k]
	var s string
	if !ok || isNull(raw) || json.Unmarshal(raw, &s) != nil {
		f.err = true
	}
	return s
}

func (f *fields) b64(k string, min, max int) []byte {
	s := f.str(k)
	if f.err {
		return nil
	}
	b, err := b64.DecodeString(s)
	if err != nil || len(b) < min || len(b) > max || B64(b) != s {
		f.err = true
		return nil
	}
	return b
}

func (f *fields) b64n(k string, dst []byte) {
	b := f.b64(k, len(dst), len(dst))
	copy(dst, b)
}

func (f *fields) hex32(k string) [32]byte {
	v, ok := ParseHex32(f.str(k))
	if !ok {
		f.err = true
	}
	return v
}

// u32 accepts exactly an unsigned decimal: "0", or a non-zero digit
// followed by digits, at most 4294967295. No sign, fraction or exponent.
func (f *fields) u32(k string) uint32 {
	raw, ok := f.obj[k]
	if !ok || len(raw) == 0 || len(raw) > 10 || raw[0] == '0' && len(raw) > 1 {
		f.err = true
		return 0
	}
	for _, c := range raw {
		if c < '0' || c > '9' {
			f.err = true
			return 0
		}
	}
	n, err := strconv.ParseUint(string(raw), 10, 32)
	if err != nil {
		f.err = true
	}
	return uint32(n)
}

// spki reads a key: the canonical 91-byte DER SubjectPublicKeyInfo of a
// P-256 point (ParseSPKI).
func (f *fields) spki(k string) []byte {
	b := f.b64(k, SPKILen, SPKILen)
	if !f.err {
		if _, err := ParseSPKI(b); err != nil {
			f.err = true
		}
	}
	return b
}

// sig reads a signature: one DER SEQUENCE of two positive INTEGERs, with
// nothing after it. Whether it verifies is the receiver's check.
func (f *fields) sig(k string) []byte {
	b := f.b64(k, 8, MaxSig)
	if f.err {
		return b
	}
	var v struct{ R, S *big.Int }
	rest, err := asn1.Unmarshal(b, &v)
	if err != nil || len(rest) != 0 || v.R.Sign() <= 0 || v.S.Sign() <= 0 {
		f.err = true
	}
	return b
}

func (f *fields) hexList(k string) [][32]byte {
	raw, ok := f.obj[k]
	var l []string
	if !ok || isNull(raw) || json.Unmarshal(raw, &l) != nil || len(l) > MaxAdmit {
		f.err = true
		return nil
	}
	out := make([][32]byte, 0, len(l))
	seen := make(map[[32]byte]bool, len(l))
	for _, s := range l {
		v, ok := ParseHex32(s)
		if !ok || seen[v] {
			f.err = true
			return nil
		}
		seen[v] = true
		out = append(out, v)
	}
	return out
}

// slot reads a 16-byte base64 slot; with nullable, JSON null is nil.
func (f *fields) slot(k string, nullable bool) *[16]byte {
	raw, ok := f.obj[k]
	if !ok {
		return nil
	}
	if isNull(raw) {
		if !nullable {
			f.err = true
		}
		return nil
	}
	var s [16]byte
	f.b64n(k, s[:])
	return &s
}

const (
	minE = endorsementFixed + 1
	maxE = endorsementFixed + MaxName
)

// Decode parses one frame payload strictly: a JSON object whose "type" is
// one of the types above, with exactly its members, each well-formed.
func Decode(b []byte) (Msg, error) {
	obj, err := parseObject(b)
	if err != nil {
		return nil, err
	}
	f := &fields{obj: obj}
	typ := f.str("type")
	spec, ok := members[typ]
	if f.err || !ok {
		return nil, ErrMalformed
	}
	allowed := 1
	for _, k := range spec.req {
		if _, ok := obj[k]; !ok {
			return nil, ErrMalformed
		}
		allowed++
	}
	for _, k := range spec.opt {
		if _, ok := obj[k]; ok {
			allowed++
		}
	}
	if len(obj) != allowed {
		return nil, ErrMalformed
	}
	var m Msg
	switch typ {
	case "challenge":
		x := &Challenge{}
		f.b64n("nonce", x.Nonce[:])
		m = x
	case "unit":
		m = &UnitLogin{SPKI: f.spki("spki"), Sig: f.sig("sig")}
	case "prove":
		x := &Prove{}
		f.b64n("nonce", x.Nonce[:])
		m = x
	case "key_proof":
		m = &KeyProof{SPKI: f.spki("spki"), Sig: f.sig("sig")}
	case "get_backup":
		x := &GetBackup{}
		f.b64n("nonce", x.Nonce[:])
		m = x
	case "backup":
		m = &Backup{SPKIA: f.spki("spki_a"), SPKIHashB: f.hex32("spki_hash_b"), Sig: f.sig("sig")}
	case "register":
		x := &Register{Admit: f.hexList("admit")}
		inner, err := parseObject(obj["endorsement"])
		if err != nil || len(inner) != 2 {
			return nil, ErrMalformed
		}
		g := &fields{obj: inner}
		x.Endorsement = SignedEndorsement{E: g.b64("e", minE, maxE), Sig: g.sig("sig")}
		if g.err {
			return nil, ErrMalformed
		}
		m = x
	case "admit":
		m = &Admit{Admit: f.hexList("admit"), N: f.u32("n")}
	case "admitted":
		m = &Admitted{N: f.u32("n")}
	case "pair_slot":
		m = &PairSlot{Slot: f.slot("slot", true)}
	case "invite":
		x := &Invite{Client: f.hex32("client"), Via: f.str("via")}
		f.b64n("cookie", x.Cookie[:])
		if x.Via != ViaAdmit && x.Via != ViaPair {
			f.err = true
		}
		m = x
	case "decline":
		x := &Decline{}
		f.b64n("cookie", x.Cookie[:])
		m = x
	case "ping":
		m = &Ping{N: f.u32("n")}
	case "pong":
		m = &Pong{N: f.u32("n")}
	case "which":
		m = &Which{Lookup: f.hex32("lookup")}
	case "endorsement":
		m = &EndorsementFrame{SPKI: f.spki("spki"), E: f.b64("e", minE, maxE), Sig: f.sig("sig")}
	case "client":
		m = &ClientLogin{SPKI: f.spki("spki"), DeviceID: f.hex32("device_id"), Sig: f.sig("sig")}
	case "connect":
		m = &Connect{Slot: f.slot("slot", false)}
	case "ready":
		m = &Ready{}
	case "registered":
		m = &Registered{}
	case "unavailable":
		m = &Unavailable{}
	case "join":
		x := &Join{}
		f.b64n("cookie", x.Cookie[:])
		m = x
	}
	if f.err {
		return nil, ErrMalformed
	}
	return m, nil
}
