# Fleet multiplayer AUTO deployment: the two-match gate

**Status: RUN 2026-09-30, 19:25-19:36 local - PASS**, over LoRa between unit A
and unit B. **Unit A carries the gate build `2a6740e`** (`doors-shell` only,
md5 `ab4ee2dd…`, stripped 1,756,744 B, built in a clean clone at that commit,
0 first-party warnings). It is left on the home screen with no match in
hand, and its rollback is in `/root/rollback-fleet-auto/RESTORE.sh`. **Unit B
was not changed.** It stayed on master `bae3695` (shell `5a1f8e43…`) and
served as the opponent and the negative control.

| unit | identity | build before | build during the gate | rotation |
| --- | --- | --- | --- | --- |
| A | `Mstr_k230` | `3235f38` (shell `dbcc3975…`) | `2a6740e` | automatic, landscape (270) |
| B | `K230-B` | `bae3695` (shell `5a1f8e43…`) | `bae3695`, unchanged | automatic, landscape (270) |

`meshcored` and `radiod` were not touched on either unit.

## What is being checked

Before the fix, Deploy seeded its AUTO stream from the solo game's seed.
That seed does not change between multiplayer matches in one app run, and on
a unit with a saved solo game it does not change across runs either. AUTO
therefore placed the same fleet in every match. This was seen on unit A on
2026-09-30, during the fleet chat gate. Every match ends with both fleets
revealed, so an opponent who has seen the fleet once knows it the next time.

The host already shows that two matches against the virtual opponent in one
app run get different AUTO fleets, and that solo AUTO still follows the match
seed (`fleet_app_test`, "multiplayer, AUTO per match"). The gate repeats the
multiplayer half on real glass against a real opponent.

Unit A holds a stored solo match (`save.v1`, "Officer turn 14"), which the
shell reads each time Fleet opens. That is the across-runs case of the
defect.

## Procedure

Both units: Fleet open, MULTIPLAYER. For each match:

1. B selects `MSTR_K230` and invites it. A accepts.
2. Both press **AUTO** and then **CONFIRM DEPLOYMENT**, with no other placement.
3. Once both reach Battle, the committed layout is read from each unit's
   `match.v1` (bytes 110-114, one byte per ship: bow cell plus a vertical
   flag).
4. The match is ended by forfeit from the lobby (Command, RESUME MATCH,
   FORFEIT, CONFIRM FORFEIT), and both players put it away.

In matches 1 and 2, A forfeited. Getting to A's lobby meant closing and
reopening Fleet, so each of those matches started a new app run on A. In
matches 3 and 4, B forfeited and A put the match away from its Result. A's
`shell.log` shows one `open app fleet` at 17:30:06Z and no close until
17:35:57Z, after match 4, so **matches 3 and 4 were played in a single app run on A**.

## Result

| match | sid | A (`2a6740e`) layout | B (`bae3695`) layout | A's app run |
| --- | --- | --- | --- | --- |
| 1 | `00c1cf17` | `5124cd06d9` | `bb939a1656` | opened 17:25:21Z |
| 2 | `007897bd` | `b9c0c187bf` | `bb939a1656` | opened 17:27:41Z |
| 3 | `0093be3b` | `4719a99603` | `bb939a1656` | opened 17:30:06Z |
| 4 | `003a827f` | `48550e2a96` | `bb939a1656` | same run as match 3 |

- **PASS: A never repeated a layout.** All four were different, including
  the two in one app run (3 and 4) and the ones across runs with a stored
  solo game (1 to 3).
- **Negative control: B, on the old seeding, placed the identical fleet in
  all four matches.** That covers two matches within one app run on B
  (1 and 2) and matches across B's reopens.
- A's solo `save.v1` was not changed by the gate: md5 `c237e3d2…` before
  and after.
- Both units' `shell.log` since 17:25Z has no error, assert or crash lines,
  and there are no crash files. A logged the known
  `radio.status timed out after 200 ms` warnings, each followed within a
  second by "radiod is answering again". These predate this change
  (FLEET_MULTIPLAYER_GATE.md).

Not checked on hardware: solo AUTO determinism. The host test covers it,
and nothing on this path reads the new entropy.

## Found during the gate (not this change)

**The lobby's selection follows a row number, not a player.** On B, the
player list re-sorted between matches (`MSTR_K230` moved from the fourth
row to the first). The highlight stayed on the fourth row, which was then
`ACK.11…`, a stranger's node. Pressing INVITE at that point would have
invited that node over the air. `fleet_screen_lobby.c` keeps `selected` as an
index into the peer list, and that list is re-fetched and can reorder on
every refresh. It was caught on a capture before INVITE was pressed. It is
filed separately.

## As left

- A: `doors-shell` `2a6740e`, home, landscape, match put away (idle),
  `save.v1` unchanged. Rollback: `/root/rollback-fleet-auto/RESTORE.sh`
  (shell `3235f38`, previous `match.v1` and `save.v1`).
- B: `bae3695`, unchanged, home (it was in Terminal before the gate), match
  put away (idle).

Gate scripts, captures and logs: `out/fleet-auto-gate/` (not tracked).
