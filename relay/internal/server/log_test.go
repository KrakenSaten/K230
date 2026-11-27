// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"encoding/base64"
	"go/ast"
	"go/parser"
	"go/token"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// Rule 41: lifecycle, fingerprint prefixes, truncated addresses and byte
// counts; never a payload, a full fingerprint, a full address, a nonce, a
// cookie or a signature.
func TestLogsRule41(t *testing.T) {
	w := newWorld(t, opts{})
	p := newPhone(w.to)
	rec := pairLocally(t, w.u, p, true)
	s, err := p.Connect(ctx(t), rec, nil)
	if err != nil {
		t.Fatal(err)
	}
	const marker = "PAYLOAD-MARKER-31415"
	if got, _ := s.Echo(marker); got != marker {
		t.Fatal("echo")
	}
	nonce := s.Relay.Nonce
	s.Close()
	w.u.Set(func(u *fake.Unit) { u.Manual = true })
	go p.Connect(ctx(t), rec, nil)
	inv := <-w.u.Invites()
	time.Sleep(2500 * time.Millisecond) // the invitation expires
	w.u.Disconnect()
	time.Sleep(100 * time.Millisecond)

	logs := w.logs.String()
	if !contains(logs, "control up", "session up", "session down", "127.0.0.0/24",
		"unit="+wire.Prefix(w.u.ID), "phone="+wire.Prefix(p.FP), "in=", "out=") {
		t.Fatalf("lifecycle missing:\n%s", logs)
	}
	for what, s := range map[string]string{
		"full address":    "127.0.0.1",
		"payload":         marker,
		"unit full fp":    wire.Hex(w.u.ID[:]),
		"phone full fp":   wire.Hex(p.FP[:]),
		"nonce":           base64.StdEncoding.EncodeToString(nonce[:]),
		"cookie":          base64.StdEncoding.EncodeToString(inv.Cookie[:]),
		"cookie hex":      wire.Hex(inv.Cookie[:]),
		"endorsement sig": base64.StdEncoding.EncodeToString(w.u.Endorse().E),
	} {
		if strings.Contains(logs, s) {
			t.Errorf("log carries the %s", what)
		}
	}
}

func TestTruncated(t *testing.T) {
	for addr, want := range map[string]string{
		"192.0.2.77:5000":         "192.0.2.0/24",
		"[::ffff:192.0.2.77]:1":   "192.0.2.0/24",
		"[2001:db8:abcd:12::1]:1": "2001:db8:abcd::/48",
	} {
		a, _ := net.ResolveTCPAddr("tcp", addr)
		if got := truncated(a); got != want {
			t.Errorf("%s: %s", addr, got)
		}
	}
}

// The lint of R-T29: every log call of the server is in log.go, so the
// typed fields there are the only way anything reaches the log.
func TestLogCallsOnlyInLogGo(t *testing.T) {
	files, _ := filepath.Glob("*.go")
	fset := token.NewFileSet()
	for _, f := range files {
		if strings.HasSuffix(f, "_test.go") || f == "log.go" {
			continue
		}
		src, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		file, err := parser.ParseFile(fset, f, src, 0)
		if err != nil {
			t.Fatal(err)
		}
		ast.Inspect(file, func(n ast.Node) bool {
			call, ok := n.(*ast.CallExpr)
			if !ok {
				return true
			}
			sel, ok := call.Fun.(*ast.SelectorExpr)
			if !ok {
				return true
			}
			if id, ok := sel.X.(*ast.Ident); ok && (id.Name == "log" || id.Name == "slog" ||
				id.Name == "fmt" && strings.HasPrefix(sel.Sel.Name, "Print")) {
				t.Errorf("%s: %s.%s outside log.go", fset.Position(call.Pos()), id.Name, sel.Sel.Name)
			}
			if inner, ok := sel.X.(*ast.SelectorExpr); ok && inner.Sel.Name == "log" {
				t.Errorf("%s: .log.%s outside log.go", fset.Position(call.Pos()), sel.Sel.Name)
			}
			return true
		})
		for _, imp := range file.Imports {
			if p := strings.Trim(imp.Path.Value, `"`); p == "log" || p == "log/slog" {
				t.Errorf("%s imports %s", f, p)
			}
		}
	}
}
