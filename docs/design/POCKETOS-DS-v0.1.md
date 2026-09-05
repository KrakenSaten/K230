# PocketOS Design System v0.1

**STATUS: APPROVED FOR IMPLEMENTATION** — 2026-09-04

Final design handoff. Supersedes `uploads/POCKETUI.md` (v0) and
`POCKETUI-v1.md` (draft). Target: LILYGO T-Display K230, 568 × 1232 portrait
AMOLED, LVGL shell in `ui/pocketui` and `ui/shell`.

Files in this handoff:

| File | Role |
| --- | --- |
| `POCKETOS-DS-v0.1.md` | this document — the contract |
| `themes.json` | machine-readable token tables: 5 themes × 3 modes, derived tokens, contrast audit. Generated from §4–§6 rules; if they ever disagree, this document wins and the JSON is regenerated |
| `PocketOS Design System.dc.html` | reference screens (visual reference only, §12) |
| `PocketOS Visual Directions.dc.html` | exploration history; not normative |

Wording: **MUST** / **MUST NOT** = normative. **SHOULD** = recommendation.
Sections tagged [REFERENCE] are illustrative and never override normative
text.

---

## 1. Canvas and hardware [NORMATIVE]

- Panel 568 × 1232 px, ~330 ppi. Design at 1:1.
- Dark base is the system, not a variant. `bg` MUST be ≤ #0b0a08 in Normal
  and #000000 in Outdoor and Night.
- Large filled bright areas MUST be limited to: back slab, status chips,
  primary button, secondary button, list icon wells, theme-row miniatures.
  Panels, status bar and screen background are outlines on `bg`.

## 2. Visual language [NORMATIVE]

Approved combination of direction 1a (Instrument) and 1b (Slab).

- Structure is drawn with hairlines. Panels: `hairline` border in `line`,
  radius 6, transparent fill, 20 px padding (or `0 20px` for list panels).
  Panel title: mono caption (see §3) in `text_secondary`, positioned in the
  top border, 16 px from the panel's left edge, with 6 px horizontal padding
  and `bg` behind it so it breaks the rule. A panel MAY have no title.
- Tappable elements are slabs: filled `surface` (or accent) rectangles,
  radius 6, no border unless stated.
- One accent hue per screen region. Brightness carries importance.
- `status_error` is used only for error state. Colour never carries meaning
  alone: every status has a label or glyph too.
- No shadows, gradients (except the spectrum grid ruled lines), blur or
  translucency.

## 3. Typography [NORMATIVE]

Families: **IBM Plex Sans** (body) and **IBM Plex Mono** (captions, values,
button labels). Both are bitmap in LVGL; enable exactly the sizes below in
`ui/shell/lv_conf.defaults`.

| Style | Family | Size | Weight | Use |
| --- | --- | --- | --- | --- |
| body | Sans | 16 | 400 | body text, row labels (`text_secondary`), descriptions |
| row-title | Sans | 20 | 400 | notification titles, theme name (600) |
| app-title | Sans | 24 | 600 | header title, crash notice title |
| hero-40 | Sans | 40 | 600 | first-boot heading, letter-spacing −1 % |
| hero-48 | Sans | 48 | 600 | hero numeral (RSSI), letter-spacing −2 %, line-height 1.05 |
| caption | Mono | 14 | 400 | panel titles, status bar cells, labels above values, footers, timestamps. UPPERCASE, letter-spacing 0.1 em (0.08 em in the status bar) |
| button | Mono | 16 | 500 | button labels, UPPERCASE, 0.1 em |
| value | Mono | 20 | 400 | key/value values, unit suffix at 14 `text_secondary` |
| value-24 | Mono | 24 | 400 | countdowns |
| value-32 | Mono | 32 | 400 | secondary hero (SNR) |
| mini | Mono | 10 | 400 | theme-row miniature labels only (§10.5) |

Outdoor mode raises `type_default` 16 → 20 (§6); layouts MUST tolerate the
20 px body without truncating row labels. Timestamps and status-bar text stay
at 14 (caption) in all modes. Body line-height 1.5, titles 1.2.

## 4. Semantic colour tokens [NORMATIVE]

Components MUST reference tokens only. C naming `POS_COLOR_<TOKEN>` (upper
case), resolved via `pos_theme_color(token)`. Theme tables supply the 13
base tokens; the engine derives the rest.

| Token | Use | Value |
| --- | --- | --- |
| `bg` | screen, status bar, caption backing, toast fill | theme |
| `surface` | slabs: back button, OFF/NA chips, secondary button, list icon wells, notification meta block, segmented-control idle segments, theme-row right pane | theme |
| `surface_raised` | meter off-blocks, toggle track (off), dividers between rows inside a panel, spectrum grid lines | theme |
| `line` | panel borders, status-bar cell rules, slider tracks, idle spectrum bars, storage track | theme |
| `text_primary` | titles, values, body | theme |
| `text_secondary` | labels, captions, hints, timestamps, unit suffixes | theme |
| `text_muted` | non-essential hints ("TAP TO PREVIEW"), NA chip | mix(text_secondary, bg, 0.35) |
| `text_on_accent` | text/glyphs on any filled accent, RX, TX or `text_primary` slab | = bg |
| `accent_primary` | active/selected: segment, meter on-blocks, active spectrum bars, primary button fill, back chevron, link-style header action, progress dots | theme |
| `accent_secondary` | second series in charts, secondary chips; not used on the five reference screens except as a swatch | theme |
| `status_ok` | success text and dot | theme |
| `status_warn` | limits, guard caption, limit ticks, "not running" | theme |
| `status_error` | error toast outline + label, crash-loop panel outline + caption | theme |
| `radio_rx` | RX chip fill, RX row glyph/text | theme |
| `radio_tx` | TX chip fill | theme |
| `net_connected` | connected dot beside AP name | = status_ok |
| `focus` | 2 px outline on pressed / focused / selected element | = accent_primary |
| `disabled_fg` | disabled labels, values, glyphs | mix(text_secondary, bg, 0.50) |
| `disabled_bg` | disabled slab fill | = surface |

`mix(a, b, t)`: per-channel linear sRGB interpolation, `t` toward `b`,
rounded to integer.

Invariants the engine MUST check when loading a theme (else fallback, §8):
`radio_rx` ≠ `radio_tx`; `status_ok`, `status_warn`, `status_error` pairwise
different; contrast(`text_primary`, `bg`) ≥ 7; contrast(`text_on_accent`,
each of `accent_primary`, `radio_rx`, `radio_tx`) ≥ 4.5 in Normal mode.

Non-colour theme tokens: `hairline_px` (1 Normal/Night, 2 Outdoor),
`type_default_px` (16 Normal/Night, 20 Outdoor). Radii are constants (§5).

## 5. Theme values — Normal mode [NORMATIVE]

Theme ids are stable strings used in persistence and shown in the Theme
screen. Order below is display order. Derived tokens per theme are in
`themes.json`.

| token | `ice` Ice & Ember | `brass` Brass & Verdigris | `olive` Olive & Chalk | `slate` Slate & Lavender | `carbon` Carbon & Signal Orange |
| --- | --- | --- | --- | --- | --- |
| bg | #06080b | #0b0a08 | #070a08 | #080810 | #050505 |
| surface | #10151b | #16140f | #10160f | #12131f | #121212 |
| surface_raised | #19212a | #201d16 | #18211a | #1b1d2c | #1c1c1c |
| line | #273139 | #33301f | #28352b | #2c2f44 | #2e2e2e |
| text_primary | #e9eef3 | #f2ece0 | #edf2ea | #ecebf5 | #f5f5f5 |
| text_secondary | #8d99a6 | #a0967f | #8fa08f | #9494ad | #9a9a9a |
| accent_primary | #8ccfff | #63c1ad | #e6e2d6 | #b7a6ff | #ff7a1a |
| accent_secondary | #b8c4d0 | #d6b26a | #9bb89a | #6ee7d8 | #9ec5ff |
| status_ok | #57c785 | #84c46c | #5fcf8a | #4ade80 | #3ddc84 |
| status_warn | #e3b341 | #ff8e3c | #e9c46a | #f5b942 | #ffd23f |
| status_error | #e5534b | #e8564f | #e76f51 | #f0625f | #f24d4d |
| radio_rx | #8ccfff | #63c1ad | #9fd4e8 | #6ee7d8 | #9ec5ff |
| radio_tx | #ff9f5a | #d6b26a | #f4a261 | #b7a6ff | #ff7a1a |
| text_muted (derived) | #5e6670 | #6c6555 | #5f6c60 | #636376 | #666666 |
| disabled_fg (derived) | #4a5159 | #565044 | #4b554c | #4e4e5f | #505050 |

Rationale [REFERENCE]: ice — cool neutral, ice-blue active, ember TX.
brass — warm charcoal, verdigris active/RX, brass TX, warning moved to orange.
olive — green-black, chalk-white accent (brightness marks activity), sky RX,
clay TX. slate — blue-slate, lavender active/TX, mint RX. carbon — neutral
greys, signal-orange active/TX, pale-blue RX, warning moved to yellow.

## 6. Display modes [NORMATIVE]

Theme and display mode are independent settings. The engine MUST compute
the 3 mode tables from the Normal table by rule at (theme, mode) change
time and MUST NOT store 15 hand-edited tables. Derived tokens (§4) are
recomputed from the transformed base tokens.

Outdoor:

| token | rule |
| --- | --- |
| bg | #000000 |
| surface | mix(surface, #000000, 0.50) |
| surface_raised | unchanged |
| line | mix(line, text_secondary, 0.45) |
| text_primary | #ffffff |
| text_secondary | mix(text_secondary, #ffffff, 0.40) |
| accent_*, status_*, radio_* | mix(v, #ffffff, 0.15) |
| hairline_px | 2 |
| type_default_px | 20 |

Night:

| token | rule |
| --- | --- |
| bg | #000000 |
| surface | mix(surface, #000000, 0.50) |
| surface_raised | mix(surface_raised, #000000, 0.40) |
| line | mix(line, #000000, 0.35) |
| text_primary, text_secondary, accent_*, status_*, radio_* | mix(mix(v, #000000, 0.50), #ffb060, 0.15) |
| hairline_px | 1 |
| type_default_px | 16 |

Worked example, `ice` (matches reference 3f and `themes.json`):

| token | Normal | Outdoor | Night |
| --- | --- | --- | --- |
| bg | #06080b | #000000 | #000000 |
| surface | #10151b | #080b0e | #080b0e |
| surface_raised | #19212a | #19212a | #0f1419 |
| line | #273139 | #55606a | #192025 |
| text_primary | #e9eef3 | #ffffff | #8a8076 |
| text_secondary | #8d99a6 | #bbc2ca | #635c55 |
| accent_primary | #8ccfff | #9dd6ff | #62737b |
| accent_secondary | #b8c4d0 | #c3cdd7 | #746e67 |
| status_ok | #57c785 | #70cf97 | #4c6f47 |
| status_warn | #e3b341 | #e7be5e | #87672a |
| status_error | #e5534b | #e96d66 | #883e2f |
| radio_rx | #8ccfff | #9dd6ff | #62737b |
| radio_tx | #ff9f5a | #ffad73 | #935e35 |

Mode switching uses the same live path as theme switching (§8). Layouts do
not change between modes except the `type_default` and `hairline` tokens.

## 7. Spacing, radii, borders [NORMATIVE]

| Constant | Value |
| --- | --- |
| status bar height | 56 |
| app header height | 72 |
| screen horizontal padding | 20 |
| body top padding | 24 (first-boot: 40) |
| gap between panels | 22 (Theme screen rows: 12) |
| panel padding | 20; list panels `0 20` |
| list row height | 64 (Brightness/Storage rows with inline meters: 72) |
| notification item | min 80, padding 18 vertical |
| back slab | 72 × 56 |
| primary / secondary button | full width × 64 |
| paired buttons inside a panel | 56, gap 8 |
| status chip | 36 tall, 12 horizontal padding, 8 gap glyph→label |
| inline row action (e.g. "Check now") | 44 tall, 16 horizontal padding; the whole 64 px row is the hit area |
| toggle | 64 × 36, 4 px inset, 28 px square knob |
| segmented control | 56 tall, 4 px gap between segments |
| meter block gap | 4 |
| radius: panels, slabs, buttons, chips, toggle | 6 |
| radius: meter blocks, swatches, toggle knob (4), glyph squares (3) | 2 / 4 / 3 |
| hairline | `hairline_px` (1 / 2 outdoor) |
| focus / selected outline | 2 px `focus` |
| error outline (toast, crash panel) | 1.5 px `status_error` |
| toast outline (neutral) | 1.5 px `text_primary` |
| minimum touch target | 64 × 64 (see caveat C1) |

## 8. Theme engine behaviour [NORMATIVE]

- **Selection = preview.** Tapping a theme row repaints the entire shell in
  that theme immediately and marks the row ACTIVE. No confirm step; the last
  tapped theme is the setting. Back keeps it.
- **Persistence.** `theme=<id>` and `display_mode=normal|outdoor|night` are
  written to the settings store on each change and read before the first
  frame at shell start.
- **Fallback.** Unknown id, missing key, unparsable value, or a table failing
  the §4 invariants → use `ice` + `normal`, log a warning, leave the stored
  value untouched until the user changes it. Appearance MUST never prevent
  boot.
- **Live switch, no restart.** Token lookups go through `pos_theme_color()`
  at style-apply time. The engine keeps one shared LVGL style per token role;
  on switch it rewrites the properties and calls
  `lv_obj_report_style_change(NULL)`. Apps using the shared styles inherit
  automatically. Apps MUST NOT cache colour values; if unavoidable they
  subscribe to `POS_EVENT_THEME_CHANGED` and re-read. Icons are recoloured
  (`currentColor` semantics) so they follow.
- **Transition.** Theme and mode changes are instant (no cross-fade).
- **Apps** receive tokens only; an app MUST NOT define its own colour
  constants except through a token alias declared in its manifest.

## 9. Components and states [NORMATIVE]

Colours are tokens; geometry from §7; type from §3.

**Status bar** — 56 px on `bg`, bottom `hairline` in `line`. Four cells left
→ right, separated by `hairline` rules in `line`, all caption style
(0.08 em): (1) wordmark "PocketOS" weight 500, padding 0 20; (2) hint cell,
`text_secondary`, flex 1, padding 0 16 — shows AP dot (`net_connected`, 8 px)
+ SSID when connected, else context text ("3 unread", "First boot"); (3)
radio cell, padding 0 10, containing the radio chip; (4) clock, padding 0 20.
Radio chip states: RX → `radio_rx` fill, `text_on_accent` glyph+label; TX →
`radio_tx` fill; OFF → `surface` fill, `text_secondary` glyph+label; NA
(service absent) → `surface` fill, `text_muted` dashed-ring glyph, label "—".

**App header** — 72 px, bottom `hairline`. Back slab 72 × 56 `surface`,
radius 6, chevron glyph in `accent_primary`. Title app-title style. Optional
right caption in `text_secondary` (context) or `accent_primary` (action,
e.g. "Clear all"). First-boot has no header; the wizard body starts under
the status bar.

**Panel** — §2. Titled variants carry the caption; the caption colour is
`text_secondary`, or `status_warn` for guard panels, or `status_error` for
error panels (with a 1.5 px `status_error` border).

**Key/value row** — 64 px, label body `text_secondary` left, value style
right, `surface_raised` 1 px divider between rows, none after the last.
Values truncate with ellipsis. Value colour may be `status_warn` for
warning states ("not running").

**Toggle** — off: `surface_raised` track, `text_secondary` knob left. On:
`accent_primary` track, `text_on_accent` knob right.

**Segmented control** — segments `surface` + `text_secondary` caption; the
selected segment `accent_primary` + `text_on_accent`, weight 500.

**Segmented meter** — 10 blocks (7 + 3 in the TX-power guard), 12 px tall
(20 in guard). On: `accent_primary` (or `text_primary` for non-radio
quantities such as brightness). Off: `surface_raised`. Over-limit blocks:
1.5 px `status_warn` outline, transparent fill.

**Spectrum** — 96 px tall, 24 bars, 4 px gap, ruled every 24 px in
`surface_raised`, baseline 1 px `line`. Idle bars `line`, bars above 60 %
`accent_primary`. Axis captions below in `text_secondary`.

**Track slider** — 2 px `line` track, filled portion `text_primary`, limit
tick 2 × 14 px `status_warn`.

**Primary button** — `accent_primary` fill, `text_on_accent` button label.
Pressed: 2 px `focus` outline plus fill mixed 15 % toward `text_on_accent`.
Disabled: `disabled_bg` fill, `disabled_fg` label.

**Secondary button** — `surface` fill, `hairline` `line` border,
`text_primary` label. Pressed: `surface_raised` + 2 px `focus` outline.

**Emphasis button** (crash notice "Restart now") — `text_primary` fill,
`text_on_accent` label.

**Inline action** — 44 px, 1.5 px `accent_primary` outline, `accent_primary`
button label.

**Notification item** — 44 px `surface` icon well, radius 6, glyph in
`accent_primary` (unread) or `text_secondary` (read); title row-title,
timestamp caption, body text `text_secondary`. Read items render the title
in `text_secondary`.

**Toast** — 64 px, `bg` fill, 1.5 px outline (`text_primary` neutral,
`status_error` error), 20 px inset from screen edges, 24 px from the
bottom (104 px when a primary button occupies the bottom), value/caption
text, action word right in `accent_primary`. Auto-dismiss 4 s; tap
dismisses.

**Empty state** — dashed `hairline` panel in `line`, centred 80 px `surface`
disc with a 1.5 px stroked glyph in `text_secondary`, caption below.

**Progress dots** (wizard) — 3 blocks 6 px tall, gap 4: done/current
`accent_primary`, upcoming `surface_raised`.

**Global pressed rule** — every tappable element shows the 2 px `focus`
outline while pressed, plus the fill change stated above. Focus (hardware
keys, future) reuses the same outline.

## 10. Screen specifications [NORMATIVE]

All screens: status bar, then (except 10.4) header, then body with §7
spacing. Content strings shown are sample data.

### 10.1 Settings
Header "Settings". Panels: **Network** (Wi-Fi with connected dot; Mesh;
Hotspot toggle), **Display** (Mode segmented control Normal/Outdoor/Night in
a 20/0/16 padded block with divider; Brightness 10-block meter in
`text_primary` + value), **Appearance** (row "Theme": three 10 × 20 px
swatch bars — `accent_primary`, `radio_tx`, `surface` with `line` border —
then theme name, then `accent_primary` chevron; navigates to 10.5),
**System** (Storage 2 px track + value; Battery; Firmware; Update with
inline action "Check now"), **About** (Device; Serial). Body scrolls if
needed.

### 10.2 Radio detail
Header "Radio", right caption "EU868 · LONG RANGE". Panels: **Link** (RSSI
hero-48 with 20 px unit; SNR value-32 right-aligned; 10-block meter;
spectrum; axis captions; RX row: 12 px top padding, `surface_raised` top
divider, RX glyph + "RX" in `radio_rx`, then details in `text_secondary`),
**Profile** (Frequency, Bandwidth, Spreading factor, Coding rate),
**Region guard · EU868** (caption in `status_warn`; TX power value with
" · limit 14" suffix in `status_warn`; 7 + 3 guard meter; Duty cycle value;
track slider with limit tick), validation line (ok dot, "VALID" in
`status_ok`, description), primary button "Apply profile" pinned to the
bottom. The reference shows an error toast above the button.

### 10.3 Notifications
Status bar hint "3 unread", radio OFF. Header "Notifications", right action
"Clear all". Error panel titled "POS-SUPERVISE · CRASH LOOP": title
app-title + timestamp; body text; meta block (`surface`, caption "NEXT
ATTEMPT" + value-24 countdown); paired buttons "Restart now" (emphasis) and
"View log" (secondary), 56 px. Panel **Today** with notification items.
Neutral toast at the bottom in the reference.

### 10.4 First boot / no service
Status bar hint "First boot", radio NA, clock "--:--" acceptable. No header.
Body top padding 40, gap 28: progress dots (2 of 3 done); caption "STEP 2
OF 3 · RADIO CHECK"; hero-40 title "Radio service not available"; body
text; empty-state panel 240 px "NO SIGNAL DATA"; panel **Service** (Unit;
State in `status_warn`; Last exit); bottom: primary "Retry" and secondary
"Continue without radio", gap 8.

### 10.5 Settings → Appearance → Theme
Header "Theme", right caption "SETTINGS · APPEARANCE". Body gap 12. Five
theme rows in §5 order, each 176 px, **drawn entirely in its own theme's
Normal-mode tokens** (not the current theme), so the list is a live
comparison:

- Left pane 232 px on that theme's `bg`, right `line` rule. Contents:
  28 px mini status bar (wordmark; RX and TX chips 16 px tall) with bottom
  rule; 12/10 padded column: mini panel (RSSI caption + 14 px value, 10-block
  meter 5 px tall), legend "● OK ● WARN ● ERR" in the three status colours,
  bottom-pinned 20 px `accent_primary` slab labelled "APPLY". Mini text uses
  the `mini` style (§3).
- Right pane on that theme's `surface`, padding 16/18: theme name
  (row-title 600), theme id (caption, not uppercase), 11-swatch strip (bg,
  surface, line, text_primary, accent_primary, accent_secondary, status_ok,
  status_warn, status_error, radio_rx, radio_tx; 14 px tall, gap 4, `line`
  border), footer: "● ACTIVE" in `accent_primary` when selected, else
  "TAP TO PREVIEW" in `text_muted`.
- Selected row: 2 px `focus` outline. Others: `hairline` `line` border.
- Bottom-pinned footer captions: "DISPLAY MODE · <mode>" left
  (`text_secondary`), "APPEARANCE › DISPLAY MODE" right (`accent_primary`,
  navigates to the Display mode setting). Display mode is not changed here.

## 11. Icons [NORMATIVE]

- Stroke icons, 1.5 px, geometric, square ends, no fills except the 4 px
  centre dot of the radio glyph. Drawn in the container's current colour.
- Sizes: 14 px inside chips and captions; 24 px grid for app icons; 28–32 px
  inside empty-state discs.
- Glyph set used on the reference screens: radio (ring + centre dot), radio
  NA (dashed ring), radio off (ring + diagonal), square (generic item),
  chevron left (back), chevron right (navigate), status dot (8 px filled).
- Back chevron in the reference is a text glyph; implement as an icon at
  24 px in `accent_primary`.

## 12. Motion [NORMATIVE]

| Event | Motion | Timing |
| --- | --- | --- |
| LoRa TX | 1.5 px ring on the radio chip glyph expands 8 px and fades | 300 ms, linear |
| LoRa RX | same ring contracts into the glyph | 300 ms, linear |
| Connecting | radio chip fill alternates `surface` ↔ `surface_raised` | 1.5 s period, stepped |
| Error | 2 px horizontal shake of the toast/panel | 200 ms, once |
| App open / close | fade + 8 px slide | 150 ms, ease-out, no bounce |
| Press | outline appears | instant |
| Theme / mode change | none | instant |
| Meter / value change | none (stepped) | instant |

Motion never blocks input. Reduced-motion setting → all rows become instant
state changes.

## 13. Accessibility and contrast [NORMATIVE]

Targets against `bg` (WCAG 2 ratios), evaluated by the engine at build or
test time from `themes.json`:

- Normal and Outdoor: `text_primary` ≥ 7; `text_secondary` ≥ 4.5; every
  accent/status/radio colour ≥ 4.5; `text_on_accent` on filled slabs ≥ 4.5.
  All five themes pass (Normal minima: text 16.8, text_secondary 6.7,
  status_error 5.4, accent 7.8, on-accent 7.8; Outdoor minima: text 21,
  text_secondary 11.6, all others ≥ 6.8).
- Night: intentionally below AA for dark adaptation; `text_primary` ≥ 5,
  `text_secondary` ≥ 3, status/radio ≥ 2.8 (lowest: `status_error` 2.8–3.1,
  always paired with an outline and an "ERROR" label). `text_muted` ≈ 1.9
  is permitted only for non-essential hints.
- Status differentiation: ok is green, warn is amber/orange/yellow, error is
  red, RX and TX are two distinct hues neither of which is green or red, in
  every theme and mode.
- Outdoor: no hairline under 2 px, no information in `text_muted`, body
  type 20 px.
- Touch: all controls ≥ 64 px in their primary dimension; row hit areas span
  the full row.

Per-token, per-mode numbers: `themes.json → themes.<id>.contrast`.

## 14. Caveats and open questions

- **C1 — touch target 72 → 64.** v0 stated a 72 px minimum. The approved
  screens use 64 px rows and buttons (and 56 px paired buttons inside
  panels). This document codifies 64. Confirm with field testing in gloves;
  if 72 is required, row height changes, layout otherwise identical.
- **C2 — mini font.** Theme-row miniatures are drawn at 9 px in the
  reference; implement with the 10 px mono size (§3) or omit the mini labels
  if 10 px is unavailable. The miniature is illustrative; its geometry may
  be approximated with primitives.
- **C3 — Night `status_error`.** 2.8–3.1 contrast. If night-time error
  visibility proves insufficient, raise the Night dim factor for `status_*`
  from 0.50 to 0.35 (a one-line rule change; regenerate `themes.json`).
- **C4 — Outdoor 20 px body.** The reference screens are drawn at 16 px
  body in all modes; Outdoor's 20 px default has not been laid out. Long
  row labels ("Spreading factor", "Continue without radio") must be checked.
- **C5 — Fonts.** IBM Plex Sans/Mono replace Montserrat; licence (OFL) and
  flash budget for 11 bitmap sizes to be confirmed.
- **C6 — `accent_secondary`** is defined and themed but unused on the five
  reference screens; keep it for two-series charts, do not invent uses.
- **C7 — Launcher tiles** (v0 component) were not re-drawn in the 1a + 1b
  language. Apply §2: hairline panel, `accent_primary` 24 px icon top-left,
  row-title label bottom-left, pressed = `surface_raised` + focus outline.
  Tile size 150 px, 2 columns, 20 px gutter unchanged.
- **C8 — Text input / keyboard, dialog** remain undesigned (v0 "missing"
  list); out of scope for v0.1.

## 15. Reference material [REFERENCE]

`PocketOS Design System.dc.html`: sections 3a–3e = one theme each with
Settings, Radio detail, Notifications, First boot, Theme screen; 3f = `ice`
in Normal / Outdoor / Night using §6 rules. Every colour in the file is a
CSS variable bound to a token; the columns differ only in token values. The
DC's `--text2` is `text_secondary`, `--raised` is `surface_raised`, `--acc`
is `accent_primary`, `--inv` is `text_on_accent`.

Known deviations from normative text (intentional; text wins): back chevron
is a text character (§11); mini labels 9 px (C2); body stays 16 px in the
Outdoor column (C4).

## 16. Implementation checklist

1. Add IBM Plex Sans (16, 20, 24, 40, 48) and IBM Plex Mono (10, 14, 16,
   20, 24, 32) to `lv_conf.defaults`; remove Montserrat sizes no longer used.
2. Implement `pos_theme.c`: base token struct, `derive()` (§4), `outdoor()`
   and `night()` (§6), invariant check, `pos_theme_color()`.
3. Load the five Normal tables from `themes.json` (or a generated C table)
   and unit-test the derived/mode outputs against the JSON.
4. Shared LVGL styles per token role; `lv_obj_report_style_change` on
   switch; `POS_EVENT_THEME_CHANGED`.
5. Persistence: `theme`, `display_mode` keys; read before first frame;
   fallback `ice` + `normal` on any failure.
6. Update components to §9: panel (hairline, radius 6, caption-in-rule),
   slab back button, status bar with four cells and chip states, key/value
   row 64, toggle, segmented control, segmented meter, spectrum, track
   slider, primary/secondary/emphasis/inline buttons, notification item,
   toast, empty state, progress dots, pressed outline.
7. Build screens 10.1–10.5; Theme rows rendered in their own theme's tokens.
8. Motion table §12 with reduced-motion switch.
9. Simulator screenshots of all five themes × Radio detail, plus `ice` ×
   three modes, compared against 3a–3f.
10. Contrast test over `themes.json` in CI per §13 thresholds.
11. Resolve caveats C1–C5 with hardware in hand.

---

PocketOS Design System v0.1 — **STATUS: APPROVED FOR IMPLEMENTATION**
