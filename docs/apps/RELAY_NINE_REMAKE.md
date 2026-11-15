<!-- Copyright (c) 2026 PocketOS authors. SPDX-License-Identifier: Apache-2.0 -->
# Relay Nine — Reactor Chamber visual remake

Implemented 2026-11-15 against `ee7c7f1d`. This document describes the current
presentation; earlier Relay Nine design and performance sheets describe their
recorded versions. **Host validation only. No K230 deployment or firmware change.**

## Game and art direction

The actual game is a first-person sector shooter, with height-aware sightlines,
working stairs, moving doors, windup telegraphs, two weapons, optional equipment
and a secret route. Restoring the upper relay unlocks the return journey to the
exit. Its mechanics, world geometry, collision, enemy navigation, damage, ammo,
sound events and fixed 10 ms simulation remain in the unchanged `relay.c`.

The new direction is an optical chamber inside an emergency reactor: black
basalt, layered ceramic armor, blue superconducting spines, violet machinery,
exposed sentinel cores and amber locks. It replaces every material recipe,
weapon, pickup, machine silhouette and title illustration. Cyan communicates
current, pink hostile cores/damage, amber windup/locks, green recovery and restored
power, and violet the second weapon. These cues supplement the written HUD.

The portrait viewport changes from 320×200 to **320×360 RGB565**. The horizontal
projection and optical center stay the same; the camera sees more ceiling and
floor. It is scaled uniformly into the portrait optical chamber, with readable
instruments above, an objective strip below, two circular compound touch pads,
and a separate action row. Landscape and HDMI have side consoles. There is no
stretching, crop, altered aiming or change to touch axes. Touchable pad rectangles
retain their full area; the circles are visual affordances. Action targets are
at least 80 px tall in gameplay, 72 px in menus.

The typography uses the existing generated IBM Plex fonts. No font, image,
engine, SDK or runtime dependency is added. The six albedo/emission maps and
original procedural silhouettes are first-party recipes in `relay_art.c`.

## Rendering decision

| Approach | Benefit | Cost / decision |
| --- | --- | --- |
| Existing portal renderer + LVGL | Already handles real sector heights, doors, per-pixel sprite occlusion and display handoff | Retained and extended locally to the game |
| Individual LVGL world objects | Crisp interface shapes | Does not solve textured 3D visibility; creates many objects and invalidations |
| GPU / replacement engine | Potential richer lighting and geometry | No verified accelerated game API or compatible engine in the pinned build; new dependencies and display integration would need separate justification |
| Full-resolution software scene | More fine detail | 568×1232 color/depth alone would be 2.67 MiB and over six times this scene's samples; unjustified before device measurement |

The renderer still draws into one owned image and depth buffer. The shell's
DRM, evdev and audio ownership are unchanged. The existing game style extension
in `pos_styles.c` holds the cockpit roles and authored ink palette; unrelated
application styles are unchanged. No direct
hardware access or worker thread is introduced.

## Motion, light and load control

- Cached 64×64 albedo and independent emission maps retain light in dark rooms.
  A bounded current sweep follows surface texture coordinates. It uses the
  simulation clock, freezes on pause, and becomes static with Reduced Motion.
- Room tint, integer distance haze and edge falloff give spatial separation.
  A 4×4 light-cell field spreads short cyan halos from bright sources; there is
  no full-resolution convolution, shadow solver or per-frame heap allocation.
- Nine depth-tested impact particles disperse from the original impact point.
  Muzzle light, the redesigned shot crown, segmented glove, recharge/ejection
  poses, reticle and broader damage edges respond to existing game timers.
- Sentinel carapaces expose red cores, amber windups and pale hit responses.
  The existing directional, gait, attack and collapse states select the art.
- Success/failure get distinct rings, a check or fractured cross, and a 1.4 s
  eased expansion. Restart/menu input stays active. Reduced Motion shows a
  static emblem. These effects reuse the existing app timer.
- Four scene renders over 24 ms, or eight gameplay intervals over 45 ms, enable
  economy mode. It samples alternate world columns, reconstructs their depth,
  omits the optional halo pass and uses nearest image scaling. Sprites, weapon,
  reticle, interaction and simulation retain their resolution and cadence.
  After 180 renders below 14 ms, full detail is probed again. Thresholds are
  conservative **ASSUMED device tuning**, not a K230 performance claim.
- Static labels, styles, title art and framing are only invalidated when their
  values or geometry change. Animated scene pixels still invalidate their own
  image. Audio and input continue through the existing foreground lifecycle.

## Visual evidence

These are actual LVGL captures at the same production-tour checkpoints, using
Large text at native 568×1232. The old 8:5 viewport and the new tall viewport
are intentionally shown at their real sizes. They are not concept art.

![Combat before and after](relay-nine/remake/comparison-combat.png)

![Upper control room before and after](relay-nine/remake/comparison-upper-control.png)

![Title, stairs combat, shotgun and completion](relay-nine/remake/presentation.png)

[Production renderer motion](relay-nine/remake/gameplay.gif) and
[fixed-camera surface current](relay-nine/remake/energy-current.gif) are captured
from the actual rendering functions. The first follows the deterministic pilot;
the second advances only the visual clock in a fixed camera fixture. GIF timing
illustrates animation and is not a measured device frame rate.

The comparison shows a different world material language, object silhouettes,
weapon construction, camera composition, lighting and control presentation.
The remaining deliberate limitation is software-rendered detail at 320 pixels
across. Antialiased scaling softens fractional magnification, but this does not
establish final on-glass quality or readability.

## Host performance and memory (VERIFIED here)

GCC 14.2.0, `-O2`, Linux x86-64, process CPU time; 400 timed frozen-state renders
following 20 warmups and identical view bearings. Full replay: 2,027 frames per
variant, including the full tour, two 360° sweeps, both reloads and Reduced Motion.
All variants finish the tour in **4,621 steps**, health 100, nine kills and ammo
24/76. The optional-free main route still wins in **4,141 steps**.

| Frozen state | Before p50 / p95 ms | Full detail p50 / p95 ms | Economy p50 / p95 ms |
| --- | --- | --- | --- |
| Stairs | 1.20 / 1.71 | 3.51 / 6.12 | 1.95 / 2.66 |
| Closed door | 1.13 / 1.44 | 3.30 / 5.56 | 2.00 / 2.63 |
| Shotgun discharge | 1.26 / 1.61 | 3.48 / 5.59 | 2.07 / 2.67 |
| Gallery | 1.22 / 1.57 | 3.64 / 5.89 | 2.15 / 2.97 |
| Upper control | 1.21 / 1.54 | 3.41 / 4.28 | 2.01 / 2.63 |

The new frame contains 1.8× as many pixels and additional light/effect work.
This is a quality/cost tradeoff, not a speedup over the old art. Raw results:
[before](relay-nine/remake/before-timing.txt),
[full detail](relay-nine/remake/after-timing.txt),
[economy](relay-nine/remake/economy-timing.txt).

The isolated antialiased LVGL image bench measures 4.05 ms p50 / 4.84 ms p95 at
400/256 scale with a no-op flush ([raw](relay-nine/remake/image-bench.txt)). It
excludes the rest of the cockpit, DRM and physical scanout. Larger test scales
may clip to that bench's 1280×720 display. These numbers cannot predict K230 FPS.

| Fixed allocation | Before | After |
| --- | ---: | ---: |
| Game state | 21,312 B | 21,312 B |
| Color + depth + counters | 256,016 B | 460,816 B |
| Material and shading tables | 68,448 B | 117,600 B |
| Sum above, excluding LVGL/audio/objects | 345,776 B | 599,728 B |

The added fixed storage is 253,952 B. The light-cell field uses a bounded 7,200 B
local array; GCC's [stack report](relay-nine/remake/relay_render.su) gives 7,176 B
for that function after optimization. This is compile-time/host evidence, not a
measurement of total device RSS, CMA or frame high-water marks.

## Validation and reproduction

VERIFIED on this host:

- Full SDL shell build; focused changed code compiles with `-Wall -Wextra -Werror`.
  Initial full-shell build reports existing warnings in unrelated applications.
- `make CC=gcc relay-nine-test`: engine 29,820 checks; visual invariants 745
  checks; actual service/ALSA file-output audio 26 checks.
- `make CC=gcc relay-nine-san-test`: both engine and visual checks pass under
  AddressSanitizer/UndefinedBehaviorSanitizer.
- `neon_app_test`: 1,293 checks across Small/Medium/Large, 568×1232, 1232×568
  and 1280×720; touch movement/turn/fire, releases, focus/lock/screen-off,
  switching, pause/restart, title/help/sound, full route, death/victory,
  repeated close/reopen, timer/event cleanup and load fallback.
- `relay_sdl_input_test`: 21 physical key translation/held-input checks.
- `neon_shell_test.sh`: real-shell all-layout/all-text-size overlap audits,
  full-screen/menu transitions, pointer, physical key path, Back/Home and reopen.
  A final Large run also passes after the layout and redraw refinements.
- Rendering remains deterministic, does not mutate the game, preserves depth
  while animating current, freezes that current under Reduced Motion, and
  retains sentinel occlusion behind low walls in both detail modes.
- `text_size_ui_test`: 199 shared style checks, zero failures.
- `style_lint.sh`, `neon_outpost_lint.sh`, `license_audit_test.sh` and
  `git diff --check` pass.

Normal simulator commands from a host with the documented development packages:

```sh
cmake -S ui/shell -B work/relay-shell -DCMAKE_BUILD_TYPE=Release
cmake --build work/relay-shell --target pocketos-shell neon_app_test \
  relay_visual_test relay_sdl_input_test relay_image_bench -j6
make CC=gcc relay-nine-test
make CC=gcc relay-nine-san-test
mkdir -p work/relay-shots
RELAY_SHOTS="$PWD/work/relay-shots" work/relay-shell/neon_app_test
SDL_VIDEODRIVER=dummy work/relay-shell/relay_sdl_input_test
SHELL_BIN="$PWD/work/relay-shell/pocketos-shell" TEXT_SIZES='small medium large' \
  bash tests/neon_shell_test.sh
```

Here the required Debian development headers and CMake executable were unpacked
under `work/` because the managed machine lacked them. Runtime libraries, pinned
LVGL and the application dependency versions were unchanged. Frozen renders use
`tests/relay_render_replay.c`; the reference sources were exported from
`ee7c7f1d` into `work/` and compiled separately, never swapped into the live tree.

## Remaining device gate

There is no attached Unit B or pinned Xuantie SDK/toolchain in this environment.
The RISC-V/DRM build, physical touch coordinates, AMOLED appearance, thermal/load
behavior, total device RSS and actual frame pacing are **not verified** for this
change. No firmware was flashed, service stopped or live unit modified.

On a connected build host, first build the exact candidate source snapshot using
the pinned SDK. The package export uses `git archive HEAD`; a dirty-build override
does not package working changes. Keep the installed binary and its hash, stage a
separate candidate, and follow the existing [isolated Relay Nine gate](../hardware/relay-nine-gate/rn-start.sh)
and its restoration procedure. Confirm the shell returns after candidate exit
before extending the run. Use `DOORS_RELAY_STATS` for render/draw/flush/interval
windows, compare full/economy behavior with sound on and off, inspect memory over
repeated open/play/pause/close, and restore the original binary/service after the
session. There is no reason to reflash the SD image for this app-only gate.
