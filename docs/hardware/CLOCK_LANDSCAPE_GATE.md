# Clock in landscape: unit A gate

Branch `feature/clock-landscape`, written from origin/master `aaad9f4` (the
shared corner-clearance helper) and rebased onto origin/master `dcf906d`
(Settings §24 and System §25 accepted). Code `3f4b584`, docs `912849c`; before
the rebase `0d04b45` and `8d5dcc1`. VERSION stays 0.0.10.

**Result: host and simulator validation PASS, 2026-09-17, before the rebase.
Unit A NOT RUN.** That work was done host and simulator only, by the product
owner's instruction: unit A was not accessed in any way (no serial console, no
SSH, no transfer, no restart of `doors-shell`, no rotation change, no
flashing). The remote gate and the owner's physical check below are prepared,
not run. DS §26 (Amendment J) is PROPOSED.

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

## Found on the way, left alone

Outside this scope:

- **An orientation change ends a running stopwatch, countdown and snooze.** A
  rotation restarts the shell in place (DS §21.2, `restart_in_place()` is an
  `execv`), and the clock runtime starts again from the store, which by design
  holds neither. With Automatic that includes attaching or removing the
  keyboard base. From the code; not observed on unit A (a remote gate step
  below can observe it).
- A stepper's name sits at the top of its 64 px row, not centred; a single-line
  field's text sits at the top of the field. Both as on master.
- The label keyboard's Done does nothing (KNOWN_ISSUES). In landscape, Add is
  beside the field above the keyboard.
- The shell's keyboard sheet (portrait) and alarm alert (landscape) reach the
  corner squares, as on master.

## Unresolved hardware questions

Only the panel and a finger can answer these; nothing here is claimed for
unit A.

- Whether the label field and the Cancel | Add rail across the top of the
  landscape form read naturally, above the keyboard and without it, given
  that portrait puts the time first.
- Whether two halves that scroll on their own feel natural under a thumb in
  the Alarm pane, drags starting on a row and between the notice and the list
  included, and whether a row's switch is never toggled by a drag.
- Whether the big numbers in the left half, and the clock face across both,
  read well at arm's length in Normal and Outdoor.
- Capture against the simulator on the real panel (RGB565, within 13 per
  channel, as for Calculator, Notes and Settings). The face, the status bar
  and every running value follow the unit's clock, so the simulator must be
  given the unit's time and store at the moment of capture, or those areas
  masked.
- On the unit a rotation comes back on the launcher, so Clock is never open
  while the display turns; the relayout under an open app is host evidence,
  and on the unit only the keyboard coming up and going down changes Clock's
  body while it is open.

## The gate to run later

Only in a session the owner allows to use unit A. Build the stripped riscv64
DRM `doors-shell` from the branch tip as for the Settings and System gates
(a stripped build of `8d5dcc1` was 944,816 B, md5 `4f721e6c…`; the gate
rebuilds it from the tip it runs).
**Clock's store on unit A may hold the owner's alarms**: back it up first,
add nothing that is left behind, and prove it unchanged at the end. An alarm
the gate adds is deleted in the gate.

**Remote** (serial console or SSH, as the owner allows):

1. Identity before (release file, `doors-shell` md5 and build, rotation mode,
   theme, restarts, crash reports, `shell.log` errors); md5 and a copy of
   `/var/lib/pocketos/clock/clock.conf`. Rollback copy of the installed
   `doors-shell`; install the build; restart only `S90doors-shell`; the shell
   answers the new build, supervised, restarts 0.
2. Portrait: open Clock; capture each tab and the new-alarm form; compare with
   the simulator given the unit's time and store.
3. Landscape (`doors call shell shell.rotation mode=landscape`, restart in
   place, back on the launcher): open Clock; capture each tab (the face across
   both halves; Alarm, Watch, Timer in halves); drag the alarm list (Add alarm
   does not move); Watch Start, Lap, Lap, Pause, Reset; Timer +1 minute,
   Start, capture, Cancel; Add alarm: capture; tap the label field (keyboard
   up: field and the Cancel | Add rail in view), capture; type a label and
   tap Add; tap that alarm's delete: the confirmation centred, capture; Delete.
4. Optional, if the owner wants the rotation finding observed: start a
   10-minute countdown, turn back to portrait (restart in place), reopen Clock:
   record whether the countdown is gone.
5. Back to the mode found (Automatic); health after (`shell.log` 0 ERROR and
   WARN, no crash report, services running, restarts 0); `clock.conf` md5 as
   before; the foot corner squares of every Clock capture hold only background.

**Physical** (the owner, one batch):

1. **Portrait** (as the unit is): open Clock. It looks as it did; each tab;
   Add alarm, tap the label, type, Add; delete it (Cancel first, then Delete).
2. **Landscape** (Settings > Rotation > LANDSCAPE; open Clock): it looks
   designed for landscape - nothing clipped, overlapping or cut by the rounded
   corners; the face large across the screen; drag the alarm list with a
   thumb; Watch Start, Lap, Pause, Reset; Timer set, Start, Cancel; Add alarm:
   tap the label field - with the keyboard up the field, Cancel and Add are
   all visible - type a label, Add; delete that alarm (the confirmation in the
   middle).
3. **Back**: Settings > Rotation > AUTOMATIC. Clock still looks right in
   portrait.

**Result: NOT RUN** - host and simulator only by instruction; the owner's
physical check and the remote gate are pending.
