# Doors display geometry: hardware gate

Branch `feature/doors-display-geometry`, from master `13029a3`.

**Result: PASS on unit A, 2026-09-16** (four power-off boots with the product
owner, plus everything below). **DS Amendment E (§21) ACCEPTED.**

What the four boots settle, and what they deliberately do not: the orientation
Doors opens with is **VERIFIED** for every case of the policy, and the
keyboard base being **attached or removed while the board is powered stays
UNVERIFIED as a hardware operation**. The software side of a live change is
verified (the simulator's 29 checks, and on unit A the controller held in
reset), but no base was ever mated or unmated live, by design: see "Hot-plug"
below.

Not merged. No change to VERSION, typography, icons, icon sizes, theme
palette, tile styling or the focus model; Rebrand Phase 3 not started.

## What this milestone changes

**Rounded corners (the defect).** On unit A the left of the `DOORS` wordmark
and the last clock digit sat inside the panel's rounded top corners: the
status bar placed them 20 px from the screen's rectangle. There is now a
safe area:

```
physical panel (568x1232 native portrait, edge strips, corner squares)
        |  effective rotation (one value, decided by the shell at start)
        v
logical width x height, logical edge and corner insets   (ui/pocketui/pos_display.c)
        |
        v
PocketUI (pocketui_apply_bar_insets), status bar, launcher, apps
```

A bar along a screen edge takes the corner inset at each end where its own
padding does not already clear it. The status bar's side padding becomes
30 px; the launcher, app headers and bodies start below the corner band and
do not move. The corner square is **PROVISIONAL at 30 px**: no datasheet or
source gives the corner radius or mask; 30 px is the side inset the vendor
launcher gives its own status bar on this panel (`STATUS_BAR_SAFE_SIDE`).
`POCKETOS_SAFE_CORNERS=tl,tr,br,bl` in `/etc/default/pocketos-shell` tries
another value without a rebuild.

**Rotation.** Settings > Display > Rotation: Automatic, Portrait, Landscape,
stored as `display_rotation`. Portrait and Landscape are forced; Automatic is
Landscape only with a keyboard known to be present. Landscape is DRM rotation 270, the
rotation the vendor launcher runs at on this board with its keyboard
(KEYBOARD_BRINGUP_2026-09-10.md §6): **the device turned a quarter turn
clockwise, the portrait left edge at the top**.

One geometry per shell run drives the DRM plane rotation and the evdev
touch transform, set together in `pocketos_platform_init()`; the transform
per rotation equals the vendor launcher's for the same DRM index. The old
`POCKETOS_DRM_ROTATION` path that turned the picture without touch is gone:
that variable now overrides the policy for display and touch together.

## Automatic: what decides that a keyboard is there

The keyboard base answers on the bit-banged bus or it does not, and that is
the signal (`ui/shell/shell_kbd.c`). The TCA8418 acknowledging at 0x34 is what
the vendor launcher calls the base being detected, and it is verified in both
directions on this unit: with the base unmated the shell logs "the controller
did not answer; touch only", and remating gives "TCA8418 ready"
(KEYBOARD_BRINGUP §0, §5.3 C3). Nothing on this board reports the base
mechanically, and nothing here pretends otherwise.

- The probe runs **before the display is opened**, so a boot with the base
  attached opens landscape directly instead of correcting itself afterwards.
- A watch re-probes once a second while nothing is attached, and reads the
  driver's own state while one is, so attaching or removing a keyboard is
  noticed within seconds without any bus traffic in the common case.
- Three consecutive agreeing readings - two whole seconds - are needed before
  the state changes (`KBD_PRESENCE_STABLE`), so contacts that bounce cost
  nothing. A transport that cannot be claimed at all is **unknown**, not
  absent, and unknown resolves to portrait.
- Forced Portrait and forced Landscape ignore all of it.

## Hot-plug: not done, and why

Every attach and detach on record, this gate included, was done with the SoC
halted and USB power removed (KEYBOARD_BRINGUP §5.3 C3, §8). Detection does
not care either way - it sees the controller answer or not, and the simulator
proves what the shell does when that changes while it runs - but whether
mating a powered board is safe **for the hardware** is a separate question,
and the documents do not answer it. What the vendor sources do and do not say
is recorded in KEYBOARD_BRINGUP §8; the short of it:

- The interconnect is **JP1, a plain 2x20 header** (`HEADER_20X2_H` in the
  main-board schematic). No part number, pitch, gender or mating specification
  exists in any vendor document, so **whether ground mates first is unknown**.
- **JP1 pin 3 is the board's 3V3 rail** - the same net that feeds the K230's
  VDDIO banks. A base board's bulk capacitance therefore charges through that
  rail at the moment of contact, and nothing in the extracted netlist limits
  it: **no load switch, no series resistance, no TVS or ESD part on any JP1
  net** (protection exists only on USB, microSD and the antenna).
- There is **no board-detect or ID pin**; the vendor detects the base in
  software by probing I2C, exactly as this milestone does.
- **No vendor statement about hot-plug exists at all**, permissive or
  prohibitive, and there is **no schematic of any base board**.

A controller that recovers after a dropout says nothing about this: I2C
recovering is not evidence that the supply and the SoC's I/O rail survived the
event. So live attach/detach stays **software-verified, hardware-UNVERIFIED**,
and the gate above avoids it.

## Applying an orientation: the shell opens the display again

The display cannot be turned while it is open. Evidence:

- The vendor LVGL patch 0002 swaps the DRM framebuffer's width and height when
  the device is opened (`lv_linux_drm_set_file`); `lv_linux_drm_set_rotation`
  afterwards only changes the plane property, not the buffers or LVGL's
  resolution. The kernel plane (canaan_plane.c) can rotate per commit, but the
  buffers LVGL draws into cannot change shape without re-creating the display.
- LVGL's own software rotation needs matrix transforms in this driver's direct
  render mode, and the device's LVGL is built with
  `LV_DRAW_TRANSFORM_USE_MATRIX 0`.
- The vendor launcher rotates 0 <-> 180 live but restarts its own process
  (`execl`) to go between portrait and landscape.

So the shell opens the display again, itself. It leaves its main loop the
ordinary way - the open app is closed and persists what it holds, the
keyboard's pin mux goes back, the IPC socket is unlinked - closes every
descriptor it opened, above all the DRM device the next image must open as
master, and re-executes its own binary with its own arguments. The pid does
not change, so `pos-supervise` sees no exit and counts no restart, and the
device is never rebooted. If the exec fails the shell exits instead and the
supervisor starts it again: one counted restart, same orientation, still no
reboot. A change settles for 800 ms first, on top of the presence debounce, so
a mode tapped twice or a base finding its contacts costs nothing.

Cost, and Settings says it before it happens: the panel is dark for the length
of a shell start, and Doors comes back on the launcher. Measured on unit A:
four in-place restarts during the remote pass, pid unchanged (16105
throughout), **0 supervisor restarts**. The alternative, re-creating the LVGL
display inside the running process, needs the whole shell UI rebuilt and gains
nothing visible.

## Validation done without hardware (2026-09-15, extended 2026-09-16)

Source `e3f900d` for the safe area and the rotation policy, `1bd7cbf` for the
keyboard provider and applying an orientation in place; from fresh WSL clones,
with master `13029a3` built and run the same way as the baseline. VERSION
0.0.9 on both.

| Check | Result |
| --- | --- |
| `make all`, host, `-Werror` | rc 0, 0 warnings |
| `make test`, host | rc 0 at the tip `1c85b16`: 3,633 ok, 0 FAIL, 0 warnings (master 3,623); `display_geometry_test` 76 checks, `orientation_test` 29, `kbd_presence_test` 28, `settings_view_test` 120, `initscript_test` (the corner override is exported); the six `NOT RUN` lines are check names, identical to master's |
| Shell tests, SDL simulator | all 20 scripts rc 0 at the tip `1c85b16`, 472 ok, 0 FAIL, including `auto_rotation_shell_test.sh` 29 ok and `display_geometry_shell_test.sh` 70 ok (71 with `SHOTS_DIR`, which adds the contact sheet) with `display_touch_test` 58 checks, `settings_shell_test.sh` with `settings_app_test` 81, `kbd_shell_test.sh` with `shell_kbd_test` 18, `launcher_icons_shell_test.sh`, `shell_ipc_test.sh`, `pos_input_test.sh`, `pos_keyboard_test.sh`, `kbd_shell_test.sh`. At `e3f900d` two static checks in `calculator_shell_test.sh` and `wave_shell_test.sh` still counted grid rows written out in `home_create()`; they now read the grid from the running shell (`5ac0569`) |
| Safe area, portrait | in all 15 theme/mode pairs every ink column of the status bar lies at least 30 px from each side; three ink groups (wordmark, radio chip, clock); the wordmark's left and the clock's right clear of the 30 px corners; every tile 254 x 150 in its place with its own icon, nothing below the last row |
| Safe area, landscape | the same on 1232x568 in all 15 pairs, the bar's ends 30 px from both ends of the long edge; tiles 182 x 150 in six columns and two rows |
| Rectangular panel | with `POCKETOS_SAFE_CORNERS=0,0,0,0`, in both orientations, the bar keeps its own 20 px; with the 30 px corners the wordmark is the same pixels moved 10 px in (drawn whole) and everything below the status bar is identical: the inset comes from the platform description, not from the bar |
| Geometry against master | the portrait launcher screenshotted from both builds in all 15 pairs and compared pixel by pixel: **0 pixels differ below the status bar**; 1,319–1,346 differ in it (wordmark and clock 10 px further in) |
| Model | every rotation maps the native panel onto the logical one pixel for pixel and back; size swap at 90/270; four different edge strips and four different corners each land on the right logical edge and corner; top, bottom, left and right bars at 0, 90 and 270; a bar's end takes the larger of strip and corner; `rect_is_safe` at every corner, including the status-bar label and clock positions in both orientations; a rectangular panel gives no insets |
| Touch | the per-rotation transform equals the vendor launcher's settings for the same DRM index; a touch on native corners, centre and off-axis points lands on the logical pixel the display draws there, at all four rotations with the controller mounted straight or swapped in the model, and through LVGL's own `lv_evdev` fed a GT9895-style multitouch stream at all four rotations (swapped at 0 and 270) (`display_touch_test`); the same checks catch a landscape display with portrait touch, 270 with 90's touch, 90 with 270's and 0 with 180's |
| Policy | Automatic with the keyboard unknown, absent and present (Portrait, Portrait, Landscape); forced Portrait and Landscape with each keyboard state; manual overrides automatic; persisted across restart; invalid stored value → Automatic with a WARN, the stored text left alone; `POCKETOS_DRM_ROTATION` overrides display and touch together and is logged; an invalid one ignored |
| IPC and Settings | `shell.rotation` stores Landscape, reports `applying`, and the shell opens the display again in the same process, coming back landscape with nothing pending; an invalid mode is error 2; Settings shows the selected mode and the note (what is being applied and what it costs, why Automatic chose what it did, stored value not recognised) |
| Automatic and the keyboard (`auto_rotation_shell_test.sh`, 29 checks) | boot with a keyboard (landscape at once, no restart), without one, and with a provider that cannot tell; a keyboard attached and removed while Doors runs, each applied in the same process (checked by pid); forced Portrait with one attached and forced Landscape with one removed, neither moving; a base bouncing on its contacts changing nothing; detection failing falling back to portrait and recovering; no second restart while the state holds; an app open across the change, closed the ordinary way; the mode changed over IPC while a keyboard is attached |
| Presence debounce (`kbd_presence_test.c`, 28 checks) | boot present, absent and unknown; attach and removal; bouncing contacts; a run broken by one good reading; a provider that fails publishing unknown rather than absent, and recovering; nothing announced twice |
| Theme change | a live theme and mode switch keeps the orientation and redraws the landscape launcher in the new theme |
| Every app | in landscape all eleven open through `app start` and come home, no ERROR; in portrait the per-app shell tests pass |
| Reduced motion | the landscape launcher is pixel-identical below the status bar |
| Single source | only `shell_display.c` reads `POCKETOS_DRM_ROTATION`; the DRM backend rotates the plane from the geometry it is handed and derives touch from the same geometry in one place; no app rotates the display, builds a geometry or publishes keyboard presence; only the simulator's test hook publishes presence |
| Focus model | unchanged: no file of the input stream, focus group or tile widget is changed; input, keyboard and keymap tests pass |
| riscv64 `make all` (`ENABLE_SX1262=1`) | rc 0, 4 warnings, all in vendor ggwave, as on master |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings, branch and master; the branch build found `lv_linux_drm_set_rotation` in the SDK's LVGL (`POCKETOS_DRM_ROTATION_API`) |

Deliberate breakages (a scratch copy; each mutation built, and each was
caught by the tests named):

| Mutation | Caught by |
| --- | --- |
| 270 native→logical uses the 90 mapping | `display_geometry_test` (11 FAIL) |
| logical size not swapped at 90/270 | `display_geometry_test` (17) |
| native top-right corner not rotated | `display_geometry_test` (8) |
| native left edge inset not rotated | `display_geometry_test` (4) |
| top bar ignores the top-left corner | `display_geometry_test` (3) |
| `rect_is_safe` ignores the top-right corner | `display_geometry_test` (6) |
| 270 touch Y reversed | `display_geometry_test` (6), `display_touch_test` (12) |
| 90 touch axes not swapped | `display_geometry_test` (7), `display_touch_test` |
| 180 touch X not mirrored | `display_geometry_test` (6), `display_touch_test` |
| Automatic with the keyboard unknown → Landscape | `orientation_test` (2), `display_geometry_shell_test` |
| forced Portrait follows the keyboard | `orientation_test` (2) |
| invalid stored mode falls back to Landscape | `orientation_test` (3) |
| Landscape direction flipped to 90 | `orientation_test` (4), `display_geometry_shell_test` |
| status bar does not adopt the safe area | `display_geometry_shell_test` (126) |
| landscape launcher keeps two columns | `display_geometry_shell_test` (368) |
| landscape launcher one column too many | `display_geometry_shell_test` (368) |
| next rotation stale (so nothing is ever applied) | `display_geometry_shell_test` (1) |
| DRM touch always at rotation 0 | `display_geometry_shell_test` (1, source check: the DRM backend cannot run on the host) |
| DRM touch no longer taken from the display's geometry | `display_geometry_shell_test` (1, source check) |
| an app rotates the display itself | `display_geometry_shell_test` (1) |
| every Rotation button in Settings stores Automatic | `settings_app_test` (3) |
| the forced note names the wrong orientation | `settings_view_test` (2) |
| Settings never says a change is being applied | `settings_view_test` (2), `settings_app_test` (1) |

23 of 23 caught.

Size and memory:

| | master | branch | change |
| --- | --- | --- | --- |
| riscv64 DRM shell, `size` text / data / bss | 914,115 / 9,948 / 22,400 | 926,110 / 10,060 / 22,720 | +11,995 / +112 / +320 |
| riscv64 DRM shell, stripped file (what ships) | 928,376 B | 940,720 B | **+12,344 B** |
| SDL x86-64 shell, text / data / bss | 2,208,585 / 15,320 / 23,680 | 2,224,836 / 15,424 / 24,000 | +16,251 / +104 / +320 |
| Heap allocated by `home_create()` (glibc `mallinfo2`, simulator, 2 runs each, identical) | 13,856 B | portrait 13,856 B, landscape 13,856 B | **0** |

The grid descriptors are static arrays sized for the largest grid (bss), so
the launcher allocates the same in both orientations. The keyboard provider
and the apply mechanism cost no heap at all on the launcher and fit inside the
same stripped page count as the safe area alone. The heap figure comes
from a measurement patch applied only in the validation clones, never
committed. The device's own figure (VmRSS) is taken at the gate.

Screenshots (simulator, not the panel):
`docs/design/brand/shots/display-geometry-contact.png` - top row the portrait
launcher, the landscape launcher and Settings > Display with Rotation; below,
the status bar's ends in portrait and then landscape at full size with the
30 px corner line in magenta. `shots/launcher-contact.png` is replaced by
this branch's fifteen portrait launchers (only the status bar differs).

## Why the panel still has to be seen

The simulator proves the model, the layout in both orientations and the touch
transform against the real `lv_evdev` code. It cannot show three things: where
the RM69A10's rounded corners actually cut (so whether 30 px clears them),
whether the K230 plane rotation in the vendor LVGL build turns the picture the
documented way, and whether GT9895 taps land under the finger once it has.

## The gate

**Operator time: about five minutes, four power cycles and one answer.**
Everything else is done by the session over SSH, or over the USB serial
console when there is no network, and every scripted check prints `ok` or
`FAIL`.

Before (session, no operator), all of it done for the run below:

1. Build from the branch tip: clean WSL clone, `apply_to_sdk.sh`,
   `build_image.sh` (runs `verify_image.sh`); record the shell binary's
   SHA-256 and that the DRM build found `lv_linux_drm_set_rotation`
   (`POCKETOS_DRM_ROTATION_API`).
2. Over SSH, before deploying: `pocketos-shell` VmRSS and VmHWM on the running
   shell. Record, and move aside for the test, any `POCKETOS_DRM_ROTATION`,
   `POCKETOS_TOUCH_CALIB`, `POCKETOS_TOUCH_SWAP` or `POCKETOS_SAFE_CORNERS` in
   `/etc/default/pocketos-shell` (each would override what is being tested).
3. `platforms/k230/scripts/deploy.sh 192.168.10.157` (network), or the same
   image already on the unit; check `doors version`
   shows the branch tip's build, `/usr/bin/pocketos-shell` matches the build's
   SHA-256, the four services run, the log says what the probe found and which
   way the display and touch were opened, `app list` shows eleven apps, every
   app opens and comes home, no ERROR, no crash report.
4. Both orientations and both ways of choosing them, without the operator:
   forced Portrait and forced Landscape over `shell.rotation`, and Automatic
   with the keyboard's answer changed underneath it - the bench holds the
   TCA8418's reset line low (`gpioset -c gpiochip1 11=0`), which is the same
   silence on the bus as an absent base, then releases it. Each change is
   checked to be applied in the same process (pid and start time), with the
   supervisor counting no restart, and VmRSS/VmHWM taken in each orientation.
5. Put the SDK target tree back as found (docs/hardware/DOORS_PHASE2_GATE.md,
   "After the test"), and leave the unit on Automatic, Ice/Normal, on the
   launcher.

Then one pass on the panel, **with every attach and detach done on a
powered-down board**: the connector is not mated live (see "Hot-plug" above).
Each boot is verified from the unit itself before the next one.

| # | Operator does | Pass |
| --- | --- | --- |
| 1 | Boots with the base **attached**, Rotation = Automatic | opens in **landscape**, six tiles in each of two rows; the whole `D` of `DOORS` and the clock's last digit clear of the rounded corners; a tap near the top-left tile and one near the bottom-right open the tile under the finger |
| 2 | Powers down, removes USB power, **detaches** the base, powers on | opens in **portrait**, two columns, wordmark and clock clear of the corners |
| 3 | Rotation = **Portrait**, powers down, **attaches** the base, powers on | stays **portrait**: a forced mode ignores the keyboard |
| 4 | Rotation = **Landscape**, powers down, **detaches** the base, powers on | stays **landscape** |

One answer: PASS if all four hold, otherwise FAIL with the step number. The
probe result, the resolved policy, the logical size, the DRM rotation, the
touch transform, the services and the crash/error state are read from the unit
for each boot; with no network the console over USB serial is enough for all of
it, including the panel itself (`ffmpeg -f kmsgrab`, carried over the serial
line and checked in pixels).

What this does **not** cover, and must not be inferred from it: a base
attached or removed **while the board is powered**. That is the same policy in
software, but a different question in hardware, and it stays open.

If the corners clip in 1, the fix is a number, not code: try another
`POCKETOS_SAFE_CORNERS` in `/etc/default/pocketos-shell` and restart, then
commit the value that clears. A landscape picture upside down means the plane
turns the other way from the vendor's index (270 would become 90,
`ORIENTATION_LANDSCAPE_ROTATION`); taps that land mirrored or on the wrong axis
mean the transform, which the touch line in the log then shows.

## Result on unit A: the four power-off boots (2026-09-16)

Build `1bd7cbf` deployed with `deploy.sh` (no flash). Ethernet was not
available, so this pass ran entirely over the **USB serial console** (CH342 A,
COM9, 115200): every check below was read from the unit itself, and each panel
image was captured on the device from the DRM plane (`ffmpeg -f kmsgrab`) and
carried over the serial line as base64, checked in pixels here, MD5 confirmed
end to end. The base was attached and detached only with the board powered
down and USB power removed.

| # | Boot | The shell's own account | Panel |
| --- | --- | --- | --- |
| 1 | base **attached**, Automatic | `keyboard: TCA8418 ready, polling every 15 ms (INT-gated)`; `rotation mode automatic (stored), keyboard present: rotation 270, 1232x568`; `DRM plane rotation 270 degrees`; touch `rotation 270: swap 1, calibration 2400,0,0,1060 onto 1232x568`; `launcher: 6 column(s), 2 row(s)`; **no restart** | landscape 1232x568; `DOORS` ink from x 31, clock ends x 1199, both inside the 30 px corners; 11 of 11 tiles 182x150 on the six-column grid |
| 2 | base **detached**, Automatic | `keyboard: the controller did not answer; touch only`; `rotation mode automatic (stored), keyboard absent: rotation 0, 568x1232`; touch `rotation 0: swap 0, calibration 0,0,1060,2400 onto 568x1232`; `launcher: 2 column(s), 6 row(s)` | portrait 568x1232; `DOORS` from x 31, clock ends x 536, inside the corners; 11 of 11 tiles 254x150 on the two-column grid |
| 3 | base **attached**, forced **Portrait** | `keyboard: TCA8418 ready`; `rotation mode portrait (stored), keyboard present: rotation 0, 568x1232`; touch at rotation 0; two columns. Held for 14 minutes with the keyboard attached: no drift, no in-place restart | portrait, identical bytes to boot 2's capture (the capture path proved live in the same boot: opening Notes changed the image) |
| 4 | base **detached**, forced **Landscape** | `keyboard: the controller did not answer; touch only`; `rotation mode landscape (stored), keyboard absent: rotation 270, 1232x568`; `DRM plane rotation 270 degrees`; touch at 270; six columns | landscape, identical bytes to boot 1's capture; owner's visual answer: PASS |

Throughout: services 4/4, **0 crash reports**, and the only ERROR in any log is
one this session caused by asking for `shell.screenshot`, which the device
build has compiled out (`built without LODEPNG/SNAPSHOT`) - the reason the
captures go through ffmpeg. Applying a mode over IPC was exercised twice more
on the panel (Portrait→Landscape, then back to Automatic): each applied itself
in place within about 4 s, in the same process.

**Also seen, unplanned and worth keeping.** In an earlier boot the controller
went silent for 53 ms and the debounce absorbed it with no rotation; a longer
dropout (about 3 s) published absent, turned the display portrait, and
re-contact turned it back - the safety logic working on a real contact glitch
rather than a simulated one.

Evidence kept outside the repository: the session's scratchpad holds the four
panel captures, the serial transcripts of each boot, and the remote pass below.
The unit was left as found - Automatic, Ice/Normal, base detached, portrait -
and the bench's gate scripts were removed.

## Result of the remote pass on unit A (2026-09-16)

Build `1bd7cbf`, deployed with `deploy.sh` (no flash); shell SHA-256
`dbc76a6a…`, image `6942b2db…`, IMAGE GATE PASS. **23 scripted checks, 0
FAIL**, plus the 26 + 6 of the earlier round on `e4abe90`.

| Check | Result |
| --- | --- |
| The probe, before the display was opened | `keyboard: TCA8418 ready, polling every 15 ms (INT-gated)`; the base on unit A answers |
| Automatic with a keyboard present | opened **landscape first time**, no restart: `rotation mode automatic (stored), keyboard present: rotation 270, 1232x568`, `DRM plane rotation 270 degrees`, touch `rotation 270: swap 1, calibration 2400,0,0,1060 onto 1232x568`, `launcher: 6 column(s), 2 row(s)` |
| Forced Portrait with the keyboard attached | applied in place and stayed portrait; the keyboard stayed present and polled; touch went back to `rotation 0 … onto 568x1232` |
| Presence lost while running (reset line held low) | the shell reported `keyboard: the controller stopped answering; retrying`, published **unknown** rather than absent, and turned the display **portrait** |
| Presence restored (line released) | back to present, and the display turned **landscape** again |
| Every change applied in place | pid **16105** throughout, same process start time, **4 in-place restarts, 0 supervisor restarts** |
| No oscillation | 20 s idle with the keyboard attached: no further restart, still landscape |
| The eleven apps, landscape, keyboard attached | all open and come home |
| Faults | no new crash report, no ERROR in any log; the only WARN is the expected `the controller stopped answering; retrying` from the bench holding reset |
| Shell memory (VmRSS = VmHWM) | landscape with keyboard 12,544 kB; forced portrait 12,416 kB; landscape again 12,288 kB; after the eleven apps 13,056 kB (the `54e01f7` build measured 12,288 kB idle / 12,928 kB after apps) |

What the remote pass cannot show, and the panel must: the rounded corners, the
direction the picture actually turns, taps landing under the finger, and a base
board that is physically there or not - the reset line proves the shell reacts
to the bus going silent, not that mating the connector is what does it.
