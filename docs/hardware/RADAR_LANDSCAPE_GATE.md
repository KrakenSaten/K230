# PocketRadar in landscape — validation gate

Branch `feature/radar-landscape`, rebased onto master `f577fff` (the master
Fleet made), tip `c39b7d4`. VERSION 0.0.10, unchanged. DS Amendment M (§29) is
**ACCEPTED**.

**Verdict: PASS.** Host and simulator PASS; unit A remote PASS; and on
2026-09-18 the product owner put a hand on build `c39b7d4` and **ruled PASS**
on all three of the questions this gate was for. Nothing is outstanding.

The branch was developed independently of `feature/fleet-landscape`, both from
`7d0d1ea`, and numbered §29 against Fleet's §28 so the two would not collide.
Fleet went in first; this is the rebase onto the master it made, revalidated
whole.

Radar is the eighth app given a landscape layout under DS §21.3. What is
different about it is that it costs something per frame: it repaints a
custom-drawn scope twenty times a second while a run is on, and that is its
whole frame cost (`docs/KNOWN_ISSUES.md`, hardware verification H1). So the
work had to show not only that the layout is right but that it is **cheaper**,
and that is measured here three ways — in pixels, in draw calls, and in CPU on
the unit.

```text
TALL   portrait, 528 x 1060: the v0.0.10 layout, scope 520 px.
WIDE   landscape, 1192 x 386 once the foot clears the rounded corners:
       scope 386 px at the left, the two cards abreast, ENGAGE across the foot.
```

## What the owner was asked, and answered

All three were ruled **PASS** on 2026-09-18, with the unit in hand on build
`c39b7d4`:

1. **Is the landscape radar scope comfortably readable?** The scope is 386 px
   against 520 — 74 % of the diameter — with the same rings, ticks and
   contact shapes. **PASS.**
2. **Tap a contact and then ENGAGE — does targeting feel comfortable with a
   normal thumb?** Contacts are selected by tapping them on the scope; ENGAGE
   is 786 x 64 across the whole foot of the right-hand region. **PASS.**
3. **Does the scope-left / information-right layout feel balanced and
   natural?** **PASS.**

There was no fourth question about portrait: it is pixel-identical to master
below the status bar in all 24 simulator captures, before and after the
rebase.

This is the question a bench could not answer. A run of PocketRadar ends after
five lost tracks, which is sooner than a round trip from the bench to the unit
and back: taps injected from here find and engage contacts (the record rose
from 120 to 265 doing it), but nothing here can chase a contact the way a
thumb does. That is what question 2 was for.

## Host validation

The original pass, from a fresh clone of `5881704` and of master `7d0d1ea` for
comparison. The rebase onto Fleet's master was revalidated whole as well, and
those figures are further down.

| Check | Result |
| --- | --- |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,783 ok, 0 FAIL, 0 warnings (master: 3,772; the 11 new `radar_lint` checks) |
| SDL simulator build | rc 0, 0 first-party warnings |
| 21 shell and UI test scripts | 538 ok, 0 FAIL, every script rc 0 (master: 525; `radar_shell_test.sh` 31 → 44) |
| `radar_app_test` | **191 checks, 0 failures** (new) |
| `radar_scope_test` | 132 checks, 0 failures (master: 64 — the whole round trip now runs at both scope sizes) |
| `radar_lint.sh` | 16 checks, 0 failures (master: 5) |
| `radar_rules_test` / `radar_score_test` / `radar_rng_test` / `radar_types_test` / `radar_store_test` | unchanged and green: the engine is byte-identical to master |
| the other six app tests | unchanged and green: no other app is touched |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 4 warnings, all `vendor/ggwave`, as in the release; 0 first-party |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; `Doors 0.0.10 build 5881704` |

### Mutation testing

24 mutations of the new layout, the scope's geometry and the tick path.
**All 24 caught**, including the three that matter most here: a tick that
invalidates the whole screen instead of the scope; a resized scope drawn at
one size and touched at another; and the scope's radius left behind by a
resize. Three of the 24 were gaps in `radar_app_test` that the first mutation
run found; they are closed in `6146fbb`, not argued away.

### What `radar_app_test` adds

The app hosted the way the shell hosts it, on the reference panel with its
30 px rounded corners and with square ones, in portrait and landscape:

- the shape rule as arithmetic, at and either side of every floor, including
  that **the scope can never exceed the size it has down the page**;
- both screens in both shapes: what stands beside what, every action a
  finger's size, nothing outside the body or the safe area;
- the scope's tap conversion at both sizes: each cardinal direction out and
  back, the centre, and points beyond the rim;
- selecting a contact by tapping where the scope drew it, at both sizes;
- the display turned under a run in progress, three times over: the same
  objects, none added, and the run — its contacts, score, level, integrity and
  selection — untouched;
- **a hundred ticks of a running scan**: no layout pass at all, no new object,
  and a draw count for each thing on the screen.

### The cost of a tick, measured

| Measure | Portrait | Landscape |
| --- | --- | --- |
| Scope area (pixels invalidated per tick) | 270,400 | **148,996 — 55 %** |
| Draws over 100 ticks: scope / numbers / contact card | — | **200 / 10 / 0** |
| Layout passes over 100 ticks | — | **0** |
| **CPU on unit A, during a running scan** | **17 % of one core** | **14 % of one core** |

The portrait figure matches the 17 % measured at bring-up
(`docs/hardware/BRINGUP_SESSION_2026-09-07.md`, H1), which is a useful check
that the measurement is of the same thing. H1 itself stays open: this work
does not measure a frame time, it shows that the wide shape asks for less than
the shape H1 was written about, and cannot be made to ask for more.

## Simulator

48 captures per run: six states (idle, scan, selected, acquired, decoy,
result) × two orientations × 30 px and square corners × Normal and Outdoor.

| Check | Result |
| --- | --- |
| Every capture rendered, no fault in any log | 48 of 48 |
| Foot corner squares, 30 px corners | **clear in 24 of 24**, both orientations |
| Portrait against master, below the status bar | **24 of 24 pixel-identical** |

Radar's cards carry no caption on their top border and its stack already fits
the body, so neither the caption headroom nor the foot clearance that Fleet's
§28.4 documents moves anything here. Portrait is untouched, exactly.

## Unit A: remote validation

Everything below ran with nobody at the unit, over Ethernet with a bench SSH
key installed for the milestone through the console and removed at the end.

| Step | Evidence | Result |
| --- | --- | --- |
| Install | `doors-shell` from the riscv64 DRM build, stripped, 953,008 B, md5 `66146be9…`, equal on both ends; only `S90doors-shell` stopped and started; the shell answers `build 5881704`; 1 doors-shell, 0 crashloop | PASS |
| Portrait | Radar opened; the v0.0.10 layout on the panel; a scan started by an injected tap and running (`SCANNING`, contacts on the scope, integrity counting down) | PASS |
| To landscape | `doors call shell shell.rotation mode=landscape`, applied in place; Radar reopened | PASS |
| **The layout on the panel** | Scope at the left with its rings, bearing ticks and N/E/S/W marks; SCORE / STREAK / LEVEL and SECTOR INTEGRITY in the middle card; the contact card at the right; BEGIN SCAN, then ENGAGE, across the whole foot of that region | PASS (captures `l-radar-idle.png`, `l-radar-live.png`) |
| **Selecting and engaging by touch** | 108 taps swept over the scope in three rings with an ENGAGE after each, all injected into `/dev/input/event1`: the run scored, and the Result screen carried **BEST 120** — so contacts were found, selected and engaged by taps on the 386 px scope | **PASS** |
| The Result screen | `RUN COMPLETE`, the score card at the left and all five figures at the right in two columns — **none of them below a fold** — with NEW RUN across the foot | PASS (`l-radar-played.png`) |
| The record | `/var/lib/pocketos/radar/record.v1`, 30 bytes, written on the unit by a run played across the page | PASS |
| **CPU** | 14 % of one core in landscape against 17 % in portrait, each over a five-second sample of a running scan taken from `/proc/<pid>/stat` | PASS |
| Resources | VmRSS 12,800 kB before 216 injected taps and several complete runs, and 12,800 kB after | PASS |
| Health after | 1 doors-shell; sysd, netd and radiod unchanged; 0 crashloop; 0 crash reports; `shell.log` **0 ERROR, 0 WARN**; no segfault in `dmesg` | PASS |

### What only the panel and a hand could show

A capture is the framebuffer, not the glass, and an injected event cannot show
where a finger lands or how a moving contact feels to chase. That is what the
three questions at the top of this sheet were for, and on 2026-09-18 the owner
answered all three **PASS** with the unit in his hand.

## The rebase onto the master Fleet made

Radar was validated whole on `5881704`, from `7d0d1ea`. Fleet was accepted and
merged first, so this branch was rebased onto `f577fff` and revalidated. What
the rebase touched and what it did not:

| | |
| --- | --- |
| Radar's own files | **byte-identical**: all 26 blobs — `apps/radar/**`, `tests/radar_*`, `POCKETRADAR.md`, this sheet — have the same hashes they had on `4a77323` |
| Fleet's files | **untouched**: nothing under `apps/fleet`, `tests/fleet_*` or Fleet's docs differs from master |
| Conflicts | three documentation files, all resolved by keeping both amendments whole: §21.3's list becomes `… Calendar: §27. Fleet: §28. Radar: §29.`; §28 is kept entire and §29 follows it; the register carries both lines. Fleet's §28 is byte-identical to master's, checked |
| One extra commit | Fleet and Radar both added an app test to the same `if(POCKETOS_DISPLAY STREQUAL "sdl")` block, and the merge left `radar_scope_test`'s comment without the `if` it used to open. Comment moved; same targets, same conditions |

Revalidated from a fresh clone of `c39b7d4`: `make all` rc 0 with 0 warnings;
`make test` **3,804 ok, 0 FAIL, 0 warnings**; 21 shell and UI scripts **549
ok, 0 FAIL**, every script rc 0; `radar_app_test` 191, `radar_scope_test` 132,
**`fleet_app_test` 605**, and Settings, System, Clock and Calendar all
unchanged and green; every lint green including `radar_lint` 16 and
`fleet_lint` 26; riscv64 `make all` rc 0 with 0 first-party warnings, and the
riscv64 DRM/sysroot shell rc 0 with none at all.

Installed on unit A the same way as before - userspace only, nothing flashed:
the stripped riscv64 DRM build of `c39b7d4`, 957,104 B, md5 `5d2dcf83…` equal
on both ends, with only `S90doors-shell` stopped and started and a rollback
copy of the Fleet build kept first. The shell answers `build c39b7d4`; one
doors-shell, 0 crashloop, sysd/netd/radiod unchanged, `shell.log` 0 ERROR and
0 WARN, no segfault in `dmesg`, VmRSS steady at 12,544 kB. Exercised remotely
before the owner was asked for anything: Radar opened, BEGIN SCAN, a live
sweep with contacts on the scope, then 108 taps over three rings worked from
the rim inwards with an ENGAGE after each and the whole set repeated three
times, ending at RUN COMPLETE. **The record rose from 120 to 265**, which only
happens by finding, selecting and engaging contacts. A running scan costs
**14 % of one core**, the same figure measured before the rebase.

The things the rebase could have broken, checked directly: **portrait is 24 of
24 pixel-identical to master**; the foot corner squares are clear in 12 of 12
landscape and 12 of 12 portrait captures; the landscape geometry is what
`radar_app_test` pins; and the tick path is untouched — **a hundred ticks lay
the app out not once**, the scope is drawn 200 times over them (twice a tick,
the 20 Hz repaint), and it is 148,996 px against portrait's 270,400.

## As left

Unit A: **landscape**, on the launcher, theme carbon, Normal, brightness 100,
Wi-Fi **off** exactly as found, 1 doors-shell, 0 crashloop, 0 crash reports,
`shell.log` 0 ERROR 0 WARN, 0 segfaults, VmRSS 12,544 kB. Running build
**`c39b7d4`** — this branch.

It is in forced landscape rather than Automatic/portrait because that is how
it was found at the start of this work, and it is what this gate needed.

Two rollback copies: `/root/doors-shell.0dd9ee1`, the Fleet build the unit
carried before this one, and `/root/doors-shell.rollback`, the `99b2374` shell
it was originally found with.

**Rollback**: stop `S90doors-shell`, copy either over `/usr/bin/doors-shell`,
start it again.

`radar/record.v1` now reads **BEST 265 over 51 runs**, up from 120: the remote
exercise played the game rather than pretending to. `fleet/save.v1` is
untouched, still the match at Officer turn 4.

The bench SSH key installed for this work is removed, returning `/root/.ssh`
to the state it was found in.

Captures and logs: `out/radar-rebase/hwgate-unitA/` for this pass, and
`out/radar-landscape-5881704/hwgate-unitA/` for the original validation. Both
outside the repository.
