// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package wire

import (
	"bufio"
	"bytes"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/hex"
	"reflect"
	"strings"
	"testing"
)

func seq(start byte, n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = start + byte(i)
	}
	return b
}

func h32(start byte) (v [32]byte) { copy(v[:], seq(start, 32)); return }
func h16(start byte) (v [16]byte) { copy(v[:], seq(start, 16)); return }

func testKey(t testing.TB) *ecdsa.PrivateKey {
	k, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	return k
}

// allMessages has one value of every type, for round trips.
func allMessages(t testing.TB) []Msg {
	k := testKey(t)
	spki := SPKI(&k.PublicKey)
	sig, _ := Sign(k, []byte("x"))
	e, _ := Endorsement{ConfigID: 7, Seq: 2, Name: "relay.example.org", Port: 443, KeyHash: h32(1)}.Marshal()
	slot := h16(9)
	return []Msg{
		&Challenge{Nonce: h32(0)},
		&UnitLogin{SPKI: spki, Sig: sig},
		&Prove{Nonce: h32(2)},
		&KeyProof{SPKI: spki, Sig: sig},
		&GetBackup{Nonce: h32(3)},
		&Backup{SPKIA: spki, SPKIHashB: h32(4), Sig: sig},
		&Register{Admit: [][32]byte{h32(5), h32(6)}, Endorsement: SignedEndorsement{E: e, Sig: sig}},
		&Register{Admit: [][32]byte{}, Endorsement: SignedEndorsement{E: e, Sig: sig}},
		&Admit{Admit: [][32]byte{h32(7)}, N: 4294967295},
		&Admitted{N: 0},
		&PairSlot{Slot: &slot},
		&PairSlot{},
		&Invite{Cookie: h16(1), Client: h32(8), Via: ViaAdmit},
		&Invite{Cookie: h16(2), Client: h32(8), Via: ViaPair},
		&Decline{Cookie: h16(3)},
		&Ping{N: 1},
		&Pong{N: 2},
		&Which{Lookup: h32(9)},
		&EndorsementFrame{SPKI: spki, E: e, Sig: sig},
		&ClientLogin{SPKI: spki, DeviceID: h32(10), Sig: sig},
		&Connect{},
		&Connect{Slot: &slot},
		&Ready{},
		&Unavailable{},
		&Join{Cookie: h16(4)},
	}
}

func TestRoundTrip(t *testing.T) {
	for _, m := range allMessages(t) {
		b := Encode(m)
		if len(b) > MaxFrame {
			t.Fatalf("%s: %d bytes", m.Type(), len(b))
		}
		got, err := Decode(b)
		if err != nil {
			t.Fatalf("%s: %v: %s", m.Type(), err, b)
		}
		if !reflect.DeepEqual(got, m) {
			t.Fatalf("%s: got %#v want %#v", m.Type(), got, m)
		}
		var buf bytes.Buffer
		if err := WriteMsg(&buf, m); err != nil {
			t.Fatal(err)
		}
		again, err := ReadMsg(&buf)
		if err != nil || !reflect.DeepEqual(again, m) {
			t.Fatalf("%s: frame round trip: %v", m.Type(), err)
		}
	}
}

func TestDecodeRefuses(t *testing.T) {
	nonce := B64(seq(0, 32))
	fp := strings.Repeat("ab", 32)
	bad := []string{
		``, `[]`, `null`, `"x"`, `{}`,
		`{"type":"nope"}`,
		`{"type":"ready","extra":1}`,
		`{"type":"ready"} {}`,
		`{"type":"ready"}x`,
		`{"type":"ready","type":"ready"}`,
		`{"type":"challenge"}`,
		`{"type":"challenge","nonce":null}`,
		`{"type":"challenge","nonce":1}`,
		`{"type":"challenge","nonce":"` + B64(seq(0, 31)) + `"}`,
		`{"type":"challenge","nonce":"` + B64(seq(0, 33)) + `"}`,
		`{"type":"challenge","nonce":"` + strings.TrimRight(nonce, "=") + `"}`,
		`{"type":"challenge","nonce":"` + nonce[:20] + "\\n" + nonce[20:] + `"}`,
		`{"type":"challenge","nonce":"` + strings.ReplaceAll(strings.ReplaceAll(nonce, "+", "-"), "/", "_") + `x"}`,
		`{"type":"which","lookup":"` + strings.ToUpper(fp) + `"}`,
		`{"type":"which","lookup":"` + fp[:62] + `"}`,
		`{"type":"which","lookup":"` + fp + `0"}`,
		`{"type":"ping","n":-1}`,
		`{"type":"ping","n":1.5}`,
		`{"type":"ping","n":1e3}`,
		`{"type":"ping","n":4294967296}`,
		`{"type":"ping","n":"1"}`,
		`{"type":"ping","n":null}`,
		`{"type":"admit","admit":null,"n":1}`,
		`{"type":"admit","admit":["` + fp + `","` + fp + `"],"n":1}`,
		`{"type":"admit","admit":[` + strings.Repeat(`"`+fp+`",`, 8) + `"` + strings.Repeat("cd", 32) + `"],"n":1}`,
		`{"type":"invite","cookie":"` + B64(seq(0, 16)) + `","client":"` + fp + `","via":"lan"}`,
		`{"type":"connect","slot":null}`,
		`{"type":"pair_slot"}`,
		`{"type":"register","admit":[],"endorsement":{"e":"AA==","sig":"AA==","x":1}}`,
		"{\"type\":\"ready\",\"\xff\":1}",
	}
	for _, s := range bad {
		if m, err := Decode([]byte(s)); err == nil {
			t.Errorf("accepted %q as %#v", s, m)
		}
	}
	// A key that is not P-256 has the right length nowhere: refused by size.
	rk, _ := rsa.GenerateKey(rand.Reader, 1024)
	rspki, _ := x509.MarshalPKIXPublicKey(&rk.PublicKey)
	if _, err := Decode(Encode(&UnitLogin{SPKI: rspki, Sig: []byte{1}})); err == nil {
		t.Error("accepted an RSA key")
	}
}

func TestFrameLimits(t *testing.T) {
	for _, n := range []uint32{0, MaxFrame + 1, 1 << 31} {
		h := []byte{byte(n >> 24), byte(n >> 16), byte(n >> 8), byte(n)}
		if _, err := ReadFrame(bytes.NewReader(append(h, make([]byte, 16)...))); err != ErrFrame {
			t.Errorf("length %d: %v", n, err)
		}
	}
	ok := append([]byte{0, 0, 0x10, 0}, bytes.Repeat([]byte{'a'}, MaxFrame)...)
	if b, err := ReadFrame(bytes.NewReader(ok)); err != nil || len(b) != MaxFrame {
		t.Errorf("4096 bytes: %v", err)
	}
	if _, err := ReadFrame(bytes.NewReader([]byte{0, 0, 0, 5, 'a'})); err == nil {
		t.Error("short frame accepted")
	}
}

func TestParseSPKI(t *testing.T) {
	k := testKey(t)
	spki := SPKI(&k.PublicKey)
	if _, err := ParseSPKI(spki); err != nil {
		t.Fatal(err)
	}
	k384, _ := ecdsa.GenerateKey(elliptic.P384(), rand.Reader)
	s384, _ := x509.MarshalPKIXPublicKey(&k384.PublicKey)
	if _, err := ParseSPKI(s384); err == nil {
		t.Error("P-384 accepted")
	}
	mangled := append([]byte(nil), spki...)
	mangled[len(mangled)-1] ^= 1 // off the curve
	if _, err := ParseSPKI(mangled); err == nil {
		t.Error("point off the curve accepted")
	}
}

func TestSignVerify(t *testing.T) {
	k := testKey(t)
	msg := UnitLoginMsg(h32(0), h32(1))
	sig, err := Sign(k, msg)
	if err != nil {
		t.Fatal(err)
	}
	if !VerifySPKI(SPKI(&k.PublicKey), msg, sig) {
		t.Fatal("does not verify")
	}
	if VerifySPKI(SPKI(&k.PublicKey), UnitLoginMsg(h32(0), h32(2)), sig) {
		t.Fatal("verifies for another relay key")
	}
	if VerifySPKI(SPKI(&testKey(t).PublicKey), msg, sig) {
		t.Fatal("verifies under another key")
	}
}

func TestContextsAreDistinct(t *testing.T) {
	ctxs := []string{CtxEndorsement, CtxUnitLogin, CtxClientLogin, CtxKeyProof, CtxBackup, CtxLookup, CtxPairSlot}
	seen := map[string]bool{}
	for _, c := range ctxs {
		if seen[c] || strings.ContainsRune(c, 0) || c[0] == 0x20 || c[0] == 0x30 {
			t.Fatalf("context %q", c)
		}
		seen[c] = true
	}
	// No message of one kind is a prefix-match of another's context.
	msgs := map[string][]byte{
		CtxUnitLogin:   UnitLoginMsg(h32(0), h32(1)),
		CtxClientLogin: ClientLoginMsg(h32(0), h32(1), h32(2)),
		CtxKeyProof:    KeyProofMsg(h32(0)),
		CtxBackup:      BackupMsg(h32(0), h32(1)),
	}
	for ctx, m := range msgs {
		for _, other := range ctxs {
			if other != ctx && bytes.HasPrefix(m, append([]byte(other), 0)) {
				t.Fatalf("%q message starts with %q", ctx, other)
			}
		}
	}
}

func TestEndorsement(t *testing.T) {
	e := Endorsement{ConfigID: 1<<63 + 5, Seq: 9, Name: strings.Repeat("a", 64), Port: 65535, KeyHash: h32(3)}
	b, err := e.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if len(b) != 27+8+8+1+64+2+32 {
		t.Fatalf("length %d", len(b))
	}
	got, err := ParseEndorsement(b)
	if err != nil || got != e {
		t.Fatalf("round trip: %v %+v", err, got)
	}
	for _, n := range []string{"", strings.Repeat("a", 65), "Relay.example", "relay_x", "relay example", "réla"} {
		if _, err := (Endorsement{Name: n, Port: 1}).Marshal(); err == nil {
			t.Errorf("name %q accepted", n)
		}
	}
	if _, err := (Endorsement{Name: "a", Port: 0}).Marshal(); err == nil {
		t.Error("port 0 accepted")
	}
	for i := 0; i < len(b); i++ {
		if _, err := ParseEndorsement(b[:i]); err == nil {
			t.Fatalf("prefix of %d bytes accepted", i)
		}
	}
	if _, err := ParseEndorsement(append(b, 0)); err == nil {
		t.Fatal("trailing byte accepted")
	}
	bad := append([]byte(nil), b...)
	bad[len(CtxEndorsement)] = 1
	if _, err := ParseEndorsement(bad); err == nil {
		t.Fatal("missing 0x00 accepted")
	}
}

// Fixed vectors, for the C (gatewayd) and Kotlin (app) implementations.
// Inputs: nonce = 00..1f, relay key hash = 20..3f, device_id = 40..5f,
// backup hash = 60..7f, token = 80..8f.
func TestVectors(t *testing.T) {
	nonce, rk, dev, bk := h32(0x00), h32(0x20), h32(0x40), h32(0x60)
	tok := seq(0x80, 16)
	l := Lookup(rk, dev)
	s := SlotFromToken(tok)
	e, _ := Endorsement{ConfigID: 0x0102030405060708, Seq: 3, Name: "relay.example.org", Port: 443, KeyHash: rk}.Marshal()
	got := map[string]string{
		"unit login":  hex.EncodeToString(UnitLoginMsg(nonce, rk)),
		"lookup":      hex.EncodeToString(l[:]),
		"pair slot":   hex.EncodeToString(s[:]),
		"endorsement": hex.EncodeToString(e),
		"backup":      hex.EncodeToString(BackupMsg(bk, nonce)),
		"ping frame":  hex.EncodeToString(AppendFrame(nil, Encode(&Ping{N: 7}))),
	}
	want := map[string]string{
		"unit login":  hex.EncodeToString([]byte(CtxUnitLogin)) + "00" + hex.EncodeToString(seq(0, 32)) + hex.EncodeToString(seq(0x20, 32)),
		"lookup":      vectorLookup,
		"pair slot":   vectorSlot,
		"endorsement": hex.EncodeToString([]byte(CtxEndorsement)) + "00" + "0102030405060708" + "0000000000000003" + "11" + hex.EncodeToString([]byte("relay.example.org")) + "01bb" + hex.EncodeToString(seq(0x20, 32)),
		"backup":      hex.EncodeToString([]byte(CtxBackup)) + "00" + hex.EncodeToString(seq(0x60, 32)) + hex.EncodeToString(seq(0, 32)),
		"ping frame":  "00000015" + hex.EncodeToString([]byte(`{"n":7,"type":"ping"}`)),
	}
	for k, v := range want {
		if got[k] != v {
			t.Errorf("%s:\n got %s\nwant %s", k, got[k], v)
		}
	}
}

func TestReadHead(t *testing.T) {
	req := StartRequest("relay.example.org", 443)
	br := bufio.NewReaderSize(bytes.NewReader(append(append([]byte(nil), req...), "after"...)), 4096)
	head, err := ReadHead(br)
	if err != nil || !bytes.Equal(head, req) {
		t.Fatalf("%v %q", err, head)
	}
	rest := make([]byte, 5)
	if _, err := br.Read(rest); err != nil || string(rest) != "after" {
		t.Fatal("read past the head")
	}
	long := "GET / HTTP/1.1\r\nX: " + strings.Repeat("a", MaxHead) + "\r\n\r\n"
	if _, err := ReadHead(bufio.NewReaderSize(strings.NewReader(long), 4096)); err != ErrHeadTooLong {
		t.Fatalf("long head: %v", err)
	}
	if _, err := ReadHead(bufio.NewReader(strings.NewReader("\r\nGET / HTTP/1.1\r\n\r\n"))); err != ErrHeadMalformed {
		t.Fatalf("leading empty line: %v", err)
	}
	longLine := strings.Repeat("a", 5000)
	if _, err := ReadHead(bufio.NewReaderSize(strings.NewReader(longLine), 16)); err != ErrHeadTooLong {
		t.Fatalf("long line: %v", err)
	}
}

func TestCheckStart(t *testing.T) {
	const name = "relay.example.org"
	good := []string{
		string(StartRequest(name, 443)),
		"GET /doors-relay/1 HTTP/1.1\r\nhost: RELAY.example.org:443\r\nupgrade: DOORS-RELAY/1\r\nconnection: upgrade\r\n\r\n",
	}
	for _, g := range good {
		if !CheckStart([]byte(g), name, 443) {
			t.Errorf("refused %q", g)
		}
	}
	if !CheckStart(StartRequest(name, 8443), name, 8443) {
		t.Error("refused a port in Host")
	}
	line := "GET /doors-relay/1 HTTP/1.1\r\n"
	hdr := "Host: relay.example.org\r\nConnection: Upgrade\r\nUpgrade: doors-relay/1\r\n"
	bad := []string{
		"POST /doors-relay/1 HTTP/1.1\r\n" + hdr + "\r\n",
		"GET / HTTP/1.1\r\n" + hdr + "\r\n",
		"GET /doors-relay/1/ HTTP/1.1\r\n" + hdr + "\r\n",
		"GET /doors-relay/1?x HTTP/1.1\r\n" + hdr + "\r\n",
		"GET https://relay.example.org/doors-relay/1 HTTP/1.1\r\n" + hdr + "\r\n",
		"GET /doors-relay/1 HTTP/1.0\r\n" + hdr + "\r\n",
		"GET /doors-relay/2 HTTP/1.1\r\n" + hdr + "\r\n",
		line + "Host: other.example.org\r\nConnection: Upgrade\r\nUpgrade: doors-relay/1\r\n\r\n",
		line + "Host: relay.example.org:444\r\nConnection: Upgrade\r\nUpgrade: doors-relay/1\r\n\r\n",
		line + "Host: relay.example.org\r\nUpgrade: doors-relay/1\r\n\r\n",
		line + "Host: relay.example.org\r\nConnection: Upgrade\r\n\r\n",
		line + "Host: relay.example.org\r\nConnection: keep-alive, Upgrade\r\nUpgrade: doors-relay/1\r\n\r\n",
		line + "Host: relay.example.org\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n",
		line + hdr + "User-Agent: x\r\n\r\n",
		line + hdr + "Upgrade: doors-relay/1\r\n\r\n",
		line + hdr + "Content-Length: 0\r\n\r\n",
		line + hdr + "Transfer-Encoding: chunked\r\n\r\n",
		"GET /doors-relay/1 HTTP/1.1\nHost: relay.example.org\nConnection: Upgrade\nUpgrade: doors-relay/1\n\n",
		line + "Host: relay.example.org\r\nConnection: Upgrade\r\nUpgrade: doors-relay/1\n\r\n",
		"",
		"garbage\r\n\r\n",
	}
	for _, b := range bad {
		if CheckStart([]byte(b), name, 443) {
			t.Errorf("accepted %q", b)
		}
	}
}

func TestReadSwitching(t *testing.T) {
	if err := ReadSwitching(bufio.NewReader(bytes.NewReader(Switching))); err != nil {
		t.Fatal(err)
	}
	if err := ReadSwitching(bufio.NewReader(bytes.NewReader(NotFound))); err == nil {
		t.Fatal("404 accepted")
	}
}
