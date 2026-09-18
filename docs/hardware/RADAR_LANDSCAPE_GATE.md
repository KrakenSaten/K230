# PocketRadar in landscape — validation gate

Branch `feature/radar-landscape`, tip `5881704`, from master `7d0d1ea`
(VERSION 0.0.10, unchanged). DS Amendment M (§29) is **PROPOSED**. The branch
is independent of `feature/fleet-landscape`; neither is merged.

**Verdict: host and simulator PASS; unit A remote PASS. Three questions are
left for the product owner, and all three are about how it feels.**

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

## What the owner is being asked

1. **Is the landscape radar comfortably readable?** The scope is 386 px
   against 520 — 74 % of the diameter — with the same rings, ticks and
   contact shapes.
2. **Are targeting and the controls comfortable with a real thumb?** Contacts
   are selected by tapping them on the scope; ENGAGE is 786 x 64 across the
   whole foot of the right-hand region.
3. **Is the balance between the scope and the right-hand region right?**

There is no fourth question about portrait: it is pixel-identical to v0.0.10
below the status bar in all 24 simulator captures.

## Host validation

From a fresh clone of `5881704`, and of master `7d0d1ea` for comparison.

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

### What only the panel and a hand can show

A capture is the framebuffer, not the glass, and an injected event cannot show
where a finger lands or how a moving contact feels to chase. The three
questions at the top of this sheet are what is left.

## As left

Unit A: **portrait, Automatic, on the launcher**, theme carbon, Normal,
brightness 100, Wi-Fi **off** exactly as found, 1 doors-shell, 0 crashloop,
0 crash reports, `shell.log` 0 ERROR 0 WARN. Running build `5881704` — this
branch — with the rollback copy at `/root/doors-shell.rollback`, which is the
`99b2374` shell the unit was found with.

**Rollback**: stop `S90doors-shell`, copy `/root/doors-shell.rollback` over
`/usr/bin/doors-shell`, start it again.

This gate and the Fleet one shared the unit in one overnight pass; Fleet was
validated first, and the unit was then moved to this build. The rollback copy
is the same for both, and is the state the unit started in.

The bench SSH key installed for the milestone is removed, returning
`/root/.ssh` to the state it was found in.

Captures and logs: `out/radar-landscape-5881704/hwgate-unitA/` (outside the
repository).
