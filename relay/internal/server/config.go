// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package server

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/KrakenSaten/K230/relay/internal/wire"
)

// Config is the relay's configuration file (JSON). Unknown members are
// refused, so a typo is an error rather than a default.
type Config struct {
	// Listen is the TCP address to listen on, for example ":443".
	Listen string `json:"listen"`
	// Name is the relay's DNS name as units and phones dial it: lower-case
	// a-z, 0-9, '-' and '.', at most 64 characters. SNI and Host must name it.
	Name string `json:"name"`
	// Port is the public port units and phones dial (default 443). It is
	// part of every endorsement, and may differ from Listen behind a port
	// forward or an SNI pass-through.
	Port int `json:"port,omitempty"`
	// KeyFile is the active key: a PEM P-256 private key (PKCS#8 or SEC1).
	KeyFile string `json:"key_file"`
	// CertFile, if set, is a PEM certificate chain whose leaf carries the
	// active key (for example a CA-issued one, rule 39). Without it the
	// relay presents one self-signed certificate for the key.
	CertFile string `json:"cert_file,omitempty"`
	// BackupHash is the SHA-256 of the backup key's SubjectPublicKeyInfo
	// (64 hex), as printed by backup-keygen. Without it, get_backup is
	// answered unavailable.
	BackupHash string `json:"backup_hash,omitempty"`
	// Units are the enrolled units' device_ids (64 hex).
	Units []string `json:"units"`
	// Limits are the relay's own resource limits.
	Limits Limits `json:"limits,omitempty"`
}

// Limits are what the operator may tune. The protocol's own limits (frame
// and head sizes, 8 admitted phones, one control connection, one session
// and one pairing candidate per unit, the 5 s cookie, 25 s pings and 75 s
// silence) are fixed by docs/api/relay.md and not configurable.
type Limits struct {
	// PerSourceRate is new connections per second from one source (default 10).
	PerSourceRate int `json:"per_source_rate,omitempty"`
	// PerSourceUnauth is connections from one source not yet logged in (default 64).
	PerSourceUnauth int `json:"per_source_unauth,omitempty"`
	// MaxConns is connections in all (default 1024).
	MaxConns int `json:"max_conns,omitempty"`
	// MaxSources caps the per-source table (default 4096). A new source
	// beyond it is refused at accept while no entry can be removed.
	MaxSources int `json:"max_sources,omitempty"`
	// SessionIdleSeconds ends a splice in which no byte moved (default 90).
	SessionIdleSeconds int `json:"session_idle_s,omitempty"`
}

// Defaults (RELAY_PROPOSAL.md §13; RESOLUTION §6).
const (
	DefaultPort            = 443
	DefaultPerSourceRate   = 10
	DefaultPerSourceUnauth = 64
	DefaultMaxConns        = 1024
	DefaultMaxSources      = 4096
	DefaultSessionIdle     = 90
)

func (l *Limits) fill() {
	if l.PerSourceRate == 0 {
		l.PerSourceRate = DefaultPerSourceRate
	}
	if l.PerSourceUnauth == 0 {
		l.PerSourceUnauth = DefaultPerSourceUnauth
	}
	if l.MaxConns == 0 {
		l.MaxConns = DefaultMaxConns
	}
	if l.MaxSources == 0 {
		l.MaxSources = DefaultMaxSources
	}
	if l.SessionIdleSeconds == 0 {
		l.SessionIdleSeconds = DefaultSessionIdle
	}
}

// ParseConfig decodes and checks a configuration, filling defaults.
// Relative file names are taken relative to dir.
func ParseConfig(data []byte, dir string) (*Config, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	var c Config
	if err := dec.Decode(&c); err != nil {
		return nil, fmt.Errorf("config: %w", err)
	}
	if dec.More() {
		return nil, errors.New("config: trailing data")
	}
	if c.Listen == "" {
		c.Listen = ":443"
	}
	if c.Port == 0 {
		c.Port = DefaultPort
	}
	if !wire.ValidName(c.Name) {
		return nil, errors.New("config: name must be 1 to 64 characters of a-z, 0-9, '-' and '.'")
	}
	if c.Port < 1 || c.Port > 65535 {
		return nil, errors.New("config: port must be 1 to 65535")
	}
	if c.KeyFile == "" {
		return nil, errors.New("config: key_file is required")
	}
	if c.BackupHash != "" {
		if _, ok := wire.ParseHex32(c.BackupHash); !ok {
			return nil, errors.New("config: backup_hash must be 64 lower-case hex digits")
		}
	}
	seen := map[string]bool{}
	for _, u := range c.Units {
		if _, ok := wire.ParseHex32(u); !ok {
			return nil, fmt.Errorf("config: unit %q is not 64 lower-case hex digits", u)
		}
		if seen[u] {
			return nil, fmt.Errorf("config: unit %s listed twice", u[:16])
		}
		seen[u] = true
	}
	l := &c.Limits
	if l.PerSourceRate < 0 || l.PerSourceUnauth < 0 || l.MaxConns < 0 || l.MaxSources < 0 || l.SessionIdleSeconds < 0 {
		return nil, errors.New("config: limits must be positive")
	}
	l.fill()
	if !filepath.IsAbs(c.KeyFile) {
		c.KeyFile = filepath.Join(dir, c.KeyFile)
	}
	if c.CertFile != "" && !filepath.IsAbs(c.CertFile) {
		c.CertFile = filepath.Join(dir, c.CertFile)
	}
	return &c, nil
}

// LoadConfig reads and checks the configuration file at path.
func LoadConfig(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return ParseConfig(data, filepath.Dir(path))
}

// ErrConfigChanged is a configuration file that changed between enroll's
// read and its write (Q26): nothing is written, and enroll can be run again.
var ErrConfigChanged = errors.New("enroll: the configuration file changed while enrolling; nothing written, run it again")

// enrollHook runs between enroll's read and its write (tests).
var enrollHook func()

// Enroll adds a device_id (64 hex, either case) to the configuration file
// at path, written by temp-flush-rename. It reports whether the unit was
// already enrolled. The relay reads the file at start only. If the file
// changed since it was read, Enroll writes nothing (a lost-update check);
// one operator writer at a time is still the rule.
func Enroll(path, deviceID string) (fp [32]byte, already bool, err error) {
	id := strings.ToLower(strings.TrimSpace(deviceID))
	fp, ok := wire.ParseHex32(id)
	if !ok {
		return fp, false, errors.New("enroll: a device_id is 64 hex digits")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return fp, false, err
	}
	// Check the whole file as the relay would, then edit only "units".
	if _, err := ParseConfig(data, filepath.Dir(path)); err != nil {
		return fp, false, err
	}
	var raw map[string]json.RawMessage
	if err := json.Unmarshal(data, &raw); err != nil {
		return fp, false, err
	}
	var units []string
	if u, ok := raw["units"]; ok {
		if err := json.Unmarshal(u, &units); err != nil {
			return fp, false, err
		}
	}
	for _, u := range units {
		if u == id {
			return fp, true, nil
		}
	}
	units = append(units, id)
	enc, _ := json.Marshal(units)
	raw["units"] = enc
	out, err := json.MarshalIndent(raw, "", "  ")
	if err != nil {
		return fp, false, err
	}
	out = append(out, '\n')
	if enrollHook != nil {
		enrollHook()
	}
	return fp, false, writeFileAtomic(path, out, data)
}

// writeFileAtomic writes data by temp-flush-rename, keeping the mode, and
// only if path still holds expect just before the rename.
func writeFileAtomic(path string, data, expect []byte) error {
	info, err := os.Stat(path)
	if err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), "."+filepath.Base(path)+".*")
	if err != nil {
		return err
	}
	name := tmp.Name()
	defer os.Remove(name)
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Chmod(info.Mode().Perm()); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	now, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if !bytes.Equal(now, expect) {
		return ErrConfigChanged
	}
	if err := os.Rename(name, path); err != nil {
		return err
	}
	if d, err := os.Open(filepath.Dir(path)); err == nil {
		d.Sync()
		d.Close()
	}
	return nil
}
