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

Status: **milestone P3 (contacts, centre of mass, margins, lean and
disturbance)** on branch `pockettimber-engine`. Not merged. Nothing has run on hardware; nothing here
is built into the shell yet. Master and the golden bring-up image are
untouched by this work.

## Isolation rule

This branch exists under a pre-hardware isolation rule: master is frozen,
the golden K230 image (`out/k230`, SHA-256 `bafed837…`) is not rebuilt,
replaced or altered, and nothing here merges before the first physical
bring-up. Every milestone is one focused commit on this branch.

## Layers

```text
apps/timber/engine/   vocabulary, RNG, tower state and generation, the pull
                      model, the stability model, run rules. Pure C, no
                      LVGL, no I/O, no floating point, no platform entropy.
apps/timber/ui/       (P7) the view model and the table widget.
apps/timber/timber_store.c  (P7) the record file: the app's only door to
                      the filesystem.
```

The dependency runs one way (app to ui to engine) and the engine never
calls up. It is built by the root `Makefile` so it is unit tested natively,
and from P7 again by `ui/shell/CMakeLists.txt` into the shell binary.

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
  its length out, either way, slips free into the hand and the turn moves
  on to placing it (P4).

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
- **Creak.** When the hinge's static margin falls under a quarter of a
  width the tower creaks once, naming the hinge, and not again until the
  margin has come back over the line.

The trigger, turning an effective margin below zero into a collapse, is
P4; in P3 the margin is measured and reported. Every number above is
asserted exactly in `tests/timber_stability_test.c`.

## Determinism

A run is reproduced by `(seed, the ordered list of player actions at their
ticks)`. One xorshift32 stream is consumed in exactly two places: when the
tower is built and when a collapse begins (P5). Nothing the player does
moves the stream, so no outcome is ever a roll of the dice;
`tests/timber_rules_test.c` asserts the generator state after 120 ticks of
selecting, testing and pulling is the state it had at the start.

`timber_rng.c` duplicates `radar_rng.c` deliberately, for the reasons
recorded in `docs/apps/POCKETRADAR.md`; the regression vectors are
identical by construction.

## Hardware gates

Every constant that depends on an unmeasured K230 number lives in
`apps/timber/engine/timber_tuning.h`, is a conservative placeholder, and
is HARDWARE VALIDATION REQUIRED. The gates, all unmeasured as of P1:

| Gate | Placeholder | Where it bites |
| --- | --- | --- |
| Sprite-storm redraw budget | not needed before P7 | the table widget's frame cost |
| GT9895 drag event rate | pull travel sampled per 40 ms tick, judged over a 3-tick average | the speed limits and stiction in `timber_tuning.h` |
| Drag latency | none assumed | the feel of the pull track (P7) |
| 1 to 3 px sway readability | `TIMBER_SWAY_AMP` 0.10 widths per unit of disturbance | whether the sway the model computes is visible at the view's pixels per width (P7) |
| 20/25 Hz target choice | `TIMBER_TICK_MS` 40 | every per-tick rate |
| Static-tower caching strategy | deferred | P7 rendering only |

PocketRadar H1 (docs/KNOWN_ISSUES.md) is the cheaper probe and should be
measured first.

## Deviations to request

Two rulings are needed before the app (P7) can be integrated, neither
before the engine milestones:

- D1, the framed scene: the tabletop is content inside a viewport, drawn
  with app-owned raster art rather than tokens (DS §1, §2). Pending.
- D2, an app-owned record file, as PocketRadar's D1 and PocketFleet's D2.
  Pending; refused, PocketTimber loses only its best score between
  sessions.

## Tests

| Test | Covers |
| --- | --- |
| `tests/timber_rng_test.c` | reproducibility, the zero-seed guard, two regression vectors, unbiased bounded draws, the stream-consumption contract |
| `tests/timber_types_test.c` | the name tables, layer axes, block footprints under extraction, rectangle intersection and overlap |
| `tests/timber_tower_test.c` | the canonical build, grid and block consistency, the locked-layers rule, removing and placing, gaps, every validator refusal, seeded generation and its guarantees over 400 seeds, tell rates, reseating, load |
| `tests/timber_pull_test.c` | tightness from seat and load, every class threshold, the limit and stiction tables, the jolt formula |
| `tests/timber_stability_test.c` | contacts and their box on the canonical tower, the stack above each layer, the margins of every thinned-layer case stated exactly, extraction and the support threshold, lean on both axes, micro-offsets and mass, the sine table, the sway share and its peak, sway against and with the lean, the hinge and its tie rule |
| `tests/timber_rules_test.c` | run lifecycle, generation in a run, selection and its lock, the TEST budget and stickiness, the free pull, partial extraction and pushing back, the slip, jolts and their direction, a sustained rattle and the disturbance ceiling, stiction and break-free, the slow and the yanked STUCK pull, decay and the sway's direction, the hinge and the meter in a run, the creak and its edge, the load shift and its size, the base without its centre or a side block, the event queue, replay determinism and the untouched generator |
| `tests/timber_lint.sh` | no LVGL, no I/O, no floating point and no platform entropy in the engine; one file touches the filesystem |

Run with `make CC=gcc CFLAGS="-O2 -Werror" test` (WSL2 Ubuntu 22.04, gcc
11.4). Numbers are recorded per milestone in the commit message.

## Milestones

| Milestone | Content | Status |
| --- | --- | --- |
| P1 | engine skeleton, tower and run state, selection, events | done |
| P2 | seeded generation, classes, TEST, extraction, pull dynamics | done |
| P3 | contacts, centre of mass, margins, lean, disturbance | done |
| P4 | scoring, placement, collapse trigger and cause | |
| P5 | deterministic collapse choreography | |
| P6 | replay log, modelled-player pacing | |
| P7 | minimal simulator UI with placeholder blocks | |

## Not in v0.1

No audio, haptics, tower rotation, camera pan, wood species, seeded
challenge list, hot-seat mode, irregular blocks, alternate layouts,
tutorial, achievements, networking or platform change. No pause screen: a
turn has no clock, and leaving the app abandons the run as PocketRadar
does under the v0.1 lifecycle.
