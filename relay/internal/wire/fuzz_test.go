// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package wire

import (
	"bufio"
	"bytes"
	"reflect"
	"strings"
	"testing"
)

// FuzzFrames feeds arbitrary bytes to the frame reader and the decoder, as
// a relay connection after the 101 would. Whatever decodes must encode
// back to a frame that decodes to the same message, and stay within limits.
func FuzzFrames(f *testing.F) {
	for _, m := range allMessages(f) {
		f.Add(AppendFrame(nil, Encode(m)))
	}
	f.Add([]byte{0, 0, 0, 0})
	f.Add([]byte{0xff, 0xff, 0xff, 0xff})
	f.Add(AppendFrame(nil, []byte(`{"type":"ping","n":1,"n":2}`)))
	f.Add(AppendFrame(nil, []byte(`{"type":"ready"}`+"\x00")))
	f.Fuzz(func(t *testing.T, data []byte) {
		r := bytes.NewReader(data)
		for i := 0; i < 64; i++ {
			b, err := ReadFrame(r)
			if err != nil {
				return
			}
			if len(b) == 0 || len(b) > MaxFrame {
				t.Fatalf("frame of %d bytes", len(b))
			}
			m, err := Decode(b)
			if err != nil {
				continue
			}
			again, err := Decode(Encode(m))
			if err != nil || !reflect.DeepEqual(again, m) {
				t.Fatalf("%s does not round-trip: %v", m.Type(), err)
			}
		}
	})
}

// FuzzStart feeds arbitrary bytes to the relay's HTTP start: ReadHead with
// its 2 KiB bound, then CheckStart. An accepted head is exactly the Upgrade
// request: it is never longer than the bound, and it has the request line
// and the three headers.
func FuzzStart(f *testing.F) {
	const name = "relay.example.org"
	f.Add(StartRequest(name, 443))
	f.Add(StartRequest(name, 8443))
	f.Add([]byte("GET / HTTP/1.1\r\nHost: relay.example.org\r\n\r\n"))
	f.Add([]byte("GET /doors-relay/1 HTTP/1.1\r\nHost: relay.example.org\r\nConnection: Upgrade\r\nUpgrade: doors-relay/1\r\nUpgrade: doors-relay/1\r\n\r\n"))
	f.Add([]byte("GET /doors-relay/1 HTTP/1.1\r\nHost: relay.example.org\r\n Connection: Upgrade\r\nUpgrade: doors-relay/1\r\n\r\n"))
	f.Add([]byte(strings.Repeat("a", 3000)))
	f.Fuzz(func(t *testing.T, data []byte) {
		br := bufio.NewReaderSize(bytes.NewReader(data), 4096)
		head, err := ReadHead(br)
		if err != nil {
			return
		}
		if len(head) > MaxHead || !bytes.HasSuffix(head, []byte("\r\n\r\n")) {
			t.Fatalf("head of %d bytes: %q", len(head), head)
		}
		if !bytes.HasPrefix(data, head) {
			t.Fatal("head is not a prefix of the input")
		}
		if !CheckStart(head, name, 443) {
			return
		}
		lines := strings.Split(strings.TrimSuffix(string(head), "\r\n\r\n"), "\r\n")
		if lines[0] != "GET "+Path+" HTTP/1.1" || len(lines) != 4 {
			t.Fatalf("accepted %q", head)
		}
	})
}

// FuzzEndorsement: whatever parses marshals back to the same bytes.
func FuzzEndorsement(f *testing.F) {
	e, _ := Endorsement{ConfigID: 1, Seq: 2, Name: "relay.example.org", Port: 443}.Marshal()
	f.Add(e)
	f.Add([]byte(CtxEndorsement))
	f.Fuzz(func(t *testing.T, data []byte) {
		e, err := ParseEndorsement(data)
		if err != nil {
			return
		}
		b, err := e.Marshal()
		if err != nil || !bytes.Equal(b, data) {
			t.Fatalf("does not round-trip: %v", err)
		}
	})
}
