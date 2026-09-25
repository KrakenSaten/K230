# PocketFleet multiplayer: protocol v1

Two players, one match, turn based, over the MeshCore mesh. The decision and
its alternatives are docs/decisions/ADR-007-fleet-multiplayer.md (Proposed).
Single-player Fleet is unchanged (docs/apps/POCKETFLEET.md).

Status: host-built and host-tested only. **Nothing here has been on a
radio.** The hardware gate is docs/hardware/FLEET_MULTIPLAYER_GATE.md (P7),
not yet run.

## Layers

```text
apps/fleet/net/      pure C: SHA-256, the packet codec, the match state
                     machine, the match save codec. No LVGL, no IPC, no
                     filesystem (tests/fleet_lint.sh).
apps/fleet/link/     the transport seam: fleet_link.h, and fleet_link_mesh.c,
                     the one file that speaks mesh.app_* over pocketipc.
apps/fleet/ui/       the lobby screen, and Battle/Result reading a match.
apps/fleet/engine/   fleet_store.c also reads and writes match.v1.
services/meshcored   mesh.app_send / mesh.app_inbox / mesh.app (generic,
                     port-numbered; knows nothing about Fleet).
```

```text
Fleet ── fleet_link_mesh ──[mesh.app_*]──▶ meshcored ──[radio.*]──▶ radiod ──▶ SX1262
```

## Identities

| | |
| --- | --- |
| player | the node's meshcored public key (`mesh.identity`) |
| peer name | the name in the peer's signed advert (`mesh.nodes`), shown with the first 4 hex digits of its key |
| session id (`sid`) | 24 bits, chosen by the host from the OS random source, never 0 |
| host / guest | the host sends INVITE; the guest receives it |
| ply `k` | shot number, 1 to 200. The **guest fires odd plies**, the host even plies |

Both nodes must already hold each other as MeshCore contacts, which happens
when each has heard the other's advert. The lobby offers **Make visible**
(`mesh.advert` zero-hop) for that.

## Packet format

What Fleet hands to `mesh.app_send` (port 1) and gets back from `mesh.app`:

```text
off  size  field
0    1     vt: version (bits 7-6, = 01 for v1) | type (bits 5-0)
1    3     sid, big-endian, non-zero
4    1     ply; in COMMIT and REVEAL, which have no ply, the flags
5    n     body
```

COMMIT and REVEAL carry their flags in the ply byte rather than in the body,
which keeps REVEAL to two AES blocks.

meshcored puts it in a MeshCore REQ as `tag(4) | 0xD1 | length(1) | packet`
(docs/api/mesh.md, "App datagrams"): `0xD0 | port`, then the length, because
the decrypted REQ is padded to the AES block and MeshCore records no length
of its own. A packet of at most **10 bytes** is one AES block: a **22-byte
frame, 304.1 ms**. Up to **26 bytes** is two blocks: **38 bytes, 386.0 ms**
(zero hops; each hop adds a path byte and a retransmission). Every Fleet
packet is one or the other.

Field encodings:

| | |
| --- | --- |
| cell | `row * 10 + col`, 0 to 99 |
| res | bits 1-0 outcome (1 miss, 2 hit, 3 sunk); bits 4-2 ship (0-4 when sunk, else 7); bit 5 fleet destroyed; bits 7-6 zero. 0 means "none" and is valid only as `prev` in SHOT 1 |
| layout | 5 bytes, ship `i` in roster order: bit 7 vertical, bits 6-0 the bow cell |
| commit | first 16 bytes of `SHA-256("DOORS-FLEET-COMMIT-1" ‖ sid(3) ‖ owner key(32) ‖ layout(5) ‖ salt(16))` |
| digest | FNV-1a 32 over `sid(3) ‖ host commit ‖ guest commit ‖ (cell, res) for plies 1..R`, big-endian |

## Messages

| type | name | dir | body | bytes | answered by |
| --- | --- | --- | --- | --- | --- |
| 1 | INVITE | H→G | rules (0 = classic) | 6 | ACCEPT / DECLINE |
| 2 | ACCEPT | G→H | – | 5 | START |
| 3 | DECLINE | G→H | reason (0 user, 1 busy, 2 version, 3 rules, 4 busy with you) [+ active sid(3) for 4] | 6 / 9 | – |
| 4 | START | H→G | – | 5 | – |
| 5 | CANCEL | H→G | – | 5 | – |
| 6 | COMMIT | both | commit(16); flags in the ply byte (bit 0: I hold your commit) | 21 | COMMIT / SYNC |
| 7 | SHOT | shooter | cell ‖ prev (res of ply k-1, 0 for k = 1) | 7 | RESULT |
| 8 | RESULT | defender | cell ‖ res | 7 | next SHOT |
| 9 | SYNC | both | flags (bit 0 reply, bit 1 I hold your commit, bit 2 I hold your reveal, bits 5-3 phase) ‖ pending cell or 0xFF ‖ digest(4); ply = R | 11 | SYNC(reply) |
| 10 | REVEAL | both | layout(5) ‖ salt(16); flags in the ply byte (bit 0: I hold your reveal) | 26 | REVEAL |
| 11 | END | either | reason (1 forfeit, 2 void, 3 abandon, 4 cancelled, 5 unknown, 6 violation, 7 finished); ply = R | 6 | END_ACK |
| 12 | END_ACK | either | reason echoed | 6 | – |

There is no TURN_ACK and no GAME_OVER: RESULT acknowledges SHOT, the next
SHOT carries and acknowledges the RESULT before it (`prev`), REVEAL
acknowledges the final RESULT, and the final RESULT carries "fleet
destroyed". A forfeit is `END(forfeit)`.

SYNC phase codes: 0 none, 1 before start, 2 deploying, 3 committed,
4 battle, 5 reveal, 6 done.

A packet is **dropped** unless its length is exactly right for its type, its
version is 1, its sid is non-zero, every enum and cell is in range, and its
sender is the session's peer (INVITE excepted).

## Session

```text
IDLE ─Invite─▶ INVITING ─ACCEPT rx, send START─▶ DEPLOY
 └─INVITE rx─▶ INVITED ─Accept─▶ ACCEPTING ─START rx─▶ DEPLOY
DEPLOY ─confirm fleet─▶ COMMITTED ─hold peer commit─▶ BATTLE
BATTLE ─RESULT with "fleet destroyed"─▶ REVEAL ─hold peer reveal─▶ DONE
any session state ─END─▶ DONE
```

- **Crossed invites.** Inviting P and receiving P's INVITE: the invite from
  the numerically lower public key survives. The other side drops its own
  and accepts the survivor automatically; both users asked for this match.
- **Busy.** Any state but IDLE answers a stranger's INVITE with
  `DECLINE(busy)`. An INVITE from the current opponent under a new sid gets
  `DECLINE(busy with you, active sid)`; a host that no longer knows that sid
  answers `END(unknown)` for it, which voids the old match, and its next
  INVITE is accepted.
- **Cancel.** A host cancels before START with CANCEL (sent twice). A late
  ACCEPT for a cancelled sid gets `END(cancelled)`.
- **Tombstones.** The last four finished sids are remembered, with the peer
  and how they ended, so a late packet for one gets a deterministic answer
  (at most once per 30 s per sid) and never a state change.
- **Unknown sid** (not INVITE): `END(unknown)`, at most once per 30 s, and
  only when this device's match store loaded cleanly.

## Turns

R is the number of plies resolved on this device. The shooter of ply `k` is
the guest when `k` is odd.

Defender, receiving `SHOT(k, cell, prev)`:

| Condition | Action |
| --- | --- |
| k = R+1, the peer's parity, cell valid and not fired at before | fire at the own board, persist ply k, send `RESULT(k)` |
| k = R+2 and this device has shot R+1 pending | apply `prev` as `RESULT(R+1)`, then as above |
| k ≤ R | duplicate: resend the latest packet for a ply ≥ k (`RESULT(k)`, or the newer SHOT that carries it), at most once per 2 s. A different cell for a logged ply is a violation |
| anything else | send `SYNC` |

Shooter, receiving `RESULT(k, cell, res)`: k is the pending ply and the cell
matches: apply and persist. k ≤ R: ignore. Otherwise `SYNC`.

**One physical shot can never become two logical shots.** A shot is its ply
number; FIRE is armed only on this side's turn with nothing pending; the
pending `(k, cell)` is persisted before it is first sent and never changes;
the defender accepts ply `k` exactly once and answers every later copy from
its log.

## Obligations and retries

Only the side that is owed something transmits unasked. Silence is the idle
state; there are no keepalives.

| Obligation | Held while | Packet |
| --- | --- | --- |
| invite | INVITING | INVITE (6 tries, then "no answer") |
| accept | ACCEPTING | ACCEPT (6 tries, then "no answer") |
| cancel | just cancelled | CANCEL (2 tries) |
| commit | COMMITTED and no evidence the peer holds it | COMMIT |
| shot | a pending shot | SHOT(R+1) |
| reveal | REVEAL, until both reveals are held | REVEAL |
| end | an END not yet acknowledged | END (3 tries) |

Evidence that the peer holds this side's commit: its COMMIT or SYNC with the
bit set, or any SHOT or RESULT. An incoming COMMIT or REVEAL is answered
when it said "I don't hold yours" or was the first time this side saw it
(COMMIT is answered with SYNC while this side is still deploying).

Schedule: the first try at once; the next after the transport's timeout
estimate (7 s by default, meshcored's `est_timeout_ms` when it gives one)
plus 0-50 % jitter; then doubling to a 60 s cap; **at most 6 tries**. After
that the link is **lost**: the match is paused, and while its screen is open
one SYNC probe goes out every 120 s (±25 %) for up to 15 probes, then only on
**Check link**. On the opponent's turn a SYNC probe goes out after each
5 minutes of silence, up to 6 times, so a peer that ended the match while
this side waited is heard from. A transmit the transport refused as busy is
retried after 1.5 s and does not count as a try.

The reveal is asked for at most 21 times (6 tries and 15 probes), however
often the peer answers something else and however long the governor held the
requests back; after that the result stands, **not verified**.

A match that ended with END answers END to anything else that still arrives
for it, so a peer that missed the END is not left asking into silence.

**Airtime governor**: at most 6 frames in a burst, one more every 5 s, and at
most 90 s of estimated airtime in any rolling hour of the app's run
(2.5 %). An obligation or a probe waits for room, and the link says
"throttled"; a reply to a duplicate is dropped instead (its sender will ask
again). A **first** answer - the RESULT a shooter is waiting for - needs no
token, only room in the hour: dropping it would make the peer ask again,
which costs more than it saves. The simulator is what set these numbers: at
60 s an hour and one token every 10 s the governor stalled ordinary long
matches on a lossy link.

## Resync

The two logs can differ by at most one ply, at the tail: the defender
persists ply k before it answers, the shooter only when the answer arrives.
So resync never transfers history. SYNC carries `(Rp, digest(1..Rp))`.

A SYNC may have spent a long time in flight, so a peer that looks *behind*
may only be speaking from the past: "behind" is never evidence of anything.
Only the digest of the plies both sides hold is.

| Comparison | Action |
| --- | --- |
| Rp ≤ R and digest(1..Rp) matches | in step as far as the peer knows. If Rp = R-1 and ply R was theirs, resend the latest packet (the RESULT, or the SHOT that carries it); if this side has a shot pending, the obligation resends it |
| Rp = R+1 and this side has a shot pending | they answered it; the answer is on its way |
| anything else | `END(void)`: "records differ", no winner |

"I do not hold your commit" in a SYNC is believed only before the first ply
has been played; after that the peer certainly holds it and the SYNC is old.

Also void: the local match file lost or unreadable (the own board is private
and nobody can rebuild it), the meshcored identity changed, a commit that
changed.

## Fairness

Prevented: seeing the opponent's fleet before the end (only a salted
commitment is sent); out-of-turn, repeated, off-grid or duplicate shots; one
shot counted twice; packets from another session or another node.

Detected: an answer that contradicts the committed fleet, found at REVEAL
("reports didn't match"); impossible reports during play (a ship sunk twice,
more than 17 hits, a sinking without enough hits, "destroyed" without five
sinkings or five sinkings without it, a whole board fired at without a win),
which end the match `END(violation)`; a refusal to reveal ("not verified").

Trusted in v1: that a key belongs to whoever adverted it; MeshCore's 2-byte
datagram MAC; the guest-first rule; the loser's own display of its loss.

## Persistence

`$POCKETOS_STATE_DIR/fleet/match.v1`, written only by `fleet_store.c`,
separate from `save.v1` and independent of it. **655 bytes**, fields
little-endian, FNV-1a trailer (`apps/fleet/net/fleet_match_save.c`). Written
to a temporary file, flushed, renamed, and then the directory flushed, so the
new name survives a power cut too; a failed directory flush is a failed write.

On load it is checked twice. The codec refuses a wrong size, magic, version,
checksum or field, and a file saved under another node identity.
`fleet_match_restore()` then refuses a state the protocol could not have
produced: our commitment must match our layout, every answer in the log must
be what our own fleet says, their answers must be possible, and a pending shot
must be ours to have fired. A match that fails either is not resumed; the file
is left where it is, as `save.v1` is, because it is the only evidence.

A finished match stays in the file, and on the Lobby, until the player puts
it away (CLOSE, or leaving Result), so a result is never lost to a closed app.

**Nothing is transmitted that is not already on disk.** The state machine
holds its outbox shut while it has unsaved changes; the app saves, then
releases it. A crash before the save: nothing was sent, nothing happened. A
crash after it: the packet is rebuilt from the saved state on reopen and the
peer takes it as a duplicate.

Held: role, phase, sid, both keys, peer name, own layout, salt and commit,
peer commit and the evidence bits, R and the log, the pending shot, the
peer's reveal and the verification, how it ended, four tombstones. Not
held: timers, try counts, the governor, the outbox; each is rebuilt.

## In the app

The session (`apps/fleet/link/fleet_session.c`) joins the match, a link and
`match.v1`, and keeps three rules: **nothing is pumped until the player
engages** - opens Multiplayer or presses Resume - so opening Fleet sends
nothing and does not even read the inbox; unsaved state is saved before a
packet is taken from the outbox; random bits come from `getrandom()`, never a
clock. The app drives it from a 100 ms LVGL timer (`apps/fleet/fleet_mp.c`)
and repaints only when the match's revision changes.

The screens are PocketFleet's own. One is new, the Lobby; Deploy, Battle and
Result read the match instead of the AI game when the mode is multiplayer.
The cell language, the layouts in both shapes, aim-then-confirm and the rule
that a Battle turn never scrolls are unchanged, and tests/fleet_app_test holds
multiplayer to them exactly as it holds single player.

| UX state | Where | What it says / offers |
| --- | --- | --- |
| Multiplayer menu | Command: a MULTIPLAYER panel under OPPONENT | "Play another Doors device over the mesh." MULTIPLAYER; or "An engagement with X is waiting." RESUME MATCH |
| Mesh unavailable | Lobby, ENGAGEMENT | "The mesh service is not running on this device, and multiplayer needs it." |
| Create game | Lobby | PLAYERS IN RANGE ("ANNA · DIRECT · HEARD JUST NOW"), MAKE VISIBLE (zero-hop advert), INVITE |
| Waiting for player | Lobby | "Inviting Anna…", "no answer yet (try 3 of 6)", CANCEL |
| Incoming invite | Lobby | "Anna invites you to an engagement." ACCEPT / DECLINE |
| Declined, busy, no answer, withdrawn, crossed | Lobby | one sentence each |
| Preparing game | Deploy (unchanged), then Battle | "Waiting for Anna to deploy." |
| Your turn | Battle | the aiming text, FIRE armed |
| Waiting for acknowledgement | Battle | "Shot at D3 sent. Waiting for the report." FIRE disarmed |
| Link problem / retrying | Battle | "Link problem: no report on D3 yet. Retrying (3 of 6)." |
| Airtime held back | Battle | "Holding back to spare the airtime. It will go shortly." |
| Resyncing | Battle | "Checking the match with Anna…" |
| Opponent turn | Battle | "Anna is aiming." then "No word for 7 min." |
| Opponent disconnected | Battle | "Anna is out of reach. The match is paused, not lost." FIRE becomes CHECK LINK |
| Forfeit | Lobby, match in hand | FORFEIT, then CONFIRM FORFEIT |
| Game over | Result | "Enemy fleet destroyed" / "Fleet lost" / "No result"; Ended ("ALL SHIPS SUNK", "THEY FORFEITED", "VOID · RECORDS DIFFER"); Their fleet ("Verified", "Not verified", "Reports did not match") |

The wording is `apps/fleet/ui/fleet_view_mp.c`, tested natively.
Screenshots: `docs/design/shots/fleet-mp-<state>[-landscape].png`.

**Development aid.** `POCKETFLEET_MP_FAKE=<spec>` replaces the mesh link with a
virtual opponent (`apps/fleet/link/fleet_link_loop.c`): a second match state
machine, played by PocketFleet's AI, over a channel that can lose, duplicate,
delay and be cut (`loss=20,dup=5,delay=800,invite=3000,think=2500,level=3,
decline,silent,cut=60000,seed=7`). With it, one simulator shell plays a whole
match against the real protocol. With `POCKETFLEET_SCREEN=lobby|mp_invited|
mp_deploy|mp_battle|mp_waiting|mp_lost|mp_result` the app drives the match
into that state on a skipped clock, for screenshots. Both are inert unless
set, and nothing either does reaches a radio.

## Airtime

Per frame, zero hops, the MeshCore profile: 22 bytes 304.1 ms, 38 bytes
386.0 ms. On a clean link a ply costs exactly one SHOT and one RESULT.
Measured by `tests/fleet_mp_sim_test` over 2000 matches per profile (the
players are PocketFleet's AI at 1.5-12 s a move; about 104 plies a match):

| Profile | frames per ply | airtime per device per match | worst hour |
| --- | --- | --- | --- |
| clean | 2.13 | 34.2 s (max 63.5) | 63.5 s |
| 10 % loss, collisions | 2.36 | 38.2 s (max 72.9) | 72.9 s |
| 30 % loss, 10 % duplicates, reordering | 3.27 | 52.7 s (max 103.1) | 90.0 s |
| 20 % loss, outages 30 s - 10 min | 2.90 | 46.9 s (max 95.0) | 85.8 s |
| crashes, app closed, service restarts | 2.50 | 40.8 s (max 82.6) | 74.9 s |
| all of the above, reboots too | 3.58 | 58.6 s (max 123.2) | 90.0 s |

"Frames per ply" includes setup and the reveals. These are simulated
figures on a modelled channel, not measurements: the real channel is P7's
to measure (docs/hardware/FLEET_MULTIPLAYER_GATE.md).

## Tests

| Test | Covers |
| --- | --- |
| `tests/fleet_sha256_test` | the NIST vectors, every split, the padding boundaries |
| `tests/fleet_proto_test` | every type round trip, every length, every bad field, a million random packets |
| `tests/fleet_match_test` | each session, turn, resync, fairness, END and save-codec rule by name |
| `tests/fleet_mp_sim_test` | two AI players over a simulated LoRa channel with loss, duplication, reordering, collisions, outages, crashes, app close/reopen, service restarts and reboots; invariants after every step, the perfect-network oracle, eight cheating peers. `make fleet-mp-soak` for 5000 matches a profile |
| `tests/fleet_session_test` | the session against the virtual opponent: nothing before engaging, whole matches clean and lossy, reopen and resume, a failed save, another identity's save, decline, silence |
| `tests/fleet_view_mp_test` | every UX state's words |
| `tests/fleet_save_test` (match.v1) | the file: whole, byte for byte, independent of save.v1, oversized refused, a write that cannot happen |
| `tests/fleet_app_test` (section 8) | whole multiplayer matches under a finger in both shapes, a turn across the page never scrolling, reopen and RESUME MATCH, the link lost and CHECK LINK |
| `tests/fleet_shell_test.sh` (section 10) | every multiplayer state rendered in both shapes in the shell, `match.v1` written whole, no mesh service, a damaged `match.v1` |
| `tests/fleet_lint.sh` | the layers: net pure, one file talks to a service, no radio method, nothing pumped before engaging, the virtual opponent only when asked for |
| `tests/fleet_link_test` | the mesh link against a real socket and a scripted meshcored: nothing asked before the first poll, the four link states, players listed (no repeaters, no forgotten nodes), each datagram taken once from the inbox and from events, the cursor reset on a new service run, stale and malformed datagrams refused, exactly the bytes handed over sent on port 1, a zero-hop advert, a radio that is off |
| `tests/meshcored_runtime_test.cpp`, `tests/meshcored_service_test.sh` (3d) | meshcored's half: app datagrams between two runtimes with real crypto, flood then direct once the receipt teaches the route, crafted REQs that are not app datagrams ignored, the inbox bounded; every refusal of `mesh.app_send` / `mesh.app_inbox` against the real process |
| `make ENABLE_MESHCORED=1 fleet-mp-e2e` | **P6.** Whole matches between two Fleet players (`tests/fleet_mp_player`: the real session, mesh link and match, the AI for a finger) through two real meshcored processes over stand-in radiods and a mock air: at the product's own pace; with 15 % of frames lost and 5 % duplicated; with the guest's app killed at ply 30 and resumed from its save; and with the guest's meshcored restarted at ply 40. Each must end with one win and one loss, both fleets verified, identical records, no violation, datagrams both ways, a receipt, and no chat message |
| `make fleet-mp-san-test` | the native suites under ASan and UBSan, `fleet_link_test` included |
