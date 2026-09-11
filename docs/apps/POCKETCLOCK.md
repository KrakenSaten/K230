# PocketClock

The time, an alarm, a stopwatch and a timer. A small system utility, not a
calendar: it has no events, no recurrence rules beyond "daily" and
"weekdays", and no notion of a date you can schedule something on.

Status: **v0.0.8. Host-validated, not yet run on hardware.**

## What it is

Four panes behind four tabs, on one screen.

- **Clock.** The local time, large, with the date under it. When the board
  does not know what time it is, this says so instead (below).
- **Alarm.** Up to eight alarms. Each has a time, a repeat (Once, Daily,
  Weekdays) and an optional label. A tap on the row switches one on or off;
  the trailing button deletes it, after a confirmation.
- **Watch.** A stopwatch with laps, to hundredths of a second.
- **Timer.** A countdown set in minutes and seconds, up to 99:59.

When something goes off, the whole panel becomes the alert: what is ringing,
which alarm it was, and Stop — with Snooze beside it for an alarm.

## The two things it cannot do, and says so

**1. This board has no clock that survives a power cut.** There is no
battery-backed RTC (`docs/hardware/T-DISPLAY-K230.md`: "no RTC"), so every
boot starts at the epoch and stays there until something sets the time. The
boot logs show it plainly: `1970-01-01T00:00:16`.

So PocketClock decides whether the wall clock is a time at all. Anything
before 2024-01-01 is not; it is a board that has just come up. In that state
the clock face reads `--:--`, the date is blank, an explanation appears under
it, and **no alarm fires**. A plausible-looking wrong time would be worse
than an obvious blank one, and an alarm that trusted the reading would decide
that fifty-five years had gone by and every alarm in the list was overdue.

The stopwatch and the timer are unaffected. They measure elapsed time on
`CLOCK_MONOTONIC` and never look at the date.

**2. An alarm rings while PocketClock is open.** The v0.1 app lifecycle has
no background: an app is created when it is opened and destroyed when it is
left (ADR-002). Nothing runs a clock behind the launcher, so nothing can ring
there. The Alarm pane says this in as many words rather than letting the
owner find out at 07:30. Alarms themselves are saved and survive a reboot;
what does not survive is anything watching for them.

## The alert hardware

The brief was to find out what this device can actually do to get your
attention, and to classify it rather than assume it.

| Output | On this unit | Class |
| --- | --- | --- |
| Buzzer or piezo | **None.** No buzzer, piezo or beeper appears in `vendor/T-Display-K230/k230_bsp/docs/HARDWARE_PINMAP.md` or the board schematic, and no pin is assigned to one | DOCUMENTED |
| Vibration motor | **None.** Same two sources, same answer: no haptic driver, no motor pad | DOCUMENTED |
| Audio out | **Exists, never exercised.** The K230's internal INNO codec is wired to the 3.5 mm headphone jack | DOCUMENTED |
| Whether that audio path works | Nobody knows. It has never been opened, no ALSA device has been confirmed on the running image, and this milestone did not try | UNRESOLVED |
| MAX98357A amplifier | **Not present.** It is a part of the nRF52840 base board, which unit A does not have | DOCUMENTED |

So the alert PocketClock ships is the screen, and the app says so on the
Alarm pane and again on the ringing screen: *"This board has no buzzer and no
vibration motor, so an alert appears on screen and makes no sound."* Driving
the headphone jack for an alarm nobody has plugged headphones into would be
inventing an alert, not providing one.

`apps/clock/clock_alert.[ch]` is the seam. It declares which channels exist
(`CLOCK_ALERT_VISUAL` and nothing else, today) and forwards begin and end to
a backend. When the audio path is brought up, it becomes a backend registered
there and nothing in the engine, the store or the views changes.
`tests/clock_lint.sh` fails the build if anything under `apps/clock/` opens an
audio, GPIO or PWM device, and `tests/clock_engine_test.c` checks that the
shipped backend claims no sound and no haptics.

## Architecture

```
clock_time.c     the only file that reads a clock (CLOCK_REALTIME +
                 CLOCK_MONOTONIC), and the only one that knows about
                 local time
      |   struct clock_now { wall, mono_ms }
      v
clock_engine.c   every timing rule: what counts as knowing the time,
                 when an alarm fires and when it must not, snooze,
                 the stopwatch, the countdown. Pure: no I/O, no LVGL,
                 no clock of its own
      |
      +--> clock_store.c    alarms and the timer duration, on disk
      +--> clock_alert.c    what the device can do to get attention
      |
      v
clock_app.c      four panes and three screens. Draws what the engine
                 says; decides nothing about time
```

`tests/clock_lint.sh` holds that shape: the engine, the store, the clock
reader and the alert are free of LVGL; only the store touches the filesystem;
only `clock_time.c` calls a clock; and the refresh timer writes nothing.

### The two clocks

Keeping them apart is most of the engine's job.

- **Alarms are wall-clock.** "07:30" is a statement about the world, so an
  alarm must follow the wall clock when it is corrected. Correct it forward
  past an alarm's time and that alarm rings, late but today's.
- **The stopwatch, the timer and snooze are monotonic.** "Ninety seconds from
  when I pressed start" is a statement about elapsed time, and setting the
  clock must not lengthen or shorten it. The timer holds a monotonic
  *deadline*, never a wall-clock timestamp it subtracts from.

### Firing exactly once

The app steps the engine ten times a second, and every one of those steps
sees the same 07:30. An alarm carries the local day it last fired on, so:

- it fires on the first step of its minute and stays quiet for the rest;
- acknowledging it does not re-arm it for today;
- a clock corrected backwards past its time does not ring it again;
- a **Once** alarm switches itself off when acknowledged, because once has
  happened;
- the moment the wall clock becomes real, every alarm whose time already
  passed today is marked as done for today. It did not ring while the board
  had no idea what time it was, and it must not ring in a burst now;
- an alarm **added** for a time that has already gone by today means the next
  occurrence. Setting 06:30 at 06:39 must not go off while your finger is
  still on Add.

Only one thing rings at a time. If an alarm and the timer come due in the
same step, the timer takes it and the alarm rings as soon as the timer is
acknowledged.

## Storage

`$POCKETOS_STATE_DIR/clock/clock.conf`, default
`/var/lib/pocketos/clock/clock.conf`. One small text file:

```
pocketclock 1
timer 200
alarm 1 7 30 1 Wake up
alarm 0 22 45 0
```

- **Writes are atomic**: temp file, `fsync`, `rename`, the same as Notes. A
  reader sees the previous set of alarms or this one, never half of either.
- **Written on change, never on a tick.** An alarm list is a setting. The
  ten-a-second refresh writes nothing, and `tests/clock_lint.sh` checks that
  the refresh callback contains no call to the store.
- **A damaged file is refused whole**, not half-read into alarms nobody set:
  a wrong magic line, an unknown keyword, an out-of-range field, a line
  longer than the reader, or more alarms than the engine holds all leave the
  engine as it was.
- **What is deliberately not stored**: the stopwatch, a running countdown,
  and whether an alarm has already rung today. The first two are elapsed time
  on a clock that does not survive a power cut, so a stored number would
  become a lie the moment the board went off. The third is a fact about
  today, and the engine works it out again on its first valid reading.

## Tests

| Test | What it covers |
| --- | --- |
| `tests/clock_engine_test.c` | 156 checks: validity and the boot with no RTC, firing once however often it is stepped, Once/Daily/Weekdays, midnight crossings, clock jumps forwards and backwards, snooze on the monotonic clock, adding an alarm in the past, the dense alarm list and its two index references, the stopwatch over 25 days of milliseconds, the timer's single expiry and its 23-hour range, one ring at a time, the formatters, every null argument, and the alert seam |
| `tests/clock_time_test.c` | 39 checks: the validity threshold, the local-date arithmetic across midnight, month and year ends, and that an unset clock never produces a digit |
| `tests/clock_store_test.c` | 76 checks: the round trip, what is deliberately not stored, eleven kinds of damaged file, the atomic overwrite, and label storability |
| `tests/clock_app_test.c` | 79 checks: the app under a real LVGL pointer and the real touch keyboard, against a real store — tabs, adding an alarm by tapping the steppers and typing its label, toggling, the delete confirmation, persistence across closing the app, the stopwatch and the timer |
| `tests/clock_lint.sh` | 26 checks: the layering above, the atomic write, no keyboard of its own, no invented hardware, and no calendar machinery |

**Nothing in the engine tests sleeps.** Both clocks are handed in as numbers,
so a day, a midnight crossing, a clock correction and a nine-minute snooze
all take zero seconds. The only real time anyone waits for is the thirty
milliseconds `clock_app_test.c` spends proving a running stopwatch moves,
because that one reads the real monotonic clock.

## Not in this app

No NTP or time-setting UI, no timezone picker, no world clocks, no calendar
or events, no recurrence beyond the three repeats, no per-alarm sounds, no
gradual wake, no bedtime or sleep tracking. `tests/clock_lint.sh` fails the
build if `ntp`, `timezone`, `recurrence`, `rrule`, `cron` or `ical`
machinery appears under `apps/clock/`.

Setting the clock is out of scope for this milestone: until PocketOS grows a
way to do it, `date -s` over SSH is the way, and the app is explicit that
until then it does not know the time.
