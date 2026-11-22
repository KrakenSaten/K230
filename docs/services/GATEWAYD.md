# gatewayd: the Companion Gateway (M2 core, M4.1 mesh reading, M4.2 sending)

Status: **host-tested and cross-built only.** Built by name (`make gatewayd`), not in `make all`, not in any image, not in the rootfs overlay. **No part of it has run on a K230, and no phone app exists yet.** Design: `docs/decisions/ADR-019-companion-gateway.md` (accepted 2026-11-21); protocol: `docs/api/gateway.md`; pairing: `docs/design/companion/PAIRING.md`.

gatewayd is the one Doors service that listens on the network. A paired phone reaches it over TLS 1.3 on TCP 47231 and may call an allowlist of methods; gatewayd answers them as a pocketipc client of the services that own the data. It owns no radio, no mesh key, no Wi-Fi credential and no capture.

```
 phone ── TLS 1.3, both keys pinned ──▶ gatewayd ──┬─ sysd      system.info, system.status
                                          │        ├─ netd      wifi.status
 Settings / doors ── gatewayd.sock ───────┘        ├─ radiod    radio.status
 (root only: pair, decide, scopes, revoke)         └─ meshcored mesh.status; M4.1: one kept connection,
                                                              subscribed, for reading the mesh
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

**Not in M2:** `gateway.subscribe` and every event except `gateway.scopes` and `gateway.revoked`; the mesh group; the capture group. All five scopes are defined, granted, stored and changed since M2, so later milestones add table rows and not a permission model. `gateway.hello` does not yet carry `model` or `api`.

## What M4.1 adds: reading the mesh

Served to a paired client with the `mesh_read` scope: `mesh.identity`, `mesh.nodes` (in pages), `mesh.node`, `mesh.channels`, `mesh.path_preview`, `mesh.sync`; and to any paired client `gateway.subscribe`, whose topics `mesh` and `mesh.activity` need `mesh_read`. `capabilities` is `["status", "mesh", "mesh.activity"]`. The wire details are `docs/api/gateway.md` §5 and §7.

| Part | File | What it does |
| --- | --- | --- |
| The connection to meshcored | `gw_uplink.c` | A pocketipc client that never blocks: connects or tries again later (250 ms to 4 s), reads everything as it comes, matches answers to requests by id, gives each request 2 s |
| The journal | `gw_journal.c` | The last 512 messages under the gateway's own `seq`; the cursor of `mesh.sync`. Pure, memory only |
| Bounding the events | `gw_feed.c` | `mesh.node` at most once per node per 10 s; `mesh.activity` as one-second counts. Pure, clock passed in |
| Rows, filters, parameters, topics | `gw_policy.c` | Seven table rows; every result and event reduced to named fields; every parameter checked and written again |
| The mesh section | `main.c` | meshcored's events into the journal and to subscribers; requests without waiting; the limit of 8 waiting requests per client |

**Still not served** (each "unknown method"): the capture group (ADR-016 amendment 1 is still proposed), and the topics `device` and `capture`, which a subscription leaves out.

## What M4.2 adds: sending

Served to a paired client with the `mesh_send` scope: `mesh.send` (to a node, with or without a route, or to a channel) and `mesh.resend`. `capabilities` gains `mesh.send`; `gateway.hello` names `limits.send_per_min`. The wire details and every rule below are `docs/api/gateway.md` §8.

| Part | File | What it does |
| --- | --- | --- |
| The request, the records, the rate | `gw_send.c` | Checks what a client asks to have sent and writes it again for meshcored; keeps per paired client its last 64 `client_msg_id` values with what became of them, and the times of its last 20 sends. Pure, memory only, clock passed in |
| Two table rows | `gw_policy.c` | `mesh.send` and `mesh.resend`, by `mesh_send` only |
| The send section | `main.c` | Passes a message to meshcored in one place (`send_forward`); records meshcored's answer whether or not the session that asked is still there; asks for `mesh.messages` before a send whose answer never came is passed on a second time |

One `client_msg_id` reaches meshcored as `mesh.send` once. It reaches it a second time only when meshcored's own list has proven that it did not take the first (§8 says what counts as proof); when that cannot be proven the answer is "outcome unknown" and the id is never sent again. What decides this is in `gw_send.h`, as states of a record:

| State | Meaning | A repeat of the id |
| --- | --- | --- |
| pending | meshcored was asked, no answer yet | code 5, or takes the place of a requester that is gone |
| done | meshcored accepted it | answered from the record, `duplicate: true` |
| (no record) | meshcored refused it with an error of its own, or the request never left the gateway | sent as new |
| uncertain | no answer in 2 s, or the connection to meshcored was lost | `mesh.messages` first: found, proven absent (then sent), or unknown |
| unknown | cannot be found out: meshcored started again, the connection to it was made anew since the message was passed on, or its list proves nothing | code 3, never sent again |

In practice an unanswered send ends as "found" or "unknown": the gateway gives its connection to meshcored up when meshcored sends nothing at all for 2 s, and after that nothing is proven absent. "Proven absent, then sent" needs a meshcored that went on talking and did not answer that one request.

Delivery states need nothing new: the message meshcored makes for a send arrives as `mesh.message` events and in `mesh.sync` like every other (M4.1), under the `message_id` and `run_id` of the answer.

A send is logged as an event without its content: the log says that a send got no answer, or that it was passed on again after a lookup; never a text, a key or an id.

How it behaves:

- **Read only while somebody may read.** The gateway connects to meshcored only while at least one paired client holds `mesh_read`. When the last one loses it, the connection is closed and the journal emptied.
- **What it asks meshcored for, itself:** `mesh.status` (for the `run_id`), `mesh.subscribe` with no parameters (so never the receive log), `mesh.messages`. For a client: the five reading methods, by their table name, with parameters the row's own check wrote. Nothing else, and nothing that changes the node; `tests/gatewayd_lint.sh` holds the text of that and the service suite checks what a fake meshcored was asked.
- **meshcored is not changed and not held up.** The gateway is one more subscriber, as RIFT is. It reads its socket in every turn of the loop whether or not a client is connected, so meshcored's 200 ms backpressure rule has no cause to fire. A client that does not read is disconnected at 256 KiB of unread output; meshcored does not notice.
- **Restarts.** A new `run_id` empties the journal (meshcored's message ids start again) and is announced with `gateway.upstream`. A lost connection within one run is repaired from `mesh.messages`: only what changed or is new gets a `seq`. A gateway restart is a new `journal_id`. A meshcored that reports no usable `run_id`, or answers the three calls wrongly, is dropped and tried again; the mesh group answers code 9 meanwhile.
- **Nothing secret.** meshcored reports no channel key and no identity key (`docs/api/mesh.md`). The gateway does not rely on that alone: each result and event is reduced to the fields that document names, and the scrub of secret-named members runs behind it. No message text, node name, key or `run_id` is logged.

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

**Setting that password opens root SSH by password on a stock image.** The 0.4.0 image runs OpenSSH on `0.0.0.0:22` and `[::]:22` with `PermitRootLogin yes` and `PasswordAuthentication yes`, and the empty root password it ships with is what keeps that login shut. Meeting the password gate on such a unit therefore makes root reachable over SSH with that password from the LAN and from any global IPv6 address. Before enabling the gateway on a unit, set `PermitRootLogin prohibit-password` in `/etc/ssh/sshd_config` and restart sshd; image integration and the device-password flow (U7) must ship that setting (or stronger). VERIFIED in M5 on Unit A, 2026-11-21: with the line changed, root's real password was refused over IPv4; a refusal over IPv6 was not observed (the attempt ended before authentication).

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
doors call gatewayd gateway.pair_decide allow=true
doors call gatewayd gateway.clients           # compare the new fingerprint with the phone
doors call gatewayd gateway.revoke client=<64 hex>
```

**The candidate's fingerprint is not shown before the decision on the bench.** The service announces a candidate only as the `gateway.candidate` event, which a client subscribed with `gateway.subscribe` that stays connected receives. `doors call` sends one request and exits, so it never sees the event, and `gateway.status` does not carry the candidate. On the bench the decision therefore rests on the 120 s window, a single candidate and the one-time token. Check it straight after `pair_decide`: the fingerprint `gateway.clients` lists for the new client must match "This phone's key" on the phone's pairing screen; if it does not, revoke that client. Showing the candidate before the tap is the Settings page's job (`docs/design/companion/PAIRING.md`). VERIFIED in M5 on Unit A, 2026-11-21.

Read `device_id` from `gateway.status` immediately before opening a window when it is compared by hand with the phone's view of the unit: removing the state directory gives the unit a new key on the next start (M5 compared against a stale value this way; the QR carried the correct key).

`gateway.pair_decide` takes `scopes` as an array, which `doors call` cannot express; without it a client gets `status`, `mesh_read`, `mesh_send`. The Settings page is the real client of this API (`docs/design/companion/CONTRACTS.md` §3).

## Limits

8 paired clients; 2 sessions at once (a second connection of the same client replaces its first); 8 handshakes in progress and 4 new ones a second; 64 KiB per request frame; 256 KiB per reply and of unread output; 40 requests at once then 20 a second per client (code 10 beyond); 10 s to finish a handshake and 10 s more to send `gateway.hello`; 90 s idle; 2 s for a connection that is being ended to take its last message. An upstream call has 100 ms to connect and 300 ms to answer, and during it the service serves nobody else; a service that failed to answer is not asked again for 1 s (code 9 at once meanwhile).

The mesh group (M4.1): 16 requests of one client in one turn of the loop; no further request read from a client with more than 32 KiB of answers it has not taken; 8 requests of one client waiting for meshcored at a time (nothing further is read from that client until one completes), 16 of all clients together (code 5 beyond); 2 s for meshcored to answer one; 512 journaled messages of at most 8 KiB each as JSON (a larger one is not journaled and not sent); 100 messages and 192 KiB in one `mesh.sync` answer; 100 nodes in one `mesh.nodes` page; 128 nodes tracked for `mesh.node` at a time (an advert of a further node inside one 10 s period is not sent as an event; `mesh.nodes` still has it); 16 type words in one activity bin; a request to meshcored that cannot be written within 2 s ends that connection.

The service runs at nice 5, and does not start at all unless every capability was dropped and read back as dropped.

## Logging

`gatewayd.log` in the Doors log directory: lifecycle lines only. A client is named by the first 16 hex digits of its fingerprint and its source address. Never a token, a pairing URI, a client's name, a request, a result or key material.

## Building and testing

```sh
make gatewayd            # services/gatewayd/gatewayd; needs OpenSSL 3 and cJSON
make gatewayd-test       # unit suites (plain, ASan/UBSan), fuzzer, the real service
                         # against TLS clients (plain, ASan/UBSan), the lint
make gatewayd-send-test  # the sending suite alone (gatewayd-mesh-test: the reading suite)
make CC=riscv64-unknown-linux-gnu-gcc gatewayd      # with the SDK's host/bin on PATH
make ENABLE_GATEWAYD=1 install                      # also installs it and S70gatewayd
```

The service suite runs a build with `-DGATEWAYD_TEST_HOOKS=1`, the only one that accepts `--test-listen`, `--test-port` and `--test-shadow`; the shipped binary does not contain those options (the lint checks its strings).

## Evidence

Classes as in AGENTS.md. Measured 2026-11-21 on the build host (x86-64, WSL Ubuntu 22.04, GCC 11.4, OpenSSL 3.0.2) unless it says otherwise.

| Claim | Class |
| --- | --- |
| Pure modules, identity handling: 1544 checks (528 of M2; M4.1 adds the mesh rows, filters, parameters and topics, the journal and its cursor, the event bounds) | VERIFIED host, plain and ASan/UBSan (`tests/gatewayd_unit_test.c`) |
| Frame reader, request parser, paired-list loader, gate, and since M4.1 the mesh parameter checks, filters, feed and journal: 200,000 fuzzed inputs, no fault | VERIFIED host, ASan/UBSan (`tests/gatewayd_fuzz.c`). **Not fuzzed:** `gw_uplink.c` (its reader is pocketipc's) |
| **M4.1.** The real service between a fake meshcored and TLS clients: not read while nobody holds `mesh_read`; what meshcored is asked for and in which order; the scope on every method and topic; filters; parameters; pages of `mesh.nodes`; every event and what is not passed on; the journal, its cursor, a repeated event; meshcored lost and back in the same run and in a new one; the ring wrapping; a meshcored that does not answer (given up and taken up again) and the limit of waiting requests; a client that does not read; five large answers pipelined; a meshcored that answers wrongly; a scope taken away and a client revoked mid-request; log contents: 90 checks | VERIFIED host, plain and ASan/UBSan (`tests/gatewayd_mesh_e2e_test.py`) |
| **M4.2.** `gw_send.c`: the request checks, every state of a record, the absence proof, the rate window, eviction (unit suite now 3065 checks in all); its parser and records in the fuzz run | VERIFIED host, plain and ASan/UBSan (`tests/gatewayd_send_unit.inc`, `tests/gatewayd_fuzz.c`) |
| **M4.2.** The real service between a fake meshcored and TLS clients, counting what the fake was made to transmit: the scope on its own; request checks; direct, routed and channel messages; delivery states as events and in `mesh.sync`; repeats from the record; meshcored refusing; an answer lost with the message taken, and not taken; the connection to meshcored given up meanwhile (never sent again); a client gone while a send waits; a second connection taking the first one's place; a scope taken away during the lookup; meshcored away and back as another run; resend in the same cases; the rate limit per client; log contents: 73 checks | VERIFIED host, plain and ASan/UBSan (`tests/gatewayd_send_e2e_test.py`) |
| **M4.2.** That the real meshcored reads a connection's requests in order, records an outgoing message the moment it accepts it, hands out ids in order, keeps 64 messages of each kind and loses the oldest first, and sends nothing when it answers with an error | DOCUMENTED (read in `services/meshcored/api.c`, `mesh_runtime.cpp`, `core/pocketipc/server.c`; the ring sizes are held by the lint). Not run against |
| **M4.2.** A message actually on the air, and a phone as the client | **not tested** |
| **M4.1.** The real meshcored | **not tested**: the suite's meshcored is a fake written from `docs/api/mesh.md` and `services/meshcored/api.c`. That the real service's results and events have the documented shape, that it writes an event before the answer to a later request, and its 200 ms backpressure rule against this client are ASSUMED |
| The real service against TLS clients: gate, closed until asked, pairing, failed handshakes (no certificate, TLS 1.2, RSA, P-384, unknown key, two certificates, plaintext), no resumption, scopes, revocation, malformed and oversized frames, limits, upstream failures, restart, damaged and missing state, log contents: 190 checks | VERIFIED host, plain and ASan/UBSan (`tests/gatewayd_e2e_test.py`) |
| The fingerprint equals the SHA-256 of the SubjectPublicKeyInfo as the `openssl` tool writes it | VERIFIED host (unit and service suites) |
| riscv64 build: warning-free with `-Werror`, Xuantie GCC 14.1.1, against the v0.4.0 release build's sysroot; links `libssl.so.3`, `libcrypto.so.3`, `libcjson.so.1`, all present in that build's target tree | VERIFIED (cross-build only) |
| The v0.4.0 sysroot's OpenSSL is 3.4.1 with none of `OPENSSL_NO_TLS1_3`, `_EC`, `_ECDSA`, `_ECDH` defined | VERIFIED (headers); the library's behaviour on a unit is not |
| Anything on a K230: handshake cost on the one hart, capabilities dropped with a listening socket, the entropy source at first start, the state directory across a power cut, `/etc/shadow` as the image has it, TCP 47231 free, Wi-Fi power saving and idle sessions | **not tested** (`docs/design/companion/README.md`, H2 to H8, H11) |
| A phone client | **not tested against**: every client in these suites is the Python reference client |

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

## Independent review of M4.1 (2026-11-21)

A second reviewer, given the change and the specification and not the author's conclusions, read the new modules and the mesh section of `main.c` for authorization, data exposure, memory safety, re-entrancy in the loop, bounds and liveness, cursor semantics and fail-closed behaviour. It built and ran nothing.

**No blocker:** no path was found that gives mesh data to a client without `mesh_read`, no field reaching a client that is not on a list, no parameter or method reaching meshcored that the table does not name, and no memory-safety defect in the new code.

Fixed before the commit:

| Finding | Fix |
| --- | --- |
| A client that pipelined requests for large answers (ten pages of `mesh.nodes`, two `mesh.sync`) ran into the 256 KiB output limit and was disconnected, again after every reconnect | No further request is read from a client with more than 32 KiB unsent; waiting `mesh.nodes` pages are handed out as its output is written. Tested with five pipelined `mesh.sync` answers |
| One client sending small requests as fast as the unit takes them kept the loop inside its session: meshcored unread, the other client and the local socket (revocation) unserved | At most 16 requests of one client per turn of the loop |
| A meshcored that stopped answering was never noticed: the mesh stayed "connected", the journal was served as if live | A request whose deadline passes with not one byte from meshcored since it was asked ends that connection: clients are told, and it is made again. Tested |
| After the journal was emptied (nobody held `mesh_read`), a cursor equal to the last `seq` got no `gap` although messages may have passed meanwhile | One `seq` is left out when the journal is emptied, so every earlier cursor is a `gap` |
| A `mesh.node` removal was skipped, and never repeated, for a client that was behind on reading | A removal is sent like a message: never skipped |
| An event arriving before the subscription was answered would have been journaled under the previous run | Events are taken only after `mesh.subscribe` was answered on that connection |
| A failed `mesh.subscribe` after a run change left the emptied journal marked as filled | The order was changed |

Left as they are, and listed under the limits below: no `gap` when more messages were missed during a lost connection than meshcored still holds; a held `mesh.node` that came due while the only subscriber was behind is not sent later; the places for waiting requests outlive a session that left for up to 2 s; the types of result fields are not checked, only their names; a possible short busy turn when TLS wants to read during a write while a client is at its limit of waiting requests (bounded by 2 s, not reproduced).

The reviewer could not assess, and nobody has: anything at run time on a unit; whether a phone can send faster than the one hart processes; the real meshcored's ordering of events and answers; OpenSSL's behaviour on a key update.

## Independent review of M4.2 (2026-11-21)

A second reviewer, given the change and the specification and not the author's conclusions, read `gw_send.c`, the send section of `main.c`, `gw_uplink.c`, meshcored's send path and pocketipc's server loop for duplicate transmissions, authorization and the correctness of answers and delivery states. It built and ran nothing.

**No certain duplicate transmission and no authorization fault was found.** Sound as read: the scope on every request and again before a message is passed on after a lookup; records and rate window per client key; one answer per request and the count of waiting requests on every path; nothing a client sent reaching meshcored unchecked; no memory-safety defect in `gw_send.c`.

Fixed before the commit:

| Finding | Fix |
| --- | --- |
| The author's own, before the review returned: "the list reaches back" was judged by the lowest id of the whole list, but meshcored keeps direct and channel messages in two rings | Judged per kind: a message of that kind from before, or fewer than 16 from after |
| The same: an outgoing message to the same place with another text was "not it", although meshcored reports text as it sanitised it | It is "may be it": outcome unknown |
| Plausible, not shown: after the gateway gave its connection to meshcored up, a request could lie unread in the old one while the list came over the new one, be judged absent, sent again, and then be read | Absence is proof only over the connection the message was passed on; otherwise outcome unknown. Tested |
| An id whose outcome was not known was forgotten after 64 further sends like any other, and its repeat then sent as new | Settled records are replaced first; an unresolved one only when all 64 are unresolved. Tested |
| On a channel any text ending in `: <text>` was taken for the message | Exactly `<sender_name>: <text>` |
| Without memory for the answer of an accepted send the client got code 4, which elsewhere means "refused, nothing sent" | Code 9; a result that could not be built whole is "outcome unknown" |
| The fuzz run never reached the list matching or an accepted answer | Both are fed what the fuzzer made, with a seed of each |
| Nothing held the assumption about meshcored's ring sizes | A lint line |

Left as they are, and listed under the limits below: a late answer is not used; a wrong "duplicate" when the same words went to the same place from elsewhere; "outcome unknown" more often than strictly needed.

The reviewer could not assess, and nobody has: the real meshcored's timing; whether its explicit-route send can answer with an error after a packet was already queued (`mcd_runtime_send_explicit_text`, where the vendor's `sendMessage` returned something other than a direct send); that `run_id` differs for every start.

## Known limits of M4.2

- **At most once is the gateway's, per `client_msg_id`, while it runs.** A gateway restart forgets every record (`PAIRING.md` §7: the app looks in `mesh.sync` and asks the person). meshcored itself knows no such id; giving it one would close the remaining gaps and is a change of its API, not made here.
- **"Outcome unknown" is the usual end of a send that got no answer and was not taken.** After 2 s of silence the connection to meshcored is made anew, and from then on absence cannot be proven. The person decides whether to send the message as a new one.
- **A late answer is not used.** meshcored answering after the 2 s leaves the record uncertain; the repeat then finds the message in the list (or gets "outcome unknown" if another message to the same place followed).
- **A message from elsewhere can be taken for the client's.** While a send is uncertain, the same words to the same place sent by RIFT, or by another client whose record is gone, answer the repeat with `duplicate: true` although the client's own copy was never sent. Two uncertain sends of one client with the same words to the same place can get each other's `message_id`. The number of transmissions is right in each case.
- Any other outgoing message to the same node or channel after an uncertain send makes that send "outcome unknown" for good, also when it was not taken.
- An uncertain or unknown id is forgotten when 64 unresolved ids of that client are remembered, and a removed client's records go when a ninth client needs room; a repeat after that is a new message.
- The rate limit counts a refused send and a lookup like a send: a client that meshcored refuses (code 5) twenty times in a minute waits like one that sent.
- Text is refused more narrowly than meshcored would (the C1 controls too), and a channel's own `text_limit` is only known to meshcored: its refusal is code 2 with no number in it; the client reads `text_limit` from `mesh.channels`.

## Known limits of M4.1

- **A cursor is exact only while meshcored still holds what was missed.** After a lost connection within one run the journal is repaired from `mesh.messages` (64 direct and 64 channel messages). If more than that passed meanwhile, the rest is lost and the answer carries no `gap`.

- **A meshcored restart loses what a phone had not yet taken.** The journal is emptied when the `run_id` changes, because the new run's message ids collide with the old ones and `mesh.sync` names one run per answer. A phone that was connected had everything as events; one that was away loses up to 512 messages of the old run. Keeping them would need a `run_id` per entry in `mesh.sync`: a protocol decision, not taken here.
- **meshcored holds 64 direct and 64 channel messages.** After a gateway restart, or after the first `mesh_read` client is paired, the journal starts from those; older ones are not anywhere.
- Each state change of a message is its own journal entry (as the protocol says), so a busy conversation with ACKs uses the 512 places about twice as fast as its message count.
- The device-status methods of M2 still call their service synchronously (below), `mesh.status` included, which therefore has a second, short-lived connection to meshcored. While such a call blocks (up to 400 ms) no event is read; meshcored's socket buffer holds them.
- `gateway.hello` carries no `api` object yet: a client cannot read meshcored's `api_version` from it, only from `mesh.status`.
- The `device` topic (`device.status`) is not served.
- Strings from the mesh are passed on as meshcored sanitised them; the gateway does not check their encoding again.
- A paired client can keep the 16 places for waiting requests busy for 2 s at a time while meshcored is not answering, and the other client then gets code 5 for mesh methods. It cannot stall the loop.

## Known limits of M2

- The device-status calls block the loop for up to 400 ms each. Acceptable for status reads by two clients; the mesh group has its own non-blocking client since M4.1.
- IPv4 only.
- A phone that is already paired and scans a new code connects as itself, not as a candidate; changing its scopes is `gateway.set_scopes`.
- While a pairing window is open, someone on the network can hold the door (one candidate at a time, 30 s without a token, then again) and so keep the owner's phone out for that window. It cannot pair, and it cannot close the window: only three presented wrong tokens do.
- The 8 handshake slots and the 4-a-second budget are shared by all sources. One host on the network can keep them busy and keep phones out for as long as it tries (THREAT_MODEL T4); sessions already open are not affected.
- When the paired list cannot be written (a full or read-only card), a revocation, a scope change or an unpairing is in force in memory at once, `gateway.status` reports `store_dirty`, and the write is retried every 5 s. If the service restarts before a retry succeeds, the old list on storage returns.
- The service stays uid 0 with every capability dropped and `no_new_privs` set (ADR-019 rule 8).
