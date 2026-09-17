# Calendar in landscape: unit A gate

Branch `feature/calendar-landscape`, first from origin/master `aaad9f4` (code
`d4b0fec`, docs `27718f2`, this sheet `f9af93f`), then **rebased onto
origin/master `2bdf279`** (Settings §24, System §25 and Clock §26 accepted):
code `17e5da0`, docs `57fe9e6`, and this sheet. VERSION stays 0.0.10.

**Result: PASS on unit A, 2026-09-17** - host and simulator validation before
the rebase (done host and simulator only, by the product owner's instruction,
without touching unit A), revalidation after it, a userspace deployment of the
rebased build with health checks over the serial console, then the product
owner's physical check (last sections), which he ruled PASS twice, including
the landscape day cells of §27.2. **The product owner ACCEPTED the work and DS
Amendment K (§27) on 2026-09-17.** Not merged.

Scope: Calendar only. No other app, no rotation policy, no keyboard presence
logic, no shell or PocketUI change, no boot splash, no first-boot or
vendor-launcher behaviour, no Phase 4, no new Calendar feature, no change to
the date arithmetic or the view model (`cal_date.c`, `cal_view.c` untouched).

## What changes

Calendar shapes its one screen from the size of the body it is given (DS
§27.1): tall in portrait, as before; wide in landscape, when the
body, less the corner clearance, is wider than tall, at least 1076 px across
and at least 384 px tall. Wide puts the month in the first half (586 px) at
the full height, its six weeks sharing it - cells about 80 x 56, 4 px apart,
under 24 px headings - and the portrait column without the month in the
second half: previous, the month and next at the top, Today at the foot level
with the last week, and the panel (the notice and the date in words)
stretched between them. Every day of every month is on screen at once and
nothing scrolls. Portrait does not change, with square or rounded corners.
Details: "the layout" in `apps/calendar/cal_app.c`.

**The one proposal the owner has to judge by hand (DS §27.2):** a
landscape day is wider than tall, at least 64 px wide and at least 56 px tall
(80 x 56 on the unit's panel), instead of portrait's 72 px square. Six weeks
of 64 px cells and their gaps need 404 px before any heading; the landscape
body has 386 above the corners. Selecting a day is not irreversible and is
shown at once; the arrows and Today keep 72 x 64 and 64 px.

On master, landscape was portrait stretched and cut at the foot: the grid 528
px wide at the left of a 1192 px body, the month row stretched across it, the
fourth week cut at the foot and the last two weeks, the panel and Today below
it (Today could not be pressed without scrolling), and the weeks at the foot
drawn into the rounded corners.

## Host validation, before the rebase

From fresh clones of `d4b0fec` and, for comparison, master `aaad9f4`; the
riscv64 builds from a fresh clone of `27718f2` (docs only after `d4b0fec`).

| Check | Result |
| --- | --- |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,741 ok, 0 FAIL, 0 warnings (master: 3,733; the 8 new `calendar_lint` checks); clean checkout after |
| SDL simulator build | rc 0, 0 warnings |
| 20 shell and UI test scripts | 498 ok, 0 FAIL, every script rc 0 (master: the same 20 and 494; `calendar_shell_test.sh` 21 against 17 - the real shell opened in landscape too) |
| `cal_app_test` | 1,317 checks, 0 failures (master: 88) |
| `calendar_lint.sh` | 48 checks, 0 failures (master: 40); against master's `cal_app.c` the 8 layout rules fail |
| `cal_date_test` / `cal_view_test` | 127 / 98 checks, 0 failures: unchanged, and `cal_date.c`, `cal_view.c` untouched |
| `calc_app_test`, `notes_app_test`, `clock_app_test`, `settings_app_test`, `display_geometry_shell_test.sh` | 456 / 1,138 / 118 / 81 checks / 69 ok, 0 failures: the other apps unaffected |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 4 warnings, all `vendor/ggwave`; 0 first-party |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; `Doors 0.0.10 build 27718f2`; `cal_app.c` compiled in; stripped `doors-shell` 944,816 B, md5 `793735a94ca37987416d067e9e23d9f0` (kept on the host, transferred nowhere); the app test is not a target there |
| `cal_app_test` against `aaad9f4`'s `cal_app.c` | fails at its first structural checks (no frame), then an LVGL assert on the tree master does not have holds it until the 600 s timeout |

Mutations of `cal_app.c` (each must make `cal_app_test` fail): 35 of 39
caught, counting master's `cal_app.c` as one. The four misses change nothing
that can be observed: the explicit layout call at create (the size pass just
before it already sends the frame's size event, and the call then finds the
box unchanged); Today centred rather than stretched in the tall shape (the
button is the column's full width of its own); the frame's scroll reset on
turning wide (LVGL moves a scroller back into range when it stops scrolling);
and not unhooking the frame's size handler in destroy (the shell deletes the
app's objects straight after, with no layout pass between; the unhook is
defensive, as in Notes). Caught: the wide shape never chosen; always
chosen; without the width floor, or it off by one; without the height floor,
or it off by one; without the wider-than-tall test; a 54 px cell floor; the
height taken before the corner clearance; no foot inset; the inset applied
sideways; no size handler; the weeks, the cells or the headings not grown
when wide; cells square when wide; 32 px headings when wide, 24 px when tall;
8 px weeks when wide, 4 px when tall; the month not stretched, or on one row,
when wide; the month row in the first half; the panel not stretched; Today in
the panel's row; the panel before the month when tall; the panel scrollable
when tall; the frame not scrollable when tall, or scrollable when wide; the
panel not scrollable when wide; halves 2:1; the panel's row sized to its
content; no gutter; no gap between rows.

An earlier mutation run, before the final code, also missed two lines that
set the month's and the panel's heights back to their content on turning tall
(LVGL does it itself) and a scroll reset of the panel that no reachable
content can need; those lines were removed rather than kept untested, and a
whole-screen check after each turn back to portrait was added.

What `cal_app_test` adds (the app hosted as the shell hosts it, on the
reference panel): portrait pinned to master's rectangles - the month row,
the grid, every one of the 42 cells, the panel and Today - in Normal and
Outdoor, with square and 30 px corners; September 2026, a six-week August
2026, a four-week February 2027, the longest date in words (Wednesday 30
September 2026 - Today) and no date with a day picked (the panel at its
tallest), in portrait and landscape, rounded and square corners, Normal and
Outdoor, each checked for its shape, the body never scrolled, the frame's
foot inset, every target (the arrows and Today 64 x 64; days 72 square in
portrait, at least 64 x 56 in landscape) on no other target and inside the
body and the safe area, every label laid out whole, the panel saying all it
says without scrolling, seven columns under their headings, six even weeks at
their gaps, today's dot inked below its number inside its cell, and the pieces
where the shape puts them; the landscape numbers on the unit's panel; the
display turned under the open app three times with October on screen and a
September day picked (month, selection and today's mark kept, the same days
as targets, no object more or less, every screen checked, portrait back to the
pixel), previous, a day, a blank, next and Today pressed in landscape, a
six-week November checked there, a board with no date turned to landscape and
then given the date; five open, turn and close rounds leaving nothing behind;
the width floor at 1075 and 1076 px, the height floor at 383 and 384 px, a
1192 px body taller than wide, and a 1192 x 100 body that keeps portrait and
scrolls every target wholly into view.

## Simulator validation, before the rebase

The real SDL shell with a dev-only scripted finger and a fixed wall clock
(never committed), Calendar opened in both orientations, with the reference
panel's 30 px corners and with square corners, Normal and Outdoor. Dates: 17
September 2026 (set), 30 September 2026 (the longest date in words) and a
board that has just booted (not set). States: September; a day picked; a
six-week August; December 2026 and, across the year, January 2027; a
four-week February 2027 with the 28th picked; Today; February 2026; leap
February 2028 with the 29th picked; January 2024 with no date, then a day
picked, then leap February 2024. 32 runs and 112 captures of the branch, and
the same of master `aaad9f4`. No branch run logged an error or an LVGL
warning, and no run wrote a file. On master 8 of the 16 landscape runs could
not press what they had to - Today, and the 29th of February 2028 - because
both were below the foot of the body; LVGL logged the finger off the display.

| Comparison | Result |
| --- | --- |
| Portrait, pixels | 56 of 56 captures identical to master below the status bar, square and 30 px corners, Normal and Outdoor |
| Portrait, objects | 56 of 56 states identical to master: every label and button in the same place with the same text (the status bar clock aside) |
| Rounded corners, at 30 px | branch: the foot corner squares hold only background in 56 of 56 captures (28 portrait, 28 landscape); master: 28 of 56 (every landscape capture drawn into, 88-300 px) |
| Landscape arrangement | headings 20..605 × 152..175; six weeks 20..605 × 180..537, cells 80-81 × 56-57; the month row 626..1211 × 152..215; the panel 626..1211 × 236..453; Today 626..1211 × 474..537; with square corners cells 58 tall, the panel 236..463 and Today 484..547 |
| Landscape, tallest panel | Outdoor, no date, a day picked: content 258..414 inside 236..453 (201 of 218 px with its padding); Normal 184 |
| Landscape, months | four, five and six weeks each fill the same grid; the sixth week ends level with Today; today's dot clear of its number in Outdoor (checked enlarged) |

## Found on the way, left alone

Outside this scope:

- **The selected outline is cut by its week.** The 2 px `focus` outline is
  drawn outside the cell and the week's row, exactly as tall as its cells,
  clips it: a selected day shows the outline on its sides only, a selected
  Monday on its right only. The same in portrait since v0.0.9 (identical
  pixels on master), and the same in landscape.
- **A rotation restarts the shell** (DS §21.2, `restart_in_place()` is an
  `execv`) and comes back on the launcher, so a picked day does not survive a
  turn; Calendar opened again lands on today, as every open does. Nothing is
  stored, by design.
- `docs/KNOWN_ISSUES.md` and `docs/ROADMAP.md` still list Calendar among the
  apps shown at a portrait width in landscape (and do not list Notes); like
  §21.3's list, that is for the merge, in merge order with the other pending
  landscape branches, none of which edits them either.

## Hardware questions, as answered

- **Is an 80 x 56 day, wider than tall, hit as reliably by a thumb as a 72 px
  square?** **Answered by the owner: yes** - he ruled the physical check PASS,
  twice, with §27.2 named as its decisive step. Taps leave no trace in the
  log, so this rests on his hands and his word, not on evidence this session
  can show.
- **Does the layout read on the panel?** Portrait and landscape were captured
  from the unit over the serial console (`caps/`): the month fills the left
  half, the arrows, the panel and Today the right, the whole of September 2026
  on screen at once, nothing clipped and nothing in the corner squares.
- **Outdoor on the panel: still unanswered.** The owner's batch asked for
  OUTDOOR in landscape; `shell.log` shows `mode normal` throughout and no
  appearance change, so it was not exercised on the unit. Outdoor is covered
  in the simulator and by `cal_app_test` only.
- **Portrait steps 1-3 (tapping days, the arrows and Today in portrait):**
  Calendar was open in portrait for five seconds before the owner turned the
  display, which is too short for them; portrait is unchanged from v0.0.10 by
  pixel comparison, and the owner ruled the batch PASS.
- **A pixel comparison against the simulator was not made.** The unit runs the
  Carbon theme and its own clock; the captures were read as pictures instead.
- On the unit a rotation comes back on the launcher, so Calendar is never open
  while the display turns; the relayout under an open app stays host evidence.

## The rebase onto `2bdf279`

Only `docs/design/POCKETOS-DS-v0.1.md` conflicted, in the one hunk at the end
of the document where the accepted §24, §25 and §26 meet the section this
branch had written with a placeholder number. Resolved by keeping master's
three amendments untouched - the DS diff against `2bdf279` adds lines and
removes only the one §21.3 list line it extends - and numbering Calendar
**§27, Amendment K**, with `§27.1`-`§27.4` inside it, the §21.3 list reading
"System: §25. Clock: §26. Calendar: §27.", and §27.4 citing §25.3 for the
floor test rather than restating it. `apps/calendar/cal_app.c`, the three
Calendar test files and the lint script are byte-identical to their pre-rebase
blobs (`f9af93f`); `cal_date.c`, `cal_date.h`, `cal_view.c` and `cal_view.h`
are byte-identical to master's, so the date arithmetic and the view model are
untouched by both the work and the rebase. The pre-rebase tip is kept as the
local branch `backup/calendar-landscape-pre-rebase`.

Revalidated from a fresh clone of the rebased sheet commit:

| Check | Result |
| --- | --- |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,769 ok, 0 FAIL, 0 warnings (master `2bdf279`: 3,761); clean checkout after |
| SDL simulator build | rc 0, 0 warnings |
| 21 shell and UI test scripts | 525 ok, 0 FAIL, every script rc 0 (master: the same 21 and 521) |
| `cal_app_test` / `calendar_shell_test.sh` / `calendar_lint.sh` | 1,317 checks / 21 ok / 48 checks, 0 failures |
| `settings_app_test` / `system_app_test` / `clock_app_test` | 636 / 833 / 778 checks, 0 failures: the three accepted layouts unaffected |
| `cal_date_test` / `cal_view_test` | 127 / 98 checks, 0 failures |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 4 warnings, all `vendor/ggwave`; 0 first-party |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; `Doors 0.0.10 build 99b2374`; stripped `doors-shell` 948,912 B, md5 `06472f2e461bbe60d7626dc9b88ea25c` - the build installed on unit A |
| Simulator smoke, 16 runs and 56 captures | every capture identical to the pre-rebase ones below the status bar; 56 of 56 states object-identical; the foot corner squares background only in 28 of 28 rounded-corner captures |

## Unit A: deployment

Userspace only, over the serial console; the card was not flashed and nothing
but `/usr/bin/doors-shell` changed.

| Step | Result |
| --- | --- |
| Before | `0.0.10` build `8aec2bc` (the Clock gate's), md5 `a893462f…`, pid 1748, restarts 0, crashloop 0, 0 crash reports, `shell.log` 0 ERROR 0 WARN, Automatic/portrait, theme Carbon, Wi-Fi off, brightness 100; date valid (2026-09-17 UTC, ntpd running) |
| Transfer | `doors-shell.gz.b64` (463,212 B) over COM9 in 96 s; decoded on the unit to md5 `06472f2e461bbe60d7626dc9b88ea25c`, the host artifact byte for byte |
| Install | rollback copy `/root/doors-shell.8aec2bc` kept; only `S90doors-shell` restarted; 7 of 7 checks ok |
| After | the shell answers `"build":"99b2374"`, exactly one `doors-shell` (pid 2612), supervised, restarts 0, crashloop 0; sysd, netd and radiod untouched and running |
| Remote render | Calendar opened from the console in portrait and, after `shell.rotation mode=landscape`, in landscape; both captured (`caps/portrait-calendar.png`, `caps/landscape-calendar.png`); back to Automatic before handing over |
| Stores | every file under `/var/lib/pocketos` byte-identical before and after, and no `calendar` directory: Calendar writes nothing |

## Unit A: the physical check

Handed to the owner as one batch: portrait (open, tap days, previous, next,
Today); landscape via Settings > Display > Rotation (the whole month on the
left, nothing clipped or in the corners, the right side intentional); **ten
thumb taps across the month including the first and last columns and the top,
middle and bottom weeks, each expected to select the day meant** (§27.2);
previous, next and Today; OUTDOOR and back; then AUTOMATIC and portrait again.

**The owner ruled PASS.** Asked once to reconcile the ruling with the log, he
ruled PASS again.

What `shell.log` shows of his session, and what it does not:

- 17:51:23 Calendar opened in portrait, closed 17:51:28 (five seconds);
  17:51:31 Settings; 17:51:33 rotation stored landscape and the shell
  restarted in place; 17:51:39 Calendar opened in landscape and left open.
- Taps are not logged, so the thumb test itself rests on the owner's hands
  and his ruling.
- OUTDOOR was not entered (`mode normal` throughout, no appearance change),
  and the display was left in landscape rather than returned to Automatic.
  Both are recorded as not exercised rather than as passed; Outdoor stays a
  simulator and host result. After the ruling, the session set rotation back
  to Automatic, reopened Calendar in portrait and captured it
  (`caps/portrait-after-gate.png`).
- Health throughout and after: 0 ERROR and 0 WARN in the 85 new `shell.log`
  lines, no crash report, no crashloop marker, no segfault or oops in dmesg,
  one `doors-shell`, restarts 0, sysd/netd/radiod running, VmRSS 12,672 kB,
  and every store byte-identical to the pre-deploy listing.

Unit A was left on build `99b2374`, Automatic/portrait, theme Carbon, Wi-Fi
off, brightness 100, at the launcher, with `/root/doors-shell.8aec2bc` as the
rollback copy.
