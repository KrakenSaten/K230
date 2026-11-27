// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"bytes"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

var (
	short = regexp.MustCompile(`(?m)^fingerprint: [0-9A-F]{4} [0-9A-F]{4} [0-9A-F]{4} [0-9A-F]{4}$`)
	full  = regexp.MustCompile(`(?m)^full: +([0-9a-f]{64})$`)
)

func TestKeygenPrintsBothForms(t *testing.T) {
	dir := t.TempDir()
	for _, backup := range []bool{false, true} {
		var out bytes.Buffer
		path := filepath.Join(dir, map[bool]string{false: "relay.key", true: "backup.key"}[backup])
		if err := keygen([]string{"-out", path}, backup, &out); err != nil {
			t.Fatal(err)
		}
		s := out.String()
		m := full.FindStringSubmatch(s)
		if !short.MatchString(s) || m == nil {
			t.Fatalf("output:\n%s", s)
		}
		if got := strings.ReplaceAll(strings.ToLower(short.FindString(s)[13:]), " ", ""); got != m[1][:16] {
			t.Fatalf("short %s is not the start of %s", got, m[1])
		}
		if backup && !strings.Contains(s, `"backup_hash": "`+m[1]+`"`) {
			t.Fatalf("no configuration line:\n%s", s)
		}
	}
}

func TestEnrollPrintsBothForms(t *testing.T) {
	dir := t.TempDir()
	cfg := filepath.Join(dir, "relay.json")
	os.WriteFile(cfg, []byte(`{"name":"relay.example.org","key_file":"relay.key","units":[]}`), 0o600)
	id := strings.Repeat("0123456789abcdef", 4)
	var out bytes.Buffer
	if err := enroll([]string{"-config", cfg, id}, &out); err != nil {
		t.Fatal(err)
	}
	s := out.String()
	if !strings.Contains(s, "fingerprint: 0123 4567 89AB CDEF") || !strings.Contains(s, id) {
		t.Fatalf("output:\n%s", s)
	}
}
