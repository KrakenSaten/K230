<!-- Copyright (c) 2026 PocketOS authors. SPDX-License-Identifier: Apache-2.0 -->
# Relay Nine rendering and native artwork

Host evidence, 2026-10-22. Base: verified `origin/master` `7ee12b3e` (merged
PR #104). Performance-only source: `6018dbf`; artwork: `9068361`. The existing
[device baseline](RELAY_NINE_DEVICE.md) and [content handoff](RELAY_NINE_POLISH.md)
remain the history of their respective runs. No device access, deployment,
toolchain installation, merge or release was performed here.

## Changes and measured costs

Sampling `gprof` on the existing stair/combat slice attributed approximately
33% of sampled self time to `rn_texture`, 15% to `put`, and 7% to room tinting.
Instrumentation changes absolute timings; these proportions only identified
candidates. Uninstrumented comparisons below determine the retained changes.

- Two fixed channel tables replace the exact integer shade arithmetic for
  RGB565's 32/64 component levels and 201 supported light values. Material
  recipes, clamping, texture wrapping and room tinting retain their output.
- Wall spans calculate constant depth and texture U once per column, and
  reject hidden pixels before texture work. Plane row bounds, portals, depth
  units, sprite occlusion and traversal caps are unchanged.

These changes do not add a framebuffer or per-frame allocation. The tables
add exactly **19,296 bytes** of static storage, initialized with the six existing
textures. Game allocation remains **21,312 bytes**, frame **256,016 bytes**,
textures **49,152 bytes** and helper mixer **384,096 bytes**. No audio code,
simulation, map, controls, frame cap or resolution changed.

The final artwork refines recessed steel vents, pipe collars, bolt highlights,
warmer stained concrete, door ribs and quieter floor trim. Raised edges keep
their existing hazard markings. Guard shoulders/head have bevels, recessed
optics and visible knee plates; scavengers have articulated foot tips and a
beveled shell. Both weapons have distinct barrel/sight/receiver construction,
glove seams, a compact hot-core muzzle crown and an open chamber/breech during
reload. Reload lowering is restrained so the open mechanism stays visible.
Existing simulation timers select every pose; reduced motion keeps a fixed
reload pose and suppresses recoil as before. The world palette remains
independent of the DOORS theme.

## Comparable host measurements

| Scene | Base p50 / p95 / p99 | Performance only | Final artwork |
| --- | --- | --- | --- |
| slice | 1.260 / 1.559 / 3.209 | 1.024 / 1.496 / 2.898 | 0.999 / 1.279 / 1.664 |
| closed-door | 1.161 / 1.555 / 2.401 | 0.908 / 1.195 / 2.265 | 0.907 / 1.101 / 1.359 |
| shotgun | 1.307 / 1.611 / 2.413 | 1.074 / 1.423 / 1.827 | 1.079 / 1.487 / 2.440 |
| gallery | 1.235 / 1.529 / 1.989 | 1.018 / 1.315 / 1.707 | 1.023 / 1.411 / 1.862 |
| upper | 1.136 / 1.376 / 1.524 | 0.930 / 1.175 / 1.467 | 1.046 / 1.389 / 2.698 |
| carbine-reload | 1.325 / 1.608 / 2.369 | 1.116 / 1.633 / 2.012 | 1.121 / 1.405 / 1.705 |
| shotgun-reload | 1.327 / 1.690 / 2.134 | 1.164 / 1.547 / 1.828 | 1.086 / 1.406 / 1.962 |

Values are medians of each run’s percentiles, in milliseconds. Final p50
is 8–22% lower across these fixtures. The stair-slice p50 run range is
1.210–1.318 ms before and 0.994–1.115 ms final; its p95 range is
1.541–1.841 → 1.208–1.575 ms. Tails overlap and are not uniformly improved:
upper-room p95 is essentially unchanged (1.376 → 1.389 ms) and its median
p99 is worse (1.524 → 2.698 ms), with overlapping run ranges of
1.472–4.192 → 1.436–4.080 ms. No uniform tail-latency win is claimed.

Replay peak RSS: base **1,404 KiB** in all three runs; performance only
**1,428 KiB**; final **1,428–1,436 KiB**. This is the replay process, not shell
or device memory. Static replay BSS is **433,152 → 452,448 bytes**; the exact
difference is the 19,296-byte channel tables. Code text grows separately;
[section sizes](relay-nine/render-polish/size.txt) retain all three binaries.

CPU timings use GCC 14.2.0, `-std=c11 -O2 -Wall -Wextra -Werror`, on the same
AMD EPYC 9V74 host. Each frozen scene uses the same camera, gameplay state and
twenty yaw offsets, 20 warm-up renders, then 400 timed frames. Seven fixtures
cover closed doors, stairs/robot, shotgun discharge, gallery, raised control
room and both reload poses. Three rounds alternate baseline → performance →
final, then reverse order, then forward order. No builds or other test suites
run concurrently. CPU time excludes hashing, captures, LVGL, flush and audio.
Host scheduling/cache conditions still vary; tails are reported, not selected
away. The raw [runs](relay-nine/render-polish/runs.json) retain every percentile
and peak RSS sample, including the performance-only intermediate version.

LVGL's path was inspected separately: mutable RGB565 source, nearest-neighbor
image scaling with antialiasing disabled, cache invalidation for changed pixels,
and a scale setter that already returns early when its value is unchanged.
The isolated `relay_image_bench` measures actual LVGL draws to the existing
partial RGB888 display buffer with a no-op flush. Release build (`-O3
-DNDEBUG`), pinned LVGL `59dc7e4`, 20 warm-ups and 200 samples, three runs:

| Image scale | Median of run p50 / p95 / p99, ms |
| --- | --- |
| 1× | 0.007 / 0.010 / 0.070 |
| 1.5625× | 0.912 / 1.447 / 2.259 |
| 2.5× | 2.173 / 3.081 / 4.305 |

[Raw LVGL results](relay-nine/render-polish/lvgl.txt). Scaling remains a
substantial cost. A pre-scaled app buffer would add output-sized storage and
another copy, plus exact sampling/rounding obligations; this pass retains the
measured renderer savings without that tradeoff. No LVGL, shared display or
app scaling behavior changed. These isolated costs are not shell/DRM timings,
and do not measure K230 display flush or physical input latency.

## Actual matching engine captures

All paired images are unretouched production renderer output at **320×200**,
saved from identical frozen gameplay states by `relay_render_replay`. Viewport,
camera and game state match; differences are intentional artwork. These are
host engine captures, not concepts or device photographs. Inspect at native
scale as well as enlarged. The stair view shows occlusion, tread edges
and the guard; the shotgun view is a production-tour discharge.

| Base `7ee12b3e` | Finished artwork |
| --- | --- |
| ![Before stairs and guard](relay-nine/render-polish/before-slice.png) | ![After stairs and guard](relay-nine/render-polish/final-slice.png) |
| ![Before shotgun discharge](relay-nine/render-polish/before-shotgun.png) | ![After shotgun discharge](relay-nine/render-polish/final-shotgun.png) |
| ![Before upper control room](relay-nine/render-polish/before-upper.png) | ![After upper control room](relay-nine/render-polish/final-upper.png) |
| ![Before carbine reload](relay-nine/render-polish/before-carbine-reload.png) | ![After carbine reload](relay-nine/render-polish/final-carbine-reload.png) |
| ![Before shotgun reload](relay-nine/render-polish/before-shotgun-reload.png) | ![After shotgun reload](relay-nine/render-polish/final-shotgun-reload.png) |

Actual final shell output with Large text:
[portrait](relay-nine/render-polish/shell-portrait-gameplay.png),
[landscape](relay-nine/render-polish/shell-landscape-gameplay.png),
[1280×720 HDMI layout](relay-nine/render-polish/shell-hdmi720-gameplay.png).
Shell smoke screenshots use timed input; the exact-state comparisons are the
native pairs above. Layout audits are saved alongside the captures.

## Focused validation

- `relay_render_compare.sh 7ee12b3 6018dbf`: entire color/depth buffers and
  counters match over **2,027 frames**, consisting of 463 sampled full-tour
  frames, two 720-frame 360° sweeps and 124 weapon/reload/reduced-motion poses.
  Frame-byte FNV-1a is `68dd5707179119bc` for both versions. Every material
  texel/light combination, fallback material, clamped lights and wrapped
  coordinates also match the separately compiled original art function.
  This output-preservation claim applies to the performance commit; final
  artwork intentionally changes pixels, silhouettes and masked sprite depth.
- Final engine/renderer/mixer: **29,813 assertions**, including low-wall
  sprite occlusion, door/pump shot occlusion, near portals, stairs and
  equivalent simulation tick schedules. Final ASan + UBSan run passes the
  same suite (`ASAN_OPTIONS=detect_leaks=0`; no LeakSanitizer claim).
- Complete input-only production tour: **4,621 fixed steps / 46.21 simulated
  seconds**, nine kills, shotgun acquired/fired, secret found, relay restored,
  exit won, HP 100 and carbine 24/76. Ordinary route also wins in 4,141 steps
  without shotgun or secret. Both remain identical to the base run. These
  are scripted simulation times, not human completion estimates.
- Final real LVGL app: **748 assertions**, Small/Medium/Large × portrait /
  landscape / 720p, including both weapons, touch and keyboard compound
  controls, pause/restart, source loss and held close/reopen cleanup. The
  existing 26 actual audio-service file/null checks pass; audio source is
  unchanged and no physical speaker result is claimed.
- Actual shell smoke passes all three layouts: title/sound/controls,
  gameplay, raw physical-key translation/release, pause, Back/Home, reopen;
  three layout audits have zero issues. Native frames and app/shell captures
  were inspected. Focused architecture lint and diff whitespace checks pass.

No unrelated app tests or baseline-failure cleanup were added. The previous
unrelated SPDX failure remains outside this work.

## Reproduce

Use the existing prepared host activation from [the handoff](RELAY_NINE.md).
No target toolchain is required. For the exact output-preserving comparison:

```sh
bash tests/relay_render_compare.sh 7ee12b3 6018dbf
make CC=gcc relay-nine-test
ASAN_OPTIONS=detect_leaks=0 make CC=gcc relay-nine-san-test
```

For each of `7ee12b3`, `6018dbf`, `9068361`, export its `relay.c`, `relay.h`,
`relay_render.c` and `relay_art.c` into a separate `work/<variant>/` directory,
then compile the current replay harness against those exact sources:

```sh
gcc -std=c11 -O2 -Wall -Wextra -Werror -Iwork/<variant> -Itests \
  tests/relay_render_replay.c work/<variant>/relay.c \
  work/<variant>/relay_render.c work/<variant>/relay_art.c \
  -lm -o work/<variant>/replay
python3 tests/relay_render_measure.py work/measure \
  before="$PWD/work/before/replay" perf="$PWD/work/perf/replay" \
  final="$PWD/work/final/replay"
```

The harness captures PPM pixels without resampling. Committed PNGs are
lossless RGB conversions of those PPMs. `size` records static executable
sections; the measurement driver samples Linux `/proc/<pid>/status` VmHWM
every 5 ms for process peak RSS (page granularity, excludes shell/helper).

```sh
cmake -S ui/shell -B out/relay-measure -DCMAKE_BUILD_TYPE=Release \
  -DPOCKETOS_DISPLAY=sdl -DLVGL_DIR="$PWD/vendor/lvgl"
cmake --build out/relay-measure --target relay_image_bench -j4
out/relay-measure/relay_image_bench
```

Use the existing `neon_app_test` / `neon_shell_test.sh` commands with
`RELAY_SHOTS` / `SHOTS_DIR` to regenerate app/shell captures. The host shell
and app validation build uses the existing default CMake configuration;
its timings are not substituted for the optimized CPU comparison.

## Local K230 handoff — not run here

The existing pinned Xuantie toolchain is absent in this executor; no cross
build was attempted and no replacement was installed. On the prepared local
WSL host, build only the affected DRM shell with the pinned SDK/sysroot as in
[RELAY_NINE_DEVICE.md](RELAY_NINE_DEVICE.md#reproduce). The unchanged audio
helper may be reused only when its identity matches the existing kit record.
Use `docs/hardware/relay-nine-gate/` and its existing staging/restoration and
`DOORS_RELAY_STATS` instrumentation for a separately authorized device pass.

Compare base `7ee12b3e` and this branch on the **same device, build flags,
layout, scene/camera/game state and input route**: full level, stairs/combat,
both weapons and reload, title idle, held exit/reopen. Retain every 5-second
window's scene, LVGL draw, flush/wait and interval p50/p95/max, CPU/RSS/HWM,
stalls, cap counters and helper xrun/cleanup evidence. Do not generalize the
recorded 30 fps median or 11.2–14.3 ms render median beyond those recorded
device scenarios. Compare against the matching old kit measurements, not
only against a best window; preserve its restoration checks.

**Expected, not verified on K230:** less repeated integer shading and wall
depth setup. The extra 19 KiB table may affect target cache behavior, and
RISC-V compiler/code generation differs. Host speedups cannot establish K230
frame rate, LVGL/DRM cost, flush time, physical input latency or acoustic
onset. Target shotgun pickup/switch/reload, physical keyboard rollover,
portrait/HDMI and individual weapon cues by ear retain the limitations
listed in the device handoff. No device action was performed.

## Provenance

All revised material, enemy and weapon pixels are original hand-authored
first-party recipes in `relay_art.c`, Apache-2.0. No image generation,
external assets, concept pixels, palette imported from the shell theme or
new dependency. PNGs are actual engine/shell captures covered by the existing
`docs/apps/*` CAPTURE inventory rule. Existing fonts/chrome keep their terms.
