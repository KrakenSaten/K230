// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"errors"
	"fmt"
	"math/big"
	"os"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// ParseKeyPEM accepts one PEM P-256 private key, PKCS#8 ("PRIVATE KEY") or
// SEC1 ("EC PRIVATE KEY"). Any other key type is refused.
func ParseKeyPEM(data []byte) (*ecdsa.PrivateKey, error) {
	blk, _ := pem.Decode(data)
	if blk == nil {
		return nil, errors.New("key: no PEM block")
	}
	var key any
	var err error
	switch blk.Type {
	case "PRIVATE KEY":
		key, err = x509.ParsePKCS8PrivateKey(blk.Bytes)
	case "EC PRIVATE KEY":
		key, err = x509.ParseECPrivateKey(blk.Bytes)
	default:
		return nil, fmt.Errorf("key: unexpected PEM block %q", blk.Type)
	}
	if err != nil {
		return nil, fmt.Errorf("key: %w", err)
	}
	ec, ok := key.(*ecdsa.PrivateKey)
	if !ok || ec.Curve != elliptic.P256() {
		return nil, errors.New("key: not an ECDSA P-256 key")
	}
	return ec, nil
}

// NewKeyPEM makes a P-256 key and returns it as PKCS#8 PEM.
func NewKeyPEM() (*ecdsa.PrivateKey, []byte, error) {
	k, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, nil, err
	}
	der, err := x509.MarshalPKCS8PrivateKey(k)
	if err != nil {
		return nil, nil, err
	}
	return k, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: der}), nil
}

// WriteNewKey makes a key and writes it to path (0600), refusing to replace
// an existing file.
func WriteNewKey(path string) (*ecdsa.PrivateKey, error) {
	k, p, err := NewKeyPEM()
	if err != nil {
		return nil, err
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return nil, err
	}
	if _, err := f.Write(p); err != nil {
		f.Close()
		os.Remove(path)
		return nil, err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		os.Remove(path)
		return nil, err
	}
	return k, f.Close()
}

// SelfSigned is the one certificate the relay presents without a CertFile.
// Its fields are informational, fixed as PAIRING.md §2 fixes the gateway's:
// notBefore 1970-01-01, notAfter 9999-12-31T23:59:59Z, CN=doors-relay, a
// random serial. Units and phones decide by the leaf key alone.
func SelfSigned(key *ecdsa.PrivateKey) (tls.Certificate, error) {
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 127))
	if err != nil {
		return tls.Certificate{}, err
	}
	tmpl := &x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "doors-relay"},
		NotBefore:             time.Unix(0, 0).UTC(),
		NotAfter:              time.Date(9999, 12, 31, 23, 59, 59, 0, time.UTC),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		return tls.Certificate{}, err
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}, nil
}

// ChainFor reads a PEM chain whose leaf must carry key. Dates and issuers
// are not checked: they matter to middleboxes, not to Doors (rule 39).
func ChainFor(certPEM []byte, key *ecdsa.PrivateKey) (tls.Certificate, error) {
	var chain [][]byte
	for {
		var blk *pem.Block
		blk, certPEM = pem.Decode(certPEM)
		if blk == nil {
			break
		}
		if blk.Type == "CERTIFICATE" {
			chain = append(chain, blk.Bytes)
		}
	}
	if len(chain) == 0 {
		return tls.Certificate{}, errors.New("cert: no certificate in cert_file")
	}
	leaf, err := x509.ParseCertificate(chain[0])
	if err != nil {
		return tls.Certificate{}, fmt.Errorf("cert: %w", err)
	}
	pub, ok := leaf.PublicKey.(*ecdsa.PublicKey)
	if !ok || !pub.Equal(&key.PublicKey) {
		return tls.Certificate{}, errors.New("cert: the leaf certificate does not carry key_file's key")
	}
	return tls.Certificate{Certificate: chain, PrivateKey: key, Leaf: leaf}, nil
}

// KeyFingerprint is the SHA-256 of a key's SubjectPublicKeyInfo.
func KeyFingerprint(pub *ecdsa.PublicKey) [32]byte { return wire.Fingerprint(wire.SPKI(pub)) }
