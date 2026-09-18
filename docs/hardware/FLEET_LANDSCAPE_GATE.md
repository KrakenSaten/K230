# PocketFleet in landscape — validation gate

Branch `feature/fleet-landscape`, from master `7d0d1ea` (VERSION 0.0.10,
unchanged). DS Amendment L (§28) is **PROPOSED**.

**Verdict: host and simulator PASS; unit A remote PASS. Two questions are
left for the product owner, and both are about how it feels under a hand.**

**Unit A is running this branch.** `doors shell info` answers
**`build 0dd9ee1`**; the unit is in landscape, on the launcher, with a match
saved at Officer turn 4. Fleet → RESUME puts the screen in question on the
display in two taps. (This was not true of the first pass, and that is what
this round is about — see "The rule, and what was actually wrong".)

**The Battle screen across the page has been through two rounds of rework.**
First the arrangement, after the player was found to have to scroll: the rule
it is now built round is DS §28.6, *normal landscape Battle gameplay fits in
one viewport and requires no scrolling*. Then the board itself, after 34 x 34
cells were ruled too small under a thumb: there is no bigger square to be had
in this body, so the cell grew on the axis with room and aiming stopped
needing a precise touch at all (§28.2).

```text
TALL   portrait, 528 x 1060: the v0.0.10 layout, boards at 48 x 48 cells.
WIDE   landscape, 1192 x 386 once the foot clears the rounded corners:
       board 552 x 382 at 51 x 34 a cell; beside it TARGET - the readout and
       the four one-square nudges - and YOUR WATERS, your own board at 20 px;
       FIRE across the foot of both, its foot level with the board's.
```

Fleet is the seventh app given a landscape layout under DS §21.3, after
Calendar (§27). The shape is chosen from the body the shell gives the app and
from nothing else; `tests/fleet_lint.sh` refuses any reference to the
orientation in the app. What is new against the six before it is that the
thing being laid out is a **board of touch targets**, and the height of a
landscape body makes them smaller than Fleet's already-approved deviation D1.

## What the owner is being asked

1. **Is aiming comfortable now?** A 34 x 34 cell was rejected as too small
   under a thumb, and that was right. **It cannot be answered with a bigger
   row** - ten rows in a 386 px body can never exceed 38 px, and D1's 48 would
   need a 522 px body - so it is answered three ways at once, and the question
   is whether they add up:
   - the cell grows on the axis that has room: **51 x 34**, half as much again
     in area, and wider than the 48 px the tall shape draws;
   - **a press aims, and so does every moment of a drag**, so you land
     anywhere on the 552 x 382 board and slide, reading the square's name off
     the readout beside it rather than trying to hit it;
   - **four one-square nudges**, each a full 64 px target, so every square can
     be reached exactly without touching the board at all.
   DS §28.2 stays PROVISIONAL until a thumb has been on it.
2. **Does the whole Battle screen read naturally at a glance?** Board at the
   left, the readout and the nudges beside it, your own waters beyond that,
   FIRE across the foot of both.

Nothing else is outstanding. There is no third question about portrait: Deploy,
Battle and Result are unchanged below the status bar apart from six rows at
the foot in Outdoor, and Command's only change is nine pixels; both are
measured in §28.4.

## The cell, after it was rejected

The landscape board drew 34 x 34 cells and the product owner ruled they were
not comfortable enough under a normal thumb. The no-scroll layout was
accepted; only the geometry and the touch were sent back.

**There is no bigger square to be had.** The board is ten rows of cells with a
caption gutter and 2 px gaps, so its height is `24 + 10h + 9×2` and the body
across the page is 386 px: `h ≤ 34.4`. Spending the gutter and the gaps as
well - losing the A–J and 1–10 labels that the readout's "F6" is read
against - buys 38. Deviation D1's 48 px would need a **522 px** body. The
shell's status bar, the app header and the body's padding are not the app's to
take, and §28.6 forbids scrolling to find the rest. This is arithmetic, not a
judgement, and no arrangement of anything else on the screen moves it.

So the answer is not a bigger row. It is three changes that between them mean
**no square has to be hit exactly**:

| | Before | After |
| --- | --- | --- |
| Cell | 34 x 34 (1,156 px²) | **51 x 34 (1,734 px²)** - half as much again, and wider than the tall shape's 48 |
| Board | 382 x 382 | 552 x 382 |
| Aiming | a tap, which had to land on the right square | **a press or a drag**: the square under the finger is reported the whole way and named in the readout, so the aim is corrected by watching |
| Correcting | another tap, exactly as hard | **four one-square nudges**, each a full 64 px target, reaching every square |

The width is where the room was: the board used a third of the page and 810 px
were spare. The cap is **3:2** - past that a board of ten by ten stops reading
as a board, and 51 is already wider than the 48 px D1 was approved at.

The nudges are the part that makes deviation D1 honest at this size. D1 permits
a target smaller than §7's 64 px **because a mis-aim is correctable**; a
correction that is exactly as hard as the original aim is not a correction.
With no crosshair set the first nudge starts in the middle, so every square is
within five presses.

Nothing else about D1 moves: aiming still commits nothing, FIRE is still the
only thing that fires, and it is still armed only on a square that has not been
fired at. Deploy draws the same board, cell for cell, because a fleet is placed
on the squares the shots are later aimed at.

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
  three times the area — spanning the whole region beside the board with its foot
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

The body across the page is 1192 x 386. The board is height-bound at 382, so
it uses a third of the width and **810 px are left over** — the shape has width
to spare and no height at all. Everything below follows from that one fact:
the arrangement of what stands beside the board, and, once the board had been
sent back for being too fine, the decision to spend the spare width on the
cell itself.

Writing `C` for the height available beside the board (386 less the 9 px the
panel captions need above their top borders = 377), and taking the readout's
worst case at 192 px (Outdoor, with both its lines wrapped):

| Arrangement | Why not |
| --- | --- |
| **One right-hand region, stacked**: readout, your own waters, FIRE, all 790 wide | `192 + 22 + (40 + own) + 22 + 64 ≤ 377` needs `own ≤ 37` — a 3 px cell. A 10 x 10 board cannot go in a single column beside a readout and a button. **Arithmetically impossible**, which is what forces two columns |
| **Three bands**: readout, then your waters beside the log, then FIRE | the same sum with the log alongside rather than under: `own ≤ 37` still |
| **Two columns, both stretched** (what was there) | fits, and does not scroll — but both panels end at the edge of the body, which reads as a view cut off, and the board is the smallest of three near-equal columns |
| **Two columns, FIRE at the foot of the near one** (what was there, unstretched) | leaves two ragged blocks of background under the panels, one in each column |
| **Two columns, content-height, FIRE across the foot of both** | **chosen.** Both panels close on one line; the room over is spent on FIRE; the board is the largest object on the screen. With the cell widened to 51 the board takes 552 and the two columns 620 of what is left |

The last row is why your own board drops to 20 px: it is the tallest content
beside the board and so sets the height of both panels, and every pixel it
gives up goes to FIRE. At 26 px it made its panel 318 tall, FIRE 37, and the
screen read as two boards of equal standing. At 20 px — the size portrait
already uses — it is 218, plainly the lesser of the two, and FIRE is 95.

### The row height, and why nothing else was taken from it

The 34 px row is the ceiling, not a budget, and the three ways of buying more
all cost more than they return: the 24 px caption gutter buys 36 and costs the
A–J and 1–10 labels; a 16 px gutter buys 35 and would change the one piece of
arithmetic the drawing and the hit test share; and taking the 10 px foot
clearance would mean shifting the board sideways out of the corner squares,
which §22.2 forbids by name. With square corners the body is 396 px and the
board draws **52 x 35**, which is where that second figure in the tests comes
from.

The width is a different matter, and that is where the room was: see "The
cell, after it was rejected" above.

## Host validation

From the branch tip, and from master `7d0d1ea` for comparison.

| Check | Result |
| --- | --- |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,793 ok, 0 FAIL, 0 warnings (master: 3,772; the 21 new `fleet_lint` checks) |
| SDL simulator build | rc 0, 1 warning — a format-truncation in `tests/clock_app_test.c` that master carries too, and the only one in the build |
| 21 shell and UI test scripts | 536 ok, 0 FAIL, every script rc 0 (master: 525; `fleet_shell_test.sh` 23 → 34) |
| `fleet_app_test` | **605 checks, 0 failures** (280 before this work began) |
| `fleet_lint.sh` | 26 checks, 0 failures (13 before; master: 5) |
| `fleet_rules_test` / `fleet_ai_test` / `fleet_rng_test` / `fleet_save_test` / `fleet_theme_test` | unchanged and green: the engine is byte-identical to master |
| `calc_app_test`, `notes_app_test`, `settings_app_test`, `system_app_test`, `clock_app_test`, `cal_app_test` | 456 / 1,138 / 636 / 833 / 778 / 1,317 checks, 0 failures: the other apps unaffected |
| `display_geometry_shell_test.sh` | 69 ok, 0 FAIL: all eleven apps still open in landscape |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 4 warnings, all `vendor/ggwave`, as in the release; 0 first-party |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; `Doors 0.0.10 build f17ca59`; all 14 Fleet objects compiled |

### Mutation testing

**71 mutations, 70 caught** — 33 of the shape rule, the board's geometry, the
tap conversion and the other three screens, and **38 of the Battle layout, the
two-axis board and the ways of aiming**, run as their own suite.

The one that is not caught: the app compares the body's box (and the safe-area
insets it implies) before laying out again, and disabling that comparison
changes nothing observable — LVGL reports a size change only when the size
changed, so the comparison never gets the chance to refuse a pass. It is kept
as the same defensive guard Calculator, Notes and Clock carry.

Of the 38 on the Battle screen, these are the ones worth naming.

On the layout: FIRE left in the readout column, or never put back there down
the page, or set beside the panels instead of under them, or only as wide as
its label, or not taking the room the panels leave; the columns stretched to
the foot of the body, your own waters stretched to fill its column, either
panel ending short of its neighbour; the exchange left under your own board;
**a screen made a scroller**; and **the frame left scrollable across the
page**.

On the board: the cell never widened into the room it has; a cell allowed to
be narrower than it is tall; the width rule never consulted; the target board
left square; **Deploy disagreeing with Battle about where a square is**; **a
cell drawn narrower than it is hit**; and the hit test reading the wrong axis,
each way round.

On aiming: **the aim no longer following the finger**; the first nudge
starting in a corner instead of the middle; a nudge wrapping round the board
instead of stopping at the edge; up and down, and left and right, the wrong
way round; a nudge shorter or narrower than a finger; a nudge that moves
nothing; and the nudges hidden in the shape that needs them, or shown in the
shape that does not.

Four were gaps in `fleet_app_test` when they were first run, and all four are
closed rather than argued away: a check that compared the columns where it
should have compared the panels; a redundant height setting that made a
mutation equivalent; and - the one worth the most - **nothing at all compared
what the grid draws with what it hits**. The grid now answers for a cell's
rectangle (`fleet_grid_cell_rect`) and the test holds that against its own
restatement of the layout's arithmetic, for all 100 cells at every board size.
One mutation was withdrawn rather than counted: with the drag registered, a
press that aims on release instead cannot be told apart by any host test,
because an input read arrives before the finger lifts.

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
  corners, at 48 x 48, 52 x 35 and 51 x 34** — 1,500 taps — plus the two-pixel
  gaps between cells, the caption gutter and points just off the board;
- **what is drawn compared with what is hit**, cell by cell at all three
  sizes: the rectangle the grid says a square occupies against the layout's
  own arithmetic;
- **aiming without a precise touch**: a drag across the board ending aimed at
  the square it ended on and firing nothing; the readout following the finger
  mid-drag and staying put when it lifts; each of the four nudges moving one
  square the right way; the edges clamping rather than wrapping; and the first
  nudge starting in the middle with nothing aimed;
- aim-then-confirm: aiming never fires, however it is done; FIRE fires exactly
  one shot and only on a square not already fired at; your own waters are not a
  target at all;
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
| Portrait against the build before the **layout** was reworked | **16 of 20 pixel-identical.** The four that are not are all `battle_paced`, all the same 56 x 56 box, and the same build renders them differently between two runs of its own — the flash again |
| Portrait against the build before the **cell** was reworked | **17 of 20 pixel-identical**, the other three that same flash. Widening the cell and adding the nudges changed nothing down the page: both are the wide shape's alone |
| Landscape | Battle and Deploy both draw the wider board; Command and Result are untouched |

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
| **What was on the unit at the start of this work** | build `5881704` — the Radar branch, i.e. master's Fleet — rotation **landscape**. Fleet opened on it: Command with its first caption cut off and no way on visible, and **Battle showing rows 1–8 of the board and nothing else** | recorded; this is the screen the scrolling complaint was about |
| Transfer | `doors-shell` from the riscv64 DRM build of `0dd9ee1`, stripped, **957,104 B, md5 `ea00f227…`**, equal on both ends | PASS |
| Install | only `S90doors-shell` stopped and started; the shell answers **`build 0dd9ee1`**; 1 doors-shell, 0 crashloop, 0 restarts; `/root/doors-shell.rollback` (the `99b2374` shell the unit was found with) untouched | PASS |
| Geometry the app actually has | `shape wide cell 51 x 34 span 552 x 382 board at (20, 152) frame (20, 152, 1211, 547) foot 10`, with `fire at (901, 490)` and the four nudges at `(647, 367) (723, 367) (799, 367) (875, 367)`. The bench tool works all of that out from the app's own rule rather than measuring the picture, so a tap that lands is itself the check | PASS |
| **The whole screen, on the panel** | Battle after RESUME: the entire 10 x 10 board with its A–J and 1–10 labels, the TARGET readout, YOUR WATERS with your own board, and FIRE across the foot — all of it, nothing cut (`new-battle.png`) | **PASS** |
| **Proof that it does not scroll (1)** | Two captures with no input differ only inside the board, x 51–401 — that is the tracer animation, and it is the noise floor | recorded |
| **Proof that it does not scroll (2)** | Seven drags over the readout, the nudges, your own waters and FIRE. Everything that differs afterwards is inside the board (x ≤ 571) — the tracer animation. **Nothing in the region beside the board moved by one pixel** | **PASS** |
| **Proof that it does not scroll (3)** | And compared rectangle by rectangle, on the furniture that carries no game state: the panel captions and top borders **identical**; the panels' bottom borders **identical**; **the four nudges identical**; **FIRE identical edge to edge**; the board's A–J labels identical | **PASS** |
| **51 x 34 cells, on the panel** | Aimed at the four corners of the board and its middle: the readout named **A1, J1, A10, J10, E5**, each the cell asked for | **PASS** |
| **The whole square** | All four corners of cell C7 and its centre tapped: the readout named **C7** every time — the corners of a rectangle now, not a square | **PASS** |
| **The four nudges, on the panel** | From C7: left → **B7**, up → **B6**, down → **B7**, right → **C7**. Each moved exactly one square, the right way, and none of them fired | **PASS** |
| **Aiming by drag, on the panel** | A drag from A1's square to G8's, injected as a press, sixteen moves and a release: the readout ended at **G8**. Dragged back from J10 to A1: **A1**. The log line was unchanged throughout, so neither fired | **PASS** |
| **Nothing else is a target** | Tapped inside the readout, inside YOUR WATERS, in the gutter between the board and the panels, and in the gap between the panels and FIRE: the readout still reads C7 · Ready to fire, the same log line, nothing fired. There is no hitbox on this screen that is not drawn | **PASS** |
| A match played out | 53 aim-and-fire pairs to the end of the engagement; Result showed **`Fleet lost`**, 59 rounds, 0 of 5 afloat, 24 % / 29 % accuracy, both accounts and both ways on visible | PASS |
| Deploy across the page | NEW ENGAGEMENT → Deploy; AUTO placed the whole fleet and CONFIRM DEPLOYMENT, at the foot of its column, started the engagement | PASS |
| **Save and resume** | Three turns played, the app left for the launcher, Fleet reopened: `fleet: resumable match … Officer turn 4`, RESUME taken, and the board came back with all three shots and the enemy's replies on your own waters (`resume-battle.png`) | **PASS** |
| **Outdoor type** | `shell.theme mode=outdoor`, a square aimed: the whole screen still fits — board, readout (`E5 · Ready to fire.`), the four nudges, your own waters, FIRE armed across the foot (`f-outdoor.png`). Restored to Normal afterwards | **PASS** |
| **Portrait on this build** | Turned to portrait: the device derives `shape tall cell 48 x 48 span 522 x 522` — square again — and Battle is the v0.0.10 stack: board, TARGET, FIRE, YOUR WATERS with the log, **and no nudges at all** (`f-portrait.png`) | **PASS** |
| Foot corners in the captures | all eight landscape captures of this build — Command, Battle, Battle in Outdoor, Result, the resumed match and the two drag captures — have clear bottom corner squares | PASS |
| Resources | VmRSS 12,800 kB before the match and 12,288 kB at rest after it and another one | PASS |
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
**`build 0dd9ee1`**, md5 `ea00f227…`. It is in **landscape**, exactly as it was
found at the start of this work, on the launcher, theme carbon, Normal,
brightness 100, Wi-Fi **off**, 1 doors-shell, 0 crashloop, 0 crash reports,
`shell.log` 0 ERROR 0 WARN. **A match is saved and waiting at Officer turn 4**,
so Fleet → RESUME puts the screen in question on the display in two taps —
three shots already played, so the board has something on it.

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

Captures and logs: `out/fleet-redesign/hwgate-unitA/`. The
`masterfleet-landscape-*.png` ones are what was on the unit when this work
started; `new-*`, `aim-*`, `corner-C7-*`, `ns-*`, `bd-*`, `resume-*` and
`portrait-*` are the no-scroll layout at 34 x 34; and **`f-*` are this build**
— `f-battle`, `f-step_left`/`_up`/`_down`/`_right`, `f-drag`, `f-drag2`,
`f-ns-before`/`f-ns-after`, `f-result`, `f-resume`, `f-outdoor`, `f-portrait`.
The very first pass is in `out/fleet-landscape-a849784/hwgate-unitA/`. All are
outside the repository.
