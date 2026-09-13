# PocketClock

The time, an alarm, a stopwatch and a timer. A small system utility, not a
calendar: it has no events, no recurrence rules beyond "daily" and
"weekdays", and no notion of a date you can schedule something on.

Status: **v0.0.8. Validated on unit A, 2026-09-11, in the release image
(build `03851f5`).**

## What it is

Four panes behind four tabs, on one screen.

- **Clock.** The local time, large, with the date under it. When the board
  does not know what time it is, this says so instead (below).
- **Alarm.** Up to eight alarms. Each has a time, a repeat (Once, Daily,
  Weekdays) and an optional label. A tap on the row switches one on or off;
  the trailing button deletes it, after a confirmation. They ring whether or
  not this app is open - see below.
- **Watch.** A stopwatch with laps, to hundredths of a second.
- **Timer.** A countdown set in minutes and seconds, up to 99:59.

When something goes off, the whole panel becomes the alert - over whatever
was on screen, app or launcher - showing what is ringing, which alarm it
was, and Stop, with Snooze beside it for an alarm. That alert belongs to the
shell, not to this app.

## The one thing it cannot do, and says so

**This board has no clock that survives a power cut.** There is no
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

The status bar is held to the same rule, so the corner of every screen in
PocketOS reads `--:--` rather than 01:00 while the Clock app says the time
is not set. Whichever of the two was lying, the one nobody is looking at
would have won, because it looks like a clock.

## Who runs the alarms

**The shell does, and PocketClock is the client.** An alarm that only rings
while its app is open is not an alarm, and the v0.1 lifecycle has no
background apps - one is created when it is opened and destroyed when it is
left (ADR-002) - so the engine does not live in the app.

It did not become a daemon either. A service would need a binary, an init
script, a supervisor entry and an IPC push back to the shell to draw the
alert; the shell is already the process with the panel, a once-a-second tick
and the one-instance pattern the touch keyboard set (DS §17.4). So:

| | Owner |
| --- | --- |
| The one `struct clock_engine` | `apps/clock/clock_runtime.c`, created and stepped by `ui/shell/shell.c` |
| Stepping it | the shell tick, once a second, and nothing else |
| The alert on screen | `ui/shell/shell_alarm.c`, one full-panel sheet built hidden at boot |
| Alarm configuration and display | PocketClock |
| Loading and saving | the runtime, through the same `clock_store.c` |
| Timing rules | still `clock_engine.c`, unchanged |

What that buys: an alarm set in PocketClock rings over the launcher,
PocketNotes, PocketTimber or PocketClock itself, and Stop works from any of
them. The countdown finishes in the background the same way. The stopwatch
now also keeps running with the app shut, which is not a feature that was
asked for but is what one engine in one place means - and is what a
stopwatch should do.

**PocketClock has no ringing screen of its own.** There is one alert and the
shell owns it; `tests/clock_shell_test.sh` fails the build if a second one
appears. The pattern is normative as **DS v0.1 §18 (Amendment B)**, so the
next thing with something urgent to say uses this alert rather than
inventing another. What does not survive is the device being switched off: the alarms
are on disk, but nothing is running to watch for them.

The app still refreshes ten times a second while it is open, because the
stopwatch shows hundredths - but that refresh only *reads* the clock. One
stepper means an alarm cannot fire twice because two callers both advanced
it, and it means having the app open changes nothing about when an alarm
goes off. `tests/clock_lint.sh` checks that `clock_engine_step` has exactly
one caller in the tree.

## The alert hardware

The brief was to find out what this device can actually do to get your
attention, and to classify it rather than assume it.

| Output | On this unit | Class |
| --- | --- | --- |
| Buzzer or piezo | **None.** No buzzer, piezo or beeper appears in `vendor/T-Display-K230/k230_bsp/docs/HARDWARE_PINMAP.md` or the board schematic, and no pin is assigned to one | DOCUMENTED |
| Vibration motor | **None.** Same two sources, same answer: no haptic driver, no motor pad | DOCUMENTED |
| Audio out | **Exists, never exercised.** The K230's internal INNO codec is wired to the 3.5 mm headphone jack | DOCUMENTED |
| Whether that audio path works | Nobody knows. It has never been opened, no ALSA device has been confirmed on the running image, and this milestone did not try | UNRESOLVED |
| MAX98357A amplifier | **Not on the main board**; it is on the nRF52840 base board. *Corrected 2026-09-13:* a second unit was opened and has that board, the amplifier and a built-in speaker (docs/hardware/AUDIO_HARDWARE_MAP_2026-09-13.md). Whether unit A has the base board is not established: the earlier "not connected" reading scanned the wrong I2C bus. PocketClock still makes no sound | DOCUMENTED (main board); PHYSICALLY CONFIRMED (second unit) |

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
      ^
      |   the only caller of clock_engine_step()
clock_runtime.c  the one engine, owned and stepped by the shell
      |
      +--> clock_store.c    alarms and the timer duration, on disk
      +--> clock_alert.c    what the device can do to get attention
      |
      +--> shell_alarm.c    the one full-panel alert, in ui/shell
      +--> clock_app.c      three screens and four panes. Draws what the
                            runtime says; decides nothing about time
```

`tests/clock_lint.sh` holds that shape: everything but the app is free of
LVGL; only the store touches the filesystem; only `clock_time.c` calls a
clock; `clock_engine_step` has exactly one caller; the app owns no engine;
and the refresh timer neither writes to storage nor advances anything.

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
  still on Add;
- so does an alarm **switched back on** at or after its time today: turning a
  07:00 alarm on at 15:00 arms it for tomorrow rather than ringing it the
  moment its row is tapped;
- and the same applies to the shell itself restarting. The runtime starts
  with a fresh engine, so its first valid reading marks everything already
  due today as done: a shell that came back at 07:31 does not ring the 07:30
  alarm it was not there for. That is the safe direction, and the same rule
  as the boot case above.

Only one thing rings at a time, and nothing that comes due while something
else is ringing is lost (DS §18.6). A countdown that finishes, or a snooze
that runs out, while an alarm is ringing waits and rings as soon as that
alarm is stopped or snoozed; an alarm that comes due during a ring waits the
same way. When several are waiting, the countdown goes first, then snoozes
in the order they ran out, then alarms. Every alarm keeps its own snooze, so
snoozing a second alarm does not cancel the first one's, and Stop, switching
an alarm off or deleting it cancels that alarm's snooze and no other.

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
| `tests/clock_engine_test.c` | 210 checks: validity and the boot with no RTC, firing once however often it is stepped, Once/Daily/Weekdays, midnight crossings, clock jumps forwards and backwards, snooze on the monotonic clock, adding an alarm in the past and switching one back on after its time, the dense alarm list and the snooze that moves with its alarm, the stopwatch over 25 days of milliseconds, the timer's single expiry and its 23-hour range, one ring at a time and nothing lost behind it (a countdown ending under an alarm, snoozes up under an alarm or a countdown, overlapping snoozes, and cancelling one without the others), the formatters, every null argument, and the alert seam |
| `tests/clock_time_test.c` | 39 checks: the validity threshold, the local-date arithmetic across midnight, month and year ends, and that an unset clock never produces a digit |
| `tests/clock_store_test.c` | 76 checks: the round trip, what is deliberately not stored, eleven kinds of damaged file, the atomic overwrite, and label storability |
| `tests/clock_runtime_test.c` | 85 checks: the thing the runtime exists for — an alarm ringing with no app in sight, once, with the shell told exactly once; a one-shot acknowledgement reaching the disk; snooze, the countdown and a wall clock that is never set; a countdown that ends under a ringing alarm and rings after Stop; two snoozes waiting their turn; clock jumps; and that a read does not advance anything |
| `tests/clock_app_test.c` | 118 checks: the app under a real LVGL pointer and the real touch keyboard, against a real store — tabs, adding an alarm by tapping the steppers and typing its label, toggling, switching an alarm back on after its time without it ringing, the delete confirmation, persistence across both closing the app and restarting the shell, the stopwatch, the timer, and the shell alert firing with the app shut, over the app, and for a countdown |
| `tests/clock_lint.sh` | 30 checks: the layering above, one stepper, the atomic write, no keyboard of its own, no invented hardware, and no calendar machinery |
| `tests/clock_shell_test.sh` | 16 checks: the shell starts and steps the runtime, builds exactly one alert, no app builds another, PocketClock has no ringing screen left, and the status bar uses the validity rule instead of formatting the time itself |

**Nothing in the engine tests sleeps.** Both clocks are handed in as numbers,
so a day, a midnight crossing, a clock correction and a nine-minute snooze
all take zero seconds. The only real time anyone waits for is the thirty
milliseconds `clock_app_test.c` spends proving a running stopwatch moves,
because that one reads the real monotonic clock.

## Hardware

**Validated on unit A, 2026-09-11**, from the flashed v0.0.8 release image,
build `03851f5` (`docs/hardware/V0.0.8_RELEASE_SMOKE.md`, sections 2 and 3).
What the board stored and logged is VERIFIED over SSH; what the panel showed
is the operator's.

| Check | On unit A |
| --- | --- |
| 1. No time, no invented time | PASS. With the wall clock at 1970 the status bar and the face read `--:--`, the "Time not set" card showed, and the Alarm pane said the time was not set |
| 2. Setting the time | PASS, by the image's `S48sntp` rather than `date -s`: both went to 15:30. After the later power cycle, B (15:42, already past) did not ring when NTP set the time at 17:00 |
| 3. Alarm, app open | PASS. A rang over Clock at 15:36; Stop cleared it, and the Once alarm switched itself Off on disk |
| 4. Alarm, app shut | PASS. C rang over the launcher at 16:03 |
| 5. Alarm in another app | PASS. C's snooze came back over the Notes editor with the keyboard up; the keyboard went away, was not put back after Stop, and the text was unchanged |
| 6. Snooze | PASS. Nine minutes, 16:03 to 16:12, with Clock shut |
| 7. Acknowledged once | PASS for Once alarms: neither A nor C came back. A Daily alarm was not exercised |
| 8. Reboot with an alarm set | Partly. The three alarms and the timer came back byte-identical after a restart and after a power cycle, and the shell loaded all three; no alarm was watched ringing after a reboot |
| 9. Countdown in the background | PASS. A 2:00 timer finished over Timber with Stop and no Snooze, and Timber was untouched |
| 10. Nothing rings without a clock | Not run |

The checks as planned, for a re-run:

1. **Cold boot, no time set.** The status bar corner reads `--:--`, and so
   does the Clock face. Neither invents a time.
2. **Set the time**: `ssh root@<unit> date -s "2026-09-11 07:25"`. Both go
   to 07:25 within a second. No alarm goes off from the jump.
3. **Alarm in two minutes, app open.** Add it, stay on the Alarm pane, wait.
   The alert covers the whole panel; Stop clears it.
4. **Alarm in two minutes, app shut.** Add it, go back to the launcher, wait
   there. The alert appears over the launcher. This is the whole point.
5. **Alarm in two minutes, in another app.** Add it, open PocketNotes and
   start typing. The alert appears over Notes and takes the keyboard away.
6. **Snooze.** Let one ring, tap Snooze, confirm it comes back nine minutes
   later - with the app still shut.
7. **Acknowledged once.** After Stop, it does not come back that day.
8. **Reboot with an alarm set.** `reboot`, do not open Clock, set the time,
   and confirm the alarm is still armed and still rings.
9. **Countdown in the background.** Start a one-minute timer, leave Clock,
   and confirm "Timer finished" appears with Stop and no Snooze.
10. **Nothing rings without a clock.** Reboot, do not set the time, and
    confirm an armed alarm stays silent all the way past its hour.

Found on the board:

- **The face is UTC.** The image configures no time zone, so "the local
  time" above is UTC on the shipped image, and so is every alarm.
- **The time comes from the network.** `S48sntp` and `S49ntp` set it at boot
  when there is one, within 115 s of a cold boot on the bench LAN. Linux
  refuses a wall time earlier than the uptime, so to put a running board
  back into the unset state, stop ntpd and use
  `date -u -s '1970-01-02 00:00:00'` rather than a time on 1 January.
- **The minute stepper wraps within the hour.** From :58, Minute +5 gives :03
  of the same hour, which has already passed, so the alarm arms for the next
  day. So does an alarm added during its own minute, by the rule above.
- **The label's keyboard Done does nothing**; tap Add. (KNOWN_ISSUES)

## Not in this app

No NTP or time-setting UI, no timezone picker, no world clocks, no calendar
or events, no recurrence beyond the three repeats, no per-alarm sounds, no
gradual wake, no bedtime or sleep tracking. `tests/clock_lint.sh` fails the
build if `ntp`, `timezone`, `recurrence`, `rrule`, `cron` or `ical`
machinery appears under `apps/clock/`.

Setting the clock is out of scope for this milestone. The image's boot-time
NTP sets it when the board has a network; otherwise `date -s` over SSH is the
way, and the app is explicit that until then it does not know the time.
