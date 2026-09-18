# PocketFleet in landscape — validation gate

Branch `feature/fleet-landscape`, tip `f17ca59`, from master `7d0d1ea`
(VERSION 0.0.10, unchanged). DS Amendment L (§28) is **PROPOSED**.

**Verdict: host and simulator PASS; unit A remote PASS. Two questions are
left for the product owner, and both are about how it feels under a hand.**

Fleet is the seventh app given a landscape layout under DS §21.3, after
Calendar (§27). The shape is chosen from the body the shell gives the app and
from nothing else; `tests/fleet_lint.sh` refuses any reference to the
orientation in the app. What is new against the six before it is that the
thing being laid out is a **board of touch targets**, and the height of a
landscape body makes them smaller than Fleet's already-approved deviation D1:

```text
TALL   portrait, 528 x 1060: the v0.0.10 layout, boards at 48 px cells.
WIDE   landscape, 1192 x 386 once the foot clears the rounded corners:
       board 382 x 382 at 34 px a cell, and what is said about it beside it.
```

## What the owner is being asked

1. **Are ~34 px board cells comfortable and reliable under a normal thumb?**
   Everything that can be checked without a hand has been: the mapping is
   exact, the whole square belongs to its cell, and a mis-tap costs nothing
   because a tap only aims. Whether the square is big enough for a thumb is
   not a thing a test can answer. DS §28.2 stays PROVISIONAL until it is.
2. **Does the board-and-columns arrangement read well?** Board at the left,
   the readout and FIRE beside it, your own waters beyond that.

Nothing else is outstanding. There is no third question about portrait: Deploy,
Battle and Result are pixel-identical to v0.0.10 below the status bar, and
Command's only change is nine pixels, explained in §28.4.

## Host validation

From a fresh clone of `f17ca59`, and of master `7d0d1ea` for comparison.

| Check | Result |
| --- | --- |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,780 ok, 0 FAIL, 0 warnings (master: 3,772; the 8 new `fleet_lint` checks) |
| SDL simulator build | rc 0, 0 first-party warnings |
| 21 shell and UI test scripts | 536 ok, 0 FAIL, every script rc 0 (master: 525; `fleet_shell_test.sh` 23 → 34) |
| `fleet_app_test` | **280 checks, 0 failures** (new) |
| `fleet_lint.sh` | 13 checks, 0 failures (master: 5) |
| `fleet_rules_test` / `fleet_ai_test` / `fleet_rng_test` / `fleet_save_test` / `fleet_theme_test` | unchanged and green: the engine is byte-identical to master |
| `calc_app_test`, `notes_app_test`, `settings_app_test`, `system_app_test`, `clock_app_test`, `cal_app_test` | 456 / 1,138 / 636 / 833 / 778 / 1,317 checks, 0 failures: the other apps unaffected |
| `display_geometry_shell_test.sh` | 69 ok, 0 FAIL: all eleven apps still open in landscape |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 4 warnings, all `vendor/ggwave`, as in the release; 0 first-party |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; `Doors 0.0.10 build f17ca59`; all 14 Fleet objects compiled |

### Mutation testing

40 mutations of the new layout, the board's geometry and the tap conversion.
**39 caught.** The one that is not: the app compares the body's box (and the
safe-area insets it implies) before laying out again, and disabling that
comparison changes nothing observable — LVGL reports a size change only when
the size changed, so the comparison never gets the chance to refuse a pass. It
is kept as the same defensive guard Calculator, Notes and Clock carry.

Caught: the wide shape taken for any body; the cell floor removed; the width
floor removed and off by one; the cell not capped at the tall cell; no corner
clearance at the foot, and the clearance taken from the side instead; the
body's size never watched; each of the four screens never given the wide
shape; a screen never turned across the page; a splitter never split; a column
never given its share of the row; a caption clipped by the box it is in; the
tall stack not scrollable; a resized board drawn at one size and hit at
another; a resize that changes nothing; the span without its gutter; a cell one
pixel too large; the gap left out of the hit test; the caption gutter treated
as board; the target board, your own board and the deployment board never
resized; FIRE and CONFIRM not held at the foot of their columns; no room for
the captions beside the board, and none for the first caption down the page;
the roster read off the panel's child list again; DEPLOY FLEET stretched
across the body and not at the end of its row; a column that overflows unable
to scroll; the two accounts and the two ways on not sharing their rows; RESUME
never moved to the foot row, left there down the page, or put after the
primary action.

Five of these were gaps in `fleet_app_test` that the first mutation run found;
they are closed in `f17ca59`, not argued away.

### What `fleet_app_test` adds

The app hosted the way the shell hosts it — a header and a padded body — on
the reference panel with its 30 px rounded corners and with square ones, in
portrait and landscape, in Normal and Outdoor:

- the shape rule as arithmetic, at and either side of every floor, including
  the one case only "wider than it is tall" can refuse;
- all four screens in both shapes: what stands beside what, every action a
  finger's size, nothing outside the body or the safe area, and every panel
  caption drawn rather than cut;
- **every cell of the board tapped at its centre and at all four of its
  corners, at 48, 35 and 34 px** — 1,500 taps — plus the two-pixel gaps
  between cells, the caption gutter and points just off the board;
- aim-then-confirm: a tap never fires; FIRE fires exactly one shot and only on
  a square not already fired at; your own waters are not a target at all;
- the display turned under a match in progress, three times over: the same
  objects, none added, the match untouched — crosshair, turn, both boards, the
  fleet as placed — and a paced reply still in flight neither settled early nor
  lost;
- a body that has not changed laid out again does no work at all;
- the Deploy roster, the placement controls, CONFIRM refusing an incomplete
  fleet and starting the engagement once it is complete;
- the Result screen's figures, unchanged by the layout;
- the save written either way up, the same file, and offered again.

## Simulator

40 captures per run: five states (Command, Deploy, Battle, Battle paced,
Result) × two orientations × 30 px and square corners × Normal and Outdoor.

| Check | Result |
| --- | --- |
| Every capture rendered, no fault in any log | 40 of 40 |
| Foot corner squares, landscape, 30 px corners | **clear in 10 of 10** |
| Portrait against master, below the status bar | Deploy, Battle (Normal) and Result **pixel-identical**; Battle in Outdoor differs in 6 rows at the foot; Command is master's layout translated down 9 px |
| The 6 rows | y 1212–1217: master drew the last panel's thicker Outdoor border into the body's bottom padding; the frame now clips at the body's content box. The foot clearance doing its work (§28.4) |
| The 9 px | The caption of Command's first panel, which now has its room inside the frame instead of straddling into the shell's padding (§28.4) |
| `battle_paced` in Outdoor | Differs between runs **on master too** — the resolution flash is timed, so a still of it is a race. Three runs of the branch were identical to each other; three of master were not |

Foot corner squares in **portrait** on Battle are not clear, on this branch or
on master: the stack is longer than the body and, while it is scrolled,
content passes through the last few pixels as scrolled content does. What the
clearance guarantees is that the end of the stack clears the corners when the
scroll reaches the end, which `fleet_app_test` checks directly. On master the
same content reached 20 px further in.

## Unit A: remote validation

Everything below ran with nobody at the unit. The console (COM9, 115200) was
used to install a bench SSH key, after which Ethernet carried the transfers
and the captures; serial alone dropped about a third of the bytes of a 6.8 kB
payload, which is not a link a 953 kB binary or a dozen framebuffer grabs can
use. **The key is removed at the end of this work** (see "As left").

| Step | Evidence | Result |
| --- | --- | --- |
| Identity before | `/etc/doors-release` 0.0.10 / `9f9c802`; the installed shell was the Calendar gate's `99b2374`, md5 `06472f2e…`; portrait, Automatic, keyboard absent; theme carbon, Normal, brightness 100; Wi-Fi **off**, 0 saved networks; 1 doors-shell; sysd/netd/radiod up; 0 crashloop, 0 crash reports; `shell.log` 0 ERROR 0 WARN, 567 lines; VmRSS 12,672 kB; no fleet save | recorded |
| Transfer | `doors-shell` from the riscv64 DRM build, stripped, 953,008 B, md5 `7d0ba912…` then `128d1023…`; md5 equal on both ends | PASS |
| Install | rollback copy `/root/doors-shell.rollback` (the `99b2374` shell); only `S90doors-shell` stopped and started; the shell answers `build a849784`, later `build 052e549`; 1 doors-shell, 0 crashloop, 0 restarts | PASS |
| Touch path | taps written as input events into `/dev/input/event1`, the touch controller's own node, with raw coordinates found by inverting the calibration the shell logged for the rotation in force — so each tap goes through the kernel input core, LVGL's evdev driver, the shell's swap and calibration, and whatever is under the point | used for every tap below |
| **The roster defect, on the panel** | Portrait Deploy: tapped the **Destroyer** row — the last one, which on master selects nothing at all — then tapped A1. The Destroyer was placed at A1–B1 and its row reads PLACED | **PASS** (capture `p-deploy-destroyer.png`) |
| Portrait board | Tapped cells (4,4) and (0,9); the readout named **E5** and **J1** | PASS |
| Portrait turn | FIRE: `YOU J1 MISS · ENEMY A8 MISS`, crosshair cleared, FIRE disarmed, the enemy's shot on your own board, save written (898 B) | PASS |
| To landscape | `doors call shell shell.rotation mode=landscape`: applied in place, same pid 3657; `DRM plane rotation 270`; touch `swap 1, calibration 2400,0,0,1060 onto 1232x568` | PASS |
| The match across the turn | Fleet reopened: **"An engagement is waiting: OFFICER · TURN 2"**, RESUME taken, the board showing the J1 miss and the enemy's shot. The match survives an orientation change on the unit because it is stored after every resolved turn, not because the app is relaid out — a rotation there restarts the shell and comes back on the launcher | PASS |
| Geometry the app actually has | `shape wide cell 34 span 382 board at (20, 152) frame (20, 152, 1211, 547) foot 10` — the arithmetic `fleet_app_test` pins on the host, arrived at independently on the unit | PASS |
| **34 px cells, on the panel** | Aimed at the four corners of the board and its middle: the readout named **A1, J1, A10, J10, E5**, each the cell asked for, and J1 correctly said "has already been fired at" with FIRE unarmed | **PASS** |
| **The whole square** | All four corners of cell C7 tapped: the readout named **C7** every time | **PASS** |
| A match played out | 87 aim-and-fire pairs, 174 injected taps, to the end of the engagement; Result showed `Fleet lost`, 87 rounds, 0 of 5 afloat, 13 % / 20 % accuracy | PASS |
| Deploy across the page | NEW ENGAGEMENT → Deploy; AUTO placed the whole fleet, the roster read five PLACED, CONFIRM DEPLOYMENT armed at the foot of its column and started the engagement; aiming at (5,5) read **F6** | PASS |
| Foot corners in the captures | 16 of 16 unit A captures have clear bottom corner squares | PASS |
| Resources | VmRSS 12,544 kB before the 87-shot match and 12,544 kB after | PASS |
| Health after | 1 doors-shell; sysd, netd and radiod unchanged; 0 crashloop; 0 crash reports; `shell.log` **0 ERROR, 0 WARN**; no segfault in `dmesg` | PASS |

### What the unit found that the host had not

With a match waiting, Command's **RESUME** button sat at the end of the
right-hand column, which scrolls, so the one button you want when there is
something to resume was almost entirely below the fold. Fixed in `052e549`:
across the page the ways on share the foot row — RESUME, then DEPLOY FLEET —
and neither can be scrolled out of sight; down the page RESUME goes back
inside the panel that describes the saved match, where v0.0.10 has it. The
fix was verified on the unit on the rebuilt binary.

### What only the panel and a hand can show

A capture is the framebuffer, not the glass: it cannot show the rounded
corners cutting anything, and an injected event cannot show where a finger
lands. So the physical check is the two questions at the top of this sheet.

## As left

Unit A: **portrait, Automatic, on the launcher**, theme carbon, Normal,
brightness 100, Wi-Fi **off** exactly as found, 1 doors-shell, 0 crashloop,
0 crash reports, `shell.log` 0 ERROR 0 WARN. Running build `052e549`, with the
rollback copy at `/root/doors-shell.rollback`.

**Rollback**: stop `S90doors-shell`, copy `/root/doors-shell.rollback` over
`/usr/bin/doors-shell`, start it again.

The bench SSH key installed for this work is removed at the end of the
milestone, returning `/root/.ssh` to the state it was found in.

Captures and logs: `out/fleet-landscape-a849784/hwgate-unitA/` (outside the
repository).
