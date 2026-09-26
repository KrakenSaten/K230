# Touch after a shell restart: unit A gate

**Status: INCOMPLETE, 2026-09-26.** The defect is reproduced on unit A with a
finger injected exactly as the GT9895 reports one. The fixed build
(`fix/touch-restart-seed` `ed2026b`) was built but **never installed**: unit A
dropped off the network and went quiet on its serial console partway through
the baseline, before the fixed build could go on. No finger check has been
done yet.

**Unit A as last seen (05:5x UTC, 2026-09-26):** the flashed dev image
`ee39407` (0.0.13), shell binary unchanged (sha256 `e723ffab…76a3`), rotation
Automatic (landscape, keyboard base attached), unlocked, on the launcher. **One
change is still on the unit:** `/etc/default/doors-shell` has
`export POCKETOS_INPUT_TRACE=1` appended (the shell logs a `touch trace` line
for every press and release). The original file is in
`/root/rollback-touch-seed/doors-shell.default`. To undo:
`cp /root/rollback-touch-seed/doors-shell.default /etc/default/doors-shell && /etc/init.d/S90doors-shell restart`.

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

Tools: `tests/hw/touch_slot0_tap.py` injects a tap into slot 0 with
`ABS_MT_*` only, so the input core filters it exactly as it filters a real
finger. `tests/hw/touch_slots.py` reads the node's current values without
changing anything. Each case: a finger lifts at the first point, the shell
restarts, then one tap at the second point. The shell's own `touch trace` line
shows where LVGL put that press.

Baseline, the image's shell `ee39407` (defect expected):

| Case | Finger | LVGL press | Result |
|---|---|---|---|
| Rotation exec (landscape to portrait), same raw point `249,1974` | 133,1012 | **0,0** | defect reproduced |
| S90 restart, landscape, same point | 218,133 | **1231,0** | defect reproduced |
| S90 restart: same raw X, same raw Y, different point | — | — | not run: the unit went offline |

The unit stopped answering right after the second row: the next SSH call
timed out during banner exchange, then there was no route to `.157` or `.171`
for at least 3 minutes, and COM9/COM10 gave no response to a carriage return.
The last input the unit received was the injected press that landed at
1231,0. That corner is the status bar's right end. One press there should not
power the unit off (Power off needs three taps, including the confirmation).
**Cause UNKNOWN.** It could be power, a link loss, or something the press
started. The owner needs to look at the unit.

## Still to do

1. Find out why unit A went offline: its screen, power, and the serial
   console. Remove the trace line (above) if it is not wanted for step 2.
2. Install `ed2026b` with a rollback (`/root/rollback-touch-seed` already
   exists and holds the image's `/etc/default/doors-shell`; copy
   `/usr/bin/doors-shell` there as well before replacing it). Run every case
   above with the trace on. Expected: every press within a pixel of the
   finger, and a `touch starts at raw …` line equal to `touch_slots.py`'s slot
   0 read before the restart.
3. Real finger, owner at the bench. Tap the Calculator tile and lift. Run
   `/etc/init.d/S90doors-shell restart` over SSH without touching the panel.
   Then tap the same tile again, as close to the same spot as you can, several
   times across restarts. Each time, check that the tile opens and that
   `touch trace ... down` is at the tile. With the trace on, any
   first-press-after-restart that lands at x=0, y=0 or a panel edge shows up
   in `shell.log`, on either build.
4. Put the trace line back to the original file and state the unit's build at
   the top of this sheet.
