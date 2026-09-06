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

Status: **milestone P1 (engine skeleton and state)** on branch
`pockettimber-engine`. Not merged. Nothing has run on hardware; nothing here
is built into the shell yet. Master and the golden bring-up image are
untouched by this work.

## Isolation rule

This branch exists under a pre-hardware isolation rule: master is frozen,
the golden K230 image (`out/k230`, SHA-256 `bafed837…`) is not rebuilt,
replaced or altered, and nothing here merges before the first physical
bring-up. Every milestone is one focused commit on this branch.

## Layers

```text
apps/timber/engine/   vocabulary, RNG, tower state, run rules. Pure C, no
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
- Selecting a block does nothing to the tower. While a block is part way
  out the selection is locked to it; while a block is in hand nothing can
  be selected. A second tap on the selected block changes nothing.
- Two TESTs per turn (`TIMBER_TESTS_PER_TURN`); the TEST itself arrives in
  P2.

## Determinism

A run is reproduced by `(seed, the ordered list of player actions at their
ticks)`. One xorshift32 stream is consumed in exactly two places: when the
tower is built (P2) and when a collapse begins (P5). Nothing the player
does moves the stream, so no outcome is ever a roll of the dice.

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
| GT9895 drag event rate | pull travel sampled per 40 ms tick | speed-limit fairness (P2) |
| Drag latency | none assumed | the feel of the pull track (P7) |
| 1 to 3 px sway readability | none assumed | sway amplitude (P3, view in P7) |
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
| `tests/timber_tower_test.c` | the canonical build, grid and block consistency, the locked-layers rule, removing and placing, gaps, every validator refusal |
| `tests/timber_rules_test.c` | run lifecycle, selection and its lock, the event queue, the replay digest |
| `tests/timber_lint.sh` | no LVGL, no I/O, no floating point and no platform entropy in the engine; one file touches the filesystem |

Run with `make CC=gcc CFLAGS="-O2 -Werror" test` (WSL2 Ubuntu 22.04, gcc
11.4). Numbers are recorded per milestone in the commit message.

## Milestones

| Milestone | Content | Status |
| --- | --- | --- |
| P1 | engine skeleton, tower and run state, selection, events | done |
| P2 | seeded generation, classes, TEST, extraction, pull dynamics | |
| P3 | contacts, centre of mass, margins, lean, disturbance | |
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
