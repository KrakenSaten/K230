# PocketCalendar

A month, and which day it is. A calendar, not a scheduler: it has no events,
no reminders, no recurrence and nothing to sync, and it stores nothing at all.

Status: **v0.0.9 in development. Host-tested and built for the target; not yet
run on hardware.**

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
(568 − 2×20 padding; 1232 − 56 − 72 − 24/20).

| Block | Height |
| --- | --- |
| month, with prev and next (72 × 64 slabs) | 64 |
| weekday headings, mono caption | 32 |
| the grid: 7 × 72 px cells, 4 px apart (7×72 + 6×4 = 528) | 452 |
| the selected date, or the "Date not set" notice | ~96 |
| Today | 64 |

72 px square cells clear the 64 px minimum touch target of caveat C1 with room
for a fingertip that is not centred. Prev and next are 72 × 64 rather than DS
§7's 56 for paired buttons, for the same reason. Nothing scrolls.

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
| `tests/calendar_lint.sh` (40 checks) | No LVGL in the pure files, no filesystem, no clock, no PocketClock header, no keyboard, no animation, no store; today marked only when the date is set; blank cells blanked rather than hidden; the selected role carries no fill; 17 banned scheduler words; registered exactly once in the launcher |

All three run in `make test`. The app itself needs a display; it has no
`*_shell_test.sh` yet.

## Hardware

Not yet run on a board. What only hardware can settle:

- 72 px cells and 3-letter weekday headings at mono 14 on the real panel.
- Accent-on-slab and the 2 px focus outline in Outdoor and Night modes.
- Whether the accent dot reads at arm's length.
- The unset-clock path in its natural setting: the app open across the moment
  SNTP sets the date.

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
