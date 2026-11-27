// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

// Package wire is the DOORS Relay protocol v1 (docs/api/relay.md): the
// HTTP start, the frames, and the exact bytes every signature covers.
// It is shared by the relay server, the lying relay and the test clients,
// so that all of them agree on one encoding.
package wire

import (
	"bytes"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/hex"
	"errors"
	"strings"
)

// SPKILen is the length of the DER SubjectPublicKeyInfo of a P-256 key with
// an uncompressed point, the only key form the protocol accepts.
const SPKILen = 91

var errKey = errors.New("wire: not a P-256 SubjectPublicKeyInfo")

// ParseSPKI accepts exactly the canonical DER SubjectPublicKeyInfo of an
// ECDSA P-256 key (uncompressed point). Any other encoding of the same key
// would have another fingerprint, so it is refused rather than normalised.
func ParseSPKI(spki []byte) (*ecdsa.PublicKey, error) {
	if len(spki) != SPKILen {
		return nil, errKey
	}
	pub, err := x509.ParsePKIXPublicKey(spki)
	if err != nil {
		return nil, errKey
	}
	ec, ok := pub.(*ecdsa.PublicKey)
	if !ok || ec.Curve != elliptic.P256() {
		return nil, errKey
	}
	again, err := x509.MarshalPKIXPublicKey(ec)
	if err != nil || !bytes.Equal(again, spki) {
		return nil, errKey
	}
	return ec, nil
}

// SPKI returns the DER SubjectPublicKeyInfo of a P-256 public key.
func SPKI(pub *ecdsa.PublicKey) []byte {
	b, err := x509.MarshalPKIXPublicKey(pub)
	if err != nil {
		panic("wire: cannot marshal a P-256 key: " + err.Error())
	}
	return b
}

// Fingerprint is the SHA-256 of a DER SubjectPublicKeyInfo (PAIRING.md §1).
// A unit's fingerprint is its device_id.
func Fingerprint(spki []byte) [32]byte { return sha256.Sum256(spki) }

// Short is the form people compare: the first sixteen hex digits of a
// fingerprint, upper case, in groups of four ("3F2A 91C4 0B7E D115").
func Short(fp [32]byte) string {
	h := strings.ToUpper(hex.EncodeToString(fp[:8]))
	return h[0:4] + " " + h[4:8] + " " + h[8:12] + " " + h[12:16]
}

// Prefix is the form logs use: the first sixteen hex digits, lower case.
func Prefix(fp [32]byte) string { return hex.EncodeToString(fp[:8]) }

// Sign is ECDSA P-256 over SHA-256(msg), DER-encoded (as OpenSSL and the
// Android Keystore's SHA256withECDSA produce).
func Sign(priv *ecdsa.PrivateKey, msg []byte) ([]byte, error) {
	h := sha256.Sum256(msg)
	return ecdsa.SignASN1(rand.Reader, priv, h[:])
}

// Verify checks a DER ECDSA P-256 SHA-256 signature.
func Verify(pub *ecdsa.PublicKey, msg, sig []byte) bool {
	if pub == nil || len(sig) == 0 || len(sig) > MaxSig {
		return false
	}
	h := sha256.Sum256(msg)
	return ecdsa.VerifyASN1(pub, h[:], sig)
}

// VerifySPKI parses spki and checks sig over msg with it.
func VerifySPKI(spki, msg, sig []byte) bool {
	pub, err := ParseSPKI(spki)
	return err == nil && Verify(pub, msg, sig)
}
