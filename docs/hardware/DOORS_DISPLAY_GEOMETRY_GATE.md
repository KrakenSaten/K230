# Doors display geometry: hardware gate

Branch `feature/doors-display-geometry`, from master `13029a3`.

**Result: NOT RUN.** Everything that can be checked without the panel passed
(below). One visual and touch pass on unit A remains. DS Amendment E (§21)
stays PROPOSED until the product owner accepts it after that pass.

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
Landscape only with a keyboard known to be present. Nothing publishes
keyboard presence yet (`ui/shell/kbd_presence.h`: unknown, absent, present),
so on unit A Automatic is Portrait. Landscape is DRM rotation 270, the
rotation the vendor launcher runs at on this board with its keyboard
(KEYBOARD_BRINGUP_2026-09-10.md §6): **the device turned a quarter turn
clockwise, the portrait left edge at the top**.

One geometry per shell run drives the DRM plane rotation and the evdev
touch transform, set together in `pocketos_platform_init()`; the transform
per rotation equals the vendor launcher's for the same DRM index. The old
`POCKETOS_DRM_ROTATION` path that turned the picture without touch is gone:
that variable now overrides the policy for display and touch together.

## Runtime rotation: not supported live

The orientation is applied when the shell starts, and Settings says a change
"takes effect when the Doors shell restarts". Evidence:

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

**Proposed, not implemented (needs approval):** an "Apply now" that stores
the mode, closes the app (apps already persist their state on each change),
releases the DRM device and framebuffers with an orderly teardown, and
re-executes the shell in place (`execv` of its own binary, same pid, so
`pos-supervise` sees no exit and counts no restart). Cost: the panel shows
nothing for the time of a shell start, and the open app is closed. The
alternative, re-creating the LVGL display inside the running process, needs
the whole shell UI rebuilt and gains nothing visible. The whole OS is never
restarted for this.

## Validation done without hardware (2026-09-15)

Source `e3f900d` (the last code commit; later commits change tests and
documentation only, and the shell tests were run again at the tip), from
fresh WSL clones; master `13029a3` built and run the same way as the
baseline. VERSION 0.0.9 on both.

| Check | Result |
| --- | --- |
| `make all`, host, `-Werror` | rc 0, 0 warnings |
| `make test`, host | rc 0: 3,632 ok, 0 FAIL, 0 warnings (master 3,623); `display_geometry_test` 76 checks, `orientation_test` 29, `settings_view_test` 119 (rotation notes added), `initscript_test` (the corner override is exported); the six `NOT RUN` lines are check names, identical to master's |
| Shell tests, SDL simulator | all 19 scripts rc 0 at the tip, including `display_geometry_shell_test.sh` 68 ok with `display_touch_test` 58 checks, `settings_shell_test.sh` with `settings_app_test` 81, `launcher_icons_shell_test.sh`, `shell_ipc_test.sh`, `pos_input_test.sh`, `pos_keyboard_test.sh`, `kbd_shell_test.sh`. At `e3f900d` two static checks in `calculator_shell_test.sh` and `wave_shell_test.sh` still counted grid rows written out in `home_create()`; they now read the grid from the running shell (`5ac0569`) |
| Safe area, portrait | in all 15 theme/mode pairs every ink column of the status bar lies at least 30 px from each side; three ink groups (wordmark, radio chip, clock); the wordmark's left and the clock's right clear of the 30 px corners; every tile 254 x 150 in its place with its own icon, nothing below the last row |
| Safe area, landscape | the same on 1232x568 in all 15 pairs, the bar's ends 30 px from both ends of the long edge; tiles 182 x 150 in six columns and two rows |
| Rectangular panel | with `POCKETOS_SAFE_CORNERS=0,0,0,0`, in both orientations, the bar keeps its own 20 px; with the 30 px corners the wordmark is the same pixels moved 10 px in (drawn whole) and everything below the status bar is identical: the inset comes from the platform description, not from the bar |
| Geometry against master | the portrait launcher screenshotted from both builds in all 15 pairs and compared pixel by pixel: **0 pixels differ below the status bar**; 1,277–1,281 differ in it (wordmark and clock 10 px further in) |
| Model | every rotation maps the native panel onto the logical one pixel for pixel and back; size swap at 90/270; four different edge strips and four different corners each land on the right logical edge and corner; top, bottom, left and right bars at 0, 90 and 270; a bar's end takes the larger of strip and corner; `rect_is_safe` at every corner, including the status-bar label and clock positions in both orientations; a rectangular panel gives no insets |
| Touch | the per-rotation transform equals the vendor launcher's settings for the same DRM index; a touch on native corners, centre and off-axis points lands on the logical pixel the display draws there, at all four rotations with the controller mounted straight or swapped in the model, and through LVGL's own `lv_evdev` fed a GT9895-style multitouch stream at all four rotations (swapped at 0 and 270) (`display_touch_test`); the same checks catch a landscape display with portrait touch, 270 with 90's touch, 90 with 270's and 0 with 180's |
| Policy | Automatic with the keyboard unknown, absent and present (Portrait, Portrait, Landscape); forced Portrait and Landscape with each keyboard state; manual overrides automatic; persisted across restart; invalid stored value → Automatic with a WARN, the stored text left alone; `POCKETOS_DRM_ROTATION` overrides display and touch together and is logged; an invalid one ignored |
| IPC and Settings | `shell.rotation` stores Landscape and reports `restart_required`, the running shell does not rotate, the restarted one is landscape with nothing pending; an invalid mode is error 2; Settings shows the selected mode and the note (restart pending, why Automatic chose what it did, stored value not recognised) |
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
| next rotation stale (never `restart_required`) | `display_geometry_shell_test` (1) |
| DRM touch always at rotation 0 | `display_geometry_shell_test` (1, source check: the DRM backend cannot run on the host) |
| DRM touch no longer taken from the display's geometry | `display_geometry_shell_test` (1, source check) |
| an app rotates the display itself | `display_geometry_shell_test` (1) |
| every Rotation button in Settings stores Automatic | `settings_app_test` (3) |
| the forced note names the wrong orientation | `settings_view_test` (2) |
| Settings never says a restart is needed | `settings_view_test` (2), `settings_app_test` (1) |

23 of 23 caught.

Size and memory:

| | master | branch | change |
| --- | --- | --- | --- |
| riscv64 DRM shell, `size` text / data / bss | 914,115 / 9,948 / 22,400 | 923,719 / 10,052 / 22,672 | +9,604 / +104 / +272 |
| riscv64 DRM shell, stripped file (what ships) | 928,376 B | 940,720 B | **+12,344 B** |
| SDL x86-64 shell, text / data / bss | 2,208,585 / 15,320 / 23,680 | 2,221,557 / 15,416 / 23,936 | +12,972 / +96 / +256 |
| Heap allocated by `home_create()` (glibc `mallinfo2`, simulator, 2 runs each, identical) | 13,856 B | portrait 13,856 B, landscape 13,856 B | **0** |

The grid descriptors are static arrays sized for the largest grid (bss), so
the launcher allocates the same in both orientations. The heap figure comes
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

**Operator time: about three minutes, a few taps and one answer.** Everything
else is done over SSH by the session, and every scripted check prints `ok` or
`FAIL`.

Before (session, no operator):

1. Build from the branch tip: clean WSL clone, `apply_to_sdk.sh`,
   `build_image.sh` (runs `verify_image.sh`); record the shell binary's
   SHA-256 and that the DRM build found `lv_linux_drm_set_rotation`
   (`POCKETOS_DRM_ROTATION_API`).
2. Over SSH, before deploying: `pocketos-shell` VmRSS and VmHWM on the running
   shell, launcher showing, after 60 s idle. Record, and move aside for the
   test, any `POCKETOS_DRM_ROTATION`, `POCKETOS_TOUCH_CALIB`,
   `POCKETOS_TOUCH_SWAP` or `POCKETOS_SAFE_CORNERS` in
   `/etc/default/pocketos-shell` (each would override what is being tested),
   and any `display_rotation` in `settings.conf`.
3. `platforms/k230/scripts/deploy.sh 192.168.10.157`; check `doors version`
   shows the branch tip's build, `/usr/bin/pocketos-shell` matches the build's
   SHA-256, the four services run, and `shell.log` says
   `rotation mode automatic (default), keyboard unknown: rotation 0, 568x1232`,
   `status bar insets 30/30`, and a touch line at rotation 0 with the
   controller's own ranges. `app list` shows eleven apps; every app opens and
   comes home through `app start` / `app home`; no ERROR, no crash report.
4. Landscape, still without the operator: `pos call shell shell.rotation
   mode=landscape`, `S90pocketos-shell restart`; check `shell.log` says
   `rotation 270, 1232x568`, `DRM plane rotation 270 degrees`, the touch line
   at rotation 270 with swap on, `launcher: 6 column(s), 2 row(s)`, no ERROR;
   `shell info` reports 1232x568; the eleven apps open and come home again.
   VmRSS and VmHWM in landscape. Then `mode=automatic` and restart: portrait
   again, same checks as 3, VmRSS and VmHWM.
5. Put the SDK target tree back as found (docs/hardware/DOORS_PHASE2_GATE.md,
   "After the test").
6. Start a bench watcher on the unit for the operator's pass: once a second it
   reads `shell info` and, when `restart_required` is true, runs
   `S90pocketos-shell restart`. It stands in for the "Apply now" above, which
   does not exist yet; it is stopped and removed afterwards.

Then one pass on the panel, device upright (portrait), Ice/Normal:

| # | Operator does | Pass |
| --- | --- | --- |
| 1 | Looks at the launcher | the whole `D` of `DOORS` and the clock's last digit are visible and clear of the rounded top corners, with a little margin; tiles where they were |
| 2 | Opens Settings, scrolls to Rotation in the Display card, taps **LANDSCAPE**, then turns the device a quarter turn clockwise (the old left edge now on top) | LANDSCAPE is highlighted and the note says Landscape takes effect when the shell restarts; within about 3 s the panel goes dark briefly and comes back as an upright landscape launcher, six tiles in each of two rows |
| 3 | Looks at the landscape launcher, then taps the top-left tile and the bottom-right tile (Back after each) | `DOORS` and the clock clear of the two corners at the ends of the long top edge; no tile or label cut off; each tap opens the tile under the finger |
| 4 | Opens Settings, scrolls to Rotation, taps **AUTOMATIC**, turns the device back upright | within about 3 s the launcher is back in portrait, as in 1 |

One answer: PASS if all four hold, otherwise FAIL with the step number. Taps
and restarts are checked afterwards in the unit's log.

If the corners clip on the panel in 1 or 3, the fix is a number, not code: try
another `POCKETOS_SAFE_CORNERS` in `/etc/default/pocketos-shell` and restart,
then commit the value that clears. A landscape picture upside down in 2 means
the plane turns the other way from the vendor's index (270 would become 90,
`ORIENTATION_LANDSCAPE_ROTATION`); taps that land mirrored or on the wrong axis
mean the transform, which the log line from step 4 then shows.
