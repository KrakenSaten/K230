// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package wire

import (
	"crypto/hkdf"
	"crypto/sha256"
	"encoding/binary"
	"errors"
)

// Context strings (RELAY_PROPOSAL.md §4.3). Every signed or hashed message
// starts with one of them and a 0x00 byte, so no byte string is a message
// of two kinds, and none starts like a TLS 1.3 CertificateVerify input
// (64 x 0x20) or an X.509 TBSCertificate (0x30).
const (
	CtxEndorsement = "DOORS relay endorsement v1"
	CtxUnitLogin   = "DOORS relay unit login v1"
	CtxClientLogin = "DOORS relay client login v1"
	CtxKeyProof    = "DOORS relay key proof v1"
	CtxBackup      = "DOORS relay backup v1"
	CtxLookup      = "DOORS relay lookup v1"
	CtxPairSlot    = "DOORS relay pair slot v1"
)

func prefixed(ctx string, parts ...[]byte) []byte {
	n := len(ctx) + 1
	for _, p := range parts {
		n += len(p)
	}
	b := make([]byte, 0, n)
	b = append(b, ctx...)
	b = append(b, 0)
	for _, p := range parts {
		b = append(b, p...)
	}
	return b
}

// UnitLoginMsg is what the unit's device key signs on a control connection:
// the relay's challenge nonce and the SHA-256 of the relay key it was shown.
func UnitLoginMsg(nonce, relayKeyHash [32]byte) []byte {
	return prefixed(CtxUnitLogin, nonce[:], relayKeyHash[:])
}

// ClientLoginMsg is what the phone's key signs on a client connection: the
// challenge nonce, the relay key it was shown, and the unit it asks for.
func ClientLoginMsg(nonce, relayKeyHash, deviceID [32]byte) []byte {
	return prefixed(CtxClientLogin, nonce[:], relayKeyHash[:], deviceID[:])
}

// KeyProofMsg is what the relay's presented key signs to answer `prove`.
func KeyProofMsg(nonce [32]byte) []byte {
	return prefixed(CtxKeyProof, nonce[:])
}

// BackupMsg is what the relay's active key signs to vouch for its backup.
func BackupMsg(backupHash, nonce [32]byte) []byte {
	return prefixed(CtxBackup, backupHash[:], nonce[:])
}

// Lookup is the value a phone on the provisional path sends instead of the
// unit's device_id. It is bound to the relay key the phone was shown, so a
// relay that does not hold that key cannot use it to test device_ids it
// has not been told.
func Lookup(relayKeyHash, deviceID [32]byte) [32]byte {
	return sha256.Sum256(prefixed(CtxLookup, relayKeyHash[:], deviceID[:]))
}

// SlotFromToken is the one-way pairing slot a window gives the relay:
// HKDF-SHA256 with the window's 16 raw token bytes as input keying
// material, no salt, the context string as info, 16 bytes of output.
func SlotFromToken(token []byte) [16]byte {
	k, err := hkdf.Key(sha256.New, token, nil, CtxPairSlot, 16)
	if err != nil {
		panic("wire: hkdf: " + err.Error())
	}
	var s [16]byte
	copy(s[:], k)
	return s
}

// Endorsement is the content of a unit's statement that a relay key is its
// relay (RELAY_PROPOSAL.md §4.2).
type Endorsement struct {
	ConfigID uint64
	Seq      uint64
	Name     string
	Port     uint16
	KeyHash  [32]byte
}

// MaxName is the longest relay name a unit accepts (rule 28).
const MaxName = 64

// ValidName reports whether s is a relay name as the protocol carries it:
// 1 to 64 characters of lower-case a-z, digits, '-' and '.'.
func ValidName(s string) bool {
	if len(s) == 0 || len(s) > MaxName {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-' || c == '.') {
			return false
		}
	}
	return true
}

var errEndorsement = errors.New("wire: malformed endorsement")

// endorsementFixed is the length of everything but the name.
const endorsementFixed = len(CtxEndorsement) + 1 + 8 + 8 + 1 + 2 + 32

// Marshal returns the signed bytes:
//
//	"DOORS relay endorsement v1" 0x00 || u64 config_id || u64 seq ||
//	u8 len || name || u16 port || 32-byte SHA-256(relay SPKI)
//
// with big-endian integers.
func (e Endorsement) Marshal() ([]byte, error) {
	if !ValidName(e.Name) || e.Port == 0 {
		return nil, errEndorsement
	}
	b := make([]byte, 0, endorsementFixed+len(e.Name))
	b = append(b, CtxEndorsement...)
	b = append(b, 0)
	b = binary.BigEndian.AppendUint64(b, e.ConfigID)
	b = binary.BigEndian.AppendUint64(b, e.Seq)
	b = append(b, byte(len(e.Name)))
	b = append(b, e.Name...)
	b = binary.BigEndian.AppendUint16(b, e.Port)
	b = append(b, e.KeyHash[:]...)
	return b, nil
}

// ParseEndorsement is the exact inverse of Marshal; anything else is refused.
func ParseEndorsement(b []byte) (Endorsement, error) {
	var e Endorsement
	head := len(CtxEndorsement) + 1
	if len(b) < endorsementFixed+1 || len(b) > endorsementFixed+MaxName {
		return e, errEndorsement
	}
	if string(b[:head-1]) != CtxEndorsement || b[head-1] != 0 {
		return e, errEndorsement
	}
	p := b[head:]
	e.ConfigID = binary.BigEndian.Uint64(p[0:8])
	e.Seq = binary.BigEndian.Uint64(p[8:16])
	n := int(p[16])
	p = p[17:]
	if len(p) != n+2+32 {
		return e, errEndorsement
	}
	e.Name = string(p[:n])
	e.Port = binary.BigEndian.Uint16(p[n : n+2])
	copy(e.KeyHash[:], p[n+2:])
	if !ValidName(e.Name) || e.Port == 0 {
		return Endorsement{}, errEndorsement
	}
	return e, nil
}
