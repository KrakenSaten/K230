# Touch after a shell restart: unit A gate

**Unit A carries the fix branch's shell, build `ed2026b`** (`fix/touch-restart-seed`,
sha256 `c6e40113…dde2e`), hand-installed over the flashed dev image `ee39407`
(0.0.13). Rotation is Automatic (landscape, keyboard base attached) and the
unit is locked. `/etc/default/doors-shell` is the original file. To go back to
the image's shell, run `/root/rollback-touch-seed/RESTORE.sh`.

**Status, 2026-09-26: injected gate PASS, finger check outstanding.**

- **Baseline** on the image's shell `ee39407`: the defect reproduced three
  times on hardware.
- **Fixed build** `ed2026b`: every restart case landed on the tile it aimed
  at. That covers the same point, the same raw X, the same raw Y, a control,
  and a rotation exec.
- **Unit A froze twice during the session:**
  - once at about 05:56 UTC, right after a shell restart;
  - once a few minutes after the owner's power cycle, idle on the lock screen,
    with no input injected and only log reads over SSH.

  The cause is UNKNOWN (see "The freezes"). After the second power cycle the
  unit ran for 15 minutes without trouble, including the whole fixed-build
  run.

## The defect

Found in the Zabbix gate on 2026-09-25. After the shell restarts, whether
through `S90doors-shell restart` or the exec that applies a rotation change, a
first touch that repeats the last raw X or Y loses that axis. LVGL puts the
press at raw 0 on that axis.

| Link | Evidence | Class |
|---|---|---|
| The input core sends no ABS event whose value equals what the device already holds. For `ABS_MT_*` the comparison is per slot. | Linux `input_handle_abs_event`; fuzz is 0 on this node (below), so only exact repeats are dropped | DOCUMENTED |
| LVGL's evdev driver starts `root_x/root_y` at 0 and reads only min/max with EVIOCGABS | `vendor/lvgl/src/drivers/evdev/lv_evdev.c` (59dc7e4), `lv_evdev_create_fd` | VERIFIED (source) |
| The GT9895 driver reports fingers only as protocol-B slots (`ABS_MT_POSITION_X/Y`), never `ABS_X/ABS_Y`. A finger's contact index is its slot, and every frame walks all 10 slots | `0034-gt9895-touch.patch`, `goodix_ts_report_finger` | DOCUMENTED |
| On unit A, slot 0 holds the last real finger's lift point, and slots 1–8 were never used | `touch_slots.py` below: slot 0 `52,1153`, slots 1–8 `0,0` | VERIFIED |
| Injected taps from `rift_tap.py` do not land in slot 0. They go to slot 9 (the slot the driver's loop leaves current) and also set `ABS_X/ABS_Y` | the same read: slot 9 = `ABS_X/ABS_Y` = `408,260` | VERIFIED |

Read-only probe of `/dev/input/event1` (`goodix_ts`) on 2026-09-26:

```
ABS_X              value 408  min 0 max 1060 fuzz 0
ABS_Y              value 260  min 0 max 2400 fuzz 0
ABS_MT_SLOT        value 0    min 0 max 9
slots X            [52, 0, 0, 0, 0, 0, 0, 0, 0, 408]
slots Y            [1153, 0, 0, 0, 0, 0, 0, 0, 0, 260]
slots id           [-1 x 10]
```

## How likely a real finger hits it

This is reasoning, not a measurement (ASSUMED unless stated otherwise). Raw
resolution is 1061 x 2401 over a 65 x 145 mm active area (DOCUMENTED), so
about 16 raw units per mm on both axes and 1.9 raw units per logical pixel.
The defect needs the first touch after a restart to report *exactly* the
last touch's lift coordinate on an axis.

- A deliberate tap on the same spot again: a finger's scatter of 1–2 mm is
  roughly 16–33 raw units. The chance that two taps match exactly on one axis
  is about 0.28/σ, so 1–2 % per axis and 2–3.5 % for either axis.
- Unrelated positions, for example after tapping Rotation in Settings or after
  auto-rotation: about 1/1061 + 1/2401, so around 0.15 %.
- GT9895 firmware filtering could make exact repeats more likely than this
  continuous model predicts. That is UNVERIFIED.
- Injected gate taps repeat exactly, so they hit it every time.

When it does happen, the press lands on a panel edge (raw 0 is the logical
left or top edge in portrait, and the top or right edge at rotation 270). The
tap is lost, or lands on whatever sits at that edge. In this gate, one such
press landed on the status bar's top-right corner. It is rare, but hard to
diagnose when it happens, and the fix below is small and outside `vendor/`.

## The fix

`ui/shell/touch_seed.c`: `platform_drm.c` opens the touch node itself. It
reads the node's current position with EVIOCGMTSLOTS for slot 0, or
`ABS_X/ABS_Y` on a node without slots. That position goes down a pipe into
LVGL's own parser: the driver is created on the pipe and reads the seed
through its normal read. Then `dup3()` puts the device under the same file
descriptor number. Events the device queued after the open still land on top
of the seed. LVGL's private driver state is never touched. The shell logs
`<node>: touch starts at raw X,Y (the device's slot 0 position)`.

Slot 0 is chosen, not the kernel's current slot, because on this driver the
current slot is 9 after every frame (above). A single finger lands in slot 0
again even when a second finger was the last to lift. Two paths are not
covered:

- A touch node that appears after the shell started is created unseeded by
  LVGL's discovery. Such a node normally still holds 0,0.
- Injected taps in `rift_tap.py`'s grammar (slot 9 plus `ABS_X`) can still hit
  the defect. Use `tests/hw/touch_slot0_tap.py` for gates.

## Host evidence (fixed build `ed2026b`, clean clone, WSL)

- `display_touch_test` (82 checks, 0 failures) models the input core's
  same-value rule. It creates a restarted pointer both plain and seeded, at
  rotations 0 and 270. The taps are on the same point, the same raw X, the same
  raw Y, a different point, and a point the device moved to after the seed was
  read. Plain loses all six repeats; seeded lands every tap within a pixel.
- Mutation checks: skipping the driver's read of the seed fails 6 checks.
  Leaving the device blocking hangs the test, and its alarm fails it (rc 142).
  Never seeding fails every "seed was applied" check and the seeded repeat
  taps (rc 1).
- `make test`: 5159 ok, 0 FAIL. All 21 `tests/*_shell_test.sh` suites pass,
  0 failures each (including `display_geometry_shell_test`).
- The riscv64 DRM/sysroot shell builds with 0 first-party warnings. It links
  `touch_seed_*` against the image's LVGL (`lv_evdev_create_fd`, `pipe2`,
  `dup3`). The gate payload, a Release build stamped `ed2026b`, has sha256
  `c6e40113…dde2e`.

## Unit A run, 2026-09-26

Tools:

- `tests/hw/touch_slot0_tap.py` injects a tap into slot 0 with `ABS_MT_*`
  only, so the input core filters it exactly as it filters a real finger.
- `tests/hw/touch_slots.py` reads the node's current values without changing
  anything.

Each case works the same way: a finger lifts at one point, the shell restarts,
then one tap follows. Restarts are either `S90doors-shell restart` or a
rotation change, which applies itself by exec.

### Baseline: the image's shell `ee39407`, with the shell's touch trace on

For this part `export POCKETOS_INPUT_TRACE=1` was temporarily added to
`/etc/default/doors-shell`, so `shell.log` records every press. Presses, from
`shell.log`:

| Time (UTC) | After | Finger (logical) | LVGL press | |
|---|---|---|---|---|
| 05:55:32 | (no restart) | 218,133 | 218,133 | control: lands |
| 05:55:41 | rotation exec, landscape to portrait, same raw point `249,1974` | 133,1012 | **0,0** | defect |
| 05:56:08 | rotation exec, portrait back to landscape; the finger repeats raw `249,1974` again | 218,133 | **1231,0** | defect |

That makes three reproductions on hardware, counting the Zabbix gate's
original finding. The press at 1231,0 was on the right end of the status bar.
My script attributed it to its next case, "same point after an S90 restart".
That was wrong: `shell.log` shows the press came before the restart, which
followed at 05:56:10.

### The freezes

- **First freeze.** At 05:56:10.999 the restarted shell logged
  `listening on shell.sock`, locked, and logged nothing more. The unit then
  stopped answering: SSH timed out during banner exchange, then there was no
  route to `.157` or `.171`, and COM9/COM10 gave no response. The owner found
  it frozen and power-cycled it.
- **Second freeze.** The next boot also froze, a few minutes in. Its shell log
  ends at `lock: engaged (start)` / `listening`. No taps had been injected;
  the only activity was SSH log reads. The owner power-cycled it again.
- **What followed.** The trace line was taken out of `/etc/default/doors-shell`
  as soon as SSH answered. The unit then ran idle for 11 minutes, sampled every
  30 s: load about 0, 918 MB available, 49–51 °C, no new kernel messages. It
  then went through the whole fixed-build run below (about ten restarts and
  rotation execs), still with nothing new in `dmesg`.

The cause is UNKNOWN. The kernel log does not survive a power cycle, so
nothing records either freeze.

- **Taps are ruled out** for the second freeze: nothing was injected before it.
- **The fixed build is ruled out**: it was never installed before either freeze.
- **The trace line was active during both freezes.** It only logs on a touch
  change, so it wrote nothing while idle; it is not a likely cause.

If the unit freezes again on `ed2026b`, the next step is a serial console
capture, which would show whether the kernel is alive.

### Fixed build `ed2026b`, no trace

This run uses no configuration change on the unit. The launcher is the
instrument: tile centres come from `doors shell info` (`launcher.cells`), and
the app that opens is the answer. Tiles in landscape: Calendar at 564,280,
Notes at 460,280 and System at 564,480. Landscape swaps the axes, so tiles in
the same row share raw X, and tiles in the same column share raw Y.

| Case | Slot 0 before the restart | Shell logged | Tap | Result |
|---|---|---|---|---|
| Same point (lift on Calendar, S90 restart, Calendar) | 524,1299 | `touch starts at raw 524,1299` | Calendar, no MT event reaches LVGL | **PASS**, Calendar opened |
| Same raw X (Calendar, then Notes) | 524,1299 | `touch starts at raw 524,1299` | Notes (raw X 524 repeats) | **PASS**, Notes opened |
| Same raw Y (Calendar, then System) | 524,1299 | `touch starts at raw 524,1299` | System (raw Y 1299 repeats) | **PASS**, System opened |
| Control (Notes, then System) | 524,1502 | `touch starts at raw 524,1502` | System | **PASS**, System opened |
| Rotation exec to portrait: lift in landscape at the raw point of portrait's Calendar centre (222,504) | 416,983 | `touch starts at raw 416,983` | raw 416,983 in portrait | **PASS**, Calendar opened |

- The seed equalled the slot 0 read in every case, as the log lines show. My
  script reported the seed comparison as FAIL four times; that was its own
  grep expecting a trailing comma (fixed after the run). No case was rerun.
- The rotation mode went back to Automatic after the run (landscape), and the
  unit was locked again.
- Right after installation, before any touch, the shell logged
  `touch starts at raw 0,0 (the device's slot 0 position)`. That is correct
  for a node nothing has touched since boot.

## Still to do

1. **Real finger, owner at the bench.** On unit A as it is now (`ed2026b`):
   1. Unlock, tap the Calculator tile, then return home.
   2. Restart the shell over SSH (`/etc/init.d/S90doors-shell restart`) without
      touching the panel. Unlock with `doors call shell shell.unlock`, or on
      the panel.
   3. Tap Calculator again, as close to the same spot as you can.

   Calculator must open every time. Repeat a few times, and once with Settings
   > Display > Rotation in place of the S90 restart. The chance of an exact
   repeat is small (see above), so this checks that nothing regressed rather
   than proving the fix; the injected cases above prove the fix.
2. **Watch for another freeze.** If one happens, capture the serial console
   before power-cycling.
3. **After the decision:** merge, or `/root/rollback-touch-seed/RESTORE.sh` to
   go back to the image's shell.
