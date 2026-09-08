# PocketTimber

A tabletop balancing game for PocketOS: a tower of wooden blocks on a green
baize table, taken apart one block at a time and stacked back on top until
it falls. Local single player, a two-to-five minute run.

PocketTimber is a game. It models no hardware, reads no sensor and makes no
claim beyond the table. Every block is invented by the engine from a
recorded seed.

The design basis is the cold design review of 2026-09-06 (GO WITH MAJOR
CHANGES): a deterministic read-and-pull game with decoupled controls, no
per-pull dice, and a collapse the player can always explain.

Status: **PRE-HARDWARE COMPLETE** (P1 to P7 accepted by the product owner
and reviewed, 2026-09-08) on branch `pockettimber-engine`. Not merged. The
app runs in the SDL simulator with placeholder blocks; nothing has run on
hardware and nothing has been touched by a real finger. Master and the
golden bring-up image are untouched by this work.

## Isolation rule

This branch exists under a pre-hardware isolation rule: master is frozen,
the golden K230 image (`out/k230`, SHA-256 `bafed837…`) is not rebuilt,
replaced or altered, and nothing here merges before the first physical
bring-up. Every milestone is one focused commit on this branch.

## Layers

```text
apps/timber/engine/   vocabulary, RNG, tower state and generation, the pull
                      model, the stability model, scoring, the collapse
                      choreography, run rules, the replay log. Pure C, no
                      LVGL, no I/O, no floating point, no platform entropy.
apps/timber/ui/       timber_view: the projection, drawing order, picking
                      and the sense of the pull track, LVGL-free and tested
                      headless. timber_table: the custom-drawn tower.
                      timber_screens: the table and result screens on
                      PocketUI role styles.
apps/timber/timber_app.c   the shell app entry, screen ownership, the clock.
```

There is no store: the record file is D2, pending, so the best score lives
for the session. The dependency runs one way (app to ui to engine) and the
engine never calls up. The engine and the view model are built by the root
`Makefile` so they are unit tested natively, and again by
`ui/shell/CMakeLists.txt` into the shell binary. Registration is the two
additive edits PocketFleet was approved for: one line in `ui/shell/shell.c`
and the sources in `ui/shell/CMakeLists.txt`.

`tests/timber_lint.sh` states the structure as an executable rule: the
engine contains no LVGL, does no I/O, uses no floating point and calls no
libm, and takes no entropy from the platform; `timber_store.c` is the only
file in the app that touches the filesystem. It is a text search, so
comments in these files avoid the names it looks for.

## Geometry

The tower stands on a square footprint three block widths on a side. One
block width is `TIMBER_UNIT` (256), so every position is Q8.8 fixed point;
a block is three units long and 0.6 units tall. Layer 0 rests on the felt,
even layers run along x and odd layers along y. A block's extraction is its
displacement along its own axis, signed: positive toward +x or +y, the face
the player looks at.

Block ids are stable for the whole run. The tower keeps a grid of layers
and slots naming block ids and an array of blocks each naming its own cell;
`timber_tower_validate()` checks that the two agree and refuses every state
the rules could never produce, which is how "no impossible tower state" is
a test rather than a hope.

## Rules so far

- A block may be pulled only from below the highest completed layer. That
  layer and any incomplete layer above it are locked. The bottom layer is
  pullable; what makes a low block dangerous is the stability model (P3),
  not a rule.
- A block in hand goes to the top layer while it is incomplete, else to the
  layer above it.
- Selecting a block does nothing to the tower. From the moment a block is
  part way out the selection is locked to it, and pushing it all the way
  back unlocks it; while a block is in hand nothing can be selected. A
  second tap on the selected block changes nothing.
- Two TESTs per turn (`TIMBER_TESTS_PER_TURN`). A TEST reveals the selected
  block's class, nudges the tower a little, and is sticky: a tested block
  answers again for free. It is refused while a block is part way out.
- The pull is judged per tick (below). A block that reaches four fifths of
  its length out, either way, slips free into the hand, is scored as it was
  at that moment, and the turn moves on to placing it.
- Placing the block in hand on top ends the turn: nothing selected, two
  fresh tests. The block is reseated loose, the tower takes a knock of
  0.25, and a block placed off centre nudges the lean 0.005 widths per
  layer toward that side, so placing against the lean is the correct play;
  the two sides of a completed layer cancel whatever order they went on
  in, so the nudge is a correction that lasts until the layer is whole.
  (The review of 2026-09-08 found the completing placement had been
  exempt, which made the net nudge depend on the order; fixed to match
  this rule.) Completing a layer pays a bonus and unlocks the layer below
  it.
- Nothing can be done to a falling tower: every act is refused once the
  run is collapsing, and the score is kept.
- The summit: a placement that completes the top of a tower already at
  `TIMBER_LAYERS_MAX` ends the run standing, cause NONE, because the next
  block pulled would have nowhere to go. It is the only way a run ends
  without a collapse, and with the shift lean (D3) it is not reached in
  practice.

## Scoring

```text
points = base * depth * clean * untested * streak
```

| Factor | Value |
| --- | --- |
| base by class | FREE 60, EASY 100, FIRM 180, STUCK 300 |
| depth | +4 % per layer above the block |
| clean | ×1.5 for a pull with no jolt |
| untested | ×1.25 for a block never tested: the gamble |
| streak | +20 % per consecutive clean pull standing, capped at ×2.0 |
| completed layer | +250 |
| collapse | the run ends; nothing is taken away |

Applied in that order, each truncating, so a replay scores identically. A
FREE block sixteen layers down, clean and untested with no streak, is 183;
the best pull in the game, a STUCK bottom block clean and untested on a
full streak, is 1890. The piece card shows `timber_run_worth()`: what the
selected block would be worth pulled clean right now. A safe pull is always
available and always worth less.

## The trigger

After every tick and every act the run measures the tower. When the
hinge's effective margin is below zero the tower falls there, the run is
COLLAPSING, and the cause is read from what the player did last:

| Cause | When |
| --- | --- |
| PLACEMENT | within 6 ticks of a placement |
| JOLT | else within 10 ticks of a jolt |
| TIP | else the static margin itself is gone: a support drawn out |
| SWAY | else the tower was standing and the sway from a test or a shift tipped it |

The COLLAPSE event names the hinge layer and the cause, and the result
screen will say so. A collapse plays out until every block rests or for
`TIMBER_COLLAPSE_TICKS_MAX` (60 ticks, 2.4 s) at most, and then the run
is OVER with its score. All four causes are produced and asserted in
`tests/timber_rules_test.c`; the last support drawn out from under a
stack tips it at four tenths of the way, well before the block is free,
which is why a played tower never has an empty layer under another.

## The collapse

The choreography (`timber_collapse.c`) is an overlay the view reads; the
tower itself is frozen at the moment it went. The stump, every layer up to
and including the hinge, stays where it is, and so does a block in hand.
Every present block above the hinge:

1. **Tips** for `TIMBER_TIP_TICKS` (8, 0.32 s): each layer slides 3 units
   per tick per layer of height above the hinge along the axis the hinge
   gave way on, in its direction, and the stack drops 2 units a tick. A
   shear of sprites reads as a tilt up to about fifteen degrees, and
   nothing rotates.
2. **Breaks** into blocks that fly on their own: a horizontal speed of 24
   units a tick plus 2 per layer of height along the failing direction,
   a scatter of up to 25 either way on both axes, gravity of 9 units per
   tick per tick (`TIMBER_GRAVITY`), and a tumble through the six poses
   the view can draw every 2 to 4 ticks.
3. **Lands** on the felt, or on the stump's top if it is still over the
   footprint: the first landing keeps a fifth of its downward speed
   upward and half its horizontal speed, the second is a rest, in a pose
   drawn for it. Blocks pass through one another; the pile is a matter of
   drawing order. Each rest is a LAND event carrying the height it rests
   at.

The scatter, the tumble rate and the rest pose are drawn from the
generator when the collapse begins, four values per falling block in id
order: the second and last time the generator is consumed in a run. A
collapse is therefore a function of the tower's state and the generator's
state, the same state falls the same way twice, another seed scatters
differently, and the hinge, the direction and the number of blocks above
it are the variation. A collapse that has not ended by the ceiling is put
down where it is. Measured: 36 blocks from a hinge at layer 5 rest in 44
ticks (1.8 s); the review's 1.2 to 1.6 s target is P7 tuning of the bounce
and gravity once it can be seen.

Positions are Q8.8 widths on x and y, and on z one layer is
`TIMBER_BLOCK_HEIGHT` tall with z the block's underside, so a block from
layer j starts at `j * 154` and rests at 0 on the felt.

## Replay

Every way the player can touch the engine is one action: SELECT a block,
DESELECT, TEST, one tick of PULL travel, PLACE in a slot
(`timber_replay.h`). A live session drives the engine through
`timber_log_act()`, which records the action at the run's tick and applies
it in one step, so the log is exactly what happened, refusals included.
`timber_log_play()` builds a run from the log's seed and replays every
action at its tick; the result is the session that wrote the log, bit for
bit, collapse and score included. `tests/timber_replay_test.c` asserts it
for a scripted session and for fifteen whole modelled-player sessions, and
that one hard pull changed anywhere, or another seed, is another run.

The log is a struct in memory, `TIMBER_LOG_MAX` (6144) actions of eight
bytes; a run that drags for two of its five minutes fills about half of
it, and anything past the end is counted as dropped. Writing it anywhere is
the app's business and not part of v0.1; a bug report could carry one.

## Pacing

Three modelled players play five seeds each in `tests/timber_replay_test.c`.
The careful one reads tells, spends both tests, pulls the loosest block it
knows at four fifths of its limit, waits for the tower to settle, and
places against the lean. The ordinary one chooses the same way but never
waits and pulls everything at the EASY limit. The greedy one tests nothing,
goes for the deepest tight-looking block, pulls at the EASY limit and yanks
every fourth pull. None of them pulls the last support from under a stack.
A bot turn takes about two seconds where a person takes five to fifteen,
so the pull counts are the numbers to read:

| Player | Pulls | Layers reached | Cause | Bot time |
| --- | --- | --- | --- | --- |
| careful | 29 to 33, mean 31 | 28 to 29 | TIP, twice PLACEMENT | 67 to 76 s |
| ordinary | 28 to 33, mean 30 | 28 to 29 | TIP, twice PLACEMENT | 67 to 78 s |
| greedy | 4 to 9, mean 6 | 19 to 21 | JOLT | 5 to 10 s |

Measured after the review's placement-nudge fix; before it the careful
player lasted 38 to 48 pulls, because the placement that completed a
layer was exempt from the nudge. At a person's pace a careful run is
thirty pulls and four to seven minutes, a reckless one six pulls and a
minute; the review's two to five minutes lies between, where a person who
is neither will land. Two things the measurement showed, both for the
owner:

- **Impatience alone does not end a run.** The ordinary player is almost
  the careful one, because a loose block is always available: freshly
  placed blocks reseat loose, and pulling a loose block fast is not a
  jolt. Speed errors only bite on tight blocks, which a player who reads
  tells rarely has to touch. The game ends by greed or by the ramp, not by
  clumsiness.
- **Without the shift lean there is no ramp.** With `TIMBER_SHIFT_LEAN`
  and `TIMBER_SHIFT_LEAN_ACROSS` at zero, which is the model the review
  specified, both the careful and the ordinary player restacked every
  block to the 36-layer bound in 54 pulls and never fell; only the greedy
  one did. A stack of centre-only layers is statically sound in the model
  whatever its height, and a perfectly centred placement never costs
  anything. The physical mechanism the model lacked is that a stack
  settling onto fewer supports leaves a small permanent lean.

## Generation

`timber_tower_generate()` builds the canonical tower and then draws, in id
order, a seat, a grain variant and a micro-offset for every block; then one
guarantee per layer; then one tell roll per block. The stream a seed
produces is therefore a fixed shape, and a seed always builds the same
tower.

| Rule | Value |
| --- | --- |
| Seat | 0 wedged tight to 255 free, uniform; the bottom three layers draw below 200 |
| Guarantee | every layer keeps one block with seat 200 or looser, so there is always a playable pull in it |
| Micro-offset | 0.02 to 0.06 widths across the block's axis, either sign |
| Tell | seat 170+: 60 %; 85 to 169: 30 %; below 85: never |

A tell therefore means "probably loose" and a missing tell means nothing at
all. Measured over 400 seeds in `tests/timber_tower_test.c`: 60.6 %, 30.2 %
and 0.

A placed block is reseated from a hash of its id and the turn number rather
than from the generator, loose (seat 200 or more) because nothing rests on
it yet; it tightens as the tower is rebuilt over it.

## The pull

Tightness is the hidden seat scaled by the load above the block, in Q8.8:

```text
tightness = (255 - seat) / 255 * (0.5 + 0.5 * load / 51)
```

Load is the number of blocks above the block's layer, so a block's class
follows the tower. The class is tightness cut at 0.15, 0.35 and 0.65:

| Class | Max travel per tick | Stiction | At full load, seat |
| --- | --- | --- | --- |
| FREE | 110 (600 px/s) | none | 218 and looser |
| EASY | 73 (400 px/s) | none | 166 to 217 |
| FIRM | 40 (220 px/s) | 73 (16 px) | 91 to 165 |
| STUCK | 22 (120 px/s) | 128 (28 px) | 90 and tighter |

Travel is Q8.8 block widths per tick, converted by the view from finger
movement on the pull track (the P7 placeholder is 56 px per width). The
per-tick limits and the stiction distances live in `timber_tuning.h` and
are HARDWARE VALIDATION REQUIRED: they assume the GT9895 delivers drag
events evenly enough that a three-tick average is a fair judge.

A FIRM or STUCK block absorbs its stiction without moving, then lurches
2 px free with a small disturbance and a STICK event; changing the
selection forgets the grip. After that every block moves with the finger.
Travel averaged over three ticks above the class limit is a jolt,

```text
jolt = excess * (0.25 + tightness)
```

which adds `2.0 * jolt` of disturbance and leans the tower by
`0.01 * jolt` widths per layer along the block's axis in the direction of
the pull. A single fast sample is not a jolt; a slow steady pull on a STUCK
block never jolts and brings it out in about two and a half seconds.
Disturbance is capped at 4.0.

## Stability

This is a stability model, not a physics engine (`timber_stability.c`).
For every layer with something directly above it:

- **Contact.** The rectangles where the blocks of the layer that still
  carry load meet the blocks of the layer above, and their bounding box. A
  block carries load while at least a quarter of its footprint meets the
  layer above, so a block three quarters of the way out has already let
  go of the stack, a moment before it slips free at four fifths.
- **Stack.** The mass and centre of mass of every block above the layer,
  at its extracted position, with its micro-offset across its axis, and
  displaced by the lean: a block `j` layers up sits `lean * j` further
  over. Q8.8 positions, 64-bit sums.
- **Margin.** The smallest distance from that centre of mass to an edge of
  the contact box, on either axis, and which edge. A complete layer under a
  centred stack has 1.5 widths of it (`TIMBER_MARGIN_FULL`, 384); a layer
  missing one side block has 0.5 toward that side; a layer with only its
  centre block has 0.5 either way; a layer with one side block has −0.5,
  and the stack tips. A layer whose blocks have all let go has no margin
  at all (`TIMBER_MARGIN_NONE`).

The **hinge** is the layer with the least effective margin. A thinned
layer and the layer under it rest on the same block and share a margin,
so a tie goes to the layer with fewer blocks carrying the stack, then to
the lower one. The run refreshes the hinge and its margins after every
tick and every act; the stability meter is the effective margin as a share
of a full one.

**Lean** enters the stack's centre of mass and never decays. At 0.05
widths per layer the stack above the base sits 0.45 widths over and the
base has 0.87 widths of margin left; a high layer, with little stack above
it, feels almost none of it.

**Disturbance** decays by 225/256 per tick, so a knock is a twentieth of
itself a second later, and while it lasts the top of the tower sways
`0.10 * disturbance` widths on a 0.8 s period along the axis of the last
knock, starting in its direction. Each carrying layer feels the share of
that displacement given by the mean of a cantilever mode shape over the
stack above it, about a third at the base, peaking at two fifths around
layers four to five, and falling to a ninth just under the top. The
effective margin is the static one less that share, so a knocked tower can
fall a few ticks after the knock, at the top of its sway, and a sway
against the lean gives margin back for half a period.

Two more things the tower does:

- **Shift.** A block that was carrying the stack and lets go of it, out of
  the tower or merely three quarters out, drops the stack onto the blocks
  that are left: a disturbance from 0.25 to 1.0 with the load the block
  carried, and a SHIFT event. The slip that follows is not a second one.
  A shift also leaves the tower leaning a hair (D3): 0.006 widths per
  layer per full-load shift along the block's axis in the direction it
  was drawn, and 0.004 across it toward the neighbour the block's
  micro-offset points at, both scaled by the shift's impulse.
- **Creak.** When the hinge's static margin falls under a quarter of a
  width the tower creaks once, naming the hinge, and not again until the
  margin has come back over the line.

Every number above is asserted exactly in `tests/timber_stability_test.c`.

## Determinism

A run is reproduced by `(seed, the ordered list of player actions at their
ticks)`. One xorshift32 stream is consumed in exactly two places: when the
tower is built and when a collapse begins. Nothing the player does
moves the stream, so no outcome is ever a roll of the dice;
`tests/timber_rules_test.c` asserts the generator state after 120 ticks of
selecting, testing and pulling is the state it had at the start.

`timber_rng.c` duplicates `radar_rng.c` deliberately, for the reasons
recorded in `docs/apps/POCKETRADAR.md`; the regression vectors are
identical by construction.

## The app

Two screens. **TABLE** carries standby, the run and the collapse, because
the tower is the same thing before, during and after. **RESULT** is
separate because it shows different content. There is no pause screen: a
turn has no clock, and leaving the app abandons the run under the v0.1
lifecycle. Reading order, top to bottom, is the one the review set:

```text
HUD          SCORE (BEST in standby), LAYERS, and STABILITY as a ten-block
             meter reading the hinge's effective margin
table        528 x 600, a hairline panel: the tower in a fixed dimetric
             view, both front faces visible, so every block end is
             reachable without turning the tower
PIECE        the selected block: its class once tested (UNKNOWN, with
             LOOSE? for a tell, before), layer and side, WORTH, TESTS left;
             IN HAND while placing
controls     the pull track, 64 px, the thumb's zone; while placing, three
             side buttons instead; then TEST and BEGIN / PLACE
```

The tower is looked at, not dragged. A tap picks the nearest pullable
block end within 44 px, which is PocketFleet's approved aim-then-confirm
pattern (D1 there) applied to rows 22 px tall. The pull happens on the
track: the finger's horizontal travel is banked as it arrives and paid into
the engine once a tick, so the engine sees exactly one travel per tick
whatever the panel's event rate is, and the block moves in the viewport,
never under the finger. Track-right always moves the block right on
screen, so a block whose end faces the right face pulls on a right drag
and pushes on a left one, and the other orientation is mirrored; the
track's caption says which. Placement is aim-then-confirm too: LEFT,
CENTRE and RIGHT move a ghost on the new layer (against the lean by
default), PLACE commits, so a mis-tap can never be what fells the tower.

One clock: a single LVGL timer at `TIMBER_TICK_MS` steps the engine and
repaints the table while something moves (a finger on the track, a block
part way out, disturbance, a collapse); at rest nothing repaints. The
collapse plays from the engine's choreography, block by block, and the
result appears when the last block rests. Reduced motion draws no sway;
the engine still computes it.

**Placeholder look.** Every block is three flat faces in the `line`,
`surface_raised` and `surface` tokens with a `text_muted` hairline round
the top, the selection is a 2 px `accent_primary` outline on the block's
pulling end, the ghost an accent outline on the top and end. A tell is a
2 px misalignment along the block's axis; a falling block tumbles through
six poses by orientation and tilt. No colour is named anywhere
(`tests/style_lint.sh`), so nothing needs the framed-scene deviation yet:
D1 is the art, and the art is not in P7.

`$POCKETTIMBER_SCREEN` opens the app in a named state from a fixed seed
with the clock paused, the way PocketRadar does, so every shot is exactly
the state it names:

```bash
export SDL_VIDEODRIVER=dummy POCKETTIMBER_SCREEN=pulling
pocketos-shell --open timber --screenshot docs/design/shots/timber-pulling.png --exit-after-ms 900
```

| File | State |
| --- | --- |
| `timber-idle.png` | standby: the fresh tower, BEST, BEGIN |
| `timber-run.png` | three turns in, a bottom block selected and tested STUCK, worth 1,238 |
| `timber-pulling.png` | a block part way out, the selection locked to it |
| `timber-placing.png` | a block in hand, the ghost on the new layer, the sides |
| `timber-collapse.png` | mid-collapse after a yanked base block, the meter empty |
| `timber-result.png` | the result: score, cause, layers, pulls, clean, streak |
| `timber-pulling-reduced-motion.png` | the same as pulling with the sway not drawn |

`tests/timber_shell_test.sh` renders every state headlessly and checks
that none logs a fault:

```bash
SHELL_BIN=~/work/pocketos-build/shell/pocketos-shell bash tests/timber_shell_test.sh
```

## Art status (D1)

The visual direction, the canonical projection (2:1 dimetric, 36 px per
width, 26 px per layer) and the Blender pipeline are specified and proven
in [POCKETTIMBER_ART.md](POCKETTIMBER_ART.md): a proof set of two block
sprites, the felt and the contact shadow, rendered in batch from
`docs/design/timber-art/tools/timber_blender.py`, converted at build time
and drawn by the simulator, with the P7 placeholder kept as the fallback
(`POCKETTIMBER_PLACEHOLDER=1`). The production set (18 block sprites) is
not rendered. The engine is unchanged; the view constants and the table
widget's drawing changed. D1 verdict 2026-09-08: ART DIRECTION APPROVED FOR
PRODUCTION, with the open art decisions and the hardware-deferred numbers
listed in the art document.

## Hardware gates

Every constant that depends on an unmeasured K230 number lives in
`apps/timber/engine/timber_tuning.h`, is a conservative placeholder, and
is HARDWARE VALIDATION REQUIRED. The gates, all unmeasured as of P1:

| Gate | Placeholder | Where it bites |
| --- | --- | --- |
| Sprite-storm redraw budget | the P7 table draws every block as six triangles and five lines a frame, host only, unmeasured on the K230 | the table widget's frame cost; the real art's blits |
| GT9895 drag event rate | pull travel banked per event, paid per 40 ms tick, judged over a 3-tick average | the speed limits and stiction in `timber_tuning.h` |
| Drag latency | none assumed | the feel of the pull track |
| 1 to 3 px sway readability | `TIMBER_SWAY_AMP` 0.10 widths per unit; `TIMBER_VIEW_SCALE` 40 px per width, so one unit of disturbance sways the top 4 px | whether the sway is visible on the panel |
| 20/25 Hz target choice | `TIMBER_TICK_MS` 40 | every per-tick rate, and the clock the table repaints on |
| Static-tower caching strategy | none: the table repaints whole while anything moves and not at all at rest | P7 rendering only; the cached-band fallback in the review waits on numbers |

PocketRadar H1 (docs/KNOWN_ISSUES.md) is the cheaper probe and should be
measured first.

## Deviations to request

Two rulings are still needed before the app can go further than the
simulator placeholder; the third, D3, has been given:

- D1, the framed scene: the tabletop is content inside a viewport, drawn
  with app-owned raster art rather than tokens (DS §1, §2). Pending, and
  not yet needed: the P7 placeholder draws on tokens only.
- D2, an app-owned record file, as PocketRadar's D1 and PocketFleet's D2.
  Pending; refused, PocketTimber loses only its best score between
  sessions.
- D3, two design additions to the reviewed model, made in P6 when the
  modelled players showed the run had no ending for a decent player
  (Pacing above). **APPROVED by the product owner, 2026-09-08.**

  **The shift lean** is approved as a deterministic pacing mechanism. A
  block letting go of the stack leaves the tower leaning
  `TIMBER_SHIFT_LEAN` (0.006 widths per layer) along its axis in the
  direction it was drawn and `TIMBER_SHIFT_LEAN_ACROSS` (0.004) across it
  toward the neighbour its micro-offset points at, both scaled by the
  shift's impulse, which is the load the block carried. Every input to it
  is state or an action: the impulse from the tower, the direction from
  the pull, the settling side from the block's seeded micro-offset. It
  draws nothing from the generator, so a run's lean is a function of the
  seed and the action list and replays bit for bit, and a snapshot of the
  run resumes to the same lean (`tests/timber_rules_test.c`,
  `tests/timber_replay_test.c`). It is the ramp, it is controllable
  (pulling from alternate sides cancels the along term, and placing
  against the lean still helps), and it is legible (the tower visibly
  leans more as it thins).

  **The summit** is approved as the standing completion condition. A
  placement that completes the top of a tower already at
  `TIMBER_LAYERS_MAX` ends the run OVER with cause NONE, with nothing in
  hand, nothing selected and the turn open, before any next pull could
  start; every act is refused from then on, the tower is whole and valid,
  and the score, with that layer paid, is kept. It is the only way a run
  ends without a collapse.

  Both remain tuning constants, in `timber_stability.h` and
  `timber_rules.c`. **The current values are the simulator's placeholders,
  not hardware-tuned values;** zero restores the reviewed model, and the
  modelled players say what any setting does.

## Tests

| Test | Covers |
| --- | --- |
| `tests/timber_rng_test.c` | reproducibility, the zero-seed guard, two regression vectors, unbiased bounded draws, the stream-consumption contract |
| `tests/timber_types_test.c` | the name tables, layer axes, block footprints under extraction, rectangle intersection and overlap |
| `tests/timber_tower_test.c` | the canonical build, grid and block consistency, the locked-layers rule, removing and placing, gaps, every validator refusal, seeded generation and its guarantees over 400 seeds, tell rates, reseating, load |
| `tests/timber_pull_test.c` | tightness from seat and load, every class threshold, the limit and stiction tables, the jolt formula |
| `tests/timber_stability_test.c` | contacts and their box on the canonical tower, the stack above each layer, the margins of every thinned-layer case stated exactly, extraction and the support threshold, lean on both axes, micro-offsets and mass, the sine table, the sway share and its peak, sway against and with the lean, the hinge and its tie rule |
| `tests/timber_score_test.c` | every scoring number stated twice, the streak and its cap, the layer bonus, the record |
| `tests/timber_collapse_test.c` | the stump stays and the stack falls, a block in hand does not, the tip's shear and drop, the break, every block rests on a floor under the ceiling, some on the stump and some on the felt, a bounce, the rest pose, the same state twice, another seed, a higher hinge, both directions, the ceiling putting down a block that would fly forever |
| `tests/timber_replay_test.c` | the action vocabulary and apply, advance, the log and its bound, a scripted session replayed bit for bit, one changed pull or another seed diverging, a session through the summit replayed to the same summit, three modelled players over five seeds: every run ends, the careful one late, the greedy one first, and every session replays exactly, lean included |
| `tests/timber_view_test.c` | the projection and framing, a block's faces and pulling end, the tell nudge, extraction, lean and sway by height, reduced motion, the ghost, the drawing order, picking every pullable end and nothing else, the sense of the track, the sides of the slots |
| `tests/timber_shell_test.sh` | the app in the running shell: every state renders from a fixed seed, none logs a fault, standby opens with nothing asked for, reduced motion draws the tower without the sway |
| `tests/timber_rules_test.c` | run lifecycle, generation in a run, selection and its lock, the TEST budget and stickiness, the free pull, partial extraction and pushing back, the slip, jolts and their direction, a sustained rattle and the disturbance ceiling, stiction and break-free, the slow and the yanked STUCK pull, decay and the sway's direction, the hinge and the meter in a run, the creak and its edge, the load shift and its size, the base without its centre or a side block, placement with its reseat, lean nudge and layer bonus, scoring in a run and the piece card, the shift lean and its accumulation over pulls (the sum of every shift's share, signed by direction and by offset, with the generator untouched), all four collapse causes, the choreography in a run with every LAND announced and OVER before the ceiling, the summit reached from a constructed tower (nothing in hand, everything refused after), snapshots of the run resumed before and after the summit and mid-run, the event queue, replay determinism and the untouched generator |
| `tests/timber_lint.sh` | no LVGL, no I/O, no floating point and no platform entropy in the engine; one file touches the filesystem |

Run with `make CC=gcc CFLAGS="-O2 -Werror" test` (WSL2 Ubuntu 22.04, gcc
11.4). Numbers are recorded per milestone in the commit message.

## Milestones

| Milestone | Content | Status |
| --- | --- | --- |
| P1 | engine skeleton, tower and run state, selection, events | done |
| P2 | seeded generation, classes, TEST, extraction, pull dynamics | done |
| P3 | contacts, centre of mass, margins, lean, disturbance | done |
| P4 | scoring, placement, collapse trigger and cause | done |
| P5 | deterministic collapse choreography | done |
| P6 | replay log, modelled-player pacing | done |
| P7 | minimal simulator UI with placeholder blocks | done |

## Not in this pass

No art (D1), no record file (D2), no audio, haptics, screen shake, tower
rotation, camera pan, wood species, seeded challenge list, hot-seat mode,
irregular blocks, alternate layouts, tutorial, achievements, networking or
platform change. No pause screen: a turn has no clock, and leaving the app
abandons the run as PocketRadar does under the v0.1 lifecycle. No K230
optimisation: the table repaints every block while anything moves, which
is the simplest correct thing and is HARDWARE VALIDATION REQUIRED before
anything cleverer is built.

What the next pass needs from the owner, in order: the two rulings still
open (D1 art, D2 record file), the bench numbers for the gates, and then a
real finger on the pull track, which no test here can stand in for.
