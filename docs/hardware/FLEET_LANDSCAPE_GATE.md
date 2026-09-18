# PocketFleet in landscape — validation gate

Branch `feature/fleet-landscape`, from master `7d0d1ea` (VERSION 0.0.10,
unchanged). DS Amendment L (§28) is **PROPOSED**.

**Verdict: host and simulator PASS; unit A remote PASS. Two questions are
left for the product owner, and both are about how it feels under a hand.**

**Unit A is running this branch.** `doors shell info` answers
**`build 2a32427`**; the unit is in landscape, on the launcher, with a match
saved at Officer turn 4. Fleet → RESUME puts the screen in question on the
display in two taps. (This was not true of the first pass, and that is what
this round is about — see "The rule, and what was actually wrong".)

**The Battle screen across the page has been redesigned** after the first
arrangement was not accepted. The rule it is now built round is DS §28.6:
*normal landscape Battle gameplay fits in one viewport and requires no
scrolling.*

```text
TALL   portrait, 528 x 1060: the v0.0.10 layout, boards at 48 px cells.
WIDE   landscape, 1192 x 386 once the foot clears the rounded corners:
       board 382 x 382 at 34 px a cell; beside it TARGET, the readout, and
       YOUR WATERS, your own board at 20 px; FIRE 790 x 95 across the foot
       of both, its foot level with the board's.
```

Fleet is the seventh app given a landscape layout under DS §21.3, after
Calendar (§27). The shape is chosen from the body the shell gives the app and
from nothing else; `tests/fleet_lint.sh` refuses any reference to the
orientation in the app. What is new against the six before it is that the
thing being laid out is a **board of touch targets**, and the height of a
landscape body makes them smaller than Fleet's already-approved deviation D1.

## What the owner is being asked

1. **Are ~34 px board cells comfortable and reliable under a normal thumb?**
   Everything that can be checked without a hand has been: the mapping is
   exact, the whole square belongs to its cell, and a mis-tap costs nothing
   because a tap only aims. Whether the square is big enough for a thumb is
   not a thing a test can answer. DS §28.2 stays PROVISIONAL until it is.
2. **Does the whole Battle screen read naturally at a glance?** Board at the
   left, the readout beside it, your own waters beyond that, FIRE across the
   foot of both.

Nothing else is outstanding. There is no third question about portrait: Deploy,
Battle and Result are unchanged below the status bar apart from six rows at
the foot in Outdoor, and Command's only change is nine pixels; both are
measured in §28.4.

## The rule, and what was actually wrong

The landscape Battle screen was rejected because the player must scroll during
normal play. **That is exactly right, and it is my fault, in a way the first
pass of this gate did not catch: the unit was never running this branch when
it was looked at.**

The first pass validated Fleet on the unit and then moved the unit to the
Radar branch's build to validate that, and left it there — a line at the foot
of this sheet said so, which was not nearly enough. The Radar branch is built
from master, and **master's Fleet has no landscape layout at all**. Opened in a
landscape body it lays out its portrait stack, which the frame then scrolls.

The unit's own log records what happened after this gate was written:

```text
03:09:25  rotation mode automatic stored: rotation 0     <- left as found
03:25:31  rotation mode landscape stored: rotation 270
03:25:32  start version=0.0.10 build=5881704             <- the Radar build
03:25:37  fleet: resumable match ... Officer turn 1
03:25:37  open app fleet
03:25:45  close app fleet                                <- eight seconds
```

Captured from the panel on that build, before anything here was installed:

| Capture | What is on the display |
| --- | --- |
| `masterfleet-landscape-command.png` | Command: the first panel's caption already cut off at the top, the fleet list running off the bottom, no RESUME and no DEPLOY FLEET |
| `masterfleet-landscape-command-scrolled.png` | the same screen after four drags — RESUME and DEPLOY FLEET are down there |
| `masterfleet-landscape-battle.png` | **Battle: rows 1 to 8 of the board and nothing else.** No readout, no FIRE, no own waters, no rows 9 and 10 |

Eight seconds was plenty. That is a screen you cannot play without scrolling,
and it is the screen that was on the unit.

### What was nevertheless wrong with the branch

The branch as it stood did **not** scroll, and that was measured before
anything was changed: on the reference body, in Normal and Outdoor, with the
unit's corners and with square ones, fresh, after seventy played turns, and
with the display turned under a match in progress, every container on the
landscape Battle screen reported a scroll extent of zero and every object lay
inside the body.

But it **looked** as though it continued past the bottom of the display, which
is a real fault of the same kind and one no scroll-extent measurement catches.
Both panels were stretched to fill the body's height, so each was a bordered
box with a large empty region under its content, ending exactly at the edge of
the body — the shape an eye reads as a view that has been cut off. And the
board, at 382 px, was the smallest of three near-equal columns, so nothing on
the screen read as the primary surface.

So the screen is rebuilt around the requirement instead of merely satisfying
it by accident, and the promise is made a property of the build rather than of
one arrangement:

- **The layout is derived from the rule.** No panel is stretched to the edge of
  the body: each is exactly as tall as what it holds, and the two are given the
  same height so that they close on the same line, well clear of the foot. The
  room left over goes to FIRE, which is now **790 x 95** rather than 385 x 64 —
  2.3 times the area — spanning the whole region beside the board with its foot
  level with the board's.
- **Weight follows priority.** The board is the largest object on the screen by
  area; your own waters are the smallest of the three and drop from 26 px cells
  to the same 20 px they have in portrait; the readout, which holds the lines
  of text, takes the width. The line reporting the exchange moves out of the
  far column and in beside the readout, where the player is already looking.
- **It is held by a test that plays matches.** `check_battle_never_scrolls()`
  checks that nothing under the body can be scrolled, that nothing has been
  scrolled, that each of the eight things a turn needs is inside the body, that
  all 100 squares of the board are inside it, and that no object anywhere on
  the screen lies outside it. It runs at every landscape check and after
  **every single turn** of four matches played out — Normal and Outdoor, the
  unit's corners and square ones.
- **And by the lint.** `fleet_lint.sh` refuses a Battle screen that turns
  scrolling on or scrolls anything into view, and refuses a test file that has
  stopped making those four matches.

### The arrangements that were tried, and the arithmetic that decided it

The body across the page is 1192 x 386. The board is square and height-bound
at 382, so it uses a third of the width and **810 px are left over** — the
shape has width to spare and no height at all. Everything below follows from
that one fact.

Writing `C` for the height available beside the board (386 less the 9 px the
panel captions need above their top borders = 377), and taking the readout's
worst case at 192 px (Outdoor, with both its lines wrapped):

| Arrangement | Why not |
| --- | --- |
| **One right-hand region, stacked**: readout, your own waters, FIRE, all 790 wide | `192 + 22 + (40 + own) + 22 + 64 ≤ 377` needs `own ≤ 37` — a 3 px cell. A 10 x 10 board cannot go in a single column beside a readout and a button. **Arithmetically impossible**, which is what forces two columns |
| **Three bands**: readout, then your waters beside the log, then FIRE | the same sum with the log alongside rather than under: `own ≤ 37` still |
| **Two columns, both stretched** (what was there) | fits, and does not scroll — but both panels end at the edge of the body, which reads as a view cut off, and the board is the smallest of three near-equal columns |
| **Two columns, FIRE at the foot of the near one** (what was there, unstretched) | leaves two ragged blocks of background under the panels, one in each column |
| **Two columns, content-height, FIRE across the foot of both** | **chosen.** `cols = 40 + 218 = 258`, so FIRE gets `377 - 258 - 22 = 97`. Both panels close on one line; the room over is spent on FIRE; the board is the largest object on the screen |

The last row is why your own board drops to 20 px: it is the tallest content
beside the board and so sets the height of both panels, and every pixel it
gives up goes to FIRE. At 26 px it made its panel 318 tall, FIRE 37, and the
screen read as two boards of equal standing. At 20 px — the size portrait
already uses — it is 218, plainly the lesser of the two, and FIRE is 95.

### The cell size, recalculated rather than assumed

34 px is not a budget, it is the ceiling. The board is square and ten cells
across, so its side is `24 + 10c + 9×2`, and the body's 386 px of height is the
only thing that limits it: `c ≤ 34.4`. Width is not the constraint — 810 px of
it are spare. The three ways to buy a larger cell all cost more than they
return: the 24 px caption gutter buys 36 px and costs the A–J and 1–10 labels
the readout's "F6" is read against; a 16 px gutter buys 35 px and would change
the one piece of arithmetic the drawing and the hit test share; and taking the
10 px foot clearance would mean shifting the board sideways out of the corner
squares, which §22.2 forbids by name. **35 px is used when the panel has square
corners**, which is where that figure in the tests comes from.

## Host validation

From the branch tip, and from master `7d0d1ea` for comparison.

| Check | Result |
| --- | --- |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,787 ok, 0 FAIL, 0 warnings (master: 3,772; the 15 new `fleet_lint` checks) |
| SDL simulator build | rc 0, 1 warning — a format-truncation in `tests/clock_app_test.c` that master carries too, and the only one in the build |
| 21 shell and UI test scripts | 536 ok, 0 FAIL, every script rc 0 (master: 525; `fleet_shell_test.sh` 23 → 34) |
| `fleet_app_test` | **537 checks, 0 failures** (280 before this redesign) |
| `fleet_lint.sh` | 20 checks, 0 failures (13 before; master: 5) |
| `fleet_rules_test` / `fleet_ai_test` / `fleet_rng_test` / `fleet_save_test` / `fleet_theme_test` | unchanged and green: the engine is byte-identical to master |
| `calc_app_test`, `notes_app_test`, `settings_app_test`, `system_app_test`, `clock_app_test`, `cal_app_test` | 456 / 1,138 / 636 / 833 / 778 / 1,317 checks, 0 failures: the other apps unaffected |
| `display_geometry_shell_test.sh` | 69 ok, 0 FAIL: all eleven apps still open in landscape |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 4 warnings, all `vendor/ggwave`, as in the release; 0 first-party |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; `Doors 0.0.10 build f17ca59`; all 14 Fleet objects compiled |

### Mutation testing

**56 mutations, 55 caught** — 36 of the shape rule, the board's geometry, the
tap conversion and the other three screens, and **20 of the revised Battle
layout and the rule it exists to keep**, run as their own suite.

The one that is not caught: the app compares the body's box (and the safe-area
insets it implies) before laying out again, and disabling that comparison
changes nothing observable — LVGL reports a size change only when the size
changed, so the comparison never gets the chance to refuse a pass. It is kept
as the same defensive guard Calculator, Notes and Clock carry.

Of the 20 on the revised Battle screen, these are the ones worth naming: FIRE
left in the readout column, or never put back there down the page, or set
beside the panels instead of under them, or only as wide as its label, or not
taking the room the panels leave; the columns stretched to the foot of the
body, your own waters stretched to fill its column, either panel ending short
of its neighbour; your own board grown until it sets the height; the exchange
left under your own board; **a screen made a scroller**; and **the frame left
scrollable across the page**. Two of the twenty were gaps in `fleet_app_test`
when they were first run — a check that compared the columns where it should
have compared the panels, and a redundant height setting that made a mutation
equivalent — and both are closed, in the test and in the code respectively,
not argued away.

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

Five of the 36 were gaps in `fleet_app_test` that the first mutation run
found; they are closed too.

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
- the save written either way up, the same file, and offered again;
- **the no-scroll rule, after every turn of four matches played out across the
  page** — Normal and Outdoor, the unit's corners and square ones: nothing
  under the body can be scrolled, nothing has been scrolled, each of the eight
  things a turn needs is inside the body, all 100 squares of the board are
  inside it, and no object anywhere on the screen is outside it;
- that only the board and FIRE take a tap: a tap on the readout, on your own
  waters or in the gutter between the board and them fires nothing and does not
  move the crosshair, so there is no hitbox on the screen that is not drawn.

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
| Portrait against the branch **before this redesign** | **16 of 20 pixel-identical.** The four that are not are all `battle_paced`, all the same 56 x 56 box, and the same build renders them differently between two runs of its own — the flash again |
| Landscape against the branch before the redesign | Only the two Battle states differ, and only to the right of the board (x ≥ 422): Command, Deploy and Result are untouched, and so is the board itself |

Foot corner squares in **portrait** on Battle are not clear, on this branch or
on master: the stack is longer than the body and, while it is scrolled,
content passes through the last few pixels as scrolled content does. What the
clearance guarantees is that the end of the stack clears the corners when the
scroll reaches the end, which `fleet_app_test` checks directly. On master the
same content reached 20 px further in.

## Unit A: remote validation

Everything below ran with nobody at the unit. The console (COM9, 115200) was
used to install a bench SSH key — the console is already a root shell, and the
adapter's DTR and RTS must stay de-asserted or the board resets — after which
Ethernet carried the transfers and the captures; serial alone dropped about a
third of the bytes of a 6.8 kB payload, which is not a link a 953 kB binary or
two dozen framebuffer grabs can use. **The key is removed at the end of this
work** (see "As left").

Taps and drags are written as input events into `/dev/input/event1`, the touch
controller's own node, with raw coordinates found by inverting the calibration
the shell logged for the rotation in force. Each one therefore goes through the
kernel input core, LVGL's evdev driver, the shell's swap and calibration, and
whatever is under the point. Positions come from the layout's own arithmetic
worked out again on the device (`fgate/fleet_remote.py`), never from a
measurement of the picture: if the app is laid out differently from what it
promises, a tap lands somewhere else and the capture shows it.

| Step | Evidence | Result |
| --- | --- | --- |
| **What was on the unit before** | build `5881704` — the Radar branch, i.e. master's Fleet — rotation **landscape**, a resumable match waiting. Fleet opened on it: Command with its first caption cut off and no way on visible, and **Battle showing rows 1–8 of the board and nothing else** | recorded; this is the screen that was rejected |
| Transfer | `doors-shell` from the riscv64 DRM build of `2a32427`, stripped, **953,008 B, md5 `5d535ccd…`**, equal on both ends | PASS |
| Install | only `S90doors-shell` stopped and started; the shell answers **`build 2a32427`**; 1 doors-shell, 0 crashloop, 0 restarts; `/root/doors-shell.rollback` (the `99b2374` shell the unit was found with) untouched | PASS |
| Geometry the app actually has | `shape wide cell 34 span 382 board at (20, 152) frame (20, 152, 1211, 547) foot 10`, and `fire at (816, 490)` — the centre of a button spanning x 422–1211, y 443–537. The arithmetic `fleet_app_test` pins on the host, arrived at independently on the unit | PASS |
| **The whole screen, on the panel** | Battle after RESUME: the entire 10 x 10 board with its A–J and 1–10 labels, the TARGET readout, YOUR WATERS with your own board, and FIRE across the foot — all of it, nothing cut (`new-battle.png`) | **PASS** |
| **Proof that it does not scroll (1)** | Two captures with no input differ only inside the board, x 51–401 — that is the tracer animation, and it is the noise floor | recorded |
| **Proof that it does not scroll (2)** | Seven drags over the readout, your own waters and FIRE — with nothing aimed, so FIRE is disabled and none of them can change anything. The panel afterwards differs **only** at x 215–368, inside the board: the animation again. **Nothing in the region beside the board moved by one pixel** | **PASS** |
| **Proof that it does not scroll (3)** | Four drags over the board itself, then the furniture that carries no game state compared rectangle by rectangle: the panel captions and top borders **identical**, the panels' bottom borders **identical**. FIRE's edges differ in colour only, at the same y 443 and y 537 they were at — it armed, because a drag aimed a square | **PASS** |
| **34 px cells, on the panel** | Aimed at the four corners of the board and its middle: the readout named **A1, J1, A10, J10, E5**, each the cell asked for (`new-readouts.png`) | **PASS** |
| **The whole square** | All four corners of cell C7 and its centre tapped: the readout named **C7** every time | **PASS** |
| **Nothing else is a target** | Tapped inside the readout, inside YOUR WATERS, in the gutter between the board and the panels, and in the gap between the panels and FIRE: the readout still reads C7 · Ready to fire, the same log line, nothing fired. There is no hitbox on this screen that is not drawn | **PASS** |
| A match played out | 80 aim-and-fire pairs, 160 injected taps, to the end of the engagement; Result showed **`Enemy fleet destroyed`**, 81 rounds, 1 of 5 afloat, 21 % / 20 % accuracy, both accounts and both ways on visible | PASS |
| Deploy across the page | NEW ENGAGEMENT → Deploy; AUTO placed the whole fleet and CONFIRM DEPLOYMENT, at the foot of its column, started the engagement | PASS |
| **Save and resume** | Three turns played, the app left for the launcher, Fleet reopened: `fleet: resumable match … Officer turn 4`, RESUME taken, and the board came back with all three shots and the enemy's replies on your own waters (`resume-battle.png`) | **PASS** |
| **Outdoor type** | `shell.theme mode=outdoor`, Battle reopened and a square aimed: the whole screen still fits — board, readout (`E5 · Ready to fire.`), your own waters, FIRE armed across the foot (`new-battle-outdoor.png`). Restored to Normal afterwards | **PASS** |
| **Portrait on this build** | Turned to portrait: `shape tall cell 48 span 522`; Command is the v0.0.10 stack with RESUME inside the SAVED ENGAGEMENT panel, and Battle is the v0.0.10 stack with FIRE below TARGET and the log under your own board — both objects back where they belong (`portrait-command.png`, `portrait-battle.png`) | **PASS** |
| Foot corners in the captures | all eight landscape captures of this build — Command, Battle, Battle in Outdoor, Result, the resumed match and the two drag captures — have clear bottom corner squares | PASS |
| Resources | VmRSS 12,672 kB before the 80-shot match, 12,800 kB after it and two more matches, 12,416 kB at rest | PASS |
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

Unit A carries **this branch**: `doors shell info` answers
**`build 2a32427`**, md5 `5d535ccd…`. It is in **landscape**, exactly as it was
found at the start of this work, on the launcher, theme carbon, Normal,
brightness 100, Wi-Fi **off**, 1 doors-shell, 0 crashloop, 0 crash reports,
`shell.log` 0 ERROR 0 WARN. **A match is saved and waiting at Officer turn 4**,
so Fleet → RESUME puts the screen in question on the display in two taps.

That is deliberate, and it is the correction to the mistake described at the
top of this sheet: the unit is left on the branch whose physical check is
outstanding, and this sheet says which build that is in its first lines.

The Radar gate sheet (`docs/hardware/RADAR_LANDSCAPE_GATE.md`, on its own
branch, not edited here) says the unit is left on `5881704`. **It no longer
is.** To put Radar's landscape back on the unit, build
`feature/radar-landscape` and install it the same way.

**Rollback**: stop `S90doors-shell`, copy `/root/doors-shell.rollback` over
`/usr/bin/doors-shell`, start it again. That copy is the `99b2374` shell the
unit was originally found with, and it is untouched.

The bench SSH key is removed: `/root/.ssh` no longer exists, and the unit
refuses the key. Reinstalling it takes about a minute over the COM9 console
(`C:\K230-ml\serial.ps1`).

Captures and logs: `out/fleet-redesign/hwgate-unitA/` for this pass — the
`masterfleet-landscape-*.png` ones are what was on the unit before it, and the
`new-*`, `aim-*`, `corner-C7-*`, `ns-*`, `bd-*`, `resume-*` and `portrait-*`
ones are this build. The first pass is in
`out/fleet-landscape-a849784/hwgate-unitA/`. Both are outside the repository.
