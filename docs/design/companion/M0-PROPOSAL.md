> **Record of the M0 study (2026-11-21).** The product owner approved its direction on 2026-11-21 with eight conditions. Where this text and the M1 documents differ, the M1 documents apply: see "What changed since M0" in [README.md](README.md). Paths of the form `C:\...` are the build host's.

# DOORS Companion: M0 architecture and feasibility

Status: **proposal for owner review.** Nothing is implemented. No service, API, unit or Android project was touched; nothing was built, run or flashed.
Date: 2026-11-21. Read from `origin/master` 085a089f (v0.4.0 line, worktree `C:\k230-companion-m0`) and the unmerged prototype branch `feat/meshcore-companion-tcp-bridge` (bc70776d).

Evidence classes as in AGENTS.md. Everything about the repository below is DOCUMENTED (read from source or docs); nothing here is VERIFIED on a unit. Android-side and performance statements are ASSUMED until measured and are marked.

## 1. Verdict

Feasible, with one new device service and no change to the ownership rules of the existing ones.

- **Device:** a new supervised daemon, working name `gatewayd`, that is one more pocketipc client of meshcored, sysd, netd and the shell, and the only Doors process with an authenticated network listener.
- **Transport:** TLS 1.3 with mutual authentication and pinned keys, using the OpenSSL 3.4.1 already in the image. No new dependency.
- **Pairing:** started on the device only; a QR code on the panel (LVGL's QR widget is already enabled) carries the address, the device key fingerprint and a one-time token.
- **Protocol:** the pocketipc envelope (length-prefixed JSON, request/response/event) over TLS, with an allowlist of methods. Not a transparent proxy.
- **PacketShark:** the helper stays owned by the shell's PacketShark session. The phone sees aggregate statistics and can stop or hold; a remote start is a request that a person confirms on the panel. This needs an amendment to ADR-016.
- **Android:** Kotlin, Compose, min SDK 29, no cloud, no Google services dependency.

Three things need the owner before any code: a new ADR for the network listener and its stored key (§10, D1), the ADR-016 amendment (D5), and the scope of what a phone may change (D4).

## 2. What exists and can be reused

| Area | Reusable today | Source |
| --- | --- | --- |
| IPC | pocketipc: 4-byte length + JSON, ids, events, topics, 1 MiB frames, 32 clients, bounded backpressure (200 ms, then the client is dropped) | docs/api/pocketipc.md, core/pocketipc |
| Mesh | `mesh.status` (with `run_id`), `identity`, `nodes`, `node`, `channels`, `messages`, `send` (incl. `resend`), `path_preview`, `subscribe`; events `mesh.message`, `mesh.node`, `mesh.channel`, `mesh.state`, `mesh.activity`, topic `mesh.rx` | docs/api/mesh.md, services/meshcored/api.c |
| Mesh, multi-client | meshcored is designed for several observers: "a second client changes nothing about the node". Keys never leave it: no channel secret and no private key in any result | MESHCORED.md, api.c:114-153, 666 |
| System | `system.info`, `system.status` (uptime, load, CPU, memory, temperature, storage, network, power, services), `system.logs`, `system.crashes`, `storage.status` | docs/api/system.md |
| Wi-Fi | `wifi.status`, `wifi.networks`, `wifi.saved`; no passphrase in any result (ADR-003) | docs/api/network.md |
| Radio | `radio.status`, `radio.stats`, `radio.info` (reads are never gated by the lease) | docs/api/radio.md |
| Capture | `pos-capture` line protocol v4: `state`, `stats` (4 Hz), `bin` (1 Hz), `diag` (1 Hz); `core/pocketpcap` | docs/apps/PACKETSHARK.md §6 |
| Supervision | `pos-supervise`, `S??` init scripts, `/etc/default/<name>` enable switch, state files that sysd reports | docs/ARCHITECTURE.md |
| Crypto, parsing | OpenSSL 3.4.1 and cJSON 1.7.18 in the image; `LV_USE_QRCODE 1` in the shell's LVGL | manifest, ui/shell/lv_conf.defaults:46 |
| Prior art | `companiond` prototype (4,200 lines, host-tested only): a meshcored client that reconnects with backoff, a 64-message queue, duplicate-send recognition, the "send whose answer never came" recovery, `tests/fake_meshcored`, an end-to-end test harness | branch feat/meshcore-companion-tcp-bridge, docs/services/COMPANIOND.md |
| Precedents | peerd: "silent and closed until asked", 16-hex identity display, identity file rules (ADR-017). Zabbix: network parsers and OpenSSL never enter the shell (ADR-007) | docs/decisions |

`companiond` itself is **not** the gateway. It speaks the upstream MeshCore companion protocol for the official app, which has no authentication and no encryption by design. Its bridge logic is worth lifting; its wire protocol must not share a port or a process with an authenticated service. Recommendation: keep it a separate, unmerged prototype.

## 3. Missing capabilities

| # | Gap | Consequence | Proposed answer |
| --- | --- | --- | --- |
| G1 | No authentication or authorization anywhere. Every socket is 0660 root, every process is root. docs/api/system.md says this stops being enough once IPC leaves the filesystem | The gateway must be the whole security boundary | Pinned mTLS, per-phone scopes, method allowlist (§5, §6) |
| G2 | Message history is RAM only: meshcored keeps 64 direct + 64 channel, ids restart at 1 each run, `mesh.messages` has `limit` but no cursor. RIFT's 512 is its own RAM cache | A phone that was away loses anything older than the ring | Gateway journal with a sequence cursor (RAM only); the phone is the durable archive (§6.2) |
| G3 | Unread marks, threads, notification state, activity bins and the RX log ring live only in RIFT | Read state cannot be shared | Phone keeps its own; not synced in v1 |
| G4 | One global repeater session; RIFT sends `mesh.remote_logout` on every connect | A phone-side repeater session would be killed by RIFT, and the reverse | Repeater login/CLI excluded from the phone API |
| G5 | sysd and netd raise no events | Status must be polled | Gateway polls only while a phone is subscribed |
| G6 | No device serial or unique id (wlan0 MAC is the only stable fact; the USB Ethernet MAC is random per boot) | Multi-device needs an identity | The gateway key's fingerprint is the device id |
| G7 | No mDNS, no AP mode (hostapd present, unused; netd is station only), no firewall | Discovery and hotspot are constrained | §5.4 |
| G8 | Capture state is private to the shell and its helper (a socketpair). Only `capture.lock` is visible outside | Nothing can observe a capture | Additive `capture.*` methods served by the PacketShark app (§7) |
| G9 | No pairing UI in Settings | Needed for product use | CLI pairing for the bench first; Settings page at image integration |
| G10 | libsodium is not in any image (peerd's dependency is still pending) | Rules out a libsodium design without an image change | Use OpenSSL |
| G11 | `capacity_percent` is always null (gauge unvalidated); a received message's route is not reported | Phone shows voltage and charge state, no percentage; no hop count on received messages | Show what exists; both are separate device work |
| G12 | No RTC: the wall clock reads 1970 until NTP | Certificate dates are meaningless; `*_mono_ms` only makes sense on the device | Pin keys, ignore validity dates; gateway stamps every reply with its monotonic and wall clock |

Stale docs noticed in passing (not changed): docs/api/mesh.md:473-476 and MESHCORED.md say there is no per-run identifier, but `mesh.status.run_id` exists.

## 4. The Companion Gateway (`gatewayd`)

```
 phone app ── TLS 1.3, pinned both ways ──▶ gatewayd ──┬─ meshcored.sock  mesh.*   (subscribed, read continuously)
   (Wi-Fi LAN or the phone's hotspot)        │         ├─ sysd.sock       system.* (polled while a phone watches)
                                             │         ├─ netd.sock       wifi.status
                                             │         ├─ radiod.sock     radio.status/stats
                                             │         └─ shell.sock      capture.* only (§7)
                                             └─ gatewayd.sock  gateway.* local control: pair, list, revoke (Settings, `doors`)
```

- **Shape.** A supervised service like meshcored: `S70gatewayd`, `GATEWAYD_ENABLE=0` by default, state in `/var/lib/pocketos/gatewayd` (0700). One thread, one `poll()` loop, non-blocking OpenSSL. C11, cJSON, OpenSSL; nothing else.
- **Owns:** the TCP listener, the device key, the list of paired phones, the message journal. **Owns nothing else:** no radio, no mesh key, no capture socket, no Wi-Fi control.
- **Closed until asked** (peerd's rule): no socket is bound unless at least one phone is paired or a pairing window is open. Binds the address of a named interface (wlan0 by default), never `0.0.0.0`; accepts only loopback, RFC 1918 and link-local sources.
- **Allowlist, not a proxy.** Each phone method is a table entry: upstream service, upstream method, required scope, parameter schema, result filter. Anything not in the table is "unknown method". `shell.*` is never forwarded: `shell.key` plus the Terminal app is a root shell.
- **Upstream discipline.** The gateway reads meshcored continuously and queues per phone, so a slow phone can never trip pocketipc's 200 ms rule and stall meshcored or RIFT. Polls use `pocketipc_call_timeout`.
- **Privilege.** Root, like everything in v0, but it needs no capability (port above 1024): drop every capability and set `no_new_privs` at start, as `pos-capture` does. It stays uid 0 until per-service users exist.
- **Limits (proposed):** 8 paired phones, 2 concurrent sessions, 64 KiB inbound frame, 256 KiB outbound queue per phone (snapshot events coalesce, then the phone is dropped), 4 TLS handshakes per second, 30 s ping, 90 s idle timeout.

Alternatives considered:

| Alternative | Why not |
| --- | --- |
| Extend `companiond` | Its protocol is unauthenticated plaintext by design and fixed by upstream; mixing it with an authenticated API in one process widens both |
| Put TCP into each service | Four listeners, four security implementations, and hostile network input inside the radio and Wi-Fi owners |
| Gateway inside the shell | A network parser and TLS in the process that owns the panel; against ADR-002 and ADR-007 |
| SSH port forwarding of the Unix sockets | No per-method control at all (full root API including `system.poweroff`, `shell.key`); sshd on this image is password root login |
| HTTPS + WebSocket (libmicrohttpd is in the image) | An HTTP parser and a WebSocket layer in front of authentication, a new declared dependency, and nothing gained: both ends are ours |
| Bluetooth LE | No usable controller on the unit (KNOWN_ISSUES) |

## 5. Security design

### 5.1 Transport

- **TLS 1.3 only, mutual authentication, ECDSA P-256 keys, self-signed certificates used as key containers.** Each side verifies the other by the SHA-256 of its public key (SPKI), nothing else: no CA, no host name, **no validity dates** (the device has no RTC).
- Device key: generated on first enable from the kernel's random source, `/var/lib/pocketos/gatewayd/identity.v1`, 0600, never returned over any API, never logged. Same creation and "never silently regenerate" rules as peerd's identity (ADR-017 §2).
- Phone key: generated in the Android Keystore, non-exportable. P-256 because hardware-backed Keystore support for it is universal.
- A client whose key is not in the paired list is disconnected at the end of the handshake, before one byte of application data is parsed. The pre-authentication attack surface is OpenSSL's TLS 1.3 handshake and nothing first-party.
- Forward secrecy comes with TLS 1.3 (peerd's static-key design does not have it; this one does at no cost).

Why not the alternatives: a libsodium construction would be a custom handshake, which ADR-017's terms asked to avoid, and libsodium is not in the image. Noise has no maintained first-party Android support. A password or PIN login would need a PAKE to be safe on a hostile LAN. TLS-PSK from the QR code would work but gives the phone no individually revocable identity.

### 5.2 Pairing

1. On the device: Settings → Companion → **Pair a phone** (bench: `doors call gatewayd gateway.pair_begin`). Never startable from the network.
2. The panel shows a QR code for 120 s: `doors-pair:1?h=<ip>&p=<port>&fp=<device key fingerprint>&t=<128-bit one-time token>&n=<name>`.
3. The phone scans it, connects with the fingerprint pinned, presents its own key, and calls `gateway.pair {token, phone_name}`.
4. The panel shows the phone's name and 16 hex digits of its fingerprint (groups of four, as ADR-017); the person taps **Allow** and chooses the scopes.
5. The device stores `{fingerprint, name, scopes, paired_at}`; the phone stores `{device fingerprint, name, last address}`.

The token is single-use, the window closes after one success, three failures or 120 s. Because the QR code carries the device's fingerprint out of band, a man in the middle cannot complete either handshake. Revocation is a row in Settings (or `gateway.revoke`); it takes effect at once and drops a live session.

Not in v1: pairing by typed code (it needs a PAKE or a fingerprint comparison by eye). The phone app accepts a pasted `doors-pair:` URI for the bench.

### 5.3 Authorization

Scopes per phone, granted on the panel at pairing:

| Scope | Allows | Default |
| --- | --- | --- |
| `status` | device info and status, Wi-Fi and radio status, mesh status | on |
| `messages` | read nodes, channels, messages; send direct and channel messages | on |
| `capture` | PacketShark statistics, stop, hold, request a start | off |
| `diagnostics` | `system.logs`, `system.crashes`, mesh activity and RX log | off |

**Never reachable from a phone in v1, whatever the scope:** `system.poweroff`, `storage.expand`, `storage.eject`, `radio_setup.start`, every `wifi.*` that changes something, `radio.configure/send`, `mesh.set_name`, `mesh.set_path_hash`, `mesh.channel_add/remove`, `mesh.node_remove/reset_path`, `mesh.advert`, `mesh.discover`, every `mesh.remote_*`, `mesh.app_send`, every `shell.*`. `system.reboot` is a candidate for a later `admin` scope (D4).

### 5.4 Network, discovery, hotspot

- **Same Wi-Fi:** works with netd as it is.
- **Phone hotspot:** the K230 joins the phone's hotspot as an ordinary station; no device change. ASSUMED to work like any WPA2 network; to be checked at the hardware gate.
  - *Note, 2026-11-27:* §9's rule that sockets are bound to the Wi-Fi network does not cover this case. The phone hosting a hotspot has joined no Wi-Fi network, so the M3 app reached nothing there.
  - The app's path for it is the one in CONTRACTS.md §2 (Network), accepted that day: a socket bound to no network, kept to the hotspot's subnet, after a route check.
  - Not yet tested on a phone (README.md H3, H12).
- **K230 as access point:** not available (netd is station only) and not proposed.
- **Discovery, v1:** no device-side discovery service. The QR code gives the first address; the phone remembers it; when it fails, the phone probes its own Wi-Fi subnet for the port and accepts only a host that proves the pinned key. No unauthenticated responder on the device, no multicast.
- **Discovery, later:** a minimal DNS-SD responder (`_doors._tcp`) if the probe proves too slow. It would be a second, unauthenticated parser of hostile input on UDP 5353, so it is deferred and needs its own decision.
- **Port:** one fixed TCP port. Proposal 47231 (peerd uses UDP 47230); ASSUMED free, to be confirmed.

### 5.5 Risks

| Risk | Severity | Mitigation / status |
| --- | --- | --- |
| A gateway bug is a root compromise (all Doors processes are root) | High | Authentication before any first-party parsing; allowlist; small frames; fuzzing and sanitizer suites as for peerd; capabilities dropped. Residual risk accepted until per-service users exist |
| sshd on the same network accepts root by password | High, pre-existing | Not caused by this work, but the gateway makes "the device is on a LAN" the normal case. A device password becomes pressing (ADR-016 already says so) |
| Stolen SD card: device key and paired list are readable (file permissions only, as ADR-003) | Medium | The thief can impersonate the device to its phones, not the reverse. Accepted like ADR-003/017 until a device-bound key store exists; "forget this device" on the phone |
| Stolen or compromised phone | Medium | Non-exportable key; optional app lock; revoke on the panel; scopes limit the damage to messages sent as this node |
| Phone sends on the air as this node | Medium | That is the feature. Rate limit per phone; meshcored's own limits apply (8 ACK waits, shared with RIFT) |
| Mesh text is hostile input on the phone too | Medium | Rendered as plain text; no link previews, no automatic fetch |
| Handshake flood stalls the single hart | Medium | Handshake rate limit, 8 pending connections, nice 5 |
| Statistics or messages leak into logs | Low | Lifecycle lines only; a lint as for meshcored and PacketShark |
| Message history on the phone outlives the device's RAM-only rule | Low, a product choice | Stated in the app; excluded from cloud backup (D7) |

## 6. API v1

### 6.1 Envelope and versioning

- Frames: 4-byte big-endian length + one UTF-8 JSON object, exactly pocketipc's envelope (`id`/`method`/`params`, `result`/`error`, `event`/`data`) and error codes, plus code 7 "not permitted for this phone".
- First call `gateway.hello {proto_min, proto_max, app, app_version}` → `{proto, device_id, name, model, version, scopes, capabilities[], api: {mesh, system, wifi, radio, capture}, mono_ms, wall_ms, clock_set}`. `proto` is the integer protocol version; `capabilities` are feature strings the app gates screens on; `api` repeats each service's own `api_version`.
- Compatibility rule as pocketipc: added fields are compatible, unknown fields are ignored, anything else bumps `proto`. The gateway serves at most two consecutive `proto` values.
- Method names are the device's own (`mesh.send`, `system.status`): one vocabulary, one set of docs. The gateway adds `gateway.*` and filters results.
- Every reply and event carries `mono_ms`, so the phone can place the device's monotonic stamps on its own clock without trusting the device's wall time.

### 6.2 Surface

| Group | Methods (scope) | Events |
| --- | --- | --- |
| Session | `gateway.hello`, `gateway.ping`, `gateway.subscribe {topics[]}`, `gateway.pair` (pairing window only) | `gateway.revoked`, `gateway.upstream {service, connected}` |
| Device | `system.info`, `system.status`, `wifi.status`, `radio.status`, `radio.stats`, `storage.status` (`status`); `system.logs`, `system.crashes` (`diagnostics`) | `device.status` snapshot, at most every 2 s, only while subscribed |
| Mesh, read | `mesh.status`, `mesh.identity`, `mesh.nodes`, `mesh.node`, `mesh.channels`, `mesh.path_preview`, `mesh.sync {after_seq, limit}` (`messages`) | `mesh.message`, `mesh.node` (coalesced per node), `mesh.channel`, `mesh.state` |
| Mesh, send | `mesh.send {to \| channel, text, client_msg_id}`, `mesh.send {resend}` (`messages`) | outcome arrives as `mesh.message` state changes |
| Mesh, diagnostics | topic `activity` (1 Hz aggregate of `mesh.activity`), topic `rx_log` (`diagnostics`) | `mesh.activity_bin`, `mesh.rx` |
| Capture | `capture.status`, `capture.stop`, `capture.hold`, `capture.request_start` (`capture`) | `capture.state`, `capture.stats` (1 Hz) |

Two gateway additions carry the weight:

- **`mesh.sync` and the journal.** The gateway keeps the last 512 `mesh.message` events in memory under its own sequence number and returns `{run_id, journal_id, seq, messages[]}`. A phone resumes from `after_seq`; a changed `journal_id` (gateway restart) or `run_id` (meshcored restart) means "take the snapshot again". The phone keys a message by `(device_id, run_id, id)` and stores history itself. Memory only, in line with meshcored's choice not to persist messages. What the phone missed while both the phone was away and the ring wrapped is lost, and the app says so.
- **Idempotent send.** `client_msg_id` is remembered for the last 64 sends; a repeat after a lost reply is answered from the record and not transmitted again. The uncertain case (meshcored stalled after taking the message) is resolved against `mesh.messages`, as `companiond` already does and tests.

### 6.3 Multi-device

Nothing in the protocol assumes one device or one phone: `device_id` is the device key fingerprint, pairing is per pair, and all phone data is partitioned by `device_id`. The app holds a list of devices and one connection per device in view. Several phones on one device see the same node, exactly as RIFT and a phone do. Deferred: cross-device views, and any device-to-device relay.

## 7. PacketShark: helper ownership and capture lifecycle

ADR-016 is accepted with binding rules. The ones a companion touches:

| Rule | Text (short) | Remote statistics | Remote stop/hold | Remote start |
| --- | --- | --- | --- | --- |
| 1 | Nothing captures without START on the app's own screen; not over IPC | ok | ok | **conflict unless a person taps START on the panel** |
| 5 | A capture ends when the app is left | ok | ok | ok (unchanged) |
| 7 | FULL only by a confirmed per-session choice | ok | ok | remote requests are HEADERS only |
| 11 | Nothing is uploaded or sent; app and helper have no network code | **conflict in spirit** | ok | ok |
| 12 | No packet content, address or name is logged | ok | ok | ok |
| 13-14 | State visible on screen; screen stays on | ok | ok | ok |

**Recommendation: ownership does not move.**

- `pos-capture` remains a child of the shell's PacketShark session: spawned at START, one helper per capture, capabilities dropped before a frame is parsed, `capture.lock`, `PR_SET_PDEATHSIG`, gone when the app is left. `gatewayd` never spawns, signals or talks to the helper and never takes the lock.
- The PacketShark app serves four additive methods through the shell's socket, answered from the model it already keeps (no extra helper traffic, no packet data):
  - `capture.status` → state, interface, mode, elapsed, limit, counters, per-class rates, drops, shed, diagnosis totals. **No addresses, names, rows or bytes.**
  - `capture.stop`, `capture.hold {on}`: reducing a capture is always allowed.
  - `capture.request_start {iface, limit_s}`: opens PacketShark on its setup screen with a sheet "A paired phone asks to capture on eth0"; nothing starts until a person taps START on the panel. HEADERS only, never promiscuous.
- The helper's 4 Hz `stats` reaches the phone at 1 Hz.
- Lifecycle seen from the phone: the capture is the device's. A phone or gateway disconnect changes nothing on the device. Leaving the app, the 15-minute limit, a helper crash or STOP on either side ends it, and the phone is told which.
- While a phone is subscribed to capture events, the app's header says so (rule 13's spirit).

This needs an **amendment to ADR-016** (rule 1 wording for the confirmed request, rule 11 for aggregate counters to a paired phone) and a new shell hook so an app can serve methods. Both are owner decisions; until then the capture group is simply absent from `capabilities`.

Alternatives:

| Alternative | Assessment |
| --- | --- |
| Gateway owns its own helper (background capture) | ADR-016 option A in effect: a capture with no screen. Needs a new ADR, a system-wide indicator, and resolves who wins the lock. Not for v1 |
| Helper gets a second, read-only channel to the gateway | Breaks "one owner", adds a descriptor to a process that has dropped its privileges; no benefit over the app relaying its model |
| Packet rows and the inspector on the phone | Sends other people's addresses and DNS names off the device. Possible later behind its own scope and decision (D6) |
| pcap download to the phone | Rule 9 and 11 directly. Not proposed; saved files are already reachable by other means |

## 8. Performance constraints

- **One hart** (Linux sees one core), shared with the LVGL loop. The gateway runs at nice 5, is idle when no phone is connected (no timers), and polls sysd/netd only while a phone is subscribed (2 s and 5 s).
- **TLS handshake cost** on the C908 is unmeasured (ASSUMED tens of milliseconds for P-256). Session resumption and the handshake rate limit bound it; it is the first thing the hardware gate measures, together with the shell's frame time during a connect.
- **Memory:** about 850 MB available at idle; the gateway is a few MiB with OpenSSL. Not a constraint.
- **Event volume:** `mesh.node` fires on every advert heard and `mesh.activity` on every frame; both are coalesced before they reach a phone. `mesh.nodes` can reach about 600 KB at 1,000 nodes: the gateway pages it (`offset`, `limit`) to stay under its frame cap.
- **Shared limits:** meshcored allows 8 direct messages waiting for an ACK across all clients; the phone sees "busy" like RIFT does.
- **Wi-Fi power save** on the unit (IPS on, LPS max) may add latency to the first packet after idle; ASSUMED, to be measured.
- **Phone:** a kept connection for notifications costs a foreground service and battery; it is optional and off by default.

## 9. Android app

- **Stack:** Kotlin, Jetpack Compose, single activity, coroutines and Flow, Room, DataStore, kotlinx.serialization, CameraX + ZXing for the QR code. min SDK 29 (TLS 1.3 by default). No Firebase, no Play Services, no analytics. Project at `C:\Android\DoorsCompanion`, following the layout of the existing projects there.
- **Modules:**
  - `:core:protocol` pure Kotlin: framing, message models, capability gating. Tested on the JVM against recorded frames and a host-built `gatewayd`.
  - `:core:transport` TLS socket bound to the Wi-Fi `Network`, pinning trust manager, Keystore key, reconnect with backoff, subnet probe.
  - `:core:data` Room (devices, messages, nodes, channels, read marks per device), repositories, the sync cursor.
  - `:feature:devices`, `:feature:rift`, `:feature:capture`, `:feature:status`.
- **Rules:** sockets are bound to the Wi-Fi network explicitly, or Android routes them over mobile data when the Wi-Fi has no internet (a hotspot or an isolated LAN). `allowBackup=false`. Message text is plain text. An optional foreground service (`connectedDevice`) keeps one device connected for message notifications.
- **Permissions:** INTERNET, ACCESS_NETWORK_STATE, CAMERA (pairing only), POST_NOTIFICATIONS, FOREGROUND_SERVICE. No location.

Navigation:

```
Devices ──▶ (add) Scan QR ──▶ Paired
   │
   └─▶ Device ─┬─ Status      battery, Wi-Fi, storage, services, mesh state; Logs (diagnostics)
               ├─ RIFT        Chats (direct + channels) ─▶ Thread ─▶ compose, delivery state
               │              Nodes ─▶ Node detail        Activity (diagnostics)
               ├─ PacketShark Live: state, rates, classes, diagnosis; STOP, HOLD, Request capture
               └─ ⋮           Device details, scopes granted, Forget device
```

The device switcher is the top bar's title; every screen below it belongs to one device. A tab whose capability the device does not report is not shown.

## 10. Decisions for the owner

| # | Decision | Recommendation |
| --- | --- | --- |
| D1 | A new ADR (ADR-019) for an authenticated network listener, its stored key and paired-phone list | Accept the direction; I draft the ADR as the first step |
| D2 | Transport: pinned mutual TLS 1.3 on OpenSSL, QR pairing started on the panel | As proposed |
| D3 | Service name and port: `gatewayd`, TCP 47231; `companiond` stays a separate prototype | As proposed |
| D4 | What a phone may change: v1 = send messages, stop/hold a capture, nothing else. `system.reboot` later behind `admin`? | v1 as stated; reboot deferred |
| D5 | ADR-016 amendment: aggregate statistics to a paired phone; remote start as a request confirmed on the panel | Accept both; rows and files stay out |
| D6 | Packet rows / inspector on the phone | Defer past v1 |
| D7 | The phone keeps message history on its own storage (the device keeps none) | Accept, stated in the app, no cloud backup |
| D8 | Discovery without a device-side service in v1 | Accept; DNS-SD only if the probe disappoints |
| D9 | iOS | Out of scope; nothing in the protocol prevents it |

## 11. Phased plan

Sizes are relative (S about one working session, M two to three, L four or more). Each device milestone ends host-built and host-tested with plain and ASan/UBSan runs; nothing reaches a unit before its gate, and each gate needs its own go-ahead.

| Phase | Content | Done when | Size | Needs |
| --- | --- | --- | --- | --- |
| **M0** | This proposal | Reviewed | - | - |
| **M1** Design records | ADR-019; ADR-016 amendment text; `docs/api/gateway.md` (protocol v1, every method's filter and scope); threat model | Owner accepts | S | D1-D5 |
| **M2** Gateway core | `gatewayd` skeleton: loop, TLS, identity, pairing over `gatewayd.sock`, `gateway.*`, device status group; frame fuzzer; Python reference client and end-to-end suite on fake services. Built by name, not in `make all`, not in the image | Host suites pass | M | M1 |
| **M3** Android foundation | Project, `:core:protocol`, `:core:transport`, pairing by QR and pasted URI, Devices and Status screens, against host `gatewayd` | Pair and read status from the emulator and a phone on the LAN | M | M2 |
| **M4** RIFT read | Journal, `mesh.sync`, event coalescing, paging; app: Chats, Thread, Nodes, Room storage, resume after reconnect | Host suite incl. meshcored restart and gateway restart; app shows fake traffic | M | M2, M3 |
| **M5** RIFT send | Idempotent `mesh.send`, resend, rate limit; app: compose, delivery states, notifications, optional foreground service | Host suite: no double transmit across every reconnect case | M | M4 |
| **M6** Hardware gate 1 | Non-persistent kit on Unit A (as the PacketShark validation: tmpfs only, removed afterwards). Parts: connect and status; handshake cost and shell frame time; receive with RIFT open; phone hotspot. Sending on the air is a separate part with its own go-ahead | Gate report with PASS/FAIL per part | S | owner go-ahead |
| **M7** PacketShark tier 1 | Shell hook for app-served methods, `capture.*` in the app, gateway mapping, lint update; app: Live screen, STOP, HOLD, Request capture | Host suites on the synthetic source; ADR-016 lint still holds | M | D5, M2 |
| **M8** Product integration | `S70gatewayd` (disabled by default), Settings → Companion page (QR, paired phones, scopes, revoke), status-cluster mark while a phone is connected, Makefile install, `check_rootfs`, deploy scripts, notices | Image contents tests pass; hardware gate 2 incl. PacketShark on a unit | L | M6, M7, image build go-ahead |
| **Later** | DNS-SD discovery; read-state sync; packet rows (D6); `admin` scope; cross-device views; per-service user for the gateway; iOS | - | - | separate decisions |

Critical path to a usable messaging companion: M1 → M2 → M3 → M4 → M5 → M6. PacketShark (M7) is independent of M4-M5 and can run beside them once D5 is decided.

## 12. What this study did not do

- Nothing was built, run or tested; no unit was contacted. Every device-side claim is DOCUMENTED from `origin/master` 085a089f, not VERIFIED.
- Not checked: whether TCP 47231 is free on the image; whether OpenSSL on the image is built with TLS 1.3 and P-256 enabled (expected, since libcurl uses it, but unconfirmed); handshake cost on the C908; hotspot behaviour; the real mode of `/run/pocketos` (init scripts create it before the library's 0770 applies).
- The existing Android projects under `C:\Android` were listed, not read; "following their layout" is to be confirmed at M3.
- The shell's ability to let an app serve IPC methods was not designed in detail; M7 starts with that.
