# gatewayd: the Companion Gateway (M2 core)

Status: **host-tested and cross-built only.** Built by name (`make gatewayd`), not in `make all`, not in any image, not in the rootfs overlay. **No part of it has run on a K230, and no phone app exists yet.** Design: `docs/decisions/ADR-019-companion-gateway.md` (accepted 2026-11-21); protocol: `docs/api/gateway.md`; pairing: `docs/design/companion/PAIRING.md`.

gatewayd is the one Doors service that listens on the network. A paired phone reaches it over TLS 1.3 on TCP 47231 and may call an allowlist of methods; gatewayd answers them as a pocketipc client of the services that own the data. It owns no radio, no mesh key, no Wi-Fi credential and no capture.

```
 phone ── TLS 1.3, both keys pinned ──▶ gatewayd ──┬─ sysd      system.info, system.status
                                          │        ├─ netd      wifi.status
 Settings / doors ── gatewayd.sock ───────┘        ├─ radiod    radio.status
 (root only: pair, decide, scopes, revoke)         └─ meshcored mesh.status
```

## What M2 contains

| Part | File | What it does |
| --- | --- | --- |
| Service loop | `services/gatewayd/main.c` | One thread; the listener; sessions; the local control API; limits and deadlines |
| TLS | `gw_tls.c` | The only file that calls OpenSSL's key and certificate APIs: TLS 1.3 only, the pin check, the identity file |
| Pairing | `gw_pair.c` | The state machine: window, one candidate, one-time token, three failures, the decision. Pure, clock passed in |
| Allowlist | `gw_policy.c` | The method table, the five scopes, request validation, result filters |
| Paired clients | `gw_store.c` | `paired.v1`: load, save, find |
| Password gate | `gw_gate.c` | ADR-019 rule 9b |
| Framing | `gw_frame.c` | The bounded frame reader |
| Init | `services/gatewayd/S70gatewayd` | Supervised start and stop; `GATEWAYD_ENABLE=0` |

Served to a paired client in M2: `gateway.hello`, `gateway.ping`, `gateway.unpair`, and with the `status` scope `system.info`, `system.status`, `wifi.status`, `radio.status`, `mesh.status`. `gateway.pair` is for a pairing candidate only. `capabilities` is `["status"]`.

**Not in M2** (each returns "unknown method" today): `gateway.subscribe` and every event except `gateway.scopes` and `gateway.revoked`; the mesh group (`mesh_read`, `mesh_send`); the capture group (`capture_view`, `capture_control`; ADR-016 amendment 1 is still proposed). All five scopes are defined, granted, stored and changed now, so later milestones add table rows and not a permission model. `gateway.hello` does not yet carry `model` or `api`.

## When it listens

Never by default. Every one of these must hold, and each is re-evaluated when it changes and every 5 s:

| Condition | Otherwise `gateway.status` says |
| --- | --- |
| The identity file is present and a P-256 key only root can read | `identity_fault` |
| The paired list parses to its last byte | `state_fault` |
| The unit's root account has a password hash (`/etc/passwd`, `/etc/shadow`) | `password_required` |
| A client is paired, or a pairing window is open | `no_clients` |
| The interface (`--interface`, default `wlan0`) has an IPv4 address | `no_address` |
| That address is private, link-local or loopback | `public_address` |

Losing any of them closes the listener and every session. It binds that one address, never the wildcard, and closes connections from any other kind of source address before TLS.

**The password gate reads only whether root has a password hash.** Root's entry in `/etc/passwd` must hold a hash itself, or say `x` with a hash in `/etc/shadow`. An empty field in either place, a locked field (`!`, `*`, or a hash behind `!`) and an unreadable file all count as no password; an empty field in `/etc/passwd` counts as none whatever the shadow file holds. Doors has no way to set a device password yet (open item U7): on the bench it is `passwd` on a root console.

## Trust

- TLS 1.3 only, ciphersuites of TLS 1.3, groups P-256 and X25519, signatures `ecdsa_secp256r1_sha256`. No tickets, no session cache, no early data, no renegotiation, no compression. The settings are read back after they are made, and a finished handshake is checked again for version 1.3, not resumed, and the admitted key.
- The peer must send exactly one certificate holding a named-curve P-256 key. Its fingerprint is the SHA-256 of the SubjectPublicKeyInfo, built from the curve point's coordinates, so a key has one fingerprint however it was encoded. A key given by explicit curve parameters is refused.
- That fingerprint is admitted only if it is in the paired list, or a pairing window is open and nobody else is at the door. Nothing else is consulted: no CA, no chain, no date, no name. The unit's own certificate is made at start with fixed dates (1970 to 9999) and is only the container for its key.
- There is no option, environment variable or build flag that accepts another key. `tests/gatewayd_lint.sh` holds the text of that.
- Scopes are looked up on every request, so a reduced scope or a revocation holds from the next request.

## State

`$POCKETOS_STATE_DIR/gatewayd/` (0700):

| File | Holds | Rules |
| --- | --- | --- |
| `identity.v1` | The unit's private key, PEM, **not encrypted** (ADR-019 rule 15) | 0600; created only when the directory holds no paired list; never written over; a missing, damaged, readable-by-others or non-P-256 file is `identity_fault`, and nothing is regenerated |
| `paired.v1` | Per client: fingerprint, scopes, time, name. No secret | 0600; temp file, fsync, rename, fsync of the directory; damage is `state_fault`, never an empty list |

Replacing the identity is deliberate and manual: stop the service, remove both files, start it. Every phone then has to forget the unit and pair again.

## Pairing on the bench (until the Settings page exists)

The local socket is root-only, so these are actions on the unit. The token is in the URI: do not paste it anywhere but the phone.

```sh
doors call gatewayd gateway.pair_begin        # prints the doors-pair: URI; 120 s
# the phone connects and presents the token; then, on the unit:
doors call gatewayd gateway.subscribe         # (a client that stays connected sees gateway.candidate)
doors call gatewayd gateway.pair_decide allow=true
doors call gatewayd gateway.clients
doors call gatewayd gateway.revoke client=<64 hex>
```

`gateway.pair_decide` takes `scopes` as an array, which `doors call` cannot express; without it a client gets `status`, `mesh_read`, `mesh_send`. The Settings page is the real client of this API (`docs/design/companion/CONTRACTS.md` §3).

## Limits

8 paired clients; 2 sessions at once (a second connection of the same client replaces its first); 8 handshakes in progress and 4 new ones a second; 64 KiB per request frame; 256 KiB per reply and of unread output; 40 requests at once then 20 a second per client (code 10 beyond); 10 s to finish a handshake and 10 s more to send `gateway.hello`; 90 s idle; 2 s for a connection that is being ended to take its last message. An upstream call has 100 ms to connect and 300 ms to answer, and during it the service serves nobody else; a service that failed to answer is not asked again for 1 s (code 9 at once meanwhile).

The service runs at nice 5, and does not start at all unless every capability was dropped and read back as dropped.

## Logging

`gatewayd.log` in the Doors log directory: lifecycle lines only. A client is named by the first 16 hex digits of its fingerprint and its source address. Never a token, a pairing URI, a client's name, a request, a result or key material.

## Building and testing

```sh
make gatewayd            # services/gatewayd/gatewayd; needs OpenSSL 3 and cJSON
make gatewayd-test       # unit suites (plain, ASan/UBSan), fuzzer, the real service
                         # against TLS clients (plain, ASan/UBSan), the lint
make CC=riscv64-unknown-linux-gnu-gcc gatewayd      # with the SDK's host/bin on PATH
make ENABLE_GATEWAYD=1 install                      # also installs it and S70gatewayd
```

The service suite runs a build with `-DGATEWAYD_TEST_HOOKS=1`, the only one that accepts `--test-listen`, `--test-port` and `--test-shadow`; the shipped binary does not contain those options (the lint checks its strings).

## Evidence

Classes as in AGENTS.md. Measured 2026-11-21 on the build host (x86-64, WSL Ubuntu 22.04, GCC 11.4, OpenSSL 3.0.2) unless it says otherwise.

| Claim | Class |
| --- | --- |
| Pure modules, identity handling: 528 checks | VERIFIED host, plain and ASan/UBSan (`tests/gatewayd_unit_test.c`) |
| Frame reader, request parser, paired-list loader, gate: 200,000 fuzzed inputs, no fault | VERIFIED host, ASan/UBSan (`tests/gatewayd_fuzz.c`) |
| The real service against TLS clients: gate, closed until asked, pairing, failed handshakes (no certificate, TLS 1.2, RSA, P-384, unknown key, two certificates, plaintext), no resumption, scopes, revocation, malformed and oversized frames, limits, upstream failures, restart, damaged and missing state, log contents: 190 checks | VERIFIED host, plain and ASan/UBSan (`tests/gatewayd_e2e_test.py`) |
| The fingerprint equals the SHA-256 of the SubjectPublicKeyInfo as the `openssl` tool writes it | VERIFIED host (unit and service suites) |
| riscv64 build: warning-free with `-Werror`, Xuantie GCC 14.1.1, against the v0.4.0 release build's sysroot; links `libssl.so.3`, `libcrypto.so.3`, `libcjson.so.1`, all present in that build's target tree | VERIFIED (cross-build only) |
| The v0.4.0 sysroot's OpenSSL is 3.4.1 with none of `OPENSSL_NO_TLS1_3`, `_EC`, `_ECDSA`, `_ECDH` defined | VERIFIED (headers); the library's behaviour on a unit is not |
| Anything on a K230: handshake cost on the one hart, capabilities dropped with a listening socket, the entropy source at first start, the state directory across a power cut, `/etc/shadow` as the image has it, TCP 47231 free, Wi-Fi power saving and idle sessions | **not tested** (`docs/design/companion/README.md`, H2 to H8, H11) |
| A phone client | **does not exist** |

Not tested on the host either: the 10 s handshake and hello deadlines, the 90 s idle timeout and the 120 s window in real time (the pairing deadlines are tested with the state machine's own clock); an address change of the interface; `S70gatewayd` beyond parsing and the disabled default.

## Independent review (2026-11-21)

A second reviewer, given the code and the specification and not the author's conclusions, read `services/gatewayd` and `core/pocketipc` for authentication bypass, pairing and revocation, authorization, memory safety, key handling and fail-closed behaviour. It built and ran nothing.

**No blocker:** no path was found by which a peer completes a handshake, or has a request processed, without a pinned key or without being the one candidate inside an open window.

Confirmed weaknesses, all fixed before the first commit, each with a test where a host test can show it:

| Finding | Fix |
| --- | --- |
| A connection that was being ended while its peer did not read stayed registered for reading: the loop spun on the one hart, and a revoked client kept its slot for up to 90 s (rule 23) | An ending connection is taken off the read set and has 2 s to drain; a revoked client is closed in the revoke call itself |
| A candidate that left or timed out counted as a failed token, so three TCP connections from anyone closed the owner's pairing window (rule 21 says wrong tokens) | Only a presented wrong token counts |
| After a failed save the list on storage and the list in memory could differ and were never reconciled; a refused pairing could reappear after a restart | A dirty flag, a retry every 5 s, `store_dirty` in `gateway.status`, and a rewrite after a rolled-back pairing |
| The capability drop was attempted, not enforced (rule 8) | The service exits unless the drop succeeded and reads back as empty |
| A paired client repeating a request to a hung service could hold the loop, and with it revocation, for many seconds | 1 s back-off per service after a failure |

Smaller points fixed with them: OpenSSL no longer loads `openssl.cnf`; certificate compression is switched off where the library has it; a numeric range is checked before conversion; the listener closes at once when the last client unpairs; refused handshakes log at debug level only; the output queue holds to its documented size; nice 5 is set; the password gate reads `/etc/passwd` as well as `/etc/shadow`.

Left as they are, by decision: the shared handshake budget (above), no check of file ownership inside the 0700 root directory, and the synchronous upstream call itself, which the mesh milestone replaces.

The reviewer could not assess, and nobody has: anything on the unit (the library as built for the image, the kernel's handling of the capability drop, the image's passwd and shadow layout, the runtime directory's mode), and what the real services return for `radio.status` and `mesh.status`, which are passed whole apart from the scrub of secret-named fields.

## Known limits of M2

- Upstream calls block the loop for up to 400 ms each. Acceptable for status reads by two clients; the mesh group needs a non-blocking upstream client first.
- IPv4 only.
- A phone that is already paired and scans a new code connects as itself, not as a candidate; changing its scopes is `gateway.set_scopes`.
- While a pairing window is open, someone on the network can hold the door (one candidate at a time, 30 s without a token, then again) and so keep the owner's phone out for that window. It cannot pair, and it cannot close the window: only three presented wrong tokens do.
- The 8 handshake slots and the 4-a-second budget are shared by all sources. One host on the network can keep them busy and keep phones out for as long as it tries (THREAT_MODEL T4); sessions already open are not affected.
- When the paired list cannot be written (a full or read-only card), a revocation, a scope change or an unpairing is in force in memory at once, `gateway.status` reports `store_dirty`, and the write is retried every 5 s. If the service restarts before a retry succeeds, the old list on storage returns.
- The service stays uid 0 with every capability dropped and `no_new_privs` set (ADR-019 rule 8).
