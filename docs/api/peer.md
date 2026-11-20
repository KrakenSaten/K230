# peer.* API v0 (peerd)

Status: draft, api_version 0, wire version 1. Transport: pocketipc, socket
`peerd.sock`. The decision, its alternatives and what the design does not
give are docs/decisions/ADR-017-peer-datagrams.md (Accepted, design); this
document is the surface that ADR left to be fixed when the service was
built. **Host-built and host-tested only: nothing here has been on a device
or on a real network** (see "Not validated").

`peerd` carries opaque packets between applications on two Doors devices on
an IP network, as meshcored's app datagrams do on LoRa (docs/api/mesh.md,
"App datagrams"). It does not read them. It gives a datagram three things:
the sender is proved, nobody else can read it, and it is not accepted twice.

It is the only process that parses IP input from peers and the only one that
links libsodium (ADR-007, ADR-017).

## Not built into any image yet

`peerd` is built only on request (`make ENABLE_PEERD=1 SODIUM_DIR=<prefix>
peerd`), because no Doors image selects libsodium yet. There is no init
script and no Buildroot wiring in this version; both, and the first image
that carries it, are a separate step that needs an image build and the
owner's go-ahead. Run by hand it is `peerd [--port N] [--name NAME]`, with
the usual `POCKETOS_*_DIR` variables.

## Silent and closed until asked

peerd binds no UDP socket while it is neither **visible** nor holding a
**kept peer**. Nothing is announced and nothing is heard.

- **Visible** is a lease of 30 s (`peer.set_visible`), renewed by the client
  that wants it - a lobby that is open. While it holds, peerd announces
  itself every 2 s on each interface's broadcast address (TTL 1) and hears
  strangers. When it lapses, peerd goes quiet by itself.
- **A kept peer** is the opponent of a match in hand (`peer.keep`). While one
  exists peerd stays bound and exchanges datagrams with that key - and with
  nobody else - whether or not anything is visible or any client is attached.

## Identity

One Ed25519 key pair per device, made the first time peerd runs on an empty
state directory. The public key (32 bytes, 64 hex digits) **is** the
identity: it is the `from` of every datagram and the `to` of every send.
Where two people compare identities they compare the **fingerprint**: the
key's first sixteen hex digits in four groups, `1a2b 3c4d 5e6f 7081`.

A name (`peer.set_name`, up to 24 bytes of text with no control character)
travels in announcements. It is a label, not an identity: two keys may claim
one name.

State, under `$POCKETOS_STATE_DIR/peerd/` (0700):

| File | |
| --- | --- |
| `identity.v1` | the secret key; 74 bytes, 0600, written once |
| `epoch.a`, `epoch.b` | the epoch, twice; 16 bytes each, 0600 |
| `kept.v1` | the kept peers: key, replay floor, last address; 0600 |
| `name.v1` | the name |

**Nothing is repaired by making something new.** A state with an identity
and no readable epoch, or with an epoch that has moved on (or a kept list)
and no identity, is a fault: `peer.status` reports `identity_fault` or
`epoch_fault`, peerd binds nothing, and every other method fails with code 4
naming the fault. The damaged files are left as they are. A new identity is
a new player to everyone, so replacing one is something a person does
deliberately: stop peerd and remove the whole `peerd/` directory.

## Methods

Every method answers at once. A method that changes something rejects
parameters it does not know (code 2). Clients must ignore unknown result
fields.

### peer.info

Result: `service` (`"peerd"`), `api` (0), `wire` (1), `version`, `build`.

### peer.status

Result: `state` (`"ready"`, `"identity_fault"`, `"epoch_fault"`,
`"store_fault"`), `run_id` (16 hex digits, new on every start), `port`,
`socket` (whether the UDP socket is bound), `kept_fault` (`kept.v1` was
there and unreadable when peerd started: there are no kept peers until a
client asserts them again). When ready, also `visible`, `epoch`,
`kept_write_pending`, `interfaces` (`address`, `broadcast`) and `counters`:

| Counter | |
| --- | --- |
| `rx`, `tx`, `tx_failed` | frames in and out |
| `accepted_data`, `accepted_signed` | frames that verified and were taken |
| `drop_malformed` | wrong length, version, type or field |
| `drop_own` | this device's own key as the sender |
| `drop_other` | not addressed to this device |
| `drop_unknown` | a sealed datagram from a key that is not a node |
| `drop_not_visible` | from a stranger while not visible |
| `drop_stated` | a stranger's signed frame from another address than it states |
| `drop_replay` | seen before, below the window, or an older epoch |
| `drop_budget` | the verification budget was spent |
| `drop_bad_key` | a key that cannot be used |
| `drop_auth` | the tag or the signature did not verify |
| `drop_inbox_full` | no room in the inbox |
| `reply_limited` | a reply not sent: over the reply limits |
| `kept_writes`, `kept_write_failed` | writes of `kept.v1` |
| `evicted_nodes` | nodes replaced by newer ones |

### peer.identity

Result: `key`, `fingerprint`, `name`.

### peer.set_name

Params: `name`. Result: `name`. Stored; used from the next announcement.

### peer.set_visible

Params: `visible` (boolean). Result: `visible`, `lease_ms` (30000 or 0).
A client that wants to stay visible calls it again well inside the lease.

### peer.nodes

Result: `nodes`, `count`. A node is a key peerd will exchange datagrams
with: one whose announcement or probe verified, or a kept peer.

| Field | |
| --- | --- |
| `key`, `fingerprint`, `name` | |
| `address` | **only when known** |
| `heard_ms_ago` | **only when heard this run** |
| `kept` | a kept peer |
| `proved` | shown live this run: a reply signed over this device's own random challenge, or a sealed datagram from a **kept** peer above the floor that was on disk when this run of peerd began. **An announcement proves only that this key once said so, and a sealed datagram from a node that is not kept may be a recording**; a client must not present an unproved node as present |
| `route` | `"lan"` |

At most 32 nodes. Kept peers have four places to themselves; the other 28
are replaced least-recently-heard first. Only a node's newest announcement,
arriving from the address it states, changes its name, table, address or
`heard_ms_ago`.

### peer.announce / peer.withdraw / peer.tables

`peer.announce` params: `port` (1 to 15), `proto_min`, `proto_max` (0 to
255, min not above max), `seats` (1 to 255). Result: `tag`, 32 random bits
that name this table. It rides in this device's announcements while visible.
`peer.withdraw` ends it. `peer.tables` lists the tables other nodes
announced in the last 10 s: `host`, `fingerprint`, `name`, `port`,
`proto_min`, `proto_max`, `seats`, `tag`. One table per node, at most 16
listed. A listed table is a claim, as a node is.

### peer.probe / peer.probes

For a network where announcements do not get through. `peer.probe` params:
`address` (dotted IPv4), `port` (optional; the service port). Needs
visibility (code 3 without). Result: `probe_id`. peerd sends a signed
question carrying 16 random bytes, three times a second apart.

`peer.probes` result: `probes`, each `probe_id`, `address`, `port`, `state`
(`"pending"`, `"answered"`, `"no_answer"`) and, when answered, `key`. An
answer counts only when it is signed over this probe's random bytes and
comes from the address that was asked; the node it names is then `proved`.
At most four probes are outstanding. A probe ends as `no_answer` when
visibility ends: it never holds the socket open by itself.

### peer.keep / peer.release / peer.kept

`peer.keep` params: `key`. Idempotent. The record is on disk before the
call answers; code 4 when it could not be written (and then nothing is
kept). What was already accepted from that key - an invitation, say - is
its replay floor from that moment, and what is waiting from it in the inbox
is protected as a kept peer's from that moment. At most four (code 3 for a fifth). This device's own key and a key
that cannot be used are refused (code 2).

`peer.release` params: `key`; code 2 when that key is not kept.

`peer.kept` result: `kept` (`key`, `floor` - whether a datagram from it has
been accepted and recorded, `address` when known), `count`, `kept_fault`
(false again once a keep or a release has written the list anew).

A client that owns matches reconciles **both ways** whenever it starts or
resumes: release every kept peer it has no match for, and keep again the
opponent of every match it holds, so a record lost to a fault is restored.

### peer.app_send

Params: `to` (a whole key), `port` (1 to 15), `payload_hex` (1 to 1024
bytes). Result: `accepted`, `route` (`"lan"`), `est_timeout_ms` (500: what a
client sizes its retry on), `bytes`.

Unacknowledged, as `mesh.app_send` is: `accepted` means the sealed frame was
handed to the network. A client that needs delivery confirms it in its own
protocol.

Errors: 2 for a key that is not whole or not a node, a bad port or payload;
3 when not visible and the node is not a kept peer; **5 when the node has
no address yet** - a kept peer that has not been heard is being looked for,
and the client tries again shortly; 4 when the frame could not be sent.

A kept peer not heard for 10 s is also asked for (on its last address and
on each interface, at most once in 5 s) while a send to it is tried: only
that peer answers, signed over the question's random bytes, and its address
follows its key. That question carries this device's key, name and address
in clear and goes out **even when it is not visible**: it is the one thing
a hidden device with a match in hand says to the link.

### peer.app_inbox

Params: `port` (1 to 15), `after_id` (optional, default 0). Result:
`datagrams` (oldest first), `count`.

The last 32 datagrams, across all ports, for this run only; at most 8 from
one sender (a ninth pushes out that sender's oldest). A kept peer's are
never pushed out by another sender's, and one arriving at a full inbox
pushes out the oldest that is not a kept peer's. Ids start at 1 with each
run; when `run_id` changes a client starts again from `after_id` 0.

A datagram, here and in the `peer.app` event:

| Field | |
| --- | --- |
| `id` | 1 upwards, per run |
| `port` | 1 to 15 |
| `from` | the sender's whole key: proved, because only that key and this one can seal it |
| `payload_hex` | the payload, exactly as sent |
| `route` | `"lan"` |
| `mono_ms` | when it arrived, on this device's monotonic clock |

A kept peer's datagram reaches the inbox only once a replay floor at or
above it is **on disk** - an older one arriving beside a newer one waits
with it - so nothing delivered can be replayed into this device after a
restart or a power cut. The list is written at most four times a second, so
a datagram may wait about half a second; while the list cannot be written
at all, datagrams wait (`kept_write_pending`, `kept_write_failed`) and none
is delivered without its floor.

### peer.subscribe / peer.unsubscribe

Events, to subscribed connections: `peer.app`, one per datagram as it
enters the inbox.

## Errors

| Code | Meaning |
| --- | --- |
| 1 | unknown method |
| 2 | invalid or malformed parameters; not a node; not kept |
| 3 | policy: not visible, or no room (a fifth kept peer, a fifth probe) |
| 4 | the store or the network failed; or peerd is closed by a fault |
| 5 | busy: no address for that node yet |

## On the wire

UDP, port **47230** by default (`--port`). The number is this project's
choice and has not been checked against any registry. IPv4 only.

Every frame begins with the same 58 bytes:

```text
off  size
0    1     wire version (1)
1    1     type: 1 BEACON, 2 PROBE, 3 DATA, 4 PROBE_REPLY, 5 LOOKUP, 6 LOOKUP_REPLY
2    32    sender's Ed25519 public key
34   8     recipient key prefix (DATA and the two replies; otherwise zero)
42   8     epoch, big-endian
50   8     sequence, big-endian
```

**DATA** (100 to 1123 bytes): a 24-byte random nonce, then
`port(1) | payload` sealed with XChaCha20-Poly1305 (16-byte tag). The 58
bytes above are the associated data. The key is the direction's own, from
`crypto_kx` over the two devices' keys (ADR-017 §3).

**Signed frames** (always 210 bytes, so a reply is never larger than the
question):

```text
58   4     the sender's own IPv4 address on the interface it sends from
62   2     the UDP port it listens on, big-endian
64   1     flags (bit 0: a table is announced)
65   1     name length, 0..24
66   24    name, zero padded
90   8     table: port, proto_min, proto_max, seats, tag(4)
98   16    challenge (PROBE, LOOKUP: the asker's; replies: echoed)
114  32    target key (LOOKUP: the key sought; otherwise zero)
146  64    Ed25519 signature over "DOORS-PEERD-1" | bytes 0..145
```

A frame is dropped silently unless its length is exactly right for its
type, its version is 1, every field is in range, every field its type does
not use is zero, and it verifies.

**Replay.** Epoch and sequence are the sender's: the epoch is raised and
written to disk once per start, before any socket is bound (by one, or by
two when one of its two copies is unreadable, so a number already used on
the air is never used again); the sequence
counts from 1 across all frame types. A receiver keeps per node the highest
epoch, the highest sequence and a 64-entry window, and for a kept peer
persists the newest accepted datagram's (epoch, sequence) as a floor
(ADR-017 §6).

**Addresses.** A node's endpoint moves only on an accepted frame newer than
any seen from it, and for a signed frame only when the address it states is
the address it came from. Peers behind address translation are not
supported.

## Limits

32 nodes (4 places for kept peers); a 32-datagram inbox, 8 per sender;
1024-byte payloads; 4 kept peers; 4 probes; 16 tables listed. Signature
checks: 50 a second for any one known key, 200 a second for all known keys
together, 20 a second for keys that are not known - so neither invented
keys nor frames forged in one key's name can spend what a known peer's
announcements need. Replies: one a second to any one node, 10 a second to
kept peers and 10 a second to everyone else. One announcement per interface
every 2 s while visible.

ADR-017 section 10 also named per-source and global receive rate limits.
They are not built: a source address can be forged, so a per-source limit
would bound nothing, and every frame is refused before any costly work
unless it passes the budgets above. Frames forged in a kept peer's name can
still use up that one peer's fifty checks a second - fewer forgeries than
it took before the per-key budget, which is the price of protecting every
other key - and so delay its announcements and its answers to a lookup;
its sealed datagrams, and its address following them, are not affected.

## What it does not give

ADR-017 §9, in short: **no forward secrecy** (whoever later obtains either
device's secret key can read every recorded datagram between the two); no
revocation; no protection of who talks to whom; no defence against denial
of service from the same link; first contact is trust on first use, and
only the same fingerprint on both screens, compared by the two people, says
a key is a friend's. A node that is not kept is forgotten at a restart -
and within a run too, once 28 newer keys have been heard, which someone on
the link can arrange in seconds - and one recorded announcement or datagram
of its can then be accepted once more. Such a datagram is delivered, moves
that node's address to where it came from, and proves nothing (`proved`
stays false). Accepted for play on a local network; an independent human review is
recommended before use on untrusted networks.

## Test vectors

`tests/peer_crypto_test.c` holds the cryptography to answers from outside
this code: libsodium's Ed25519-to-X25519 conversion vector, RFC 8032 test 1,
and the XChaCha20-Poly1305 vector of draft-irtf-cfrg-xchacha A.3.1. The keys
that must be refused - the five small-order points, one point of mixed order
(RFC 8032's first public key plus a point of order 8:
`3b5b475c…2af60d`) and one encoding that is not a point - were computed and
checked with independent arithmetic, not with libsodium. The session keys, a
sealed text and a signature for fixed inputs are recorded from this
implementation so that a change in what goes on the wire cannot pass
unnoticed.

## Tests

`make ENABLE_PEERD=1 SODIUM_DIR=<prefix> peerd-test`:

| Test | Covers |
| --- | --- |
| `tests/peer_wire_test` | every type at every length; every field the parser refuses; names |
| `tests/peer_replay_test` | each window rule; restore after a restart; 200,000 random frames against a model |
| `tests/peer_crypto_test` | the published vectors; refused keys; reflection and direction; every altered byte of a sealed and of a signed frame |
| `tests/peer_store_test` | first creation, every later start, both epoch copies, every fault that must not regenerate, the kept list |
| `tests/peer_core_test` | engines wired back to back: discovery, datagrams, forged / altered / reflected / re-addressed frames, replay in and below the window and across epochs, kept peers across restarts, the floor behind a beacon and in a new epoch, the write cap, bad keys, strangers while not visible, probe and lookup, stated address, endpoint following the key, node table and inbox exhaustion, the verification budget, the end of the sequence counter |
| the same five under ASan and UBSan | |
| `tests/peer_fuzz` | the parser and receive path under ASan and UBSan: random bytes and mutations of genuine frames; nothing that is not a genuine frame is ever accepted |
| `tests/peerd_test.sh` | two real peerd processes on two loopback addresses over pocketipc |
| `tests/peerd_lint.sh` | libsodium called from one file; no primitive written here; no socket, file or clock in the engine; the secret never logged or returned |

## Not validated

- **On a device**: nothing. Not cross-run, not installed, no image.
- **On a real network**: announcements over an interface's broadcast
  address are exercised by no automated test - loopback has no broadcast.
  They were checked once by hand on the build host's own interface (silent
  before; one 210-byte announcement every 2 s with TTL 1 from the
  interface's address while visible; its own announcements dropped; silent
  and unbound after). Discovery between two machines has not been tried.
- **ADR-017's open evidence**: that the kernel's random source is ready
  before the first identity is made and that units do not share a seed;
  that the state directory's filesystem gives an atomic rename.
- **Independent human review** of the construction and of this code.
