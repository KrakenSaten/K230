// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

// Command lying-relay runs the lying DOORS Relay of internal/liar, for the
// host tests of later phases (gatewayd and its helper, the app's
// RelayTransport). It is a test tool: never deploy it.
//
//	lying-relay -listen 127.0.0.1:8443 -key relay.key -lie admit-anyone,withhold-admitted
package main

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"sort"
	"strings"
	"syscall"

	"github.com/KrakenSaten/K230/relay/internal/fake"
	"github.com/KrakenSaten/K230/relay/internal/liar"
	"github.com/KrakenSaten/K230/relay/internal/server"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

var named = map[string]func(*liar.Lies){
	"tls12":              func(l *liar.Lies) { l.TLS12 = true },
	"unsolicited-backup": func(l *liar.Lies) { l.UnsolicitedBackup = true },
	"backup-own-nonce":   func(l *liar.Lies) { l.BackupOwnNonce = true },
	"backup-other-key":   func(l *liar.Lies) { l.BackupSigner = fake.NewKey() },
	"proof-own-nonce":    func(l *liar.Lies) { l.ProofOwnNonce = true },
	"proof-other-key":    func(l *liar.Lies) { l.ProofSigner = fake.NewKey() },
	"withhold-admitted":  func(l *liar.Lies) { l.WithholdAdmitted = true },
	"ignore-removals":    func(l *liar.Lies) { l.IgnoreRemovals = true },
	"admit-anyone":       func(l *liar.Lies) { l.AdmitAnyone = true },
	"oversize":           func(l *liar.Lies) { l.Oversize = true },
	"garbage":            func(l *liar.Lies) { l.Garbage = true },
	"ping-flood":         func(l *liar.Lies) { l.PingFlood = 200 },
	"bogus-invite":       func(l *liar.Lies) { l.BogusInvite = true },
}

func main() {
	listen := flag.String("listen", "127.0.0.1:8443", "address to listen on")
	keyFile := flag.String("key", "", "relay key (PEM); a new one if empty")
	backup := flag.String("backup-hash", "", "backup key hash to claim (64 hex)")
	lies := flag.String("lie", "", "comma-separated lies: "+strings.Join(names(), ", "))
	flag.Parse()

	var l liar.Lies
	for _, n := range strings.Split(*lies, ",") {
		if n = strings.TrimSpace(n); n == "" {
			continue
		}
		f, ok := named[n]
		if !ok {
			fmt.Fprintf(os.Stderr, "lying-relay: unknown lie %q\n", n)
			os.Exit(2)
		}
		f(&l)
	}
	if *backup != "" {
		h, ok := wire.ParseHex32(*backup)
		if !ok {
			fmt.Fprintln(os.Stderr, "lying-relay: -backup-hash is 64 lower-case hex digits")
			os.Exit(2)
		}
		l.BackupHash = &h
	}
	key := fake.NewKey()
	if *keyFile != "" {
		data, err := os.ReadFile(*keyFile)
		if err == nil {
			key, err = server.ParseKeyPEM(data)
		}
		if err != nil {
			fmt.Fprintln(os.Stderr, "lying-relay:", err)
			os.Exit(1)
		}
	}
	r, err := liar.Start(*listen, key, l)
	if err != nil {
		fmt.Fprintln(os.Stderr, "lying-relay:", err)
		os.Exit(1)
	}
	h := r.KeyHash()
	fmt.Printf("lying relay on %s, key %s (%s)\n", r.Addr(), wire.Short(h), wire.Hex(h[:]))
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	r.Close()
}

func names() []string {
	var n []string
	for k := range named {
		n = append(n, k)
	}
	sort.Strings(n)
	return n
}
