# Companion: a self-hosted DOORS Relay (proposal)

Status: **proposal** for ADR-019 amendment 3 (Proposed, 2026-11-27). Nothing
is implemented. Figures marked ASSUMED are estimates to be measured; claims
about other projects are from their public documentation, to be checked
against a pinned version before any of them is used (AGENTS.md: no
dependency without a proposal).

## 1. Goals and non-goals

Goals:
- A paired phone reaches its unit when the local network does not let them
  meet (client isolation, different networks, mobile data).
- **Local first.** The relay is used only when the local path fails.
- ADR-019's trust model unchanged end to end:
  - TLS 1.3, mutual authentication, the existing P-256 identities, pinning;
  - physical pairing, scopes, revocation;
  - no clock.
- Self-hosted by the owner, reached by **outbound TCP 443** from both ends,
  **off by default**.

Non-goals:
- A cloud service run by Doors.
- Background push to the phone.
- Direct peer-to-peer over the internet (§6).
- Reaching anything on the unit but the gateway.
- Changing the gateway's API.

## 2. Architecture

```
  phone (any network)                relay (owner's host, :443)              unit (any network)
  ───────────────────                ───────────────────────────             ──────────────────
                                     ┌─────────────────────────┐
  app ──TLS 1.3, relay pinned,  ───► │ admission: device lists  │ ◄─── TLS 1.3, relay pinned,  gatewayd
        phone key as client cert     │ phones (fingerprints)    │      device key as client cert (control)
        "connect device_id"          │ splice two TCP streams   │
                                     │ sees ciphertext only     │ ◄─── TLS 1.3, relay pinned,  gatewayd
                                     └─────────────────────────┘      device key (data, per session)

  inside the spliced pair:  phone ═══ ADR-019 TLS 1.3, mutual pins, unchanged ═══ gatewayd
```

- **Control connection.** One per unit, opened by gatewayd while remote
  access is on. Over it the unit:
  - authenticates with its device key;
  - publishes the list of phones it admits (fingerprints), and pairing slots;
  - receives invitations.
- **Client connection.** Opened by the phone when it needs the unit and the
  local path failed. It authenticates with the phone's Keystore key and asks
  for a `device_id`.
- **Data connection.** For each admitted client connection, the relay sends
  the unit an invitation with a fresh 128-bit session cookie. gatewayd opens
  one new outbound connection, presents the cookie, and the relay splices it
  to the client's. Over the splice, the phone and gatewayd run ADR-019's TLS
  handshake as if on a LAN, gatewayd as server.

This is the session-invitation model of Syncthing's relay protocol (§9),
with admission added.

## 3. The relay protocol (sketch)

**Outer TLS** (both ends to the relay):
- TLS 1.3 only, ALPN `doors-relay/1`, on TCP 443. To a middlebox it looks
  like HTTPS.
- The relay presents a self-signed certificate. Its key is pinned (SHA-256
  of the SPKI), exactly as PAIRING.md §2: no CA, no dates, no names.
- Each client presents its own one-certificate chain: the unit its device
  key, the phone its Keystore key. The relay derives the fingerprint and
  never trusts a claimed identity.

**Frames:** length-prefixed JSON, as gateway frames, at most 4 KiB, with a
fixed set of types. Control connection:

| From | Type | Fields | Meaning |
| --- | --- | --- | --- |
| unit | `hello` | `proto`, `admit[]` (fingerprints, at most 8), `pair_slot?` | Register: replaces any earlier control connection of this device |
| unit | `admit` | `admit[]` | Replace the admitted list (sent on every pairing, revocation, `remote` scope change) |
| unit | `pair_slot` | `slot` (16 bytes, base64url) or null | A pairing window opened or closed (§4.3) |
| relay | `invite` | `cookie`, `client` (fingerprint), `via` (`admit` or `pair`) | A phone wants a session |
| unit | `decline` | `cookie`, `why` | No free slot, rate limit, or the phone is no longer admitted |
| both | `ping`/`pong` | `n` (nonce) | Liveness, every 25 s |

- **Client connection:** a single `connect {device, slot?}`. The relay
  answers `ready` and then splices, or `unavailable`. The answer is the same
  whether the unit is offline, unknown or does not admit this phone, so the
  relay is no oracle for device ids.
- **Data connection:** a single `join {cookie}`, then splicing.
- **Nothing in the protocol names a port, host or service** (rule 35).

## 4. gatewayd integration

### 4.1 A relay connector in the same loop

A new module `gw_relay.[ch]` in the one-thread `poll()` loop (rule 7):

- **Control connection state machine:**
  - resolve, connect, outer handshake, check the relay's pin, `hello`;
  - keepalive every 25 s; closed after 75 s of silence;
  - back-off from 1 s to 5 min with full jitter, reset by success or a
    change of address on the configured interface (netd).
- **Invitations:**
  - accepted only within the relay budget (at most 1 relay handshake
    pending and 1 relay session, inside ADR-019 §6's totals, so a LAN phone
    always finds a slot), and within §6's rate of 4 handshakes/s;
  - every other invitation is `decline`d at once.
- **After the inner handshake** of a relay session, the gateway itself
  checks that the client holds `remote`, and closes the session if not,
  whatever the relay admitted.
- **Data connections:**
  - outer TLS (`SSL` on the socket);
  - inner TLS on a BIO pair, which the connector pumps to and from the
    outer `SSL`;
  - the inner `SSL` is created from the same `G.ctx` with the same verify
    callback and pins as today's `listener_accept()` (`main.c:743-790`).
  - The only differences from a LAN session are the session's I/O and its
    peer label ("relay").
- **Admission:** the admitted list is computed from `paired.v1`: the paired
  clients that hold `remote`. It is sent at connect, and again on every
  change.

What does not change:
- **The listener** (rule 5).
- **Pairing**, and the allowlist with its scopes and per-method checks: a
  relay session is a session.
- **Rule 14**: a candidate (unknown key) is admitted only while a window is
  open and only through a `pair` invitation.

### 4.2 Configuration and control

- `/etc/default/gatewayd`: `GATEWAYD_RELAY=0|1`, `GATEWAYD_RELAY_HOST`,
  `GATEWAYD_RELAY_PORT` (443), `GATEWAYD_RELAY_PIN` (64 hex). Written by
  sysd for Settings, like the gateway switch (ADR-019 amendment 2).
- `gateway.status` gains:
  - `relay {state off|connecting|connected|failed, since_mono_ms, error}`;
  - a `via` per session (`lan` or `relay`).
- `gateway.clients` gains the scope `remote`; `gateway.scope` sets it, as
  it sets `vision`.
- **Settings > Companion** gains:
  - a REMOTE ACCESS card: the switch, the relay's host and short
    fingerprint, its state, and the sentence of rule 40;
  - a "Through the relay" switch per phone.
  - The relay is entered by scanning a `doors-relay:1?h=...&p=...&fp=...`
    QR code that the relay prints at install, with the unit's camera
    (ADR-007: the shell only decodes a QR string). Typing the host and
    fingerprint is the fallback.

### 4.3 Pairing through the relay

When a window opens with remote access on:
1. gatewayd sends `pair_slot {slot = HKDF-SHA256(token, "doors relay pair slot v1")[0..16]}`.
2. The pairing code (v1) gains two optional fields, `r=<host:port>` and
   `rf=<relay fingerprint, base64url>`. The app's parser reads only
   `h`/`p`/`fp`/`t`/`n` and ignores others (`PairingUri.kt`), so older apps
   still pair locally.
3. A phone that cannot reach `h:p` locally connects to the relay with
   `connect {device: fp, slot}`. The relay invites the unit with
   `via: pair`, at most one candidate at a time.
4. Inside the spliced TLS, everything is ADR-019:
   - the candidate presents the token (`gateway.pair`);
   - the panel shows the candidate's key;
   - a person compares and approves (amendment 2), or not.
5. The slot is withdrawn when the window closes. The relay learns no token
   and no decision.
6. A new pairing never includes `remote` (rule 33). The owner switches it
   on for that phone on the unit, if wanted.

## 5. Resource consumption

| Where | Cost | Class |
| --- | --- | --- |
| Unit, idle with remote on | One TCP connection and one outer `SSL` (tens of KB of RAM); a 25 s ping, about 0.3 MB/day | ASSUMED |
| Unit, per relay session | Two `SSL` objects instead of one; **two handshakes** per session (outer to the relay, inner end to end), each an ECDHE and ECDSA on P-256 on the one hart | ASSUMED; handshake cost is open item H2 |
| Unit, traffic | Status and RIFT: bytes per second. Vision preview: 5.4-6.6 KB pictures at 4-5/s, about 35 KB/s (Unit A, 2026-11-24, `M6-STATUS.md`). Double encryption at those rates is negligible | DOCUMENTED (rates); ASSUMED (cost) |
| Phone | Nothing in the background (no reconnect loop without the app in front, PAIRING.md §7). One more handshake per relay session | ASSUMED |
| Relay | Per session two TCP connections and a copy loop; a 1-vCPU host with 64-128 MB RAM serves tens of units | ASSUMED |

## 6. NAT traversal and reconnect

- **NAT.** Both ends connect outward, so the relay works through home NAT,
  carrier-grade NAT and most guest networks without traversal.
- **Direct paths are not attempted:**
  - TCP hole punching is unreliable;
  - UDP hole punching would need a datagram transport (DTLS or QUIC) in
    place of TLS 1.3 over TCP, which would change rules 10-13;
  - a later amendment could add it.
- **The unit's control connection:**
  - reconnects as in §4.1;
  - a change of network (netd) restarts it at once;
  - an established relay session survives a control reconnect (it is its
    own connection).
- **The phone, path selection:**
  1. With Wi-Fi, try the last LAN address, then the /24 probe (PAIRING.md §7).
  2. If no pinned answer within 1.5 s, or without Wi-Fi, start the relay
     path **in parallel**.
  3. The first session whose **inner** handshake verifies the pin wins.
  4. While on the relay, a Wi-Fi change triggers a LAN attempt. Success
     replaces the relay session ("a second connection from the same client
     replaces the first", PAIRING.md §7).
- **Resume:** unchanged. The journal cursor, `run_id`, `journal_id` and
  `boot_id` rules of PAIRING.md §7 do not care which path carried the bytes.
- **The app shows "On this network" or "Through your relay"**, and the
  unit's Settings shows the same per phone (rule 36).

## 7. Authentication, abuse and denial of service

**Who may use the relay:**
- **Units:** only device fingerprints the relay's operator enrolled
  (`doors-relay enroll <device_id>`, written to the relay's configuration).
  A stranger cannot use the relay as a free pipe.
- **Phones:** only those a connected unit admits.

**Relay limits (defaults, configurable):**
- per source address:
  - 10 new connections per second;
  - 64 connections not yet authenticated;
  - 10 s to finish the outer handshake and the first frame;
- per unit:
  - 1 control connection (a new one, once its key is proven, replaces the old);
  - 8 admitted phones and 1 pairing slot;
  - 1 spliced session, matching the unit's relay budget (R-T13). One
    remote phone at a time in v1; configurable later if measured to be safe;
  - 4 invitations per second;
- per session:
  - 90 s idle;
  - 256 KiB/s;
  - optionally a daily byte quota per unit;
- global caps on connections and memory.

**Unit side:** rule 38. Invitations are bounded by the gateway's own relay
budget and handshake rate. The relay cannot make the unit open more
connections than that budget, nor take the slots LAN phones use
(threat model R-T13).

**No amplification:** the relay never sends more than its peer sent, except
small control frames.

**What a hostile relay can do** is in the threat model, R-threats:
- deny service and read metadata;
- not decrypt, not impersonate, not pair, not raise a scope.

## 8. The server

- **Requirements:**
  - a host reachable on TCP 443: a small VPS, or a home server with port 443
    forwarded;
  - Linux;
  - one static binary and one configuration file;
  - its own P-256 key, generated at install. Its fingerprint and a
    `doors-relay:1?...` QR code are printed for the units;
  - no CA certificate needed. If 443 is shared with a web server, an SNI or
    ALPN pass-through (for example nginx `stream` with `ssl_preread`) sends
    `doors-relay/1` to the relay.
- **State:**
  - the enrolled device fingerprints (configuration);
  - the admitted lists, in memory only, re-sent by each unit at connect;
  - no secret but its own key.
- **Logging:** rule 39. Counters for operations.
- **Language: an owner decision.**
  - **Go** (recommended): the relay is the one component on the open
    internet, so memory safety matters most there. The standard library
    has TLS 1.3, client certificates and a pinning hook
    (`VerifyPeerCertificate`). One static binary, no third-party module.
    But it brings a new toolchain to the repository.
  - **C with OpenSSL**: the repository's own toolchain, and it shares
    gatewayd's frame code and test harness. But it is an internet-facing
    parser in C.
- **Tests:**
  - host suites with fake units and phones, run under the existing Python
    harness;
  - a fuzzer on the frame reader;
  - the TLS checks of THREAT_MODEL.md §5 for the outer layer.

## 9. Existing solutions compared

| | Model | Keeps the gateway TLS end to end | Only the gateway reachable | On the K230 | On Android | Clock | Self-hosted | Licence (upstream, to confirm) | Fit |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Tailscale + Headscale + DERP | WireGuard mesh, DERP relays over HTTPS | yes, inside WireGuard | **no**: IP reachability of the unit (sshd included); needs ACLs, and the image has no firewall | Go daemon (tens of MB RSS) or kernel WireGuard and TUN | the Tailscale VPN app, or an embedded library; one VPN per phone | node key expiry must be disabled | yes (Headscale, own DERP) | BSD-3-Clause | Poor: a VPN is broader than "the gateway only" |
| WireGuard + own relay/VPS | Hub VPN | yes | no, as above | kernel module or userspace | a VPN app | none | yes | GPL-2.0 (kernel), MIT (tools) | Poor, as above |
| Nebula | Overlay with lighthouses and relays | yes | no, as above | Go daemon | a VPN app | **certificates carry validity**: conflicts with the clockless rule | yes | MIT | Poor |
| ZeroTier | Overlay | yes | no, as above | C++ daemon | a VPN app | - | own controller and roots possible, complex | BSL 1.1 for recent versions (not open source in the OSI sense) | Poor |
| coturn (TURN, RFC 5766/6062) | Allocation relay, TCP via RFC 6062 | yes | yes | a TURN client in C (e.g. pjnath, heavy) | a TURN client | static credentials need none | yes | BSD-3-Clause | Fair: standard and mature, but heavy client side and credential management |
| frp / rathole | Reverse port forwarding | yes | only in secret/visitor modes; otherwise the gateway port faces the internet | a second daemon | a visitor client or a public port | none | yes | Apache-2.0 | Fair to poor |
| SSH reverse tunnel | Port forwarding | yes | needs a careful server-side configuration | autossh and a key on the unit | an SSH client in the app | none | yes | BSD (OpenSSH) | Poor: another credential and another daemon path |
| Syncthing relay (strelaysrv) | Blind relay, session invitations, device ids = certificate hashes | yes | yes | a protocol client in gatewayd | a client in the app | none | yes | MPL-2.0 | **Good model**; its servers admit anyone by design (no per-device lists), so either patch it or adopt only the protocol's shape |
| iroh relay | QUIC with relay fallback | the transport becomes QUIC, not ADR-019's TLS over TCP | yes | a Rust library | a Rust library | none | yes | MIT / Apache-2.0 | Poor: replaces the transport |
| **Minimal first-party relay** | Blind relay, invitations, unit-published admission | **yes** | **yes, by protocol** | a module in gatewayd; OpenSSL already linked | a transport in the app | none | yes, by design | Apache-2.0 (the repository's) | **Best fit**: smallest attack surface, exact revocation and scope semantics, no new daemon on the unit; the cost is first-party code to review |

**Recommendation:** a minimal first-party relay built on Syncthing's relay
model, with:
- admission published by the unit;
- pairing slots;
- a `remote` scope;
- outer TLS with the relay pinned.

Prefer no server code from elsewhere. If the owner wants to start from
existing code, the Syncthing relay is the closest, under MPL-2.0 as a
separate program.

## 10. Phases (each with its own go-ahead)

| Phase | What | Done when |
| --- | --- | --- |
| **0. Local paths first** | (a) Establish the SkullerudPark root cause with phone-side evidence (ADB on the Flip8: `ip neigh`, ping, the app's log, during an attempt, with a unit-side capture). (b) Option F1: the unit on the phone's hotspot. Today's app binds only to networks the phone *joined* (`WifiTransport`, `TRANSPORT_WIFI`), so a phone that *hosts* the hotspot is not handled (README H3/H12). Fix it by allowing connections to the phone's own tethering subnet only. (c) Option F2: the unit's own access point on `wlan1` (concurrent mode, `hostapd`, `udhcpd` on the image), as a separate ADR | Root cause recorded; F1 works on the Flip8; F2 decided |
| **1. Protocol and server** | §3 specified in `docs/api/relay.md`; the server; host suites with fake units and phones; fuzzer; outer-TLS checks; a run of the server under ASan if in C, or the race detector if in Go | Suites green; independent review of protocol and server |
| **2. gatewayd** | `gw_relay.[ch]`; BIO-pair inner TLS; `remote` scope; `gateway.status` relay state; sysd keys in `/etc/default/gatewayd`; Settings card; host integration against the real relay with a TLS phone (the existing Python harness); cross-build | Off by default verified; a revoked phone loses its relay session within a second; a relay that lies about admission gains nothing (tests) |
| **3. Android** | `RelayTransport` (default network, relay pinned with the existing `PinnedTrustManager` class); path selection (§6); the pairing code's `r`/`rf`; "Through your relay" in the UI; lint amended for a second pin of the same kind; JVM integration tests against the real relay and gatewayd | Local-first race tested; nothing falls back to unpinned anything |
| **4. Hardware gate** | Unit B on an isolating network; the phone on the same network, then on mobile data; pairing through the relay with the fingerprint confirmed on the panel; revocation; reboot; relay restart; Vision and PacketShark through the relay; the unit's handshake cost and frame time; a DoS run against the relay | PASS per part, with the owner present |
| **5. Release** | Independent security review of everything; documentation; the relay's install guide | Owner's acceptance |

## 11. Owner decisions this proposal needs

1. Accept, change or reject ADR-019 amendment 3.
2. The relay's language: Go (recommended) or C.
3. Whether a new pairing may ever include `remote` by default (proposed:
   never).
4. Whether the pairing code carries the relay (`r`/`rf`), so a first pairing
   can happen through the relay, or remote access is enabled only for
   phones first paired locally (simpler, and one less exposure).
5. Phase 0 before or alongside phase 1.

## 12. Files

New if accepted:
- `docs/decisions/ADR-019-amendment-3-relay.md`;
- this file;
- `docs/design/companion/RELAY_THREAT_MODEL.md`;
- `docs/api/relay.md` (phase 1).

The ADR index (`docs/decisions/README.md`) gains a row:
`| [019-A3](ADR-019-amendment-3-relay.md) | An optional self-hosted DOORS Relay (amends rule 5) | Proposed |`
