# Neon: Arcade, the owner's selected direction

**Owner decision, 2026-10-20:** of four rendered Neon directions (Toxic Green,
Infrared, Arcade, Signal) the owner chose **C · Arcade**: "This is approval
of its visual direction." Neon remains the single optional theme
(Settings → Appearance → Neon); Doors remains the default and fallback.
Approval of the implementation on the panel is a separate, later step.

## Palette and roles

Normal-mode base tokens (`docs/design/themes.json`, generated into
`ui/pocketui/pos_theme_table.h`), exactly the approved exploration values:

| Token | Value | Used for |
| --- | --- | --- |
| bg / surface / surface_raised | `#07031a` / `#110a2e` / `#1c1245` | indigo backgrounds and panels |
| line | `#8a5cff` | violet frames |
| text_primary / text_secondary | `#f4f0ff` / `#c9bfff` | values, body text, user content |
| accent_primary | `#39ff6a` | green primary actions, selected chips |
| accent_secondary | `#ff2bd6` | magenta secondary actions, headings, section signs |
| status ok / warn / error | `#39ff6a` / `#ffe000` / `#ff3045` | state, always with words or symbols |
| radio rx / tx | `#00e5ff` / `#ff2bd6` | radio identity |

Role colours in the shared Neon styles (`ui/pocketui/pos_styles.c`):

| Role | Colour |
| --- | --- |
| Clocks (launcher, lock), app titles (`TITLE`), selection outline, selected tile | yellow `#ffe100` |
| Folder, Favorites and Controls titles (`ENV_TITLE`), launcher group headings, section signs | magenta (`accent_secondary`) |
| Menu entries, row titles and chevrons, launcher/Controls labels, environment glyphs | cyan `#4de8ff` |
| Captions, dates, secondary lines (`CAPTION`, `ENV_TEXT_SECONDARY`) | orange `#ff9e1f` |
| Launcher tiles by app family (`pos_style_env_tile_family`) | radio `#ff9e1f`, mesh `#39ff6a`, network `#00c8ff`, tools `#ffe100`, AI `#b066ff`, games `#ff3d6e`, settings `#00ffc8`, files `#ffb000`, apps (and no family) `#ff2bd6` |

Dark labels (`text_on_accent`) stay on every coloured fill; keyboard focus
stays a separate light ring. Outdoor lifts every Neon colour 12 % toward
white (body text 25 %) on black instead of whitening it; Night dims text
22 %, accents 12 % and status 6 %. QR codes keep pure black on white;
photographs, previews, map tiles, card suits and identity colours are
unchanged. No animation, blur or new dependency.

## Production against the approved reference

Same content and state, both rendered by the SDL simulator with the fixed
wall clock, captured by `tools/design/neon_arcade_capture.py`:

| Screen | Approved reference | Production `f398ecb` | Result |
| --- | --- | --- | --- |
| Portrait launcher, Normal, Large, keyboard focus on the first cell | [reference](reference/launcher-arcade.png) | [production](production/launcher-portrait-normal-focus.png) | **pixel-identical** |
| Landscape Settings → Display, Outdoor, Large | [reference](reference/settings-arcade.png) | [production](production/settings-display-landscape-outdoor.png) | 1,656 pixels differ, layout audit identical |

The Settings difference is the dark outer band of the magenta section signs
and magenta buttons only (for example `#5a0f4e` → `#5a1850`): the
exploration derived that band from the Normal magenta, production from the
Outdoor magenta token like every other Outdoor colour. No text, fill or
frame pixel differs.

## Representative screens

Overview sheets: [Normal](sheets/screens-normal.png) (launcher, lock, folder,
Favorites, Controls, Settings, System) · [Outdoor and Night](sheets/screens-outdoor-night.png) ·
[States, RIFT and a warning](sheets/states-rift-warning.png) ·
[Landscape and HDMI](sheets/landscape-hdmi.png). Native frames in
[screens/](screens/).

## States

Selected (yellow outline), keyboard focus (light ring with a dark gap),
pressed (light ring over the unchanged fill), disabled (dimmed, no outline)
and the RIFT state fixture in Normal, Outdoor and Night are in
[screens/](screens/) (`rift-states-*`). OK is green, error red; both carry
words ("OK:", "ERROR:", "not running"). **Warning yellow `#ffe000` is the
same family as the yellow titles**: warnings are identified by their words
and symbols and their size and place, not by colour alone. The palette value
is the approved one and is kept.

## Contrast (resolved tokens, WCAG)

| Mode | All text roles on bg, surface and surface_raised, minimum | Dark label on any coloured fill, minimum | line on bg |
| --- | --- | --- | --- |
| Normal | 4.7 (error) | 5.9 (games tile) | 4.9 |
| Outdoor | 5.2 (error) | 6.8 (games tile) | 6.1 |
| Night | 4.7 (error) | 4.9 (games tile) | 2.6 |

## Focused checks (2026-10-20, SDL simulator unless noted)

| Check | Result |
| --- | --- |
| `theme_test` (every token and contrast of every theme and mode, §4 invariants, Neon Night readability) | 1369 checks, 0 failures |
| Doors frames, 2 rotations × 3 text sizes × 3 modes × 7 screens, against `56aefe5` (`neon_text_checks.py`) | 126 pixel-identical |
| Layout audits, Neon and Doors, same matrix | 252 identical to `56aefe5` (no new clipping or truncation) |
| Live round trips Doors/Ice → Neon (all modes) → back (`neon_live_switch.py`) | 40 exact |
| Live Doors → Neon launcher equals a fresh Neon start, Normal/Outdoor/Night | 3 identical |
| `neon_shell_test.py` (selection, persistence, restart, live change, reopen, switch back; 3 sizes × 2 rotations + HDMI 1920×1080) | 56 checks, 0 failures |
| `settings_app_test` Neon / default | 1563 / 1562 checks, 0 failures |
| `rift_app_test` Neon proof with state modes | 92 checks, 0 failures |
| `system_app_test`, `home_folder_test`, `shell_lock_test`, `g2048_app_test` | 1506 / 169 / 46 / 249 checks, 0 failures |
| `style_lint.sh` | 0 failures |
| K230 cross-build (pinned Xuantie 14.1.1, SDK `22d02c6`) | rc 0, 0 warnings |
