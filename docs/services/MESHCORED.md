# meshcored — the MeshCore protocol service

Status: on master and in every image since the shipping change of
2026-09-22, **disabled by default** (`S65meshcored`, `MESHCORED_ENABLE=0`):
a unit runs it only when enabled per unit (below). RF VERIFIED on unit A
against a T-Deck RIFT peer on bench-deployed builds - the hardware gate
(2026-09-19, `docs/hardware/MESHCORED_HARDWARE_GATE.md`) and the RIFT
channel, improvement and 256-node gates after it; not yet run from a flashed
image. Known limits: docs/KNOWN_ISSUES.md (meshcored).

`meshcored` is the first protocol daemon on top of `radiod`. It owns the
MeshCore runtime — the identity, the packet pool, the dispatcher, the node
table, adverts, messages, ACKs, paths and duplicate suppression — and it owns
nothing below that.

```
SX1262                         hardware
   |  SPI + GPIO
radiod                         owns the radio, exclusively (ADR-002)
   |  radio.* over pocketipc
meshcored                      owns the MeshCore protocol runtime
   |  mesh.* over pocketipc
RIFT (apps/rift)               owns the presentation
```

The name is `meshcored`, not `riftd`, and the reason is the boundary itself:
**meshcored owns the MeshCore protocol and runtime; RIFT is a UI above it.**
A second client — or a second UI, or none at all — changes nothing about the
node.

## What it does not own

It opens no `/dev/spidev`, drives no GPIO, links no RadioLib and contains no
LoRa driver. `tests/meshcored_lint.sh` checks that by inspection rather than
by intention: no device path, no GPIO call, no radio-library header, and no
use of the synchronous `radio.send`, which would block radiod for the whole
airtime and this service with it.

It draws nothing. There is no LVGL, no theme token and no screen here.
`apps/rift/` exists since RIFT phase 1 (docs/apps/RIFT.md) and this service
knows nothing about it: nothing here names it, nothing here includes it, and
its objects are not in this build (`tests/meshcored_lint.sh`). That is the
boundary the name was chosen for - meshcored owns the MeshCore protocol and
runtime, RIFT is a UI above it, and a second client, or none at all, changes
nothing about the node.

## Ownership on start-up

1. connect to radiod (`pocketipc_connect_timeout`, 500 ms, non-blocking —
   a full listen backlog must not become an unbounded wait, which it once did
   on unit A);
2. `radio.acquire` the lease, naming itself `meshcored`;
3. `radio.configure` with the MeshCore profile;
4. `radio.subscribe`;
5. one `radio.status`, so the first radio state is read rather than assumed;
6. run.

If the lease is held elsewhere the service **waits**. Nothing in meshcored can
take a lease from another owner, and the failure is reported as a service
state (`waiting_for_lease`) with a bounded, doubling backoff — 0.5 s to a 30 s
ceiling — rather than as a retry loop.

And if radiod refuses the **profile** — the region guard turning it down, or a
value it will not take — the service gives the radio back before it gives up.
It asks for `radio.release` and closes the connection, which releases the
lease on radiod's side as well, clears its own lease and profile state, and
enters `error`. That state is terminal: the profile comes from the command
line, so reconnecting would apply the same numbers and be refused the same
way, for ever. What matters is what it does **not** do — sit in an error state
holding a radio nothing else can then take.

The seven service states, what each means and which failure produces which,
are in docs/api/mesh.md.

## The radio profile

The profile the accepted P0 on-air gate used
(docs/hardware/MESHCORE_INTEROP_GATE.md), because it is what another MeshCore
node expects:

| | |
| --- | --- |
| Frequency | 869.618 MHz |
| Bandwidth | 62.5 kHz |
| Spreading factor | SF8 |
| Coding rate | 4/5 |
| Sync word | 0x12 |
| Preamble | 32 |
| CRC | on |
| TX power | **2 dBm**, configurable |

Changing any of the first seven means this node is on a different network, not
a differently tuned one.

The power is a **service value with the tested default**, not a regulatory
policy. 2 dBm is what the gate transmitted at. Regional policy — duty cycle,
ERP with antenna gain, sub-band rules — belongs above the raw radio backend
and stays the operator's; radiod's EU868 region guard is a safety net and says
so (docs/api/radio.md). meshcored range-checks its profile at start-up so a
typo in `/etc/default/meshcored` is refused before the radio is touched, and
radiod validates it again.

Every value is settable on the command line (`--frequency-mhz`,
`--tx-power-dbm`, …), and `--help` prints them.

## The radiod adapter

`mesh::Radio` is the interface MeshCore reaches its transceiver through, and
`protocols/meshcore` deliberately left it unimplemented. The implementation
lives here, in `services/meshcored/mesh_runtime.cpp`, and it is narrow: bytes
and metadata in, bytes out, one hook. No MeshCore code knows that a frame came
from an IPC event, and no IPC code knows what is in one.

### Receive

```
radio.rx  ->  validate  ->  a bounded queue  ->  mesh::Radio::recvRaw()
```

- `payload_hex` is decoded **strictly**: every character a hex digit, an even
  count, 1 to 255 bytes. MeshCore's own `Utils::fromHex()` validates the
  length and nothing else — a non-hex character becomes 0 and the call still
  returns true (upstream debt 3, `protocols/meshcore/README.md`) — so this
  service does not use it for anything from outside. A corrupt event is
  rejected and counted, not turned into a frame of plausible bytes.
- The bytes are preserved exactly; nothing normalises or re-encodes them.
- `mono_ms` is carried through. It is `CLOCK_MONOTONIC`, which is the only
  clock an interval may be measured on here: this board starts at 1970 on
  every boot and jumps by decades when the network comes up.
- RSSI, SNR and frequency error each travel with a **known** flag. An event
  that does not carry one leaves it unknown all the way to the API, where it
  is absent rather than zero.
- The queue holds 32 frames. A burst larger than that is dropped and counted
  (`rx_dropped`); the daemon does not grow because somebody is transmitting
  quickly. The main loop's poll timeout collapses to zero while a frame is
  waiting, so a burst drains one frame per turn without the service going deaf
  to its own clients in between.

### Transmit

```
MeshCore -> adapter -> radio.send_async -> tx_id -> radio.tx_done -> adapter -> MeshCore
```

Never `radio.send`. The submission is asynchronous the whole way: nothing here
blocks waiting for radiod to accept, because acceptance is itself asynchronous
and the reply arrives as an ordinary frame in the same loop everything else
runs in.

Three identities are kept apart, which is what makes the awkward cases
answerable: meshcored's own `submit_id`, the pocketipc request id, and
radiod's `tx_id`. See `services/meshcored/tx_map.h`.

| What happens | What meshcored does |
| --- | --- |
| No lease, or a transmit already outstanding | refuses the submission; MeshCore is told the send did not start |
| radiod reports state `tx` with a submission outstanding | nothing: the transmit is this service's own, and the state stays `online` (docs/api/mesh.md) |
| radiod reports state `tx` with nothing outstanding | `degraded` - a radio this service holds the lease on is transmitting something it did not submit |
| radiod refuses (BUSY, no lease, bad payload) | `tx_refused`, reported as `tx_failed` to the runtime |
| `tx_done` with `transmitted: false` | `tx_failed`. Not complete: the dispatcher expires the packet on its own deadline and charges no airtime |
| `tx_done` with `transmitted: true, rx_resumed: false` | `tx_rx_resume_failed`, and the service goes `degraded`. **Complete**, because the bytes went out |
| `tx_done` with both true | `tx_ok` |
| A second `tx_done` for the same `tx_id` | `tx_done_unmatched`, dropped |
| A `tx_done` for a `tx_id` nobody holds | `tx_done_unmatched`, dropped |
| radiod disconnects with a submission outstanding | `tx_unknown` |
| No `tx_done` within the deadline | `tx_unknown` |

**Nothing is ever retransmitted on an ambiguous outcome.** "Unknown" is a
different answer from "failed", and "failed" is the one that would put the
same packet on the air twice. A completion is never reported to MeshCore
before `radio.tx_done` arrives, and `tx_done` arriving is never read as
success — the fields are.

### The completion deadline

radiod promises exactly one `radio.tx_done` per accepted request on every
outcome path, and keeps it — except across a restart, where it has no
transmit state to deliver a completion from and says so. Its own documentation
also records a deferred item: a backend that lost a hardware completion would
leave radiod in `tx` for ever with no event to say why
(docs/KNOWN_ISSUES.md).

Either way the packet stops being this service's problem only if something
here decides it has. So a submission has a bound — five seconds for
acceptance, three times the reported airtime plus two seconds once accepted —
and passing it resolves the transmit as `tx_unknown`.

This is not defensive decoration. Without it one lost completion is permanent:
the slot stays taken, every later submission is refused because one is
outstanding, and the node goes quiet for the rest of the session with nothing
in the log to explain it. `tests/meshcored_harness_test.sh` found exactly that
when it was first asked to drop a `radio.tx_done`.

## The MeshCore runtime

Instantiated from `protocols/meshcore`, with real crypto throughout: real
Ed25519 signatures, a real X25519 agreement, real AES-128 with encrypt-then-
MAC. There is no mock cipher anywhere in this service.

| | |
| --- | --- |
| identity | `mesh::LocalIdentity`, persisted (below) |
| packet manager | `StaticPoolPacketManager`, 32 packets |
| dispatcher | `mesh::Dispatcher` through `BaseChatMesh`, with its airtime budget and duty-cycle window as upstream sets them |
| mesh tables | `SimpleMeshTables`, the duplicate table |
| adverts | `createSelfAdvert()`, `AdvertDataBuilder`/`Parser` |
| text | `sendMessage()`, `onMessageRecv()` |
| ACK and path | `processAck()`, `onContactPathRecv()`, return paths |
| clock | `CLOCK_MONOTONIC` through `mcport::MonotonicClock` |
| logging | `mcport::setLogSink()` into pocketlog |

Upstream behaviour is not redesigned. Two places where that is a deliberate
choice worth naming:

- **Contacts are added automatically**, as upstream does
  (`isAutoAddEnabled()` returns true). Any node that adverts within range
  becomes a contact, up to 1000 (`MAX_CONTACTS`, set in
  `protocols/meshcore/compat/mc_contacts.h`; upstream's default is 32, which
  a dense mesh filled within minutes). Past that, MeshCore reports the discovery
  anyway, with a contact it is about to throw away, so a UI can say somebody
  adverted and it could not be kept. meshcored does not treat that as a node:
  no `mesh.node` event, no state marked dirty, no telemetry slot taken from a
  node that *was* kept. It is counted as `nodes_unretained` so that a full
  table is visible rather than merely quiet. The table-full policy itself is
  upstream's and unchanged: nothing is evicted on its own. What makes room is
  a client asking, by `mesh.node_remove` (below).
- **This node is not a repeater.** `allowPacketForward()` stays false, so it
  hears everything and forwards nothing.

One place where upstream's behaviour is **not** kept, because it was wrong
for more than one message at a time:

- **Each sent message waits for its own ACK.** MeshCore keeps a single send
  timeout for the whole node (`BaseChatMesh::txt_send_timeout`): every
  `sendMessage()` overwrites it and any matched ACK clears it
  (`BaseChatMesh.cpp:339`, `:350`, `:451`, `:455`). With two direct messages
  in flight, an ACK for one cancelled the other's timeout, which then stayed
  `sent_*` for ever; and a timeout that did fire was pinned on the *oldest*
  unanswered message rather than the one it was for. meshcored no longer
  uses that timer (`onSendTimeout()` does nothing). Every accepted message
  takes an outbox slot with its **own deadline** - the send time plus the
  timeout MeshCore computed for that very packet, flood or direct - and
  `mcd_runtime_tick()` answers `no_ack` for each message whose own deadline
  has passed, earliest first - but only once every received frame has been
  handed to the protocol core: the daemon hands over one frame a turn, and an
  ACK that arrived in time but is queued behind others is matched before its
  message's deadline is judged. Eight messages may wait at once; a ninth is
  refused with `mesh.send` error 5 and nothing is built or sent, because a
  message the service could not watch would never be answered either way.
  `mesh.send` answers any deadline already passed before it judges the
  outbox full, since requests are served between ticks. RIFT allows a second
  message as soon as the first is accepted, so this was reachable from the
  composer.

### Forgetting a node

`mesh.node_remove` takes a node's **whole** public key - never a prefix - and
removes the contact (`BaseChatMesh::removeContact`): its learned route, its
last advert, and the signal this service recorded for it. The table is
written out before the call returns, and the answer's `persisted` says
whether it was: when it is `true` a node answered as forgotten does not come
back with the next restart; it is `false` when the stored table could not be
read at start (the service does not write over a file it could not read) or
the write failed. Subscribers get a `mesh.node` event with the reason
`removed`, carrying the node as it was, before the answer is sent. A message still waiting for
its ACK keeps waiting; an ACK names the message, not the contact.

The node is not banned: it is added back the next time it adverts. Until
then no message can be sent to it - there is no contact to encrypt to - which
is exactly the state a full table leaves a *new* node in, and why forgetting
one is the remedy for a full table.

`mesh.node_reset_path` takes a whole key too, and forgets only the learned
route (`BaseChatMesh::resetPathTo`), so the next message floods and the
reply teaches a fresh one: the remedy for a node that has moved. It transmits
nothing, and raises `mesh.node` with the reason `path`.

### What it transmits unasked

Nothing on a timer. There is no periodic advert, and `mesh.advert`,
`mesh.send` and `mesh.app_send` are the only ways a client makes it transmit. `mesh.advert` takes
`zero_hop: true` for an advert sent zero-hop - heard in direct range and
repeated by nobody, at the airtime of one packet - and floods otherwise.

What it does send without being asked is what the protocol owes a sender: an
**ACK**, and a **return path**, for a message addressed to this node. That is
correct MeshCore behaviour and it is the reason the init script ships
disabled — a node that is switched on is a node that will answer. The same
goes for an app datagram that arrives by flood: it is answered with a
five-byte receipt on MeshCore's return path, so its sender learns a route.

### App datagrams

`mesh.app_send` and `mesh.app_inbox` (docs/api/mesh.md, "App datagrams")
carry opaque packets between applications on two Doors nodes as MeshCore
`PAYLOAD_TYPE_REQ`s whose data starts `0xD0 | port`. Before them this node
served no requests: `onContactRequest()` returned "no reply" without reading
its arguments. It now reads them for exactly this shape and still answers
every other request with nothing. The length the datagram carries is checked
against the decrypted length before anything is copied, and a REQ can only
reach that handler from `BaseChatMesh::onPeerDataRecv()` - a RESPONSE carried
in a PATH payload goes to `onContactResponse()`, which reads nothing, behind
the PATH guard below. Whether the frame came by flood is noted in an
`onPeerDataRecv()` override that then calls upstream unchanged; the vendored
tree is not touched.

Received datagrams are held in a 32-entry ring for this run, for a client
that was not listening, and numbered from 1 per run; `mesh.status` carries a
`run_id` so a client knows when the numbering began again.

### The PATH guard

`mesh::Mesh` computes the length of a PATH payload's trailing field as
`extra_len = len - k`, and nothing checks `k <= len`
(`vendor/RIFT/src/Mesh.cpp:172`; recorded as debt 2 in
`protocols/meshcore/README.md`). A payload that declares a longer path than it
carries makes the subtraction negative, and the `uint8_t` truncation turns it
into a large positive length handed on with a pointer near the end of a
184-byte stack buffer. The default handler reads four bytes of it for an ACK
and passes the whole claimed length to `onContactResponse()` for a RESPONSE.

Running the MeshCore receive path in a daemon is what makes that reachable
from the air. It needs a valid MAC, so the sender must be a contact — but
MeshCore adds contacts from adverts by itself, so any node that adverts can
get there.

meshcored refuses such a payload in its own `onContactPathRecv()` override,
before anything reads through the pointer, and does not touch the vendored
tree. The test is exact rather than a heuristic: in a well-formed payload
`k + extra_len` is the decrypted length, which cannot exceed
`MAX_PACKET_PAYLOAD`; in the underflow case `extra_len` is `256 + len - k`, so
`k + extra_len` is `256 + len`, always more. No well-formed payload is refused
and no malformed one is accepted. Refusals are counted as
`path_payloads_refused`.

`tests/meshcored_runtime_test.cpp` drives all three cases — well formed,
crafted, and a payload that exactly fills its block — with a real MAC over a
real shared secret, and the suite runs again under ASan and UBSan.

The upstream defect is unchanged and remains recorded as debt.

## What is persistent

Under `$POCKETOS_STATE_DIR/meshcored` (default `/var/lib/pocketos/meshcored`),
directory mode **0700**.

| File | Mode | |
| --- | --- | --- |
| `identity.id` | 0600 | 96 bytes: public key then private key. Byte for byte a MeshCore `.id` — the same file `tools/meshcore-frame` writes and reads, so a bench identity moves between them without conversion |
| `state.v1` | 0600 | the node's advert name and its known nodes: public key, name, advert type, flags, return path, last advert timestamp, last modified, position |
| `channels.v1` | 0600 | the group channels this node has joined: slot, local name, key length, and **the pre-shared key itself** |

**Runtime only, and not persisted in this phase:** messages — channel ones
included — the duplicate table, per-node signal readings, every counter, and
the radio profile. `mesh.messages` reports `persistent: false` rather than
leaving that to be discovered, and `mesh.channels` reports `persistent: true`
for the same reason.

`channels.v1` is a separate file from `state.v1` rather than a section of it,
for two reasons that both matter. It holds key material and `state.v1` does
not, so the two have different consequences when one of them is unreadable.
And the losses are not comparable: a corrupt node table costs a rediscovery
the mesh performs on its own, while a corrupt channel table costs every key an
operator typed in by hand, and **nothing on the air will bring one back**.
Separate files mean one fault cannot take the other with it, and
`mesh.status` reports `state_fault` and `channel_fault` apart for the same
reason.

A channel key is persisted at all because the alternative is a node that
forgets every channel on each reboot, which makes the feature unusable. It is
0600 in a 0700 directory, beside `identity.id`, which already holds this
node's private key — so the file adds a secret to a directory that was already
the most sensitive thing this service owns, and does not lower the bar. It
never leaves the service: no `mesh.*` method reports a key, and
`tests/meshcored_lint.sh` checks that.

**The message id counter is runtime only as well**, and that is the half of
it a client can be caught by: `_next_msg_id` starts at 1 in the runtime's
constructor, so the ids of one run mean nothing in the next. A client that
keeps messages of its own has to empty them when this service restarts. There
is no per-run identifier in the API to tell it so; see "Read 'while it runs'"
under `mesh.messages` in docs/api/mesh.md for the signal that is available
and what RIFT does with it.

Both files are written whole or not at all, and **both are flushed twice**:
once for the contents, once for the directory entry that names them. `fsync()`
on a file says nothing about the entry pointing at it, so a power cut in
between can leave a complete file nothing can reach - which for an identity is
a node that silently becomes somebody else on its next start. A failed
directory flush is reported as a failed write, and the identity file is
removed rather than left half promised.

Messages are left out deliberately. Writing decrypted message text to the
device is a privacy decision the owner has not made, and this phase was asked
to keep persistence small. It is the obvious next step and is recorded in
docs/KNOWN_ISSUES.md.

### Node capacity

The node table holds **1000** (`MAX_CONTACTS`,
`protocols/meshcore/compat/mc_contacts.h`), and everything sized by it
follows that one number: `mcdstore::MAX_NODES` (what `state.v1` carries),
`MCD_MAX_NODES` (what `mesh.nodes` lists), and the per-node signal readings.
`mesh_runtime.cpp` static-asserts the three against each other. Nothing is
evicted at any size; see "Forgetting a node".

Measured on the x86-64 host with a real service on a seeded table
(`tests/meshcored_harness_test.sh`, section 8b), against the same
measurement at the previous 256:

| | 256 | 1000 |
| --- | --- | --- |
| `state.v1`, full | 37,932 B | 148,044 B (44 + 148 per node; the count is 16 bits, so the format is unchanged) |
| resident at start, full table | 3,828 kB | 4,484 kB |
| `mesh.nodes`, short names, no paths | 46 KB | 181 KB |
| `mesh.nodes`, every stored field at its longest | 110 KB | 431 KB; about 600 KB with a run's signal readings on every node, against `POCKETIPC_MAX_FRAME` 1 MiB |
| peak resident after `mesh.nodes`, longest fields | 4,512 kB | 6,824 kB (the reply is built whole, as cJSON and then as text) |

The two `state.v1` buffers are static (296 KB of bss at 1000) and the
`NodeState` the runtime loads and saves through is on the stack (about
184 KB, twice at start), well inside meshcored's 8 MiB stack limit
(VERIFIED on unit B, `/proc/<pid>/limits`).

Every received packet walks the table linearly a few times: MeshCore's
`lookupContactByPubKey()` and `searchPeersByHash()`, and this service's
telemetry slot and retained-contact checks - a few thousand 32-byte compares
at 1000, against an Ed25519 verify per advert that costs far more.

**A reply larger than the socket buffer needs a reader that keeps up.**
pocketipc gives a frame one 200 ms budget from its first `EAGAIN`
(`POCKETIPC_SEND_TIMEOUT_MS`) and drops a client that does not drain it in
time. About 215 KB fits in a Unix socket here before the first `EAGAIN`, so
a 181 KB reply never waits, and a 431 KB one waits for one drain: RIFT reads
every 100 ms and the harness proves that at its cadence, but a shell stalled
for more than 200 ms while a large reply is in flight loses its connection
and asks again after reconnecting. The board's socket buffer is the host's:
`net.core.wmem_default` 212,992, VERIFIED on unit B.

On unit B (docs/hardware/MESH_NODE_CAPACITY_1000_GATE.md), with 1000 nodes
of which 741 were synthetic at their longest, the reply was 368 KB. It was
built and sent in 90 ms. Readers draining every 50, 100 and 150 ms all got it
whole, and RIFT on the panel held the connection and listed all 1000.
meshcored's resident set was about 5.9 MB, against 4.0 MB at 256.

### A fault in one is not a fault in the other

The identity and the node table are not the same kind of thing.

- **`identity.id` is fatal.** It cannot be reconstructed, so a file that is
  there and wrong stops the service and is never replaced.
- **`state.v1` is a cache.** A corrupt or incompatible one used to be fatal
  too - and that was wrong in a way that took the node off the air: the
  runtime refused to start, meshcored exited, the supervisor restarted it, it
  read the same file and exited again, and after five rounds gave up with a
  crash-loop marker. A cache did that.

  Now the file is **moved aside** - renamed to `state.v1.corrupt.N`, never
  rewritten, because it is the only evidence of whatever went wrong - the node
  starts with no known nodes, keeps its identity, and learns the mesh again
  from the next adverts. `mesh.status` carries `state_fault` saying what was
  wrong and where the file was kept.

  If it cannot be moved, nothing is written for that run at all: the file is
  the only record there is, and a fresh table over it would destroy that.

### The identity

- **Generated once**, on first start, from the host CSPRNG, redrawing if the
  public key starts with 0x00 or 0xFF (MeshCore reserves both as path-hash
  markers).
- **Written with `O_EXCL`** and never overwritten. A file that is already
  there is this node's identity.
- **Validated before use**: the private key must pass MeshCore's own
  `validatePrivateKey()`, and the stored public key must be the one that
  private key derives. Otherwise this node would sign as one node and be named
  as another.
- **A file that is there and wrong stops the service.** It is never replaced
  silently. Generating a new key would make this a different node to every
  peer that knows it, and would destroy the only copy of something nothing
  else holds. The service logs why and exits 1.

`state.v1` is checked the same way: magic, version, a record count that
accounts for the file's exact length, a path length MeshCore itself would
accept, no node without an advert type, no two nodes with the same key, and
names forced to terminate inside their field.

Both files are written to a temporary in the same directory, flushed, and
renamed over the real one, so a reader sees either the whole previous file or
the whole new one. The node table is written at most every 10 seconds while it
has changed, and again on a clean stop.

## Remote text

An advert name and a message body are chosen by whoever is on the air. Every
one of them leaves through a sanitiser: well-formed UTF-8 passes byte for
byte, anything that is not becomes U+FFFD, and control characters other than
newline and tab go the same way. Invalid UTF-8 would otherwise make an IPC
frame unparsable - one hostile advert breaking every client's read of every
event - and an ESC sequence, which JSON escapes and a client decodes straight
back out, would land intact in whatever shows it.

The bytes are changed **only on the way out**. What MeshCore holds, hashes and
would put back on the air is untouched. The full rule is in docs/api/mesh.md,
"Remote text".

## Security

- The private key is loaded, used to sign, and reported by nothing.
- Remote-controlled text cannot produce an invalid IPC frame or carry a
  terminal escape sequence through (above).
- Nothing a client sends becomes part of a path. The state directory comes
  from the command line or the environment; the file names are constants.
- No `system()`, `popen()` or `exec*()` anywhere in the service.
- Every IPC input is bounded before use: key prefixes 2 to 64 hex characters,
  text 1 to 160 bytes with no control characters other than newline and tab,
  `params` must be an object, frames 1 to 255 bytes of strict hex.
- An ambiguous node prefix is refused rather than resolved by guessing.
- Rapid client reconnection, a subscriber that never reads, malformed radio
  events, oversized frames, stale and duplicate completions and a reconnect
  loop are all exercised by the two host suites.

## Building, installing and enabling

**In every image, disabled per unit.** The image package builds and installs
meshcored (`pocketos.mk`, `ENABLE_MESHCORED=1`) together with `S65meshcored`,
since the third-party notices gained entries for the three trees it compiles
(docs/LICENSING.md item 9, 2026-09-22). An ordinary host build leaves it out:
it links `protocols/meshcore`, which needs two upstream checkouts an ordinary
Doors build does not have (`vendor/RIFT` and `vendor/Crypto`, the same two
`tools/meshcore-frame` needs). `apply_to_sdk.sh` exports those two, pin-checked,
into the package's `third_party/`, with the verified commit beside each
(`.doors-pinned-commit`), because the package tree has no `.git`. A checkout
exported with uncommitted changes (only under `POCKETOS_ALLOW_PIN_DRIFT=1`)
is recorded as `<commit>-dirty`, which the pin check refuses unless the
package is built with `MESHCORE_ALLOW_UNPINNED=1`: it is not the
pinned source, and the record says so.

```sh
make ENABLE_MESHCORED=1 meshcored     # build
make meshcored-test                   # its suites, plain then sanitised
make ENABLE_MESHCORED=1 install       # install /usr/sbin/meshcored (gated on the notices)
```

The riscv64 check is the ordinary cross-build with the switch on, in a copy of
the tree (objects land beside their sources, so a cross-build in the working
checkout would clobber the host ones):

```sh
make ENABLE_MESHCORED=1 CC=<cross>gcc CXX=<cross>g++ AR=<cross>ar \
     CFLAGS="--sysroot=$SYSROOT -O2 -Wall -Wextra" \
     CXXFLAGS="--sysroot=$SYSROOT -O2" LDFLAGS="--sysroot=$SYSROOT" all
```

### The service

`S65meshcored` runs meshcored under `pos-supervise`, like radiod and the
shell: restart with a backoff doubling from 1 s to 30 s, a crash loop declared
after more than five restarts in a minute, and the supervisor's state file
`/run/pocketos/meshcored.state`, which is what `system.status` reports as the
`meshcored` service. Its order is radiod (S60), meshcored (S65), the shell
(S90); nothing waits on anything, because meshcored looks for radiod, backs
off and keeps looking (sections 1, 2 and 4 of `tests/meshcored_service_test.sh`),
and RIFT does the same for meshcored (`tests/rift_ipc_test.c`). On the way
down BusyBox stops them in reverse, so meshcored hands the lease back before
radiod goes, and writes its node table on SIGTERM.

What the script refuses, and says so:

| Situation | `start` | `stop` |
| --- | --- | --- |
| `MESHCORED_ENABLE` not 1 | `disabled`, exit 0 | stops any meshcored that is running |
| enabled, `/usr/sbin/meshcored` missing or not executable | `FAILED: enabled, but ...`, exit 1 | — |
| enabled, `pos-supervise` missing | `FAILED`, exit 1: meshcored is never run unsupervised | — |
| already running under it | `already running`, exit 0 | stops supervisor and daemon, confirms both gone |
| a meshcored it did not start is running (by hand, or orphaned) | `FAILED: ... running unsupervised (pid N)`, exit 1 | ends it too: SIGTERM, then SIGKILL after 3 s |
| stale pid files naming nothing | starts normally | `not running`, and removes both |
| the supervisor killed, its meshcored still running (an orphan) | — | SIGTERM to meshcored, so it writes its node table; SIGKILL only if it has not gone in 3 s |

Beneath the script, **one process per node and per socket**: before it reads
the identity or opens its socket, meshcored takes an exclusive `flock` on
`<runtime dir>/<socket name>.lock`, then one on its state directory. A second
one on either leaves at once with exit code 3 - `another process is already
serving the meshcored socket`, or `another meshcored is already running on
...`. Two processes on one state directory would be two radios claiming one
identity, each rewriting the other's node table; two on one socket name, even
with different state directories, would have the second take the first one's
clients, because pocketipc unlinks a socket path before it binds. The locks
are the kernel's, so they cannot go stale. The socket's is taken first, so a
second meshcored on the same node and the same socket gives the socket's
reason.

### Keeping a service whole

Unit A was found (2026-09-22) with `/usr/sbin/meshcored` installed and no
`/etc/init.d/S65meshcored`, so nothing started it at boot, and with
`/etc/doors-release` naming a different build from the binaries beside it.
Nothing in the repository put it in that state: its image predated meshcored,
and every bench gate since had copied the binary alone and started it by hand.
Three checks now stand in the way of that, all built on
`tools/release/check_rootfs.sh`, which asks a root filesystem whether every
service is a binary and its init script together, both executable, whether
`pos-supervise` is there, and whether every Doors binary is the build its
release file names (each carries the string `DOORS_BUILD_ID=<id>`):

- `deploy.sh` runs it on the tree it is about to send and refuses before it
  contacts the unit. Init scripts carry no build stamp, and a package-only
  rebuild does not refresh the tree's copies (Buildroot copies the overlay in
  only when it finalises the rootfs), so it also refuses a tree whose init
  scripts differ from the overlay `apply_to_sdk.sh` applied. On the unit it
  stops any meshcored no init script started before it stops radiod, and
  refuses to start services if what arrived is not whole. If `S65meshcored`
  itself fails to start, the rest - the shell included - still starts, and
  the deploy then exits non-zero naming it (`tests/deploy_staging_test.sh`,
  `tests/initscript_test.sh`).
- `verify_image.sh` runs it on the image's root partition, so an image with
  either half of a service missing, a mode wrong or a binary from another build
  is refused before it is flashed (`tests/image_contents_test.sh`). An image
  with no root partition it can read passes as `PASS (boot partition only)`,
  and says the root partition was not checked.
- Copying one binary onto a unit by hand is what produced the state above.
  Deploy with `deploy.sh`, which carries the whole set from one build.

### Enabling it on a unit

`S65meshcored` ships in the image and **starts nothing**: it prints
`disabled` and returns. Starting meshcored acquires the radio — it takes
radiod's lease, applies the MeshCore profile and listens on 869.618 MHz for as
long as it runs — and on a unit where nobody asked for that, the right number
of radios to take is none.

To switch it on, per unit:

```sh
echo MESHCORED_ENABLE=1 > /etc/default/meshcored
/etc/init.d/S65meshcored restart
```

Leave `MESHCORED_NAME` unset unless the node is to be named from here: the
name is stored in `state.v1`, and a name given here replaces it at a start.
A rename made on the device afterwards (`mesh.set_name`, RIFT's RENAME) is
kept over the configured name - `settings.v1` records which configured name
it replaced - until `MESHCORED_NAME` is changed to something else, which
then wins again.
`MESHCORED_TX_POWER_DBM` defaults to 2 dBm.

Before doing that on a real radio, know what it means:

1. **radiod must be on the `sx1262` backend** for anything to reach the air,
   which is itself a per-unit opt-in in `/etc/default/radiod`, and the vendor
   launcher must be off (it drives the same SPI device).
2. **The antenna on MMCX1 must be confirmed** before any transmit
   (docs/hardware/BRINGUP_CHECKLIST.md §5).
3. **The node will answer.** A message addressed to it produces an ACK and a
   return path, without a client and without being asked. That is correct
   MeshCore behaviour and it is airtime.

Stopping it releases the lease and writes the node table.

## Evidence

| Claim | Class | Basis |
| --- | --- | --- |
| An enabled build cannot be installed, packaged or imaged while the notices say nothing about what it contains | **VERIFIED host** | `tests/notices_test.sh` executes the refusal on the install, image and package paths, and proves the gate is driven by the notices rather than unconditional |
| A refused profile releases the radio and another client can take it | **VERIFIED host** | `tests/meshcored_service_test.sh`, against the real radiod |
| A full contact table produces no phantom node, no state churn and no telemetry eviction | **VERIFIED host** | `tests/meshcored_runtime_test.cpp`, 1000 real contacts (999, then the 1000th, then the 1001st turned away) then 18 more adverts |
| A full table of 1000 is persisted and reloaded whole, and is still full after the reload | **VERIFIED host** | same (`test_full_contact_table`); the 1000 / 1001 record boundary of `state.v1` in `tests/meshcored_store_test.cpp` |
| `mesh.nodes` lists the most recently heard first, and after a restart by the stored last-updated time | **VERIFIED host** | `tests/meshcored_runtime_test.cpp` (`test_nodes_newest_first`); RIFT's 64-node cache keeps the head of that list, `tests/rift_model_test.c` |
| Each sent message times out on its own deadline; an ACK for one does not strand another; a full outbox refuses rather than overwrites | **VERIFIED host** | `tests/meshcored_runtime_test.cpp` (`test_ack_deadlines`), three nodes, deadlines driven through `mcd_runtime_expire_acks` |
| A send after every deadline has passed is not refused as busy; an ACK queued behind another frame is matched before its deadline is judged | **VERIFIED host** | same, in real time: no tick between the deadlines passing and the send, and an ACK held one turn behind a frame A hears back |
| A forgotten node is gone from the table, the file and the list, refuses a message, and is learned again from its next advert; a route can be forgotten on its own | **VERIFIED host** | same, and over IPC in `tests/meshcored_service_test.sh` (section 3c) |
| A zero-hop advert goes out as MeshCore's zero-hop (route bits DIRECT, empty path) and is learned by a peer in range | **VERIFIED host** | `tests/meshcored_runtime_test.cpp`; accepted over IPC in `tests/meshcored_service_test.sh` |
| Remote text cannot make an IPC frame unparsable or carry an escape sequence | **VERIFIED host** | `tests/meshcored_util_test.c`, and end to end in the two-node harness |
| The headers and the library come from one checkout, and a mismatch is refused | **VERIFIED host** | `tests/meshcored_source_identity_test.sh`, which also builds the refusal |
| An identity is not reported as persisted until its directory entry is durable | **VERIFIED host** | `tests/meshcored_store_test.cpp`, through the directory-flush hook |
| A corrupt node state does not stop the node | **VERIFIED host** | `tests/meshcored_runtime_test.cpp`, six kinds of bad file, each followed by a working node |
| The lease, profile, subscription and reconnection against the **real radiod** | **VERIFIED host** | `tests/meshcored_service_test.sh`, radiod on its mock backend |
| A signed MeshCore advert is verified and learned as a node | **VERIFIED host** | same, using a frame from `tools/meshcore-frame` — the tool whose frames the accepted P0 gate's peer accepted |
| Two nodes: advert learned, directed text received, ACK returned, path learned | **VERIFIED host** | `tests/meshcored_harness_test.sh`, two whole processes over a mock air |
| Identity and node table survive a restart | **VERIFIED host** | same, and `tests/meshcored_runtime_test.cpp` |
| The four transmit outcomes, duplicate and stale completions, a lost completion, a disconnect mid-transmit | **VERIFIED host** | `tests/meshcored_harness_test.sh` drives each one |
| Malformed and hostile `radio.rx` | **VERIFIED host** | both suites |
| A corrupt identity file stops the service rather than being replaced | **VERIFIED host** | `tests/meshcored_runtime_test.cpp`, `tests/meshcored_store_test.cpp` |
| App datagrams: sent flood then direct once the receipt teaches the route, received whole up to 160 bytes, upstream request types and malformed lengths ignored, the inbox bounded and numbered | **VERIFIED host** | `tests/meshcored_runtime_test.cpp` (`test_app_datagrams`), real crypto, crafted REQs for the refusals; plain and under ASan/UBSan |
| Two whole meshcored processes carry a Fleet match end to end | **VERIFIED host** | `tests/fleet_mp_e2e_test.sh`, over the mock air |
| App datagrams between two units on the air | **UNRESOLVED** | the P7 gate, docs/hardware/FLEET_MULTIPLAYER_GATE.md |
| The PATH guard refuses the crafted payload and accepts every well-formed one | **VERIFIED host** | `tests/meshcored_runtime_test.cpp`, plain and under ASan/UBSan |
| The boundary: no SPI, GPIO, radio library, LVGL, or JSON below the seam | **DOCUMENTED** | `tests/meshcored_lint.sh`, statically |
| The MeshCore wire format this speaks | **VERIFIED hardware**, by inheritance | the accepted P0 gate, from the same pinned sources; that gate is evidence about the frames, not about this daemon |
| The daemon on unit A's radio | **VERIFIED hardware** | docs/hardware/MESHCORED_HARDWARE_GATE.md, RIFT_CHANNELS_GATE.md, RIFT_IMPROVEMENTS_GATE.md - every one of them started meshcored by hand, none through `S65meshcored` |
| `S65meshcored`: opt-in, start, stop, restart, stale pid files, each refusal, a crash restarted and a crash loop declared, under the real `pos-supervise` | **VERIFIED host** | `tests/initscript_test.sh` |
| One process per state directory and per socket name; the second leaves before it touches the identity or the socket | **VERIFIED host** | `tests/meshcored_service_test.sh`, section 1b: same node, same socket from another node, same node under another socket |
| A tree or image with half a service, a wrong mode, no `pos-supervise`, a per-unit switch shipped, a binary from another build, or init scripts older than the applied overlay is refused before it reaches a unit | **VERIFIED build/packaging** | `tests/deploy_staging_test.sh`, `tests/image_contents_test.sh`, `tests/initscript_test.sh` (the unit-side half of `deploy.sh`) |
| meshcored started by `S65meshcored` at boot on a unit, and a unit deployed with `deploy.sh` since | **UNRESOLVED** | not yet run on hardware |
| Behaviour on a real, busy MeshCore network | **UNRESOLVED** | the mock air is lossless, instant, collision-free and has no range |
| The service under a real duty cycle | **UNRESOLVED** | the dispatcher's airtime budget is upstream's default and has not been exercised against a regulatory limit |
