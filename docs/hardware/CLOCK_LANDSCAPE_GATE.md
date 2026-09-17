# Clock in landscape: unit A gate

Branch `feature/clock-landscape`, first from origin/master `aaad9f4` (code
`0d04b45`, docs `8d5dcc1`), then **rebased onto origin/master `dcf906d`**
(Settings §24 and System §25 accepted): code `3f4b584`, docs `912849c`, this
sheet `8aec2bc`. The build on unit A is `8aec2bc`. VERSION stays 0.0.10.

**Result: PASS on unit A, 2026-09-17** - host and simulator validation before
the rebase (done host and simulator only, by the product owner's instruction,
without touching unit A), revalidation after it, a userspace deployment of
the rebased build with health checks over the serial console, then the
product owner's physical check (last sections). **The product owner ACCEPTED
the work and DS Amendment J (§26) on 2026-09-17.** Not merged.

Scope: Clock only. No other app, no rotation policy, no keyboard presence
logic, no shell, PocketUI or alarm-alert change, no boot splash, no first-boot
or vendor-launcher behaviour, no Phase 4, no new Clock feature.

## What changes

Clock shapes its three screens from the size of the body it is given (DS
§26.1): tall in portrait, as before; wide in landscape, when the body
is wider than tall and at least 1076 px across. Wide keeps the tabs across the
top and puts each pane in two halves (586 px, 20 px gutter) that scroll on
their own: the clock face across both when the time is set, beside "Time not
set" when it is not; the alarms beside Add alarm and the notes; the running
stopwatch and the countdown filling the left half beside their buttons, laps
and steppers. The new-alarm form begins with the label field and a 288 px
Cancel | Add rail, with the time beside Hour, Minute and Repeat under them,
so that above the landscape keyboard (a 1192 x 100 body) the field, a refused
label's reason, Cancel and Add are all in view. The confirmation keeps its
portrait width, centred. In portrait the only change is with rounded corners:
the clock face and the laps end 10 px higher (1201). Details: "the layout" in
`apps/clock/clock_app.c`.

On master, landscape was portrait stretched and cut at the foot: the Timer's
Start and Cancel were out of reach, the form lost its label field, Cancel and
Add (no alarm could be added), the Alarm pane's notes were clipped, and the
foot drew into the rounded corners.

## Host validation

Before the rebase, from fresh clones of `8d5dcc1` and, for comparison, master
`aaad9f4`.

| Check | Result |
| --- | --- |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,741 ok, 0 FAIL, 0 warnings (master: 3,733; the 8 new `clock_lint` checks); clean checkout after |
| SDL simulator build | rc 0, 0 warnings |
| 20 shell and UI test scripts | 494 ok, 0 FAIL, every script rc 0 (master: the same 20 and 494 - Clock's new checks are inside `clock_app_test`, one line of `clock_shell_test.sh`) |
| `clock_app_test` | 778 checks, 0 failures (master: 118) |
| `clock_lint.sh` | 38 checks, 0 failures (master: 30) |
| `clock_engine_test` / `clock_time_test` / `clock_store_test` / `clock_runtime_test` | 210 / 39 / 76 / 85 checks, 0 failures: unchanged |
| `calc_app_test`, `notes_app_test`, `settings_app_test`, `display_geometry_shell_test.sh`, `shell_alarm_test` | 456 / 1,138 / 81 checks / 69 ok / 61 checks, 0 failures: the other apps and the alert unaffected |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 4 warnings, all `vendor/ggwave`; 0 first-party |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; `Doors 0.0.10 build 8d5dcc1`; `clock_app.c` compiled in; the app test is not a target there |
| `clock_app_test` against `aaad9f4`'s `clock_app.c` | fails: 96 failures, then an LVGL assert on the tree master does not have (no frame, no columns) |

Mutations of `clock_app.c` (each must make `clock_app_test` fail): 44 of 45
caught, counting master's `clock_app.c` as one. The miss - not unhooking the
frame's size handler in destroy - cannot be observed: the shell hides the
keyboard before destroy and deletes the app's objects straight after, so no
layout pass runs in between; the unhook is defensive, as in Notes. Caught: the
wide shape never chosen; always chosen; without the width floor; the floor off
by one; without the wider-than-tall test (a square body); no foot inset; the
inset applied sideways; no size handler; columns never scrollable; columns not
pressable when wide; columns pressable when tall; a column's scroll kept when
tall; columns content-tall when wide; not grown when wide; grown when tall;
the laps column not grown; panes stacked when wide; the big numbers
content-tall when wide; the clock face stacked, short, always growing, or
growing only with a time; the explanation not grown; a change of clock
validity not reshaped; the form not reversed; never scrollable; scrollable
when tall; its time part or label part stacked; its time content-tall or not
grown; the controls not grown; the field not grown; the rail full width or
200 px; the confirmation full width, left-aligned, or its track left-aligned;
no scroll to the top on a refused label; no scroll to the top on opening the
form; no gutter between halves (pane or group); a 20 px gap between screens.

What `clock_app_test` adds (the app hosted as the shell hosts it, on the
reference panel): portrait pinned to master's rectangles with square and
30 px corners; every screen - the face with and without a time, eight alarms
with the longest labels and the time-not-set notice, five laps, a set and a
running countdown, the form with the keyboard down and up and a refused
label, the confirmation - in portrait and landscape, rounded and square
corners, Normal and Outdoor, each checked for its shape, every target at least
64 x 56 and on no other, every target scrollable wholly into view inside the
body and the safe area, every label and row laid out whole, the body never
scrolled; a drag that starts in the gap between the notice and the list; the
width floor at 1075 and 1076 px and a 1192 px body taller than wide; and the
display turned under the open app with the stopwatch running with laps, a
countdown running, a half-made alarm typed with the keyboard up (time, repeat,
label, caret and focus kept, Add pressed in landscape and the alarm stored
once), a confirmation open (Delete removes that alarm only), a list scrolled
in landscape (portrait starts at the top), and six more turns - no object made
twice, the store byte for byte unchanged.

## Simulator validation

The real SDL shell with a dev-only scripted finger and a fixed wall clock
(never committed), Clock opened on a seeded store, in both orientations, with
the reference panel's 30 px corners and with square corners, Normal and
Outdoor. Stores: none; three alarms; eight alarms with the longest labels.
Clock: set (07:25) and not set; and running into an alarm that rings. Screens:
the four tabs, the new-alarm form (keyboard down, up, typed, a label refused),
the confirmation, the stopwatch with laps, the countdown running and paused,
the shell's alert ringing and stopped. 112 runs and 276 captures of the branch
and the same of master `aaad9f4`. No branch run logged an error or an LVGL
warning; the only script notes are the empty store's countdown, which has no
duration, so Start is refused as designed and there is no Pause to tap (the
same on master). On master the landscape runs also could not reach the
form's label field or the Timer's Start: they were below the foot.

| Comparison | Result |
| --- | --- |
| Portrait, square corners: objects | 69 of 69 states identical to master: every label, button and field in the same place with the same text (live time values aside) |
| Portrait, square corners: pixels | 66 of 69 captures identical below the status bar; the 3 others differ only in live values (a paused countdown's digits, stopwatch and lap times) |
| Portrait, 30 px corners | 60 of 69 captures identical; the other 9 are the clock face (5: it ends at 1201, time and date 5 px higher), the laps (3: the card ends at 1201) and one paused countdown's digits |
| Rounded corners, app screens at 30 px | branch: the foot corner squares hold only background in 108 of 108 (54 portrait, 54 landscape); master: 69 of 108 (drawn into by the face and laps in portrait, and by 31 landscape screens) |
| Landscape arrangement | halves 20..605 and 626..1211 under the tabs (152..215), from 236 to 537 above the corner squares; the form's field 20..903 x 152..215 and the rail 924..1211 (Cancel 924..1063, Add 1072..1211, 56 tall), the time 20..605 x 236..467 beside the controls 626..1211; the confirmation 352..879 x 152..331 |
| Landscape, keyboard up | body 1192 x 100 (152..251): field, Cancel and Add in view; a refused label's reason on one line at 224..249 in Outdoor (224..244 Normal), in view with them |
| Shell-owned screens | the touch keyboard in portrait reaches its full 568 px width into the corner squares, and the alarm alert's panel edge touches them in landscape, on master exactly as on the branch; neither is Clock's |

## The rebase onto `dcf906d`

Settings in landscape (§24) and System in landscape (§25) were accepted and
merged while this branch waited, so it was rebased onto origin/master
`dcf906d` before the gate (a local backup branch keeps `48208b9`).

- **Only the design system conflicted**, in one place: both branches appended
  their amendments after §23. §24 and §25 are kept exactly as accepted - the
  DS diff against `dcf906d` changes one line (§21.3's list gains "Clock: §26."
  after "System: §25.") and otherwise only adds - and Clock's amendment, written
  with its number left open, became **§26, Amendment J**. §26.4 now cites §25.3
  for the pressable scrolling column and the width floor instead of restating
  them.
- **Every Clock code and test file is byte-identical** to the validated branch
  (`apps/clock/clock_app.c`, `tests/clock_app_test.c`, `tests/clock_lint.sh`:
  the same blobs as `48208b9`). The branch changes no Settings, System or other
  application file; against `dcf906d` it touches only Clock's app, its test and
  lint, POCKETCLOCK.md, the DS and this sheet.

Revalidated from fresh clones of `8aec2bc` and, for comparison, `dcf906d`:

| Check | Result |
| --- | --- |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,761 ok, 0 FAIL, 0 warnings (master `dcf906d`: 3,753; the 8 `clock_lint` checks); clean checkout after |
| SDL simulator build | rc 0, 0 warnings |
| 21 shell and UI test scripts | 521 ok, 0 FAIL, every script rc 0 (master: the same 21 and 521) |
| `clock_app_test` / `clock_lint.sh` | 778 checks / 38 checks, 0 failures |
| Clock engine, time, store and runtime tests | 210 / 39 / 76 / 85 checks, 0 failures |
| `settings_app_test`, `settings_lint.sh`, `settings_shell_test.sh` | 636 checks, 0 failures; lint 0 failures; 9 ok |
| `system_app_test`, `system_lint.sh`, `system_shell_test.sh`, `system_brand_shell_test.sh` | 833 checks, 0 failures; lint 0 failures; 21 ok; 39 ok |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 4 warnings, all `vendor/ggwave`; 0 first-party |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; `Doors 0.0.10 build 8aec2bc`; stripped 948,912 B, md5 `a893462f0ea58849c2dfc8f99986b1b8` |

Simulator smoke set on the rebased tree, 30 px corners, against the accepted
pre-rebase captures: Clock portrait and landscape (each tab), the new-alarm
form with the keyboard down, up, typed and a label refused, the delete
confirmation, a running and a paused countdown, the stopwatch with laps, and
an alarm before, ringing and stopped - 14 runs free of errors and warnings, 30
captures. **Object geometry identical in 30 of 30 states**; pixels identical in
27 of 30, the other 3 only in live digits (a paused countdown, stopwatch and
lap times).

## Unit A: deployment

Over the serial console (COM9), userspace only: nothing flashed, no SSH, only
`S90doors-shell` restarted. Tools and log: `out/clock-landscape-8aec2bc/hwgate-unitA`.

| Check | Result |
| --- | --- |
| Before | build `4972860` (md5 `cf56ca1a…`), one `doors-shell` (pid 862, restarts 0), Automatic, portrait, keyboard absent, theme Slate, Wi-Fi off; sysd, netd, radiod running, restarts 0; 0 crash reports; `shell.log` 0 ERROR and 0 WARN; `netd.log` 3 WARN from the Settings gate's failed Wi-Fi join, none new; the wall clock set |
| Clock's store before | **no `clock.conf`** and no `/var/lib/pocketos/clock/` at all: no alarms stored and no timer duration |
| Transfer | the gzip+base64 of the stripped build, 461,826 B in 95 s; on the unit gz md5 `67a2a42b…` and binary md5 `a893462f…`, both the host's |
| Install | rollback copy `/root/doors-shell.4972860` (md5 `cf56ca1a…`); installed md5 `a893462f…`; the shell answers build `8aec2bc`; exactly one `doors-shell` (pid 1748); supervised, running, crashloop 0, restarts 0 |
| After | Clock opened remotely (nothing tapped; opening writes nothing - still no `clock.conf`) and captured in portrait on the panel; Settings opened and captured, `shell.rotation` answering Automatic; back on the launcher; 0 new ERROR or WARN in `shell.log`, `netd.log`, `sysd.log`, `radiod.log`; 0 crash reports; doors-shell VmRSS 12,672 kB |

## Unit A: the physical check

The product owner, one batch, 2026-09-17 17:12-17:15 UTC: **PASS**.

1. **Portrait**: Clock looks as before; a temporary alarm with a short label
   added, shown correctly, and deleted through its confirmation.
2. **Landscape** (Settings > Display > Rotation > LANDSCAPE, Clock reopened):
   the layout intentional and nothing clipped by the corners; the face large
   and readable; the alarm list dragged; Watch Start, Lap, Pause, Reset;
   Timer Start, Cancel; Add alarm, the label field tapped, with the field,
   Cancel and Add all visible above the keyboard; a short label typed, the
   alarm added and deleted through the centred confirmation.
3. **Optional observation**: the display turned once more (to PORTRAIT). What
   happened to a countdown was not reported.
4. **Return**: Settings > Display > Rotation > AUTOMATIC; Clock reopened;
   portrait correct.

The logs were read before this was recorded, and agree:

- `open app clock` 17:12:16-17:12:50 in portrait; `rotation mode landscape
  stored: rotation 270` and one restart in place (same pid, as `execv` keeps
  it); `open app clock` 17:12:58-17:14:20 in landscape (1232x568, touch
  calibration for rotation 270); `rotation mode portrait stored: rotation 0`
  and a restart in place; Clock 17:14:31-17:15:00; `rotation mode automatic
  stored: rotation 0` with no restart (still portrait); Clock 17:15:10-17:15:13.
- **The portrait alarm is corroborated by the store**: the shell logs
  `clock: N alarm(s) loaded` only when `clock.conf` exists. At the deploy it
  did not; at the restart into landscape it logged `0 alarm(s) loaded`, so the
  portrait session wrote the store, and left no alarm in it.
- 0 ERROR and 0 WARN in `shell.log` (59 new lines), and nothing new in
  `netd.log`, `sysd.log` or `radiod.log`; 0 crash reports, no crashloop, no
  segfault, oops or panic in `dmesg`; one `doors-shell`, restarts 0; sysd,
  netd and radiod running, restarts 0.
- **Outside the batch**: at 17:15:17-18 Settings changed the theme from Slate
  to Olive & Chalk, then to Carbon & Signal Orange. Not a Clock step; recorded
  and left as the owner set it.

**Clock's store after the gate**: `clock.conf` now exists (23 B, md5
`857e643e…`) holding `pocketclock 1` and `timer 71` and **no alarm line**. The
alarms are what they were before - none - so both temporary alarms were
removed. The file itself and the 71-second timer duration are new: Start saves
the duration as a setting (POCKETCLOCK.md, Storage), and there was no store
before. A copy is `/root/clock.conf.post-clock-gate`.

**Unit A left**: build `8aec2bc` (rollback `/root/doors-shell.4972860`),
Automatic, portrait, keyboard absent, theme Carbon & Signal Orange (changed by
the owner), display mode Normal, brightness 100, Wi-Fi off, Settings open.

## Found on the way, left alone

Outside this scope:

- **An orientation change ends a running stopwatch, countdown and snooze.** A
  rotation restarts the shell in place (DS §21.2, `restart_in_place()` is an
  `execv`), and the clock runtime starts again from the store, which by design
  holds neither. With Automatic that includes attaching or removing the
  keyboard base. From the code; on unit A the restart and the store reload are
  in the log, but the countdown itself was not reported on. A separate bugfix.
- A stepper's name sits at the top of its 64 px row, not centred; a single-line
  field's text sits at the top of the field. Both as on master.
- The label keyboard's Done does nothing (KNOWN_ISSUES). In landscape, Add is
  beside the field above the keyboard.
- The shell's keyboard sheet (portrait) and alarm alert (landscape) reach the
  corner squares, as on master.

## Hardware questions, as answered

- The label field and the Cancel | Add rail across the top of the landscape
  form, above the keyboard and without it: accepted by the owner (all three
  visible above the keyboard).
- The Alarm pane's halves under a thumb: the list dragged in landscape (step
  11, PASS). No alarm was stored before the gate or left after it, so no
  owner's alarm could have been switched by a drag.
- The face across both halves: large and readable on the panel (step 10,
  PASS). The stopwatch and countdown were run in landscape without a
  remark; their legibility was not asked separately.
- Capture against the simulator: Clock and Settings were captured on the panel
  in portrait after the deployment; no pixel comparison was made (the unit's
  theme and time differ from the simulator's), and the owner's check is the
  acceptance.
- On the unit a rotation comes back on the launcher, so Clock is never open
  while the display turns; the relayout under an open app remains host
  evidence, and on the panel the keyboard coming up in the form is the size
  change that was exercised.
