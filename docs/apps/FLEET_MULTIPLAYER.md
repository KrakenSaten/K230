# PocketFleet multiplayer: protocol v1

Two players, one match, turn based, over the MeshCore mesh. The decision and
its alternatives are docs/decisions/ADR-008-fleet-multiplayer.md (Accepted
2026-11-20).
Single-player Fleet is unchanged (docs/apps/POCKETFLEET.md).

Status: host-tested, and gated on hardware between units A and B over LoRa:
docs/hardware/FLEET_MULTIPLAYER_GATE.md (P7 minimum scope, PASS 2026-09-26),
FLEET_CHAT_STATUS_GATE.md and FLEET_MP_AUTO_GATE.md (PASS 2026-09-30). **Not
checked:** a non-Doors MeshCore node, range, a repeater, a busy channel, the
airtime of a full-length match, an unattended match at real pace.

## Layers

```text
apps/fleet/net/      pure C: SHA-256, the packet codec, the match state
                     machine, the match save codec. No LVGL, no IPC, no
                     filesystem (tests/fleet_lint.sh).
apps/fleet/link/     the transport seam: fleet_link.h, and fleet_link_mesh.c,
                     the one file that speaks mesh.app_* over pocketipc.
                     fleet_link_peer.c is its sibling for the local network
                     (peer.app_* to peerd), opened when the player picks
                     LOCAL NETWORK - see "The local network link" and "The
                     local network in the app".
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
(zero hops; each hop adds a path byte and a retransmission). Every packet
of the game is one or the other. Only a line of chat can need a third
block: up to **42 bytes**, a **54-byte frame, 468.0 ms** ("Chat").

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
| 13 | CHAT | either | the line: 1 to 37 bytes of UTF-8, no control character; the line's id in the ply byte | 6 to 42 | CHAT_ACK |
| 14 | CHAT_ACK | either | check(2): 16 bits of FNV-1a over id ‖ text; the id in the ply byte | 7 | – |

There is no TURN_ACK and no GAME_OVER: RESULT acknowledges SHOT, the next
SHOT carries and acknowledges the RESULT before it (`prev`), REVEAL
acknowledges the final RESULT, and the final RESULT carries "fleet
destroyed". A forfeit is `END(forfeit)`.

SYNC phase codes: 0 none, 1 before start, 2 deploying, 3 committed,
4 battle, 5 reveal, 6 done.

A packet is **dropped** unless its length is exactly right for its type, its
version is 1, its sid is non-zero, every enum and cell is in range, and its
sender is the session's peer (INVITE excepted). CHAT is the one type whose
length is its text's; its text is checked like any field (well-formed UTF-8,
no overlong form or surrogate, no C0, DEL or C1 control).

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

## Chat

Short lines between the two players of the match in hand, and nothing else:
no chat outside a match, no history kept after it, nothing saved. It rides
the same session as the game - same port, same sid, same peer check - so a
line from anyone but the opponent, or from another session, is never shown.
The code is `apps/fleet/net/fleet_chat.c` (the bookkeeping) and
`fleet_match.c` ("chat": when a line may go).

**Open** from Deploy until the match is put away, except after an END for a
forfeit, void or abandon: then the peer answers anything but the ending with
END, and a line would only cost airtime. Lines still waiting when chat closes
are given up.

**A line** is 1 to **37 bytes** - what fills a third AES block after the
header - of UTF-8 with no control character, spaces at either end dropped. The
fonts cover Latin-1, so Norwegian letters are text; a character the fonts do
not have still travels and shows as a box. The field counts characters and
the screen says when an accented line runs over the bytes.

**Ids and receipts.** Each line carries an 8-bit id, counting on from where
the last session left it (and, in a new run of the app, from a number drawn
from the match's seed), and each is confirmed by a CHAT_ACK naming its id and
its 16-bit check. A receipt that names a line only by number could confirm the
wrong one after a restart; with the check it cannot.

**Once, and once only.** The receiver remembers the id and check of the
peer's last 16 lines. A copy - a retry after a lost receipt, a duplicate on
the mesh, a line that arrives late behind a newer one - is answered again (at
most once per 2 s) and never shown again. A new run of the peer's app may
reuse an id, but not with the same words, so a new line is never taken for a
copy unless it says exactly what the copy said.

**One on the air at a time.** Ours go in order, each waiting for the one
before it to be confirmed or given up: tried three times, on the same backoff
as the game's packets (7 s, doubling), then shown as NOT DELIVERED. A line
never makes the link "lost": only the game's own obligations do. While the
opponent is out of reach, resynchronising or the governor is holding the game
back, a waiting line costs no try.

**Second to the game.** A line goes only when the game owes nothing and is
owed nothing - no obligation in force (so never while a shot waits for its
answer, which could arrive while a line was on the air), no resync, no packet
of the game's in the outbox - and it never pushes a packet of the game's out
of the outbox. Against the airtime governor a line is held to stricter terms
than anything of the game's: never the last **2 tokens** of the burst; only
while Fleet's airtime in the rolling hour is under **50 s**; and never once
chat has used **20 s** of that hour. A receipt is held to the whole 90 s
only, as the game's first answers are: the line it confirms is already on the
other screen, and withholding it would make the sender try again and then
call the line not delivered. There is at most one receipt per line.

**Bounded.** 16 lines kept (theirs and ours; the oldest settled line goes
first, never one still owed a receipt), 4 of ours waiting at most (a fifth is
refused and the screen says why), 16 remembered for copies, one line on the
air. Nothing grows with the length of a match.

**Old builds.** A peer without chat refuses both types as unknown and
answers nothing: the sender's lines end NOT DELIVERED, and the match is
untouched. A type newer than this build is refused the same way.

## Persistence

`$POCKETOS_STATE_DIR/fleet/match.v1`, written only by `fleet_store.c`,
separate from `save.v1` and independent of it. **655 bytes**, fields
little-endian, FNV-1a trailer (`apps/fleet/net/fleet_match_save.c`). Written
to a temporary file, flushed, renamed, and then the directory flushed, so the
new name survives a power cut too; a failed directory flush is a failed write.
The file is always mode 0600, whatever the umask of the process that writes
it (the temporary file is created 0600 and `fchmod`ed): it holds our own fleet
and its salt, which stay secret until the reveal.

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
held: timers, try counts, the governor, the outbox; each is rebuilt. Nor the
chat: a reopened app shows none, and a line that was waiting as it closed
is never sent. `match.v1` is unchanged by chat, still 655 bytes.

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
| Multiplayer menu | Command: a MULTIPLAYER panel under OPPONENT | "Play another Doors device over the mesh." MULTIPLAYER; or "An engagement with X is waiting." RESUME MATCH. Either opens the Lobby; with a match in hand it says "An engagement with X is under way." and offers RESUME (back to the board) and FORFEIT |
| Mesh unavailable | Lobby, ENGAGEMENT | "The mesh service is not running on this device, and multiplayer needs it." |
| Create game | Lobby | PLAYERS IN RANGE ("ANNA · DIRECT · HEARD JUST NOW"), MAKE VISIBLE (zero-hop advert), INVITE. The list is most recently heard first and re-sorts as nodes are heard; the choice is the player, not the row, so the light and INVITE follow that player, and a player no longer listed is no longer chosen |
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
| Game over | Result | "Enemy fleet destroyed" / "Fleet lost" / "No result", or "Opponent forfeited" / "You forfeited" when it ended by forfeit (a forfeit sinks nothing); Ended ("ALL SHIPS SUNK", "THEY FORFEITED", "VOID · RECORDS DIFFER"); Their fleet ("Verified", "Not verified", "Reports did not match") |
| Status | the header, every multiplayer screen | one of: CONNECTING, MESH OFFLINE, DEPLOY YOUR FLEET, SYNCING, RECONNECTING, OPPONENT DISCONNECTED, YOUR TURN · SHOT 12, WAITING FOR ANNA, GAME OVER - read afresh from the match and the link each time (`fleet_view_mp_state`), so it cannot disagree with them. MULTIPLAYER in the lobby with no match |
| Chat | Battle: CHAT beside FIRE, on FIRE's row; Result: CHAT in the foot | the button says "CHAT" or "CHAT · 2 NEW" over the newest line ("ANNA · Nice shot"); it opens the chat |
| Chat screen | its own screen, BOARD back | the status first, then the lines (oldest at the top, the list scrolling inside its panel), then the field, SEND and, in a body wider than tall, KEYS. Ours read "YOU · Hello", with " · WAITING", " · SENDING" or " · NOT DELIVERED" while it matters. Enter sends. A finger on the field brings the touch keyboard up down the page; across it, KEYS does, and the physical keyboard types without it. BOARD goes to the screen the match's phase belongs to now |

The status is the header's hint, where Fleet has always put the turn: Fleet
is fullscreen (DS §30.4), the header is on every screen in both shapes, and a
Battle turn has no height to spare for a line of its own. The chat button
costs Battle nothing either: it shares FIRE's row, so the never-scroll rule
and the tall layout are unchanged, and in single player it is hidden and FIRE
is the whole row as before. The chat field joins the focus group only while
the chat is on the screen, so keys typed on Battle go nowhere, as they always
did.

The wording is `apps/fleet/ui/fleet_view_mp.c`, tested natively.
Screenshots: `docs/design/shots/fleet-mp-<state>[-landscape].png`.

**Development aid.** `POCKETFLEET_MP_FAKE=<spec>` replaces the mesh link with a
virtual opponent (`apps/fleet/link/fleet_link_loop.c`): a second match state
machine, played by PocketFleet's AI, over a channel that can lose, duplicate,
delay and be cut (`loss=20,dup=5,delay=800,invite=3000,think=2500,level=3,
decline,silent,cut=60000,chat,crowd=3,seed=7`; `chat` has it answer every line
"Copy that."; `crowd` lists that many silent bystanders beside it, for the
lobby). With it, one simulator shell plays a whole match against the real
protocol. With `POCKETFLEET_SCREEN=lobby|mp_invited|
mp_deploy|mp_battle|mp_waiting|mp_lost|mp_result|mp_chat` the app drives the match
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

Over the whole stack (P6, `make ENABLE_MESHCORED=1 fleet-mp-e2e`: two real
meshcored processes, a mock air, `tests/fleet_mp_player` at Officer level), one
run each, 2026-09-25:

| Scenario | plies | wall time | airtime host / guest | frames host / guest |
| --- | --- | --- | --- | --- |
| real pace, clean air | 133 | 550 s | 42.7 s / 42.0 s | 139 / 137 |
| 15 % lost, 5 % duplicated (governor bucket refilled) | 142 | 276 s | 51.0 s / 48.5 s | 166 / 158 |
| guest's app killed at ply 30, resumed from its save | 74 | 13 s | 25.1 s / 14.9 s (guest's since restart) | 81 / 48 |
| guest's meshcored restarted at ply 40 | 132 | 30 s | 42.7 s / 42.1 s | 139 / 137 |

At the product's pace a match of about 130 plies takes about nine minutes:
after the first six quick shots the governor lets each player fire once per
5 s. The mock air delivers instantly, so the wall time is the governor's and
the dispatcher's, not the channel's; the airtime is computed, not measured.

"Frames per ply" includes setup and the reveals. These are simulated
figures on a modelled channel, not measurements: the real channel is P7's
to measure (docs/hardware/FLEET_MULTIPLAYER_GATE.md).

**With chat** (2026-09-30). A line costs its sender one frame of 305 ms (up
to 5 bytes), 387 ms (up to 21) or 468 ms (up to 37), and its receiver one
305 ms receipt. `tests/fleet_mp_sim_test`, 300 matches a profile, both
players talking all through - a line every 3 to 18 s whenever they have
room, far more than people do - against the same faults in silence:

| Profile | lines shown / match | airtime / device / match | worst hour | mean match |
| --- | --- | --- | --- | --- |
| 10 % loss, collisions, silent | – | 38.6 s | 70.5 s | 16 min |
| the same, talking | 40.5 | 59.2 s | 90.0 s | 18 min |
| all faults at once, silent | – | 59.2 s | 80.6 s | 54 min |
| the same, talking | 24.7 | 105.9 s | 90.6 s | 58 min |

(The talking profiles run other seeds than the silent ones, so compare the
means, not single matches. 90.6 s is a minute that straddles an app restart,
which the governor cannot see across; I5 allows for it.)

Every match still finished, verified, and was shot for shot the match the
same seeds play in silence on a perfect network: chat changes when things
happen, never what happens. On a clean link it costs the game nothing. On a
lossy one, constant talk makes a match 7 to 12 % longer on average (16 to
18 min, 54 to 58), because a match that already fills Fleet's 90 s hour then
shares it: lines are capped at 20 s of the hour and stop once it passes
50 s, but their receipts are not, so chat can hold roughly 30 s of an hour
at the very most. That is the price of one airtime budget for both, and the
limits (`FLEET_CHAT_*` in `fleet_match.h`) are the knobs if the owner wants
it smaller. In `tests/fleet_match_test`,
on a clean link, both sides saying a 30-odd byte line every 2 s: the same 106
plies in the same 646 s as in silence, still exactly one SHOT and one RESULT
per ply, 24 and 23 lines shown, each costing exactly one frame. Over the
whole stack (P6 "chat", two real meshcored processes, real MeshCore
encryption, three-block datagrams): 10 lines each way, every one delivered
at the first try, once and in order, in a 110-ply match of 21 s.

## Link profiles and a save per transport

docs/decisions/ADR-018-fleet-link-profile-and-saves.md (Accepted). What is
built so far is the part inside Fleet; no second transport exists yet, so
nothing on a device changes.

**Profiles.** The retry and governor figures above are no longer read as
constants: the match reads them from a link profile
(`apps/fleet/net/fleet_profile.h`), which the session takes from its link
(`fleet_link_ops.profile`). `fleet_profile_mesh` holds exactly the figures
of "Obligations and retries" and "Chat", and a match starts on it; the mesh
link reports it. `fleet_profile_lan` is for a local network: link waits from
0.5 s doubling to 5 s, a copy answered again after 200 ms rather than 2 s
(so the first retry brings a lost answer: 0.6 s in the test, against 9 s on
the mesh), no frame rationing and no hourly budget (starting values). Probe intervals, try counts and "paused, never forfeited" are the
same on both.

**Waiting for a person.** How long an INVITE or an ACCEPT waits for a
person is the same on every link: what the mesh's schedule comes to, six
waits from 7 s doubling to 60 s (4 min 19 s with the test's seed, the same
deadline on both profiles). What differs is how often the packet is sent
in that time. On the mesh nothing has changed: it goes out on that schedule,
six times, and the lobby says "no answer yet (try 3 of 6)". On a link whose
own waits are shorter the packet is repeated at the link's pace - 0.5 s
doubling to 5 s on the LAN profile, some 45 copies in the window - and
the wait ends at the deadline rather than after a count
(`fleet_match_human_wait`). There the lobby says "no answer yet (about 3 min
left)", to the nearest minute, from the moment the mesh would first have
said anything (7 s), and
the header does not call it a link problem before then. Three lost
invitations cost about 4 s on the LAN and about a minute on the mesh.

Waits for the link - SHOT, COMMIT, REVEAL, SYNC, END - follow the profile
and are counted in tries on both: an unanswered shot is given up as a lost
link after about 4 min 40 s on the mesh and about 21 s on the LAN profile.

**Saves.** `fleet_store` keeps one file per transport, same codec:
`match.v1` for LoRa, unchanged in name, size and format, and `match-lan.v1`
for the local network (`fleet_store_match_*_for`). The transport is chosen
by file before anything is decoded. The un-suffixed store functions are the
mesh's and behave as before.

**One match per device.** `fleet_match_save_in_hand()` says whether a blob
is a match between Deploy and Reveal, whoever saved it. A session told that
the other transport holds one (`fleet_session_set_other_in_hand`) refuses to
invite or accept (`FLEET_SESSION_OTHER_MATCH`) and sends nothing. The app
tells its session so whenever it opens one ("The local network in the app"),
and shows the other transport disabled while either save holds a match.
Should the session refuse all the same, the lobby says so: "An engagement is
already under way on LoRa. Finish it or forfeit it first."


## The local network link

`apps/fleet/link/fleet_link_peer.c` (M2 package 4) is the second link behind
`fleet_link.h`: peerd's app datagrams (`docs/api/peer.md`, ADR-017) on port 1,
the same number as on the mesh. The app opens it when the player picks
LOCAL NETWORK ("The local network in the app", M2 package 5). It has not run
on a device.

It is `fleet_link_mesh.c`'s shape - one non-blocking pocketipc connection,
requests matched by id, every datagram taken once by peerd's per-run id,
the cursor starting again when `peer.status` shows another `run_id` - and it
never opens a socket to the network or touches a key: peerd does both. Its
profile is `fleet_profile_lan`. What it adds, all of it only while polled,
which the session does only once the player has engaged multiplayer:

- **Visibility.** peerd is silent until asked. While its owner wants the
  device seen (`fleet_link_peer_set_visible`; a link does unless told
  otherwise) the link asks to be visible when it connects, renews peerd's
  30 s lease every 10 s, and renews at once when the player presses the
  button that is "advertise" on the mesh. Told otherwise it ends the lease
  and does not ask again - once the opponent of a match just begun has been
  kept, so that peerd is never hidden while keeping nobody - and closing ends
  it too. A lease that was never asked for is never ended: nothing is said.
  With a match in hand peerd stays bound for the kept opponent and nobody
  else, lease or no lease. The app wants the device seen while the lobby is
  on screen and while an invitation of the player's own is waiting for its
  answer ("The local network in the app").
- **The opponent.** `fleet_link_ops.match_peer` is a new, optional op (NULL
  on the mesh and the virtual opponent). Once the session has read its save
  it tells the link, on every poll, who the match in hand is with - from
  Deploy until the match is done, no END is owed any more (acknowledged, or
  given up), and the opponent has been quiet for thirty seconds on the
  network (the profile's six tries at its longest wait of five seconds;
  their tries, with jitter, come at most 7.5 s apart) - or that there is
  none. That last wait is because a match over on this side may not be over
  on the other: the packet that would finish it there can be the one that
  was lost, and the opponent then sends its last packet again and needs an
  answer. This side answers from DONE, but only while peerd still listens
  for that opponent; released at once, it left the opponent retrying into a
  closed socket (one match in ten at 15 % loss, before this was added) until
  it gave up and settled by itself, about half an hour later.
  Every packet heard from them starts the wait again. Putting the match
  away (CLOSE) ends it at once. So does leaving: when Fleet closes, the
  other transport is picked, or the service is looked for afresh, the app
  calls `fleet_session_close_retention()` before it closes the link, and an
  opponent held only for this wait is let go there and then - nobody will
  be polling to answer them, and peerd holds a kept peer until told
  otherwise. It lets go of nothing else: the opponent of a match still in
  play, or of one over with its END still owed, stays kept, because that
  match is resumed from its save and its opponent must be reachable then.
  This is ADR-017 Amendment 1 (and M2-PLAN 1.3): half a minute after the
  match has ended here and the opponent has gone quiet, or when Fleet
  leaves the network's link, whichever comes first. peerd itself still has
  no timed expiry. When the save could not be read (a
  store fault, a damaged file, another identity's) it says nothing until a
  match begins: what the save held is not known. From then on it speaks as
  usual, the end of that match included. The link
  reads `peer.kept` and puts it right: the opponent is kept, so the match
  survives a restart with nobody visible, and **every other kept peer is
  released**. It reads the list again every 15 s and after any reconnect or
  new run, so a record peerd lost is asserted again. Nothing is kept or
  released before the session has said. A keep or release peerd refuses is
  tried again at a growing interval, 4 s up to 15 s, starting afresh on a
  new connection.
- **An address.** `fleet_link_peer_probe()` asks the device at a dotted IPv4
  address who it is (`peer.probe`), for a network where announcements do not
  get through; anything that is not such an address never becomes a request.
  One probe at a time, a new one replacing the last; its outcome is read
  with `fleet_link_peer_probe_state()`.
- **Proved.** `fleet_link_peer_proved()` says whether peerd has shown a
  listed player to be live. The seam's player list does not carry it, and
  ADR-017 forbids a lobby to present an unproved player as present: package
  5 must read it.
- **Why it is not up.** `fleet_link_peer_info()` gives this device's address,
  whether peerd says it is visible, how many peers it keeps, and the trouble:
  no service, a fault peerd names (the link then reports `RADIO_OFF`, the
  seam's word for "the service is there and unusable"), or no interface to
  announce on. The last leaves the link up: a device at a known address is
  still reached.
- **Whether peerd is there at all.** `fleet_link_peer_present()` looks for
  peerd's socket in the filesystem and does nothing else: no connection, no
  request.
- **Closing.** The request to hide is the last thing written before the
  socket is closed. A pocketipc server drops a connection, with whatever it
  has not yet handled, at the first answer it cannot deliver, so a request
  of ours still unanswered at that moment would take the hide down with it;
  the link therefore waits for peerd's outstanding answers first - until the
  socket has been quiet for 15 ms, 60 ms at most. It is the one wait in the
  link beyond pocketipc's own. If the hide is lost all the same, the lease
  ends the visibility within 30 s. One more thing can come before the hide:
  an opponent the session has stopped naming and peerd's list has not yet
  been seen without (the ordinary reconciling needs a poll, and a closing
  link gets none) is released by name, `peer.release`. Never the opponent
  the session still names. When a hide is to follow it, peerd's answer to
  the release is waited for first, 120 ms at most, for the same reason as
  above; peerd puts its list on disk before it answers. If the release is
  lost all the same, peerd keeps that opponent until Fleet is next in
  multiplayer on the network, where it is released after the same half
  minute.

To know about it:

- **The link assumes Fleet is peerd's only client.** Releasing every kept
  peer that is not this match's opponent is right while that holds, and
  ADR-017 asks for exactly that reconciliation. A second game on peerd would
  need kept peers to have an owner first; that is the lobby's design (M3),
  not this link's.
- **A send is "sent" once handed to peerd.** If peerd then refuses it - no
  address for that node yet - the link counts it and the match retries on
  its own timer, as for a lost packet. The mesh link behaves the same.
- **`hops` is 0 or -1.** 0 for a node peerd has an address for, -1 for one
  it does not (a kept opponent after a restart, until it is heard).
- **The opponent is released half a minute after the match is done and they
  have gone quiet, or at once when Fleet leaves** (above). A player who
  closes Fleet, or switches to LoRa, inside that half minute cuts the wait
  short: an opponent whose last packet was lost and who is still asking is
  then not answered, and settles by itself as below. That is the price of
  not leaving peerd bound for an opponent nobody is there to answer.
- **A match that is not finished keeps its opponent across a close**, for
  as long as its save exists: in play, or over with an END nobody has
  answered or given up. peerd stays bound for that one authenticated
  opponent until the match is resumed and ended. That is ADR-017's kept
  peer, and is what lets a match go on after both devices have been off.
- **Left as it is:** if all of the opponent's tries are lost, or the player
  presses CLOSE and leaves the lobby in the first seconds after a match
  whose last packet was lost, the opponent is no longer heard; it settles by
  itself, with the same record, after its own probes run out.
- **A peerd that is connected and silent leaves the link up**, as a silent
  meshcored does the mesh link: there is no reply timeout in either.
- Not tested: a match whose END is still owed (the rule is in the session;
  no test reaches that state through a link).

## The local network in the app

M2 package 5. Nothing here has run on a device, and no image contains peerd:
on every device there is today, Fleet is exactly what it was.

**When it is offered.** The local network is offered where peerd's socket
exists when Fleet opens (`fleet_link_peer_present`), and always where
`match-lan.v1` holds a match in hand (between Deploy and Reveal, and not
another identity's), service or no service, so that the match can be
reached. A network save that holds only a finished match does not offer the
network by itself. Elsewhere `MULTIPLAYER` is one button
with the words it always had, and nothing about the network is shown or
asked. When this was written, screenshots of Command, Deploy, Battle,
Result and the LoRa lobby from this build and from the one before were
compared and were the same files, byte for byte, in both shapes and in the
two themes compared, Carbon (the default) and Neon. No test repeats that
comparison, since it needs the older build.

**One session, one transport at a time** (`apps/fleet/fleet_mp.c`). The
session is on LoRa unless the network's save holds the match in hand; the
file decides, before a byte is decoded. It moves to the other transport only
when the player picks it and a choice is on offer: the device can do both,
neither save holds a match, and no invitation is open. Moving closes the
link it leaves - on the network that ends this device's visibility - and
opens the other with its own save; nothing is carried over. A transport
becomes a save file in one function, and the virtual opponent's profile
follows the transport, never its own spec.

**Command.** The `MULTIPLAYER` panel ends in one row. On a device with both
transports it always holds two buttons at the height the one button had,
LoRa's first: `LORA` and `LOCAL NETWORK`, with "Play another Doors device
over LoRa or the local network." With a match in hand on one of them
(M2-PLAN 1.1), that transport's button is `RESUME MATCH`, the other stands
disabled, and the panel says why: "An engagement with ANNA is under way on
LoRa." With an invitation open on one, the other stands disabled and the
panel says "An invitation is open on LoRa." A disabled button does nothing.
On a device with LoRa alone it is one button, `MULTIPLAYER` or `RESUME
MATCH`, with the words it always had.

**Visibility.** The device is announced on the network, and hears strangers,
while the network's lobby is on screen (ADR-008: while its screen is open) -
and while an invitation the player sent, or one they accepted, is waiting
for its answer, wherever they are. Leaving the lobby otherwise - to Command,
into Deploy or Battle, or by closing Fleet - ends it; coming back asks again
at once. The player's own invitation is the one exception because until a
match begins nobody is kept, and a peerd that is hidden and keeps nobody
drops the packets that would begin it: an invitation would die, or be
accepted on one side only, when either player looked at another screen. It
ends when the invitation is answered or cancelled, and runs out by itself
after the match's waiting window. An invitation merely received keeps
nobody visible: anyone on the network can send one, and answering it means
coming back to the lobby. A match in hand needs no
visibility: peerd keeps exchanging datagrams with the kept opponent, and
with nobody else. `ADD BY ADDRESS` is the lobby's, so it always has it. One
function decides (`follow_visibility` in `fleet_mp.c`).

**A service that does not answer.** A match in hand is kept whatever happens
to its service: its save is not touched, and the other transport stays
disabled, so no second match can be begun beside it. Opening the lobby then
says so - "Your engagement with ANNA is kept. It goes on when the local
network service is running again, and nothing else can be started until
then. Fleet keeps trying; TRY AGAIN looks now." - under the link line that
says the service is not running or names its fault. The link tries again by
itself every few seconds; `TRY AGAIN` opens the session afresh and looks at
once. Once the service answers the lobby offers `RESUME` and `FORFEIT` as
ever. This is on a device with both transports; with LoRa alone the lobby
is what it was. The way out of a match that can never be resumed because
its service is gone for good is to restore the service or remove the save
by hand: Fleet does not discard a match on its own.

**The lobby** is the same screen. On the local network:

- the link line is the network's: "Local network ready. This device is
  192.168.1.42.", looking for the service, the service not running, a fault
  peerd names, or "Wi-Fi is off and no cable is connected." The header says
  `NETWORK OFFLINE` where LoRa says `MESH OFFLINE`;
- a row has no hops: "ANNA · HEARD JUST NOW". A player peerd has only heard
  announced is listed as "ANNA · UNCONFIRMED · HEARD JUST NOW" (ADR-017
  section 5: not presented as present) - the marker straight after the name,
  where a row too long for its column is never cut - and can be invited: an
  answer is what confirms them;
- where LoRa has `MAKE VISIBLE` there is `ADD BY ADDRESS`, which shows a
  field for a dotted IPv4 address and `ASK`. The field takes digits and dots
  only. Enter or `ASK` hands the question to peerd; the lobby then says
  "Asking ...", and either who answered - who is chosen as soon as peerd
  lists them, so `INVITE` is the next press, unless the player has chosen
  somebody themselves meanwhile - or "No answer from 192.168.1.42. This
  network may keep devices apart. Try another network, a cable, or LoRa.",
  with nothing spinning and nothing retried. A good address that could not
  be asked just then is said to be that, not blamed. What was asked is
  forgotten when the lobby is left, unless the question is still out: its
  outcome is then shown on the next visit;
- with nobody listed it says it is listening, and after six seconds "No
  players heard on this network. Enter the other device's address: this one
  is 192.168.1.42."

The words are `fleet_view_mp_lan_*` and `fleet_view_mp_command`, tested
natively.

**One match per device.** While either save holds a match between Deploy and
Reveal the other transport stands disabled, and the session is told
(`fleet_session_set_other_in_hand`) so that it would refuse to begin one in
any case. A finished match waiting to be put away does not block the other
transport (ADR-018 point 4), one whose END is still owed included: its save
keeps it, and the END is sent again when the player comes back to that
transport.

**Another identity's save.** A save made under an identity its transport no
longer has can never be resumed. On a device with both transports that is
remembered once the link has said who this device is: a small file beside
the save, `match.v1.stale` or `match-lan.v1.stale`
(`fleet_store_match_stale`), holding the identity the save was written
under. It counts only while the save still carries that identity, so a save
written anew - by this build, by one that knows nothing of marks, or in a
run where the network was not on offer - is never taken for the dead one,
whether or not the mark was removed. Fleet removes it when it writes that
transport's save anew, or when the identity turns out to be this device's
after all (which is learnt only when that transport is next engaged; until
then the save is treated as dead). A marked save does not count as a match
in hand - it does not choose the transport when Fleet opens and holds
neither transport back - and the save itself is left in place, as `match.v1`
always was. Whenever the other transport's save holds a match and this one
has nothing in hand, Command's `RESUME MATCH` leads there. A device with
LoRa alone writes no such file and behaves as it always did. ADR-018 does
not describe the marks; they are an addition of this package.

Known limits of this package:

- Words written for LoRa can still be read on the network in a match: "out of
  reach" and, never in practice, "to spare the airtime".
- **Ending visibility is a request.** If it does not get through - a keep
  that keeps failing, a peerd that has stopped reading - the lease is simply
  not renewed and peerd ends it within 30 s.
- **The touch keyboard over the address field has not been seen on a
  device.** The field is in a column that scrolls and is brought into view
  when focused; on the keyboard base nothing covers it.
- **No table codes**, no QR, no invitation outside Fleet: those are the
  lobby's (M3) and the shell's (M4).
- A match in hand whose opponent's address is lost is reached again by peerd
  (its last address, then a lookup). If that fails too, `ADD BY ADDRESS`
  from the lobby finds them; nothing steers the player there.

## End to end over the local network

M2 package 6: `make ENABLE_PEERD=1 SODIUM_DIR=... fleet-lan-e2e`. Not part of
`make test`: it needs libsodium and a host that allows unprivileged user
namespaces (it fails, rather than skips, where it cannot run). Nothing here
has run on a device.

What it adds to the tests before it is the real thing on both sides of the
socket and of the wire: the peerd is `services/peerd/peerd` as it would be
shipped (the test refuses a build with test hooks), it enumerates a real
interface, announces on its broadcast address and binds every address, and
the two of them are joined by a veth pair - or, for the network that carries
no announcements, by a third namespace that routes between two subnets. The
players stand in for the app: `tests/fleet_lan_player` is seen on the network
by the app's own rule, so every match is played hidden, with the kept
opponent alone.

What the scenarios prove, each by something that fails when it is not so:

- **Found by announcement** ("clean"): the host lists the guest within six
  seconds without asking any address (twelve under loss, where beacons are
  lost too).
- **A network that carries no announcements** ("blocked"): two subnets and a
  router. After five seconds with both lobbies open and announcing - each
  with an interface and beacons sent on it - neither peerd has heard the
  other; the host listens for seven, has nobody listed,
  asks the guest's address, and is answered. Run on one subnet instead, the
  same checks fail.
- **A killed peerd comes back with its kept opponent** ("restart"): by
  peerd's own log line at its second start ("1 kept peer(s)"), written
  before any client could have asked for anything, and the match goes on.
- **An address change needs nothing from the player.** In "address" the
  match simply goes on: the host's peerd follows the guest's next packet, or
  asks, whichever comes first. In "lookup" the guest's app is stopped before
  its address changes, so nothing it sends can say where it went: the host's
  peerd finds it at the new address while the app is still stopped, and has
  accepted a signed answer from a device that announces nothing - which is
  peerd's "who holds this key" being asked and answered.
- **A short outage** ("outage"): eight seconds with the guest's interface
  down - less than the match's retry budget, so nothing is given up and no
  probe or lookup is needed - the match standing still, and going on when it
  is back. A longer one, where the match goes "out of reach" and recovers by
  probing, is not covered here.
- **An app killed mid-match** ("crash", on the host's turn, so the host has
  something to send): its peerd still keeps the opponent
  with no client attached, what the host sends after the crash waits in
  peerd's inbox, and the app started again on its save resumes that match.
- **An app closed mid-match** ("closemid"): the player closes its link as the
  app's close does; peerd still keeps the opponent of the unfinished match
  and stays bound, and the app opened again resumes and finishes it.
- **A forfeit under loss**: the END is answered, not given up.
- **The last packet of a match lost** ("lastpacket"): every packet is held
  400 ms on its way out, and the instant the first player is done its
  interface goes down for half a second, throwing away the packet the other
  needs to be done. The other still finishes, seconds later, by asking again
  and being answered by a side that is already done. With the half minute
  a finished match keeps its opponent taken out, it does not. When the two
  reveals cross on the wire, or both are done within one look, no such
  packet exists: the scenario says so and is played again, three times at
  most.
- **An END nobody answers** ("endlost"): the host forfeits and its interface
  is down for eight seconds from that moment, so its END is given up; the
  guest, checked not to be done while the host was cut off, still learns of
  the forfeit afterwards, from the host still answering. With the
  half minute taken out, it does not.
- **Let go when the app closes** ("clean"): both keep the opponent after the
  match; the host's app closed, its peerd keeps nobody and closes its socket
  within seconds, while the guest's, still open, lets go by itself later
  (checked: not before 15 s; the product's wait is thirty quiet seconds).
- **Loss is real** ("lossy", "forfeit"): netem reports packets dropped.
- **Hidden throughout**: neither device is visible again from the start of
  the match to its end, through each of the faults above.

Two things measured, for the hardware gate to weigh; neither is changed by
this package:

- **peerd rewrites its kept record about once per ply.** A match of 100
  plies costs each peerd some 105 writes of `kept.v1` (its counter
  `kept_writes`), each a temporary file, a flush and a rename: ADR-017's
  replay floor being put on disk before a kept peer's datagram is delivered,
  at most one write per 250 ms. On a device that is as many flash writes per
  match, and a datagram that arrives just after a write waits for the next.
- **More traffic than the game asks for when players talk.** With both
  players talking, about 250 packets cross each way where a silent match of
  the same length sends about 110, and some 75 of them on each side are
  answers to copies of a shot already answered; lines and their receipts
  account for about 45. Which timer is responsible was not established: the
  match re-sends a shot after 500 ms (the LAN profile), and peerd may hold a
  kept peer's datagram for up to that long while its floor is written, so
  the two are close enough to race, and more datagrams make it likelier.
  That is a reading of the code, not a measurement. The matches are correct
  and nothing is rationed on the network.

Not covered here, and left to the hardware gate: two real devices; Wi-Fi and
Ethernet; a DHCP lease or a cable replug; client isolation on a real access
point (a probe that gets no answer is tested against the scripted peerd
only); rotation; a reboot straight after FIRE; Fleet closed and reopened by
its player through the app's own screens (the player here closes its link
the same way); anything on a panel. Not covered at all: an
invitation while one player is off the lobby, on two devices (the players
here stay "in the lobby" until the match begins); an address lost on the
network that carries no announcements; an outage longer than the retry
budget; a fault on the host's side other than its forfeit's END ("endlost")
and its interface after the last packet ("lastpacket"); an END that is lost
once and then answered, other than by chance in "forfeit".

## Tests

| Test | Covers |
| --- | --- |
| `tests/fleet_sha256_test` | the NIST vectors, every split, the padding boundaries |
| `tests/fleet_proto_test` | every type round trip, every length, every bad field, a million random packets; CHAT at 37 bytes and not 38, empty, every kind of bad UTF-8 and control character, the receipt's exact length, a type newer than the build |
| `tests/fleet_match_test` | each session, turn, resync, fairness, END and save-codec rule by name; chat: sent, received from the opponent only, a copy shown once, a lost receipt, a line given up after three tries without the link called lost, reordered, the history and the waiting lines bounded under a flood, the limits, malformed and unknown types, a line of an old session never shown in a new one, a shot always before a line, the token and hour reserves, lines given up when a forfeit ends the match, a word after a finished match, and a whole match with both sides talking played shot for shot as in silence |
| `tests/fleet_mp_sim_test` | two AI players over a simulated LoRa channel with loss, duplication, reordering, collisions, outages, crashes, app close/reopen, service restarts and reboots; invariants after every step, the perfect-network oracle, eight cheating peers; and two profiles with both players talking throughout (no line shown twice, none from another session, the oracle unchanged). `make fleet-mp-soak` for 5000 matches a profile |
| `tests/fleet_session_test` | the session against the virtual opponent: nothing before engaging, whole matches clean and lossy, reopen and resume, a failed save, another identity's save, decline, silence; chat through the session with no save, the opponent out of reach while typing and after, the app closed with a line waiting and reopened twenty times, a new match with an empty chat on both sides |
| `tests/fleet_view_mp_test` (local network) | the header on each transport; the link line in each state, never naming the mesh or a radio; listening, the sentence after six seconds, asking, an answer, no answer and what is not an address, each to the word; a row with no hops, unconfirmed, not heard yet; Command's sentence on a device with and without the network, word for word what it was without |
| `tests/fleet_app_test` (section 10, the owner's four decisions) | the other transport shown disabled beside RESUME MATCH and beside an open invitation, the panel's sentence to the word, a disabled button doing nothing; visibility ended on leaving the lobby for Command, not asked for again while it is closed with the link still up, asked at once on coming back, kept while the player's own invitation is open until it is cancelled, and not kept by an invitation merely received; a match in hand with no peerd: still offered, LoRa disabled, the lobby's words and TRY AGAIN, the save byte for byte what it was |
| `tests/fleet_link_peer_test` (visibility) | a link told not to be seen never asking for the lease, a packet still going, the lease asked at once when wanted and ended when not - after the opponent of a match just begun is kept - and left to lapse when the keep cannot succeed |
| `tests/fleet_app_test` (section 10) | a device without the network: one button, the old words, the button filling its row; the choice on one row, both buttons in view across the page, neither clipped; LoRa and the network each opening its own session and profile; a match on the network saved in `match-lan.v1` only, and one on LoRa in `match.v1` only; reopened on the transport whose save holds the match, the other not offered; another identity's save holding neither transport back, remembered beside the save, gone as a match to resume once a new one is written over it, its mark not taken for a later save's, a finished match not holding the other transport back, the session's refusal said in words, and - with a live match on the other transport - RESUME MATCH leading to the live one, in the same run and after a restart; LoRa alone leaving no mark; and, against the real link and a scripted peerd: nothing asked before LOCAL NETWORK is pressed, this device's address, proved and unconfirmed rows, an address typed on keys (letters refused) and answered, the answerer lit with INVITE ready (also when it was not listed before it answered), an old answer gone on coming back to the lobby, an address with no answer, the touch keyboard only for a finger, the field out of the keys' way off the lobby, peerd going away under an open lobby, six seconds of silence, a peerd closed by a fault, visibility ended on close |
| `tests/fleet_shell_test.sh` (section 11) | against `tests/fake-peerd` in the running shell: Command with both transports, the lobby with players, an address answered and unanswered, and silence, in both shapes with no fault; nothing asked of peerd by opening Fleet; visibility asked for by the lobby and ended on exit; no match file written; and no peerd, no offer |
| `tests/fleet_view_mp_test` | every UX state's words; the status for each state and in its order of precedence, a long or accented name cut whole; the chat button and every line's wording |
| `tests/fleet_save_test` (match.v1) | the file: whole, byte for byte, independent of save.v1, oversized refused, a write that cannot happen, 0600 under any umask |
| `tests/fleet_app_test` (sections 8, 9) | whole multiplayer matches under a finger in both shapes, a turn across the page never scrolling, reopen and RESUME MATCH, the link lost and CHECK LINK; the header always exactly the derived status, turn by turn; the chat button beside FIRE in multiplayer and absent in single player; the chat typed on keys and sent with Enter, the answer shown, an over-long accented line refused, the touch keyboard only for a finger down the page, a line said while on the board counted on the button, chat from Result after the match and gone once the match is put away |
| `tests/fleet_shell_test.sh` (section 10) | every multiplayer state rendered in both shapes in the shell, `match.v1` written whole, no mesh service, a damaged `match.v1` |
| `tests/fleet_match_test` (profiles, golden) | two fixtures written by the build before profiles existed (master `1767f09d`, reproduced from a clean clone of it) - `match.v1.golden`, mid-battle with a pending shot, and `match.v1.done.golden`, a finished and verified match over a full tombstone table: each decodes, restores, re-encodes byte for byte, and the same match played now saves the same bytes; the mesh profile is the old constants; the invite and accept windows are equal on both profiles while the LAN repeats at its own pace; lost invitations, a guest who answers after three minutes, the deadline itself and an acceptance after it, a lost START; the mesh still six tries and no deadline; link waits in minutes on the mesh and seconds on the LAN; nothing rationed on the LAN, a whole match there verified; which saves count as a match in hand |
| `tests/fleet_session_test` (profiles, one match) | a session takes its link's profile; whole matches on the LAN profile, clean and lossy; invite and accept refused, with nothing sent, while the other transport holds a match |
| `tests/fleet_save_test` (transports) | the two files side by side, each read back whole by its own transport only, 0600, a damaged `match.v1` left untouched by a LAN save |
| `tests/fleet_lint.sh` | the layers: net pure, two files talk to a service (the mesh link to meshcored, the LAN link to peerd, neither to the other's; the LAN link opens no network socket, does no cryptography, and is not opened by the app yet), no radio method, nothing pumped before engaging, the virtual opponent only when asked for; `match.v1` named by the store alone and in one place, no link touching a save, only the mesh session asking for the mesh's file |
| `tests/fleet_link_test` | the mesh link against a real socket and a scripted meshcored: nothing asked before the first poll, the four link states, players listed (no repeaters, no forgotten nodes), each datagram taken once from the inbox and from events, the cursor reset on a new service run, stale and malformed datagrams refused, exactly the bytes handed over sent on port 1, a zero-hop advert, a radio that is off |
| `tests/meshcored_runtime_test.cpp`, `tests/meshcored_service_test.sh` (3d) | meshcored's half: app datagrams between two runtimes with real crypto, flood then direct once the receipt teaches the route, crafted REQs that are not app datagrams ignored, the inbox bounded; every refusal of `mesh.app_send` / `mesh.app_inbox` against the real process |
| `make ENABLE_MESHCORED=1 fleet-mp-e2e` | **P6.** Whole matches between two Fleet players (`tests/fleet_mp_player`: the real session, mesh link and match, the AI for a finger) through two real meshcored processes over stand-in radiods and a mock air: at the product's own pace; with 15 % of frames lost and 5 % duplicated; with the guest's app killed at ply 30 and resumed from its save; and with the guest's meshcored restarted at ply 40; and ("chat") with both players talking in three-block lines all through a fast match. Each must end with one win and one loss, both fleets verified, identical records, no violation, datagrams both ways, a receipt, and no message in `mesh.messages` (Fleet's chat is app datagrams, never RIFT's text); "chat" also every line held the opponent's, once, in order, none given up |
| `tests/fleet_link_peer_test` | the LAN link against a real socket and a scripted peerd (`tests/fake_peerd.c`): nothing asked before the first poll; the link states and why; the LAN profile; players listed most recent first, without this device or a broken entry, a kept peer not heard this run with no time and no route; each datagram once from the inbox and from events, the cursor reset on a new run; exactly the bytes handed over, on port 1; the lease renewed after ten seconds and not on every poll, at once on request, ended on close; nothing kept or released before the session says who, then the opponent kept and a stale peer released, asserted again after a restart, released when the match is over; closing with a match in hand releasing nobody, closing straight after the session stops naming an opponent releasing them by name and still hiding, a link that has already put the list right releasing nobody twice; an opponent let go and named again before a poll not released; what is not an address never asked, a probe answered, unanswered, replaced by another before its answer, pending, and ended when peerd goes; which players are proved; an event during the catch-up and a reconnect to the same run, nothing lost or taken twice; a refused send, and a refused keep retried at a growing interval; a peerd closed by a fault; a peerd with nothing to announce on |
| `tests/fleet_session_test` (match peer) | the session tells its link who the match is with: nothing before engaging, none while an invitation is only out, the opponent from Deploy, the saved match's opponent first thing after a reopen, still the opponent when the match is over on this side and for as long as they may be trying, the wait starting over when they are heard, none once they have been quiet for the profile's run of tries or the match is put away; on leaving (`fleet_session_close_retention`) nothing said by a session that never engaged, has no match or has not read its save, the opponent of a match in play or read back unfinished from its save never let go, nor one whose END is still owed, the opponent of a finished match let go at once, also after a reopen on that finished match; nothing at all when the save could not be read or is damaged, and a match begun after that named and then ended as usual |
| `make ENABLE_PEERD=1 SODIUM_DIR=... fleet-lan-e2e` | **M2 package 6.** Whole matches between two Fleet players (`tests/fleet_lan_player`: the real session, LAN link and match, the AI for a finger) through two **shipped** peerd processes, each in a network namespace of its own with a real interface (`tests/fleet_lan_e2e.py`; no root, and nothing left behind when the test is stopped - a SIGKILL of the wrapper alone leaves its scenarios to run to their end): found by announcement on one subnet; with 15 % of packets lost, 5 % duplicated and 20 ms delay each way; with the guest's app killed at ply 30 (on the host's turn) and resumed from its save; with the guest's peerd killed at ply 40 and started again; with the guest's address changed at ply 30; with the guest's app stopped and then its address changed, so that only the host's peerd asking can find it; with the guest's interface down for eight seconds; across a router that carries no broadcast, joined by address; with both players talking; with the host forfeiting at ply 20 under loss; with the first finisher's last packet thrown away; with the host's END given up unanswered; and with the guest's app closed at ply 30 and reopened. Each must end with one win and one loss, both fleets verified, identical records, no violation (the forfeit: the forfeiter lost by its own END, which was answered; "endlost": given up, and learnt all the same). After "clean" each keeps the opponent, the host's app closed lets go at once, the guest's by itself later (not before 15 s). In every one the players are hidden from Deploy on, as the app is outside its lobby, so the match is played with the kept opponent alone; each peerd is checked to be hidden, bound and keeping exactly one when the match begins, sampled to the end of the match for never being visible again, and checked to keep nobody and close its socket after it |
| `tests/fleet_link_peer_test` (late answer) | against a scripted peerd that answers `peer.probe` late: an address asked and replaced before the first answer arrives - the late answer is not taken for the second's |
| `make ENABLE_PEERD=1 SODIUM_DIR=... fleet-link-peer-real` | the same link against two real peerd processes on two loopback addresses (`tests/fleet_link_peer_real`): found by address, datagrams both ways through peerd's sealing, each peerd keeping exactly the other, visibility ended and the kept peer still bound after the links close. Opt-in: it needs libsodium |
| `make fleet-mp-san-test` | the native suites under ASan and UBSan, `fleet_link_test` and `fleet_link_peer_test` included |
