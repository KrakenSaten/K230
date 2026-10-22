<!-- Copyright (c) 2026 PocketOS authors. SPDX-License-Identifier: Apache-2.0 -->
# Neon Outpost

**Historical prototype, superseded on PR #104 (2026-10-22).** This page and
`neon.c` / `neon_render.c` record the preserved flat-grid prototype. They are
not the current game. See [Relay Nine: implemented game and validation](RELAY_NINE.md).
The launcher now displays Relay Nine; the internal app ID remains `neon-outpost`.
All behavior and results below describe the earlier prototype only.

An original, short, single-player software-raycast FPS for Doors. Open **Games →
Neon Outpost**, read the help and press **PLAY** (Enter on a keyboard). Clear a
route through the four connected sectors, open pink doors, and stand inside the
green exit frame and **USE** to win. Defeating every enemy is optional. Death
ends the run; **RETRY** or R starts a fresh level.

One handcrafted 16×12 level, one hitscan pulse weapon, six enemies (diamond
crawlers close to melee range; triangular sentries stop and attack at range),
ammunition bars and health crosses. Start with 100 health and 36 rounds; all
six enemies take fourteen hits in total. Three ammunition pickups add twelve
rounds each; three health pickups restore thirty health. Ammo caps at 99 and
health at 100. No campaign, multiplayer, vertical aiming, jump or save.
**Leaving, Home, Back, or a rotation restart abandons the current run.**

| Input | Action |
| --- | --- |
| W / S, Up / Down | Step forward / backward |
| A / D, Left / Right | Turn left / right |
| Q / E | Step sideways |
| Space | Fire |
| F | Use a nearby door or the exit |
| P / Esc | Pause / resume |
| H | Help (pauses); Enter / PLAY returns |
| R / RETRY | Restart |
| Touch | Hold LEFT, FWD, RIGHT, BACK, FIRE or USE; release to stop |

The physical keyboard stream (`pos_input`) delivers discrete presses, not held
keys. Each movement press produces a 120 ms pulse; repeated presses extend it.
Firing is rate limited to one shot per 280 ms. Touch is a single-pointer stream
(`platform_drm.c` / LVGL evdev), so the controls require only **one action at a
time**: turn, release, advance, release, fire. No assumed multitouch, latched
movement, auto-fire on behalf of the player, or new keyboard driver. The normal
shell Back/Home actions and back slab remain available.

## Implementation and limits

- `apps/neon-outpost/neon.c`: pure C simulation, ray/grid intersections,
  swept-by-small-step circle collision, first-visible-target hitscan, LOS
  attacks and a breadth-first distance field. No UI, IO, hardware, heap or RNG.
- `neon_render.c`: 256×160 RGB565 framebuffer, procedural wall/door panels,
  depth-tested geometric sprites, weapon/crosshair and bounded visual feedback.
  Flat floor and ceiling. Scales with one uniform factor; controls keep at
  least 64×64 pixels. Portrait uses bottom controls; landscape/HDMI use a side
  panel. Shell geometry remains authoritative; no physical panel size in app
  layout. Help scrolls vertically if a smaller body needs it.
- `neon_app.c`: one LVGL timer (30 ms), one approximately 83 KB app allocation
  containing the reusable 81,920-byte framebuffer, one logical key target.
  HUD strings change only when their contents change. Renderer scratch is
  bounded (256 depths, at most thirteen sprites); every ray visits at most
  192 cells. Navigation visits at most 192 cells every 200 ms. No workers,
  processes, files, background activity or campaign persistence.
- Fixed 10 ms steps, at most 25 per advance. Delays over 250 ms in the UI pause
  the game instead of catching up. Keyboard pulse expiry splits elapsed time
  at the deadline. Focus loss, redirected input, screen-off and lock pause and
  release actions; return requires PLAY. Closing deletes the timer, clears
  input, detaches the image descriptor and drops the image cache before freeing
  its buffer. Restart resets every enemy, pickup, door and timer.
- No screen shake, bob, animated transitions or strobing. The same restrained
  rendering applies with reduced motion enabled: a brief local muzzle mark and
  edge damage indicator, rather than a full-screen flash.
- **Audio is intentionally unavailable in this version.** Source inspection
  found no generic game-effect API. `core/pocketaudio` belongs behind hardware
  owners; ADR-004 / ADR-010 limit per-operation helper exceptions to named apps.
  RIFT's `pos-record` notification path is explicitly scoped to RIFT and would
  need a new client lifecycle and temporary WAV/helper infrastructure. Neon
  neither opens ALSA nor assumes SDL/device audio. Firing, ammunition changes,
  enemy removal, damage edges/HP and terminal screens supply visual feedback.

Map validation rejects malformed dimensions/characters, open boundaries,
invalid/blocked spawns, malformed door alignment and unreachable floor/exit
cells (with doors treated as usable connections). The level and entity spawn
positions are checked in the tests. Doors stay open once used. A use ray stops
at the first obstacle; shots and enemy attacks require line of sight. Death
and victory stop simulation, damage and pickup processing.

## Build and run on another PC

Use the existing host prerequisites and **pinned** LVGL source from
[BUILD_ENVIRONMENT.md](../BUILD_ENVIRONMENT.md). No new dependency is added.
From a clean checkout:

```sh
git fetch origin
git switch feat/neon-outpost
# Populate vendor/lvgl at the documented 59dc7e4 pin using the existing setup.
cmake -S ui/shell -B out/neon-shell -DPOCKETOS_DISPLAY=sdl \
  -DLVGL_DIR="$PWD/vendor/lvgl"
cmake --build out/neon-shell --target pocketos-shell neon_app_test -j4
out/neon-shell/pocketos-shell --no-lock --open neon-outpost
# Explicit layouts, also useful with SDL_VIDEODRIVER=dummy for screenshots:
out/neon-shell/pocketos-shell --no-lock --open neon-outpost --rotation landscape --text-size large
POCKETOS_EXTERNAL_DISPLAY=1280x720 out/neon-shell/pocketos-shell --no-lock --open neon-outpost --text-size large
```

Focused verification (no broad repository suite):

```sh
make CC=gcc neon-outpost-test
make CC=gcc neon-outpost-san-test
out/neon-shell/neon_app_test
# Optional deterministic real-LVGL captures, all states at Large in four layouts:
mkdir -p out/neon-shots
NEON_SHOTS="$PWD/out/neon-shots" out/neon-shell/neon_app_test
# tools/pos/pos must be host-built using the documented host build first.
SHELL_BIN=out/neon-shell/pocketos-shell bash tests/neon_shell_test.sh
make CC=gcc tests/home_layout_test && ./tests/home_layout_test
bash tests/license_audit_test.sh
```

If a tracing runner prevents LeakSanitizer from starting, use
`ASAN_OPTIONS=detect_leaks=0 make CC=gcc neon-outpost-san-test`; ASan and UBSan
remain enabled. This is the setting used in the cloud, not a passing leak scan.
The existing `tools/legal/public_source_tree.sh --list HEAD` export rules apply.
Do not fetch or install a new SDK/toolchain for this feature.

## Validation recorded 2026-10-22

Host: x86_64 Linux, GCC 14.2.0, repository-pinned LVGL and existing SDL simulator.

| Check | Observed result |
| --- | --- |
| Native `pocketos-shell` and `neon_app_test` build | Passed; no new-source compiler warnings |
| Engine/renderer regression suite | 62 checks passed |
| Complete deterministic input-driven run | Victory after 2,981 simulation steps (29.81 s); six kills, HP 100, ammo 58 |
| ASan + UBSan engine/renderer | Passed, leak detection disabled due to runner tracing |
| Real LVGL controls/lifecycle/layout | 843 checks; Small/Medium/Large × portrait/landscape/720p/1080p; 20 repeated held-action closes |
| Launcher layout regression | 369 checks passed |
| Real SDL shell smoke | Portrait, landscape, 720p at Large; pointer PLAY, physical key W/P, pause, Back/Home and reopening |
| Licensing/public-source inventory audit | New feature files/captures pass; one pre-existing missing SPDX header in `tests/hw/mesh_explicit_path_rf.sh` on `origin/master` keeps the whole-repository audit red |
| Final bounded diff review | Completed; test failures remain fatal with NDEBUG, elapsed combat timing compared, shell audits asserted |
| Architectural lint | Pure core, bounded lifecycle and game registration passed |

The licensing audit reports exactly one failure: the unchanged `origin/master`
file `tests/hw/mesh_explicit_path_rf.sh` has no SPDX header. No unrelated radio
work was changed. Every asset is classified, public-source exclusion checks pass,
and all newly added source files carry Apache-2.0 headers.

The deterministic driver in `tests/neon_pilot.h` chooses movement, turn, fire and
use inputs. It does not teleport, remove enemies, open doors, grant ammo/health,
or assign a win state. The LVGL capture fixture runs that same production game
instance; the death capture waits through actual enemy attacks. Renderer bounds,
malformed map input, collision, doors, wall occlusion, nearest-target shots,
attack ranges, navigation through a doorway, pickup single-use/clamps, terminal
states, restart and equivalent elapsed-time scenarios are checked separately.

Host CPU timing for **300 256×160 renders** with changing angles: approximately
**0.103 ms/frame** at `-O2`; the sanitizer build measured approximately 0.25 ms/frame. These
measure the pure renderer only, excluding LVGL scaling/flush, and **are not K230
frame-rate measurements**. There is no target-performance claim.

## Captures and provenance

Every added image below is a host capture of the actual LVGL app. `shell-*`
images include the real shell chrome; the other captures use the integration
harness's labelled frame. They are not static game mockups. All four harness
layouts include gameplay, firing, sentry, help, pause, death and victory at Large
text. The script also exercises Small and Medium without saving captures.

| Representative view | Capture |
| --- | --- |
| Real shell, portrait help | [Help](neon-outpost/shell-portrait-help.png) |
| Real shell, landscape gameplay | [Gameplay](neon-outpost/shell-landscape-gameplay.png) |
| Real shell, HDMI gameplay | [720p](neon-outpost/shell-hdmi720-gameplay.png) |
| Firing / triangular sentry | [Firing](neon-outpost/landscape-firing.png) · [Sentry](neon-outpost/landscape-sentry.png) |
| Pause | [Landscape](neon-outpost/landscape-pause.png) |
| Death | [Portrait](neon-outpost/portrait-death.png) |
| Victory | [1080p](neon-outpost/hdmi1080-victory.png) |

All new engine/UI/test code, the handcrafted map and the procedural visual
recipes were written for this feature under the repository's Apache-2.0
conventions. No imported game art, fonts, audio, engine source or translated
code was added. Raycasting uses elementary grid-boundary intersections and
camera projection. No Doom/Wolfenstein/GPL/proprietary game material was used.
Captures use the existing Doors UI/font assets under their existing terms;
`docs/licensing/asset-inventory.txt` classifies `docs/apps/*` as CAPTURE. No new
third-party notice or public-source exclusion is needed.

## Smallest local check still required

The pinned Xuantie V3.0.2 target toolchain and SDK are **absent in this cloud
checkout**; none was installed or upgraded. Use the already configured local
WSL build tree and the existing package rebuild instructions in
[BUILD_ENVIRONMENT.md](../BUILD_ENVIRONMENT.md) to compile the branch with the
pinned toolchain. No device access, deployment, radio operation, merge or release
was performed here.

Once the owner has arranged a local build/device session: open the game, try
several discrete physical keys and one-finger holds/releases (including moving
off a control), pause/lock/screen-off and resume, then leave via Back/Home and
reopen. Check one short fight and an exit, rotate to confirm the warned run
reset, and measure actual frame/flush latency and CPU on K230 in panel and
established HDMI modes. This is the remaining physical input/performance gate;
host captures prove layout, not device delivery or frame rate.
