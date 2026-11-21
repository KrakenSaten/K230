# Companion: integration contracts

Status: **draft, not implemented.** The minimum each side must do so that the unit and the app can be built separately and still fit. The wire format is `docs/api/gateway.md`; the rules come from ADR-019 and ADR-016 amendment 1 (both proposed).

## 1. `gatewayd` must

**Network**
- Stay closed while disabled, while no client is paired and no window is open, and while the unit has no device password (ADR-019 rule 9b).
- Listen on TCP 47231 on the address of one configured interface; never the wildcard address; private, link-local and loopback sources only; nothing bound while no client is paired and no window is open.
- TLS 1.3 only; one ECDSA P-256 certificate; request and require a client certificate; decide by the pin alone (PAIRING.md §2); no tickets, no resumption, no 0-RTT.
- Follow the interface's address: rebind when it changes, close when the interface goes away.

**Protocol**
- Refuse every request before a successful `gateway.hello`.
- Stamp every response and event with `mono_ms`; report `boot_id`, `journal_id`, and meshcored's `run_id` wherever message ids appear.
- Check the scope on every request; answer methods outside the allowlist with code 1 without contacting any service.
- Bound everything: frame sizes, 8 requests in flight per client, the outbound queue, the journal, the `client_msg_id` records.

**Upstream**
- Be a pocketipc client only, with a deadline on every call and a bounded connect; reconnect with backoff; announce `gateway.upstream`.
- Read meshcored's events continuously whether or not a client is connected (the journal fills while phones are away).
- Subscribe to meshcored without `rx_log`. Never call a method outside the allowlist, on any socket.
- Poll sysd, netd and radiod only while a client holds the `device` topic.

**State and logs**
- `identity.v1` and `paired.v1` under `$POCKETOS_STATE_DIR/gatewayd`, 0600 in 0700, temp-flush-rename, the directory flushed; never regenerate an identity beside existing state.
- Log lifecycle only: start, stop, listen address, a client connecting or leaving (by fingerprint prefix), pairing opened, allowed, refused, revoked, a request refused by scope or rate. Never text, names, tokens or keys.

**Process**
- One thread, non-blocking; nice 5; capabilities dropped and `no_new_privs` set before the first accept.
- A lint, `tests/gatewayd_lint.sh`, holding: no `SSL_VERIFY_NONE`; no verification callback that returns success unconditionally; no TLS version below 1.3; no `shell.` method string other than the four `capture.*` entries; no logging call that takes message text or a key; no field named for a key, secret, passphrase or token in any allowlist result filter; no include of `pocketpcap`, RadioLib or MeshCore.

## 2. The Android app must

Identity (proposed 2026-11-21): application id `app.doors.companion`, Apache-2.0, project at `C:\Android\DoorsCompanion` on the build host, min SDK 29. The project does not exist yet.

**Trust**
- Hold one non-exportable P-256 key in the Android Keystore, generated at first use, hardware-backed where available.
- Build the TLS context for a connection from exactly one pinned fingerprint: a trust manager that accepts a server certificate only if it is a single certificate whose key matches, and that never delegates to the platform trust store. TLS 1.3 only.
- Contain no accept-all trust manager, no hostname verifier that returns true, no "insecure" build variant or developer switch, and no cleartext fallback. A lint (or a unit test over the dependency graph and sources) holds that.
- Never accept a changed key for a known unit. Show the unit's name and fingerprint before storing a pin.

**Network**
- Bind its sockets to the Wi-Fi network explicitly.
- Reconnect, probe and back off as PAIRING.md §7; run no reconnect loop in the background without the optional connection service.

**Protocol**
- Send `gateway.hello` first; show a feature only when its capability and scope are present; ignore unknown fields and events.
- Key a message by `(device_id, run_id, id)`; keep the newest state per message; treat `seq` as a cursor only.
- Convert `*_mono_ms` with the `mono_ms` of the same frame; never display the unit's wall clock as a fact when `clock_set` is false.

**Sending**
- One `client_msg_id` per message a person sends, reused only to repeat that same message.
- Send only on a person's action: no automatic send, no scheduled send, no automatic resend, no sending while disconnected. `mesh.resend` is offered as a button, as RIFT offers it.

**Data**
- Messages, contacts and pins in app-private storage; excluded from cloud backup and device transfer; removable per unit.
- Render mesh text as plain text: no link preview, no automatic fetch, no markup.
- Request no permission beyond INTERNET, ACCESS_NETWORK_STATE, CAMERA (for the QR code), POST_NOTIFICATIONS and FOREGROUND_SERVICE.

## 3. The shell and PacketShark must (ADR-016 amendment 1)

- Offer a way for an app inside the shell to serve named methods on `shell.sock` while it is in front. This hook does not exist today; its shape is the first design task of that milestone. It must not let an app's handler block the LVGL loop: a handler answers from memory.
- PacketShark serves `capture.status`, `capture.hold`, `capture.stop`, `capture.request_start` from `psk_model`, adding no helper command and no helper traffic.
- `capture.status` is built from counters only: the lint is extended so that no address, name, row or byte buffer can reach it.
- The request sheet: names the client and the interface, HEADERS only, a 60 s timeout, START by tap only.
- The header mark while a companion reads the figures.
- The Settings page for pairing draws the QR code with LVGL's QR widget from the URI `gateway.pair_begin` returns; the shell links no TLS and no gateway code, and talks to `gatewayd.sock` like any other service.

## 4. Existing services must

Nothing. meshcored, sysd, netd and radiod are not changed by v1: the gateway uses their APIs as documented at their current `api_version`. Two gaps are worked around in the gateway rather than fixed upstream:

| Gap | Worked around by | A cleaner fix, later and separately |
| --- | --- | --- |
| `mesh.messages` has no cursor and ids restart per run | the gateway's journal and `run_id` | an `after_id` cursor in meshcored |
| sysd and netd raise no events | polling while subscribed | `system.subscribe`, `wifi.subscribe` |

## 5. Compatibility between releases

- The unit and the app are released separately; the app must work with a unit one protocol version behind, and a unit with an app one behind.
- The gateway's document (`docs/api/gateway.md`) is the contract. A change to an upstream service's result that an allowlist filter passes on is a change to this protocol too, and is reviewed as one.
- A reference client and a fake gateway, both built from that document, are the test fixtures each side uses without the other (a Python client for the unit's suite; a host-built `gatewayd` against fake services for the app's).
