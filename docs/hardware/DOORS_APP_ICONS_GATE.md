# Doors app icons: hardware gate

Branch `rebrand/doors-app-icons`, from master `95b4f58`.

**Result: PASS on unit A, 2026-09-15** (product owner's visual confirmation,
build `3174471` deployed over SSH). All eleven launcher apps show their Doors
icon on the panel, tinted and legible in Normal, Outdoor and Night, and a tap
on the Wave icon opens Wave. Every remote check passed. DS Amendment D (§20) is
accepted.

Not merged. No change to VERSION, release metadata, service or shell names,
launcher layout, typography, spacing, the focus model, the boot splash or the
System brand mark; Phase 3 not started.

## What changed

| App (`id`) | Launcher icon, an A8 mask tinted `accent_primary` |
| --- | --- |
| Radio, System, Fleet, Radar, Timber, Notes, Clock, Calendar, Calculator, Settings | `docs/design/brand/doors-threshold/icons/png-32/<id>.png` |
| Wave | `docs/design/brand/doors-icon-extension/png-32/wave.png` |

All eleven launcher apps have their icon; no tile draws a glyph. The icon
extension's twelve icons for apps that do not exist are inventoried and not
compiled in.

Data model: `struct pocketos_app` gains one appended optional field,
`const lv_image_dsc_t *icon_mask` (NULL = draw `icon` as text);
`POCKETOS_APP_API_VERSION` stays 0. PocketUI: `pocketui_tile_mask()`, and
`pocketui_tile()` is now that call with a NULL mask. Style role:
`POS_STYLE_APP_ICON`. Details: docs/design/brand/README.md, "App icons".

An earlier revision of this branch (`30e22b0`) kept Wave's speaker glyph,
because the first package had no Wave icon. The icon extension supplied it;
this sheet describes the revision with all eleven.

## Why the panel still has to be seen

The simulator proves placement, colour and navigation pixel for pixel, but it
is not the device's renderer: on unit A LVGL draws into RGB565 through the
vendor LVGL build (`LV_DRAW_SW_SUPPORT_A8 1`), and the AMOLED shows it. What is
left is whether 2 px antialiased strokes look crisp and legible there, in
daylight Outdoor and dim Night.

## The gate

**Operator time: about two minutes, one look and one tap.** Everything else is
done over SSH by the session.

Before (session, no operator):

1. Build from the branch tip: clean WSL clone, `apply_to_sdk.sh`,
   `build_image.sh` (runs `verify_image.sh`); record the shell binary's
   SHA-256.
2. Over SSH, before deploying: `pocketos-shell` VmRSS and VmHWM on the running
   `0.0.9` / `691b508` shell, launcher showing, after 60 s idle.
3. `platforms/k230/scripts/deploy.sh 192.168.10.157`; check `doors version`
   shows the branch tip's build, `/usr/bin/pocketos-shell` matches the build's
   SHA-256, the four services run, `app list` shows eleven apps, and every app
   opens and comes home through `app start` / `app home` with no ERROR in
   `shell.log` and no crash report.
4. VmRSS and VmHWM again, same conditions.
5. Put the SDK target tree back as found (docs/hardware/DOORS_PHASE2_GATE.md,
   "After the test").

Then one timed sequence on the panel (the session switches with
`pos shell theme` and says when it starts):

| Seconds | Panel shows |
| --- | --- |
| 0–20 | Launcher, Ice & Ember / Normal |
| 20–40 | Carbon & Signal Orange / Outdoor |
| 40–60 | Slate & Lavender / Night |
| after | back to the theme and mode the unit had before the gate |

Operator checklist, three answers:

| # | Check | Pass |
| --- | --- | --- |
| 1 | Icons on the panel (0–20 s) | eleven stroke icons, crisp, not blurry, blocky or fringed, light blue, at the top left of their tiles; Wave shows its waveform between two arcs, no speaker symbol anywhere; tiles and names unmoved |
| 2 | Tint and modes (20–60 s) | the icons turn orange with the Outdoor screen and legible in it, then dim lavender with Night and still recognisable |
| 3 | One tap | afterwards, a tap directly on the **Wave icon** opens Wave; Back returns to the launcher |

If all three pass: the gate is PASS, Amendment D can be put to the owner for
acceptance, and this sheet records the evidence. A FAIL on 1 or 2 is a
rendering question for the device (RGB565 or the panel), not a layout one: the
simulator checks already fix the placement.

## Result on unit A (2026-09-15)

Every check below except the three visual answers ran by script, over SSH
with key authentication and a host key read over the serial console in the
Phase 2 gate, and printed `ok` or `FAIL`. The only operator actions were
watching the panel and one tap.

**Build and provenance.** Clean WSL clone at `3174471` (branch tip, clean
tree, VERSION 0.0.9); `apply_to_sdk.sh` rc 0 (source worktree clean, RadioLib
`034126e` and ggwave `a38e38b` clean, BUILD_ID `3174471`); `build_image.sh`
rc 0 in 230 s, IMAGE GATE: PASS, SPLASH: PASS, `sha256sum -c` of
SHA256SUMS.txt and of the release's `.sha256`: OK. The 488 compiler warnings
in the build log are all in vendor code (opencv headers, the vendor
`face_detect` package); none is in Doors sources.

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `sysimage-sdcard.img` (not flashed) | 763,363,328 | `c98ded1d77681ecd0b94efc806377bef667738f2b9aeeee34442fb7cce5ca0af` |
| `/usr/bin/pocketos-shell` | 928,192 | `715018378f3a4416c6dd63909ce3ba1270db3c3742d11f4a70a17bbfb4df9bad` |

The shell binary is the same file in the SDK target tree `deploy.sh` ships,
in the image's root filesystem (debugfs), on the device, and behind the
running process (`/proc/<pid>/exe`); it holds each of the eleven compiled mask
byte sequences exactly once, and `/etc/doors-release` reads `0.0.9` /
`BUILD_ID=3174471` in the image and on the device.

**Before** (unit A as it was: `0.0.9` / `691b508`, Doors shell on the panel,
Ice/Normal, 0 crash reports, no WARN or ERROR line in any service log). The
old shell was restarted and measured the same way as the new one: 60 s idle on
the launcher, then after all eleven apps opened and came home (11/11), with no
WARN or ERROR logged across the restart and the round trip.

**Deploy.** `deploy.sh 192.168.10.157` from the build clone: rc 0 in 4 s,
services stopped, archive extracted, services started, `Doors 0.0.9 (build
3174471)`, `pos 0.0.9 (build 3174471)`, radio info, `system.info`, `Done.`
WSL's ssh printed a host-key-changed warning for a stale ECDSA entry in its
own `known_hosts`; the ED25519 key the unit presented,
`SHA256:q+QqO4nwkYQTH1h4Gk+JU50CufMwIpy0VN/vkik4E5g`, is the one read over
the serial console, and the Phase 2 deploys printed the same warning.

| # | Result | Evidence |
| --- | --- | --- |
| R1 | **PASS** | `doors version` `Doors 0.0.9 (build 3174471)`, `pos version` `pos 0.0.9 (build 3174471)`, `/etc/doors-release` `0.0.9` / `BUILD_ID=3174471` |
| R2 | **PASS** | `/usr/bin/pocketos-shell` and the running process's executable both SHA-256 `71501837…9bad`; `shell.info` build `3174471`, backend `drm`, eleven apps listed |
| R3 | **PASS** | `sysd`, `netd`, `radiod`, `pocketos-shell` running, 0 restarts, no crashloop |
| R4 | **PASS** | all eleven apps opened through `app start`, seen open in `app list`, and returned home through `app home`: 11/11 |
| R5 | **PASS** | 0 crash reports before, 0 after the round trip, 0 after the visual check; `doors logs --crashes` none |
| R6 | **PASS** | 0 WARN or ERROR lines in the 45 service-log lines written from the deploy to the end of the round trip, 0 in the 56 by the end of the visual check; 0 in the shell's stdio log |
| R7 | **PASS** | after the visual check: build still `3174471`, shell home on Ice/Normal, stored theme and display mode equal to before the gate, services and crash reports as in R3 and R5 |

Shell memory on the device (`/proc/<pid>/status` and `smaps_rollup`; 60 s idle
on the launcher, then 10 s after the eleven-app round trip):

| | 691b508 idle | 3174471 idle | 691b508 after apps | 3174471 after apps |
| --- | --- | --- | --- | --- |
| VmRSS (= VmHWM) | 12,544 kB | 12,288 kB | 13,184 kB | 12,928 kB |
| RssAnon | 4,352 kB | 4,352 kB | 4,608 kB | 4,608 kB |
| RssFile | 8,192 kB | 7,936 kB | 8,576 kB | 8,320 kB |
| Pss | 10,084 kB | 9,833 kB | 10,648 kB | 10,441 kB |
| VmSize | 46,476 kB | 46,488 kB | 46,608 kB | 46,620 kB |

Anonymous memory, where the launcher's objects live, is identical to the kB;
resident memory is 256 kB lower, in file-backed pages. The icon change costs
the device no measurable RAM.

**Visual check.** A script on the unit cycled the launcher every 20 s through
Ice & Ember / Normal, Carbon & Signal Orange / Outdoor and Slate & Lavender /
Night (18:20:19 to 18:22:39 UTC, two full cycles and part of a third) and
logged every app open and close it saw, with no app command sent from the
session. The operator's tap opened Wave at 18:22:39 (`open app wave` in
`shell.log`), the cycle stopped on Ice/Normal, and Back returned home at
18:22:53.

| # | Check | Result |
| --- | --- | --- |
| 1 | Icons: eleven Doors icons, Wave's waveform and no speaker symbol, crisp and in place, nothing else moved | **PASS** (operator) |
| 2 | Tint and legibility in Normal, Outdoor and Night | **PASS** (operator) |
| 3 | A tap on the Wave icon opens Wave; Back returns to the launcher | **PASS** (operator; open and return in the unit's log) |

### After the test

- Unit A runs `0.0.9` / `3174471` (this branch, deployed over the `691b508`
  image), Doors shell on the panel, Ice/Normal, bench SSH key installed. No
  flash, and `deploy.sh` does not write the boot partition: `/boot/logo.xrgb`
  is still the `691b508` image's, the vendor splash (SHA-256 `9fd79fee…`). The
  app icons do not touch the splash; a flashed image of this branch carries
  the committed Doors splash (SPLASH: PASS above).
- The shared SDK was put back as found: the target tree in the PocketOS-era
  identity layout (the three links replaced by copies; `/usr/bin/doors`,
  `/etc/doors-release`, `/usr/share/doors` removed), the applied manifest set
  aside as `.pocketos-applied.icons-3174471`, the vendor splash (`9fd79fee…`)
  back in the overlay. `git status` of the SDK differs from before only by that
  set-aside manifest. The target tree's binaries are now this build's.
- Evidence (not in the repository): the session's scratchpad holds the build,
  apply, image and deploy logs, and the before, after, visual and final check
  outputs; the unit keeps `/root/iconsgate/`.

## Validation done without hardware (2026-09-15)

Source `7e81136` (the last code commit; later commits change documentation
only), from fresh WSL clones; master `95b4f58` built and run the same way as
the baseline. VERSION 0.0.9 on both.

| Check | Result |
| --- | --- |
| Packages | the icon extension's 43 committed files byte-identical to the zip; `doors-threshold/` unchanged against master |
| `make all`, host, `-Werror` | rc 0, 0 warnings |
| `make test`, host | rc 0: 3,623 ok, 0 FAIL, 0 warnings (master 3,578; the difference is `app_icons_test`'s 45); the six `NOT RUN` lines are check names, identical to master's |
| Shell tests, SDL simulator | all 19 scripts rc 0: 373 ok, 0 FAIL (master 350), including `launcher_icons_shell_test.sh` 23 ok (67 launcher checks in each of 17 screenshots) with `pocketui_tile_test` 54 checks, and `wave_shell_test.sh`, `system_brand_shell_test.sh`, `shell_ipc_test.sh`, `pos_input_test.sh`, `pos_keyboard_test.sh`, `kbd_shell_test.sh` |
| All eleven icons | each app's own mask found in its tile in every screenshot; no tile without a mask; the ten earlier masks byte-identical to `30e22b0`'s |
| Every app launches | all eleven open through `app start` from the running shell and come home, each logged `open app <id>`, no ERROR; the per-app shell tests pass |
| Keyboard and focus | unchanged: no tile or icon joins a focus group, the group's size is unchanged, the icon is not clickable, taps on icon, label and tile open the app (`pocketui_tile_test`); input, keyboard and keymap tests pass |
| Five themes × three modes | every icon in that pair's `accent_primary` (max error 2, nearest wrong mask 99 away); live switch; Night dimmed |
| Reduced motion | launcher pixel-identical below the status bar |
| Geometry against master | the launcher screenshotted from both builds in all 15 pairs and compared pixel by pixel below the status bar: **0 pixels differ outside the eleven icon cells**; 6,081–6,089 differ inside them per pair, 539 of those in Wave's |
| Deliberate breakages | each fails a test. At `7e81136`: Wave back on its glyph (app_icons 3 FAIL, shell 68). At `e71c5c5`, same mechanism: icon 2 px lower (shell 342, tile test), icon in `text_primary` (shell 218, tile test 30), Radio showing Radar's icon (app_icons 1, shell 51), clickable icon (tile test 4), icon scaled 125 % (shell 510), the brand-mark role (app_icons 1), one mask byte (app_icons 2) |
| riscv64 `make all` (`ENABLE_SX1262=1`) | rc 0, 4 warnings, all in vendor ggwave, as on master |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings, branch and master |
| Device LVGL (SDK sysroot `lv_conf.h`) | `LV_COLOR_DEPTH 16`, `LV_DRAW_SW_SUPPORT_A8 1`, `LV_USE_IMAGE 1`, `LV_CACHE_DEF_SIZE 0` |

Size and memory:

| | master | branch | change |
| --- | --- | --- | --- |
| riscv64 DRM shell, `size` text / data / bss | 902,137 / 9,420 / 22,384 | 914,115 / 9,948 / 22,400 | +11,978 / +528 / +16 |
| riscv64 DRM shell, stripped file (what ships) | 916,088 B | 928,376 B | **+12,288 B** (the same file size as the ten-icon revision: Wave's +1,072 B of text fit the existing page) |
| of which `pos_app_icon_*` (nm) | — | 22 symbols, 11,704 B | 11,264 mask + 11 × 40 descriptor |
| SDL x86-64 shell, text / data | 2,196,441 / 14,616 | 2,208,585 / 15,320 | +12,144 / +704 |
| Heap allocated by `home_create()` (glibc `mallinfo2`, simulator, 3 runs each, identical; measured at `e71c5c5`) | 16,512 B | 14,032 B | **−2,480 B** |
| Heap in use on the launcher after 1.5 s (same run) | 1,495,984 B | 1,489,952 B | **−6,032 B** |

The heap figures come from a measurement patch applied only in the validation
clones, never committed, on the ten-icon revision; they were not measured
again for Wave, whose tile now holds an image where it held a label, the swap
that lowered the heap for the other ten. An `lv_image` pointing at a constant
mask costs less than a label that copies its glyph string, and LVGL draws an
uncompressed A8 variable in place, with the image cache off. The device's own
figure (VmRSS) is taken at the gate.

Screenshots for review (simulator, not the panel): the fifteen launcher
screenshots and `launcher-before-after.png` (master and branch side by side in
Ice/Normal, Carbon/Outdoor, Slate/Night) were kept outside the repository;
`docs/design/brand/shots/launcher-contact.png` and
`launcher-icons-contact.png` are committed and show all eleven icons.
