// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

// Command doors-relay is the DOORS Relay server and its set-up tools
// (docs/api/relay.md, docs/design/companion/RELAY_PROPOSAL.md §13).
//
//	doors-relay serve -config relay.json
//	doors-relay keygen -out relay.key
//	doors-relay backup-keygen -out backup.key
//	doors-relay enroll -config relay.json <device_id>
package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"syscall"

	"github.com/KrakenSaten/K230/relay/internal/server"
	"github.com/KrakenSaten/K230/relay/internal/wire"
)

const usage = `usage:
  doors-relay serve -config FILE
  doors-relay keygen -out FILE
  doors-relay backup-keygen -out FILE
  doors-relay enroll -config FILE DEVICE_ID
`

func main() {
	if len(os.Args) < 2 {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
	var err error
	switch os.Args[1] {
	case "serve":
		err = serve(os.Args[2:])
	case "keygen":
		err = keygen(os.Args[2:], false, os.Stdout)
	case "backup-keygen":
		err = keygen(os.Args[2:], true, os.Stdout)
	case "enroll":
		err = enroll(os.Args[2:], os.Stdout)
	default:
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "doors-relay:", err)
		os.Exit(1)
	}
}

func serve(args []string) error {
	fs := flag.NewFlagSet("serve", flag.ExitOnError)
	path := fs.String("config", "", "configuration file")
	fs.Parse(args)
	if *path == "" || fs.NArg() != 0 {
		return errors.New("serve needs -config FILE")
	}
	cfg, err := server.LoadConfig(*path)
	if err != nil {
		return err
	}
	s, err := server.New(cfg, os.Stderr)
	if err != nil {
		return err
	}
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sig
		s.Close()
	}()
	return s.ListenAndServe()
}

func printFingerprint(w io.Writer, fp [32]byte) {
	fmt.Fprintf(w, "fingerprint: %s\n", wire.Short(fp))
	fmt.Fprintf(w, "full:        %s\n", wire.Hex(fp[:]))
}

func keygen(args []string, backup bool, w io.Writer) error {
	name := "keygen"
	if backup {
		name = "backup-keygen"
	}
	fs := flag.NewFlagSet(name, flag.ExitOnError)
	out := fs.String("out", "", "file for the new private key (PEM, 0600); must not exist")
	fs.Parse(args)
	if *out == "" || fs.NArg() != 0 {
		return fmt.Errorf("%s needs -out FILE", name)
	}
	k, err := server.WriteNewKey(*out)
	if err != nil {
		return err
	}
	fp := server.KeyFingerprint(&k.PublicKey)
	if backup {
		fmt.Fprintf(w, "backup key written to %s\n", *out)
		printFingerprint(w, fp)
		fmt.Fprintf(w, "\nPut this line in the relay's configuration:\n  \"backup_hash\": %q\n", wire.Hex(fp[:]))
		fmt.Fprintln(w, "\nThen move the key file offline. The relay needs only the hash until the")
		fmt.Fprintln(w, "day the backup is promoted: then it becomes key_file.")
		return nil
	}
	fmt.Fprintf(w, "relay key written to %s\n", *out)
	printFingerprint(w, fp)
	fmt.Fprintln(w, "\nA unit shows this fingerprint at set-up; compare all sixteen digits.")
	return nil
}

func enroll(args []string, w io.Writer) error {
	fs := flag.NewFlagSet("enroll", flag.ExitOnError)
	path := fs.String("config", "", "configuration file")
	fs.Parse(args)
	if *path == "" || fs.NArg() != 1 {
		return errors.New("enroll needs -config FILE DEVICE_ID")
	}
	fp, already, err := server.Enroll(*path, fs.Arg(0))
	if err != nil {
		return err
	}
	if already {
		fmt.Fprintln(w, "already enrolled:")
	} else {
		fmt.Fprintln(w, "enrolled (restart the relay to apply):")
	}
	printFingerprint(w, fp)
	fmt.Fprintln(w, "\nCompare the sixteen digits with the unit's own Settings before trusting it.")
	return nil
}
