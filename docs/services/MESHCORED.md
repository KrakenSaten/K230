# meshcored — the MeshCore protocol service

Status: implemented, host-validated, **not enabled in any image and never run
on hardware**. VERSION is unchanged at 0.0.10.

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
a future RIFT client           owns the presentation
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
  becomes a contact, up to 32. Past that, MeshCore reports the discovery
  anyway, with a contact it is about to throw away, so a UI can say somebody
  adverted and it could not be kept. meshcored does not treat that as a node:
  no `mesh.node` event, no state marked dirty, no telemetry slot taken from a
  node that *was* kept. It is counted as `nodes_unretained` so that a full
  table is visible rather than merely quiet. The table-full policy itself is
  upstream's and unchanged.
- **This node is not a repeater.** `allowPacketForward()` stays false, so it
  hears everything and forwards nothing.

### What it transmits unasked

Nothing on a timer. There is no periodic advert, and `mesh.advert` and
`mesh.send` are the only ways a client makes it transmit.

What it does send without being asked is what the protocol owes a sender: an
**ACK**, and a **return path**, for a message addressed to this node. That is
correct MeshCore behaviour and it is the reason the init script ships
disabled — a node that is switched on is a node that will answer.

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

Not in `make all` or `make test`, and **off by default**. It links
`protocols/meshcore`, which needs two upstream checkouts an ordinary Doors
build does not have (`vendor/RIFT` and `vendor/Crypto`, the same two
`tools/meshcore-frame` needs). With `ENABLE_MESHCORED` unset, `make all`,
`make test`, the Buildroot package and the image are byte for byte what they
were.

```sh
make ENABLE_MESHCORED=1 meshcored     # build
make meshcored-test                   # its suites, plain then sanitised
make ENABLE_MESHCORED=1 install       # install /usr/sbin/meshcored
```

The riscv64 check is the ordinary cross-build with the switch on, in a copy of
the tree (objects land beside their sources, so a cross-build in the working
checkout would clobber the host ones):

```sh
make ENABLE_MESHCORED=1 CC=<cross>gcc CXX=<cross>g++ AR=<cross>ar \
     CFLAGS="--sysroot=$SYSROOT -O2 -Wall -Wextra" \
     CXXFLAGS="--sysroot=$SYSROOT -O2" LDFLAGS="--sysroot=$SYSROOT" all
```

### Enabling it on a unit

`S65meshcored` ships in the image and **starts nothing**: it prints
`disabled` and returns. Starting meshcored acquires the radio — it takes
radiod's lease, applies the MeshCore profile and listens on 869.618 MHz for as
long as it runs — and on a unit where nobody asked for that, the right number
of radios to take is none.

To switch it on for a hardware session, per unit:

```sh
cat > /etc/default/meshcored <<'EOF'
MESHCORED_ENABLE=1
MESHCORED_TX_POWER_DBM=2
MESHCORED_NAME=K230-A
EOF
/etc/init.d/S65meshcored restart
```

Before doing that on a real radio, know what it means:

1. **radiod must be on the `sx1262` backend** for anything to reach the air,
   which is itself a per-unit opt-in in `/etc/default/radiod`, and the vendor
   launcher must be off (it drives the same SPI device).
2. **The antenna on MMCX1 must be confirmed** before any transmit
   (docs/hardware/BRINGUP_CHECKLIST.md §5).
3. **The node will answer.** A message addressed to it produces an ACK and a
   return path, without a client and without being asked. That is correct
   MeshCore behaviour and it is airtime.
4. **Nothing about this service has been on a radio.** Everything below is
   host evidence.

Stopping it releases the lease and writes the node table.

## Evidence

| Claim | Class | Basis |
| --- | --- | --- |
| An enabled build cannot be installed, packaged or imaged while the notices say nothing about what it contains | **VERIFIED host** | `tests/notices_test.sh` executes the refusal on the install, image and package paths, and proves the gate is driven by the notices rather than unconditional |
| A refused profile releases the radio and another client can take it | **VERIFIED host** | `tests/meshcored_service_test.sh`, against the real radiod |
| A full contact table produces no phantom node, no state churn and no telemetry eviction | **VERIFIED host** | `tests/meshcored_runtime_test.cpp`, 32 real contacts then 18 more adverts |
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
| The PATH guard refuses the crafted payload and accepts every well-formed one | **VERIFIED host** | `tests/meshcored_runtime_test.cpp`, plain and under ASan/UBSan |
| The boundary: no SPI, GPIO, radio library, LVGL, or JSON below the seam | **DOCUMENTED** | `tests/meshcored_lint.sh`, statically |
| The MeshCore wire format this speaks | **VERIFIED hardware**, by inheritance | the accepted P0 gate, from the same pinned sources; that gate is evidence about the frames, not about this daemon |
| Anything at all on unit A | **UNRESOLVED** | no hardware was touched |
| Behaviour on a real, busy MeshCore network | **UNRESOLVED** | the mock air is lossless, instant, collision-free and has no range |
| The service under a real duty cycle | **UNRESOLVED** | the dispatcher's airtime budget is upstream's default and has not been exercised against a regulatory limit |
