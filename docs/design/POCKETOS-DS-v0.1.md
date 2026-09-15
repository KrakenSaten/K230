# PocketOS Design System v0.1

**STATUS: APPROVED FOR IMPLEMENTATION** — 2026-09-04

**Amendment A (§17) — text input, keyboard and dialogs — approved
2026-09-10.** It closes caveat C8 and is normative on the same terms as the
rest of this document. It is an amendment, not a separate deviation
document: §1, §14 and §16 are updated in place to point at it, and nothing
is renumbered.

**Amendment B (§18) — system alerts — approved 2026-09-11.** The
shell-owned, full-panel alert that interrupts whatever is on screen, first
used by PocketClock's alarms and countdown. Normative on the same terms;
nothing is renumbered.

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
  primary button, secondary button, list icon wells, theme-row miniatures,
  and — added by Amendment A (§17.3) — the touch keyboard's Done key and an
  engaged Shift. Panels, status bar and screen background are outlines on
  `bg`.

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
(0.08 em): (1) wordmark "PocketOS" weight 500, padding 0 20 (the product name
is Doors since ADR-005: the wordmark text is "Doors", everything else in this
cell is unchanged); (2) hint cell,
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
- The Doors brand mark is not an icon and these rules do not cover it: §19.

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
- **C8 — Text input / keyboard, dialog.** **CLOSED 2026-09-10 by Amendment
  A (§17)**, which specifies the text field, the focus model, the touch
  keyboard with its approved deviation DEV-1, and the dialog. Was: undesigned
  (v0 "missing" list), out of scope for v0.1.
- **C9 — Norwegian and other Latin-1 letters.** **CLOSED 2026-09-10 by the
  product owner.** For v0.0.8: æ, ø and å MUST be reachable from the symbol
  layer (§17.3); they MUST NOT be added to the primary QWERTY alpha layout;
  the alpha keys MUST NOT shrink below the DEV-1 geometry to make room; and
  long-press character or accent popups stay out of scope (§17.7). The
  fonts already carry `0xA0–0xFF`, so no glyph work follows from this. A
  future layout that wants them on the alpha layer needs a new deviation:
  twelve columns would mean about 43 px keys, which DEV-1 does not cover.

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
12. Amendment A (§17): logical key layer and focus group, text field, touch
    keyboard, dialog. Approved for implementation 2026-09-10.
13. Amendment B (§18): the shell-owned system alert. Approved for
    implementation 2026-09-11.

---

## 17. Amendment A — text input, keyboard and dialogs [NORMATIVE]

**Approved 2026-09-10.** Closes C8, which left text input, the keyboard and
dialogs undesigned and out of scope for v0.1. Everything here is normative
and uses the same MUST / MUST NOT wording as the rest of the document.
Nothing in §1–§16 is renumbered; where this amendment extends an earlier
section it says so.

Scope: the primitives PocketNotes needs, and nothing more. §17.7 lists what
is deliberately still out of scope.

### 17.1 Text field

The single text-entry primitive, built on LVGL's text area. No custom text
engine.

**Geometry.** Single-line 64 tall, matching the list row and button height
of §7. Multi-line: minimum 3 body lines plus padding, growing into the space
its parent gives it. Both: full width of the parent, radius 6, 16 horizontal
padding, single-line text vertically centred.

**States.** Colours are §4 tokens; no literals.

| State | Fill | Border | Text |
| --- | --- | --- | --- |
| Normal | `surface` | `hairline` in `line` | `text_primary` |
| Focused | `surface` | `hairline` in `line`, plus the 2 px `focus` outline of §9 | `text_primary` |
| Disabled | `disabled_bg` | none | `disabled_fg`, no caret |
| Error | `surface` | 1.5 px `status_error` | `text_primary` |

The focused treatment is the global focus outline of §9 and MUST NOT be a
second, keyboard-only visual. The error state is optional per field; a field
with no invalid condition MUST NOT show it. When shown it MUST carry a
caption below the field in `status_error`, because §2 forbids colour from
carrying meaning alone.

**Placeholder.** `text_secondary`, body style, shown only while the field is
empty. It MUST NOT use `text_muted`: Outdoor forbids information in
`text_muted` (§13), and a placeholder says what the field is for. It
disappears on the first character, not on focus.

**Caret.** 2 px wide, `accent_primary`, full line height, blinking 500 ms on
/ 500 ms off. Under reduced motion (§12) it is solid and does not blink.
Drawn only in the focused state.

**Scrolling.** The field MUST keep the caret visible. Single-line scrolls
horizontally and never wraps. Multi-line wraps at the field width, scrolls
vertically, and never scrolls horizontally. A field's scrolling MUST NOT
propagate to the screen body: a body that overflows steals taps and drags
that roll more than 10 px, which is the defect PocketTimber's table screen
found on hardware and fixed by fitting exactly.

**Selection.** Whatever LVGL already provides, and nothing else. v0.0.8 adds
no selection gestures, no handles and no clipboard.

**Typography.** Body (16; 20 in Outdoor per §6). Screens hosting fields MUST
tolerate the 20 px body without truncation, as §3 already requires.

### 17.2 Focus

One focus model for every input source.

- The shell MUST own exactly one focus group. Every focusable element joins
  it: text fields, buttons, dialog actions.
- Exactly one element is focused at a time.
- The focus ring is the 2 px `focus` outline of §9. §9 already reserves that
  outline for "hardware keys, future"; this makes it concrete and adds no
  new treatment.
- A tap focuses the element tapped. A logical Next / Prev key moves focus in
  the order elements joined the group. Touch and keys MUST converge here:
  there MUST NOT be one focus notion for touch and another for keys.
- Opening the keyboard MUST NOT change focus. The focused field is the sink
  for the key stream; the keyboard is only a source.

### 17.3 Touch keyboard

A shell-owned sheet docked to the bottom of the screen, 568 wide. One
instance serves every app.

**Geometry.** 6 outer padding, so each row is 556 wide. Keys 52 × 64, gap 4
between keys, 8 between rows. Wide keys (Shift, Backspace, `?123`, Enter /
Done) are 80. Space fills the remainder of its row. Four rows plus padding
give a sheet **296 tall**, under a `hairline` top rule in `line`. On the
1232 panel that leaves 808 for the editor above it (1232 − 56 status bar −
72 header − 296).

| Row | Contents | Arithmetic |
| --- | --- | --- |
| 1 | 10 keys | 10×52 + 9×4 = 556 |
| 2 | 9 keys, centred | 9×52 + 8×4 = 500, inset 28 each side |
| 3 | Shift, 7 keys, Backspace | 80 + 7×52 + 80 + 8×4 = 556 |
| 4 | `?123`, Space, Enter / Done | 80 + 388 + 80 + 2×4 = 556 |

**Layers.** Two, and only two. **Alpha**: QWERTY, with Shift for capitals.
**Symbols** (`?123`): digits 1–0 on row 1, common punctuation and **æ ø å**
on rows 2–3, `ABC` to return.

The three Norwegian letters live on the symbol layer by the owner's ruling
of 2026-09-10 closing C9. They MUST NOT be added to the alpha layer, and the
alpha keys MUST NOT shrink below the DEV-1 geometry to make room for them: a
twelve-column row would mean about 43 px keys, which DEV-1 does not cover.
Shift applies to them as it does to any letter.

**Keys.**

- **Shift** has three states. Tap → next-character; tap again within the
  double-tap window → locked; tap again → off. Off is a normal key face.
  Next-character is `accent_primary` fill with a `text_on_accent` glyph.
  Locked is that fill plus a 2 px `text_on_accent` underline, so the two are
  distinguishable without relying on colour.
- **Backspace** MUST always be present and MUST never be disabled, including
  on an empty field, where it is simply inert. It repeats: after a 400 ms
  hold, every 60 ms. The repeat applies under reduced motion too — it is
  function, not decoration.
- **Space** inserts one space. No double-space-to-period.
- **Enter / Done** is the right key of row 4 and depends on the field. On a
  single-line field it is **Done**: it commits and dismisses the keyboard,
  and carries `accent_primary` fill with a `text_on_accent` label. On a
  multi-line field it is **Enter**, inserts a newline, and is drawn as a
  normal key; those screens dismiss from the app header instead (§17.6).
- No hide key. A single-line field has Done, a multi-line editor has the
  header action, and a third route to dismissal would be a third thing to
  explain.

**States.** Key face `surface`, label `text_primary`. Letter keys use the
row-title style (Sans 20); word keys (SHIFT, SPACE, DONE, `?123`, `ABC`) use
the button style (Mono 16, 500, uppercase, 0.1 em). Pressed follows the
global rule of §9 — the 2 px `focus` outline plus a fill change to
`surface_raised`. A disabled key is `disabled_bg` with a `disabled_fg`
label.

**Extension to §1.** §1 limits large filled bright areas to a named list.
That list gains exactly two items: the keyboard's Done key and an engaged
Shift. Both are at most 80 × 64, comparable to a status chip. Nothing else
on the keyboard is filled.

**DEV-1 — keyboard key width (approved deviation, 2026-09-10)**

§7 and §13 require a 64 px minimum touch target. Ten QWERTY columns cannot
meet it in 568 px: 64 px keys would need 676. Keys are therefore **52 wide
× 64 tall** — the minimum is met in height and missed in width only.

Approved on the reasoning that carried PocketFleet's deviation D1, and on
these conditions, all of which MUST hold:

- a mis-key is immediately visible in the field and immediately correctable;
- **Backspace is always available** and never disabled;
- no irreversible action is ever bound to a reduced-size key — every
  irreversible action stays a full-size control: a 64 px button, or a dialog
  action under §17.5;
- the deviation covers **keyboard keys only**. It MUST NOT be cited to
  shrink any other control. The global 64 px rule of §7 and §13 is otherwise
  unchanged.

This is the first DS-level deviation, distinct from app-level deviations
such as PocketFleet D1, which bind only their own app.

### 17.4 One logical input path

Normative, and the reason this amendment exists before any code.

- There MUST be exactly one logical key stream into the focused element. The
  touch keyboard of §17.3, the simulator's SDL keyboard and a future
  physical keyboard are **sources** feeding it, not alternative paths.
- Key identity is LVGL's: a printable character is its Unicode code point,
  everything else is an `LV_KEY_*` constant. No parallel vocabulary.
- That promise is about the **logical** stream. LVGL's device layer is not
  the same: `lv_indev_data_t.key` carries a printable character as its UTF-8
  *bytes* packed into the word, which agrees with the code point for ASCII
  and diverges above U+007F. Converting between the two is the input layer's
  job, at its boundary, so that nothing above it sees the packing. Anything
  that skips the layer and writes an LVGL key directly will be wrong for
  every character C9 put on the symbol layer.
- Apps MUST NOT bind to a touch-keyboard-specific API, MUST NOT read the
  keyboard widget, and MUST NOT branch on where a key came from. An app that
  behaves differently depending on the source is in breach of this section.
- The shell owns the keyboard, the group and the stream. An app sees a
  focused field and the characters that arrive in it.

The consequence is deliberate: adding the physical keyboard becomes a driver
pushing into an existing stream, not a second input design.

### 17.5 Dialogs

This codifies the pattern already shipped and validated on unit A in the
System app's restart and power-off confirmations (v0.0.7). It is not new.

A dialog asks about something the owner has just done. Something that
happens **to** them and cannot wait is a system alert (§18), which is a
different surface with different rules.

**Structure.** A panel (§9) holding a title in app-title style, body text in
`text_secondary` wrapped to the panel width with 12 above and 20 below, and
one row of exactly two buttons: 56 tall, gap 8, each taking half the width —
the paired-buttons geometry of §7.

**Cancel is first**, on the left, and is the safe action.

**Emphasis.** The accented button is the safe one whenever confirming is
irreversible or expensive to undo; otherwise the confirm carries the accent.
The other takes the secondary treatment. The shipped precedent is normative:
a restart undoes itself in about thirty-five seconds and the accent sits on
Restart; a power-off costs a walk to the bench and the accent sits on
Cancel. **A destructive delete follows the power-off case — Cancel is
accented.**

**No action before confirm.** Nothing is deleted, sent or destroyed when the
dialog opens. The action happens on the confirm press and at no other
moment. Dismissal by any other route MUST be equivalent to Cancel.

**Interaction with text entry.** When a dialog opens over a field:

1. the keyboard is dismissed;
2. focus moves into the dialog and lands on **Cancel**;
3. on Cancel, focus returns to the field it came from, and the keyboard is
   restored only if it was open;
4. on a destructive confirm, focus does not return — the field is gone.

### 17.6 PocketNotes implications

The primitives above are normative; how Notes arranges them is the app's.
This is the design guidance the MVP needs and no more.

- **Note list.** Rows at 64 (§7), full-row hit area. Title is the note's
  first line in row-title style, truncated with an ellipsis; timestamp in
  caption style, `text_secondary`. `surface_raised` dividers between rows,
  none after the last (§9).
- **Empty state.** The §9 empty state exactly: dashed `hairline` panel in
  `line`, centred 80 px `surface` disc with a 1.5 px stroked glyph in
  `text_secondary`, caption below.
- **Editor.** One multi-line text field (§17.1) filling the body above the
  keyboard. No formatting controls.
- **Keyboard presentation.** The editor opens with the field focused and the
  keyboard shown. **Done** is the app header's right action — §9 already
  allows a right caption in `accent_primary` for an action — and it
  dismisses the keyboard and saves. Leaving the app saves too: the v0.1
  lifecycle has no pause, so an app wanting continuity persists on change
  (ADR-002).
- **Delete.** A dialog under §17.5, destructive, Cancel accented.

### 17.7 Out of scope

Not designed here and not to be inferred from anything above: accented and
non-ASCII characters beyond what the two layers carry — which as of C9's
closure means æ ø å on the symbol layer and nothing further — a third
symbol page, word prediction, autocorrect, key preview popups, text
selection gestures, cut / copy / paste, undo and redo inside a field,
right-to-left text, CJK or any IME, landscape layout, haptics, a physical
keyboard driver, and — in Notes — search, folders, tags, formatting and
sync.

---

## 18. Amendment B — system alerts [NORMATIVE]

**Approved 2026-09-11.** Codifies the shell-level alert PocketClock's alarms
and countdown now use, so that the next thing with something urgent to say
uses it too rather than inventing a second one. Normative on the same terms
as the rest of this document. Nothing in §1–§17 is renumbered.

### 18.1 What a system alert is

A **system alert** is a single full-panel surface, owned by the shell, that
appears over whatever is on screen when a time-sensitive event happens.

- The shell MUST own it. It is built once, hidden, on the screen rather than
  inside any app — the same ownership as the touch keyboard (§17.4) — and is
  never rebuilt.
- An app or service MUST NOT build its own full-panel alert, and MUST NOT
  duplicate one the shell already shows. It raises an event; the shell draws
  it.
- It covers the whole panel, including the status bar, and it MUST swallow
  taps that land on it so nothing reaches what is underneath.
- It is neither a dialog (§17.5) nor a status-bar hint (§9). A dialog asks
  about something the owner just did; an alert reports something that
  happened to them.

### 18.2 When one may be raised

System alerts are for events that are **time-sensitive or high-priority**:
the owner needs to know now, and knowing later is worth less or worth
nothing.

- Anything that can wait for the owner to open the app MUST NOT raise one.
  It belongs in the app, or in the status-bar hint cell (§9).
- Anything that is merely a result, a completion or a piece of news MUST NOT
  raise one either. Interrupting is the cost of an alert, and it is only
  paid where the alternative is the owner missing the moment.

### 18.3 Content

An alert MUST say, in words, **what is alerting and why**.

- A title in hero-40 (§3) naming the event — what is happening, not the
  component that noticed it.
- A line below it identifying **which** one: the specific alarm, the
  duration that ran out, the condition that tripped. An alert that only
  names its category leaves the owner guessing which of several it was.
- Where the device cannot deliver the alert as expected, the alert MUST say
  so in caption style rather than let the owner infer it from silence.
- Colour MUST NOT carry any of this alone (§2). The words carry the meaning
  and the colours only reinforce it.
- As long as PocketOS has one source of alerts, the event name is enough to
  identify the source. The first alert from a **second** source MUST make
  the source unambiguous in the title.

### 18.4 Actions

- Exactly **one dominant acknowledgement action**. It carries the accent, it
  is the widest control in the row, and it is the last one — under the
  thumb. It is what the owner reached for the device to do.
- **At most one secondary action**, and only where it means something for
  that event. Snooze on an alarm means something; there is nothing to snooze
  on a countdown that has already finished, and the alert MUST NOT show a
  control that does nothing. It takes the secondary treatment (§9).
- Every action is a full 64 × 64 touch target (§7). DEV-1 relaxed that for
  keyboard keys only; an alert MUST NOT borrow it.
- **Nothing destructive or irreversible on the alert.** An alert is answered
  in a hurry, sometimes half awake. An action that deletes, sends or cannot
  be undone MUST go through a confirmation under §17.5 with its emphasis
  rules, and MUST NOT be reachable in one tap from the alert.

### 18.5 Interrupting, and coming back

- The keyboard MUST be dismissed when the alert takes the panel, as a dialog
  dismisses it (§17.5). Unlike a dialog it is **not** restored on
  acknowledgement: the alert is not a step in the task the owner was in the
  middle of, and putting the sheet back would imply it was.
- The app underneath MUST be left exactly as it was. An alert MUST NOT
  close, pause, destroy or navigate away from it, and MUST NOT change what
  it has on screen, in its fields or in its storage.
- Acknowledging MUST return to the interrupted context and nothing else: the
  alert goes away and reveals precisely what it covered. It MUST NOT
  navigate anywhere, not to the launcher and not to the app that raised it.
- Anything the alert changed that must survive a power cut MUST be written
  at the moment of acknowledgement, not left for the app to notice later.
- A view that was already on screen showing state the alert has changed MUST
  notice and redraw. The owner has just watched something happen; a list
  still showing the old answer is worse than one that was never open.

### 18.6 One at a time

- **One active system alert.** v0.0.8 has no stacking, no queue and no
  priority order between alerts, and MUST NOT gain one by accident: a second
  raiser while an alert is showing waits, and is shown only once the first
  is acknowledged.
- **Repeated polling MUST NOT produce duplicate alerts.** Whatever raises an
  alert is stepped on a tick, and every one of those ticks sees the same
  condition. The raiser MUST signal the **transition** into and out of the
  alerting state, never the state itself, and showing an alert that is
  already shown MUST do nothing.
- Acknowledgement MUST be final for that occurrence. The same condition,
  still true on the next tick, MUST NOT bring the alert back.

### 18.7 Motion and accessibility

- An alert MUST NOT depend on motion to be noticed. It has to work at a
  glance, standing still, for someone who has turned motion off.
- Any motion it does use MUST honour reduced motion (§12) and apply its end
  state immediately when reduced motion is on.
- Contrast, type and touch targets are §13, §3 and §7 as everywhere else. An
  alert has no licence to shrink a control because it is urgent.

### 18.8 v0.0.8 limits, and the first implementation

**First implementation:** `ui/shell/shell_alarm.c`, raised by the clock
runtime (`apps/clock/clock_runtime.c`) for PocketClock's alarms and
countdown. The pattern above is the contract; PocketClock is one client of
it and has no alert of its own.

Limits of this version, to be designed rather than inferred:

- **The alert was touch-only, and is not any more.** Its actions did not
  join the one focus group of §17.2 — a named exception to that section,
  taken because the sheet lives for the whole life of the shell and a hidden
  control in the group would be key-reachable from every screen in PocketOS.
  The consequence was that keys still reached whatever is behind the alert,
  which this section made a gate before a physical keyboard ships.

  **RESOLVED 2026-09-12**, commit `2afe7fe`. The requirement is met, and by a
  **different mechanism than the one prescribed above**, which is recorded
  here rather than quietly substituted. The actions still never join the one
  group: the §17.2 exception stands, for the reason it was taken. Instead the
  alert keeps a **private** focus group holding its own actions, and the key
  stream is redirected into that group for exactly as long as the alert is
  shown. Redirecting rather than filtering is deliberate — filtering would
  mean deciding, key by key, what an app may still see, and one wrong answer
  is a leak; a group that is not delivered to cannot receive anything.

  What the gate asked for is satisfied: focus starts on the dominant action
  (§18.4), Snooze joins and leaves the private group with its visibility so a
  timer alert cannot focus a control that is not on screen, acknowledgement
  restores the app's previous focus, and the touch keyboard is suppressed for
  as long as the alert holds the panel and is **not** restored afterwards
  (§18.5). Verified on hardware: `docs/hardware/KEYBOARD_BRINGUP_2026-09-10.md`
  §5.4.
- No stacking, queue or priority (§18.6), no alert history, no per-alert
  sound or haptics, no auto-dismiss after a timeout, and no alerts raised
  from outside the shell process.

## 19. Amendment C — the Doors brand mark [NORMATIVE]

**Accepted 2026-09-15** by the product owner, after the unit A hardware gate
(`docs/hardware/DOORS_GRAPHICS_GATE.md`). Brings the compact mark from the
Doors graphics package (ADR-005, `docs/design/brand/README.md`) into the UI in
one place. Normative on the same terms as the rest of this document. Nothing in
§1–§18 is renumbered.

### 19.1 What it is

- The **Doors mark** is the Threshold compact mark: one rectilinear
  silhouette, 20 × 24, 4 px stems, a 6 px threshold gap at the lower left.
  It is a brand mark, not an icon: §11's stroke rule does not apply, and it
  MUST NOT be redrawn as a stroke icon or used as an app icon.
- Its only source is the package's `brand/doors-mark.png`, converted without
  scaling into an A8 mask (`ui/pocketui/pos_brand_mark.c`, generated by
  `tools/design/gen_brand_mark.py`).
- It lights 264 px. It is not a large filled bright area under §1.

### 19.2 Colour

- It MUST be drawn in `accent_primary`, through the `POS_STYLE_BRAND_MARK`
  role, and never in a colour of its own. It follows theme and display-mode
  changes like text does (§8), Night dimming included.
- It carries no meaning of its own: the name "Doors" is always beside it
  (§2).

### 19.3 Size and place

- One size: 20 × 24, drawn 1:1.
- One place: the System app's identity row, before the name "Doors", with the
  §7 glyph-to-label gap of 8 px, centred in the 64 px row and starting at the
  panel's content edge. The row keeps the key/value row's geometry (§9); only
  the name moves right, by 28 px.
- Not in the status bar (the wordmark stays the `DOORS` caption, §9), the
  launcher or an app header. Any other placement, the 10 × 12 small mark, the
  primary lockup and the app icons each need an amendment of their own.
- Clear space: at least one stem width (4 px) on every side.

### 19.4 Motion

- None. The mark is static in every state, so reduced motion (§12) changes
  nothing about it.

### 19.5 Outside this document

- The boot splash U-Boot shows before Linux
  (`platforms/k230/rootfs_overlay/logo.xrgb`) is fixed artwork in the Ice &
  Ember colours on black. It is not themed and no token applies to it.

---

PocketOS Design System v0.1 — **STATUS: APPROVED FOR IMPLEMENTATION**
Amendment A (§17) approved 2026-09-10; C8 closed.
Amendment B (§18) approved 2026-09-11.
Amendment C (§19) accepted 2026-09-15.
