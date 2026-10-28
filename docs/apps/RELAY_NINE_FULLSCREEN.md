<!-- Copyright (c) 2026 PocketOS authors. SPDX-License-Identifier: Apache-2.0 -->
# Relay Nine fullscreen gameplay

Baseline: current master `5011fac9` (PR #107). Relay Nine did **not** request
fullscreen: its app descriptor used the default chrome/header, and the shell
provided a 72 px header plus 20 px side/bottom and 24 px top body padding. The
app then fitted the image into a smaller HUD/control layout (two-thirds of the
body in landscape). This was a hosting/layout constraint, not a renderer limit
or a failed display-mode request.

Gameplay now requests the existing shell chrome path's `NONE` policy, hides the
existing header and removes body padding. A small shell API changes that same
body in place; it creates no second screen, display backend or input target.
Pause, help, sound, death and victory restore the app's declared chrome and
normal header/body. Restart re-enters fullscreen, and closing clears the request
before the next app/launcher. Back/Home retain their existing meaning (leave the
run); the visible PAUSE button or P/Esc returns to normal navigation first.

## Geometry and cost

Internal rendering remains **320×200 RGB565**, with the same depth buffer,
projection, renderer, 30 ms timer and nearest-neighbour LVGL scaling. No native
resolution render, new render buffer or change to scene work was introduced.

| Layout | Body content before → gameplay now | Image before, Large | Image now, all text sizes | LVGL scale before → now (256 = 1×) |
| --- | --- | --- | --- | --- |
| Panel portrait, 568×1232 | 528×1116 → 568×1232 | 511.25×319.53 | 567.5×354.69 | 409 → 454 |
| Panel landscape, 1232×568 | 1192×452 → 1232×568 | 505×315.63 | 908.75×567.97 | 404 → 727 |
| HDMI, 1280×720 | 1240×604 → 1280×720 | 747.5×467.19 | 1151.25×719.53 | 598 → 921 |

Image extents are the exact uniform fixed-point scale applied to 320×200,
rounded here to two decimal places; raster edges can round by a pixel. The
baseline extents follow the baseline layout's integer arithmetic and match the
captured image bounds. Current body dimensions are asserted over real shell IPC;
app checks assert the maximum fitting scale, equal X/Y scales and centring.

The exact edge-to-edge conflict is aspect ratio: the panel's portrait aspect is
568/1232 ≈ 0.461, landscape ≈ 2.169, and HDMI ≈ 1.778; the rendered view is 1.6.
Filling both axes would require cropping or unequal scales, or changing the
renderer/projection and authored weapon composition. The smallest supported
solution is a fullscreen game surface with the complete 8:5 scene fitted and
centred, leaving letterbox bands. It is deliberately **not** an image stretched
across every pixel. Portrait controls occupy the lower band; landscape/HDMI
controls sit along the sides, keeping the central reticle and weapon visible.
Controls overlay the scene where side bands are narrower than their touch targets.
HUD/message backplates keep text readable over the world. Corner-safe UI insets
and the selected text roles apply independently of image scaling.

Pad input still uses the displayed LVGL button coordinates; there is no separate
render-coordinate hit-test transform to drift. Existing held-key subscriptions,
pad release/press-loss clearing and focus/screen-off/lock/stall pause paths remain.

The displayed image is larger, so LVGL scaling/blending and display flush work
can increase. Native scene work and memory are unchanged. Host tests do **not**
establish K230 frame rate, input latency or display throughput.

## Simulator evidence

Actual SDL shell captures at Large text; baseline and final smoke runs use the
same input sequence, not a pixel-identical frozen engine state. These were opened
and visually inspected, along with Small/Medium gameplay captures and the
restored pause/launcher views. Menu/help audits pass at all three text sizes.

| Layout | Before | After | Pause with restored header | Return to launcher |
| --- | --- | --- | --- | --- |
| Portrait | [gameplay](relay-nine/fullscreen/before/portrait-gameplay.png) | [gameplay](relay-nine/fullscreen/after/portrait-gameplay.png) | [pause](relay-nine/fullscreen/after/portrait-pause.png) | [launcher](relay-nine/fullscreen/after/portrait-launcher.png) |
| Landscape | [gameplay](relay-nine/fullscreen/before/landscape-gameplay.png) | [gameplay](relay-nine/fullscreen/after/landscape-gameplay.png) | [pause](relay-nine/fullscreen/after/landscape-pause.png) | [launcher](relay-nine/fullscreen/after/landscape-launcher.png) |
| HDMI 720p | [gameplay](relay-nine/fullscreen/before/hdmi720-gameplay.png) | [gameplay](relay-nine/fullscreen/after/hdmi720-gameplay.png) | [pause](relay-nine/fullscreen/after/hdmi720-pause.png) | [launcher](relay-nine/fullscreen/after/hdmi720-launcher.png) |

[Saved shell geometry and audits](relay-nine/fullscreen/audit.json) cover Small,
Medium and Large in each layout: zero gameplay, pause and controls audit issues.

## Focused validation

Used the existing prepared host, pinned LVGL and SDL/ALSA libraries; installed no
toolchain, SDK or dependencies. No broad repository sweep.

- `cmake --build out/cloud-shell --target pocketos-shell neon_app_test relay_sdl_input_test -j4`: passed, no compiler warnings in final builds.
- `out/cloud-shell/neon_app_test`: **1161 checks passed** across all three layouts/text sizes; fullscreen request/restore, centred maximum uniform scale, reachable targets, help from gameplay, held chords/release, pad centre/opposite directions, compound movement/fire, cancellation, focus/lock/stall pause, full campaign victory/death/restart and repeated teardown.
- `out/cloud-shell/relay_sdl_input_test`: **21 checks passed** (real SDL held-input path).
- `make CC=gcc relay-nine-test`: **29813 engine checks** and **26 audio checks** passed.
- `ASAN_OPTIONS=detect_leaks=0 make CC=gcc relay-nine-san-test`: **29813 checks passed**, no ASan/UBSan findings.
- `SHELL_BIN=out/cloud-shell/pocketos-shell TEXT_SIZES=<size> SHOTS_DIR=<dir> bash tests/neon_shell_test.sh`: passed separately for `small`, `medium`, `large`; each runs portrait, landscape and HDMI. Asserts full body/hidden header/cluster during play and restored chrome on pause and Home; captures help, pause and launcher; checks Back and reopening.
- `bash tests/neon_outpost_lint.sh` and `git diff --check`: passed.

Death/victory/restart requests are exercised by the real-app LVGL fixture; the
shell's resulting geometry is exercised through the same restore path on pause.
One bounded final source/diff review completed; it removed the obsolete compact
gameplay layout branches. The affected build, real-app and SDL checks and Relay
Nine lint passed again.

## Local physical display/input handoff (not performed)

No device was accessed, deployed to, merged or released. Physical observations
remain **ASSUMED** until checked locally. Use an already prepared local build
and the existing [Relay Nine device gate](RELAY_NINE_DEVICE.md); this work grants
no device/deployment access.

On panel portrait/landscape and the existing HDMI layout, check Small/Medium/Large
HUD and control readability, panel corner clearance, complete weapon/reticle,
correct scene proportions and expected letterboxing. Drag both pads through their
centres and edges, release/cancel them, hold W/turn/Space together and release
individual contacts. Check PAUSE/P/Esc, HELP, Back/Home, death/victory, restart and
reopen restore the expected shell navigation with no stuck action. Measure the
larger LVGL scale/flush cost and physical input response with the existing opt-in
stats gate; compare like scenes against master. Native renderer host timings alone
cannot clear that check.
