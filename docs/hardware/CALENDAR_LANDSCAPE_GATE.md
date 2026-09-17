# Calendar in landscape: unit A gate

Branch `feature/calendar-landscape`, first from origin/master `aaad9f4` (code
`d4b0fec`, docs `27718f2`, this sheet `f9af93f`), then **rebased onto
origin/master `2bdf279`** (Settings §24, System §25 and Clock §26 accepted):
code `17e5da0`, docs `57fe9e6`, and this sheet. VERSION stays 0.0.10.

**Result so far: host and simulator validation PASS, 2026-09-17**, done host
and simulator only, by the product owner's instruction, without touching
unit A. After the rebase the work is revalidated, the rebased build is
deployed to unit A and the owner's physical check is run (later sections).
The design system's amendment is §27, Amendment K, PROPOSED.

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

## Unresolved hardware questions

Only the panel and a finger can answer these; nothing here is claimed for
unit A.

- Whether an 80 x 56 day, wider than tall, is hit as reliably by a thumb as a
  72 px square, across the whole month and at its edges (Monday, Sunday, the
  sixth week above the rounded corners).
- Whether 24 px weekday headings in mono 14, today's accent number with its
  dot in a 56 px cell, and the selected outline read at arm's length in
  Normal and Outdoor.
- Whether the stretched panel with one line of text reads as intended rather
  than empty.
- Capture against the simulator on the real panel (RGB565, within 13 per
  channel, as for Calculator, Notes and Settings). The status bar and today's
  mark follow the unit's clock, so the simulator must be given the unit's
  date at the moment of capture, or those areas masked.
- On the unit a rotation comes back on the launcher, so Calendar is never open
  while the display turns; the relayout under an open app is host evidence.

## The gate to run later

Only in a session the owner allows to use unit A. Build the stripped riscv64
DRM `doors-shell` from the branch tip as for the Settings, System and Clock
gates (a stripped build of `27718f2` was 944,816 B, md5 `793735a9…`; the gate
rebuilds it from the tip it runs). Calendar stores nothing, so there is no
store to back up; nothing else of the owner's is touched.

**Remote** (serial console or SSH, as the owner allows):

1. Identity before (release file, `doors-shell` md5 and build, rotation mode,
   theme, date valid or not, restarts, crash reports, `shell.log` errors).
   Rollback copy of the installed `doors-shell`; install the build; restart
   only `S90doors-shell`; the shell answers the new build, supervised,
   restarts 0.
2. Portrait: open Calendar; capture; tap previous, a day, next, Today;
   capture; compare with the simulator given the unit's date.
3. Landscape (`doors call shell shell.rotation mode=landscape`, restart in
   place, back on the launcher): open Calendar; capture (month left, row,
   panel and Today right, the foot corner squares background only); tap a day
   in each corner of the month (the Monday of the first week, the Sunday of
   the last), previous twice, next three times (to a six-week month if the
   unit's date allows), Today; capture after each.
4. Back to the mode found (Automatic); health after (`shell.log` 0 ERROR and
   WARN, no crash report, services running, restarts 0); nothing written under
   `/var/lib/pocketos` by Calendar.

**Physical** (the owner, one batch, about five minutes):

1. **Portrait** (as the unit is): open Calendar. It looks as it did. Tap a
   few days, previous, next, Today.
2. **Landscape** (Settings > Display > Rotation > LANDSCAPE; open Calendar):
   it looks designed for landscape - the whole month on the left, nothing
   clipped, overlapping or cut by the rounded corners; the month row, the
   panel and Today on the right. With a thumb, tap ten days across the month, including
   the first and last columns and the bottom week: **does every tap select
   the day you meant?** (the §27.2 question). Previous, next, Today. In OUTDOOR
   (Settings > Appearance) the numbers, headings and today's dot still read;
   back to the mode it was in.
3. **Back**: Settings > Display > Rotation > AUTOMATIC. Calendar still looks
   right in portrait.

**Result: NOT RUN** - host and simulator only by instruction; the owner's
physical check and the remote gate are pending.
