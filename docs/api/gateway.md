# Companion Gateway protocol v1 (`gateway.*`)

Status: **draft, not implemented.** Protocol version 1. Specified for ADR-019 (proposed) and ADR-016 amendment 1 (proposed); it binds nothing until they are accepted. Served by `gatewayd` to paired clients over TLS; a second, local surface on `gatewayd.sock` is for the unit's own Settings and the `doors` CLI.

Design rules: `docs/decisions/ADR-019-companion-gateway.md`. Pairing and connection life cycle: `docs/design/companion/PAIRING.md`.

## 1. Transport

- TCP port 47231 (proposed; ADR-019 rule 9a), on the address of one interface. TLS 1.3, mutual authentication, pinned keys (ADR-019 rules 10 to 14).
- Framing as pocketipc: a 4-byte big-endian length, then that many bytes of UTF-8 JSON encoding one object. Client to gateway at most 64 KiB; gateway to client at most 256 KiB. A longer frame, invalid UTF-8, invalid JSON or a non-object closes the connection.
- Messages as pocketipc (`docs/api/pocketipc.md`): request `{id, method, params}`, exactly one response `{id, result}` or `{id, error: {code, message}}`, events `{event, data}`. Requests may be pipelined; the gateway answers at most 8 at a time per client and reads no further until one completes.
- Every response and event also carries `mono_ms`: the unit's monotonic clock when it was written. All `*_mono_ms` fields anywhere are on that clock. The unit's wall clock is reported, never relied on (`clock_set`).

## 2. Versioning

- `proto` is one integer for the envelope, the `gateway.*` methods, the allowlist and its filters. Adding a method, a result field, an event or a capability is compatible. Removing or renaming anything, changing a type, or widening what a scope allows raises `proto`.
- The first request on every connection must be `gateway.hello`. The gateway picks the highest `proto` both sides support, or answers error 8 and closes. A gateway serves at most two consecutive versions.
- `capabilities` is a list of strings; a client shows a feature only when its capability is present. v1: `status`, `mesh`, `mesh.activity`, `capture` (absent until amendment 1 is implemented and PacketShark is in the image).
- `api` repeats the `api_version` of each upstream service, so a client can tell which fields of a passed-through result to expect.
- Clients ignore unknown fields, events and capabilities.

## 3. Errors

Codes 1 to 6 are pocketipc's (unknown method, invalid params, rejected by policy, backend failure, busy, not supported). Added:

| Code | Meaning |
| --- | --- |
| 7 | not permitted: the client's scopes do not include what the method needs |
| 8 | no common protocol version |
| 9 | upstream unavailable: the service behind the method is not connected or did not answer in time |
| 10 | rate limited |

An unknown method and a method outside the allowlist are both code 1: the gateway does not reveal what exists behind it.

## 4. Scopes

Granted per client on the unit (ADR-019 rule 24).

| Scope | Allows |
| --- | --- |
| `status` | §6: device, Wi-Fi, radio and mesh service status |
| `mesh_read` | §7: nodes, channels, messages, activity |
| `mesh_send` | §8: sending messages. Requires `mesh_read` |
| `capture_view` | §9: the capture's state and aggregate figures |
| `capture_control` | §9: hold, stop, request a start. Requires `capture_view` |

## 5. Session

| Method | Params | Result |
| --- | --- | --- |
| `gateway.hello` | `proto_min`, `proto_max`, `app`, `app_version` | `proto`, `device_id` (64 hex), `name`, `model`, `version`, `build`, `scopes[]`, `capabilities[]`, `api{mesh, system, wifi, radio}`, `wall_ms`, `clock_set`, `boot_id`, `limits{frame_in, frame_out, send_per_min}`, `paired` (false for a pairing candidate) |
| `gateway.pair` | `token` (base64url), `name` (1 to 32 bytes) | only for a pairing candidate, once. Answers when a person has approved or refused on the unit, or the window closed: `{paired: true, scopes[]}` or error 3 |
| `gateway.ping` | - | `{}` |
| `gateway.unpair` | - | `{}`, then the gateway closes. Removes the calling client from the paired list (a phone forgetting the unit); it can remove nobody else |
| `gateway.subscribe` | `topics[]` | `{topics[]}`: the topics now held. Replaces the previous set. Topics: `device`, `mesh`, `mesh.activity`, `capture`; one the client's scopes do not allow is left out |

Events: `gateway.scopes {scopes[]}` when the unit reduced them; `gateway.revoked {}` immediately before the gateway closes a revoked client; `gateway.upstream {service, connected}` when a service behind the gateway goes away or comes back.

`boot_id` changes when the unit restarts: `mono_ms` values from another `boot_id` are not comparable.

## 6. Device status (`status`)

Passed through with a deadline; the result is the upstream result reduced to the fields named. "As upstream" means the fields documented in that service's API document.

| Method | Upstream | Fields passed on |
| --- | --- | --- |
| `system.info` | sysd | `version`, `build`, `model`, `kernel`, `hostname`, `cpus`, `os`, `api_version` |
| `system.status` | sysd | `uptime_s`, `load`, `cpu_percent`, `memory`, `temperature_c`, `clock_set`, `storage[]`, `network[]` (name, operstate, carrier, ipv4; **no MAC**), `power`, `services[]` (name, running, crashloop, restarts) |
| `wifi.status` | netd | `state`, `reason`, `ssid`, `signal_dbm`, `bars`, `ipv4`, `security` (**no BSSID**) |
| `radio.status` | radiod | as upstream |
| `mesh.status` | meshcored | as upstream |

Event (topic `device`): `device.status` `{system, wifi, radio, mesh}`, the four status results above in one object, at most every 2 s and only when something changed. The gateway polls upstream only while a client holds the topic.

Not in v1: `system.logs`, `system.crashes`, `storage.status`, `radio.stats` (open item U6).

## 7. Mesh, reading (`mesh_read`)

| Method | Params | Result |
| --- | --- | --- |
| `mesh.identity` | - | as upstream (public key, name; never the private key) |
| `mesh.nodes` | `offset`, `limit` (at most 100) | `nodes[]` as upstream, `count`, `total`, `run_id` |
| `mesh.node` | `node` | as upstream |
| `mesh.channels` | - | as upstream (slot, name, hash, limits; never a key) |
| `mesh.path_preview` | `to`, `via[]` | as upstream; transmits nothing |
| `mesh.sync` | `after_seq` (optional), `limit` (at most 100) | `journal_id`, `run_id`, `seq` (the newest held), `first_seq` (the oldest held), `messages[]` each `{seq, message}`, `more` |

**The journal.** The gateway keeps the last 512 `mesh.message` events it saw, in memory, under its own `seq` (from 1, never reused within a `journal_id`). A message that changes state (ACK, no ACK, heard back) gets a new `seq` with the same message `id`; the client keeps the newest per `(run_id, id)`.

- No `after_seq`: the newest `limit` entries. On a new journal the gateway first fills it from `mesh.messages`.
- `after_seq` older than `first_seq`: the answer starts at `first_seq` and carries `gap: true`; messages in between are lost to this client.
- A changed `journal_id` (the gateway restarted) or `run_id` (meshcored restarted; message ids start again at 1): the client discards its cursor and syncs without `after_seq`. Messages it already stored stay stored under their old `run_id`.
- The journal is not history. The unit keeps no messages on storage (meshcored's rule); a client that wants history keeps it.

Events (topic `mesh`): `mesh.message {seq, message}`; `mesh.node {reason, node}`, at most one per node every 10 s except `removed`; `mesh.channel`; `mesh.state`.

Events (topic `mesh.activity`): `mesh.activity_bin {start_mono_ms, rx_frames, rx_bytes, tx_ok, tx_failed, by_type{...}}` once a second while there was activity. Counts only; the per-frame `mesh.activity` and the receive log `mesh.rx` are not passed on in v1.

## 8. Mesh, sending (`mesh_send`)

| Method | Params | Result |
| --- | --- | --- |
| `mesh.send` | exactly one of `to` (64 hex) or `channel` (slot); `text`; `client_msg_id` (16 to 36 characters, chosen by the client per message); optional `via[]` | `accepted`, `message_id`, `run_id`, `route`, `ack_expected`, `ack_timeout_ms`, `duplicate` |
| `mesh.resend` | `message_id`, `run_id`, `client_msg_id` | as above |

- **At most once.** The gateway remembers the last 64 `client_msg_id` values per client with their outcome. A repeat is answered from the record with `duplicate: true` and is not transmitted. If the first attempt's outcome is unknown (meshcored did not answer in time), the gateway looks the message up in `mesh.messages` before deciding, and transmits only if it is not there.
- `mesh.resend` with a `run_id` that is no longer current is error 3: the message belongs to an earlier run and cannot be resent, only sent as new by the person.
- Text is refused (code 2) unless it is well-formed UTF-8 with no NUL and no control character other than newline and tab, within the upstream limit.
- Rate limit: 20 sends per minute per client (code 10). Upstream limits apply unchanged: 8 direct messages waiting for an ACK across all clients, RIFT included, is code 5.
- Delivery state is not in the response. It arrives as `mesh.message` with the same `message_id`: `sent_flood`/`sent_direct`, then `acked` or `no_ack`; for a channel, `transmitted` and `heard_back`.

## 9. Capture (`capture_view`, `capture_control`)

Only with ADR-016 amendment 1. Served by the PacketShark app; the gateway passes these four methods to the shell's socket and no other.

| Method | Scope | Result |
| --- | --- | --- |
| `capture.status` | view | see below |
| `capture.hold` `{on}` | control | `{state}`; error 3 when nothing is capturing |
| `capture.stop` | control | `{state}`; error 3 when nothing is capturing |
| `capture.request_start` `{iface, limit_s}` | control | `{shown: true}` when the sheet is up on the unit; the outcome arrives as `capture.state`. Error 3: a capture is running, a request is already shown, the unit is locked or showing an alert, or `iface`/`limit_s` is not one the setup screen offers. Error 10: more than one request in 30 s |

`capture.status`:

```json
{"available": true, "state": "capturing", "reason": "none", "iface": "eth0", "keep": "headers",
 "elapsed_ms": 61250, "limit_s": 900, "seen": 48211, "stored": 48211, "held": 0,
 "kernel_drops": 0, "iface_drops": 0, "shed": 0, "bytes": 31022764, "pps": 812, "bps": 4120000,
 "fill_permille": 310,
 "classes": {"tcp": 39120, "udp": 5210, "dns": 1630, "dhcp": 4, "arp": 61, "icmp": 12, "ipv6": 2101, "other": 73},
 "findings": [0, 3, 0, 1, 0, 0], "diag": {"dns_queries": 815, "dns_retries": 4, "...": 0},
 "requested_by_companion": false}
```

- `available: false` (and nothing else) when PacketShark is not the app in front.
- `state`: `idle`, `capturing`, `hold`, `saving`, `stopped`, or `ended` with `reason` (`left`, `limit`, `link`, `helper_crashed`, `helper_hung`, `refused`, `timeout`).
- Every figure is a count, a rate or a size. **No field identifies a host**; ADR-016 rule 11a lists what may be here.

Events (topic `capture`): `capture.state` on every change; `capture.stats`, the figures above, once a second while capturing.

## 10. The local control surface (`gatewayd.sock`)

pocketipc, root only, for Settings and `doors`. Never reachable from the network.

| Method | Meaning |
| --- | --- |
| `gateway.info`, `gateway.status` | version, `api_version`; enabled, listening address, sessions, and why it is closed if it is: `disabled`, `no_clients`, `password_required` (ADR-019 rule 9b), `identity_fault` |
| `gateway.pair_begin` | opens a window; returns the pairing URI for the QR code and the time left |
| `gateway.pair_cancel` | closes it |
| `gateway.pair_decide` `{allow, scopes[]}` | the person's decision about the waiting candidate |
| `gateway.clients` | the paired list: fingerprint, name, scopes, connected, last seen |
| `gateway.set_scopes` `{client, scopes[]}` | reduces or changes one client's scopes |
| `gateway.revoke` `{client}` | removes one client |
| `gateway.subscribe` | events `gateway.candidate`, `gateway.session` |

## 11. Not in v1

Anything that changes the node, the radio, Wi-Fi, storage or power (ADR-019 rule 25); repeater sessions; the receive log; app datagrams; logs and crash reports; packet rows, the inspector or files; discovery; push without a connection.
