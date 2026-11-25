# ADR-019 amendment 2: Companion in the image, its Settings switch, and the device password

Status: **Proposed** (2026-11-25). Written before the implementation and committed ahead of it on branch `claude/companion-settings-2-aad748`, which implements it as a draft PR for review. Host-tested and cross-built only; nothing has been deployed to a unit. Nothing here takes effect until the product owner accepts it.
Date: 2026-11-25
Deciders: product owner (final), AI engineering partner (author)
Amends: ADR-019 (accepted 2026-11-21), its "Consequences" and rule 9b. Settles open items U5, U7 and the Settings half of U12 (`docs/design/companion/README.md`) and the security prerequisites of issue #172.

## Context

ADR-019 accepted the gateway and left its product integration to later steps, each with its own go-ahead: "a Settings page (QR, paired clients, scopes, revoke) ... an init script, install lines and release checks; the hardware gate". Rule 9b made a device password a precondition and said that "how a device password is set on a unit is not defined by Doors today". Issue #172 lists what must hold before the gateway enters an image: SSH hardening shipped with it, a device-password flow, and a Settings pairing page that shows the candidate's key before the decision.

The product owner asked on 2026-11-25 for Companion to be a feature a person can use with no USB cable, SSH session or command line: gatewayd in the normal installation and upgrade, off by default; a persistent switch under Settings > Companion; pairing by QR code with the candidate verified and approved on the unit; paired phones, their state, permissions and removal on the unit; state kept across upgrades and reboots; errors a person can act on.

Facts this rests on (classes as AGENTS.md):

- The 0.5.0 release image (its rootfs, extracted on the build host on 2026-11-25) runs OpenSSH 9.9p2 with `PermitRootLogin yes`, `PasswordAuthentication yes`, `PermitEmptyPasswords no`, and no `Include` or `Match` line; root's `/etc/shadow` entry has an empty password field; `/etc/shadow` is 0600; Buildroot's `S50sshd` has `reload` (SIGHUP). VERIFIED (read from the image, not on a unit).
- The image's BusyBox has no `chpasswd` (`# CONFIG_CHPASSWD is not set` in the build's BusyBox configuration; no such applet in the release rootfs); `passwd` exists but asks on a terminal. VERIFIED (build tree and release rootfs).
- sshd in the image links `libcrypt.so.2`, which is libxcrypt 4.4.38 (`/usr/lib/libcrypt.so.2.0.0` in the rootfs; `docs/legal/manifest.csv`). VERIFIED (readelf on the release rootfs) and DOCUMENTED (legal-info).
- OpenSSL 3.4.1 and cJSON are in the image; gatewayd cross-builds against the v0.4.0 sysroot. DOCUMENTED (`docs/services/GATEWAYD.md`).
- sysd already makes the system-wide changes the owner asks for from the UI: reboot and power-off, `storage.expand`, and `radio_setup.start`, which writes `/etc/default/meshcored` and restarts `S65meshcored` in a child process. DOCUMENTED (`services/sysd`, `docs/api/system.md`).
- The Terminal app opens root's login shell on the unit. DOCUMENTED (`docs/apps/TERMINAL.md`). Whoever has the unit's unlocked screen is already root.

## Options

| | A. sysd `companion.*` (chosen) | B. The shell runs the init script | C. gatewayd always runs, switched inside | D. A generic `service.start {name}` |
| --- | --- | --- | --- | --- |
| Who changes the system | sysd, as for radio setup | the shell process | gatewayd itself | sysd |
| Scope of the control | one service, one boolean, fixed argv | any script the shell chooses | none needed | any named service |
| Rule 1 ("disabled" = not running) | kept | kept | broken: a daemon runs and parses its local socket on every unit | kept |
| Against | sysd grows by one module | ADR-002 (services own system changes); blocks the UI thread for the stop's 3 s | rule 1; a process on every unit for a feature most do not use | not narrow; any root process could start anything |

Device password: (a) sysd hashes with libxcrypt and writes `/etc/shadow` (chosen); (b) BusyBox `chpasswd` - not in the image; (c) BusyBox `passwd` under a pseudo-terminal - scripting an interactive tool, fragile and not testable; (d) `mkpasswd` plus our own shadow write - the same write with an extra process and the password in another process's memory.

## Decision

1. **gatewayd ships in every image and every bench deploy, disabled.** The `pocketos` Buildroot package builds it (`ENABLE_GATEWAYD=1`) and installs `/usr/sbin/gatewayd`; `S70gatewayd` moves from `services/gatewayd/` into the rootfs overlay beside the other init scripts (U5: `S70`, as proposed). `tools/release/check_rootfs.sh` treats it as a service like the others (binary and init script together, executable, `DAEMON=` names the binary, one build stamp) and lists `/etc/default/gatewayd` as per-unit (never in an image). `deploy.sh` carries both and `deploy_unit.sh` stops gatewayd first and starts it last. Its only dependency is OpenSSL, already in the image (ADR-019, relation to ADR-017): a build dependency, not a package. With no `/etc/default/gatewayd`, `GATEWAYD_ENABLE=0` and nothing runs: ADR-019 rule 1 is unchanged.

2. **The persistent switch is `GATEWAYD_ENABLE` in `/etc/default/gatewayd`**, the variable rule 1 already names. It is per unit: no image ships it and no deploy writes it, so it survives upgrades and reboots exactly as `/etc/default/meshcored` does. The gateway's own state (`$POCKETOS_STATE_DIR/gatewayd/`: the key and the paired list) is untouched by every step here. A unit enabled by hand before this amendment stays enabled.

3. **sysd serves `companion.status`, `companion.set_enabled {enabled}` and `companion.set_password {password}`** (`docs/api/system.md`). It is the narrow control the owner asked for:
   - one service: the path of the init script and the binary are compiled in; the request names neither, nor a command, a path or another service;
   - one argument: a boolean (or, for the password, one string checked before anything runs);
   - fixed argv: `/etc/init.d/S70gatewayd start` or `stop`, executed directly (no shell, no `system()`), its output to `companion.log`;
   - the work runs in a child process, as `radio_setup` does, so sysd keeps answering and the shell never waits on a stop;
   - **enabling** is refused unless gatewayd and its init script are installed and root has a password (rule 9b; the same rule as gatewayd's, from the same source file, `gw_gate.c`). SSH is hardened first (decision 5). Then `GATEWAYD_ENABLE=1` is written (other lines kept), the script is run, and gatewayd must answer `gateway.status` within 10 s; otherwise the previous value is written back and the service stopped;
   - **disabling** is always accepted: `GATEWAYD_ENABLE=0` is written *before* the stop, so a stop that fails still leaves a unit that will not start the gateway at the next boot (fail closed).
   sysd's socket is root-only like every pocketipc socket; no gateway allowlist entry can reach it (ADR-019 rule 25 already excludes `system.*` changes; `tests/gatewayd_lint.sh` now also refuses the `companion.` prefix in gatewayd).

4. **The device password is set on the unit (U7)**: Settings > Companion > Device password, typed twice. sysd checks it (8 to 64 printable ASCII characters), hashes it with libxcrypt as SHA-512 crypt (`$6$`, a 16-character salt from `getrandom`), the library and scheme the image's sshd and BusyBox already verify, and writes root's password field in `/etc/shadow` under `lckpwdf()`: a temp file in `/etc`, 0600, fsync, rename, fsync of the directory; every other line and field unchanged. It then reads the result back through `gw_gate_password_set()`, the definition rule 9b uses, compiled into sysd from `services/gatewayd/gw_gate.c` so "a password is set" has one definition. "Configured" means what U7 proposed: root's `/etc/passwd` field is `x` and `/etc/shadow` holds a hash. Any other layout (a hash or an empty field in `/etc/passwd` itself, no shadow file) is refused, not rewritten.
   - The password crosses one root-only Unix socket, shell to sysd, as a Wi-Fi passphrase crosses to netd (ADR-003); Settings clears its fields before it waits for the answer; sysd forks the job and wipes its copy, the child wipes its copy after hashing; nothing logs it (lint). Doors stores no secret: the system stores its own hash, where `passwd` would.
   - **No current password is asked to change it.** The unit is already root for anyone at its unlocked screen (Terminal), so asking would protect nothing and would lock out an owner who forgot it.
   - What it is for afterwards: the serial console's login and `su`; after decision 5 not root over SSH.

5. **SSH is hardened, in the image and on every unit before it gets a password or a gateway** (issue #172):
   - Image: `apply_to_sdk.sh` patches the vendor `sshd_config`: `PermitRootLogin yes` becomes `PermitRootLogin prohibit-password`, and `verify_image.sh` refuses an image whose `sshd_config` permits root by password.
   - Units deployed over an older card keep their old file, so sysd hardens it as the first step of both jobs: every `PermitRootLogin` with an unsafe value becomes `prohibit-password` (one is added at the top when there is none), the result is checked with `sshd -t -f`, a copy of the original is kept once as `sshd_config.doors-orig`, the new file replaces the old by rename, and sshd is reloaded (`S50sshd reload`; sessions already open stay). A failure at any point stops the job before the password or the switch changes.
   - "Hardened" is: at least one `PermitRootLogin` before the first `Match`, and every one in the file is `prohibit-password`, `without-password`, `forced-commands-only` or `no`. A file sysd cannot judge (an `Include` line, or one it cannot read) is "unknown", and unknown is refused, never assumed safe. No sshd installed is "absent": nothing listens.
   - Consequence: once Companion is used, root signs in over SSH with a key only. The page says so before the button is tapped.

6. **The pairing candidate is part of `gateway.status`** on the local socket: `pair {state, left_ms, candidate {name, fingerprint}}` while a window is open, and `pair_closed`, why the last one closed. Settings is a one-shot pocketipc client, polled once a second like every Settings page, and cannot hold the subscription the `gateway.candidate` event needs (the reason the bench path cannot show the key first, issue #172). The candidate's fingerprint is its public key's hash; the token stays where it was, in `gateway.pair_begin`'s answer alone. Compatible (an added field; `docs/api/gateway.md` section 2).

7. **Settings > Companion** (the Settings half of U12): the gateway switch with its state in words; the device password; the paired phones with connection state and permissions, one page each with its permission switches and Remove; Pair a phone: a QR code drawn by LVGL from the URI (ADR-007 holds: no TLS and no gateway code in the shell), a countdown, Cancel, and - when a phone has presented the token - its name and key, a "keys match" confirmation that ALLOW waits for, the permissions it gets, REFUSE and ALLOW. Leaving the page, Back, Home or the app closing cancels an open window. The status-cluster mark of U12 is not part of this amendment.

## What does not change

ADR-019 rules 1 to 27 and amendment 1. The gateway is still off by default, closed until a phone is paired or a window opened on the unit, TLS 1.3 with both keys pinned, private sources only, scopes checked on every request, revocation immediate and durable. Rule 9b now has its mechanism; the gateway's own gate is unchanged and still fails closed by itself.

## Consequences

Needed now: this amendment; `services/sysd/sysd_companion.[ch]`; the `pair` fields in gatewayd's status; the packaging and deploy changes; the Settings pages; tests (`tests/sysd_companion_test.c`, `tests/companion_settings_integration_test.py`, the Settings suites, the release-check suites); `docs/api/system.md`, `docs/api/gateway.md` section 10, `docs/services/GATEWAYD.md`, `docs/apps/SETTINGS.md`.

Before a unit runs it (each with its own go-ahead, as ADR-019 says): the hardware gate of ADR-019 and the checks of this amendment below; an image build with `verify_image.sh`.

Risks accepted with this amendment:

- sysd can now write `/etc/shadow` and `/etc/ssh/sshd_config`. Bounded: fixed paths, fixed transformations, a child process, the result read back.
- An operator who signed in as root over SSH by password loses that the first time Companion is set up. Keys keep working; the page says so first.
- A new image no longer accepts root's password over SSH at all; bench practice is a key in `/root/.ssh/authorized_keys` (the deploy scripts already use keys: `BatchMode=yes`).
- The serial console asks for the device password once one is set. A forgotten password is recovered on the unit (Terminal: `passwd`) or by editing the card.
- Not verified on a unit: libxcrypt's hash at run time, the shadow write and `lckpwdf()`, `sshd -t` and the reload, S70gatewayd run by sysd, the QR code read from the panel (H10).

## Evidence

- Release rootfs facts above: VERIFIED on the build host (`~/work/unitb-v0.5.0/rootfs`, the 0.5.0 image's rootfs), not on a unit.
- BusyBox configuration without `chpasswd`: VERIFIED (build tree `.config` and the release rootfs).
- libxcrypt and OpenSSL in the image: DOCUMENTED (`docs/legal/manifest.csv`).
- sysd's child-job pattern: DOCUMENTED (`services/sysd/sysd_radio.c`).
- Everything at run time on a K230: ASSUMED until the hardware gate.

## Decision record

| Date | Decision | By |
| --- | --- | --- |
| 2026-11-25 | Proposed, with its implementation in a draft PR | AI engineering partner |
