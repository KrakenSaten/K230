// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package wire

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"os"
	"testing"
)

// T16 (Q12): the published frame vectors in testdata/vectors.json, which
// the C and Kotlin ends can reuse, hold for this decoder.
func TestFrameVectors(t *testing.T) {
	data, err := os.ReadFile("testdata/vectors.json")
	if err != nil {
		t.Fatal(err)
	}
	var v struct {
		Accept, Reject []struct{ Name, JSON, Hex string }
	}
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&struct {
		About  string
		Accept *[]struct{ Name, JSON, Hex string }
		Reject *[]struct{ Name, JSON, Hex string }
	}{Accept: &v.Accept, Reject: &v.Reject}); err != nil {
		t.Fatal(err)
	}
	if len(v.Accept) == 0 || len(v.Reject) < 40 {
		t.Fatalf("%d accept, %d reject vectors", len(v.Accept), len(v.Reject))
	}
	payload := func(j, h string) []byte {
		if h != "" {
			b, err := hex.DecodeString(h)
			if err != nil {
				t.Fatal(err)
			}
			return b
		}
		return []byte(j)
	}
	for _, x := range v.Accept {
		if _, err := Decode(payload(x.JSON, x.Hex)); err != nil {
			t.Errorf("accept %q: %v", x.Name, err)
		}
	}
	for _, x := range v.Reject {
		if m, err := Decode(payload(x.JSON, x.Hex)); err == nil {
			t.Errorf("reject %q: accepted as %#v", x.Name, m)
		}
	}
}
