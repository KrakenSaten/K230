# Companion Gateway protocol v1 (`gateway.*`)

Status: **draft; partly implemented (M2, M4.1 and M4.2, host-tested only).** `services/gatewayd` serves §5 (the topics `device` and `capture` are not served yet), §6 without the `device.status` event, §7, §8, and §10; §9 is not implemented and its methods answer "unknown method" (`docs/services/GATEWAYD.md`). Protocol version 1. Specified for ADR-019 (accepted 2026-11-21) and ADR-016 amendment 1 (proposed): §9 binds nothing until the amendment is accepted. Served by `gatewayd` to paired clients over TLS; a second, local surface on `gatewayd.sock` is for the unit's own Settings and the `doors` CLI.

Design rules: `docs/decisions/ADR-019-companion-gateway.md`. Pairing and connection life cycle: `docs/design/companion/PAIRING.md`.

## 1. Transport

- TCP port 47231 (ADR-019 rule 9a), on the address of one interface. TLS 1.3, mutual authentication, pinned keys (ADR-019 rules 10 to 14).
- Framing as pocketipc: a 4-byte big-endian length, then that many bytes of UTF-8 JSON encoding one object. Client to gateway at most 64 KiB; gateway to client at most 256 KiB. A longer frame, invalid UTF-8, invalid JSON or a non-object closes the connection.
- Messages as pocketipc (`docs/api/pocketipc.md`): request `{id, method, params}`, exactly one response `{id, result}` or `{id, error: {code, message}}`, events `{event, data}`. Requests may be pipelined; the gateway answers at most 8 at a time per client and reads no further until one completes.
- Every response and event also carries `mono_ms`: the unit's monotonic clock when it was written. All `*_mono_ms` fields anywhere are on that clock. The unit's wall clock is reported, never relied on (`clock_set`).

## 2. Versioning

- `proto` is one integer for the envelope, the `gateway.*` methods, the allowlist and its filters. Adding a method, a result field, an event or a capability is compatible. Removing or renaming anything, changing a type, or widening what a scope allows raises `proto`.
- The first request on every connection must be `gateway.hello`. The gateway picks the highest `proto` both sides support, or answers error 8 and closes. A gateway serves at most two consecutive versions.
- `capabilities` is a list of strings; a client shows a feature only when its capability is present. v1: `status`, `mesh`, `mesh.activity`, `mesh.send` (present since the gateway serves §8; a client with the `mesh_send` scope offers sending only when it is there), `capture` (absent until amendment 1 is implemented and PacketShark is in the image).
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
| `gateway.subscribe` | `topics[]` (at most 8 names) | `{topics[], upstream{meshcored}}`: the topics now held, and - when one is held - whether meshcored is connected now. Replaces the previous set. Topics: `device`, `mesh`, `mesh.activity`, `capture`; one the client's scopes do not allow, one this build does not serve and one it does not know are left out |

Events: `gateway.scopes {scopes[]}` when the unit reduced them; `gateway.revoked {}` immediately before the gateway closes a revoked client; `gateway.upstream {service, connected}` when a service behind the gateway goes away or comes back. For meshcored it goes to the clients holding a mesh topic, and with `connected: true` it carries `run_id` and `journal_id`: the moment to call `mesh.sync`.

A subscription belongs to the connection: a new connection holds no topic. A topic is lost when the unit takes away the scope it needs (`gateway.scopes`), and does not return with the scope: the client subscribes again.

A parameter object with a member the method does not take is code 2, for every method of §5 to §8 (the gateway is an allowlist of parameters too).

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

As implemented (M4.1):

- `gap: true` also answers an `after_seq` newer than `seq` (a cursor this journal never gave). With `gap`, what lay between is lost to this client; the answer starts at `first_seq`.
- `more: true` means entries after the last one returned are held: ask again with that entry's `seq`. An answer holds at most `limit` entries and stays under the frame limit, so it can hold fewer than `limit` with `more` set. Without a cursor the answer is the newest entries that fit and `more` is false.
- When meshcored starts again the entries of the old run are discarded at once (its ids would collide); `seq` goes on, so every older cursor is a `gap`. What a client had not yet taken of the old run is lost to it.
- A repeated event, and a snapshot entry that says what the journal already holds as the newest state of that message, get no `seq` and raise no event. After a lost connection to meshcored within one run the gateway takes `mesh.messages` again and journals only what changed or is new.
- `mesh.sync` is answered from memory, also while meshcored is away. It is code 9 only until the journal has been filled once for the run it is in.
- The gateway reads the mesh only while at least one paired client holds `mesh_read`. When the last one loses it (revoked, unpaired, scopes reduced) the connection to meshcored is closed and the journal is emptied; `journal_id` is kept and the sequence goes on with one `seq` left out, so every cursor from before is a `gap`.
- A meshcored that sends nothing at all while a request waits out its 2 s is treated as gone: `gateway.upstream {connected: false}`, then `true` when it is back.
- Pipelining is allowed, and bounded: a client with more than 32 KiB of answers it has not taken is not read from until they are written, so several large answers asked for at once arrive one after another.
- `mesh.nodes`: one upstream list serves the pages asked for within 5 s, so the pages of one reading belong together. `offset` past the end is an empty page.
- `mesh.identity`, `mesh.node`, `mesh.channels` and `mesh.path_preview` have 2 s to be answered by meshcored (code 9 after that, and at once while it is not connected); an upstream error keeps its code (1 to 6) with the message "upstream error". Results are reduced to the fields `docs/api/mesh.md` documents, by name.

Events (topic `mesh`): `mesh.message {seq, run_id, journal_id, message}`; `mesh.node {reason, node}`, at most one per node every 10 s except `removed` (the first at once, the newest of what followed when the 10 s are over); `mesh.channel {reason, channel}`; `mesh.state {state, reason, mono_ms}`.

Events (topic `mesh.activity`): `mesh.activity_bin {start_mono_ms, rx_frames, rx_bytes, tx_ok, tx_failed, tx_unknown, by_type{...}}` once a second while there was activity. Counts only; the per-frame `mesh.activity` and the receive log `mesh.rx` are not passed on in v1. `tx_ok` counts frames that went on the air (meshcored's `ok` and `rx_resume_failed`), `tx_failed` those that did not (`tx_failed`, `refused`), `tx_unknown` the rest.

Delivery: `mesh.message`, `mesh.channel`, `mesh.state` and `gateway.upstream` are never skipped; a client whose unread output would pass 256 KiB with one of them is disconnected, and syncs when it returns. `mesh.node` and `mesh.activity_bin` are skipped for a client with more than 128 KiB unread. No other event of meshcored is passed on.

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

As implemented (M4.2):

- **The scope.** Both methods need `mesh_send` and nothing else opens them: with `mesh_read` alone they are code 7, before anything about the request is looked at. The scope is looked up on every request and once more at the moment a message is passed to meshcored after a lookup (below).
- **The request.** `to` is a whole public key (64 hex digits; a prefix is code 2). `channel` is a whole number from 0 to 255; meshcored says which slots exist. `text` is 1 to 160 bytes of well-formed UTF-8 with no control character but newline and tab - not the C0 set, DEL or the C1 set (U+0080 to U+009F); a channel's shorter `text_limit` is meshcored's to refuse (code 2). `client_msg_id` is 16 to 36 characters of `A-Z a-z 0-9 - _` (a UUID fits). `via` is as in `mesh.path_preview`: an array, also an empty one, of at most 63 whole keys, with `to` only. What meshcored receives is written again from the checked values: `to` or `channel`, `text`, `via`; for `mesh.resend`, `resend` with the message id.
- **The answer** is `accepted`, `message_id`, `run_id`, `duplicate`, and what meshcored said of `route`, `ack_expected`, `ack_timeout_ms` (absent for a channel), `channel` (a channel), `resent` (a resend). The `mesh.message` event of the message can arrive before the answer; both name it by `message_id` within `run_id`.
- **A `client_msg_id` belongs to one message.** The same id with another destination, text or route is code 2 and nothing is sent. The same id while its first request still waits for meshcored is code 5, unless the connection that asked is gone: then the repeat takes its place and gets that answer (`duplicate: true`).
- **What is remembered**: per paired client its last 64 ids, in memory, for as long as the gateway runs and the client stays paired. A record is replaced oldest first, never while it waits for meshcored, and one whose outcome is settled before one whose outcome is not known: an unresolved id is forgotten only when all 64 are unresolved. A gateway restart forgets them (`boot_id`/`journal_id` change; `docs/design/companion/PAIRING.md` §7).
- **Refused by meshcored** (its own codes 1 to 6, the message "upstream error"): nothing was sent and the id is not remembered; the same id may be tried again. Code 5 is the upstream limit of this section or a radio that is not available.
- **No answer** from meshcored in 2 s, or its connection lost meanwhile: code 9, and whether it took the message is not known. Code 9 at once, with nothing remembered, when meshcored is not connected. A repeat of an id whose outcome is not known is never passed on directly. The gateway first asks meshcored for `mesh.messages`:
  - the message is there (an outgoing message to the same node or channel with the same text - on a channel exactly `<sender_name>: <text>` - with an id above the highest the gateway had seen when it asked, and not the message of another recorded send): the answer is made from it, `duplicate: true`, without `ack_timeout_ms`; nothing is sent;
  - the message is not there, the list came over the same connection to meshcored that the message was passed on (there meshcored reads the request before the question), no other outgoing message to that node or channel came after, and the list still reaches back to before the gateway asked (of the message's kind, direct or channel, it holds one the gateway had seen before, or fewer than 16 newer ones: meshcored keeps 64 of each kind and loses the oldest first): it was not taken, and it is sent now - once the client's scope has been looked up again;
  - anything else: an outgoing message to the same node or channel came after with a text that is not the same (meshcored reports text as it sanitised it, so it may be the one), the list does not reach back that far or cannot be read, the gateway's connection to meshcored was made anew since (it is given up when meshcored sends nothing at all for 2 s, and a request may then lie unread in the old one), or meshcored has started again (`run_id` changed): code 3, "outcome unknown". The id is never sent again; whether to send the message as a new one is for the person to decide.
  For `mesh.resend` the message's `attempts` decide: one more than before the resend was asked for means it was taken; the same number means it was not; a message that is no longer in the list, or whose attempts the gateway had not seen, is "outcome unknown".
- A message the unit's own RIFT sent to the same place with the same words while the answer was missing is taken for the client's: the client is told `duplicate: true` and its own copy is not sent. The error is on the side of not transmitting twice.
- An answer of meshcored that says `accepted` without a usable `message_id` is code 3, "outcome unknown", and the id is never sent again.
- `mesh.resend` with a `run_id` other than the current one is code 3 without asking meshcored. Which message can be resent is meshcored's rule (an outgoing direct message of this run that is `no_ack`); its refusal is code 2.
- **Rate limit**: 20 in any 60 s per paired client, across its connections, counted when a request is passed to meshcored (a send, a resend, or the lookup for an unanswered one); the 21st is code 10. Answers from the record are not counted. At most 8 sends of one client wait for meshcored at a time (code 5 beyond). `gateway.hello` names the limit as `limits.send_per_min`.
- While no paired client holds `mesh_read` the gateway has no connection to meshcored (§7); `mesh_send` is only ever held with `mesh_read`.

## 9. Capture (`capture_view`, `capture_control`)

Only with ADR-016 amendment 1. Served by the PacketShark app; the gateway passes these four methods to the shell's socket and no other.

| Method | Scope | Result |
| --- | --- | --- |
| `capture.status` | view | see below |
| `capture.hold` `{on}` | control | `{state, changing}`: the state when asked, and whether the capture was told to change (false when it is already in the state asked for). Error 2 unless `on` is true or false; error 3 when nothing is capturing |
| `capture.stop` | control | `{state, changing}`; error 3 when nothing is capturing |
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
- `state`: `idle`, `capturing`, `hold`, `saving`, `stopped`, or `ended`.
- `reason`, the same words as on the unit's own socket (`docs/api/shell.md`, "PacketShark: `capture.*`"):

| `reason` | With `state` | Meaning |
| --- | --- | --- |
| `none` | any | no reason: setup, or a capture that is running or held |
| `user` | `stopped`, `saving` | STOP, tapped on the screen or asked by `capture.stop` |
| `limit` | `stopped`, `saving` | the capture's own time limit |
| `link` | `stopped`, `saving` | the interface went down or lost its carrier |
| `end` | `stopped`, `saving` | a replayed file ended (the simulator and the tests) |
| `error` | `stopped`, `saving` | the capture source failed |
| `helper_crashed` | `ended` | the helper is gone, however it went; its packets with it |
| `helper_hung` | `ended` | the helper stopped answering and was ended |
| `left` | `ended` | PacketShark was left with a capture in it, which ends the capture. Only ever in a `capture.state` event: afterwards the app is not there to be asked |
| `refused`, `timeout` | (none yet) | reserved for the outcome of a start request (ADR-016 amendment 1 rule 1a). Nothing raises them |

- Every figure is a count, a rate or a size. **No field identifies a host**; ADR-016 rule 11a lists what may be here.

Events (topic `capture`): `capture.state` `{state, reason, iface, keep, limit_s}` on every change; `capture.stats`, the figures above, once after each change of state that has figures (unless it changes again first) and once a second while capturing or held. Figures never come before their state; at most four of each in a second, the last `ended`/`left` aside.

**Where this stands.** PacketShark serves `capture.status`, `capture.hold`, `capture.stop` and the two events on the unit's own shell socket (`docs/api/shell.md`). The gateway passes none of them on and knows no `capture.*` name: this section still binds nothing until ADR-016 amendment 1 is accepted. `capture.request_start` is not implemented anywhere.

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
