# Fleet chat and status: the two-device gate

**Status: RUN 2026-09-30, 17:55-18:40 local - PASS**, all twelve steps, on
LoRa between unit A and unit B. **Both units carry the gate build
`3235f38`** (`doors-shell` only, md5 `dbcc3975…`, stripped 1,728,072 B,
built in a clean clone at that commit, 0 first-party warnings) and are left
on the home screen with no match in hand; rollback in
`/root/rollback-fleet-chat/RESTORE.sh` on each.

| unit | identity | build before | build reported for the gate | rotation |
| --- | --- | --- | --- | --- |
| A | `Mstr_k230`, mesh key `19f7b327…` | `50bf635` (shell `e6e653fb…`) | `3235f38` | automatic, landscape (270) |
| B | `K230-B`, mesh key `ca74f762…` | `c7e1910` (shell `f5d6b626…`) | `3235f38` | automatic, landscape (270) |

`meshcored` and `radiod` were not touched (A: meshcored `36a8ed8a…`; B:
`0a4026dc…`); profile 869.618 MHz, SF8, 62.5 kHz, 2 dBm on both. Unit A came
back re-flashed, with a new SSH host key (`SHA256:0Yxof…`), accepted by the
owner for this gate and kept in a gate-only known_hosts file.

What the host already proved, so the gate is only about what it cannot:
the codec, the state machine and the chat rules (`fleet_proto_test`,
`fleet_match_test`), thousands of matches with both players talking through
loss, collisions, outages, crashes, restarts and reboots (`fleet_mp_sim_test`),
the session and the virtual opponent (`fleet_session_test`), the screens under
a finger in both shapes (`fleet_app_test` sections 8 and 9), and whole matches
with chat through two real `meshcored` processes with real MeshCore
encryption, including the 42-byte three-block datagram (P6, `make
ENABLE_MESHCORED=1 fleet-mp-e2e`, scenario `chat`). What only the radios can
show: that a three-block line crosses the real air, how long a line takes,
how chat and shots share a real half-duplex channel, and how the status reads
on real glass.

## Install: one file per unit

Only the shell changes; `meshcored`, `radiod`, `match.v1` and every message
the game already sends are unchanged, so a unit on this build still plays a
unit on master (its chat is simply never answered).

1. Build the riscv64 DRM shell from the branch tip
   (`POCKETOS_DISPLAY=drm`, `POCKETOS_LVGL_MODE=sysroot`) and strip it.
2. On each unit: copy `/usr/bin/doors-shell` (and `match.v1`) to
   `/root/rollback-fleet-chat/` first, then install the new one (0755 root),
   stop and start only `S90doors-shell`. `meshcored` must be running.
3. `POCKETFLEET_MP_FAKE` and `POCKETFLEET_SCREEN` must be unset.

Rollback: `/root/rollback-fleet-chat/RESTORE.sh` puts the shell and
`match.v1` back and restarts `S90doors-shell`.

## How it was driven

No unit has a physical keyboard, so everything was done with injected
single-finger taps (`tests/hw/touch_slot0_tap.py`), chat lines included:
they were typed key by key on the Doors touch keyboard (KEYS, in landscape),
which puts the touch-keyboard path under test too. Tools and logs are in
`out/fleet-chat-gate/` (not in the repo): `gate_drive.py` plays the match
from each unit's `match.v1` - its targets are the opponent's own ships, read
from its save, so the match ends in under 40 plies - and logs both headers
(`doors shell info`, `header.hint`) before every shot, right after FIRE and
after the answer; `gate_type.py` types a line on the unit.

## Steps

| # | Do | Expect | Result |
| --- | --- | --- | --- |
| 1 | Invite, accept, both deploy | CONNECTING while the invitation is out, DEPLOY YOUR FLEET, then WAITING FOR `<other>` until both have deployed | **PASS.** B invited (see deviations): B CONNECTING; after ACCEPT both DEPLOY YOUR FLEET; A deployed first: A WAITING FOR K230-B, B still DEPLOY YOUR FLEET |
| 2 | Both at the start of Battle | the guest YOUR TURN · SHOT 1; the host WAITING FOR `<guest>`; CHAT beside FIRE on both | **PASS.** A (guest) YOUR TURN · SHOT 1, B WAITING FOR MSTR_K230; CHAT on FIRE's row on both |
| 3 | Play plies | after every FIRE the shooter's header turns WAITING at once; when the answer lands it is YOUR TURN on the other side | **PASS, all 38 plies of the match**: the shooter WAITING right after FIRE every time, the other YOUR TURN · SHOT n (n correct) after the answer |
| 4 | A: CHAT, type a line, Enter | the line delivered; B's button "CHAT · 1 NEW" over "`<A>` · …" | **PASS.** "hello from unit a": A "YOU · hello from unit a" (delivered), B "CHAT · 1 NEW / MSTR_K230 · hello from unit a" while its header said WAITING FOR MSTR_K230 |
| 5 | B answers | the same the other way; on the chat screen nothing counts as new | **PASS.** "copy that from unit b" on A's open chat, no NEW |
| 6 | Several lines mixed with moves; a 37-byte one | each line once on each side, in order; the game never slows | **PASS.** Lines at plies 2, 3, 5, 20 between shots; FIRE → answer 1.6-1.8 s throughout. The field stopped a 38-character entry at 37 ("…thirty seven byt"), and that 37-byte line - a 42-byte packet, three AES blocks - crossed the air and was delivered. æøå: not on the touch keyboard's letter layer, so not typed here (host tests cover them) |
| 7 | Count on both | lines shown = lines sent, no repeats | **PASS.** Nine lines by the end, the same nine in the same order on both, each once, every one of ours delivered |
| 8 | Compare the records | the same plies, hits and sinkings on both | **PASS.** Both `match.v1` logs byte-identical over all 38 plies; each side "Verified" the other's fleet |
| 9 | Stop `meshcored` on B mid-battle; A fires and says a line | A: RECONNECTING while the shot is retried, then OPPONENT DISCONNECTED; FIRE reads CHECK LINK; B: MESH OFFLINE; the line waits | **PASS.** B MESH OFFLINE at once; A WAITING FOR K230-B at FIRE, RECONNECTING from about +20 s, OPPONENT DISCONNECTED at +279 s (six tries); note "K230-B is out of reach. The match is paused, not lost."; CHECK LINK. The line read "YOU · are you still there · WAITING" and was never put on the air (the first draft of this sheet said NOT DELIVERED; a line waits while the opponent is out of reach, by design) |
| 10 | Start `meshcored` again; A presses CHECK LINK | SYNCING, then the turn status; the shot answered once; the waiting line goes | **PASS.** SYNCING, then within 8 s A WAITING FOR K230-B and B YOUR TURN · SHOT 5 (the shot answered, once); "are you still there" delivered and shown once on B |
| 11 | Finish the match | GAME OVER on both; Result offers CHAT; a word still goes both ways | **PASS.** Ply 38 sank A's last ship: GAME OVER on both at once; A "Fleet lost", B won; Result CHAT; "good game well played" / "thanks good game" delivered both ways; BOARD back to Result |
| 12 | Put the match away on both, reopen Fleet, start a new match | the new match's chat empty on both; no old line | **PASS.** New session `278cac` (was `073615`); B's chat "Nothing said yet…", A's button "Say something to K230-B"; the new match was then forfeited by A (both GAME OVER; "You forfeited") and put away on both |

## Measurements

| | |
| --- | --- |
| FIRE → answer on the shooter, 38 plies | median 1.7 s (1.6-1.8 s; one 2.5 s) |
| Enter → the line in the other unit's meshcored | 1.1 s, including the ~0.5 s SSH round trip of the injected DONE tap (one clean measurement; the first line's clock started late) |
| receipt back | within the same 0.2 s poll |
| `radio.stats` over the whole session (two matches, 11 chat lines, adverts) | A 78 frames, 26.2 s airtime; B 69 frames, 22.8 s |
| `mesh.status` app datagrams at the end | A `app_tx` 77 / `app_rx` 65; B 48 / 45 (B's counters restarted with its meshcored in step 9) |
| errors | 0 error, assert or crash lines in either `shell.log` since the gate build started; no crash files |

## Deviations

- **B invited, A accepted** (the sheet had A inviting). A had just booted and
  its lobby did not list K230-B although its `meshcored` held the node and
  heard B's advert (RSSI -26 dBm); B listed A at the top. The lobby's player
  list on a unit whose node table holds ~270 nodes showed only two stale
  ones - existing lobby behaviour, not this branch; A's list showed K230-B at
  the top as soon as the first datagram from B arrived. Roles do not change
  anything under test.
- The "local move and a line in the same breath" case cannot be timed by hand
  on hardware (typing a line takes about 20 s); the host tests pin that a
  line never goes while a shot waits for its answer.
- Only one clean chat-latency measurement instead of five.

## Found in passing, not this branch

- AUTO on Deploy gave unit A the identical fleet in both multiplayer matches:
  multiplayer seeds AUTO from the single-player game's seed
  (`fleet_screen_deploy.c`, `fleet_screen_deploy_enter`). Filed as a separate
  task.
- My first tap on B's lobby hit a player row (ANDERS) rather than MAKE
  VISIBLE, because B listed five players and A two; nothing was sent to
  that node (INVITE was not pressed).
