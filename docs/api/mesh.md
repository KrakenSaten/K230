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

**Channels are ON-AIR VERIFIED.** Every channel method, event and field below
is exercised by the suites and by two whole `meshcored` processes over a mock
air — and the group frames themselves have been on a real SX1262, in both
directions, against an independently built MeshCore implementation (LILYGO
T-Deck, RIFT v0.9.5) on the MeshCore profile at 2 dBm. A channel conversation
and a direct one were run side by side on one node with their own delivery
semantics intact, and both survived a restart of this service with the peer's
learned path reloaded and used.

The evidence, part by part, is
[docs/hardware/RIFT_CHANNELS_GATE.md](../hardware/RIFT_CHANNELS_GATE.md)
(unit A, 2026-09-21, **PASS**). What it does **not** establish is anything
about delivery: `ack_expected` is `false` on a channel because the protocol
acknowledges nothing, and the gate demonstrated exactly that — a message the
peer demonstrably received, with this service still reporting `sent_flood`
and refusing to claim it.

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
| `nodes_unretained` | adverts from nodes the 256-slot contact table had no room for |
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

At most 256 nodes, **most recently heard first**: the nodes heard since the
service started, newest `last_heard_mono_ms` first; then the ones not heard
since it started, newest first by when MeshCore last updated the contact
(kept in the service's state across a restart); ties in table order. A
client that keeps fewer nodes than this - RIFT keeps 64 - takes the head of
the list and so keeps the nodes heard last.

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

### mesh.node_remove

Params: `node` - a node's **whole** public key, 64 hex characters. A prefix is
refused: this changes what the service holds, and doing that to whichever node
a prefix happened to match would be acting on a guess.

Result: `removed` (`true`), `persisted` (boolean), `node` - the node **as it
was**, in the shape above.

Forgets the node: its contact, its learned route, its last advert and the
signal this service recorded for it. The node table is written to `state.v1`
before the answer is sent, and `persisted` says whether that write happened:
`true`, and a node answered as forgotten does not return with the next
restart; `false` - the stored table could not be read at start
(`state_fault`), so this service does not write over it, or the write failed -
and it is forgotten for this run only. Every subscriber gets a `mesh.node`
event with the reason `removed`, raised before the answer is sent. Nothing is
transmitted.

It is the node's entry that goes, not the node: it is **added back the next
time it adverts**. Until then a message to it is refused (error 2, no single
node matches) because there is no contact to encrypt to. MeshCore's table
holds 32 and evicts nothing on its own, so this is what makes room when
`nodes_unretained` says adverts are being turned away. A message already
waiting for its ACK keeps waiting.

Errors: 2 for a key that is not 64 hex characters, or a node this service
does not hold.

### mesh.node_reset_path

Params: `node` - a whole public key, as `mesh.node_remove`.

Result: the node afterwards, in the shape above, with `path_known` false.

Forgets only the learned route, so the next message to the node floods and
the reply teaches a fresh one - the remedy for a node that has moved and whose
direct messages are no longer acknowledged. Nothing is transmitted by this
call. Subscribers get a `mesh.node` event with the reason `path`.

Errors: as `mesh.node_remove`.

### mesh.channels

No params. Result: `channels` (array, in slot order), `count`, `max`,
`persistent` (`true`).

A channel:

| Field | |
| --- | --- |
| `channel` | the **slot**, 0 to `max`-1. This is what a channel is named by. |
| `name` | local, and **never on the air**. Two nodes on one channel routinely call it different things. |
| `channel_hash` | one byte, hex: `SHA-256(key)[0]`, which is what MeshCore puts in the clear at the head of every group frame |
| `key_bits` | 128 or 256 |
| `text_limit` | the longest body `mesh.send` will take on this channel |
| `ack_expected` | always `false` |

**The key is not here, and no method reports it.** It is written to
`channels.v1` at mode 0600 and read back, and that is the whole of its travel:
anything else would put a shared secret into an IPC frame, a log, or a
screenshot. What a client gets is enough to show a channel and tell two apart,
and not enough to join one.

**The slot is the identity, not the position.** Leaving a channel empties its
slot rather than compacting the table, so no channel changes slot while it
exists. A slot that has been emptied may later be taken by a **different**
channel, which is why the hash and the name travel with it: a client that
caches a slot re-reads on a `mesh.channel` event rather than assuming it still
means what it did.

**The one-byte hash is a routing hint, not an identity.** Collisions are
ordinary - it is one byte - and MeshCore tries up to four channels whose hash
matches and lets the MAC decide (`Mesh.cpp:236-246`). Two channels with the
same `channel_hash` are not the same channel.

### mesh.channel

Params: `channel` (a slot). Result: one channel, as above.

A slot is an integer and nothing else: no prefix matching and no name lookup.
Two channels may legitimately carry the same name - the name is local and
nobody has to agree about it - so a name is not an identity and resolving one
would mean guessing which the caller meant.

Errors: 2 for a slot that is not a whole number in range, or that holds no
channel.

### mesh.channel_add

Params: `name` (1 to 31 bytes, no control characters), `key` (standard base64,
decoding to exactly 16 or 32 bytes). Result: the channel that was created.

This is the **only** way a key reaches the service, and deliberately the only
one: nothing here derives a key from a name, generates one, or ships a
well-known one. A channel exists because somebody supplied the secret for it.
It goes into the lowest free slot.

Errors, all code 2 except a full table (5):

| | |
| --- | --- |
| not base64, or not 16/32 bytes | a decoder that mapped unknown characters to `0` would turn a mistyped key into a *different* key that still appeared to work, so the decode is strict: any character outside the RFC 4648 alphabet is a refusal |
| an all-zero key | that is what an unused MeshCore slot holds, and the key is public by construction |
| a 32-byte key whose upper 16 bytes are zero | **refused as ambiguous.** MeshCore's `setChannel()` reads such a key as a 128-bit one and hashes it over 16 bytes, while its `addChannel()` would use the decoded length and hash it over 32. The same key would then derive two different channel hashes depending on which path a peer took, and this node would sit on a channel some of its peers cannot reach it on. The symptom would be silence, so it is refused with a reason instead. |
| a key already in the table | the key **is** the channel, so a second copy under another name would be one that can never be routed to: MeshCore's scan finds whichever comes first and stops |
| all slots taken | code 5 |

### mesh.channel_remove

Params: `channel` (a slot). Result: `removed`, `channel`, `key_forgotten`.

The key is overwritten in the service's own table, not merely marked unused,
and `channels.v1` is rewritten without it. This node held the only copy, and
nothing on the air will give it back - which is what `key_forgotten` says.

Errors: 2 for a slot that holds no channel.

### mesh.messages

Params: `limit` (optional). Result: `messages` (array, oldest first),
`count`, `total`, `persistent`.

With `limit`, the **newest** `limit` messages, still oldest first: a client
that asks for ten wants the last ten and wants to read them in the order they
happened.

`persistent` is `false` in this phase and says so rather than leaving it to be
discovered: the message list does not survive a restart. See "What is
persistent" in docs/services/MESHCORED.md.

The list holds direct messages and channel messages together, in the order
they happened. They are held in **separate rings** inside the service, so a
busy channel cannot push a direct conversation out of the history and a busy
conversation cannot push a channel out of it; `mesh.messages` merges the two
by `id`, which is one counter handed out in arrival order.

**Read "while it runs" in the `id` row below as the warning it is.** That one
counter starts again at 1 on every run of the service, so an id identifies a
message within one run and not across two. A client that holds messages of
its own - as RIFT does, in a window this service's rings cannot be relied on
to refill - must notice that the service restarted and empty that window, or
a new id 1 lands on top of an old id 1 and what it shows is a history blended
from two sessions. There is no per-run identifier in this version of the API;
the signal available to a client is `mesh.status`'s `uptime_s` read against
its own monotonic clock, which is what RIFT does
(`apps/rift/rift_messages.c`). Unit A found this the hard way on 2026-09-21,
in part D of the channels gate, and it is not a channel fault: the counter
has always been one counter and has always started again.

A message:

| Field | |
| --- | --- |
| `id` | meshcored's own, 1 upwards, never reused **while it runs**; the next run starts at 1 again |
| `direction` | `in` or `out` |
| `kind` | `direct` or `channel`. Which of the two shapes below this is. |
| `peer_public_key`, `peer_name` | **`direct` only** |
| `channel`, `channel_name`, `channel_hash` | **`channel` only**: the slot, this node's local name for it, and the one byte that was on the air |
| `sender_name` | **`channel` only**, and only when it parses: the name the sender **claimed**. See below. |
| `text`, `timestamp` | `timestamp` is the **sender's** clock, MeshCore's own stamp |
| `mono_ms` | when this service saw it |
| `state` | `received`, `sent_flood`, `sent_direct`, `acked`, `no_ack`, `failed` |
| `ack_expected` | whether an acknowledgement can **ever** arrive for this message |
| `ack_mono_ms` | when the ACK matched; absent until it does |
| `snr_db`, `rssi_dbm` | only when known, by the same rule as a node's |

**A channel message names no node.** A MeshCore group frame carries no public
key and nothing signs it, so there is no `peer_public_key` on one and there
cannot be. What it has instead is `sender_name`: MeshCore writes `"<name>: "`
into the *encrypted payload* (`BaseChatMesh.cpp:492`) and this service parses
it back out, on the first `": "`. It is a **claim**, not an identity - anyone
holding the channel key can send any name - and a client that draws it the way
it draws a `peer_name` is saying something the protocol does not support.
`text` is the whole payload including the prefix, so a client that wants the
body alone skips `strlen(sender_name) + 2`.

**`ack_expected` is `false` for everything on a channel.**
`PAYLOAD_TYPE_GRP_TXT` is flooded and unacknowledged: there is no
`expected_ack`, no timeout and no delivery report anywhere in the protocol. An
outgoing channel message therefore reaches `sent_flood` and stays there
forever. A client that draws "delivered" or "no ack" where `ack_expected` is
`false` is inventing a guarantee MeshCore does not offer - and one that shows
a permanent `sent_flood` the way it shows a direct message waiting for an ACK
teaches a reader to read it as a failure.

### mesh.send

Params: exactly **one** of `to` (a public key or prefix, as `mesh.node`) or
`channel` (a channel slot), plus `text` (1 to 160 bytes, no control characters
other than newline and tab).

Giving both is an error rather than a precedence rule: they are different
destinations with different delivery semantics, and silently preferring one
would send a message somewhere the caller did not mean. Giving neither is an
error too.

Result: `accepted` (always `true`), `message_id`, `route`, `ack_expected`, and
then one of:

| | |
| --- | --- |
| to a node | `route` is `flood` or `direct`, `ack_timeout_ms` is how long MeshCore will wait, `ack_expected` is `true` |
| to a channel | `route` is `flood`, `channel` is the slot, `ack_expected` is `false`, and **there is no `ack_timeout_ms`** - there is no ACK to time out, and a timeout of 0 would read as "answered instantly" |

**Accepted is not transmitted.** The frame is queued for the MeshCore
dispatcher, goes to radiod as an asynchronous transmit, and its outcome
arrives later as a `mesh.activity` event and in the message's own `state`. The
first message to a node goes `flood`, because no route back is known yet; that
is what the ACK supplies, and the next one goes `direct`. A channel message is
always flooded and there is no second attempt.

**A channel's text limit is smaller than 160.** MeshCore puts `"<this node's
name>: "` inside a channel payload, and upstream's `sendGroupMessage()`
silently truncates the caller's text to make the whole thing fit. This service
refuses instead, and `mesh.channels` reports the real number as `text_limit`
so a composer can show it rather than discover it.

Errors: 2 for a recipient, channel or text the service will not take, 5 when
the radio is not available (the message text says which state it is in) or
when eight direct messages are already waiting for their ACK, 4 when the
protocol core could not build the message.

**Each direct message waits for its own ACK.** `ack_timeout_ms` is that
message's own deadline, and when it passes with no ACK the message - that
one, not the oldest one waiting - becomes `no_ack`. An ACK for one message
does not end another's wait. (MeshCore itself keeps one timer for the whole
node; see docs/services/MESHCORED.md for why the service does not use it.) A
send that finds eight messages still waiting is refused with error 5 and
nothing is built or transmitted: a message the service could not watch would
never be answered either way. "Still" waiting: any whose deadline has already
passed are answered `no_ack` first, so a send is never turned away by messages
nobody is waiting for any more. A deadline is not judged while received
frames are still queued behind the one being handled, so an ACK that arrived
in time is matched before its message is called unacknowledged.

### mesh.advert

Params: `zero_hop` (optional, a boolean). Result: `accepted`, `route`
(`"flood"` or `"zero_hop"`).

Builds one signed self-advert and floods it, or with `zero_hop: true` sends it
zero-hop: heard by the nodes in direct range and repeated by none of them, at
the airtime of one packet rather than a flood across the mesh. `accepted`
means the protocol core queued it; how the transmit went arrives as
`mesh.activity`, as for any packet.

Errors: 2 when `zero_hop` is present and not a boolean (a string `"false"`
read as true would flood an advert the caller meant to keep local), 5 when the
radio is not available, 4 when the runtime could not build one.

**This and `mesh.send` are the only ways meshcored transmits without having
been sent something first.** There is no periodic advert. What it does send
unasked is what the protocol owes a sender: an ACK, and a return path, for a
message addressed to this node.

### mesh.subscribe / mesh.unsubscribe

No params. Result: `{"subscribed": true|false}`. Per connection, cleared by
disconnecting.

## Events

- `mesh.state`: `state`, `reason`, `mono_ms`. One per transition.
- `mesh.node`: `reason` (`discovered`, `path`, `removed`), `node` (as above).
  With `removed` the node is the one `mesh.node_remove` forgot, **as it
  was**; a client takes it off its list rather than applying it as an
  update.
- `mesh.channel`: `reason` (`added`, `removed`), `channel` (as above, and
  still without the key). One per change.
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

MeshCore's contact table holds 256 on this port (upstream's default is 32;
`protocols/meshcore/compat/mc_contacts.h`). Once it is full an advert from a new node
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

## What a channel is, and is not

Worth stating in one place, because almost every mistake a client can make
about channels follows from expecting one to behave like a conversation with a
node.

| | a node | a channel |
| --- | --- | --- |
| identity | a public key, which nothing else can forge | a **pre-shared key**, which everybody on the channel holds |
| named by | `public_key` | `channel` (a slot) |
| who sent it | proved: the frame is addressed and the contact's key opened it | **claimed**: a name inside the payload, unsigned |
| route | learned, reported as `hops`/`path_hex` | none. Flooded to whoever holds the key. |
| delivery | an ACK, a timeout, `acked`/`no_ack` | **nothing**. `sent_flood` is the end. |
| confidentiality | an X25519 agreement between two nodes | the group key: every holder can read every message |
| forgetting it | the node re-adverts | the key is gone; nothing on the air brings it back |

## Not in v0

Contact import and export, a message store that survives a restart, renaming
the node over IPC, `/trace`, repeater behaviour (`allowPacketForward()` stays
false, so this node hears everything and forwards nothing), a periodic advert,
group **data** frames (`PAYLOAD_TYPE_GRP_DATA` is parsed by the protocol core
and this service does nothing with it - only `GRP_TXT` becomes a message), and
any protocol other than MeshCore.
