# Fleet chat and status: the two-device gate

**Status: NOT RUN.** Written 2026-09-30 with branch `feat/fleet-chat-status`.
Unit A did not answer on the bench network that day (it was last prepared
for the office, see the unit A transfer notes), so there was no second
Fleet-capable device. **Neither unit carries this branch.** Before any step,
`doors shell info` on both units must report the build id of the branch
tip; write it here:

| unit | build id reported | rotation mode |
| --- | --- | --- |
| A (`Mstr_k230`) | | |
| B (`K230-B`) | | |

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
2. On each unit: copy `/usr/bin/doors-shell` to `/root/rollback-fleet-chat/`
   first, then install the new one (0755 root), stop and start only
   `S90doors-shell`. `meshcored` must be running (`MESHCORED_ENABLE=1`).
3. `POCKETFLEET_MP_FAKE` and `POCKETFLEET_SCREEN` must be unset.

Rollback: put `/root/rollback-fleet-chat/doors-shell` back and restart
`S90doors-shell`.

## Steps

| # | Do | Expect |
| --- | --- | --- |
| 1 | A: Fleet, MULTIPLAYER, invite B; B accepts; both deploy | header: CONNECTING while the invitation is out, DEPLOY YOUR FLEET, then WAITING FOR `<other>` until both have deployed |
| 2 | Look at both at the start of Battle | the guest (B) YOUR TURN · SHOT 1; the host (A) WAITING FOR `<B>`; a CHAT button beside FIRE on both |
| 3 | Play six plies | after every FIRE the shooter's header turns WAITING at once; when the answer and the next shot land it turns YOUR TURN on the other side |
| 4 | A: CHAT, type a line, Enter (or SEND) | A shows "YOU · … · SENDING", then the line alone; B's button reads "CHAT · 1 NEW" over "`<A>` · …" |
| 5 | B: CHAT, answer | the same the other way; opening the chat clears "NEW" |
| 6 | Several lines each way mixed with moves; one of 37 characters, one with æøå | each line once on each side, in the order said; nothing waits more than a few seconds unless a shot is waiting for its answer; the game never slows |
| 7 | Count on both | lines shown = lines sent, no repeats (`doors shell` log has no errors) |
| 8 | Compare the boards | the same plies, hits and sinkings on both |
| 9 | Stop `meshcored` on B (or take B out of range) mid-battle, A fires | A: RECONNECTING while the shot is retried, then OPPONENT DISCONNECTED; FIRE reads CHECK LINK; a line said now ends NOT DELIVERED |
| 10 | Start it again (back in range), A presses CHECK LINK | SYNCING, then the turn status; the shot is answered once; new lines go |
| 11 | Finish the match | GAME OVER on both; Result offers CHAT, and "gg" still goes both ways |
| 12 | MULTIPLAYER (puts the match away) on both, reopen Fleet, start a new match | the new match's chat is empty on both sides; no line of the old match appears |

Record, for the owner: the time from SEND to the line appearing on the other
unit (median of five), whether any line appeared twice, the `radio.stats`
`tx_packets` before and after step 6, and each unit's `mesh.status`
`app_tx`/`app_rx`.

## Results

Not run. See the status line at the top.
