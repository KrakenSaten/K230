# mesh.* API v0 (meshcored)

Status: draft, api_version 0. Transport: pocketipc, socket `meshcored.sock`.

`meshcored` owns the MeshCore protocol runtime. It owns no radio: every byte
in and out goes over radiod's generic `radio.*` IPC (ADR-002,
docs/api/radio.md), and this service opens no SPI device, drives no GPIO and
links no radio library.

```
SX1262  ->  radiod  ->  [radio.*]  ->  meshcored  ->  [mesh.*]  ->  a client
```

This API is **protocol-oriented, not screen-oriented**. It names what a
MeshCore service knows - its identity, the nodes it has heard, the messages it
has exchanged, the state of its radio relationship - and says nothing about
how any of it is drawn. There is no ordering shaped around a list widget, no
pagination shaped around a screen height and no RIFT vocabulary. A client
decides all of that.

Two rules hold throughout the results:

- **No internal structure escapes.** Every value is a copy of a plain field,
  and a node is named by its public key, which is a fact about the node rather
  than an index into a table this service may reorder.
- **A value that is not known is absent.** An RSSI nobody measured is not
  reported as 0, a hop count nobody learned is not reported as 0 hops, and a
  radio state nobody has told us is not reported at all. This is the same rule
  `radio.channel` follows, for the same reason: "unknown" and "quiet" are
  different answers, and a client drawing a bar from a fabricated zero would
  be showing a measurement nobody made.

## Evidence

Everything below is **VERIFIED host** - by `tests/meshcored_service_test.sh`
against the real radiod on its mock backend, and by
`tests/meshcored_harness_test.sh` between two whole meshcored processes over a
mock air - unless a line says otherwise. **No part of this service has run on
unit A, and nothing here has been on a radio.** The MeshCore wire format it
speaks is the one the accepted P0 gate proved on air
(docs/hardware/MESHCORE_INTEROP_GATE.md), from the same pinned sources; that
gate is evidence about the frames, not about this daemon.

## Service state

`mesh.status` reports one of seven states. The order below is the start-up
order, and every transition is announced as a `mesh.state` event carrying the
state and a `reason` in plain words.

| State | What it means |
| --- | --- |
| `starting` | before the first connection attempt; seen only at start-up |
| `waiting_for_radiod` | not connected to radiod; retrying with backoff |
| `waiting_for_lease` | connected, but the radio lease is held elsewhere |
| `configuring` | lease held; applying the profile and subscribing |
| `online` | profile applied, subscribed, lease held, radio receiving |
| `degraded` | attached and holding the radio, but it is not usable |
| `error` | radiod refused the profile; retrying will not change that |

Which one a failure produces is a judgement about what the failure means:

- **radiod is not there** is not an error. It may not have started yet, or it
  may be restarting. The service waits, doubling a 0.5 s backoff to a 30 s
  ceiling, and says `waiting_for_radiod`.
- **the lease is held elsewhere** is not an error either, and is never
  answered by taking the radio. The service waits with the same backoff and
  says `waiting_for_lease`. Nothing in meshcored can take a lease from another
  owner.
- **the radio is not receiving** - radiod reporting state `error` after a
  failed receive re-entry, or a `radio.tx_done` saying the packet went out but
  the receiver did not come back - is `degraded`. The protocol runtime keeps
  running and can still transmit, which is what radiod's own documentation
  says a send does in that state. A later `radio.state` of `rx` returns it to
  `online`, and nothing else does: a service that is `degraded` stays
  `degraded` while it transmits, because its own voice is not evidence that
  the receiver came back.
- **this service's own transmit is not** `degraded`. radiod reports state
  `tx` for the whole airtime of a packet, and while meshcored holds the lease
  that packet is its own: the service stays `online` and raises no
  `mesh.state` event, so a client is not told the mesh is unusable every time
  this node speaks. The test is the submission, not the state word. A `tx`
  reported while this service has no transmit outstanding is a radio being
  driven by something it cannot account for, and is `degraded` with a reason
  that says which of the two it is. A submission given up on at its
  completion deadline takes the excuse with it: if radiod is still reporting
  `tx` after that, the service degrades.
- **the profile was refused** (radiod error 3 or 2) is `error`, and it is
  terminal. Asking again with the same values would be refused again: the
  profile comes from the command line, so a reconnect would apply exactly the
  same numbers for ever. The lease is handed back and the connection closed
  before the state changes, so the radio is not held by a service that has
  stopped trying - `mesh.status` then shows `connected: false`,
  `lease_held: false` and no applied profile, and another client can take the
  radio immediately. Whoever changes the profile or radiod's region guard
  restarts the service; until then `mesh.status` keeps answering and says
  why.

`degraded` and `waiting_*` are not the same answer and are not collapsed: one
says the radio is here and not working, the other says it is not here.

## Methods

### mesh.info

Result: `service`, `api_version`, `version`, `build`, `protocol`
(`"meshcore"`), `source` (`rift_commit`, `crypto_commit`), `radiod_socket`,
`requested_profile`.

`source` is which revision of the MeshCore protocol sources this build speaks.
A service whose whole purpose is another project's wire format has to be able
to say what it was built from; these are the commits `protocols/meshcore` pins
and `tools/meshcore-frame` was built from for the accepted P0 gate.

### mesh.status

Result:

| Field | |
| --- | --- |
| `state`, `reason`, `state_since_mono_ms`, `uptime_s` | the service state above |
| `radio.connected` | is there a connection to radiod |
| `radio.lease_held`, `radio.lease_owner_id` | the lease, when held |
| `radio.online` | can the protocol core transmit right now |
| `radio.radio_state` | radiod's own state word - **absent until radiod has said** |
| `radio.profile` | the profile radiod applied - **absent until it has been applied** |
| `counters` | see below |
| `nodes`, `messages`, `packets_free`, `packets_total` | runtime sizes |
| `state_fault` | **present only when the stored node state could not be read at start-up**; see "A node state this service will not read" below |

The counters are deliberately meshcored's own, not the protocol library's:
they count what radiod said, not what MeshCore believes.

| Counter | |
| --- | --- |
| `rx_events` | `radio.rx` events seen |
| `rx_delivered` | frames handed to the protocol core |
| `rx_rejected` | malformed: bad hex, empty, oversized, or no payload field |
| `rx_dropped` | well formed, but the receive queue was full |
| `tx_submitted` / `tx_accepted` / `tx_refused` | `radio.send_async` requests, and radiod's answer |
| `tx_ok` | transmitted, and the radio is receiving again |
| `tx_rx_resume_failed` | **transmitted**, but the radio is not receiving |
| `tx_failed` | **not** transmitted |
| `tx_unknown` | the connection went away, or no completion came within the deadline |
| `tx_done_unmatched` | a `radio.tx_done` for a `tx_id` this service does not hold |
| `radiod_connects` / `radiod_disconnects` | |
| `lease_acquired` / `lease_refused` / `lease_lost` | |
| `sent_flood` / `sent_direct` / `recv_flood` / `recv_direct` | the MeshCore dispatcher's own |
| `path_payloads_refused` | see "The PATH guard" in docs/services/MESHCORED.md |
| `nodes_unretained` | adverts from nodes the 32-slot contact table had no room for |
| `contacts_full` | how often MeshCore reported the table full |

`tx_ok`, `tx_rx_resume_failed`, `tx_failed` and `tx_unknown` are four
different answers and are never collapsed into two. A daemon asking "must I
send this again?" reads whether the bytes went out; calling a transmit that
went out a failure is how a fault in the receive path becomes twice the
airtime.

### mesh.identity

Result: `public_key` (64 hex characters), `node_hash` (the first byte, which
is what MeshCore routes on), `name`.

The private key is not reported by this method or any other, at any verbosity.

### mesh.nodes

Result: `nodes` (array), `count`.

A node:

| Field | |
| --- | --- |
| `public_key`, `node_hash`, `name`, `type` | `type` is MeshCore's `ADV_TYPE_*`: 1 chat, 2 repeater, 3 room, 4 sensor |
| `path_known` | whether a route back is known at all |
| `hops`, `direct`, `path_hex` | **only when `path_known`**; `direct` means zero relays |
| `last_advert_timestamp` | by **their** clock, absent when never seen |
| `last_heard_mono_ms` | by **ours**, `CLOCK_MONOTONIC` |
| `last_snr_db`, `last_rssi_dbm` | **only when radiod reported them for the frame that was heard** |

The signal fields are recorded only while the frame that caused the
observation is the one that turn took off the receive queue. A flood packet
held in MeshCore's delayed inbound queue and processed later arrives with no
metadata attached, and is then recorded as heard with the signal unknown
rather than with the signal of whatever came next.

### mesh.node

Params: `node` - a public key, or a prefix of one, as 2 to 64 hex characters.

Result: one node, in the shape above.

Errors: 2 when the prefix is not hex, is not an even number of characters,
matches nothing, or matches **more than one node**. An ambiguous prefix is
refused rather than answered: the answer is a key-exchange partner, and
picking one would be a guess about which node the caller meant.

### mesh.messages

Params: `limit` (optional). Result: `messages` (array, oldest first),
`count`, `total`, `persistent`.

With `limit`, the **newest** `limit` messages, still oldest first: a client
that asks for ten wants the last ten and wants to read them in the order they
happened.

`persistent` is `false` in this phase and says so rather than leaving it to be
discovered: the message list does not survive a restart. See "What is
persistent" in docs/services/MESHCORED.md.

A message:

| Field | |
| --- | --- |
| `id` | meshcored's own, 1 upwards, never reused while it runs |
| `direction` | `in` or `out` |
| `peer_public_key`, `peer_name` | |
| `text`, `timestamp` | `timestamp` is the **sender's** clock, MeshCore's own stamp |
| `mono_ms` | when this service saw it |
| `state` | `received`, `sent_flood`, `sent_direct`, `acked`, `no_ack`, `failed` |
| `ack_mono_ms` | when the ACK matched; absent until it does |
| `snr_db`, `rssi_dbm` | only when known, by the same rule as a node's |

### mesh.send

Params: `to` (a public key or prefix, as `mesh.node`), `text` (1 to 160
bytes, no control characters other than newline and tab).

Result: `accepted` (always `true`), `message_id`, `route` (`flood` or
`direct`), `ack_timeout_ms`.

**Accepted is not transmitted.** The frame is queued for the MeshCore
dispatcher, goes to radiod as an asynchronous transmit, and its outcome
arrives later as a `mesh.activity` event and in the message's own `state`. The
first message to a node goes `flood`, because no route back is known yet; that
is what the ACK supplies, and the next one goes `direct`.

Errors: 2 for a recipient or text the service will not take, 5 when the radio
is not available (the message text says which state it is in), 4 when the
protocol core could not build the message.

### mesh.advert

No params. Result: `accepted`.

Builds and floods one signed self-advert. Errors: 5 when the radio is not
available, 4 when the runtime could not build one.

**This and `mesh.send` are the only ways meshcored transmits without having
been sent something first.** There is no periodic advert. What it does send
unasked is what the protocol owes a sender: an ACK, and a return path, for a
message addressed to this node.

### mesh.subscribe / mesh.unsubscribe

No params. Result: `{"subscribed": true|false}`. Per connection, cleared by
disconnecting.

## Events

- `mesh.state`: `state`, `reason`, `mono_ms`. One per transition.
- `mesh.node`: `reason` (`discovered`, `path`), `node` (as above).
- `mesh.message`: `message` (as above). Raised when a message arrives, when
  one is sent, and again when its state changes - an ACK matching, or a
  timeout.
- `mesh.activity`: the raw feed, for a client that wants to show the link
  rather than the conversation.
  - `kind: "rx"`: `payload_type` (`advert`, `text`, `ack`, `path`,
    `group_text`, ...), `bytes`, `mono_ms`, and `rssi_dbm`, `snr_db`,
    `frequency_error_hz` **only when known**. One per frame MeshCore parsed,
    including frames that were not for this node.
  - `kind: "tx"`: `submit_id`, `result` (`ok`, `rx_resume_failed`,
    `tx_failed`, `refused`, `unknown`), `bytes`, `mono_ms`.

A client that treats the arrival of a `kind: "tx"` activity as "a packet went
out" will be wrong for three of those five results. Read `result`.

## Remote text

An advert name and a message body are chosen by whoever is on the air. They
are protocol bytes, and MeshCore neither knows nor should care what is in
them - but they must not leave this service as text it vouched for. Two things
in particular:

- **invalid UTF-8** makes the whole IPC frame unparsable, so one hostile
  advert would break every client's read of every event, not only its own;
- **an ESC sequence** is escaped by JSON and decodes straight back out again,
  landing intact in whatever terminal or panel shows it.

So every remote string this API emits - `name` in a node, `peer_name` and
`text` in a message - is passed through one rule:

- well-formed UTF-8 goes through byte for byte, including every non-ASCII
  script, emoji and combining mark;
- a byte that is not part of a well-formed sequence becomes U+FFFD
  REPLACEMENT CHARACTER, and decoding resumes at the next byte - a bad lead
  byte does not get to say how much of what follows to swallow;
- C0 controls other than newline and tab, DEL, and the C1 range
  U+0080..U+009F become U+FFFD, so no escape sequence, cursor move or colour
  change survives;
- the result is truncated on a character boundary if it will not fit, never
  mid-sequence.

**The bytes are only changed on the way out.** What MeshCore holds, hashes and
would put back on the air is untouched: this is a presentation decision at the
service boundary, not an edit to the protocol. A client that needs the exact
bytes a node sent is not served by this API and would need a method that says
so.

Outbound text, in `mesh.send`, is *rejected* rather than sanitised - a caller
sending a control character has made a mistake and should be told, where a
remote node sending one has done something this service simply will not pass
on.

## A node state this service will not read

The identity and the node table are not the same kind of thing, and a fault in
one is not answered like a fault in the other.

- **The identity cannot be reconstructed.** A corrupt `identity.id` stops the
  service, and is never replaced: a new key would make this a different node
  to every peer that knows it.
- **The node table is a cache the mesh refills.** A `state.v1` this build will
  not read does *not* stop the service. It is moved aside - renamed, never
  rewritten, because it is the only evidence of whatever went wrong - the node
  starts with no known nodes, keeps its identity, and learns the mesh again
  from the next adverts. `mesh.status` then carries `state_fault` describing
  what was wrong and where the file was kept, so a client can tell "this node
  forgot what it knew" from "this node has never heard anyone".

If the file cannot be moved aside, nothing is written for that run: the file
is the only record of the fault, and a fresh table written over it would
destroy that. The node still runs; it simply starts empty again next time.

## Nodes the table had no room for

MeshCore's contact table holds 32. Once it is full an advert from a new node
is reported to the service anyway, with a contact MeshCore is about to throw
away, so that a UI can say "somebody adverted and I could not keep them".

meshcored does not treat that as a node. It raises **no** `mesh.node` event
for it - an event naming a node `mesh.node` cannot then find would be worse
than silence - does not mark its state as changed, and does not record the
node's signal. It counts them, as `nodes_unretained`, so that a table which is
full is visible rather than merely quiet.

## Clients are observers

The protocol runtime does not belong to any client and does not stop when one
goes away:

- with **no client connected** it ticks, receives, learns nodes and answers
  messages exactly as before;
- a **client disconnecting** takes its subscription with it and nothing else;
- a **client that stops reading** its socket is disconnected by pocketipc's
  documented backpressure policy (docs/api/pocketipc.md) and the service
  carries on. Event subscribers must read continuously.

## Errors

The pocketipc codes (docs/api/pocketipc.md), used as follows:

| Code | Here |
| --- | --- |
| 1 | unknown method |
| 2 | invalid params: a key prefix, a text, or a params object this service will not take |
| 4 | the MeshCore runtime could not do it |
| 5 | the radio is not available, or the protocol core is busy |

Code 3 is not used by this service: a policy refusal about the radio is
radiod's to make, and meshcored reports it as a service state rather than as
an answer to an unrelated request.

## Not in v0

Group channels (`MAX_GROUP_CHANNELS` is left undefined in
`protocols/meshcore`, so upstream's channel code is not compiled), contact
import and export, a message store that survives a restart, renaming the node
over IPC, `/trace`, repeater behaviour (`allowPacketForward()` stays false, so
this node hears everything and forwards nothing), a periodic advert, and any
protocol other than MeshCore.
