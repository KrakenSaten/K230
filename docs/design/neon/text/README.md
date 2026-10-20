# Neon text roles after the unit B review

**Owner feedback, 2026-10-20, after extensive testing of `56aefe5` on unit B:**
the overall appearance looks good, but too much of the interface is still
white: the clock, framing lines and borders in Outdoor, titles of opened
folders and Favorites, and the text of menus, System and Settings. The earlier
request for neutral body text does not mean white clocks, headings, menu
labels or settings labels.

Visual approval of this refinement is **pending**; PR #98 stays draft.

## What changed (Neon only)

All through the shared role styles (`fill_neon_styles`, `pos_styles.c`) and
the theme resolver (`pos_theme.c`); every other theme keeps its exact pixels.

| Owner-reported element | Role | Before | Neon now |
| --- | --- | --- | --- |
| Launcher clock, lock-screen clock | `ENV_CLOCK`, `ENV_CLOCK_LARGE` | text primary (white) | electric cyan, `accent_primary` |
| App header titles (Settings, System, …) | `TITLE` | white | electric cyan |
| Opened folder and Favorites titles, Controls title | `ENV_TITLE` | white | hot magenta, `accent_secondary` |
| Menu entries, Settings rows and their chevrons, launcher and Controls labels, back chevrons | `ROW_TITLE`, `ENV_TEXT`, `ENV_TEXT_SMALL` | white | Neon label cyan: `accent_primary` mixed 30 % toward `text_primary` |
| System/Settings labels and captions, launcher date, lock hints | `CAPTION`, `ENV_TEXT_SECONDARY` | grey | deeper cyan: `accent_primary` mixed 22 % toward `bg` |
| Outdoor framing (panel borders, status capsule, field edges) | `line` token, Outdoor | `#7c8898` grey | `#2da6b2` teal-cyan: Neon's normal line mixed 55 % toward its Outdoor cyan |

Unchanged: values and readouts (`VALUE`), body and paragraph text
(`TEXT_PRIMARY`/`SECONDARY`/`MUTED`), user content, lime selection, light
keyboard-focus rings, dark labels on cyan and magenta fills, cyan section
signs, warning/error/OK colours, QR codes and quiet zones (black/white),
photographs, maps, camera images, card suits. Layout, fonts, sizes, touch
targets and navigation are untouched.

The Settings row chevron took the font-only `SYMBOL` role and so inherited
the screen's white; it now carries the row's own `ROW_TITLE` with the symbol
font on top (`settings_app.c`). In every other theme `ROW_TITLE` is the same
`text_primary` it inherited, and its frames are pixel-identical.

### Contrast (resolved tokens, WCAG ratio)

| Mode | Label cyan on bg / surface / raised | Deeper cyan on bg / surface / raised | Magenta heading on bg / surface | Outdoor line on bg |
| --- | --- | --- | --- | --- |
| Normal | `#4af7ff` 15.6 / 14.8 / 13.3 | `#01c0c9` 9.2 / 8.7 / 7.8 | 6.0 / 5.7 | |
| Outdoor | `#67f9ff` 16.6 / 15.9 / 13.7 | `#1ec1c7` 9.5 / 9.1 / 7.9 | 6.5 / 6.3 | 7.2 (was 5.8) |
| Night | `#3ad2d9` 11.4 / 10.9 / 10.3 | `#00a8af` 7.2 / 6.9 / 6.5 | 4.9 / 4.7 | |

Every one of these roles sits on a dark surface or the scrimmed launcher
photograph. A static scan of every label given one of them, whose parent is
styled with a filled role, found only dark slabs and glass panels with their
pressed variants (which in Neon keep the dark fill and add a light ring).

## Rendered evidence

Actual LVGL/SDL simulator captures; before is `56aefe5`, after is this
change, same fixture, fixed wall clock (`tools/design/neon_capture_clock.c`),
no post-processing. [`pairs/`](pairs/) holds 28 native-pixel before/after
pairs, portrait and landscape, Large text:

| Screen | Normal | Outdoor | Night |
| --- | --- | --- | --- |
| Launcher clock | [portrait](pairs/launcher-portrait-normal.png) · [landscape](pairs/launcher-landscape-normal.png) | [portrait](pairs/launcher-portrait-outdoor.png) · [landscape](pairs/launcher-landscape-outdoor.png) | [portrait](pairs/launcher-portrait-night.png) · [landscape](pairs/launcher-landscape-night.png) |
| Opened folder | [portrait](pairs/folder-portrait-normal.png) · [landscape](pairs/folder-landscape-normal.png) | [portrait](pairs/folder-portrait-outdoor.png) · [landscape](pairs/folder-landscape-outdoor.png) | |
| Favorites picker | [portrait](pairs/favorites-portrait-normal.png) · [landscape](pairs/favorites-landscape-normal.png) | | |
| Settings | [portrait](pairs/settings-portrait-normal.png) · [landscape](pairs/settings-landscape-normal.png) | [portrait](pairs/settings-portrait-outdoor.png) · [landscape](pairs/settings-landscape-outdoor.png) | [portrait](pairs/settings-portrait-night.png) · [landscape](pairs/settings-landscape-night.png) |
| System | [portrait](pairs/system-portrait-normal.png) · [landscape](pairs/system-landscape-normal.png) | [portrait](pairs/system-portrait-outdoor.png) · [landscape](pairs/system-landscape-outdoor.png) | |
| Controls | [portrait](pairs/controls-portrait-normal.png) · [landscape](pairs/controls-landscape-normal.png) | [portrait](pairs/controls-portrait-outdoor.png) · [landscape](pairs/controls-landscape-outdoor.png) | |
| Lock clock | [portrait](pairs/lock-portrait-normal.png) · [landscape](pairs/lock-landscape-normal.png) | | |

## Focused checks

[checks.txt](checks.txt) records the results. Reproduction (SDL simulator per
`docs/BUILD_ENVIRONMENT.md`; build the before and after shells separately):

```sh
gcc -shared -fPIC tools/design/neon_capture_clock.c -ldl -o out/neon-capture-clock.so
LD_PRELOAD="$PWD/out/neon-capture-clock.so" python3 tools/design/neon_text_captures.py BEFORE_SHELL out/text/before neon doors
LD_PRELOAD="$PWD/out/neon-capture-clock.so" python3 tools/design/neon_text_captures.py AFTER_SHELL out/text/after neon doors
python3 tools/design/neon_text_checks.py out/text/before out/text/after docs/design/neon/text/pairs
LD_PRELOAD="$PWD/out/neon-capture-clock.so" python3 tools/design/neon_live_switch.py AFTER_SHELL out/text/live
```

`neon_text_captures.py` covers Neon and Doors × portrait/landscape × Small,
Medium and Large × Normal, Outdoor and Night × seven screens (252 frames per
build) and stores each frame's `shell.audit` layout findings.
