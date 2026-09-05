# PocketRadar

A tactical sensor game for PocketOS: contacts appear on a circular scope and
the operator selects, acquires and engages them before their tracks fade.
Local single player, one screen, a two-to-five minute round.

PocketRadar is a game. It models no radio hardware, performs no RF sensing
and reads no sensor of any kind. Every contact is invented by the engine
from a recorded seed. It borrows the language of a tactical sensor because
the game is played in that idiom, and it makes no claim beyond that.

Status: engine phases P1 to P4 complete on branch `pocketradar-engine`. Not
merged. There is no UI yet, so nothing here has been seen on a screen or
touched by a finger; P5 onwards await review.

## Layers

```text
apps/radar/engine/   vocabulary, RNG, run rules, scoring. Pure C, no LVGL,
                     and no I/O at all.
apps/radar/radar_store.c   the record file: the app's only door to the
                     filesystem.
apps/radar/ui/       (P5) view model, scope widget, screens.
apps/radar/radar_app.c     (P5) the shell app entry.
```

The dependency runs one way and the engine never calls up. It is built by
the root `Makefile` so it is unit tested natively, and will be built again
by `ui/shell/CMakeLists.txt` into the shell binary when the UI lands.

`tests/radar_lint.sh` states the structure as an executable rule rather than
as prose: the engine contains no LVGL, does no I/O, uses no floating point
and calls no libm, and takes no entropy from the platform; `radar_store.c`
is the only file in the app that touches the filesystem. It is a text
search, so comments in these files avoid the names it looks for.

This is stricter than PocketFleet in one place. PocketFleet confines
filesystem access to one file *inside* its engine; PocketRadar keeps its
store outside `engine/` entirely, so the engine is pure computation. That
also means the lint's "only `radar_store.c` touches the filesystem" rule
held vacuously through P1 to P3, which is the correct answer rather than a
gap.

## The interaction

SELECT → ACQUIRE → ENGAGE, and each step is a separate thing the operator
knows.

A tap selects a contact and does nothing else. Acquisition then runs by
itself for `RADAR_ACQUIRE_TICKS` (12 ticks, 600 ms) while the selection
holds, and only on completion is the contact's class revealed and a shot
armed. `radar_run_engage()` refuses anything that is merely selected, so a
foul is always a decision and never a surprise.

That gap is what gives the decoy a job. A decoy cannot be told from a target
until it has been locked, so its real cost is the 600 ms it steals from the
tracks that are still fading, not the score penalty. Two consequences are
rules rather than conveniences, and both are tested:

- A second tap on the contact already selected changes nothing, so a
  double-tap cannot throw away a lock that is nearly complete.
- What acquisition revealed is sticky; the firing solution is not. A decoy
  identified once stays identified, and switching away costs only the lock.
  Time spent is never wasted twice.

Selection is by contact id, never by array slot. A contact that fades takes
the selection with it and its slot is handed to another track within a
second or two; had ENGAGE referred to a slot it would eventually fire at the
wrong contact.

## Geometry and motion

The scope is polar and integral: bearing in decidegrees clockwise from the
top, range in permille of the scope radius. No floating point anywhere, so a
run replays bit-identically on the host and on the K230 and there is no
soft-float cost in a per-tick loop.

Range is derived, not integrated:

```text
range = RADAR_RANGE_MIN + (spawn_range - RADAR_RANGE_MIN) * ttl / ttl_max
```

It therefore cannot accumulate rounding error over a five-minute run, and a
track reaches the inner limit exactly as it fades. The scope needs no
numbers on it: how close a contact is to the hub is how little time is left
to work it. It also collapses two tuning knobs into one — a shorter-lived
class is a visibly faster one — which is why no class carries a speed of its
own. Bearing drift is left as the independent axis, because that is what
makes a contact hard to tap rather than merely urgent.

`radar_polar_dist2()` is the picking metric, and it is what earns the
no-floating-point rule. The tangential term is the small-angle arc length
r·θ with θ in decidegrees over 573 (within 0.008 % of a radian). A quarter
turn at the rim measures 1571 against a true 1571, and the whole metric
stays under 1.1 × 10⁷ so it cannot overflow 32 bits.

The sweep lives in the run rather than in the UI: four lines, deterministic,
testable, and the UI has nothing to invent. One turn every four seconds,
asserted against `RADAR_TICK_MS` so the two cannot drift apart.

## Determinism

A run is reproduced by `(seed, the ordered list of player actions at their
ticks)`. One xorshift32 stream drives every decision, in a fixed order.

The stream is stronger than that, though. Every spawn attempt draws its four
values *before* the scope is consulted for room, so a full scope costs a
contact but never shifts the stream: the sequence of contacts a seed
produces is a function of the tick count alone and never of how the run is
played. Two operators on the same seed meet the same contacts in the same
order. `tests/radar_rules_test.c` asserts it by running one quiet run and
one played hard against the same seed and comparing the generators after
every tick.

`radar_rng.c` duplicates `fleet_rng.c` deliberately. Apps do not depend on
one another and PocketOS has no shared first-party RNG in `core/`; forty
lines of self-contained arithmetic beside the engine that uses it is cheaper
than a platform change (ADR-002 fixed point 1). The seed-1 regression vector
is identical to PocketFleet's by construction, which is itself the check
that the copy is faithful.

## Scoring

```text
points = base * (1 + response bonus) * streak multiplier
```

| Class | Base | Lifetime at level 0 |
| --- | --- | --- |
| NORMAL | 100 | 140 ticks (7.0 s) |
| FAST | 150 | 80 ticks (4.0 s) |
| HIGH VALUE | 300 | 100 ticks (5.0 s) |
| DECOY | −150 if engaged | 120 ticks (6.0 s) |

The response bonus is up to +50 % and falls linearly with the life left in
the track, so working a contact the moment it appears is worth half as much
again as finishing it on its last tick. The streak multiplier rises 0.2 per
consecutive valid engagement and stops at 2.0, so a good run cannot run away
from an average one by an order of magnitude.

Engaging a decoy costs a flat 150 and the streak. A valid target that fades
costs 25 and the streak. **A decoy left alone to fade costs nothing at all**,
because that is the correct play.

The running score floors at zero while the value functions still return what
an action was worth, so a penalty larger than the score is reported in full
but cannot put the operator in a hole they can see no way out of.

Sector integrity, not a timer, ends a run: 100 points of it, 20 for a leaker
and 10 for a foul. Five leakers finish you. An unattended run ends in about
twenty seconds, which is the same rule seen from the other end.

## Difficulty

Difficulty is a function of elapsed time and of nothing else. Every
`RADAR_LEVEL_TICKS` (600 ticks, 30 s) the run steps up, to `RADAR_LEVEL_MAX`
(10) at five minutes. Nothing is derived from how the operator is doing, so
the curve is the same for everybody and `radar_level_params()` is testable on
its own.

| | Level 0 | Level 10 |
| --- | --- | --- |
| Spawn interval | 40 ticks (2.0 s) | 10 ticks (0.5 s) |
| Scope capacity | 4 | 9 |
| Lifetime scale | 100 % | 40 % |
| Drift | ±3 dd/tick | ±13 dd/tick |
| NORMAL / FAST / DECOY / HIGH VALUE | 61 / 18 / 15 / 6 % | 28 / 28 / 35 / 9 % |

The ceiling is where it is for one specific reason. A contact cannot be
worked in less than the 12-tick acquisition, so even an operator with no
reaction time at all services at most one contact per 12 ticks; the top
level sends one every 10. Above that line the arithmetic ends the run, and
nothing has to be taken away from the operator to make them lose — which is
the difference between a difficulty ramp and a cheat. The test states the
crossing as an assertion rather than leaving it as a claim in a comment.

Worth recording because it is counter-intuitive: **the decoy share does not
affect that crossing at all.** It slows arrival and service by exactly the
same factor. The ramp therefore had to cross on the spawn interval, which is
why `RADAR_LEVEL_MAX` is 10 rather than the 9 an earlier draft used. Decoys
are there to make the middle of a run a decision rather than a reflex.

Measured with the modelled operator in `tests/radar_rules_test.c`, which
works the most urgent contact it has not identified, engages targets and
leaves decoys:

| Operator | Round length |
| --- | --- |
| 500 ms reaction, five seeds | 231, 240, 244, 244, 246 s |
| No reaction time at all | 312 s |

A person is worse than the model at tapping a drifting marker, so real
rounds should land at the shorter end of the intended two to five minutes.
The test asserts a wide band and prints the numbers, because it is a model
of an operator and not an operator.

## Persistence

`$POCKETOS_STATE_DIR/radar/record.v1`, default
`/var/lib/pocketos/radar/record.v1`. Thirty bytes: magic, version, best
score, best streak, best level, runs, lifetime engagements, lifetime
mistakes, FNV-1a checksum.

- There is deliberately no resume. A run is two to five minutes long, so an
  interrupted one is simply lost. That is what keeps the file thirty bytes
  and its validation obvious.
- Writes are atomic (temp file, `fsync`, `rename`), so a power loss leaves
  either the previous record or the new one.
- Load is tri-state: 0 read, 1 no file, −1 present but unusable. The
  caller's record is untouched unless 0, so the app initialises an empty one
  and simply keeps it otherwise.
- Failure is never fatal and never loud. An absent, unreadable, damaged or
  impossible record means only that there is no best score to beat; the game
  plays identically. A damaged file is left in place rather than deleted,
  matching the settings store's policy.
- The checksum catches accidental corruption and is explicitly not a
  cryptographic digest — anyone who can write the file can recompute it.
  What keeps an impossible record out is the invariant check: a level above
  the ceiling, a streak longer than every engagement, a score with no
  engagement behind it, or counters with no run behind them are all refused.

That `/var/lib` is writable on the K230 is DOCUMENTED from the Buildroot
defconfig (ext4 rootfs, no read-only setting) and remains ASSUMED until
hardware confirms it.

## Deviations

### D1 — app-owned record file (PENDING product-owner approval)

PocketOS has no `storage.*` service, and the shell's settings store is for
short non-secret preferences rather than app data. PocketRadar writes its
own file, entirely inside `radar_store.c`, exactly as PocketFleet does under
its approved deviation D2 (`docs/apps/POCKETFLEET.md`).

This follows an already-approved pattern, but the approval was granted to
PocketFleet and not to PocketRadar, so it is recorded here as pending rather
than assumed. Nothing else in the app depends on the answer: if app-owned
files are refused, PocketRadar loses only its best score between sessions.

## Tests

| Test | Covers |
| --- | --- |
| `tests/radar_rng_test.c` | reproducibility, the zero-seed guard, two regression vectors, unbiased bounded draws, the stream-consumption contract |
| `tests/radar_types_test.c` | the class table, the name tables, bearing wrap and delta across the seam, the bearing readout, the picking metric |
| `tests/radar_rules_test.c` | run lifecycle, spawn schedule and class mix against the level table at both ends of the ramp, lifetime and inbound motion, selection and acquisition, picking, the event queue, replay determinism, engagement, the difficulty table, game over, round pacing |
| `tests/radar_score_test.c` | every scoring number stated twice, the streak multiplier and its cap, penalties, the zero floor, integrity, the lifetime record |
| `tests/radar_store_test.c` | codec round trip, refusal of damaged and impossible records, atomic writes, missing and unwritable directories, and a real run played to its end, stored and reloaded |
| `tests/radar_lint.sh` | no LVGL, no I/O, no floating point and no platform entropy in the engine; one file touches the filesystem |

Verified with `make CC=gcc CFLAGS="-O2 -Werror" test`: 21 steps, 646 checks
in total of which 309 are PocketRadar's (18 RNG, 33 vocabulary, 146 rules,
61 scoring, 51 store), and no compiler diagnostics at all on a clean tree.
The engine and store also cross-compile
clean for `riscv64-unknown-linux-gnu` with the pinned Xuantie toolchain
(gcc 14.1.1) at `-mcpu=c908v -mtune=c908 -O2 -Werror`. Built and tested
inside WSL2 Ubuntu 22.04; nothing has run on hardware.

## Not in v0.1

No radio, LoRa, Wi-Fi, BLE, PocketLink, GPS, real RF sensing, external
sensors, maps, campaigns, achievements, cloud scoreboards, accounts or
progression systems. PocketRadar is a local single-player micro-game.

Audio and haptics are not implemented and nothing depends on them. The
engine's event queue is the seam they would attach to when PocketOS has an
audio service: acquire, engage, foul and warning are already distinct events
carrying the class and the position they happened at.
