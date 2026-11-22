# ADR-017: IP peer datagrams (`peerd`) and their security

Status: Accepted (product owner, 2026-11-20), on the terms under "Terms of
acceptance". Nothing is implemented; the acceptance is of the design.
Date: 2026-11-20
Deciders: product owner (final), AI engineering partner (author)

## Context

Fleet over the local network (docs/design/games-multiplayer/M2-PLAN.md) needs
a second transport with the contract its mesh link already has: lossy,
unordered, possibly duplicated datagrams between proved 32-byte identities,
with an inbox that outlives the client. The owner approved the direction on
2026-11-20 on these terms: a dedicated security ADR and a review before
implementation; established cryptographic constructions; no custom handshake
without justification and validation; libsodium as the preferred library.

Binding constraints: services own I/O and apps reach them over pocketipc
(ADR-002); the shell links no network-facing parser and no crypto library
(ADR-007); a rotation re-executes the shell in place (`ui/shell/shell.c`);
the board has no clock that survives a power cut, so nothing may depend on
wall time (ADR-008, risks).

## Terms of acceptance

The product owner accepted this ADR on 2026-11-20:

- libsodium, the construction of "Decision" and the limitations of §9 are
  accepted, including **no forward secrecy, for play on a local network**.
- A human cryptographic review is **not required before local prototype
  development**. An independent human review is **recommended before
  broader deployment on untrusted networks**.
- Every item of "Validation before implementation carries a game packet"
  stands, and so do the checks named in "Evidence": the entropy source,
  the state directory's filesystem, known-answer vectors, the replay and
  restart tests, fuzzing, and the sanitizer suites.
- **Approval of the design does not mean the implementation is validated.**
  That is shown by those tests and by the hardware gate, each of which
  needs its own go-ahead.

## Requirements

1. A long-term identity per device; the sender of a datagram is proved.
2. Confidentiality and integrity per datagram (a Fleet layout is secret
   until its reveal).
3. **No state to re-establish with the opponent of a match in hand.**
   Either service or device may restart at any time, and that peer's next
   datagram must be accepted without a round trip. A peer with no match in
   hand may have to be heard again first.
4. Discovery beacons are authenticated.
5. Replays are rejected by the transport, not only tolerated by games.
6. Trust on first use; a changed key is a different player.
7. No first-party cryptographic primitive.

Not goals in v1: forward secrecy, anonymity on the LAN, hiding who talks to
whom, or defence against code already running on the device (every Doors
process is root, docs/api/network.md).

## Options

| | A. libsodium, static keys | B. Noise IK/XX on OpenSSL primitives | C. DTLS 1.2 (OpenSSL), pinned keys |
| --- | --- | --- | --- |
| Handshake | none | specified, implemented here | standard, connection-shaped |
| Requirement 3 | inherent | a handshake after every restart, and rules for packets in flight | the same, per peer, on one UDP socket |
| Forward secrecy | no | yes | yes |
| First-party security code | framing, replay window, key file | the Noise state machine, key schedule, rekeying, replay | pinning glue, peer multiplexing, timers |
| Dependency | libsodium (new to the image) | none new | none new |

## Decision

**Option A**, with libsodium 1.0.18 from the pinned SDK's Buildroot, built in
its default **minimal** configuration. `BR2_PACKAGE_LIBSODIUM_FULL` is not
needed (Evidence).

### 1. Service

`peerd` is a supervised service like meshcored, disabled per unit by default
(`PEERD_ENABLE=0`). It is the only process that parses IP input from peers
and the only one that links libsodium. Its pocketipc surface mirrors
`mesh.app_*` (docs/design/games-multiplayer/ARCHITECTURE.md §5.5,
M2-PLAN.md §1.3) and is specified in `docs/api/peer.md` when built. It binds
no socket while it holds neither a visibility lease nor a kept peer.

### 2. Identity

- One Ed25519 key pair per device (`crypto_sign_keypair`). The 32-byte
  public key **is** the identity games and players see. Where a person
  compares identities, **sixteen** hex digits are shown (64 bits), in
  groups of four. Four can be matched with about 65,000 generated keys and
  eight with a few billion, which is within offline reach (review R5).
- Stored as `$POCKETOS_STATE_DIR/peerd/identity.v1`: magic, version, the
  64-byte secret key, a checksum. Mode 0600, written once, temp-flush-rename
  and the directory flushed. Never logged, never returned over IPC. It is
  held for the life of the process in `sodium_malloc` memory; it, the
  derived X25519 secret and every cached session key are wiped on exit.
- **Created only when the `peerd/` state is absent**, from the kernel's
  random source through libsodium: the epoch files first, the identity
  last. Epoch files without an identity count as an interrupted first
  creation, and creation is finished, **only if** the epoch still has its
  initial value and no `kept.v1` exists. In every other case - a raised
  epoch or a kept record without an identity, or an identity with
  unreadable or incomplete state - nothing is regenerated: peerd reports
  `identity_fault` or `epoch_fault` and stays closed. Replacing the
  identity is an explicit, logged operator action, because a new one
  orphans a saved match and every peer's memory of this device.
- A lost or replaced key is a new player. There is **no revocation**: a
  stolen key works until its owner replaces it and every peer forgets it.

### 3. Keys for sealing

- X25519 keys are derived from the Ed25519 ones with
  `crypto_sign_ed25519_sk_to_curve25519` and `_pk_to_curve25519` (own public
  key included). The conversion of a peer's key **must succeed**; a key it
  refuses (small order, off the main subgroup) is not a node.
- Directional session keys come from `crypto_kx` over the X25519 keys: the
  device with the bytewise lower Ed25519 public key takes the client role.
  One key per direction, so a datagram cannot be reflected or passed off as
  travelling the other way. `crypto_kx` binds the X25519 keys; the binding
  to the **Ed25519 identities is the associated data of §4**, which carries
  the sender's whole key.
- A frame whose sender key equals this device's is dropped before any other
  work, and `peer.keep` or a send to the own key is refused. (A device
  hears its own broadcasts; a cloned state directory gives equal keys.)
- Every return value is checked: `sodium_init`, both conversions, the key
  exchange (which refuses an all-zero secret), verification, decryption.
- Session keys are memory-only and recomputed after a restart. No rekeying.

### 4. Sealed datagrams

`crypto_aead_xchacha20poly1305_ietf` with the direction's key.

```text
off  size  field
0    1     wire version (1)
1    1     type (3 = DATA)
2    32    sender's Ed25519 public key
34   8     recipient key prefix (a filter only; the key authenticates)
42   8     epoch, big-endian        (§6)
50   8     sequence, big-endian     (§6)
58   24    nonce
82   n+17  ciphertext of  port(1) | payload(n)  and the 16-byte tag
```

- **Associated data is bytes 0 to 57.**
- **Nonce: 24 random bytes per datagram.** Nonce safety depends on neither
  a counter nor the disk; two devices cloned from one state do not reuse
  one. Epoch and sequence are for replay only.
- Receive order: length, version, type; own-key drop; recipient prefix;
  the sender is a node (§5); a read-only replay precheck; the tag; then the
  replay state is updated. An unknown sender costs no public-key operation.
- **Without a visibility lease, DATA is accepted from kept peers only.**
- Payload 1 to 1024 bytes (a frame of at most 1123, no IP fragmentation);
  `port` 1 to 15, as `mesh.app_send`.

### 5. Nodes, kept peers and signed frames

**A node** is a key peerd will exchange datagrams with. It becomes one by a
verified signed frame, or by being kept.

**Kept peers** are the opponents of matches in hand (`peer.keep {key}`,
from the game; at most 4). Each has a persisted record in
`peerd/kept.v1`: the whole key, the highest epoch and sequence accepted
(§6), the last address. The records are loaded at start, so a kept peer is a
node from the first instant after a restart and peerd binds its socket
whenever one exists - **requirement 3 holds for exactly the peers that
matter**, with no beacon and no client attached. A record lasts until the
game releases it (the match ends or is put away; as amended, Amendment 1:
thirty quiet seconds after the match ends, at once when it is put away, and
at once when the game leaves the network unless the match is unfinished);
peerd has no timed expiry, because there is no clock to time it by. A fifth `peer.keep` is refused
with an error. `peer.kept` lists the records, and a game reconciles **both ways**
when it opens or resumes: it releases any record it has no match for and
asserts `peer.keep` again (idempotent) for every saved match, so a record
lost to a fault is restored. A re-created record has no replay floor until
the first datagram is accepted. An operator command releases one by hand.

**Signed frames** (beacon, probe, probe reply, lookup, lookup reply) are in
clear,
signed with `crypto_sign_detached` over `"DOORS-PEERD-1" | frame`. They
carry the sender's key, epoch, sequence, the sender's own IPv4 address,
name (up to 24 bytes of checked UTF-8), wire version and at most one table
announcement. Receive order: length, version, type; own-key drop; for a
known node a read-only replay precheck; verification, on a budget separate
for known and unknown keys so junk cannot starve genuine beacons; then
state is updated.

- **A beacon proves "this key once said this", not presence.** A receiver
  with no state for a key cannot tell a recorded beacon from a live one.
  Liveness is shown only by a challenge-bound reply or a sealed exchange; a
  listed player or table is a claim until then, and the lobby must not
  present it as more.
- **A probe** (ADD BY ADDRESS) is a signed frame from the prober carrying
  16 random bytes; the **reply is signed over those bytes** and must come
  from the address probed. The prober then holds a node proved live; to
  the responder the probe is an ordinary signed frame, and its own proof
  comes with the first sealed exchange. A
  **lookup** for key K is a signed frame carrying a challenge likewise and
  is answered only by K itself, signed.
- Replies are capped globally as well as per source, since a source
  address can be forged, and are never larger than about three times the
  request. Unicast from another subnet is allowed (manual join across a
  router); broadcasts use TTL 1.
- **Stated address.** A signed frame states the address of the interface it
  is sent from. One whose stated address differs from its UDP source is
  dropped when the key is unknown, and for a known key updates replay state
  only, never the endpoint. Peers behind address translation are therefore
  **not supported**; a routed subnet without translation is.
- **Without a visibility lease** signed frames are processed from kept keys
  only, and a lookup is answered only for a kept requester: a device that
  is not visible makes no nodes from strangers and answers none.
- **Table**: 32 nodes. Kept peers hold reserved slots and are never
  evicted; the rest are replaced least-recently-heard first. An on-link
  attacker can still fill the unreserved slots with invented players; that
  spoils discovery, not a match in hand.
- **Inbox**: 32 datagrams, at most 8 per sender (a ninth pushes out that
  sender's oldest; datagrams waiting for their write, §6, count). A kept
  peer's are never pushed out by another sender's, and one arriving at a
  full inbox pushes out the oldest that is not a kept peer's.
- **No pin store in v1.** Nothing remembers a name-to-key binding outside a
  saved match; a name is a label, two keys may claim it, and the
  remembered-players store (M4) is where a change will first be shown.

### 6. Replay protection

No clock, so freshness is the sender's own persistent counter.

- **Epoch**: 64 bits, kept in two checksummed files (`epoch.a`, `epoch.b`);
  the larger valid one wins. Each run writes the new value over the smaller
  or invalid one, in place, flushed, **before** a socket is bound; the other
  copy is untouched, so the epoch does not depend on an atomic rename. No
  valid copy while the identity exists is `epoch_fault`, closed - never a
  restart from 1, which would lock the device out of every kept peer.
- **Sequence**: 64 bits from 1 per run, one space for all frame types;
  0 is invalid. At either counter's maximum peerd stops sending.
- **Window**, per node: the highest epoch accepted, the highest sequence
  `top`, and a 64-bit map of `top-63 .. top`. A higher epoch resets it. In
  the same epoch: above `top` is accepted and advances it (a jump of 64 or
  more clears the map); inside the map and unseen is accepted; seen, or
  below the map, is dropped. A datagram delayed behind more than 64 later
  frames is therefore lost, and the game's retry resends it.
- **Kept peers: a DATA high-water mark is persisted** as the pair
  (epoch, sequence) of the newest DATA frame accepted, compared by epoch
  first: a higher epoch replaces the record and restarts the sequence, and
  within an epoch a higher sequence replaces it, whatever the in-memory
  `top` is (beacons share the sequence space and move `top` without being
  persisted). It is written durably whenever an accepted DATA frame exceeds
  the stored pair, and the datagram reaches the inbox only **after** the
  write has landed. After a restart the window starts in the stored epoch
  with `top` at the stored sequence and every position seen, so nothing at
  or below the mark is accepted: an opponent's earlier moves cannot be
  replayed into a restarted device, including one that had arrived behind
  a beacon or early in a new epoch.
- These writes are capped per kept peer per second; one write of the
  newest mark covers every datagram waiting behind it, and waiting
  datagrams count against the sender's 8 in the inbox. The last address is
  written only when it changes, under the same cap. `kept.v1` is written
  temp-flush-rename with the directory flushed. Missing, it is an empty
  list; unreadable, it is `kept_fault`: peerd runs with no kept peers, so
  the non-kept residual below applies to all of them until games assert
  their keeps again (§5), and says so.
- Only a frame bearing the sender's signature or the pairwise key changes
  replay state, so a third party cannot advance it to lock a peer out.
- **Residual**: for a node that is *not* kept, a restarted receiver has no
  state, and frames of that node's current or older epochs are accepted
  once more: a ghost player, table or invitation. Games validate their own
  session and state, and a ghost invitation dies at the first reply.
- **Residual**: a state directory restored from an older copy rolls a
  sender's epoch back (peers ignore it until it passes theirs) and a
  receiver's records back (old frames replay). Restoring `peerd/` is
  treated as a new device: the operator removes it instead.
- `identity.v1` and `kept.v1` need an atomic rename from the state
  directory's filesystem (ext4 gives one; not verified for the unit's
  actual mount, see Evidence). The epoch files do not.

### 7. Addresses

A node's endpoint moves only on an accepted frame that **advances the
window top**, and for a signed frame only when the address it states equals
the UDP source. Broadcast, multicast, zero and own addresses are never
endpoints. A match thus survives a new DHCP lease or Ethernet's per-boot
MAC, while a frame replayed from elsewhere - one the receiver had missed,
inside the window - does not move anything.

What remains: an attacker on the link who gets a *newest* genuine sealed
frame to the receiver first, from their own address, moves the endpoint to
themselves until the node is next heard directly. They can delay or drop,
not read or forge. Likewise, after a restart a recorded beacon of a kept
peer's current epoch, sent from the address it states, can set the endpoint
back to that peer's old address until the peer is next heard. An on-link
attacker can do as much to any IP traffic; both are accepted and stated.

### 8. Recovery

| Event | Behaviour |
| --- | --- |
| peerd restarts | identity, epoch and kept records are read; the epoch is raised; the socket is bound if a kept record exists; a kept peer's next datagram is accepted with no round trip |
| Device reboots | the same; the endpoint follows the key at the next accepted frame |
| Peer restarts | its higher epoch resets this side's window |
| Shell rotates or the game closes | peerd is unaffected; datagrams wait in the inbox |
| A peer that is not kept, after a restart | not a node until heard again (a beacon or a probe); nothing is owed to it |
| Identity or epoch state unreadable | closed, with the fault named; never regenerated silently |
| Cloned state directory (two devices, one identity) | each receiver takes whichever frame arrives first and drops the other as a replay; no nonce is reused; unsupported |
| State directory lost | a new identity: a new player to everyone |

### 9. What v1 does not give

- **No forward secrecy.** The keys that protect a datagram are a function
  of the two long-term keys alone. Whoever obtains **either** device's
  secret key, at any later time, can decrypt every recorded datagram of that
  pair in both directions, and can impersonate the device.
- **Key-compromise impersonation.** With device A's secret key an attacker
  can also pose *to A* as any peer whose public key they know.
- **No recovery after compromise and no revocation** (§2). The key is on
  the device's storage, where every process is root.
- **Sealed datagrams are not evidence.** The recipient holds the same keys
  and could have made any datagram "from" its peer; a game must not treat
  one as proof against a player. Signed frames are the opposite: proof,
  transferable, of a key and the name it claimed.
- **No liveness in beacons** (§5), and replay against a restarted receiver
  for nodes that are not kept (§6).
- **No metadata protection.** The long-term key is in clear in every frame,
  so a device can be followed across networks; epoch and sequence reveal
  how often it restarted and how much it sent.
- **No resistance to denial of service from the same link** (§5, §7).
- **First contact is trust on first use.** Only the same sixteen digits on
  both screens, compared by the two people, say the key is the friend's; an attacker present at first
  contact can stand between two players and relay.
- Accepted for v1 because the traffic is game moves and a fleet layout
  revealed at the end of every match, on a local network. **Not acceptable
  as it stands for the online route**, which needs its own ADR and, most
  likely, an ephemeral exchange over the same identities.

### 10. Limits

32 nodes (4 reserved for kept peers), 16 announced tables, a 32-datagram
inbox with 8 per sender, 1024-byte payloads; receive budgets per source,
global, and separately for known and unknown keys; replies capped globally;
beacons at most one per 2 s; IPv4 only. The UDP port and the exact figures
are fixed with `docs/api/peer.md` at acceptance; none is asserted here.

## Validation before implementation carries a game packet

1. Owner acceptance of this ADR: given 2026-11-20 ("Terms of acceptance").
2. An independent review of §2-§10: **three passes, 2026-11-20** (see
   "Independent review"); a human review remains the owner's to require.
3. Known-answer tests: fixed keys, epoch, sequence and nonce produce fixed
   frames; libsodium's own vectors for the primitives.
4. A fuzz target for the frame parser; ASan and UBSan suites.
5. Negative tests by name: reflected datagram, re-addressed datagram,
   altered header, replay in, above and below the window, a jump of 64 or
   more, sequence 0, lower epoch, replay of a kept peer's frames after a
   restart, small-order and off-subgroup keys (a gate, not an assumption),
   own key as sender, unknown sender, another recipient, a probe reply
   without the challenge or from another address, a beacon whose stated
   address is not its source, a full node table with a kept peer in it,
   DATA accepted behind a beacon then replayed after a restart, a fifth
   keep, a stranger's beacon and lookup with no lease, a power cut between
   the epoch files and the identity at first creation, an identity file
   deleted from a device with a raised epoch, DATA of a new epoch with a
   lower sequence then replayed after a restart, a kept record lost and
   re-asserted, a kept peer's datagram arriving at a full inbox.
6. The entropy check of "Evidence": units A and B, from one image, make
   different identities.

## Consequences

Needed now: this ADR accepted; libsodium selected in the Doors defconfig
and recorded in `docs/LICENSING.md` and `THIRD_PARTY_LICENSES.md` (ISC);
`services/peerd`, its init script, `docs/api/peer.md`.

Useful soon: `peer.status` counters for every drop reason; an operator
command to show the identity's fingerprint.

Future: the online route's ADR (forward secrecy, a relay); dropping root.

Risks: first-party framing and replay code is security-critical though
small; the endpoint rule of §7; the residuals of §6; libsodium 1.0.18 is the
version the pinned Buildroot carries, not the newest upstream.

Migration cost if option B is chosen later: peerd's sealing layer and wire
version change; identities, the pocketipc surface, games and saves do not.

## Evidence

| Claim | Class | Basis |
| --- | --- | --- |
| libsodium is packaged in the pinned Buildroot 2025.02.1, version 1.0.18, ISC, built `--enable-minimal` unless `BR2_PACKAGE_LIBSODIUM_FULL` | VERIFIED locally | `package/libsodium/libsodium.mk` in the build host's SDK tree, 2026-11-20 |
| It is not selected in either defconfig today | VERIFIED locally | `# BR2_PACKAGE_LIBSODIUM is not set` in both `.config` files |
| The minimal build contains `crypto_sign` (Ed25519), both Ed25519-to-X25519 conversions, `crypto_kx`, `crypto_aead_xchacha20poly1305`, `crypto_generichash`, `randombytes` and `sodium_memzero` | DOCUMENTED | upstream tag 1.0.18: `src/libsodium/Makefile.am` lists them outside `if !MINIMAL`; `include/sodium.h` includes their headers outside `SODIUM_LIBRARY_MINIMAL`; the conversions are in `crypto_sign/ed25519/ref10/keypair.c` |
| The minimal build omits only: XChaCha box and secretbox variants, `crypto_core_ed25519`, ristretto255, `crypto_scalarmult_ed25519`, scrypt, siphashx24, salsa2012/208, `crypto_stream_xchacha20`, obsolete signing | DOCUMENTED | the same two files; none is used here |
| `pk_to_curve25519` rejects small-order and off-subgroup keys | DOCUMENTED | `keypair.c` at 1.0.18 |
| The library builds and passes its tests for the K230 toolchain | **not verified** | nothing was built or installed for this ADR |
| The kernel's random source is initialised before peerd first makes a key, and no seed is shared through the image | **not verified** | to be checked on units A and B before the gate (review R11) |
| The unit's state directory is on a filesystem with atomic rename | **not verified** | the P7 gate reports ext4 for the card; the mount under `$POCKETOS_STATE_DIR` is to be confirmed |
| A rotation does not touch a service | VERIFIED source | `execv` in `ui/shell/shell.c`; services run under `pos-supervise` |
| Everything in "Decision" | PROPOSED | this ADR |

## Independent review

2026-11-20. Three passes over §2-§10 against requirements 1-7 by a separate
AI reviewer session that was given this file, not the author's reasoning: a
first review (R1-R15), a second pass over the text as revised for it
(N1-N8), and a third over the text as revised again (P1-P5), which also
compared it with M2-PLAN.md §1.3-1.4. **It is not a review by a human
cryptographer**; the owner did not require one before prototype work
("Terms of acceptance"). The
design above is the text after all three; the corrections made for the
third pass are clarifications and were not sent back a fourth time.

Verdicts. First pass: the construction (static keys, `crypto_kx`,
XChaCha20-Poly1305, epoch plus window) is acceptable and need not be
replaced, once R1 is fixed and R2-R5 are fixed or accepted as stated
residuals. Second: no blockers remain; N1 and the eight-digit claim to be
corrected before acceptance. Third: **no blockers remain; every earlier
finding is resolved in the design text; the text is fit for the owner's
acceptance decision as a design**, with P1-P5 and some wording mismatches
as corrections that do not change the construction.

First pass:

| # | Severity | Finding | Disposition |
| --- | --- | --- | --- |
| R1 | BLOCKER | "Sender must be a known node" contradicted requirement 3: after a restart the opponent's datagram was dropped until a beacon, which might never come; the socket might not even be bound; an unsigned probe left the responder not knowing the prober | **Fixed**: persisted kept-peer records loaded at start, socket bound while one exists (§5, §8); the probe is signed and challenge-bound (§5); no timed keep expiry |
| R2 | MAJOR | Only the epoch was persisted for kept peers, so a restarted device accepted the opponent's earlier moves again | Fixed in part at the first pass; **fixed** after N1: a DATA high-water mark, persisted before delivery (§6) |
| R3 | MAJOR | Node table and inbox could be exhausted from the link, evicting the opponent or their waiting move | **Fixed** for a match in hand: reserved slots, eviction rule, kept-only DATA without a lease, inbox caps (§4, §5). **Accepted residual**: invented players in the lobby |
| R4 | MAJOR | A missing or rolled-back epoch file was undefined and could lock a device out of its peers | **Fixed**: `epoch_fault`, two copies, created only in a first creation (§2), counters stop at maximum (§2, §6). **Accepted residual**: a restored state directory. The reviewer's optional epoch hint is not adopted (it is a round trip) |
| R5 | MAJOR | Four hex digits can be matched by brute force; no pin store; beacons have no freshness; the probe reply was not bound to the probe | **Fixed**: challenge-bound probe and lookup (§5); sixteen digits, after the second pass judged eight still within offline reach (§2). **Stated**: no pin store in v1; a beacon is a claim, not presence |
| R6 | MINOR | Own or equal key undefined | Fixed (§3, §8) |
| R7 | MINOR | Check order and verification budget for signed frames | Fixed (§4, §5, §10) |
| R8 | MINOR | The endpoint rule was easier to abuse than stated | Fixed in part: stated address must match, only a top-advancing frame moves it (§7); the remainder stated |
| R9 | MINOR | "Does not give" was incomplete | Fixed (§9) |
| R10 | MINOR | `crypto_kx` binds X25519 keys, not Ed25519; return values; key lifetime in memory | Fixed (§2, §3) |
| R11 | MINOR | No evidence for the entropy source | Added as not verified, with a gate (Evidence, Validation 6) |
| R12 | MINOR | Window semantics underspecified | Fixed (§6) |
| R13 | MINOR | "On-link only" was unenforceable and contradicted the manual join across subnets; lookup replies | Fixed (§5) |
| R14 | INFO | Cloned state directory | Stated (§8) |
| R15 | INFO | The AEAD is not key-committing; no consequence with pairwise directional keys; no fragmentation | Noted |

Second pass:

| # | Severity | Finding | Disposition |
| --- | --- | --- | --- |
| N1 | MAJOR | Persisting only when DATA advanced `top` missed DATA accepted inside the window behind a beacon; it replayed after a restart | **Fixed**: DATA high-water mark (§6) |
| N2 | MINOR | The probe proves liveness to the prober only | Fixed: reworded (§5) |
| N3 | MINOR | A stated address that differs from the source was undefined; address translation breaks it | Fixed: rule stated, translation unsupported (§5) |
| N4 | MINOR | A durable write per datagram in the receive path | Fixed: capped, delivery after the write (§6) |
| N5 | MINOR | Kept records with no expiry could leak; a fifth keep undefined | Fixed: refusal, `peer.kept`, reconcile, operator release (§5) |
| N6 | MINOR | With a kept peer and no lease, strangers' signed frames and lookups were still served | Fixed (§5) |
| N7 | MINOR | A power cut at first creation left a permanent fault; epoch file rule; `kept.v1` faults | Fixed (§2, §6) |
| N8 | MINOR | A recorded beacon can reset a kept peer's endpoint after a restart | Stated (§7) |

Third pass (all MINOR):

| # | Finding | Disposition |
| --- | --- | --- |
| P1 | The high-water mark was compared by sequence alone, so the first DATA of a new epoch was never persisted | Fixed: the mark is the pair (epoch, sequence), epoch first (§6) |
| P2 | "Epoch files without an identity" could not tell an interrupted first creation from a lost identity file, and would have regenerated silently | Fixed: only at the initial epoch with no kept records (§2) |
| P3 | A kept record lost to a fault was never restored | Fixed: games reconcile both ways and re-assert (§5, §6) |
| P4 | The queue behind a write was unbounded; `kept.v1`'s write method unstated | Fixed (§5, §6) |
| P5 | A kept peer's datagram was not guaranteed room in a full inbox | Fixed (§5) |
| - | Wording: lookup missing from the list of signed frames; "done once"; R4's disposition; M2-PLAN §1.3-1.4 out of step on the probe, the endpoint rule, when Fleet asserts a keep, and the inbox | Fixed here and in M2-PLAN. The pass also reported requirement 3 as still universal; it had already been narrowed |

Found sound as written: one Ed25519 key converted for agreement; role by
key order, no reflection or unknown-key-share with the sender's whole key
in the associated data; random 192-bit nonces; the layout and lengths; that
no third party can advance a victim's replay state; tag before replay
update; "never regenerate silently"; the forward-secrecy and KCI
statements (first pass); the in-place two-file epoch rule; the window
restored at the mark; signed frames from kept keys only without a lease;
the stated-address rule, under which a device on two interfaces of one LAN
alternates harmlessly between two valid endpoints (third pass).

**Blockers remaining: none in the design.** Still open after acceptance:
the "not verified" rows of Evidence; `docs/api/peer.md` with the port and
figures; a human review before deployment on untrusted networks
(recommended, "Terms of acceptance").

## Amendment 1 (ACCEPTED 2026-11-22): when the game releases a kept peer

Status: Accepted (product owner, 2026-11-22, with the approval to merge PR
164). The decision stands as accepted; this amendment makes one statement of
§5 exact. peerd is unchanged: it still has no timed expiry, and a record
still lasts until the game releases it.

**Why.** A match over on one side may not be over on the other: the packet
that would finish it there can be the one that was lost, and the opponent
then sends its last packet again and needs an answer. Released the moment
the match ended here, the opponent was left retrying into a closed socket
(3 matches in 30 at 15 % loss, end to end over two real peerd processes)
until it settled by itself about half an hour later.

**§5, "A record lasts until the game releases it (the match ends or is put
away)", now reads as follows.** The game releases a kept peer:

- **thirty seconds after the match has ended on this side and the opponent
  has been quiet** - the LAN profile's six tries at its longest wait, counted
  from the later of the match ending here and the opponent's last datagram;
  every datagram from them starts the wait again; or
- **at once, when the match is put away** (CLOSE) or another begins; or
- **at once, when the game leaves the network** - Fleet closes, another
  transport is picked, the service is looked for afresh - if that wait is
  all the record is held for. Nobody would be there to answer, and peerd
  would otherwise keep the record indefinitely.

**Never released on leaving:** the opponent of a match in play, of an
unfinished match read back from its save, or of a match over with its END
still owed. Such a match is resumed from its save, and its opponent must
still be reachable then; the record lasts until that match ends or is put
away, as before.

**Known limits, as built.** A game that dies without closing (killed,
crashed) releases nothing; the record is reconciled away when the game is
next on the network. A player who leaves inside the thirty seconds cuts the
wait short, and an opponent whose last datagram was lost then settles by
itself, with the same record, as before this amendment. The release written
as the link closes, and the hide after it, are best effort; a hide that is
lost ends with the lease (30 s).

Evidence: `docs/apps/FLEET_MULTIPLAYER.md` (the LAN link, "To know about
it"); PR 164 (unit tests, mutations, and end-to-end runs with a
deterministically lost final datagram and an unanswered END). Not verified
on a device.
