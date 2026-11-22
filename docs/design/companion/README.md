# DOORS Companion: design package (M1)

Status: the design package of M1. Since M2 the gateway core exists, host-tested and cross-built only (`docs/services/GATEWAYD.md`); still no Android project and no unit touched. Written 2026-11-21 against `origin/master` 085a089f.

DOORS Companion is an Android app for a paired Doors unit: RIFT messaging, PacketShark statistics and device status over the local Wi-Fi network or the phone's hotspot, through a new authenticated service on the unit, `gatewayd`.

## Documents

| Document | What it settles | Status |
| --- | --- | --- |
| [M0-PROPOSAL.md](M0-PROPOSAL.md) | Feasibility, what exists, what is missing, alternatives, the phased plan | Direction approved 2026-11-21 with eight conditions |
| [ADR-019](../../decisions/ADR-019-companion-gateway.md) | The gateway: service, transport, keys, pairing, what a client can never do | **Accepted 2026-11-21** |
| [ADR-016 amendment 1](../../decisions/ADR-016-amendment-1-companion.md) | Aggregate capture figures, HOLD/STOP, START by request and a tap | Proposed |
| [docs/api/gateway.md](../../api/gateway.md) | Protocol v1: envelope, versioning, scopes, every method and event | Draft |
| [PAIRING.md](PAIRING.md) | Identities, certificates without a clock, pairing, revocation, reconnect and resume | Draft |
| [THREAT_MODEL.md](THREAT_MODEL.md) | Assets, adversaries, threats, residual risks, required checks | Draft |
| [CONTRACTS.md](CONTRACTS.md) | What `gatewayd`, the app, the shell and PacketShark must each do | Draft |

## How the owner's conditions are met

| # | Condition | Where |
| --- | --- | --- |
| 1 | Separate supervised `gatewayd`, disabled by default | ADR-019 rules 1 to 9 |
| 2 | TLS 1.3, mutual authentication, strict pinning, safe without an RTC, no generic bypass | ADR-019 rules 10 to 14; PAIRING.md §2 |
| 3 | Physical approval, individual revocation, scopes, protected keys | ADR-019 rules 15 to 24; PAIRING.md §3 to §6 |
| 4 | RIFT: read nodes, channels, messages, activity; send user-initiated messages | gateway.md §7, §8; CONTRACTS.md §2 "Sending" |
| 5 | PacketShark: aggregates, HOLD/STOP, START with a tap, no raw packets | ADR-016 amendment 1; gateway.md §9 |
| 6 | Ownership boundaries preserved | ADR-019 rules 3, 4, 6; amendment 1 "What does not change"; CONTRACTS.md §4 |
| 7 | `companiond` stays separate | ADR-019 options (E); nothing here touches that branch |
| 8 | Clean worktree from `origin/master` | branch `docs/companion-gateway-m1` |

## What changed since M0

- **No TLS session resumption in v1.** M0 counted on it to bound handshake cost; dropping it makes revocation immediate and removes a cached trust decision. The cost moves to assumption H2.
- **Scopes are split**: `mesh_read` / `mesh_send`, `capture_view` / `capture_control`.
- **Mesh activity is in `mesh_read`** as one-second counts (the owner's condition 4). The receive log is not in v1.
- **Logs and crash reports are out of v1** (they were a `diagnostics` scope in M0); see U6.
- **`gateway.unpair`** added so a phone can remove itself.
- Certificates carry fixed dates and no decision reads them (M0 said only "ignore validity dates").

## Decisions (product owner, 2026-11-21; ADR-019 accepted the same day)

| Decision | Value | Where |
| --- | --- | --- |
| Gateway service | `gatewayd`, supervised, **disabled by default** | ADR-019 rules 1, 9a |
| TCP port | `47231` (subject to H4) | ADR-019 rule 9a |
| API namespace | `gateway.*`, protocol version 1 | `docs/api/gateway.md` |
| Device password | The gateway cannot be enabled, and does not listen, until a device password is configured | ADR-019 rule 9b |
| Android project | `C:\Android\DoorsCompanion` on the build host (not created) | CONTRACTS.md §2 |
| Application id | `app.doors.companion` | CONTRACTS.md §2 |
| Licence | Apache-2.0, as the repository (ADR-013) | CONTRACTS.md §2 |

## Unresolved decisions

For the product owner. Each blocks the milestone named.

| # | Decision | Recommendation | Blocks |
| --- | --- | --- | --- |
| U2 | Accept ADR-016 amendment 1 | Accept | the capture group only |
| U4 | The shell hook that lets an app serve methods on `shell.sock`: not designed, and an addition to an existing API | Design it as the first step of the capture milestone; alternative: a dedicated socket owned by the app | the capture group |
| U5 | The init order `S70` (the scope names were accepted with ADR-019 rule 24) | As written | product integration |
| U6 | Logs and crash reports to a phone (`system.logs`, `system.crashes`) | Not in v1; later behind its own scope, after checking what the logs can contain | nothing |
| U7 | **How a device password is set.** Rule 9b requires one, and Doors has no way to set it today (only `passwd` on a root console). Also: what exactly counts as configured (a non-empty root password hash is proposed) | A Settings page, designed with its own decision; until then `passwd` by hand on the bench | product integration (not the host-side gateway core) |
| U8 | Pairing without a camera (a typed code) | Not in v1 | nothing |
| U9 | A discovery responder on the unit (DNS-SD) | Not in v1; the phone probes its subnet | nothing |
| U10 | An independent security review before use on networks the owner does not control (ADR-017's precedent) | Recommended before product integration; not required for bench work | product integration |
| U11 | Listening on `eth0` as well as `wlan0` | `wlan0` only in v1; the interface is one setting | nothing |
| U12 | A status-cluster mark while a client is connected, and the Companion page in Settings: both are Design System additions | Specify with mockups at product integration | product integration |
| U13 | Where the app's source is versioned: its own repository, or a directory of this one. Min SDK 29 | Its own repository; 29 | Android foundation |
| U14 | `system.reboot` from a phone | Not in v1 | nothing |

## Hardware-dependent and unverified assumptions

Everything about the repository in this package is DOCUMENTED (read from source and docs). Nothing is VERIFIED on a unit. These are ASSUMED and need a check; none was performed.

| # | Assumption | How to check | Needs a unit |
| --- | --- | --- | --- |
| H1 | The image's OpenSSL 3.4.1 has TLS 1.3 and P-256 (ECDSA and ECDHE) enabled | **Partly checked 2026-11-21, read-only:** one SDK sysroot on the build host (`~/work/neon-p98/f398ecb/sysroot`, OpenSSL 3.4.1) defines none of `OPENSSL_NO_TLS1_3`, `OPENSSL_NO_EC`, `OPENSSL_NO_ECDSA`, `OPENSSL_NO_ECDH` in `openssl/configuration.h`. **2026-11-21, M2:** the v0.4.0 release build's sysroot says the same, and `gatewayd` cross-builds and links against its `libssl.so.3`. Not checked: the library's behaviour on a unit | the rest: yes |
| H2 | A full mutual P-256 handshake is cheap enough on the one hart that the shell keeps its frame time, with no resumption | Measure handshake time and shell frame time during a connect and during a flood | yes, with go-ahead |
| H3 | The unit reaches, and is reached by, a phone whose hotspot it joined (no client isolation between hotspot host and client) | Join a hotspot, connect | yes |
| H4 | TCP 47231 is unused on the image | `netstat -ltn` on a unit, read-only | yes (read-only) |
| H5 | The kernel's random source is ready when the key is first generated, on a board with no RTC and little boot entropy | As ADR-017's entropy check | yes |
| H6 | `$POCKETOS_STATE_DIR` survives a power cut after temp-flush-rename | As ADR-017's state-directory check | yes |
| H7 | A process that dropped every capability can keep accepting on a socket it bound before, and can still open the root-owned pocketipc sockets | VERIFIED for `pos-capture`'s open packet socket only; check a listener and a Unix connect | yes (or host) |
| H8 | The Wi-Fi driver's power saving (IPS on, LPS max) does not drop an idle TLS connection inside the 30 s ping | Observe a quiet session for an hour | yes |
| H9 | Android Keystore P-256 keys work for TLS 1.3 client authentication on min SDK 29 and accept the fixed certificate dates | Instrumented test on a phone and an emulator | no |
| H10 | The pairing URI (about 150 characters) is readable as a QR code on the panel at a comfortable size | Simulator screenshot, then the panel | simulator first |
| H11 | `/run/pocketos` is not world-accessible on a unit (init scripts create it before the library's 0770 applies) | `ls -ld` on a unit, read-only | yes (read-only) |
| H12 | Android routes a socket bound to a hotspot's or an internet-less Wi-Fi network as expected on the owner's phones | Manual test | no |

## Remaining blockers before implementation

1. ~~ADR-019 accepted~~ done 2026-11-21.
2. ~~H1~~ settled as far as a build can: `gatewayd` cross-builds against the v0.4.0 sysroot (M2). Its behaviour on a unit is hardware gate 1.
3. ~~Names~~ settled with the acceptance (service, port, namespace, scopes).
4. U13 settled before the Android project is created.
5. U2 and U4 before any capture work; they do not block the gateway core, status or RIFT.

## Plan after M1

As M0 §11, unchanged in order: gateway core (host only) → Android foundation → RIFT read → RIFT send → hardware gate 1 → PacketShark tier 1 → product integration. Each hardware gate and each image change needs its own go-ahead.
