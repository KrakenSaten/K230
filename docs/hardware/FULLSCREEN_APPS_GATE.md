# Fullscreen apps on unit A: the gate

**Status: PASS, 2026-09-24 07:36–08:10 UTC, on `a30678e`.** DS §30.8 is
accepted on this gate (see the end). The owner did not attend: the owner asked
for the gate to run unattended, and asked for §30.8 to be accepted if the
implementation matched the proposal.

**Unit A carries `a30678e`**, all of it. It was deployed with
`platforms/k230/scripts/deploy.sh` from an image built from a clean clone
(`doors-0.0.11-tdisplay-k230-a30678e.img.gz`; raw image sha256
`6c1c752a…a8c`; IMAGE GATE PASS; source clean; vendor pins clean). After the
deploy, all 37 deployed files hashed the same on the unit as in the build's
target tree: the binaries, the init scripts, `doors-release` and the UI
assets. `/etc/doors-release` says `BUILD_ID=a30678e`, as do the shell and
`doors system info`. All five services came up. Before the deploy the unit
carried RC1 `9a4afeb` with the Files shell `2bccc5b` over it. Rollback:
`sh /root/rollback-fullscreen/RESTORE.sh`, which restores
`userspace-before.tar` (sha256 `8f1b6186…`). A per-unit state tar is kept
beside it (`bd48b49c…`). Both are also copied to
`out/fullscreen-gate/backup/` on the host.

The evidence is in `out/fullscreen-gate/`: the step scripts (`s1`–`s8`), their
logs (`step*.log`) and the panel captures (`caps/`, taken with kmsgrab from the
DRM plane). Touch was injected into the evdev node through the shell-logged
calibration (`rift_tap.py`). Rotation and lock were driven over `shell.*`, and
unlock was a real swipe gesture. Taps were made on the real layout; the two
that missed were at old coordinates (Wave's STOP LISTENING moves down under the
microphone banner; Fleet's portrait RESUME sits in its panel) and were redone
at the right place.

## Result

| | |
| --- | --- |
| **PASS** | A. Fullscreen: all six apps under NONE in portrait and landscape (`shell.info` `policy none, 0, shown 0`), no bar and no strip; System and Calendar keep FULL/COMPACT; the launcher keeps its 56 px bar |
| **PASS** | B. Lock over each of the six in both orientations: the bar is shown at 56 (portrait) or 32 (landscape) over the lock, with DOORS, the app's hint and the chip, while the app stays NONE underneath. A real swipe unlocks back to the same app with `shown_height 0`, and the header is as before. A capture mid-sequence (the open door) shows the bar still up, as designed: it goes when the app starts to show through |
| **PASS** | C. Open, home, reopen, three times each for all six in both orientations, plus two ordinary apps: NONE while open and FULL 56 at home, every time |
| **PASS** | D. Rotation with an app open lands on the launcher with FULL. Rotation while locked (over Radar) comes back locked on the launcher with the right bar, both ways |
| **PASS** | RIFT: ACTIVITY (meshcored online, 202 nodes), NODES, COMMS list, channel threads, composer; both orientations |
| **PASS** | Notes: a new note typed on the touch keyboard in portrait, saved and found on disk; edited in landscape with the keyboard up (field 96..251, 20 px above the keyboard); back, reopen; the editor was opened and closed 10 times more |
| **PASS** | Wave: listening shows MIC ON at the header's right end and the banner in both orientations; STOP LISTENING and the header back button each end the helper (`pidof pos-wave` empty); the landscape send page scrolls to TRANSMIT |
| **PASS** | Fleet (landscape): Deploy with AUTO; Battle aiming on the 51 x 40 cells (the centres of A1 and J10 give A1 and J10; points 4 px inside E7's top-left and bottom-right corners both give E7; the nudges move E7 to F8); a match with a rotation-restart RESUME in the middle, played to the Result screen (62 rounds); the turn text in the header; the portrait Command and Deploy screens |
| **PASS** | Radar: SCANNING in the header, the landscape scope as tall as the body, the run played to RUN COMPLETE; portrait scanning |
| **PASS** | Timber: the 728 px portrait viewport with the controls on the foot; BEGIN, a block chosen, TEST, pulled on the track into the placing state; IN PLAY and STANDBY in the header; the landscape page scrolls as before |
| **PASS** | Corners: the header's right end stops at the calibrated insets (x 1182 in landscape, 538 in portrait) and the back slab starts at them (50, 30). The 50 px landscape top corner is the unit's calibration (2026-09-21: 50 PASS, 45 FAIL) |
| **PASS** | Stability: 48 open/close cycles, 4 rotations, 5 lock/unlock swipes, 10 keyboard show/hide cycles. doors-shell RSS 15.1 MB → 15.6 MB, the other services flat. The same PIDs throughout: no supervisor restart, no crash report, no WARN/ERROR in 446 shell.log lines |

## Seen, not changed

- Radar in portrait keeps §29's scope, which the width limits, so the page
  ends about 200 px above the foot (56 px more than under FULL).
- Timber and Wave in landscape are the scrolling pages they were before
  (§30.8 table).
- RIFT's landscape ROUTE pane reaches the right edge of the screen. It did
  that before this branch as well (simulator, `07b0ec8`).

## DS §30.8

The implementation matches the proposal: six apps are NONE in both
orientations, the hint is in the header, and the lock shows the default bar
and hides it again once the app shows through. Fleet's cells are 40 x 51,
Timber's viewport is 728 px in portrait, and every other screen is unchanged.
§30.8 is therefore marked ACCEPTED, on this gate, as the owner asked.
