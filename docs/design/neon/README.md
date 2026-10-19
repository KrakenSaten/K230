# Optional system-wide Neon theme

Owner-approved direction, implemented in PR #98. Select **Settings →
Appearance → Neon**; tapping any other theme switches back immediately.
Doors is still the default and fallback. The seventh row uses the existing
scrollable Appearance page, including its two-column landscape list.

## Palette and behaviour

| Semantic role | Normal colour | Purpose |
| --- | --- | --- |
| `bg` | `#050810` | Very dark canvas |
| `surface` / `surface_raised` | `#0d1420` / `#192336` | Controls and pressed/selected surfaces |
| `line` | `#4c627d` | Quiet structure without losing boundaries |
| `text_primary` / `text_secondary` | `#f1f5fa` / `#b0bfd0` | Neutral readable text |
| `accent_primary`, `focus`, `radio_rx` | `#00e5ff` | Cyan actions, selection, focus, RX |
| `accent_secondary`, `radio_tx` | `#ff59df` | Magenta secondary controls and TX |
| `status_ok` | `#b6ff36` | Lime positive states |
| `status_warn` | `#ffdf38` | Yellow warnings |
| `status_error` | `#ff626c` | Red errors |

The existing derivations produce muted text, dark text on bright fills,
disabled colours and connected state. Outdoor and Night use the existing
transforms, including Night's deliberately reduced luminance/contrast. There
is no new animation, blur, glow or dependency.

Normal-mode contrast: body/background **18.3:1**, secondary/background
**10.7:1**, muted/background **7.0:1**, cyan fill text **13.0:1**, magenta fill
text **7.4:1**, focus/raised surface **10.2:1**, warning/raised surface
**11.9:1**, error/raised surface **5.4:1**, disabled text/background **3.1:1**.
Disabled controls remain quiet and noninteractive; labels, icons, selection
words, underlines and focus brackets continue to carry meaning alongside colour.

## Integration

- `themes.json` and generated `pos_theme_table.h` register `neon` after all
  existing themes. `pos_theme_select` and shared styles perform live updates.
- Settings lists it through the registry. Its existing shell entry point
  persists `theme=neon` and the display mode atomically through the settings
  store and emits the normal `shell.theme` notification. Startup reads that
  selection before the first frame. No extra preference or framework exists.
- Shared styles cover app chrome, buttons, fields, focus/selection, dialogs,
  notifications and keyboard states. Neon secondary controls use magenta
  text/borders while keeping shared state precedence and geometry.
- The environment's previously fixed package palette has a Neon-only semantic
  mapping for Launcher, lock, Controls and status capsules: opaque dark panels,
  cyan borders/glyphs/sliders and a darker backdrop. Portal illustrations keep
  their symbols and original artwork. Every other theme uses the old path.
- Audited Settings, RIFT Activity/graph/widgets, Notes, Calculator and Clock:
  their shared chrome and UI already consume semantic styles/tokens. No app
  runtime recolouring was needed. Suit palettes, map tiles, photos, camera
  previews, detection overlays, identity hues and diagnostic palettes are
  unchanged. The global style lint passes.
- No new listener or screen callback is introduced. Existing style refresh,
  RIFT invalidation and destroy-time callback removal remain authoritative.
  Tests switch live while screens are open, destroy/reopen them, and switch
  again after destruction.

The [original RIFT concept](../rift/concepts/NEON-ACTIVITY.md) and its SVG
remain historical inspiration. This implementation changes neither RIFT's
Activity geometry nor routing. The [design-system amendment](../POCKETOS-DS-v0.1.md#58-optional-system-wide-neon-theme-accepted)
records the owner-approved exception to the fixed environment palette.

## Rendered evidence

All images here are actual LVGL/SDL simulator snapshots. Contact sheets only
resize and label those renders. Full-resolution source captures are in
[`shots/`](shots/). RIFT uses existing synthetic fixtures; no radio service,
network helper, radio operation or device was started for these captures.

| Evidence | Coverage |
| --- | --- |
| [Portrait matrix](portrait-matrix.png) | Launcher, Settings, Appearance and Activity at Small, Medium, Large; 568×1232 |
| [Landscape matrix](landscape-matrix.png) | Same screens and text sizes; 1232×568 |
| [HDMI](hdmi.png) | Launcher, Appearance, Controls and synthetic Activity; 1920×1080, Large |
| [States](states.png) | Keyboard focus, Controls, disabled/error states, lock and service warning |
| [Existing-theme comparison](existing-theme-comparison.png) | Doors and Ice launcher, portrait and landscape, before/after |

Visual inspection: neutral reading text remains legible; active chips, focus
brackets and underlines remain visible. Long RIFT values/names retain their
existing ellipsis; its Activity body scrolls to content below the viewport.
The seventh Appearance row makes the landscape page scroll; mode controls
remain reachable through normal page scrolling (tested). No layout, touch
size, navigation or text-size rule was changed to fit the palette.

## Validation — 2026-10-19, cloud host

- Host simulator built successfully with pinned LVGL
  `59dc7e436ae97a25e32656739ea6a943f9f11b6a`.
- `theme_test`: **1,364 checks, 0 failures**, all seven themes/modes, derived
  colours, contrast, fallback and Neon live selection/pressed/disabled states.
- `settings_test`: **0 failures**, including Neon persistence and switching
  back to Doors through reloads.
- `settings_app_test`: **1,555 checks, 0 failures**, real pointer-driven
  Settings selection, reopen, selection words, all text sizes/orientations.
- `settings_shell_test.sh`: **0 failures**, including the app suite above,
  lint and actual shell startup without netd or unintended writes.
- `neon_shell_test.py`: **56 checks, 0 failures**, seven display/text cases:
  UI selection, saved preference, actual process restart, switch back through
  Settings, live changes under lock, keyboard focus and reopened screens.
- Bounded `rift_app_test` theme proof: **73 checks, 0 failures**; synthetic
  Activity, all six orientation/text combinations plus HDMI, live round trips,
  unclipped button labels and callback safety after destruction.
- `g2048_theme_test`: **67 checks, 0 failures** across the expanded registry.
- Style and Settings lints: **0 failures**. `git diff --check`: clean.
- Doors and Ice Launcher/Settings, both orientations: **8 comparisons, zero
  changed pixels below y=180**, at full resolution. The top 180 px are excluded
  because they contain the live clock. Baseline runtime is PR parent
  `cf7ffc8`/current master runtime `0f7c587`; only the original shared styles
  were substituted for the baseline build, since registration alone does not
  change existing-theme rendering. The comparison sheet shows full renders.

Host development headers/tools were unpacked under scratch storage
(`work/`), using the host's existing SDL2 runtime. No system package, renderer,
SDK, toolchain pin or vendor source was changed.

Reproduction with the repository's normal simulator prerequisites:

```sh
cmake -S ui/shell -B out/neon-build -DPOCKETOS_DISPLAY=sdl
cmake --build out/neon-build --target pocketos-shell settings_app_test rift_app_test -j6
make CC=gcc tests/theme_test tests/settings_test tests/g2048_theme_test
./tests/theme_test
./tests/settings_test
./tests/g2048_theme_test
SHELL_BIN="$PWD/out/neon-build/pocketos-shell" bash tests/settings_shell_test.sh
python3 tests/neon_shell_test.py out/neon-build/pocketos-shell out/neon-shots
mkdir -p out/neon-rift
SDL_VIDEODRIVER=dummy RIFT_THEME=neon RIFT_THEME_PROOF=1 \
  RIFT_SHOTS_DIR="$PWD/out/neon-rift" out/neon-build/rift_app_test
bash tests/style_lint.sh
```

## Remaining checks

The pinned Xuantie toolchain and K230 SDK are absent in this cloud checkout,
so no cross-build was attempted. No hardware or deployment was accessed.
Target DRM compilation, AMOLED/HDMI output, panel colour/luminance and input
behaviour on hardware remain unverified. Simulator layout and colour evidence
is not a claim about target rendering cost. Owner visual review is pending;
PR #98 remains draft and unmerged.
