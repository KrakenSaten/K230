# PocketCalendar

A month, and which day it is. A calendar, not a scheduler: it has no events,
no reminders, no recurrence and nothing to sync, and it stores nothing at all.

Status: **shipped in v0.0.9 and v0.0.10.** The landscape layout (below) is on
branch `feature/calendar-landscape`: host and simulator validated, and
**accepted on unit A, 2026-09-17** (`docs/hardware/CALENDAR_LANDSCAPE_GATE.md`:
PASS; DS §27, Amendment K).

## What it is

One screen.

- **A month**, seven columns by six rows, Monday first, with the weekday
  headings above it.
- **Previous and next**, either side of the month and year.
- **Today**, which returns to the current month and selects the current day.
- **A selected day**, outlined, with its date written out in words below the
  grid.

That is the whole application. Tapping a day selects it; nothing else happens,
because there is nothing else to attach to a day.

## Portrait and landscape

PocketCalendar lays itself out in the body the shell gives it and picks its
shape from that body's size, never from the orientation (DS §21.2), on the
landscape pattern of DS §22.3 to §26.4. DS §27 (Amendment K) is the normative
version of this section.

- **Tall** - portrait, 528 x 1060 on the reference panel - is the layout
  Calendar always had (see Layout).
- **Wide** - landscape, 1192 x 396, or 1192 x 386 once the foot has cleared
  the unit's rounded corners - is chosen when the body is wider than tall, at
  least 1076 px across (two portrait bodies and the 20 px gutter, so nothing
  in it is narrower than in portrait) and at least 384 px tall (six weeks of
  56 px cells under 24 px headings, 4 px apart). The month takes the first
  half of the width and the full height; the second half is the portrait
  column without the month:

  | Left half (586 px) | Right half (586 px) |
  | --- | --- |
  | Mon .. Sun, then six weeks sharing the height: cells about 80 x 56 | previous, the month and next (the portrait row, 64 px) |
  | | the panel: the notice and the date in words, as tall as the room between |
  | | Today (64 px), at the foot, level with the last week |

  Every day of every month is on screen at once, and nothing scrolls. The
  panel says the most - "Date not set", its explanation and a picked day, in
  Outdoor type - in 201 px of its 218, and scrolls itself should it ever say
  more.
- **The cells are wider than tall in landscape.** Six weeks of 64 px cells
  and their headings need at least 404 px; the landscape body has 386 above
  the corners. So there the weeks share the height and the cells are at
  least 56 px tall - the height DS §7 gives paired buttons - and about 80 px
  wide. Portrait keeps its 72 px squares. Whether a thumb lands on the day it
  means to in landscape is the first thing the unit A gate asks.
- Nothing is built twice. The body changing size moves objects and changes
  nothing else: the month on screen, the selected day, today's mark, the
  notice and whether Today can be pressed all stay as they are. There is
  nothing to write.
- The foot of the body clears the panel's rounded corners
  (`pos_display_rect_insets()`, as Calculator and Notes do). In landscape the
  last week and Today end 10 px above the body's foot. In portrait nothing
  comes near the foot, so portrait is the previous layout to the pixel with
  square corners and with rounded ones.
- **On the unit, turning the display restarts the shell** (DS §21.2): it
  comes back on the launcher, so Calendar is never open while the display
  turns, and reopening it lands on today, as every open does. A day picked
  before the turn is not kept - nothing is stored, by design (see Storage).
  The relayout under an open app is proven on the host.

## The one thing it cannot do, and says so

**This board has no clock that survives a power cut**
(`docs/hardware/T-DISPLAY-K230.md`: "no RTC"), so every boot starts at the
epoch and reaches the real date only when the image's SNTP finds a network or
an operator runs `date -s`. On this hardware an unset clock is the ordinary
state for the first seconds after boot, not a rare fault.

A calendar that trusted the wall clock would mark 1 January 1970 as today. So:

- Nothing is today until the system date is valid. Not a faint marker, not a
  guess - no cell in any month carries the today mark.
- The screen says **"Date not set"** where the selected date would go, and
  explains why.
- **Today** is disabled, because there is no today to go to.
- **The months are still browsable.** The calendar opens on January 2024 and
  prev/next, selection and the date in words all work. January 2024 is not
  arbitrary: it is the month of `CLOCK_WALL_VALID_FROM`, the earliest reading
  PocketOS counts as a real time, so the fallback is the floor of what is
  believable rather than a guess. `tests/cal_date_test.c` fails if the two
  ever drift apart.

When the clock is later set, the app **recovers by itself**: its once-a-second
tick sees the date appear, moves to that month, marks and selects today, and
enables the button. Nothing has to be reopened. The reverse is handled the
same way - if the date stops being valid, the today mark goes and the notice
returns, but the month on screen and the selection are left where the user put
them rather than yanked away.

## Where the date comes from

PocketCalendar reads no clock. The shell already reads one once a second for
its status bar, through PocketClock's single clock reader and its validity
rule, so the app asks the shell instead:

```c
/* app.h */
int64_t pocketos_shell_system_day(void);   /* YYYYMMDD local, or -1 when unset */
```

Three lines in `ui/shell/shell.c` over `clock_runtime_now()->wall`. This is the
same arrangement as `pocketos_shell_radio_state()`: the shell is already
asking, and a second reader would be a second answer to what day it is.

`-1` is not 1970. It is the shell saying the board does not know, and every
caller has to show that rather than draw the epoch.

`tests/calendar_lint.sh` fails the build if anything under `apps/calendar/`
calls `time`, `localtime`, `gmtime`, `mktime`, `strftime` or `clock_gettime`,
or includes a PocketClock header.

## Today, and the day you picked

They are different things and are drawn differently, so neither leans on
colour alone (DS §2):

| | Grid | In words |
| --- | --- | --- |
| **Today** | the number in `accent_primary`, with an accent dot beneath it | " - Today" after the date, when the selected day is today |
| **Selected** | a 2 px `focus` outline around the cell (DS §7, "focus / selected outline") | the date itself, written out |

A day can be both at once, and the two marks do not collide. The selection is
a date, not a position: stepping to another month leaves it where it is, which
is why there is no 31 January to clamp into February, and why browsing away
and back finds it undamaged. Midnight moves today and leaves the selection
alone.

## Architecture

Three files, and the split is the point: everything that decides a date is
pure and tested, and what is left is a screen.

| File | What it is |
| --- | --- |
| `apps/calendar/cal_date.c/.h` | Leap years, month lengths, the Monday-first weekday, month stepping, the grid, the names and the formatting. No state. |
| `apps/calendar/cal_view.c/.h` | Which month is shown, what is selected, whether there is a today, and every transition between those. No LVGL. |
| `apps/calendar/cal_app.c` | LVGL: the cells, three buttons, and the once-a-second question to the shell. |

### The weekday

`cal_weekday()` works the weekday out with integer arithmetic - days-from-civil
(Hinnant), then `(days + 3) mod 7` because 1970-01-01 was a Thursday, which is
column 3 when Monday is column 0. Not `localtime_r`, for two reasons: the grid
needs the weekday of the first of an arbitrary displayed month, which may be
years away and is a timezone question this app has no business asking; and
`strftime` in a pure module would be a clock call in a file that must not have
one.

That leaves one risk - that the calendar and PocketClock could disagree about
what day a date is - and one test closes it: `cal_date_test` walks 20000
consecutive days from `CLOCK_WALL_VALID_FROM`, breaks each down with
PocketClock's own `clock_wall_from_epoch`, and asserts the two weekdays agree.
It runs under `TZ=UTC`, like `clock_time_test`.

### Six rows, always

A 31-day month that starts on a Sunday spans six leading blanks and 31 days:
37 cells, six rows. A 28-day February that starts on a Monday needs four. The
grid is always six so that it does not change height as the user browses.

An out-of-month cell is **blanked, not hidden**. `LV_OBJ_FLAG_HIDDEN` takes an
object out of LVGL's flex layout, which would close the gap and shift the rest
of the week left - and that leading gap is exactly what puts the first of the
month in the right column. The cell keeps its place, is drawn as nothing, and
takes no taps. `calendar_lint.sh` checks this, because it is the kind of
tidying that looks harmless.

### Motion

None. A month change is a relabelling of cells that are already there, applied
immediately. There is nothing for the reduced-motion setting to switch off,
which is what DS §12 asks of a reduced-motion build and is no worse for anyone
else. The lint fails on `lv_anim` or a timer of the app's own.

## Layout

The shell gives the app 528 × 1060 px below its status bar and header
(568 − 2×20 padding; 1232 − 56 − 72 − 24/20). Portrait, as measured in the
simulator (y from the top of the screen; unchanged by the landscape work):

| Block | Height | Where |
| --- | --- | --- |
| month, with prev and next (72 × 64 slabs) | 64 | 152..215 |
| weekday headings, mono caption | 32 | 236..267 |
| six weeks of 72 px cells, 4 px apart across and 8 px down (7×72 + 6×4 = 528) | 472 | 276..747 |
| the selected date, or the "Date not set" notice | 68 one line; 184 with the notice and a day (Normal) | from 768 |
| Today | 64 | 856..919 with one line above it |

72 px square cells clear the 64 px minimum touch target of caveat C1 with room
for a fingertip that is not centred. Prev and next are 72 × 64 rather than DS
§7's 56 for paired buttons, for the same reason. Nothing scrolls.

Landscape, on the unit's panel with its 30 px corners (the body 20..1211 ×
152..547, its foot 10 px short of the corner squares):

| Block | Where |
| --- | --- |
| weekday headings, 24 px | 20..605 × 152..175 |
| six weeks, cells 80-81 × 56-57, 4 px apart both ways | 20..605 × 180..537 |
| month, with prev and next (72 × 64 slabs) | 626..1211 × 152..215 |
| the panel | 626..1211 × 236..453 |
| Today | 626..1211 × 474..537 |

With square corners the foot is 547: cells 58 tall, the panel 228, Today
484..547. Under the wide floors (1076 px across, 384 px tall) the portrait
layout is kept whole and scrolls in the body.

## Storage

**None.** There are no events to keep, and a selection is not worth a file:
opening the app lands on today, which is the right answer every time. There is
no store to corrupt and no migration to write. `calendar_lint.sh` fails the
build if anything under `apps/calendar/` opens a file.

## Tests

| Test | What it covers |
| --- | --- |
| `tests/cal_date_test` (127 checks, `TZ=UTC`) | Leap years including 1900, 2000, 2100, 2400; every month length; weekday vectors; the 20000-day cross-check against PocketClock; the fallback month pinned to `CLOCK_WALL_VALID_FROM`; Dec↔Jan and ±12/±25 month steps; Monday-first grid offsets over 96 months; a 31-day month starting Sunday; the date and month strings |
| `tests/cal_view_test` (98 checks) | The unset state and that no month browsed has a today; unset → set recovery; set → unset; midnight, including across a year boundary; selection surviving month navigation; Today; selecting days a month does not have; the exact selected-date strings |
| `tests/calendar_lint.sh` (48 checks) | No LVGL in the pure files, no filesystem, no clock, no PocketClock header, no keyboard, no animation, no store; today marked only when the date is set; blank cells blanked rather than hidden; the selected role carries no fill; 17 banned scheduler words; registered exactly once in the launcher; and the layout rules - shaped from the body and never the orientation, built once and only shaped after, one frame shaped again on a size change and unhooked on the way out, the corner clearance read from the display geometry, and the wide floors |
| `cal_app_test` (1,317 checks) | The app under a real LVGL pointer device, hosted as the shell hosts it: it opens on the right month, a finger on previous/next steps it (including across both year boundaries), a finger on a day selects it, Today returns and marks, an empty cell is not a target and taps on one change nothing, the unset-date state disables Today while browsing and selecting still work, no month browsed without a date claims a today, a tick recovers the app when the clock is set, midnight moves the mark and not the selection, and five open/close rounds leave nothing behind. Then the layout: portrait pinned to its v0.0.10 rectangles in Normal and Outdoor, with square and 30 px corners; September, a six-week August, a four-week February, the longest date in words and no date with a day picked, in portrait and landscape, rounded and square corners, Normal and Outdoor, each checked for its shape, every target (arrows and Today 64 × 64; days 72 square in portrait, at least 64 × 56 in landscape) on no other and inside the body and the safe area, every label whole, the panel saying all it says without scrolling, seven columns under their headings and six even weeks, and today's dot inked below its number; the display turned under the open app three times (month, selection and today kept, no object more or less, portrait back to the pixel), every control pressed in landscape, the date arriving in landscape; the width floor at 1075 and 1076 px, the height floor at 383 and 384 px, a wide body taller than it is wide, and a 100 px landscape body that keeps portrait and scrolls every target into view |
| `tests/calendar_shell_test.sh` (21 checks) | Runs `cal_app_test`, then the wiring around it: the shell answers the date from the reading it already took and with -1 when there is none, the app reads it exactly twice (create and tick), the selected outline is a shared role defined once, and the real shell opens Calendar in portrait and in landscape without a fault or a byte written |

The first three run in `make test`. The last two need a display and the
CMake-built shell, so they run the way every other app's do:

```sh
SHELL_BIN=~/work/pocketos-build/shell/pocketos-shell bash tests/calendar_shell_test.sh
```

`cal_app_test` supplies `pocketos_shell_system_day()` itself, which is what
makes an unset clock, the moment it is set and the moment it stops being
valid all reachable without touching the host's clock or waiting for
midnight. That seam exists because the app asks the shell for the date
instead of reading one.

## Hardware

Portrait shipped in v0.0.9 and v0.0.10. The tap path was tested before any
board saw it: `cal_app_test` drives it through a real LVGL pointer device, so
a board is the first test of the *panel* rather than the first test of
tapping at all.

What only hardware can settle:

- 72 px cells and 3-letter weekday headings at mono 14 on the real panel, and
  whether a thumb lands on the cell it means to.
- Accent-on-slab and the 2 px focus outline in Outdoor and Night modes.
- Whether the accent dot reads at arm's length.
- The unset-clock path in its natural setting: the app open across the moment
  SNTP sets the date.

**Landscape on unit A: PASS, 2026-09-17**
(`docs/hardware/CALENDAR_LANDSCAPE_GATE.md`). The rebased build was installed
over the serial console with only the shell service restarted; Calendar
rendered in both orientations on the panel, wrote nothing, and left the unit
with 0 ERROR and 0 WARN. The question the gate was for - whether an 80 × 56
day, wider than tall, is as easy to hit with a thumb as a 72 px square - the
owner answered by hand: yes. Outdoor in landscape was not exercised on the
unit; it rests on the simulator and `cal_app_test`.

Found in the simulator while making the landscape layout, and left alone:

- **Before it, landscape was portrait stretched and cut short.** On master
  (`aaad9f4`) the grid stayed 528 px wide at the left of a 1192 px body, the
  month row stretched across the whole width, the fourth week was cut at the
  foot of the body and the last two weeks, the panel and Today were below it -
  Today could not be pressed without scrolling - and the weeks at the foot
  drew into the rounded corners.
- **The selected outline is cut by its week.** The 2 px `focus` outline is
  drawn outside the cell (DS §7), and the week's row, exactly as tall as its
  cells, clips it: a selected day shows the outline on its left and right
  only, and a selected Monday only on its right, because the grid starts at
  the body's edge. The same in portrait since v0.0.9, and the same in
  landscape; the "Selected" row of the table above still holds, but the
  outline is two bars rather than a ring.

## Not in this app

No appointments, events, reminders, alarms, notifications, recurrence, notes
attached to dates, sync, cloud calendars, Exchange, Microsoft 365 or Google,
importing or exporting, and no week, day or agenda views. No timezone picker
and no time setting - that is PocketClock's territory, and it does not have it
either.

`tests/calendar_lint.sh` fails the build if `appointment`, `event`, `reminder`,
`alarm`, `notification`, `recurrence`, `rrule`, `ical`, `agenda`, `sync`,
`exchange`, `gcal`, `attendee`, `invite`, `timezone`, `ntp` or `cron`
machinery appears under `apps/calendar/`. It is the mirror image of the scope
check in `tests/clock_lint.sh`, which keeps calendars out of the clock; this
one keeps appointments out of the calendar, which is the direction a month
view drifts if nobody is watching.
