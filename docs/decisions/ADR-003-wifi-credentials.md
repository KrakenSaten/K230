# ADR-003: Storing Wi-Fi credentials

Status: Accepted for this milestone (product owner, 2026-09-13)
Date: 2026-09-12 (proposed), 2026-09-13 (accepted)
Deciders: product owner (final), AI engineering partner (author)

## Acceptance (2026-09-13)

Accepted for the post-v0.0.9 foundations milestone, with these conditions,
which the rest of this record already follows and which stay binding:

- Wi-Fi credentials may be stored root-only in
  `/var/lib/pocketos/netd/wifi.conf`: directory 0700, file 0600.
- No credential in logs, API results, process command lines or the
  environment.
- **The credentials are protected by Unix file permissions only. There is no
  cryptographic at-rest protection.** The hex encoding in the file is an
  encoding that keeps any byte from breaking the line format; it is not
  encryption and must never be described as such.
- Secure credential storage is revisited if the platform gains a meaningful
  device-bound key store (hardware key store, secure element, or a key that
  does not live on the same card).

## Context

Joining a WPA2-Personal network needs its passphrase, and "reconnect after a
reboot" needs it to be kept. Until now PocketOS has deliberately persisted no
secret: `docs/ARCHITECTURE.md` lists secure credential storage as not yet
decided ("threat model, key storage and access control before any secret is
persisted"), and `ui/shell/settings.h` forbids Wi-Fi credentials in the
settings store, which is world-readable plain text.

What the platform offers, as of v0.0.9 (VERIFIED on unit A unless noted):

- every PocketOS process runs as root; there is no per-service user yet;
- the root filesystem is ext4 on a removable microSD card, not encrypted;
- there is no secure element, TPM or hardware key store in use (the K230 has
  security IP, DOCUMENTED, but nothing in the pinned BSP exposes it);
- the IPC sockets are 0660 in a 0770 directory, so anything that can reach
  netd is already root (docs/api/system.md, Trust model);
- the vendor image keeps its own Wi-Fi credentials in the U-Boot environment
  (`wlanssid`/`wlanpass`) and its launcher writes plaintext profiles
  (DOCUMENTED; review item F2).

## Threat model

In scope, and what the design has to hold against:

1. **Another local user or an unprivileged process** reading the passphrase.
   There are none today, but the files must already be closed to them.
2. **Logs, crash reports, screenshots and support bundles** carrying a
   passphrase off the device. These get copied around far more often than
   the card is imaged.
3. **Process command lines and environment**, readable by every process in
   `/proc`.
4. **A malformed SSID or passphrase** turning into a control-interface or
   configuration injection.
5. **A power cut during a write** leaving a half-written file, or a damaged
   file silently replaced by an empty one.

Out of scope, stated so nobody assumes otherwise:

6. **Root on the device.** Root can read the store, and can also read
   netd's memory. Without per-service users and a key the device does not
   hand to root, nothing file-based helps.
7. **Physical access to the card.** Pull the card, mount it, read the file.
   Protecting against that needs encryption with a key that is not on the
   same card (a passphrase at boot, or a hardware key store) - neither exists.
8. **An attacker on the Wi-Fi network itself.** That is WPA2's job.

## Options

### A. Keep nothing; ask for the passphrase every time

- Pro: no secret at rest.
- Con: no reconnect after reboot, which is a stated requirement; on a
  handheld with a mis-keying touch keyboard (DEV-1 qualification) retyping a
  63-character passphrase is not a product.

### B. wpa_supplicant's own config with `update_config=1`

- Pro: standard; wpa_supplicant writes it.
- Con: wpa_supplicant then owns a file netd cannot validate or version; its
  mode depends on the umask it inherits; it stores every network it has ever
  been told about; the plaintext `psk="..."` line is exactly the shape that
  ends up pasted into bug reports.

### C. A netd-owned store, root-only, strictly parsed (chosen)

- One file, `$POCKETOS_STATE_DIR/netd/wifi.conf` (default
  `/var/lib/pocketos/netd/wifi.conf`), directory 0700 and file 0600, written
  only by netd.
- Pro: one writer, one format, tested; permissions set explicitly rather
  than inherited; a damaged file is never half-believed or overwritten.
- Con: plaintext-equivalent at rest (hex-encoded); protection is file
  permissions only, which is what threats 1-5 need and not what 6-7 need.

### D. Encrypt the store

- Con: with no key outside the card, the key would sit beside the file and
  add nothing but code. Revisit when there is a hardware key store or a
  boot-time secret.

## Decision

Option C, with these rules, all implemented in `services/netd/` and tested in
`tests/wifi_store_test.c` and `tests/netd_test.sh`:

1. **Location and modes.** `$POCKETOS_STATE_DIR/netd/wifi.conf`; the
   directory is created 0700 and chmodded back to 0700 on every save, the
   file is created with `O_EXCL|O_NOFOLLOW` and `fchmod(0600)` regardless of
   umask. A file found wider than 0600 is logged and rewritten 0600.
2. **Format.** `version=1`, `enabled=0|1`, and one
   `network=<ssid hex> <security> <hidden> <passphrase hex or ->` per network,
   most recently joined first, at most 16. Hex is an encoding so no byte can
   break the line; the header says the file contains passphrases.
3. **Integrity.** Written to a temp file, fsynced, renamed, directory
   fsynced. A file that does not parse completely is reported `damaged`, the
   service starts with Wi-Fi off, and the file is renamed to
   `wifi.conf.damaged` (0600) before anything new is written. A file that
   exists but cannot be read is never replaced. A symlink is never followed.
4. **Only proven credentials are kept.** A network is stored only after
   wpa_supplicant reports the association complete with that passphrase; a
   wrong passphrase is removed from the supplicant and never written.
5. **No secret in logs.** netd logs SSIDs and states, never a passphrase and
   never the value of a control-interface command that can carry one (the
   verb and field name only); requests are logged by method name only.
   `wifi.status`, `wifi.networks` and `wifi.saved` never return a passphrase.
6. **No secret on a command line or in the environment.** wpa_supplicant is
   started with a generated config that holds no network; networks and
   passphrases go over its control socket (root-only, inside netd's 0700
   runtime directory). `pos wifi connect` reads the passphrase from stdin
   with echo off. (`pos call` can still put one on its own command line; it
   is a developer tool and is documented as such.)
7. **Validation before use.** SSIDs are 1..32 bytes and are sent as hex;
   passphrases must be 8..63 printable ASCII characters (IEEE 802.11i),
   which also makes quoting in the control protocol unambiguous.
8. **Memory.** Passphrase buffers netd owns are cleared with
   `explicit_bzero` when an attempt ends, when a network is forgotten and at
   exit. Copies inside cJSON and wpa_supplicant are not controlled.
9. **Forget means gone** from the file (on the next successful write) and
   from wpa_supplicant immediately.

## Consequences

Needed now: the rules above; the architecture doc's "no component may
persist a secret" becomes "netd may, under ADR-003"; the settings store rule
stays as it is.

Useful soon: per-service users so netd's store is not readable by every root
process (threat 6 partly); exclude `/var/lib/pocketos/netd` from any future
support bundle or backup by default; remove the vendor `wlanssid`/`wlanpass`
from the U-Boot environment (F2) so the card carries one credential store,
not two.

Future: encryption at rest once a key can live off the card (hardware key
store, or a user secret at boot).

Risks: a card handed to someone else carries the passphrases of every saved
network. The Settings UI and the docs say so; "Forget" is the remedy.

Migration: none; there was no store before. If the owner rejects this ADR,
netd can run with persistence disabled (every join is for the session only)
by removing rule 4's write, and the store file is deleted.

## Evidence

- All PocketOS processes run as root; sockets 0660 in a 0770 directory:
  VERIFIED (docs/api/system.md, Trust model).
- No encryption, no secure element in use: DOCUMENTED (pinned BSP/defconfig).
- wpa_supplicant 2.11 with the control interface in the image; no init script
  starts it: DOCUMENTED (Buildroot config and target tree,
  docs/hardware/WIFI_2026-09-12.md).
- Vendor credentials in the U-Boot environment and plaintext launcher
  profiles: DOCUMENTED (vendor default.env, launcher source).
- The behaviour in rules 1-9: VERIFIED on the host only
  (tests/wifi_store_test.c, tests/netd_test.sh); not yet run on unit A.
