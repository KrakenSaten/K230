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
- **the owner switched the radio off** (radiod state `off`,
  docs/api/radio.md "Radio on and off") is `degraded` with the reason "the
  radio is switched off" and `radio.radio_state` `off`. Unlike `error`, radiod
  refuses every transmit then, so the protocol runtime is told the radio is
  not there (`radio.online` false; adverts are refused with 5 as when
  disconnected) instead of building packets that can only be refused. The
  connection, the lease and the profile are kept - radiod accepts the profile
  while off and applies it when the radio comes back - so there is no
  reconnect, no backoff and no `error`. The first `rx` after the owner
  switches it on returns the service to `online`. Identity, channels and
  contacts are not touched.
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
| `run_id` | 16 hex characters naming this run of the service; new on every start. App datagram ids (below) start again from 1 with each run |
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
| `nodes_unretained` | adverts from nodes the 1000-slot contact table had no room for |
| `contacts_full` | how often MeshCore reported the table full |
| `app_rx` / `app_tx` / `app_receipts` | app datagrams received, sent, and flood receipts answered (see "App datagrams") |

`tx_ok`, `tx_rx_resume_failed`, `tx_failed` and `tx_unknown` are four
different answers and are never collapsed into two. A daemon asking "must I
send this again?" reads whether the bytes went out; calling a transmit that
went out a failure is how a fault in the receive path becomes twice the
airtime.

### mesh.identity

Result: `public_key` (64 hex characters), `node_hash` (the first byte, which
is what MeshCore routes on), `name`, `name_source` and `name_max`.

`name_source` says where the name in use came from: `config` - the command
line (`--name`, which `S65meshcored` passes from `MESHCORED_NAME` in
`/etc/default/meshcored`); `stored` - `state.v1`, which is also where a rename
made over a configured name is kept; `derived` - made from the key on a first
start.
`name_max` is the most bytes a name can be (31, MeshCore's `node_name`).

The private key is not reported by this method or any other, at any verbosity.

### mesh.set_name

Params: `name` - 1 to `name_max` bytes of well-formed UTF-8, on one line (no
newline or tab), no control characters, not only spaces. Result: the identity
afterwards (as `mesh.identity`) and `persisted` (boolean).

Renames this node: the name its adverts carry, and the `"<name>: "` it writes
in front of every channel message - so each channel's `text_limit` changes
with it, and a client re-reads `mesh.channels`. **Nothing is transmitted.**
Peers learn the new name from this node's next advert (`mesh.advert`); until
then they show the old one.

`state.v1` is the one place the name is kept, and it is written before the
answer is sent. A node whose name is configured (`name_source` `config`) is
renamed too: `settings.v1` then records which configured name the rename
replaced (`renamed_over`, a mark of that name and not a name), so the next
start keeps the rename instead of putting the configured name back. A
configured name that is changed afterwards wins again.

`persisted` is `true` only when everything a restart needs was written. When
it is `false` the node runs under the new name now and **comes back under the
old one at the next start**; a client must not show such a rename as saved.
(False while the stored table is not being written, see "A node state this
service will not read", or when either file could not be written.)

Errors: 2 for a name that breaks the rule above.

### mesh.path_hash

No params. Result: `bytes` (1, 2 or 3), `allowed` (`[1, 2, 3]`), `default`
(1).

How many bytes of each relay's public key a flood this node starts asks the
repeaters to write into its path: MeshCore's **path hash size**. It is
upstream's (`Packet::setPathHashSizeAndCount`, the size in the top two bits of
the path length byte; the companion firmware sets it with
`CMD_SET_PATH_HASH_MODE`, mode 0..2 = 1..3 bytes, mode 3 reserved), applied
here exactly as the companion firmware applies it with no flood scope: every
flood this node starts - a message or a returned path to a node, a channel
message, an advert - goes with this size. A zero-hop advert carries no path
and is not affected; nor is a direct message, which follows the learned route
in whatever size it was learned.

`1` is the default and what every MeshCore node has always sent and read.
`2` and `3` tell more relays apart in a large mesh (a 1-byte hash names one
relay in 256), and **need repeaters whose firmware reads the size bits**:
one that does not drops such floods, so a message or advert may not get
through a mesh where 1 byte would have. That is the operator's trade to make;
this service does not decide it. Kept in `settings.v1` in the state directory
(`path_hash_bytes=N`, one line, mode 0600); a file it cannot read leaves the
default and is logged.

### mesh.set_path_hash

Params: `bytes` - a whole number, 1 to 3. Result: as `mesh.path_hash`, and
`persisted` (whether `settings.v1` was written; the size applies either way).
Nothing is transmitted; the next flood uses it.

Errors: 2 for anything but 1, 2 or 3 - a string `"2"`, or `2.5`, is refused
rather than read as a size somebody may not have meant.

### mesh.nodes

Result: `nodes` (array), `count`.

At most 1000 nodes, **most recently heard first**: the nodes heard since the
service started, newest `last_heard_mono_ms` first; then the ones not heard
since it started, newest first by when MeshCore last updated the contact
(kept in the service's state across a restart); ties in table order. A
client that keeps fewer nodes than this takes the head of
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
| `advert_hops`, `advert_mono_ms` | **only when an advert from the node was heard in this run**: how many relays that last advert came through (MeshCore's own hop count of the advert packet's path - `0` is heard straight from the node, by a zero-hop advert or a flood no repeater had taken up yet), and when, by ours |
| `lat`, `lon` | **only when one of the node's adverts carried a location** (MeshCore `ADV_LATLON_MASK`): degrees, WGS84 as the node claims it, 6 decimals. Kept across a restart with the node (state.v1). Absent for MeshCore's 0,0 ("never set") and for anything outside -90..90 / -180..180. A claim by the node, never a measurement; this node's own adverts carry none |

`advert_hops` is the advert's way **here**, read off the packet; it is not the
route back (`hops`, `path_hex`), which MeshCore learns separately and only
when a message to the node is answered. A node can be `advert_hops: 0` with
no route known at all, which is the usual case for a repeater nearby. Like
the signal it is kept in memory for this run, not in `state.v1`, and a node
that has not adverted since the service started has none.

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
holds 1000 and evicts nothing on its own, so this is what makes room when
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
| `well_known` | `"public"` when the key is MeshCore's well-known Public channel key (`8b3387e9c5cdea6ac9e5edbaa115cd72`, upstream `PUBLIC_GROUP_PSK`); absent otherwise. Decided by the service from the key itself, never from the name or the hash. |
| `text_limit` | the longest body `mesh.send` will take on this channel |
| `ack_expected` | always `false` |

**The standard Public channel is mandatory in Doors.** On every start, after
`channels.v1` is restored, the service joins MeshCore's well-known Public key
(`izOH6cXN6mrJ5e26oRXNcg==`) as `Public` into the lowest free slot when no
slot holds that exact key, and writes `channels.v1` at once - so an empty
store, or one written before this rule, gets it, and one that has it (under
any name, in any slot) is left alone. It is identified by the key alone: a
hashtag channel called `#public` (its key is `SHA-256("#public")[:16]`) is a
different channel and is untouched. With every slot already taken by other
channels nothing is evicted and the start is logged as a warning. Joining
transmits nothing. `mesh.channel_remove` refuses it (below).

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

This is the only way a key a reader chose reaches the service: nothing here
derives a key from a name or generates one. The one exception is the standard
Public channel, which the service joins itself (`mesh.channels`, "mandatory"):
its key is public by design and every MeshCore node holds it.
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

Errors: 2 for a slot that holds no channel, and 2 for the standard Public
channel, which is mandatory ("the standard Public channel cannot be left").

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
| `state` | `received`, `sent_flood`, `sent_direct`, `acked`, `no_ack`, `failed`. `sent_*` means accepted, not transmitted, and `failed` is not produced in this version: see "Accepted is not transmitted" under `mesh.send` |
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

**Accepted is not transmitted, and a message's `state` does not say whether
it was.** This is a known limitation of this version (docs/KNOWN_ISSUES.md),
stated exactly so no client reads more into a state than it carries:

- **`sent_flood` and `sent_direct` mean accepted and queued, on that route.**
  The outgoing message is recorded with one of them the moment `mesh.send` is
  accepted, before anything reaches radiod. They are not a report that the
  frame went on the air.
- **The radio's outcome is not in the message.** The frame goes to radiod as
  an asynchronous transmit, and what radiod says about it arrives only as a
  `mesh.activity` event of `kind: "tx"` - keyed by radiod's `submit_id`,
  which no field of the message carries. A client cannot tie that outcome to
  a message, and the service does not do it either.
- **`failed` is never produced in this version.** It is in the `state` table
  because the API reserves it; nothing assigns it.
- So a frame that never left - radiod restarted before the dispatcher handed
  it over, a `tx` outcome of `tx_failed` or `refused`, the dispatcher giving
  up on it - leaves a **channel** message at `sent_flood` for good, exactly
  like one that went out, and a **direct** message at `sent_*` until its ACK
  deadline passes and then `no_ack`, not `failed`.

What the states do promise, by kind:

| | direct (`ack_expected: true`) | channel (`ack_expected: false`) |
| --- | --- | --- |
| `sent_flood` / `sent_direct` | accepted, waiting for the recipient's ACK | `sent_flood` only: accepted, and final |
| `acked` | the recipient's ACK matched this message: it was delivered | never |
| `no_ack` | the deadline passed without an ACK - whether or not the frame was ever transmitted | never |
| `failed` | not produced in this version | not produced in this version |

A **channel message is an unacknowledged flood**: `PAYLOAD_TYPE_GRP_TXT` has
no ACK, no timeout and no delivery report, so nothing - in the protocol or in
this service - ever says a channel message arrived anywhere, and in this
version nothing says it was transmitted either. Only a direct message can be
confirmed, and only by its ACK.

The first message to a node goes `flood`, because no route back is known yet;
that is what the ACK supplies, and the next one goes `direct`. A channel
message is always flooded and there is no second attempt.

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

**An advert is stamped with the wall clock, and nothing checks that the clock
is set.** The board has no RTC: until NTP answers, the clock reads 1970. An
advert built then is accepted and transmitted, but a MeshCore peer that already
holds a newer advert from this node treats it as a replay and ignores it
(upstream `BaseChatMesh.cpp:131`), so name and route refreshes do not arrive
and nothing reports that. A known limitation (docs/KNOWN_ISSUES.md); advert
after the clock has been set.

**This, `mesh.send`, `mesh.app_send`, `mesh.discover` and the
`mesh.remote_login` / `_request` / `_cli` requests (see "Repeater control") are
the only ways meshcored transmits without having been sent something first.**
There is no periodic advert, and nothing is resent on a timer. What
it does send unasked is what the protocol owes a sender: an ACK, and a return
path, for a message addressed to this node, and the receipt for an app
datagram that arrived by flood (below).

### App datagrams: mesh.app_send / mesh.app_inbox

Opaque packets between applications on two Doors nodes, for the likes of
Fleet's multiplayer protocol (docs/apps/FLEET_MULTIPLAYER.md, ADR-008). The
service does not read them; it carries them.

On the air an app datagram is a MeshCore `PAYLOAD_TYPE_REQ` to the peer, so it
is addressed, encrypted and MACed exactly as a direct message is. The
decrypted request data is `0xD0 | port`, the payload's length, and the
payload. The first byte is outside every request type upstream defines
(`0x00`-`0x07` in the pinned tree), so a MeshCore node that is not Doors
answers it as an unknown request - with nothing. The explicit length is there
because the decrypted REQ is padded to the 16-byte AES block and MeshCore
records no length of its own.

A payload of up to 10 bytes is one AES block (a 22-byte frame at zero hops,
304 ms on the MeshCore profile), up to 26 bytes two (38 bytes, 386 ms).

**Unacknowledged at this layer.** Like a channel message, and unlike
`mesh.send`: an application that needs delivery confirms it end to end in its
own protocol, which it has to do anyway to survive a restart on either side.
What this service does do is teach the route. A datagram that arrives **by
flood** is answered with a five-byte `RESPONSE` riding MeshCore's return path,
so the sender learns a direct route and the next datagram goes direct; one
that arrives direct is answered with nothing.

#### mesh.app_send

Params: `to` (a **whole** public key, 64 hex characters: an application's
packet goes to the peer it means or nowhere), `port` (1 to 15), `payload_hex`
(1 to 160 bytes). Result: `accepted`, `route` (`"flood"` or `"direct"`),
`est_timeout_ms` (MeshCore's own estimate of how long an answer could take on
that route - what an application sizes its retry timer on), `bytes`.

`accepted` means the protocol core queued it; the transmit arrives as
`mesh.activity`, as for any packet.

Errors: 2 for a key that is not whole, a port outside 1..15, a payload that is
not 1 to 160 bytes of hex, or a node this service does not hold; 5 when the
radio is not available; 4 when the runtime could not build it.

#### mesh.app_inbox

Params: `port` (1 to 15), `after_id` (optional, default 0). Result:
`datagrams` (oldest first), `count`.

The service holds the last 32 datagrams it received, across all ports, for
this run only. A client that was not listening - an app opened after its
opponent's packet arrived, say - catches up here by id. Ids start at 1 with
each run of the service and never repeat within one; when `mesh.status`
`run_id` changes, a client starts again from `after_id: 0`.

A datagram, here and in the `mesh.app` event:

| Field | |
| --- | --- |
| `id` | 1 upwards, per run |
| `port` | 1 to 15 |
| `from` | the sender's whole public key: proved, because the contact's key opened it |
| `payload_hex` | the payload, exactly as sent |
| `route` | `"flood"` or `"direct"`: how it arrived |
| `mono_ms` | when it arrived, on this node's monotonic clock |
| `rssi_dbm`, `snr_db` | **only when known** |

A REQ from a node this service does not hold cannot be decrypted and never
becomes a datagram: a peer has to have adverted first, as for a message.

### Repeater control

Finding the repeaters this node hears directly, logging in to one, and
reading it back - with upstream MeshCore's own requests and its own matching
rules, and no private protocol. **VERIFIED host** (tests/meshcored_repeater_test.cpp,
a whole runtime against a test repeater built from upstream's
`simple_repeater` handlers); **not yet run against a real repeater**
(docs/hardware/RIFT_REPEATER_CONTROL_GATE.md).

#### Repeaters heard directly: mesh.discover / mesh.discovered

`mesh.discover` (no params) **transmits one packet**: a zero-hop
`PAYLOAD_TYPE_CONTROL` `CTL_TYPE_NODE_DISCOVER_REQ` (`0x80`, whole keys) with
a repeater-only type filter and a random tag - byte for byte what upstream's
`simple_repeater` answers and what the T-Deck RIFT firmware's DISCOVER 0-HOP
sends. Every repeater in direct range answers, after a random delay, with a
zero-hop `CTL_TYPE_NODE_DISCOVER_RESP` carrying the tag, its whole public key
and the SNR it heard the request at.

**Proof of 0-hop is upstream's.** `mesh::Mesh` hands a control packet to the
node only when it is direct-routed with no relay hash in its path
(`Mesh.cpp:70-75`); a relayed or flooded answer never reaches this service.
Nothing is inferred from a name, a cached route or the repeater's own
neighbour list.

A round stays open for **30 s** (`window_ms`) and collects answers as they
arrive. While it is open, `mesh.discover` sends nothing and answers the open
round with `started: false`, so repeated presses cannot put more requests on
the air - and a repeater answers at most four requests in two minutes anyway
(upstream's `discover_limiter`).

Result: `started` (boolean), `round` (1 upwards, this run), `open`,
`window_ms`, `started_mono_ms`, `until_mono_ms`.

`mesh.discovered` (no params): the same round fields, `count`, and
`repeaters` - at most 16, newest answer first, this run only:

| Field | |
| --- | --- |
| `public_key`, `node_hash` | from the answer itself |
| `name`, `type` | **only when this service holds the node as a contact** (its advert was heard) |
| `known` | whether it does. A repeater that is not a contact cannot be logged in to |
| `round`, `current` | the round it last answered, and whether that is the latest round. An answer to an earlier round is kept and marked, never passed off as current |
| `mono_ms` | when that answer was heard |
| `their_snr_db` | how the **repeater** heard our request (its own reading) |
| `snr_db`, `rssi_dbm` | how **we** heard its answer - only when radiod reported them |

Event `mesh.discover`: `reason` `reply` (with `repeater`, one entry as above)
or `closed`, plus the round fields.

#### The repeater session: mesh.remote_*

One target and **one request at a time**. Every request needs the node's
**whole** public key (`node`, 64 hex) and a contact for it.

| Method | Params | On the air |
| --- | --- | --- |
| `mesh.remote_login` | `node`, `password` (0 to 15 printable bytes) | `BaseChatMesh::sendLogin`: an `ANON_REQ` with this node's clock and the password |
| `mesh.remote_request` | `node`, `kind`: `status`, `neighbours` or `owner` | `sendRequest` with `REQ_TYPE_GET_STATUS`, `REQ_TYPE_GET_NEIGHBOURS` (version 0, 11 newest, 6-byte prefixes) or `REQ_TYPE_GET_OWNER_INFO` |
| `mesh.remote_cli` | `node`, `command` (one line, 1 to 160 bytes) | `sendCommandData` (`TXT_TYPE_CLI_DATA`) |
| `mesh.remote_logout` | `node` (optional: without it, whichever session there is) | **nothing** |
| `mesh.remote_session` | - | - |

The first three answer `accepted`, `request_id`, `route` (`flood` or
`direct`) and `wait_ms`; the outcome arrives later as a `mesh.remote` event.
Errors: 2 for a key, password, kind or command the service will not take, no
contact for the node, a request before a login OK, or a command from a guest
login (the repeater drops a guest's commands without answering); 5 when the
radio is not available or a request is already waiting; 4 when the runtime
could not build it.

How answers are matched, upstream's way: a login answer from the target in
the shape `RESP_SERVER_LOGIN_OK` (13 bytes: the repeater's clock, `0`, admin,
permissions, firmware level) or the legacy `"OK"`; status, neighbours and
owner by the tag the repeater reflects; a command's answer is command data
from the target while a command waits (it carries no tag). Anything else from
the target is counted as `stale_replies` or `malformed_replies` and ignored.
A wait ends at `max(20 s, 2 x MeshCore's estimate + 8 s)` (upstream RIFT's
rule). A login OK that arrives up to 5 minutes after its wait ended, while
nothing else waits, is still taken and marked `late`, as upstream RIFT does.

**A wrong password is not answered.** Upstream's repeater sends nothing back
(`handleLoginReq` returns 0), so a wrong password ends as `timeout` and
cannot be told apart from a request the repeater never heard. A login answer
that is neither OK shape is reported `refused`, as upstream's companion
firmware does, but no upstream repeater sends one.

**There is no logout on the air.** Upstream's logout is
`BaseChatMesh::stopConnection`: local keep-alive state, nothing transmitted.
`mesh.remote_logout` does the same and forgets the session; the repeater
keeps this node in its access list. The session is memory only, never
written, and ends with a logout, with the service, and when the target's
contact is forgotten (`mesh.node_remove`) - a request it had waiting is then
answered `cancelled`.

**The password** is copied, handed to `sendLogin` and wiped, and the
request's own copy is overwritten once used. It is never logged, stored or
echoed. A password longer than 15 bytes is refused rather than cut to
upstream's 15.

A session (`mesh.remote_session`, and in every event): `active`; when active,
`node`, `name`/`type`/`known` as above, `login` (`none`, `waiting`, `ok`,
`refused`, `timeout`), and after an OK `legacy` and - unless legacy -
`admin`, `permissions`, `acl`, `firmware_level`; `repeater_clock` (from its
login answer), `login_mono_ms`, `pending` (`kind`, `request_id`,
`deadline_mono_ms`) while a request waits, `stale_replies`,
`malformed_replies`.

Event `mesh.remote`: `reply` and `session`. A reply: `request_id`, `kind`
(`login`, `status`, `neighbours`, `owner`, `cli`), `outcome` (`replied`,
`refused`, `timeout`, `cancelled`), `node`, `mono_ms`, `late` when it was,
`snr_db`/`rssi_dbm` when known, and on `replied`:

| kind | |
| --- | --- |
| `status` | `status`: `battery_mv`, `tx_queue`, `noise_floor_dbm`, `last_rssi_dbm`, `last_snr_db`, `packets_recv`, `packets_sent`, `air_time_s`, `uptime_s`, `sent_flood`, `sent_direct`, `recv_flood`, `recv_direct`, `err_events`, and **only when the reply carried them** `direct_dups`/`flood_dups` and `rx_air_time_s`/`recv_errors` (upstream's struct grew; an older repeater sends less) |
| `neighbours` | `neighbours`: `total` the repeater holds, `entries` (`prefix` 12 hex, `name` only when the prefix names exactly one held node, `heard_s_ago`, `snr_db`) - the **repeater's** own list of repeaters it heard zero-hop, not this node's |
| `owner` | `owner`: `firmware`, `name`, `owner` |
| `cli` | `text`: the repeater's answer |

Every string a repeater chose goes through the remote-text sanitiser.

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
- `mesh.app`: `datagram` (as above). One per app datagram received, on any
  port; a client filters on `port`.
- `mesh.discover`: a repeater answered the open discovery round, or the round
  closed (see "Repeater control").
- `mesh.remote`: a repeater request ended, and the session afterwards (see
  "Repeater control").
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

MeshCore's contact table holds 1000 on this port (upstream's default is 32;
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
| 2 | invalid params: a key prefix, a text, a port, a payload, or a params object this service will not take |
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

Contact import and export, a message store that survives a restart, `/trace`,
flood scopes (regions: no transport codes are written, so every flood is
unscoped, and there is no per-channel scope),
discovery of anything but repeaters (only the repeater-filtered
`CTL_TYPE_NODE_DISCOVER_REQ` is sent, see "Repeater control"), repeater
behaviour (`allowPacketForward()` stays
false, so this node hears everything and forwards nothing), a periodic advert,
group **data** frames (`PAYLOAD_TYPE_GRP_DATA` is parsed by the protocol core
and this service does nothing with it - only `GRP_TXT` becomes a message), and
any protocol other than MeshCore.
