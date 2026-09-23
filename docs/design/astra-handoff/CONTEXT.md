# PocketOS → "Doors": current visual state (handoff for Astra)

State as of 2026-09-13, branch `feature/post-v0.0.9-foundations` (1f73d88).
This describes what exists today. **Nothing has been renamed and there are no
Doors assets yet.** Evidence tags: **[HW]** verified on the device (unit A),
**[SRC]** read from source or config, **[DS]** stated in Design System v0.1
(`POCKETOS-DS-v0.1.md`, approved) but not measured.

## 1. Hardware and display

| Fact | Value |
| --- | --- |
| Device | LILYGO T-Display K230 (Kendryte K230 SoC), handheld |
| Panel | RM69A10 AMOLED, MIPI-DSI, 24 bpp link [SRC] |
| Resolution | **568 × 1232 px, portrait**, DRM mode on DSI-1, rotation 0 [HW] |
| Aspect | 568:1232 ≈ 1 : 2.17 |
| Physical size | 65 × 145 mm in the device tree [SRC] (≈ 220 ppi from those numbers; DS §1 says "~330 ppi"; the two disagree and neither was measured) |
| Rendering | LVGL 9 on DRM, **16-bit RGB565** (`LV_COLOR_DEPTH 16` in the vendor LVGL package the shell links) [SRC] |
| Input | Capacitive touch, **touch-first**: minimum target 64 × 64 px [DS]. On-screen touch keyboard owned by the shell |
| Physical keyboard | Optional keyboard base board (TCA8418 matrix). Driver runs in the shell and typing is verified [HW]. It only types into text fields, it cannot navigate the launcher |
| Brightness | Backlight 0–255; the UI allows 10–100 %. The 10 % floor is validated, and marginal in Night mode [HW] |

Display constraints that matter visually:
- Dark UI is the system, not a variant. `bg` must be ≤ #0b0a08 in Normal and
  #000000 in Outdoor and Night [DS]. On AMOLED, black pixels are unlit.
- RGB565 makes the darkest steps collapse: `bg`/`surface`/`surface_raised`
  are only a few 5–6-bit steps apart, and Night `surface` vs `bg` is one step.
  Hairlines stay visible, but a fill-only distinction may vanish (open
  hardware item H1). Avoid gradients and subtle dark tints.
- No shadows, gradients, blur or translucency. Large bright filled areas are
  limited to buttons, chips, keys and slabs [DS §1–2].

### U-Boot boot logo

| Fact | Value |
| --- | --- |
| File | `/logo.xrgb` on the ext4 boot partition (partition 1 of the SD card) |
| Format | Raw, headerless, **XRGB8888**, 4 bytes/pixel. The 4th byte is 0xFF on every pixel. Byte order of the colour channels is not independently checked |
| Dimensions | **568 × 1232**, native portrait. No rotation is applied |
| Size | **2,799,104 bytes** (= 568 × 1232 × 4). U-Boot skips the logo if the size differs by even one byte [SRC] |
| Load | `ext4load mmc ${mmc_boot_dev_num}:1 0x1f000000 /logo.xrgb`, then fallbacks `mmc 1:1` and `mmc 0:1` [SRC]. Boot log `RM69A10 direct XRGB8888 logo.xrgb full-screen OSD4` [HW] |
| Coverage | **Fills the whole display** (OSD4 layer, 568 × 1232 at the origin) [SRC] |
| Current content | Vendor LILYGO logo on black. The artwork sits inside x 127–440, y 294–899 (314 × 606 px); everything else is pure black |
| Origin | Vendor prebuilt binary; no source image or conversion script in the vendor tree. PocketOS ships it unchanged |

## 2. Current UI structure

- **Screen stack:** a 56 px status bar, then the launcher, or one open app. A
  full-panel system alert (alarms) can cover either. One app at a time, no
  multitasking, no swipe gestures.
- **Status bar (56 px, `bg`, hairline bottom rule):** left, the `POCKETOS`
  wordmark as plain mono 14 caption text. Next, an app hint slot (e.g.
  `OFFICER · TURN 13`). Then the radio chip (glyph + `RX`/`TX`/`OFF`/`--`,
  filled with `radio_rx`/`radio_tx` or `surface`). Right, the clock (HH:MM,
  UTC today).
- **Launcher:** 2-column grid, **10 tiles** in 5 rows, 20 px outer padding
  and gaps. Each tile is 254 × 150 px, `surface` fill, radius 6, no border,
  12 px inner padding. There is about 326 px of empty space below the last row.
- **Tile anatomy:** glyph top-left (32 px symbol font, `accent_primary`). The
  app name is bottom-left (Sans 20, `text_primary`). **The name is always
  shown.**
- **App screen:** a 72 px header with a back slab (72 × 56, `surface`,
  chevron in `accent_primary`) and the app title (Sans 24 semibold). The body
  below has 20 px side padding, 24 px top padding and scrolls vertically.
  Back returns to the launcher.
- **States:**
  - Pressed: fill turns `surface_raised` plus a 2 px `focus` outline.
  - Selected: 2 px `focus` outline, fill kept.
  - Active choice: accent fill with `text_on_accent`, e.g. `ON`, `NORMAL`, `SELECTED`.
  - Disabled: `disabled_bg` fill with `disabled_fg` text.
  - Keyboard focus exists only on text fields and dialog buttons.
  - **Tile icons do not change on press or selection**; only the slab does.
- **Typography:** IBM Plex, converted to 4-bpp bitmaps, Latin-1.
  - Sans: 16 body, 20 row title, 24 semibold app title, 40/48 semibold hero.
  - Mono: 14 UPPERCASE captions with tracking, 16 medium button labels, 20/24/32 values.
  - Outdoor mode raises body text 16 → 20.
- **Shapes:**
  - Radius 6 on panels, slabs, buttons and chips.
  - Panels are 1 px hairline outlines (2 px in Outdoor) with no fill.
  - Buttons are full width × 64 px. Chips are 36 px tall.
  - A mono caption sits above each panel's content (`WI-FI`, `DISPLAY`, `TARGET`).
- **Character:** dark, instrument-like, functional and minimal. Moderately
  dense, with hairline structure and uppercase mono labels. There is no
  decoration, imagery or brand art anywhere except the Timber game's art.
- **Motion:** theme and mode changes are instant, and app open/close has no
  transition in the current code. A reduced-motion setting exists.

**Icons today and constraints for new ones**
- The current glyphs are LVGL's built-in symbol font, a FontAwesome-derived
  subset. It is a stopgap: the code marks them temporary, "until DS icons".
  Two apps share a glyph (Radio and Radar both use the Wi-Fi symbol).
- DS §11 target style:
  - 1.5 px stroke, geometric, square ends, no fills.
  - 24 px grid for app icons, 14 px inside chips and captions.
  - Recoloured by the theme, so icons are single-colour (`currentColor`).
- Icons must read as **one colour on dark**, tinted by a token, across
  5 themes × 3 modes. The hardest case is Night (dim, amber-shifted, low
  contrast; see `theme-alt.png`).
- Launcher icon area today is about a 32 px glyph cell in a 230 × 126 px
  content box, with the name label taking the bottom ~26 px.

## 3. Themes and colour system

Theme and display mode are independent settings, both chosen in Settings →
Appearance and applied live. Components use semantic tokens only; **apps may
not define their own colours**. The one exception is Timber's pre-rendered
art.

**Themes** (id: name): `ice`: Ice & Ember (default and fallback), `brass`:
Brass & Verdigris, `olive`: Olive & Chalk, `slate`: Slate & Lavender,
`carbon`: Carbon & Signal Orange.

**Display modes:** Normal, Outdoor, Night. Each is computed from the Normal
table by rule:
- **Outdoor:** `bg` #000; text pushed to white; accents mixed 15 % toward
  white; hairlines 2 px; body text 20 px.
- **Night:** `bg` #000; every text, accent and status colour halved toward
  black, then mixed 15 % toward #ffb060 amber; body text 16 px.

Normal-mode base tokens:

| Token | ice | brass | olive | slate | carbon |
| --- | --- | --- | --- | --- | --- |
| bg | #06080b | #0b0a08 | #070a08 | #080810 | #050505 |
| surface (slabs) | #10151b | #16140f | #10160f | #12131f | #121212 |
| surface_raised | #19212a | #201d16 | #18211a | #1b1d2c | #1c1c1c |
| line (hairlines) | #273139 | #33301f | #28352b | #2c2f44 | #2e2e2e |
| text_primary | #e9eef3 | #f2ece0 | #edf2ea | #ecebf5 | #f5f5f5 |
| text_secondary | #8d99a6 | #a0967f | #8fa08f | #9494ad | #9a9a9a |
| accent_primary | #8ccfff | #63c1ad | #e6e2d6 | #b7a6ff | #ff7a1a |
| accent_secondary | #b8c4d0 | #d6b26a | #9bb89a | #6ee7d8 | #9ec5ff |
| status_ok | #57c785 | #84c46c | #5fcf8a | #4ade80 | #3ddc84 |
| status_warn | #e3b341 | #ff8e3c | #e9c46a | #f5b942 | #ffd23f |
| status_error | #e5534b | #e8564f | #e76f51 | #f0625f | #f24d4d |
| radio_rx | #8ccfff | #63c1ad | #9fd4e8 | #6ee7d8 | #9ec5ff |
| radio_tx | #ff9f5a | #d6b26a | #f4a261 | #b7a6ff | #ff7a1a |

Derived tokens:
- `focus` = `accent_primary`
- `text_on_accent` = `bg`
- `text_muted` = `text_secondary` mixed 35 % toward `bg`
- `disabled_fg` = `text_secondary` mixed 50 % toward `bg`
- `disabled_bg` = `surface`
- `net_connected` = `status_ok`

All 15 theme × mode tables are in `docs/design/themes.json`.

**Contrast rules [DS §13]:**
- Normal and Outdoor: `text_primary` ≥ 7 : 1 on `bg`; `text_secondary`,
  every accent/status/radio colour, and `text_on_accent` on filled accents
  ≥ 4.5 : 1.
- Night is intentionally below AA for dark adaptation: text ≥ 5, secondary ≥ 3.
- Semantics: ok is green, warn amber/yellow, error red. RX and TX are two
  distinct hues, neither green nor red. `status_error` is reserved for errors.
  Colour never carries meaning alone: there is always a label or shape too.

## 4. Launcher apps (actual order, left→right, top→bottom)

| # | Name | Purpose | Current glyph |
| --- | --- | --- | --- |
| 1 | Radio | LoRa radio status (state, frequency, TX power, last RX, airtime), send test packet | Wi-Fi arcs |
| 2 | System | Model, kernel, storage, interfaces, services; Restart / Power off | Gear |
| 3 | Fleet | Naval grid game (see §5) | Navigation arrow |
| 4 | Radar | Sensor-scope game (see §5) | Wi-Fi arcs (same as Radio) |
| 5 | Timber | Block-tower balancing game (see §5) | Grid list |
| 6 | Notes | Plain-text notes list and editor | Document |
| 7 | Clock | Clock, alarms, countdown | Bell |
| 8 | Calendar | Month view (Monday first), Today, selected day; no events | Three bars |
| 9 | Calculator | Four-function calculator | Plus |
| 10 | Settings | Wi-Fi, display brightness, Appearance (theme + mode) | Pencil |

Launcher labels drop the "Pocket" prefix used in the docs (PocketFleet, PocketNotes, …).

## 5. Game identities

- **Fleet:** single-player Battleship against the engine.
  - Layout: 10 × 10 grid (A–J, 1–10), a `TARGET` panel, a full-width `FIRE`
    button and a `YOUR WATERS` mini-map.
  - Motif: instrument-grid cells in token colours only. Your hits are
    `radio_tx` (orange in Ice), enemy hits `radio_rx` (blue). A solid square
    marks a hit, a cross a sunk ship, a dot a miss. Faint sweep lines cross
    the grid.
  - Difficulty levels are named RECRUIT, OFFICER, COMMANDER and ADMIRAL, and
    the status-bar hint shows the level and turn.
  - **Preserve:** the cell language (every state has a shape, not just a
    colour) and the naval-command wording.
- **Radar:** tactical sensor-scope game that models no real RF.
  - Layout: a 520 × 520 circular scope with range rings, N/E/S/W labels, a
    rotating sweep wedge and ring-outlined contacts.
  - HUD: SCORE/STREAK/LEVEL and a five-block SECTOR INTEGRITY bar above the
    scope; a TARGET card (bearing/range/lock) and an `ENGAGE` button below.
  - **Preserve:** the scope face and sweep.
- **Timber:** tabletop block-tower game. Pull blocks, restack them, and don't
  topple the tower.
  - The **only app with raster art**: pre-rendered 2:1 dimetric wooden
    blocks on green baize felt, made in Blender.
  - HUD: SCORE/LAYERS and a dotted STABILITY meter in standard DS panels,
    with a drag track and TEST/PULL buttons below the table.
  - **Preserve:** the warm wood-and-felt table look, which deliberately
    contrasts with the dark instrument UI.

## 6. Screenshots (`docs/design/astra-handoff/`)

All seven are **simulator/host renders**: the SDL build of this branch's
exact `ui/` and `apps/` trees, captured with LVGL snapshot at 568 × 1232,
rendered at 16-bit colour like the device. **None is a unit A capture**
(device-side screenshots are not supported). AMOLED true black and the
panel's dark-step behaviour are not represented. Demo data was staged for
realism: a mock radio backend (RX chip), a fake Wi-Fi network "Workshop",
a fake backlight at 80 %, three sample notes, and Fleet's debug battle
position.

| File | Shows | Theme / mode |
| --- | --- | --- |
| `launcher.png` | Home, status bar, 10 tiles | Ice & Ember / Normal |
| `settings.png` | Wi-Fi (connected), Brightness, Appearance, Display mode | Ice & Ember / Normal |
| `calendar.png` | Month view, today selected | Ice & Ember / Normal |
| `notes.png` | Notes list, primary button | Ice & Ember / Normal |
| `calculator.png` | Keypad and display | Ice & Ember / Normal |
| `fleet.png` | Fleet battle screen | Ice & Ember / Normal |
| `theme-alt.png` | Launcher in Night mode (icon/wordmark worst case) | Ice & Ember / Night |

## 7. Boot and branding situation

- Project name today: **PocketOS**. Planned alternative name: **Doors**.
  **No rename has been implemented; no Doors assets exist.**
- Boot sequence as seen on the panel: the U-Boot splash (vendor
  **LILYGO logo**, `logo.xrgb`, loaded before Linux), then the PocketOS
  launcher. Autoboot countdown at ~6.5 s, kernel at ~8 s.
- PocketOS branding is otherwise functional and minimal. There is no logo,
  symbol, icon family or splash of its own. The name appears as text only:
  - the `POCKETOS` status-bar caption;
  - the System app's `PocketOS <version>` row;
  - the "Restart PocketOS?" / "Power off PocketOS?" dialogs.
- Likely future design deliverables (not designed here):
  - primary Doors logo and a compact symbol;
  - a U-Boot boot splash (must be 568 × 1232 raw XRGB8888, exactly
    2,799,104 bytes, portrait, full-screen, no alpha);
  - an app icon family meeting the §2 icon constraints;
  - small-screen UI branding guidance (status-bar wordmark, themes and
    modes, AMOLED dark base).

## FILES TO ATTACH TO ASTRA

1. `docs/design/astra-handoff/CONTEXT.md` (this file)
2. `docs/design/astra-handoff/launcher.png`
3. `docs/design/astra-handoff/settings.png`
4. `docs/design/astra-handoff/calendar.png`
5. `docs/design/astra-handoff/notes.png`
6. `docs/design/astra-handoff/calculator.png`
7. `docs/design/astra-handoff/fleet.png`
8. `docs/design/astra-handoff/theme-alt.png`

Optional, only if Astra needs exact values: `docs/design/themes.json` (all
15 token tables and the contrast audit, ~680 lines).
