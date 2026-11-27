// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/pem"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

func TestParseConfig(t *testing.T) {
	id := strings.Repeat("ab", 32)
	c, err := ParseConfig([]byte(`{"name":"relay.example.org","key_file":"relay.key","units":["`+id+`"]}`), "/etc/doors-relay")
	if err != nil {
		t.Fatal(err)
	}
	if c.Listen != ":443" || c.Port != 443 || c.KeyFile != filepath.Join("/etc/doors-relay", "relay.key") ||
		c.Limits.PerSourceRate != 10 || c.Limits.PerSourceUnauth != 64 || c.Limits.MaxConns != 1024 ||
		c.Limits.SessionIdleSeconds != 90 {
		t.Fatalf("defaults: %+v", c)
	}
	for name, bad := range map[string]string{
		"unknown member": `{"name":"r","key_file":"k","units":[],"lisen":":1"}`,
		"bad name":       `{"name":"Relay","key_file":"k","units":[]}`,
		"long name":      `{"name":"` + strings.Repeat("a", 65) + `","key_file":"k","units":[]}`,
		"no key":         `{"name":"r","units":[]}`,
		"bad unit":       `{"name":"r","key_file":"k","units":["` + strings.ToUpper(id) + `"]}`,
		"twice":          `{"name":"r","key_file":"k","units":["` + id + `","` + id + `"]}`,
		"bad backup":     `{"name":"r","key_file":"k","units":[],"backup_hash":"00"}`,
		"bad port":       `{"name":"r","key_file":"k","units":[],"port":70000}`,
		"negative":       `{"name":"r","key_file":"k","units":[],"limits":{"max_conns":-1}}`,
		"trailing":       `{"name":"r","key_file":"k","units":[]} {}`,
	} {
		if _, err := ParseConfig([]byte(bad), "."); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
}

func TestEnroll(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "relay.json")
	os.WriteFile(path, []byte(`{"name":"relay.example.org","key_file":"relay.key","units":[],"limits":{"max_conns":50}}`), 0o640)
	u := fake.NewUnit("relay.example.org", 443, nil)
	id := strings.ToUpper(wire.Hex(u.ID[:]))
	fp, already, err := Enroll(path, id)
	if err != nil || already || fp != u.ID {
		t.Fatal(err, already)
	}
	if _, already, _ = Enroll(path, id); !already {
		t.Fatal("enrolled twice")
	}
	c, err := LoadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(c.Units) != 1 || c.Units[0] != wire.Hex(u.ID[:]) || c.Limits.MaxConns != 50 {
		t.Fatalf("after enroll: %+v", c)
	}
	if info, _ := os.Stat(path); info.Mode().Perm() != 0o640 {
		t.Fatalf("mode %v", info.Mode())
	}
	if _, _, err := Enroll(path, "abcd"); err == nil {
		t.Fatal("short id accepted")
	}
}

func TestKeys(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "relay.key")
	k, err := WriteNewKey(path)
	if err != nil {
		t.Fatal(err)
	}
	if info, _ := os.Stat(path); info.Mode().Perm() != 0o600 {
		t.Fatalf("key mode %v", info.Mode())
	}
	if _, err := WriteNewKey(path); err == nil {
		t.Fatal("an existing key was replaced")
	}
	data, _ := os.ReadFile(path)
	again, err := ParseKeyPEM(data)
	if err != nil || !again.Equal(k) {
		t.Fatal("key does not load back")
	}
	sec1, _ := x509.MarshalECPrivateKey(k)
	if _, err := ParseKeyPEM(pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: sec1})); err != nil {
		t.Fatal("SEC1 refused:", err)
	}
	rk, _ := rsa.GenerateKey(rand.Reader, 1024)
	rder, _ := x509.MarshalPKCS8PrivateKey(rk)
	if _, err := ParseKeyPEM(pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: rder})); err == nil {
		t.Fatal("RSA key accepted")
	}
	other := fake.SelfSigned(fake.NewKey(), "x")
	otherPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: other.Certificate[0]})
	if _, err := ChainFor(otherPEM, k); err == nil {
		t.Fatal("a chain for another key accepted")
	}
	cert, err := SelfSigned(k)
	if err != nil {
		t.Fatal(err)
	}
	leaf, _ := x509.ParseCertificate(cert.Certificate[0])
	if leaf.NotBefore.Unix() != 0 || leaf.NotAfter.Year() != 9999 || leaf.Subject.CommonName != "doors-relay" {
		t.Fatalf("self-signed fields: %v %v %q", leaf.NotBefore, leaf.NotAfter, leaf.Subject.CommonName)
	}
	cfg := &Config{Name: "relay.example.org", Port: 443, KeyFile: path, Limits: Limits{}}
	cfg.Limits.fill()
	s, err := New(cfg, os.Stderr)
	if err != nil {
		t.Fatal(err)
	}
	if s.KeyHash() != KeyFingerprint(&k.PublicKey) {
		t.Fatal("relay presents another key")
	}
	s.Close()
}
