# ADR-019: An authenticated Companion Gateway (`gatewayd`)

Status: **Accepted** by the product owner, 2026-11-21, as written. The direction had been approved the same day with eight conditions, recorded under "Terms of the approval"; this ADR turns them into rules. **Acceptance of the design does not mean an implementation is validated**: nothing is implemented, nothing has run on a unit, and the checks of `docs/design/companion/THREAT_MODEL.md` §5 and each hardware gate still need their own go-ahead. ADR-016 amendment 1 is a separate decision and remains proposed.
Date: 2026-11-21
Deciders: product owner (final), AI engineering partner (author)

Companion documents: `docs/api/gateway.md` (the protocol), `docs/design/companion/PAIRING.md` (pairing, revocation, reconnect, certificates), `docs/design/companion/THREAT_MODEL.md`, `docs/design/companion/CONTRACTS.md` (what each side must do), `docs/design/companion/README.md` (open decisions and assumptions), `docs/decisions/ADR-016-amendment-1-companion.md`.

## Context

DOORS Companion is an Android app for a paired Doors unit: RIFT messaging, PacketShark statistics and device status over the local Wi-Fi network or the phone's hotspot (`docs/design/companion/M0-PROPOSAL.md`).

Binding constraints:

- Services own I/O; apps and the shell reach them over pocketipc (ADR-002). The shell links no network-facing parser and no crypto library (ADR-007).
- pocketipc has no authentication: access is the socket's file mode, and every Doors process is root (`docs/ARCHITECTURE.md`). `docs/api/system.md` says this stops being sufficient the moment an API leaves the filesystem.
- No Doors component may persist a secret except under an ADR (`docs/ARCHITECTURE.md`; ADR-003 for Wi-Fi, ADR-017 for peerd's identity).
- The board has no clock that survives a power cut; the wall clock reads 1970 until NTP answers (ADR-007, ADR-008).
- Packet capture is bound by ADR-016.
- The image carries OpenSSL 3.4.1 and cJSON; it carries no libsodium, no mDNS responder and no firewall. Linux sees one hart.

## Terms of the approval (product owner, 2026-11-21)

1. A separate supervised `gatewayd`, disabled by default.
2. TLS 1.3, mutual authentication, strict public-key pinning; certificate handling that is safe without an RTC; no generic TLS verification bypass.
3. Pairing requires physical approval on the unit; individual client revocation, scoped permissions, protected private keys.
4. RIFT v1: read nodes, channels, messages and activity; send user-initiated messages.
5. PacketShark v1: aggregate statistics and HOLD/STOP; remote START only with physical confirmation on the unit; no raw packet streaming.
6. The ownership boundaries of RIFT, PacketShark and pocketipc are preserved.
7. The `companiond` prototype stays separate.
8. Work happens in a clean worktree from `origin/master`.

## Options

| | A. `gatewayd`, allowlisted API over pinned mutual TLS | B. TCP in each service | C. Gateway in the shell | D. SSH forwarding of the Unix sockets | E. Extend `companiond` |
| --- | --- | --- | --- | --- | --- |
| Network parsers | one process | four | in the panel's process | sshd | one, unauthenticated by upstream design |
| Per-method control | yes | per service | yes | none: the whole root API | no |
| Against | a new service | hostile input inside the radio and Wi-Fi owners | ADR-002, ADR-007 | `shell.key` and `system.poweroff` to anyone with the tunnel | cannot be authenticated without breaking its client |

## Decision

**Option A.**

### 1. Service

1. `gatewayd` is a supervised service (`pos-supervise`, an `S??` script), **disabled per unit by default** (`GATEWAYD_ENABLE=0` in `/etc/default/gatewayd`).
2. It is the only Doors process that accepts network connections for this purpose, the only one that links OpenSSL for a listener, and the only parser of Companion protocol input.
3. It owns: the listener, the device key, the list of paired clients, an in-memory message journal. It owns nothing else. It opens no device node, no radio, no capture socket; it holds no mesh key and no Wi-Fi credential.
4. It reaches meshcored, sysd, netd, radiod and the shell **only as a pocketipc client**, like RIFT and `doors`. It does not change what any of them own or store.
5. **Closed until asked.** No socket is bound unless at least one client is paired or a pairing window is open. It binds the address of one configured interface (default `wlan0`), never the wildcard address, and accepts only loopback, RFC 1918 and link-local sources.
6. It is an **allowlist, not a proxy**: every method a client may call is an entry naming the upstream method, the scope it needs, its parameter schema and the fields of the result that are passed on (`docs/api/gateway.md`). A method without an entry does not exist. No `shell.*` method is ever an entry; the only methods the gateway calls on the shell's socket are the four `capture.*` methods of ADR-016 amendment 1.
7. One thread, one `poll()` loop, non-blocking I/O. It reads its upstream connections continuously and queues per client, so a slow client cannot make pocketipc's backpressure rule fire against another service.
8. It drops every capability and sets `no_new_privs` before it accepts a connection. It remains uid 0 until Doors has per-service users.
9. It logs lifecycle lines only: never message text, a node name, a key, a token, or an address other than a connecting client's.
9a. Names: the service is `gatewayd`; its protocol and local control API are `gateway.*`, protocol version 1; it listens on **TCP 47231** (subject to the port being free on the image, `docs/design/companion/README.md` H4).
9b. **No device password, no gateway.** The service does not listen, and cannot be enabled from Settings, while the unit's root account has no password set: a unit whose sshd would admit anyone on the network must not also be made a normal member of that network by this service. The service reports `password_required` and stays closed. How a device password is set on a unit is not defined by Doors today (ADR-016 calls it "a device password, which this app makes more pressing"); that mechanism is a precondition of product integration, not of this ADR's host-side work.

### 2. Transport

10. **TLS 1.3 only**, on the image's OpenSSL. No earlier version, no 0-RTT, **no session tickets and no resumption in v1**: every connection performs the full mutually authenticated handshake, so a revocation holds from the next connection without a ticket to invalidate.
11. **Both sides present a certificate; each side accepts exactly the keys it has pinned.** A certificate is a container for a public key. The decision is: one certificate; an ECDSA P-256 key; the SHA-256 of its SubjectPublicKeyInfo equals a pinned value, compared in constant time. The TLS handshake itself proves possession of the private key.
12. **Validity dates, names and issuers are not part of the decision,** and are fixed values (`PAIRING.md` §2) so that nothing depends on either clock. This is not a relaxation of public PKI validation: no CA is trusted at any point, so there is no chain whose dates could be checked. The pin is the whole of the trust decision and expiry is replaced by revocation.
13. **No bypass.** There is no option, flag, build setting, environment variable or debug mode that accepts an unpinned key, disables peer verification, or falls back to another protocol. `SSL_VERIFY_NONE` and an accept-all verifier appear nowhere in the service or the app; a lint on each side holds that (`CONTRACTS.md`).
14. The one unpinned case is the **pairing candidate**: while a pairing window is open on the unit, one client with an unknown key may complete the handshake and may call only `gateway.hello` and `gateway.pair`. Outside a window an unknown key is disconnected when the handshake ends, before any application byte is read.

### 3. Keys

15. Device key: ECDSA P-256, generated from the kernel's random source through OpenSSL when the service is first enabled. Stored as `$POCKETOS_STATE_DIR/gatewayd/identity.v1`, 0600 in a 0700 directory, written once by temp-flush-rename. Never returned over any API, never logged, wiped from memory on exit. **Not encrypted at rest**: as ADR-003 and ADR-017, encryption waits for a device-bound key store.
16. It is created only when the `gatewayd/` state is absent. A missing key beside an existing paired list is a fault (`identity_fault`): the service stays closed and regenerates nothing. Replacing the key is an explicit, logged operator action, and unpairs every client.
17. Client key: ECDSA P-256 generated in the Android Keystore, non-exportable, hardware-backed where the phone offers it. One key per app install.
18. The paired list (`paired.v1`, 0600) holds, per client: the key's fingerprint, a name, the scopes, the pairing time by the monotonic boot count and by wall clock if set. It holds no secret. At most 8 clients.
19. The **device id** clients see is the device key's fingerprint; where a person compares fingerprints, sixteen hex digits in groups of four are shown (ADR-017 §2).

### 4. Pairing, approval and revocation

20. Pairing starts **only on the unit**: from Settings, or from the local `gatewayd.sock` for the bench. Nothing a network peer sends can open a pairing window.
21. A window lasts at most 120 s, shows a QR code carrying the address, the device fingerprint and a 128-bit one-time token, admits one candidate at a time, and closes after one success, three failed tokens, or its time.
22. A candidate that presents the token is **not paired until a person approves it on the unit's panel**, where its name and fingerprint are shown and its scopes are chosen. No approval within the window is a refusal.
23. Each client is revoked individually on the unit. Revocation removes the pin, closes a live session of that client at once, and is durable before it is acknowledged.
24. Scopes are granted per client at approval and can be reduced later on the unit; a client cannot raise its own. v1 scopes: `status`, `mesh_read`, `mesh_send`, `capture_view`, `capture_control` (`docs/api/gateway.md`).

### 5. What a client can never do in v1

25. No method that changes the node, the radio, Wi-Fi, storage or power is an entry: `mesh.set_name`, `mesh.set_path_hash`, `mesh.channel_add/remove`, `mesh.node_remove/reset_path`, `mesh.advert`, `mesh.discover`, `mesh.remote_*`, `mesh.app_send`, every changing `wifi.*`, `radio.configure/send*/set_enabled`, `storage.*` changes, `radio_setup.start`, `system.reboot`, `system.poweroff`, and every `shell.*`.
26. No secret crosses the gateway outward: meshcored already withholds channel keys and the identity key, netd withholds passphrases, and the gateway asks for none.
27. `mesh.send` is for messages a person sent. The gateway rate-limits it per client and transmits a repeated `client_msg_id` once.

### 6. Limits (v1)

8 paired clients; 2 concurrent sessions; 4 handshakes per second and 8 pending connections; inbound frame 64 KiB; outbound queue 256 KiB per client, after which snapshot events are coalesced and then the client is dropped; ping 30 s, idle timeout 90 s; 20 sends per minute per client; journal 512 messages, memory only.

Revisit when: Doors gets per-service users (then the gateway gets its own) or a device-bound key store (then the key is sealed); a client must change the node; pairing without a camera is wanted; a discovery responder is wanted; the service is to be exposed beyond a local network (not intended).

## Relation to accepted ADRs

Reviewed against every accepted ADR on 2026-11-21. No accepted rule is contradicted; three are touched and say so here.

- **ADR-002** (services own I/O, apps use pocketipc): followed. The gateway is a service and a pocketipc client; no app or shell code opens a network socket for it.
- **ADR-003 and `docs/ARCHITECTURE.md`** ("no other Doors component may persist a secret"): **this ADR adds one exception**, the gateway's private key (rule 15), under the same file-permission protection and the same limits as ADR-003 and ADR-017 §2. That sentence in `docs/ARCHITECTURE.md` names it since the acceptance. The paired list holds no secret.
- **ADR-007** (no network parser and no crypto library in the shell): followed. Settings talks to `gatewayd.sock`; the QR code is drawn by LVGL from a string.
- **ADR-016** (packet capture): not changed by this ADR. Everything a client may see or do of a capture is decided in ADR-016 amendment 1, which is proposed separately; without it the gateway has no capture entry.
- **ADR-017** (peerd): independent. peerd's UDP 47230 and this service's TCP 47231 do not collide. ADR-017's terms asked for established constructions and no custom handshake, and named libsodium for peerd; TLS 1.3 is an established construction with no first-party handshake, and uses a library already in the image, so no dependency is added (AGENTS.md: no dependency change without a proposal).
- **ADR-005** (names): new names carry neither product name; state stays under the existing `$POCKETOS_STATE_DIR`.
- **ADR-013** (Apache-2.0): the service is first-party under it; the Android app is proposed under the same licence (`docs/design/companion/README.md`).
- ADR-001, 004, 006, 008, 009, 010, 011, 014, 018: not touched. ADR-012 and ADR-015 are proposed and unrelated.

## Consequences

Needed now (documentation): this ADR, the protocol, the pairing and threat documents, ADR-016 amendment 1.

Needed for implementation, each a later step with its own go-ahead: `services/gatewayd`, a local `gateway.*` control API, host tests with fake services, a fuzzer for the frame and parameter parsers, sanitizer runs, `tests/gatewayd_lint.sh`; a Settings page (QR, paired clients, scopes, revoke); a status-cluster mark while a client is connected; an init script, install lines and release checks; the hardware gate.

Risks accepted with this ADR:

- A defect in `gatewayd` after authentication is a root compromise, because every Doors process is root. Authentication precedes all first-party parsing, and the pre-authentication surface is OpenSSL's TLS 1.3 handshake.
- The device key and the paired list are protected by file permissions only. Whoever can read the card can impersonate the unit to its phones until each phone forgets it. They cannot impersonate a phone.
- A paired phone with `mesh_send` transmits on the air as this node. That is the feature; the rate limit and the scope bound it.
- sshd on the image accepts root by password on the same network (`docs/hardware/T-DISPLAY-K230.md`). This ADR does not cause it, and does not fix it; a device password becomes more pressing (ADR-016 says the same).
- A phone keeps message history that the unit, by meshcored's design, does not.

## Evidence

Classes as in AGENTS.md.

- No authentication in pocketipc, all processes root, socket mode 0660: DOCUMENTED (`docs/api/pocketipc.md`, `docs/ARCHITECTURE.md`, `core/pocketipc/pocketipc.c`).
- meshcored and netd return no key or passphrase: DOCUMENTED (`docs/api/mesh.md`, ADR-003, `services/meshcored/api.c`).
- OpenSSL 3.4.1 and cJSON in the image; no libsodium, no mDNS, no firewall: DOCUMENTED (`docs/legal/manifest.csv`, ADR-007, ADR-017, `docs/apps/PACKETSHARK.md`).
- TLS 1.3 and P-256 enabled in the image's OpenSSL build: ASSUMED. One SDK sysroot on the build host (OpenSSL 3.4.1) defines none of `OPENSSL_NO_TLS1_3`, `OPENSSL_NO_EC`, `OPENSSL_NO_ECDSA`, `OPENSSL_NO_ECDH` (read 2026-11-21); not checked against the v0.4.0 image or on a unit (`docs/design/companion/README.md`, H1).
- Handshake cost on the unit, behaviour on a phone hotspot, a free TCP port: ASSUMED, not measured (`docs/design/companion/README.md`).
- Dropping all capabilities while keeping an open socket usable: VERIFIED on Unit A for `pos-capture` (ADR-016); ASSUMED to carry over to a listening TCP socket.

## Decision record

| Date | Decision | By |
| --- | --- | --- |
| 2026-11-21 | M0 direction approved with the eight conditions above; M1 documentation requested | Product owner |
| 2026-11-21 | Decided for the final text: `gatewayd`; TCP 47231; `gateway.*` v1; disabled by default; a configured device password required before the gateway can be enabled (rules 1, 9a, 9b) | Product owner |
| 2026-11-21 | **ADR-019 accepted as written**, including rules 9a and 9b and the scope names of rule 24 | Product owner |
