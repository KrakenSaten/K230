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
| `themes.json` | machine-readable token tables: 6 themes × 3 modes (the sixth, `doors`, is §32), derived tokens, contrast audit. Generated from §4–§6 rules; if they ever disagree, this document wins and the JSON is regenerated |
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
| status bar height | 56 (FULL chrome); 32 (COMPACT); 0 (NONE) — which one a screen gets is §30 |
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
  boot. (§32, accepted 2026-09-23: the fallback, and so the default, is `doors`.)
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

**Status bar** — 56 px on `bg`, bottom `hairline` in `line` (this is the
FULL chrome of §30; the COMPACT chrome is the same bar at 32 px, and NONE is
its absence — which one a screen gets is decided there, by the shell). Four
cells left
→ right, separated by `hairline` rules in `line`, all caption style
(0.08 em): (1) wordmark "PocketOS" weight 500, padding 0 20 (the product name
is Doors since ADR-005: the wordmark text is "Doors", everything else in this
cell is unchanged); (2) hint cell,
`text_secondary`, flex 1, padding 0 16 — shows AP dot (`net_connected`, 8 px)
+ SSID when connected, else context text ("3 unread", "First boot"); (3)
radio cell, padding 0 10, containing the radio chip; (4) clock, padding 0 20.
The bar's outer ends also clear the panel's rounded corners: §21.1 (proposed).
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

All screens: status bar (in the chrome §30 gives the screen: FULL in
portrait and on the launcher, COMPACT under an app in landscape), then
(except 10.4) header, then body with §7 spacing. Content strings shown are
sample data.

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
- The launcher's app icons, and their 32 px size on a tile, are §20
  (Amendment D).

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
  All six themes pass (Normal minima: text 16.4, text_secondary 6.7,
  status_error 5.3, accent 7.8, on-accent 7.8; Outdoor minima: text 21,
  text_secondary 11.6, all others ≥ 6.8; the sixth theme, `doors`, is §32).
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
  Tile size 150 px, 2 columns, 20 px gutter unchanged. The icon's size and
  source: §20 (Amendment D; 32 px on a tile). Landscape columns: §21.3
  (proposed).
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
   fallback `ice` + `normal` on any failure (`doors` + `normal` under §32).
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
- **Landscape.** §23.

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

## 20. Amendment D — Doors app icons on the launcher [NORMATIVE]

**Accepted 2026-09-15** by the product owner, after the unit A visual gate
(`docs/hardware/DOORS_APP_ICONS_GATE.md`). Brings the app icons of the Doors
graphics packages (ADR-005, `docs/design/brand/README.md`) onto the launcher
tiles, the amendment §19.3 asks for. Normative on the same terms as the rest
of this document. Nothing in §1–§19 is renumbered.

### 20.1 What they are

- One icon for each of the launcher's eleven apps, named by app id: `radio`,
  `system`, `fleet`, `radar`, `timber`, `notes`, `clock`, `calendar`,
  `calculator`, `settings` and `wave`. Stroke icons drawn on a 24-unit grid,
  1.5-unit stroke, square caps, miter joins, one colour, no fill, no tile
  background; §11 describes them.
- Their only sources are the owner's packages: the ten first icons from the
  Threshold package (`doors-threshold/icons/png-32/<id>.png`), Wave from the
  icon extension (`doors-icon-extension/png-32/wave.png`). Each is converted
  without scaling into an A8 mask, the whole 32 × 32 canvas with its
  antialiasing (`ui/pocketui/pos_app_icons.c`, generated by
  `tools/design/gen_app_icons.py`).
- An icon comes only from the owner, in a package. None is drawn, adapted or
  approximated in code. The extension's icons for apps that do not exist yet
  are not launcher icons until those apps are.

### 20.2 Size

- On a launcher tile an app icon is drawn **32 × 32, 1:1**: the 24-unit master
  at 4/3, a 2 px stroke. For the launcher this replaces C7's "24 px icon";
  §11's 24 px grid stays the grid the icons are drawn on.
- Why 32: both packages supply `png-32` as the launcher size (the Threshold
  launcher mockup uses it; the extension's notes give the 2 px stroke at
  32 px); at 24 px the 1.5 px strokes cover almost no whole pixel (none in six
  of the eleven icons, at most 23 in the rest), so they would be soft on the
  panel; and 32 px is the cell the launcher's glyph icons occupied before, so
  the tile does not change.
- Icons in chips, captions or lists are not covered here.

### 20.3 Colour

- Drawn in `accent_primary`, through `POS_STYLE_APP_ICON`, never in a colour of
  its own: the accent the launcher's glyph icons already used. Theme and
  display-mode changes repaint the icons like text (§8), Night dimming
  included. No per-app colour.

### 20.4 Place

- C7's tile, unchanged: 150 px slab, 12 px inset, the icon at the inset's top
  left, the row-title label bottom-left, 2 columns, 20 px gutter; pressed =
  `surface_raised` + focus outline.
- The icon is not a touch target of its own and takes no focus: a tap anywhere
  on the tile, the icon included, opens the app.
- Launcher tiles only: not the status bar, an app header or a list.

### 20.5 A complete set

- Every launcher app has its icon; no launcher tile shows a glyph. A new app
  comes to the launcher with its icon from an owner package.

### 20.6 Motion

- None. The icons are static in every state, so reduced motion (§12) changes
  nothing about them.

## 21. Amendment E — safe area and orientation [ACCEPTED]

**ACCEPTED 2026-09-16** by the product owner after the unit A gate
(`docs/hardware/DOORS_DISPLAY_GEOMETRY_GATE.md`: PASS). Found on unit A: the
panel's rounded top corners cut the left of the status bar wordmark and the
last clock digit. Nothing in §1–§20 is renumbered.

The orientation Doors opens with is verified on the panel for every case of
§21.2's policy. A keyboard base attached or removed *while the board is
powered* changes the orientation by the same rule, and that path is verified in
the simulator and on the unit's own bus, but mating the connector live is not a
verified hardware operation and nothing in this amendment asks for it.

### 21.1 Safe area

- The platform describes the physical panel: native size, straight-edge
  strips that are not visible, and a square at each rounded corner that the
  corner may cut (`ui/pocketui/pos_display.h`). Layout uses the logical
  geometry derived from it for the orientation in force, never the screen's
  rectangle alone.
- A bar that lies along a screen edge from corner to corner - the status bar
  (§9) - keeps its own padding where that clears the corner squares and takes
  the corner inset where it does not. On the T-Display K230 the status bar's
  side padding becomes the corner square's side instead of 20 px; nothing
  else in it changes.
- Content that starts below the corner band keeps its geometry: the launcher
  (C7), app headers and bodies are unchanged in portrait.
- The corner squares are **30 px at every corner in portrait**, the vendor
  launcher's own status-bar side inset on this panel, with which every
  portrait layout since v0.0.10 was accepted on unit A. **In landscape the two
  TOP corners are 50 px** (the native left corners, §21.2) and the two bottom
  corners 30 px - owner decision 2026-09-23. The 50 comes from unit A's
  calibration with `POCKETOS_SAFE_CORNERS` on 2026-09-21: 45 px still cut the
  landscape status bar, 50 px was the smallest that passed, and §31 was
  accepted on the unit with it. That calibration is the device evidence; the
  built-in default (`platform.h`, applied by `shell_display_resolve` once the
  orientation is known) is confirmed on the device by the v0.0.11 release
  gate, not before. Until then it was one PROVISIONAL 30 px square at every
  corner in both orientations.

### 21.2 Orientation

- Orientation belongs to the system. Settings > Display > Rotation offers
  **Automatic**, **Portrait** and **Landscape**; apps never choose or change
  it and lay out in the body they are given.
- Portrait and Landscape are forced whatever the keyboard. Automatic is
  Landscape only when a keyboard is known to be present; absent or unknown is
  Portrait. A keyboard attached or removed while Doors runs changes the
  orientation in Automatic, and changes nothing in a forced mode.
- Landscape is one direction: the device turned a quarter turn clockwise from
  portrait (the portrait left edge at the top), the rotation the vendor
  launcher uses on this board with its keyboard.
- A change is applied by the system, not asked of the owner: the display is
  rotated when it is opened, so Doors opens it again - it restarts itself in
  place, in the same process, and comes back on the launcher. The screen is
  dark for that moment, Settings says so before it happens (§2), and the
  device itself is never restarted for it. A change settles first (about a
  second), so a mode tapped twice, or a base board finding its contacts, costs
  nothing. No animation: reduced motion (§12) changes nothing.
- Display and touch always turn together; the safe area turns with them
  (the portrait corners are the same physical corners in landscape).

### 21.3 Landscape layout

- Status bar: the same bar along the long edge, with the §21.1 insets - 56 px
  on the launcher, and under an app the chrome §30 resolves for it, which in
  landscape is COMPACT (32 px) unless the app declares otherwise.
- Launcher: the same tiles - 150 px high, 20 px gutters and padding, the §20
  icon, the row-title label - in as few columns as let every row fit without
  scrolling: six columns and two rows for eleven apps on 1232x512. Only the
  column count and so the tile width (182 px) differ from portrait.
- App bodies are not re-laid out: fixed-width content stays at its portrait
  width and the body scrolls vertically. An app that needs a landscape layout
  gets one in its own amendment. Calculator: §22. Notes: §23. Settings: §24.
  System: §25. Clock: §26. Calendar: §27. Fleet: §28. Radar: §29.

## 22. Amendment F — Calculator in landscape [ACCEPTED]

**ACCEPTED 2026-09-16** by the product owner, after the unit A gate
(`docs/hardware/CALCULATOR_LANDSCAPE_GATE.md`: PASS). Proposed the same day.
The acceptance rests on: the host validation (make test and every shell and
UI test in both orientations, the layout mutations caught); the remote
validation on unit A (the build installed with only the shell service
restarted, every key tapped through the touch device in portrait, landscape
and back, the panel captured and matching the simulator, rotation applied in
place, no fault); the owner's physical check of portrait, landscape and touch
in both; the owner's approval of the 10 px portrait keypad lift (§22.2); and
the unit's clean recovery from an unplanned, unclean reset during that check.
Normative on the same terms as the rest of this document. The first app given
its own landscape layout under §21.3. Nothing in §1–§21 is renumbered, and
nothing outside Calculator changes.

### 22.1 Two shapes, chosen from the body

- Calculator lays out in the body it is given (§21.2) and picks its shape
  from that body's size alone, never from the orientation: **tall** when the
  body is at least as tall as it is wide, **wide** otherwise - and wide only
  when the body is tall enough for five rows of 64 px keys (§7), so no key
  can ever come out under the touch minimum. The shape is chosen again
  whenever the body changes size; the calculation in progress is kept.
- **Tall** (portrait, 528 x 1060 on the reference panel) is the existing
  layout: the display above, the keypad at the foot, five rows of 128 px,
  keys 126 px wide, the display taking the rest.
- **Wide** (landscape, 1192 x 396): the display on the left and the keypad on
  the right, **sharing the width equally** with the §7 20 px gutter between
  them (586 px each), both the full height. The keypad keeps its 4 x 5 grid,
  the same nineteen keys in the same places, and its 8 px gaps; the five rows
  share the height (70 px each, keys 140 px wide). The display keeps its two
  right-aligned lines at its foot, and at 586 px is wider than in portrait, so
  every number that fits there fits here.
- Same styles, same glyphs, same `=` as the one primary button, no motion.
  Keys stay taller than the §7 minimum; they are no longer the 128 px of
  portrait, because the landscape body is only 396 px tall.

### 22.2 The foot clears the rounded corners

- The body's 20 px padding clears the straight edges but not the 30 px
  corner squares of §21.1 at the foot of the panel, into which the bottom row
  of keys (and, in wide, the display) would reach by 10 px. Calculator pads
  its own foot by however far a corner square reaches into its body, measured
  from the platform description (`pos_display.h`), so on a panel with square
  corners the pad is 0.
- The foot gives way rather than the sides, so the display and keypad stay in
  line with the header's Back button and the §7 body edges in both shapes.
- Consequence for portrait: the keypad sits 10 px higher and the display is
  10 px shorter (358 px); every key keeps its 126 x 128. With square corners
  the tall layout is the previous one to the pixel.

### 22.3 The pattern for the next app

What this amendment establishes for any app given a landscape layout later,
one app at a time. It is a way of working before it is shared code: nothing is
extracted until several apps demonstrably need the same thing. Twice now
something has been (§22.4), and both times as an implementation refactor that
changed no rule and no pixel.

- Choose the layout from the size of the body, not from the orientation; add
  the size limit below which the wide layout would break §7, and fall back to
  the portrait layout (which scrolls) under it.
- Build the objects once; one layout step sets flow, sizes and grid tracks,
  and runs again on the body's size change. No second set of widgets, no
  rebuild, no state lost.
- Keep portrait as it is; test that it is, pixel for pixel, on a panel with
  square corners.
- Anything that reaches the foot of the body takes the corner squares from
  the platform geometry, never a hard-coded inset (`pos_display_rect_insets()`,
  §23.4, reached through `pocketui_layout_begin()` since §22.4).
- Test both orientations on the laid-out objects: every control inside the
  body and the safe area, no overlaps, the touch minimum, text drawn whole in
  every display mode, and the body changing shape with the app open.

Notes (§23) is the second app under this pattern and adds to it in §23.4.

### 22.4 What the pattern has left behind in PocketUI

A record of what the eight responsive apps have turned out to share, and
where it now lives. Neither entry changes a rule of this document: both were
mechanical extractions of code that eight apps had already written the same
way, made after the eighth, and each was held to producing identical geometry
and pixel-identical simulator captures before and after.

- **The corner clearance of a box** — `pos_display_rect_insets()` in
  `pos_display.h`, extracted 2026-09-17 after Calculator and Notes had both
  written it. It is a pure function of a box and the platform geometry: no
  LVGL, no state, no memory of who asked. That is what lets the shell, the
  display backends, the widgets and the host tests share one definition of the
  safe area, and it is why it may not grow into anything else.
- **Whether a layout pass is needed at all** — `pocketui_layout_begin()` and
  `struct pocketui_layout_guard` in `pocketui.h`, extracted 2026-09-18 after
  all eight apps had written it. This is *PocketUI layout state*, not display
  geometry, and that is the whole reason it is in `pocketui.[ch]` and not in
  `pos_display.[ch]`: it holds LVGL types and it remembers the last pass.
  `pos_display` stays pure C and LVGL-free.

  The guard owns change detection and nothing else. It reads the frame's box,
  refuses a frame with no area yet, asks `pos_display_rect_insets()` what the
  panel leaves of it, and answers one question: is this box, with these
  insets, the one the last pass was chosen from? Everything an app does with
  the answer — the wide-or-tall decision and its size limits, padding, child
  sizing, scroll rules, widget creation, Settings' async reveal, System's
  rebuild, Fleet's and Radar's layout counters — stays in the app, where the
  amendments put it. It is a guard, not a layout framework, and it is not to
  become one.

  The eight now share one behaviour where six had a weaker one: the six older
  apps compared the box alone, and Fleet and Radar compared the box *and* the
  insets. The stricter form is the shared one. On this hardware that changes
  nothing that can be observed — the panel's corner geometry is fixed for the
  whole life of an app, because a change of orientation restarts the shell in
  place (§21.2) and every app is built again afterwards — so with the box
  unchanged the insets cannot have moved. It is stricter on paper and
  identical on the glass, which is why it needed no physical retest.

## 23. Amendment G — Notes in landscape [ACCEPTED]

**ACCEPTED 2026-09-17** by the product owner, after the unit A gate
(`docs/hardware/NOTES_LANDSCAPE_GATE.md`: PASS). Proposed 2026-09-16. The
acceptance rests on: the responsive-layout pattern of §22.3 reused - the shape
chosen from the body's size, the objects built once, the corner clearance
read from the platform; the owner's physical check of Notes in portrait and in
landscape, touch and typing included; the owner's approval of the 20 px
taller portrait editor field (§23.3); the rebase onto the shared multi-line
error-caption fix of `pocketui_text_field` and the Notes changes it needed,
with the error captions verified on the panel in portrait and landscape; the
state, persistence and rotation checks (a note open while the display turns
keeps its edit); and clean host validation (make test, every shell and UI
test, the layout mutations caught), riscv64 and DRM builds, and remote
regression on unit A. Normative on the same terms as the rest of this
document. The second app given its own landscape layout under §21.3, on the pattern of
§22.3, which it follows and extends (§23.4); it is not a second layout
system. Nothing in §1–§22 is renumbered, and nothing outside Notes changes:
not the shell, its keyboard, the rotation policy or any other app.

### 23.1 Two shapes, chosen from the body

- Notes lays its three screens - list, editor, delete confirmation - out in
  one frame that is exactly the body's content box, and shapes each from that
  box's size alone, never from the orientation: **wide** when the box is wider
  than tall and at least 836 px across, **tall** otherwise. The shape is
  chosen again whenever the box changes size, which in practice is the
  keyboard coming up or going down. The objects are built once; a change of
  shape moves only flow and sizes, so the open note, its caret, the focus and
  the keyboard are untouched.
- **Tall** (portrait: 528 x 1060, and 528 x 764 with the keyboard up) is the
  existing layout: rows over New note, Done and Delete over the field, the
  confirmation across the top.
- **Wide** (landscape: 1192 x 396, and 1192 x 100 with the keyboard up): the
  actions move into a **288 px rail on the right** and the content takes the
  rest of the width and the full height. New note sits at the top of the rail
  beside the rows (six rows in view instead of five); Done and Delete, side by
  side at 140 x 56, at the top of the rail beside the field; the confirmation
  keeps its **portrait width (528 px), centred**. Every control keeps its own
  height, so no body can bring one under the §7 minimum.
- The 836 px floor is the portrait body's 528 px, the §7 20 px gutter and the
  rail: in the wide shape a note is never narrower than in portrait.

### 23.2 Above the keyboard

- The shell takes the §17.3 sheet's 296 px off the content area across its
  full width (the shell's behaviour since v0.0.8, unchanged here), so in landscape the app has
  a 1192 x 100 px body while the keyboard is up. Under a row of Done and
  Delete that left the field 4 px; beside the rail it has all 100 px: four
  lines of body type in Normal, three in Outdoor.
- The field grows into its wrapper and an error caption takes its room from
  the field (§17.1, `pocketui_text_field`). The field's floor of three body
  lines, as `pocketui` computes it (the font's line height times 1.5: 94 px
  Normal, 117 px Outdoor), does not fit 100 px in Outdoor, nor in Normal
  together with a caption, and a floor the wrapper cannot hold pushes the
  caption out of sight and scrolls the caret into the part that is cut off.
  In the wide shape only, the field has no floor of its own: the whole
  wrapper, or all of it but the caption's room while an error is shown. The
  caption is always read; a failed save above the landscape keyboard shows
  three lines of the note in Normal and two and a half in Outdoor.
- The editor is shown, keyboard and caption in place, before the note goes
  into the field, so the field scrolls to the caret for the size it is seen
  at; and when an error caption first appears, the field is scrolled again for
  the size it has once the caption has its own, so the note is never left
  scrolled out of sight above it.

### 23.3 Consequences for portrait

- **The editor's field is 20 px taller** (228..915 above the keyboard, and it
  stops 20 px above the sheet as §7 intends, not 40). In the body's own flex
  flow LVGL 9.5 took a row gap from the editor screen for the hidden list
  screen before it; inside the gapless frame it has the full height.
- **The foot clears the rounded corners** exactly as §22.2: with 30 px corner
  squares a long list's New note sits 10 px higher (1138..1201), and the
  caption of a note shown read-only, with the keyboard down, ends at 1201. Above the
  keyboard, in the empty or short list and in the confirmation nothing reaches
  the foot and nothing moves.
- With square corners the list and the confirmation are the previous layout
  to the pixel; the editor differs only by the 20 px of field.

### 23.4 What Notes adds to the pattern

To §22.3, for the next app:

- **Height is what landscape lacks.** Where content and actions stack, put
  the actions beside the content in a rail rather than sharing the height out:
  controls keep their sizes, and the width guard (content never narrower than
  in portrait) replaces a height guard.
- **Design the keyboard-up body.** In landscape it is 100 px tall. Any
  per-control floor must be checked against it, with the error caption shown,
  and give way where it would cut off what it protects.
- **Lay a screen out before filling a scrolling control.** A text area
  scrolls to its caret for the size it has when the text is set.
- **Screens shown one at a time go in one gapless frame**, not straight into
  the body's flow, or LVGL takes a gap for every hidden one before the one on
  show.
- **Dialogs keep their portrait width** in the wide shape, centred.
- **Test with the keyboard up and with a screen hidden and shown again**, as
  well as with the display turned under the open app.
- **Shared code.** Notes needed Calculator's rule for how far the unsafe area
  reaches into a box, unchanged, and carries a copy of it. That rule - one
  pure function of a box and the platform geometry - is the one piece two apps
  now demonstrably share, and is proposed for PocketUI beside
  `pocketui_display_geometry()`. The shapes themselves are each app's own and
  are not candidates. The extraction is a separate, owner-approved change.
  *Done 2026-09-17, as an implementation refactor that changes no rule and no
  pixel:* the rule is `pos_display_rect_insets()` in `pos_display.h`, and
  Calculator and Notes call it with `pocketui_display_geometry()` instead of
  carrying copies. *And again 2026-09-18, on the same terms:* the guard that
  decides whether a layout pass is needed at all is
  `pocketui_layout_begin()`, and all eight responsive apps open with it
  (§22.4). The shapes themselves are still each app's own.

**Open for the shell, not decided here.** With a keyboard base attached,
Automatic is landscape (§21.2) and the touch sheet still comes up over half
the panel. Whether the sheet should stay down while a physical keyboard is
present, or reserve only its own 568 px footprint in landscape, is a shell
and §17.3 question for its own amendment.

## 24. Amendment H — Settings in landscape [ACCEPTED]

**ACCEPTED 2026-09-17** by the product owner, after the unit A gate
(`docs/hardware/SETTINGS_LANDSCAPE_GATE.md`: PASS). Proposed the same day.
The acceptance rests on: the responsive-layout pattern of §22.3 and §23.4
reused - the shape chosen from the body's size, a screen shaped when it is
built and again when the body changes size, the corner clearance read from the
platform; clean host validation (make test, every shell and UI test, portrait
pixel-identical to the previous layout with square corners, the layout
mutations caught) and riscv64 and DRM builds; the remote validation on unit A
(the build installed with only the shell service restarted, the panel matching
the simulator in both orientations, taps and drags injected through the Wi-Fi
switch, the independent columns, the passphrase sheet above the landscape
keyboard with its error caption, and the rotation modes); and the owner's
physical check of both orientations, touch, scrolling, typing and the error
caption on the panel. Normative on the same terms as the rest of this
document. The third app given its own landscape layout under §21.3, on the
pattern of §22.3 and §23.4, which it follows and extends (§24.4); it is not a
second layout system. Nothing in §1–§23 is renumbered, and nothing outside
Settings changes: not the shell, its keyboard, the rotation policy, PocketUI
or any other app.

### 24.1 Two shapes, chosen from the body

- Settings lays its two screens - the main screen and the network sheet - out
  in one frame that is exactly the body's content box, and shapes each from
  that box's size alone, never from the orientation: **wide** when the box is
  wider than tall and at least 1078 px across, **tall** otherwise. A screen is
  built when it is shown, as before, and shaped when it is built; a change of
  the box's size only shapes it again - flow, sizes and which box scrolls - so
  the values on show, the typed passphrase, the focus and the keyboard are
  untouched.
- **Tall** (portrait: 528 x 1060, and 528 x 764 with the keyboard up) is the
  existing layout: the Wi-Fi, Display and Appearance panels in one column with
  the §7 22 px panel gap, the body scrolling; the network sheet's text above
  its field and buttons.
- **Wide, main screen** (landscape: 1192 x 396): the panels in **two columns
  that share the width**, with the same 22 px gap between them (585 px each):
  Wi-Fi on the left, Display and Appearance on the right. **Each column
  scrolls on its own**, so the network list is scrolled without moving the
  display controls, and neither column is ever narrower than the portrait
  body.
- **Wide, network sheet** (1192 x 396, and 1192 x 100 with the keyboard up):
  one panel across the body in **two halves** with the §7 20 px gutter between
  them: the network described on the left (name, security, what joining
  means) and the field, SHOW and the buttons on the right, from the top of the
  panel. It is a form, not a §17.5 dialog, so §23.4's dialog rule (portrait
  width, centred) does not apply to it; §23.4's rail rule does - the actions go
  beside the content because height is what landscape lacks.
- The 1078 px floor is two portrait bodies and the panel gap: in the wide
  shape no panel, row, button row or field is narrower than in portrait. No
  control takes its size from the height, so no body can bring one under the
  §7 minimum.

### 24.2 Above the keyboard

- In landscape the shell leaves the app a 1192 x 100 px body while the
  keyboard is up (§23.2). Under the network's text, as in the tall shape, the
  field would open out of sight. Beside it, the field is at the top of the
  panel: it opens in view with the network's name beside it.
- **A field and its error caption are kept in view.** When an error caption
  appears under the field, or the body changes size while the sheet is open,
  the sheet scrolls just far enough for the field and its caption to be seen;
  where they already are, nothing moves. Every message the sheet shows today,
  netd's longest refusal included, fits one line across the landscape field in
  Normal and in Outdoor, so field and caption are seen together; a caption
  that ever took more room than is left would still be read whole, the field
  partly above it. SHOW and the buttons are one short scroll away; the
  keyboard's Done joins, as before.

### 24.3 Consequences for portrait

- **The foot clears the rounded corners** exactly as §22.2: the body scrolls,
  so panels pass its foot, and with 30 px corner squares the box they scroll in
  ends 10 px higher (1201). Unscrolled, the only pixels that change are that
  10 px strip; scrolled to the end, everything sits 10 px higher.
- With square corners every screen - the main screen, each network sheet,
  the error captions, Normal and Outdoor - is the previous layout to the
  pixel.

### 24.4 What Settings adds to the pattern

To §22.3 and §23.4, for the next app:

- **A scrolling screen of panels goes into columns in the wide shape, each
  column scrolling itself**, rather than one scroller holding a two-column
  grid: the columns are unequal in length, and a long list should scroll
  without taking the other column with it. Keep the portrait reading order
  down the first column, then the second.
- **Forms are not dialogs.** A screen with a field and actions follows the
  rail rule; only a §17.5 confirmation keeps its portrait width.
- **Keep the focused field and its caption in view**, when a caption appears
  and after the body changes size. The size change is learned in the middle
  of the layout pass, so the scroll waits until the pass has finished.
- **A screen that is rebuilt when shown is shaped when built**, as well as on
  a size change; nothing else about rebuilding needs to change.
- **Shared code.** Settings needed the corner clearance and called
  `pos_display_rect_insets()` unchanged. The shapes are its own. No new
  helper is proposed: the frame, the shape choice and the size handler are a
  few lines each app writes against its own objects. *Revised 2026-09-18:*
  one of those few lines turned out to be the same in all eight apps and is
  now `pocketui_layout_begin()` (§22.4); the frame, the shape choice, the
  size handler and the async reveal are still Settings' own.

## 25. Amendment I — System in landscape [ACCEPTED]

**ACCEPTED 2026-09-17** by the product owner, after the unit A gate
(`docs/hardware/SYSTEM_LANDSCAPE_GATE.md`: PASS). Proposed the same day, host
and simulator only, and rebased onto the accepted §24 before the gate. The
acceptance rests on: the responsive-layout pattern of §22.3, §23.4 and §24.4
reused - the arrangement chosen from the body's size, the screen arranged when
it is built and again when the body changes size, columns that each scroll
themselves, a confirmation at its portrait width, the corner clearance read
from the platform; clean host validation before and after the rebase (make
test, every shell and UI test, portrait pixel-identical to the previous layout
with square corners, the layout mutations caught) and riscv64 and DRM builds;
the rebased build installed on unit A with only the shell service restarted,
healthy, and System and Settings rendering on the panel; and the owner's
physical check of both orientations - the landscape screen designed rather
than stretched, the freshness line belonging to the whole screen, each column
scrolling naturally under a thumb, a drag started between two panels
included, both confirmations centred and cancelled, and portrait correct
before and after. Neither power action was confirmed on the unit. Normative on
the same terms as the rest of this document. The fourth app given its own
landscape layout under §21.3, after Settings (§24), on the pattern of §22.3,
§23.4 and §24.4, which it follows and extends (§25.3); it is not a second
layout system. Nothing in §1–§24 is renumbered, and nothing outside System
changes: not the shell, the rotation policy, PocketUI, sysd or any other app.

### 25.1 Two shapes, chosen from the body

- System lays its three screens - the live status screen, a confirmation
  (§17.5), and the panel a power action leaves - out in one frame that is
  exactly the body's content box, and arranges each from that box's size
  alone, never from the orientation: **wide** when the box is wider than tall
  and at least 1078 px across, **tall** otherwise. The 1078 px floor is two
  portrait bodies (528 px) and the §7 22 px panel gap, so no panel, row or
  button is narrower in the wide shape than in portrait. The screen is built
  as before - again whenever what it holds changes shape: a service appears
  or goes, the card row starts or stops disagreeing, a confirmation opens -
  and arranged when it is built; a change of the box's size only arranges it
  again (flow, sizes and which box scrolls), so the values on show, a
  confirmation that is open and a refused action's reason are untouched.
- **Tall** (portrait: 528 x 1060 on the reference panel) is the existing
  layout: the freshness line, then the vitals, storage, network, services,
  radio and identity panels and the Restart and Power off pair, one column
  with the §7 22 px panel gap, the body scrolling; a confirmation or the
  power-action panel across the top.
- **Wide, live** (landscape: 1192 x 396): the freshness line, and a refused
  action's reason while one is shown, **across the top**; below them the
  panels in **two columns that share the width**, 585 px each with the 22 px
  gap, **each scrolling on its own**, in portrait order down the first and
  then the second: the machine on the left (vitals, storage, network), and on
  the right what runs on it, what it is and what can be done (services,
  radio, identity, and Restart and Power off). The actions stay last, so they
  are still never under the thumb on arrival. The split keeps the two columns
  of similar length with unit A's content and with every row the screen can
  hold (six mounts, eight interfaces, twelve services).
- **Wide, a confirmation or the power-action panel**: the portrait width
  (528 px), **centred**, with the freshness line over it at the same width
  (§23.4). All of it is in view without scrolling in Normal and Outdoor, the
  power-off recovery sentence included; the body would scroll if it were not.
- Every control keeps its portrait size or grows: the paired buttons stay
  56 px tall (§7) and are 267-268 px wide in the wide shape (239 in portrait).
  No control takes its size from the height, so no body can bring one under
  the §7 minimum. Long values keep their portrait rule: cut short with an
  ellipsis inside their row, never onto a neighbour.
- There is no text field and no keyboard in System, so §23.2's keyboard-up
  body does not arise.

### 25.2 Consequences for portrait

- **The foot clears the rounded corners** exactly as §22.2: the body scrolls,
  so panels pass its foot, and with 30 px corner squares the box they scroll
  in ends 10 px higher (1201). Unscrolled, the only pixels that change are
  that 10 px strip; scrolled to the end, everything sits exactly 10 px higher.
  A confirmation and the power-action panel do not reach the foot and do not
  change. (Even with sysd not answering, the live screen is taller than the
  body and reaches it.)
- With square corners every screen - live, both confirmations, a refusal, both
  power-action panels, Normal and Outdoor, unscrolled and scrolled to the end
  - is the previous layout to the pixel.

### 25.3 What System adds to the pattern

To §22.3, §23.4 and §24.4, for the next app. The columns themselves are
§24.4's rule, which System follows.

- **What qualifies every value stays across the columns.** A line that says
  how far everything on the screen can be trusted - System's freshness line,
  and the reason an action was refused - goes above both columns in the wide
  shape and does not scroll with either, rather than into the first column
  where the second column's values would lose it.
- **A column that scrolls must be something a finger can press.** LVGL scrolls
  what the pressed object, or a parent of it, can scroll, and only a clickable
  object is found under a finger; a finger that lands in the 22 px gap between
  two panels has to land on the column, or the drag does nothing. Test a drag
  that starts in that gap.
- **Centre a fixed-width dialog by its track as well as its items.** In a
  column flow the one track is as wide as its widest item, so centring the
  items alone leaves the dialog at the left.
- **Test the width floor on both sides of it** - a body one pixel narrower
  than the wide shape needs, and one exactly as wide - not only the reference
  panel's two orientations.
- **Shared code.** System needed the corner clearance and calls
  `pos_display_rect_insets()` unchanged. The arrangement is its own. No new
  helper is proposed. *Revised 2026-09-18:* the corner clearance now reaches
  System through `pocketui_layout_begin()`, the guard all eight responsive
  apps share (§22.4); the rebuild and the arrangement are still its own.

## 26. Amendment J — Clock in landscape [ACCEPTED]

**ACCEPTED 2026-09-17** by the product owner, after the unit A gate
(`docs/hardware/CLOCK_LANDSCAPE_GATE.md`: PASS). Proposed the same day, host
and simulator only, written against §23 and rebased onto the accepted §24 and
§25 before the gate. The acceptance rests on: the responsive-layout pattern of
§22.3, §23.4, §24.4 and §25.3 reused - the shape chosen from the body's size,
the objects built once and shaped again when the body changes size, halves
that each scroll themselves, a confirmation at its portrait width, the corner
clearance read from the platform; clean host validation before and after the
rebase (make test, every shell and UI test, portrait identical to the previous
layout with square corners, the layout mutations caught, the display turned
under the open app with nothing lost or made twice) and riscv64 and DRM
builds; the rebased build installed on unit A with only the shell service
restarted, healthy, and Clock and Settings rendering on the panel; and the
owner's physical check of both orientations - portrait as before, an alarm
added and deleted through its confirmation; landscape designed rather than
stretched, nothing cut by the rounded corners, the face large and readable,
the alarm list dragged, the stopwatch and the countdown run, the label field,
Cancel and Add in view above the keyboard, an alarm added and deleted through
the centred confirmation; and portrait correct again in Automatic. Normative
on the same terms as the rest of this document. The fifth app given its own
landscape layout under §21.3, after Settings (§24) and System (§25), on the
pattern of §22.3, §23.4, §24.4 and §25.3, which it follows and extends
(§26.4); it is not a second layout system. Nothing in §1–§25 is renumbered,
and nothing outside Clock changes: not the shell, its keyboard, the alarm
alert (§18), the rotation policy, PocketUI or any other app.

### 26.1 Two shapes, chosen from the body

- Clock lays its three screens - the tabbed main screen, the new-alarm form
  and the delete confirmation (§17.5) - out in one frame that is exactly the
  body's content box, and shapes each from that box's size alone, never from
  the orientation: **wide** when the box, less the corner clearance of
  §22.2, is wider than tall and at least **1076 px** across, **tall**
  otherwise. 1076 px is two portrait bodies (528 px) and the §7 20 px
  gutter, so nothing is narrower in the wide shape than in portrait. The
  shape is chosen again whenever the box changes size. The objects are built
  once; a change of shape moves only flow, sizes and which box scrolls, so
  the time on show, a running stopwatch and its laps, a running countdown, a
  half-made alarm, its typed label, caret and focus, the keyboard and an
  open confirmation are untouched, and nothing is written.
- **Tall** (portrait: 528 x 1060, and 528 x 764 with the keyboard up) is the
  existing layout: the four tabs over one pane; the form's time, Hour,
  Minute, Repeat, label field and Cancel | Add down the body; the
  confirmation across its top.
- **Wide, main screen** (landscape: 1192 x 396): the tabs stay **across the
  top**. Under them the pane's content goes into **two halves of the width**
  with the 20 px gutter (586 px each), both the full height, **each scrolling
  on its own**:
  - Clock: with a time to show, the face takes **both halves** and the full
    height. Without one, the face and the
    "Time not set" explanation share the width, each as tall as it needs.
  - Alarm: the time-not-set notice when shown, then the alarms, on the left;
    Add alarm or the eight-alarms notice, then what an alarm can and cannot
    do, on the right.
  - Watch: the running time **fills the left half**; Start | Lap and the laps
    on the right, the laps growing to the foot and scrolling themselves.
  - Timer: the countdown **fills the left half**; the minute and second
    steppers, then Start | Cancel, on the right.
- **Wide, new alarm** (1192 x 396, and 1192 x 100 with the keyboard up): the
  **label field across the top**, with **Cancel and Add beside it in a
  288 px rail** at 140 x 56 each (§23.1's rail); under them the time **in the
  left half, as tall as Hour, Minute and Repeat** stacked in the right half
  (232 px). The form scrolls. This departs from the portrait order on
  purpose (§26.2).
- **Wide, confirmation**: the portrait width (528 px), **centred** (§23.4).
- Every control keeps its portrait size: tabs and steppers 64 px tall,
  paired buttons 56 px, rows 64 px. No control takes its size from the
  height, so no body can bring one under the §7 minimum. A long label keeps
  its portrait rule: cut short with an ellipsis inside its row.

### 26.2 Above the keyboard

- In landscape the shell leaves the app a 1192 x 100 px body while the
  keyboard is up (§23.2). Portrait's order - the time first, the label and
  the actions last - would put the field, the reason a label was refused
  and Add out of sight under a 100 px window onto the time. In the wide shape
  the form therefore begins with them: the field at the top of the body,
  Cancel and Add beside it, and the refusal's one line of caption under the
  field - 64 + 8 + 21 px in Normal, 64 + 8 + 26 px in Outdoor, inside 100.
  The time is a drag away.
- A label refused with the form part-way scrolled brings the form back to
  its top, so the reason is read. A new form opens at its top.
- The keyboard's Done still does nothing in Clock (KNOWN_ISSUES); with Add
  beside the field it is no further away in landscape than in portrait.

### 26.3 Consequences for portrait

- **The foot clears the rounded corners** exactly as §22.2. With 30 px corner
  squares the clock face, which grows to the foot when it has a time to show,
  and the laps, which grow to it, end 10 px higher (1201). Every other
  portrait screen - the tabs, the alarm list, the form with the keyboard down
  and up, the refusal, the confirmation, the running countdown - does not
  reach the foot and does not move.
- With square corners every screen is the previous layout to the pixel; the
  objects that group items for the wide shape draw nothing, take no finger
  and never scroll in the tall shape.

### 26.4 What Clock adds to the pattern

To §22.3, §23.4, §24.4 and §25.3, for the next app.

- **Tabs stay where they are; the pane under them is what goes side by
  side.** A tabbed screen keeps its tab row across the top in the wide shape.
- **A big number fills its half.** Where a pane is one large value and what
  acts on it, the value takes the first half at the full height and the
  controls go beside it; when there is nothing to act on beside it (the clock
  face with a time), the value takes both halves.
- **In the wide shape, a form begins with what the keyboard is up for.** When
  a screen's field is typed into, the field, its caption and the actions that
  finish the form go at the top - the actions in §23.1's rail beside the
  field - even where that departs from the portrait reading order, because
  the landscape keyboard leaves 100 px. The rest of the form follows and
  scrolls.
- **Grouping boxes are invisible in the tall shape.** A box that exists only
  so that items can go side by side draws nothing, is not pressed and does
  not scroll in the tall shape, so portrait keeps its pixels and its touch
  behaviour; in the wide shape it scrolls and can be pressed (§25.3), so a
  drag that starts between two items scrolls it. Test that it is reset - not
  scrollable, scrolled to its top - after the display turns back.
- **Test a wide body that is taller than it is wide**, as well as the width
  floor on both sides (§25.3).
- **Shared code.** Clock needed the corner clearance and calls
  `pos_display_rect_insets()` unchanged. The shapes are its own. No new helper
  is proposed. *Revised 2026-09-18:* the corner clearance now reaches Clock
  through `pocketui_layout_begin()`, the guard all eight responsive apps share
  (§22.4); the shapes are still its own.

**Open, not decided here.** On the unit a change of orientation restarts the
shell in place (§21.2). The one clock runtime is the shell's, so a running
stopwatch, a running countdown and any snooze - none of which is stored, by
design - end with it, while alarms, which are stored, do not. With Automatic
that includes attaching or removing a keyboard base. Whether an orientation
change should carry the runtime's running state across is a shell question;
this amendment changes nothing about it.

## 27. Amendment K — Calendar in landscape [ACCEPTED]

**ACCEPTED 2026-09-17** by the product owner, after the unit A gate
(`docs/hardware/CALENDAR_LANDSCAPE_GATE.md`: PASS). Proposed the same day,
host and simulator only, written against §23 and rebased onto the accepted
§24, §25 and §26 before the gate. The acceptance rests on: the
responsive-layout pattern of §22.3, §23.4, §24.4, §25.3 and §26.4 reused - the
shape chosen from the body's size, the objects built once and shaped again
when the body changes size, the corner clearance read from the platform; clean
host validation before and after the rebase (make test, every shell and UI
test, portrait identical to the previous layout in every simulator capture,
the layout mutations caught, the display turned under the open app with
nothing lost or made twice) and riscv64 and DRM builds; the rebased build
installed on unit A with only the shell service restarted, healthy, writing
nothing, and Calendar rendering on the panel in both orientations; and the
owner's physical check, which he ruled PASS twice, **including the landscape
day cells of §27.2, the question the gate was for**. Not everything the batch
asked was exercised on the unit: Outdoor in landscape was not entered, and
portrait was on screen for five seconds, so both rest on the host and
simulator evidence and on the owner's ruling (the gate sheet records this).
Normative on the same terms as the rest of this document. The sixth app given
its own landscape layout under §21.3, after
Clock (§26), on the pattern of §22.3, §23.4, §24.4, §25.3 and §26.4, which it
follows and extends (§27.4); it is not a second layout system. Nothing in
§1–§26 is renumbered, and nothing outside Calendar changes: not the shell,
PocketUI, the rotation policy or any other app.

### 27.1 Two shapes, chosen from the body

- Calendar lays its one screen out in a frame that is exactly the body's
  content box and shapes it from that box's size alone, never from the
  orientation: **wide** when the box, less the corner clearance of §22.2, is
  wider than tall, at least **1076 px** across and at least **384 px** tall;
  **tall** otherwise. 1076 px is two portrait bodies (528 px) and the §7 20 px
  gutter, so neither half is narrower than portrait; 384 px is six weeks of
  56 px cells (§27.2) under 24 px weekday headings, 4 px apart. The shape is
  chosen again whenever the box changes size. The objects are built once; a
  change of shape moves only tracks, sizes and which box scrolls, so the month
  on screen, the selected day, today's mark, the "Date not set" notice and
  whether Today can be pressed are untouched, and nothing is written.
- **Tall** (portrait: 528 x 1060) is the existing layout: previous, the month
  and next in a 64 px row; the weekday headings (32 px) and six weeks of 72 px
  cells, 4 px apart across and 8 px down; the panel with the notice and the
  date in words; Today (64 px); 20 px apart. Under either wide floor the tall
  layout is kept whole and scrolls.
- **Wide** (landscape: 1192 x 396, 1192 x 386 above the reference panel's
  corners): **the month in the first half of the width** (586 px) at the full
  height - 24 px headings over the six weeks, which share the height, cells
  4 px apart both ways. **The second half** (586 px, past the 20 px gutter) is
  the portrait column without the month, in portrait order: the row of
  previous, the month and next at the top (72 x 64 slabs); **Today at the
  foot**, full width of the half, level with the last week; and the **panel
  between them**, as tall as that leaves (218 px above the reference panel's
  corners). The panel's tallest content - the notice and a picked day in
  Outdoor type - measures 201 px; should it ever say more, the panel scrolls
  itself. Every day of every month is on screen at once, and nothing else
  scrolls.
- Same styles, the same today mark (accent number, dot beneath), the same
  selected outline, the same disabled Today, no motion.

### 27.2 Day cells in the wide shape

- The landscape body is too short for the portrait cells: six weeks of 64 px
  cells and their 5 gaps need 404 px before any heading, in a 386 px body.
  The choice is between cells under 64 px tall and a month that is never
  wholly on screen. This amendment proposes the first: **in the wide shape a
  day is at least 64 px wide and at least 56 px tall** - wider than tall
  (about 80 x 56 on the reference panel, and 80 x 58 with square corners),
  the height §7 gives paired buttons and segmented controls, larger in area
  than 64 x 64.
- It rests on what a day is: tapping one only selects it, the selection is
  shown at once in the grid and in words, and another tap corrects it. No
  irreversible action is on a day. The arrows and Today keep 72 x 64 and
  64 px.
- It is **not** DEV-1 (§17.3) and does not cite it: DEV-1 is keyboard keys
  only and narrower than 64 px; this is a day cell, at least 64 px across, in
  the wide shape only. Portrait keeps its 72 px squares.
- **The owner's physical check accepted it** on unit A, 2026-09-17: thumb
  taps across the month in landscape select the day meant. Had they missed,
  the alternative was 64 px weeks in a month that scrolls in its half, which
  no other rule here depends on.

### 27.3 Consequences for portrait

- None visible. Nothing in portrait reaches the foot of the body, so the
  corner clearance moves nothing: with square corners and with the reference
  panel's 30 px corners every portrait screen is the previous layout to the
  pixel (56 of 56 simulator captures, Normal and Outdoor).
- The body no longer scrolls in the fallback: the frame does, when a box too
  short for the tall layout is given, as the body did.

### 27.4 What Calendar adds to the pattern

To §22.3, §23.4, §24.4, §25.3 and §26.4, for the next app.

- **A fixed grid of targets that loses height shares it.** Where a screen's
  content is a grid whose count cannot change (a month is always six weeks),
  and landscape is too short for it at portrait size, let its rows share the
  height and its cells go wide rather than cut or scroll it - down to a
  stated floor that is itself a proposed touch size, with the shape falling
  back to tall below it. Test both sides of that floor, as §25.3 asks of the
  width floor.
- **Move the tallest item beside the rest, and keep the rest in portrait
  order.** When one item is most of a portrait column, it takes the first
  half; the others stay in their order in the second half, the stretchy one
  taking the room between the fixed ones.
- **Measure the tallest text before rearranging.** The panel's worst case
  wraps differently at the half's width than in portrait; size a stretched
  panel against its tallest content in Outdoor type as laid out, not as
  estimated.
- **One grid can hold both shapes.** A frame laid out as one LVGL grid - one
  column in the tall shape, two in the wide, with the tall item spanning the
  rows beside it - needs no grouping boxes and no second tree; the tall
  shape's tracks give the portrait positions to the pixel.
- **Shared code.** Calendar needed the corner clearance and calls
  `pos_display_rect_insets()` unchanged. The shapes are its own. No new helper
  is proposed. *Revised 2026-09-18:* the corner clearance now reaches Calendar
  through `pocketui_layout_begin()`, the guard all eight responsive apps share
  (§22.4); the shapes are still its own.

**Open, not decided here.** The selected outline is drawn outside its cell
(§7) and the week's row clips it, so a selected day shows the outline on its
sides only, and a selected Monday on its right only; the same in portrait since
v0.0.9. On the unit a change of orientation restarts the shell in place
(§21.2) and comes back on the launcher; Calendar, opened again, lands on today,
so a picked day does not survive the turn - nothing is stored, by design.
Neither is changed here.

## 28. Amendment L — Fleet in landscape [ACCEPTED]

**ACCEPTED 2026-09-18** by the product owner, after the unit A gate
(`docs/hardware/FLEET_LANDSCAPE_GATE.md`: PASS). Proposed the same day and
reworked twice before he passed it, each time on his ruling: first the
arrangement, because normal play required scrolling, which §28.6 now forbids
outright; then the board itself, because a 34 x 34 cell was too small under a
thumb. Both rulings were right and neither was a matter a test could have
settled. The acceptance rests on: the
responsive-layout pattern of §22.3, §23.4, §24.4, §25.3, §26.4 and §27.4
reused — the shape chosen from the body's size, the objects built once and
shaped again when the body changes size, the corner clearance read from the
platform; clean host validation (make test, every shell and UI test, Deploy,
Battle and Result in portrait pixel-identical to v0.0.10 below the status bar,
every cell of the board hit at its centre and its four corners at three board
sizes, the layout mutations caught) and riscv64 and DRM builds; and the build
installed on unit A with only the shell service restarted, healthy, playing
matches out in portrait and in landscape with taps, drags and button presses
injected into the touch device; and **the owner's physical check, which he
ruled PASS** — on the two questions the gate was for, the 51 x 34 cell with
the two ways of aiming (§28.2) and whether the whole no-scroll screen reads at
a glance. Normative on the same terms as the rest of this document. The
seventh app given its own landscape layout under §21.3, after Calendar
(§27); it is not a second layout system. Nothing in §1–§27 is renumbered, and
nothing outside Fleet changes: not the shell, PocketUI, the rotation policy or
any other app.

**The rule this amendment is built round** is §28.6: *normal landscape Battle
gameplay fits in one viewport and requires no scrolling.* That is not a
consequence of the layout — it is the constraint the layout is derived from, it
is what the body's scarce height is spent on in §28.3, and it is held by a test
that plays whole matches out across the page and measures the screen after
every turn, in both type sizes and with both corner shapes.

**What the owner was asked to decide** was §28.2: a landscape board cell of
**51 x 34 px**, and with it the two ways of aiming that mean no square has to
be hit exactly. A 34 px row was tried first and rejected as too small for a
thumb; the answer is not a bigger row - §28.2 shows that ten rows in this body
can never exceed 38 px - but a cell that grows on the axis with room, and an
aim that does not depend on landing on it. **He has had a thumb on it and
ruled PASS**, so §28.2 is settled and no longer provisional.

### 28.1 Two shapes, chosen from the body

- Fleet lays its four screens out in a frame that is exactly the body's content
  box and shapes them from that box's size alone, never from the orientation:
  **wide** when the box is wider than tall, when a labelled board fits it at no
  finer than **32 px** a cell, and when what is left across it after the board
  and a §7 gutter is at least **two 280 px columns** — a column that can hold a
  64 px action and a wrapped line of text; **tall** otherwise. The shape is
  chosen again whenever the box changes size, and only then.
- The board is square and its side is what the box's height can hold: ten
  cells, nine 2 px gaps and the 24 px gutter the A–J and 1–10 captions take. A
  taller box does not grow the board past the tall shape's 48 px; it gives the
  room to what is said about the board instead.
- **Tall** (portrait: 528 x 1060) is the v0.0.10 layout unchanged: each screen
  a single column, the boards at 48 px cells, the frame scrolling what does not
  fit. **Wide** (landscape: 1192 x 386 once the foot has cleared the rounded
  corners) puts the board at the left and everything said about it beside it.
- The objects are built once. A change of shape turns boxes, moves sizes and
  changes the boards' cell size; it creates and deletes nothing, so a match in
  progress — the crosshair, the turn, both boards, the fleet as placed, and an
  opponent's reply still being paced out — is untouched by it.
- Whatever the shape, the frame pads its foot by however far the panel's
  rounded corner squares reach into it, measured with the one platform rule
  (§22.2, §23.4), never worked out in the app.

### 28.2 The board across the page, and the cell it draws

**A row can never be more than 38 px in this shape, so the design does not ask
for a precise touch.** That is the whole of §28.2 in one line; the rest is why,
and what is done instead.

#### The arithmetic, which is not negotiable

The board is ten rows of square-cornered cells with a caption gutter, so its
height is `24 + 10h + 9×2`. The body across the page is 386 px once the foot
has cleared the rounded corners, which gives `h ≤ 34.4`. Spending the gutter
and the gaps as well - losing the A–J and 1–10 labels the readout's "F6" is
read against - would buy 38. **The 48 px of deviation D1 would need a 522 px
body**, which the wide shape does not have and cannot be given: the shell's
status bar, the app header and the body's own padding are not the app's to
take, and nothing may be scrolled (§28.6). No arrangement of anything else on
the screen changes this by a pixel.

#### What is done about it

- **The cell grows on the axis that has room.** The board takes a third of the
  width and 810 px are left over, so once the wide shape is taken the board
  spreads into whatever the two columns beside it can spare, up to **half as
  wide again as it is tall** and never less than square. On this panel that is
  **51 x 34** - half as much again in area as the square 34, and wider than
  the 48 px the tall shape draws. Past 3:2 a board of ten by ten stops reading
  as a board, so that is the cap.
- **Aiming does not depend on hitting a cell.** A press reports the square
  under it, and so does every moment of a drag, so the player lands anywhere on
  the 552 x 382 board and slides, reading the square's name off the readout
  beside it. The aim is corrected by watching, not by hitting.
- **And it can be done without touching the board at all.** Four one-square
  nudges stand in the readout panel, each a full 64 px target. With no
  crosshair the first press starts in the middle, so every square is within
  five presses. This is what makes deviation D1's premise true at this size:
  D1 permits a small target because a mis-aim is *correctable*, and a
  correction that is as hard as the original aim is not a correction.
- **Nothing about this commits a shot.** FIRE is still the only thing that
  fires, still the full §7 size, and still armed only on a square that has not
  been fired at.

#### What is unchanged

- **The mapping is exact on both axes.** One stored width and one stored
  height drive the drawing and the hit test alike, so a cell can never be drawn
  at one size and hit at another; a point maps to exactly one cell; the 2 px
  gap after a cell belongs to that cell; and the caption gutter and everything
  outside the board aim at nothing. This is tested at 48 x 48, 52 x 35 and
  51 x 34, at the centre and at all four corners of all 100 cells.
- **Deploy draws the same board, cell for cell.** A fleet is placed on the
  squares the shots are later aimed at; the two screens may not disagree about
  where a square is.
- Below 32 px a row the wide shape is refused and the tall stack is kept whole
  and scrolled, as the body did before this layout existed.

### 28.3 What stands beside the board

- **Battle.** The board at the left, taking the body's whole height. Beside it,
  in the order a turn is played: **TARGET**, the readout — the square aimed at,
  the enemy fleet, what firing would do, and what the last exchange did — then
  **YOUR WATERS**, your own board at the same 20 px cells it has down the page,
  and then **FIRE across the foot of both of them**, 790 x 95, its foot level
  with the board's.
  - The two panels are each **exactly as tall as what they hold**, and are
    given the same height as each other so that they close on the same line.
    Your own board is the taller content and sets that height; the readout,
    which has the fewer pixels of content and the more words, takes the width.
  - **Nothing is stretched to the foot of the body.** A panel that ends where
    its neighbour ends reads as a panel; one that ends at the edge of the body
    reads as a view that has been cut off, whether or not it can be scrolled.
    The room the panels leave is spent on FIRE rather than left as a gap.
  - The board is the largest object on the screen by area and your own waters
    the smallest of the three; the readout is the widest because it is the only
    one holding lines of text. Weight follows priority, not symmetry.
  - Two objects move between the shapes rather than being built twice: FIRE,
    which is at the foot of the readout column down the page and across the
    whole region over here; and the log line, which reports the exchange just
    played and so belongs beside the readout, where there is room for it.
  - The readout panel also carries **the four one-square nudges** (§28.2), at
    its foot, nearest FIRE: what is aimed at, what firing would do, what the
    last exchange did, and then the controls. The panel's slack goes to the
    log line, so the nudges come to rest on the panel's content edge whatever
    the text above them does - a control a thumb can learn the position of,
    rather than one that moves when a line wraps. They are shown only in the
    wide shape; a hidden child takes no room in a flex layout, so portrait is
    untouched by them.
- **Deploy.** The board at the left, YOUR FLEET beside it, and the placement
  controls beyond: TURN, AUTO and CLEAR across the top of that column, the
  refusal line taking the slack, and CONFIRM DEPLOYMENT at its foot.
- **Command.** The four panels in three columns — the choice, the fleet, the
  terms — with a foot row under them carrying the ways on: RESUME, when there
  is a match to resume, and DEPLOY FLEET, each a third of the body wide and at
  the end of the row. A column that cannot show all of its panels scrolls
  (§24.1, §25.1, §26.1); the ways on are never in a column, so neither can be
  the thing that has been scrolled out of sight.
- **Result.** The outcome over everything, the two accounts of it side by side
  in halves, and the two ways on side by side under them.

### 28.4 Consequences for portrait

- Deploy, Battle and Result are pixel-identical to v0.0.10 below the status
  bar, in Normal and in Outdoor, with 30 px and with square corners — with one
  measured exception: in **Outdoor**, Battle's bottom panel border comes to
  rest **6 px higher**, which is the foot clearance below doing its work, seen
  at the one type size where the stack happens to end within those rows.
- Command's stack starts **9 px lower**. A panel's caption straddles its top
  border and so is drawn above the panel; the frame now clips at the body's
  content box, where before the shell's body clipped at its own outer edge and
  the caption of the first panel was drawn into the body's padding. The nine
  pixels are that caption's rise, given to it inside the frame.
- The foot of a scrolled stack stops **10 px higher**, which is the corner
  clearance doing its work: where the last panel used to come to rest in the
  bottom corner squares, it now clears them. While a stack is being scrolled,
  content still passes through that band, as scrolled content does.
- Nothing else in portrait moves, and no portrait cell, button or behaviour
  changes. Portrait Battle still scrolls, as it has since v0.0.10: 528 px of
  width cannot hold a 48 px board and everything said about it on one screen,
  and the tall shape is the one deviation D1 was approved for.

### 28.5 What Fleet adds to the pattern

To §22.3, §23.4, §24.4, §25.3, §26.4 and §27.4, for the next app.

- **A custom-drawn viewport should own its geometry and be resizable in
  place.** Where a screen is built round one custom-drawn object, give that
  object the smallest possible setter for its size and let the drawing and the
  hit test read the one size it stores. Then a change of shape is a resize, not
  a rebuild, and there is no way for the picture and the touch target to
  disagree. Put the arithmetic that turns a size into that geometry in the
  object's own module, so a layout asks it for a size rather than working one
  out from the gutters and the gaps itself.
- **When a target cannot be made big enough, stop asking for a precise
  touch.** Some shapes simply have less room than a finger needs, and no
  arrangement of the rest of the screen changes it - work the limit out and
  write it down rather than shaving pixels towards it. Then grow the target on
  whichever axis does have room, and add a way to reach the thing that asks no
  precision at all: a drag that reports what is under the finger the whole
  way, and a set of one-step controls at full size. A small target is only
  acceptable while a mis-aim is genuinely correctable, and a correction that
  is as hard as the original aim is not one.
- **A deviation follows its interaction, not its number.** Where a landscape
  layout makes an already-deviating target smaller, say plainly what the
  deviation rested on and show that it still holds. A number alone decides
  nothing; the interaction — here, that a tap is reversible and a separate
  64 px action commits — is what was approved.
- **Test the whole target, not its centre.** A dense grid is worth hitting at
  all four corners of every cell, in the gaps between cells, in the caption
  gutter and just outside the board, at every size the layout can produce. That
  is what turns "the mapping is right" from a claim into a test.
- **A transparent container clips too.** LVGL clips a child to its parent's box
  grown by the parent's own extra draw size, so a caption that straddles a
  panel's top border is cut by every grouping box it is nested in, not only by
  the one that paints. A box that paints nothing should say so; where something
  must clip — the frame, or a column made to scroll — the layout gives the
  caption its room instead.
- **An action must not live in a column that scrolls.** Put the ways on a foot
  row of their own. Moving one button between a panel and that row across a
  change of shape is a move, not a rebuild, and is preferable to leaving the
  button below a fold.
- **A panel stretched to the edge of the body reads as a view that has been cut
  off.** Filling a column with a panel is the obvious thing to do with room
  left over, and it is the wrong thing: the eye takes a border that coincides
  with the edge of the body as the edge of a viewport, and the screen reads as
  though it continues past it. Size a panel to what it holds, line its foot up
  with its neighbour's, and spend what is left on the action instead. See
  §28.6.
- **Shared code.** Fleet needed the corner clearance and calls
  `pos_display_rect_insets()` unchanged. The shapes, the boxes and the board's
  resize are its own. No new platform helper is proposed here — but see the
  note below. *Revised 2026-09-18:* Fleet's stricter guard — box *and* insets,
  behind an explicit valid flag — is the one all eight responsive apps now
  share, as `pocketui_layout_begin()` (§22.4). Fleet's layout counter stays in
  Fleet and still counts exactly the passes that were needed; the shapes, the
  boxes and the board's resize are still its own.

### 28.6 Normal landscape Battle gameplay fits in one viewport and requires no scrolling

This is the rule. It is written as a sentence because it is a promise to the
player, not a property of one arrangement of boxes.

- **What "normal gameplay" means here**, exactly: seeing the whole target
  board, every square of it; seeing the square currently aimed at and what
  firing on it would do; seeing what the last exchange did; seeing your own
  waters and how much of your fleet is left; knowing whose turn it is and what
  state the game is in; and reaching FIRE. None of these may require the player
  to change a scroll offset, in either direction. Whose turn it is is the one
  of those that is not on the screen at all: it is in the status bar, above the
  body and unscrollable by construction (§9), and the test holds the app to
  keeping it current for exactly that reason.
- **The screen is laid out to fit, not scrolled to fit.** Across the page the
  Battle root is not a scroller, nothing under it is a scroller, and nothing it
  holds has been scrolled. Where content would not fit, the layout is what
  gives — a smaller own board, a shorter panel — never a scrollbar.
  `tests/fleet_lint.sh` refuses a Battle screen that turns scrolling on or
  scrolls anything into view.
- **It is held through whole matches, not at the moment of building.** The two
  things on this screen whose height depends on what they say — the note under
  the readout and the line reporting the exchange — only grow once a game is
  under way, which is exactly when the player cannot afford a fold.
  `tests/fleet_app_test.c` plays a match out across the page and, after every
  single turn, checks that nothing can be scrolled, that nothing has been
  scrolled, and that no object on the screen lies outside the body: four times
  over, in Normal and in Outdoor type, with the unit's corners and with square
  ones.
- **Deploy may scroll; Battle may not.** Setting a fleet out is a workflow with
  an end, done once, and its roster genuinely wants the room. A turn of Battle
  is the thing the game is, taken sixty times a match.
- **For the next app.** Where a screen is a loop the player repeats, fitting it
  on the display is a requirement to derive the layout from, not a result to
  check afterwards — and it is worth a named test and a lint rule, because
  nothing else stops the fold coming back the next time something is added.

**Settled by the owner's hand, not by a test.** Whether a 51 x 34 cell, a drag
that reports what is under the finger, and four one-square nudges together
make aiming comfortable under a thumb - and whether the whole no-scroll Battle
screen reads naturally at a glance - were the two things only the panel and a
hand could answer. Both were ruled **PASS** on 2026-09-18, on build `0dd9ee1`.
A 34 x 34 cell was tried first and rejected as too small, which is what §28.2
is written around. §28.6 is not one of them: that a turn fits and is
never scrolled is measured, on the host after every turn of four played-out
matches and on unit A by drags that move nothing. On the unit a change of orientation restarts the shell in place
(§21.2) and comes back on the launcher, so Fleet is never open while the
display turns; the match survives because it is stored after every resolved
turn and offered again, which is verified on unit A, while the relayout under
an open app stays host evidence. Seven apps now carry the same size-change
guard and two now carry a custom-drawn viewport with a resize setter; whether
either is worth a shared helper is worth asking once Radar has landed, and is
not proposed here.
## 29. Amendment M — Radar in landscape [ACCEPTED]

**ACCEPTED 2026-09-18** by the product owner, after the unit A gate
(`docs/hardware/RADAR_LANDSCAPE_GATE.md`: PASS on all three questions).
Proposed the same day, developed independently of Fleet's §28 and numbered so
the two would not collide, then rebased onto the master Fleet made and
revalidated whole. The acceptance rests on: the
responsive-layout pattern of §22.3 through §28.5 reused — the shape chosen
from the body's size, the objects built once and shaped again when the body
changes size, the corner clearance read from the platform; clean host
validation (make test, every shell and UI test, **both screens in portrait
pixel-identical to v0.0.10 below the status bar in all 24 simulator
captures**, the scope's pixel round trip run at both sizes, the layout
mutations caught) and riscv64 and DRM builds; and the build installed on
unit A with only the shell service restarted, healthy, with a run played in
both orientations through taps injected into the touch device; and **the
owner's physical check, which he ruled PASS** on all three of the questions
the gate was for - the scope's readability, targeting with a thumb, and the
balance of scope-left against information-right. Normative on the same terms
as the rest of this document. The eighth app given its own landscape layout
under §21.3, after Fleet (§28); it is not a second layout system. Nothing in
§1–§28 is renumbered, and nothing outside Radar changes.

**What is different about this app** is that it costs something per frame.
PocketRadar repaints a custom-drawn scope twenty times a second while a run is
on, and that is the whole of its frame cost (`docs/KNOWN_ISSUES.md`, hardware
verification H1, still unmeasured on the K230). A landscape layout therefore
has to be shown not to have made it worse. §29.2 is how.

### 29.1 Two shapes, chosen from the body

- Radar lays its two screens out in a frame that is exactly the body's content
  box and shapes them from that box's size alone, never from the orientation:
  **wide** when the box is wider than tall, when the scope would be at least
  **240 px**, and when what is left across it after the scope and a §7 gutter
  is at least **360 px** — a region that can hold two cards abreast and a
  64 px action under them; **tall** otherwise. The shape is chosen again
  whenever the box changes size, and only then.
- The scope is square and its side is what the box's height can hold, **never
  more than the 520 px it has down the page** (§29.2).
- **Tall** (portrait: 528 x 1060) is the v0.0.10 layout unchanged: the numbers
  that persist, the scope, the contact being worked, the action. **Wide**
  (landscape: 1192 x 386 once the foot has cleared the rounded corners) puts
  the scope at the left, the two cards abreast beside it, and ENGAGE across
  the whole foot of that region — it is hit often and in a hurry, and a wide
  target is what the room is for.
- The objects are built once. A change of shape turns boxes, moves sizes and
  resizes the scope; it creates and deletes nothing, so a run in progress —
  its contacts, its score, its level, its selection — is untouched by it. The
  scope object itself is **moved** between the card column and the screen,
  because down the page it belongs between the numbers and the contact card
  and across the page it belongs before both; moving is not rebuilding, and
  the pattern is §28.5's.
- Whatever the shape, the frame pads its foot by however far the panel's
  rounded corner squares reach into it, measured with the one platform rule
  (§22.2, §23.4).

### 29.2 The wide shape may not cost more per tick

This is normative, and it is the reason the scope is capped rather than simply
fitted:

- **The scope across the page is never larger than the scope down it.** On the
  reference panel it is 386 px against 520, which is 55 % of the pixels; a
  taller body does not grow it past 520. So a tick invalidates fewer pixels in
  the wide shape than in the tall one, never more, whatever body it is given.
- **The layout runs only when the body's box or its safe-area insets change.**
  It never runs on a tick. The run's clock steps the engine, drains its events
  and invalidates the scope; it sizes, moves and reshapes nothing.
- **The numbers are still written only when the value behind them changes.**
  The `seen_*` comparison of v0.0.9 is untouched by this layout.

All three are measured rather than asserted: over a hundred ticks of a running
scan the scope is drawn 200 times, the numbers 10 and the contact card 0, and
the layout is not worked out once.

### 29.3 The figures on the Result screen

Five Design System rows are taller than a landscape body. Across the page the
figures therefore stand in **two columns inside their own card** — three and
two — with the divider under the third dropped, because in a column it would
be a line into the gutter beside it. Down the page they are the one column
they have always been, divider and all. A results screen is the last place
anything should have to be scrolled to.

### 29.4 Consequences for portrait

**None.** Both screens are pixel-identical to v0.0.10 below the status bar, in
Normal and in Outdoor, with 30 px and with square corners, in all 24 simulator
captures. Radar's cards carry no caption on their top border, and its stack
already fits the body, so neither the caption headroom nor the foot clearance
of §28.4 moves anything here.

### 29.5 What Radar adds to the pattern

To §22.3 through §28.5, for whoever comes next.

- **Where a layout costs something per frame, cap it rather than fit it.** A
  viewport that is repainted continuously should be given a ceiling equal to
  what it costs in the shape that already exists, so that no body can make it
  dearer. A floor keeps it usable; the ceiling is what keeps it affordable.
  Then say so in a test that measures, not one that asserts.
- **Count the draws.** A draw-event counter on each thing on the screen, over
  a hundred ticks, turns "only the scope is repainted" from a design intention
  into a number. It costs a few lines and it is the only way to catch a
  layout that quietly starts invalidating a parent.
- **A fixed list that will not fit goes into columns inside its own card**,
  not into a scroll, when the screen exists to report it. Drop the divider
  that a column turns into a line to nowhere.
- **A tolerance derived from a geometry must be derived in the test too.** The
  scope's round trip carries range in pixels of radius, so the same absolute
  error is a larger share of the range on a smaller scope; a fixed tolerance
  passed at one size and failed at the other for no reason but arithmetic.
- **Shared code.** Radar needed the corner clearance and calls
  `pos_display_rect_insets()` unchanged. The shapes, the boxes and the scope's
  resize are its own. *Revised 2026-09-18:* Radar's guard — box *and* insets,
  behind an explicit valid flag — is, with Fleet's, the one all eight
  responsive apps now share, as `pocketui_layout_begin()` (§22.4). Radar's
  layout counter stays in Radar, so §29.2's "never on a tick" is still checked
  against the app's own number; the shapes, the boxes and the scope's resize
  are still its own.

**Open, not decided here.** Whether the landscape scope is comfortably
readable, whether selecting and engaging contacts is comfortable with a real
thumb, and whether the scope and the right-hand region are in the right
balance are the three things only the panel and a hand can answer. H1 — the
frame cost on the K230 — remains unmeasured; this amendment does not close it,
it only makes the wide shape cheaper than the shape H1 was written about. On
the unit a change of orientation restarts the shell in place (§21.2) and comes
back on the launcher, so Radar is never open while the display turns, and
PocketRadar has no resume: a run is abandoned by a turn, as it is by leaving
the app, which is v0.1 behaviour and not changed here.

## 30. Amendment N — Status chrome policy [ACCEPTED]

**ACCEPTED 2026-09-23** by the owner, on the v0.0.11 RC1 unit A gate
(`docs/hardware/V0.0.11_RELEASE_SMOKE.md`), which is what §30.7 below asked of the device
only in part: see that sheet for the items it covered and the ones it did
not. Stage 1 only; stages 2 and later are not part of the acceptance.
**Proposed 2026-09-21**, and implemented on master the same day as stage 1
(§30.4) for validation on unit A. It becomes normative on the same terms as
the rest of this document when the owner accepts it after that gate; until
then the implementation is what is described here and nothing else. Nothing
in §1–§29 is renumbered. §7, §9, §10 and §21.3 point here where they said
the status bar is 56 px.

**Why.** The 56 px status bar is 4.5 % of the height in portrait and 10 % in
landscape, where it sits above a 72 px header and 44 px of body padding and
leaves 396 px of body on a 568 px panel - 100 px with the keyboard up.
Landscape is what §21–§29 were written to make usable, and this is the
cheapest pixel in it.

### 30.1 The three chromes

- **FULL** — the status bar of §7 and §9: 56 px, four cells, the bottom
  hairline, the §21.1 insets. Unchanged.
- **COMPACT** — the same bar at 32 px: the same four cells in the same order,
  the same caption type, the same insets and hairline. Nothing is dropped and
  nothing moves horizontally. Only the height changes, and with it the radio
  chip: 24 px tall (36 in FULL), the 14 px caption centred in it by 5 px of
  vertical padding; the §7 chip style itself is not changed. (§32.4,
  accepted: 26 px, and in FULL too its text centred by the line height of
  the font it is drawn in - the 24 px chip clipped `RX`.) 32 was more than
  the 30 px corner squares of §21.1 when this was written. Since the
  landscape top corners became 50 px (§21.1, 2026-09-23) it is not: the
  first 18 px of what sits under a COMPACT bar lie in the corner band, and
  the headers do not inset for it. Unit A showed nothing of an app header cut
  under COMPACT with 50 px corners (the §31 gate ran with them as a bench
  override, the v0.0.11 RC1 gate with them as the default); a header that
  reached further into the corner would need the §21.1 inset.
- **NONE** — no bar. The bar's objects exist and keep being written (clock,
  chip, hint), so the §9 hint API and the radio poll are unchanged, but
  nothing of it is drawn and the content area starts at the top edge. An app
  header directly under the top edge then runs corner to corner and takes the
  §21.1 bar insets exactly as the bar does: on the T-Display K230 its side
  padding becomes 30 px. Defined here; the fullscreen apps of §30.8 use it.

### 30.2 Who decides

- **The shell owns the chrome**: its lifecycle, its height, and everything
  that follows from the height - the content area, the launcher's column
  count, the keyboard reserve. All of it is derived from the chrome in force,
  in one place (`ui/shell/chrome.h`), never from a constant.
- **An app declares; it does not manipulate.** `struct pocketos_app` carries
  one field, `chrome`: DEFAULT (the shell's choice for the orientation),
  FULL, COMPACT or NONE. The shell resolves it against the orientation
  *before* the app is created, so the body an app is created in is its final
  one and its first layout pass is its only one. An app never reads the
  bar's height and never sets it; it lays out in the body it is given, as
  §21.3 already requires. Changing a screen's chrome must never require an
  app to know a pixel of it.
- **The launcher is FULL in every orientation.** Home is where the wordmark,
  the clock and the radio chip belong, and the §21.3 grid was chosen for the
  height below a 56 px bar. Coming home from any app restores it.
- **The chrome is invisible to an app.** The hint, the clock and the radio
  poll continue under every chrome. An app that writes a hint under NONE is
  not wrong, it is merely not seen - which is why an app that relies on the
  hint (Fleet's turn, Radar's run state, Timber's play state, Wave's MIC ON)
  must not declare NONE until that text has a place of its own in its body.

### 30.3 Resolution

| Screen | Portrait | Landscape |
| --- | --- | --- |
| Launcher | FULL | FULL |
| App declaring DEFAULT | FULL | COMPACT |
| App declaring FULL | FULL | FULL |
| App declaring COMPACT | FULL (stage 1, §30.4) | COMPACT |
| App declaring NONE | NONE (stage 2, §30.8; FULL in stage 1) | NONE |

### 30.4 Staged rollout

The portrait column above is a rollout stage, not an architectural rule: it
is the state of the implementation, and each stage is lifted by its own
change and its own gate.

- **Stage 1 (this amendment).** Portrait is FULL for every screen whatever an
  app declares, so every portrait screen is pixel-identical to v0.0.10.
  Landscape is COMPACT by default. One app declares: **Fleet declares FULL**,
  because the wide shape of §28 was constructed for the 386 px body under the
  56 px bar and does not hold at the 410 px under a 32 px one - the cell
  grows to 36, the board widens to 54 across, the readout column loses 30 px,
  the four one-square nudges fall to 60 px (under the 64 px minimum of §7),
  and in Outdoor the log line overflows the column by 10 px at turn 37 of a
  match, which §28.6 forbids. All of it measured in `tests/fleet_app_test.c`
  before this stage shipped. Fleet's stage 2 change re-derives §28 for the
  taller body at the same time as it moves the COMMAND hint into its own
  body; until then Fleet keeps the bar it was validated under. No app is NONE.
  The infrastructure for NONE is complete and exercised in the simulator,
  and no screen on the panel uses it.
- **Stage 2.** After unit A has validated stage 1 (§30.7): apps opt into NONE
  in landscape, one per change, each first moving whatever it wrote to the
  hint into its own body. Notes, Fleet, Radar, Timber, in that order of need.
  (As built, §30.8: six apps at once, in both orientations, with the hint
  carried by the header rather than by each body.)
- **Stage 3.** COMPACT and NONE in portrait are evaluated on the panel. The
  stage 1 portrait rule is one line in `chrome_resolve()` and one group of
  checks in `tests/chrome_test.c`.
- The 72 px app header is not part of this amendment. It is the larger cost
  in landscape - RIFT spends 240 px of chrome before its first message - and
  is the next thing to bring under the same policy.

### 30.5 What it buys

Body frame below all chrome and padding, landscape 1232x568:

| Chrome | Frame | Above the keyboard |
| --- | --- | --- |
| FULL | 396 | 100 |
| COMPACT | 420 | 124 |
| NONE | 452 | 156 |

Portrait is unchanged: 1060 and 764.

### 30.6 Validation on the host

`tests/chrome_test` (make test): the resolver in both orientations and on the
launcher, the three heights, the content box under each chrome with and
without the keyboard, and that hiding the keyboard gives the box back to
exactly the foot under every chrome - no dead strip. `tests/chrome_shell_test.sh`:
the running simulator in both orientations - FULL at home and under every
app in portrait, COMPACT under every app in landscape and drawn so, FULL
again on coming home, open/close/reopen over IPC, the radio poll seeing a
radiod that starts under COMPACT, a hint drawn in the compact bar, and NONE
through the simulator's test hook with the header's back slab moved clear of
the corner. Every app test builds its frame from the same resolver, so an
app is tested under the bar the shell gives it.

### 30.7 Unit A gate (before stage 2)

The build's identity stated first. Both orientations: the launcher unchanged
in both; every app but Fleet in landscape under the 32 px bar with the
wordmark, the chip and the clock readable and clear of the corners, and the
app's header straight under the hairline; Fleet in landscape under the 56 px
bar it declares, its Battle screen exactly as §28 left it; the keyboard up in
Notes, Settings and Clock in landscape with the field and its caption in view
above it, and no dead strip after it hides; Radar's SCANNING, Timber's
STANDBY and Wave's MIC ON visible in the compact hint cell, Fleet's COMMAND
in its full one; coming home from each app restoring the 56 px bar; a
rotation change through Settings landing on the launcher with the right bar.
Portrait: any screen, pixel for pixel what v0.0.10 showed.

### 30.8 Stage 2: the fullscreen apps [ACCEPTED]

**ACCEPTED 2026-09-24** on the unit A gate of `a30678e`
(docs/hardware/FULLSCREEN_APPS_GATE.md, PASS), as the owner asked for when
the implementation matched this proposal. Proposed the same day on branch
`feat/fullscreen-apps`, at the owner's request. Six apps are fullscreen: **RIFT, Notes,
Wave, Fleet, Radar and Timber** declare NONE, and have no status bar in
either orientation. Every other screen - the launcher, Controls, the lock
and the other seven apps - keeps exactly the chrome §30.3 gave it.

- **Resolution.** The stage 1 portrait rule is narrowed, not lifted: in
  portrait an app that declares NONE gets NONE, and DEFAULT and COMPACT are
  still FULL. It is still one line in `chrome_resolve()`.
- **The header carries the hint.** Under NONE the shell's app header shows
  whatever the app writes with `pocketos_shell_set_status_hint()` at its
  right end, in the bar's caption type - Fleet's turn, Radar's and Timber's
  run state, Wave's `MIC ON`, a Notes storage error. It is the same call and
  the same text; an app does not know which of the two shows it. This
  replaces the per-app move of §30.2 and §30.4 for these six.
- **The lock.** The lock lies under the bar and shows its wordmark and chip.
  While it is engaged over a fullscreen app the bar comes back at the height
  an ordinary app has in that orientation (FULL in portrait, COMPACT in
  landscape), so the lock looks as it does over any other app; the content
  area underneath keeps the NONE box. The bar goes again the moment the
  opening lock starts to show the app through it (the open door beginning
  to fade), so the app is never seen with a bar over its header.
  `shell.info` reports the bar as drawn in `chrome.shown_height` beside the
  policy's `status_bar_height`.
- **What it buys.** The body frame is 1116 in portrait (1060 under FULL) and
  452 in landscape (420 under COMPACT, 396 under FULL); above the keyboard,
  820 and 156.

Per app, the smallest change that uses the height:

| App | Portrait | Landscape |
| --- | --- | --- |
| RIFT | flex layout, unchanged: the thread and the lists take the height | the same |
| Notes | the list and the editor field take the height; the field still ends 20 px above the keyboard | the field above the keyboard is 156 px (124 under COMPACT) |
| Wave | unchanged; more of the page before any scroll | unchanged; TRANSMIT is still below the fold, as before |
| Fleet | the tall shape, unchanged | §28 re-derived for the taller body: the cell down the board grows to 40 (34 under FULL), the cell across is held at 51 (`FLEET_CELL_ACROSS_MAX`), so the readout column, its nudges and the log line keep the width §28 measured |
| Radar | §29 unchanged; the page is 56 px shorter than the body | §29's rule, unchanged: the scope grows with the body |
| Timber | the viewport takes whatever the body has beyond the 1060 px it was measured in: 728 px (672), the controls on the foot as before | unchanged: the 672 px viewport, and the page scrolls as it did |

Validation on the host: `tests/chrome_test` (the resolver in both
orientations), `tests/chrome_shell_test.sh` (every app under the chrome it
declares in both orientations, the hint in the header, open/close/reopen of
all six over IPC, the lock over fullscreen Notes), and the app tests of the
six, which build their frames from the resolver. Unit A: PASS on `a30678e`
(docs/hardware/FULLSCREEN_APPS_GATE.md).

---

PocketOS Design System v0.1 — **STATUS: APPROVED FOR IMPLEMENTATION**
Amendment A (§17) approved 2026-09-10; C8 closed.
Amendment B (§18) approved 2026-09-11.
Amendment C (§19) accepted 2026-09-15.
Amendment D (§20) accepted 2026-09-15.
Amendment E (§21) accepted 2026-09-16.
Amendment F (§22) accepted 2026-09-16.
Amendment G (§23) accepted 2026-09-17.
Amendment H (§24) accepted 2026-09-17.
Amendment I (§25) accepted 2026-09-17.
Amendment J (§26) accepted 2026-09-17.
Amendment K (§27) accepted 2026-09-17.
Amendment L (§28) accepted 2026-09-18.
Amendment M (§29) accepted 2026-09-18.
Amendment N (§30) accepted 2026-09-23 (stage 1; v0.0.11 RC1 unit A gate, docs/hardware/V0.0.11_RELEASE_SMOKE.md).
Amendment N §30.8 (stage 2, the fullscreen apps) accepted 2026-09-24 (unit A gate, docs/hardware/FULLSCREEN_APPS_GATE.md).

## 31. Amendment O — The DOORS environment [ACCEPTED]

**ACCEPTED 2026-09-23 by the product owner** as the production design
direction of Doors, and normative from that date. Proposed 2026-09-22 on
branch `feat/doors-visual-refresh`; **hardware-validated on unit A on
2026-09-23 in both portrait and landscape** (build `3c3d2b5`: lock screen,
unlock gesture and door sequence, launcher, Controls, apps opened and closed
by touch, keyboard input while locked, the 50 px landscape corners, rotation
both ways, memory returning after unlock; docs/hardware/DOORS_VISUAL_REFRESH_GATE.md);
merged to master `36d216f`. It supersedes §14 C7 and §20's tile launcher;
§20's masks stay (they are the launcher's fallback, §31.3). Nothing in
§1–§30 is renumbered, and §30 is unchanged.

**What.** The shell's own screens - lock, launcher, Controls - are drawn in
the approved visual package (`docs/design/brand/doors-visual-pack-v1`,
direction B "Framed spaces"): its photographs, its dark glass panels, its
portal icons and glyphs, its warm white. Apps keep their theme and look
exactly as before; the environment is the threshold, the apps are the
spaces behind it.

### 31.1 Environment

- One fixed palette from the package (`pos_styles.c`, ENV_* roles): text
  `#eeeae2`, secondary `#c3c0b9`, glass `#161c20` at 56 % with a `#a0a8a4`
  hairline at 42 %, the nine package hues for per-app colour. Not the
  theme's: the art is the same in every theme. The display mode applies -
  Night dims and warms it by §8's night rule and darkens the photograph;
  Outdoor whitens the text and thickens the glass.
- On the environment the status bar keeps its height and cells (FULL,
  §30) but has no fill and no hairline, and no clock: every environment
  screen shows the time large. In an app the bar is §7's, unchanged.
- The wordmark is live text (the package has no standalone mark; §19's mark
  stays on System).

### 31.2 Groups

Shell-owned, by app id (`ui/shell/home_layout.c`); no app declares or knows
its group. CONNECTIONS: RIFT, Radio, Wave. WORKSPACE: Notes, Calendar,
Clock, Calculator. PLAY: Fleet, Radar, Timber. DEVICE: Settings, System. An
app the table does not name is shown under MORE; an empty group is not
drawn. The package's own nine categories (Mesh, Network, Tools, AI, Files,
Apps …) are not apps and are not shown; their glyphs and hues are reused.

### 31.3 Launcher

Header (time 64 px, date), one glass panel per group, a footer with Lock and
Controls (64 px). Portrait: panels stacked, four 124 px cells across.
Landscape: the panels side by side in one row, each exactly as wide as its
apps (87 px cells, 16 px labels), the row centred; with more apps than fit
at 84 px a cell, the panels wrap and the launcher scrolls - nothing is
shortened. A cell is the app's 96 px portal icon over its name; held, it
shows the package's focus mark (a bracket over the frame, a rule under the
name) in the app's hue. An app with no portal icon is drawn on the empty
portal with its §20 mask (or its text icon) in the environment's text
colour. No page, folder or second level: every app is one tap from home.

### 31.4 Lock screen

Engaged at every cold start, from Lock (launcher or Controls) and
`shell.lock`, and not when `lock_screen=0` is in settings.conf. A rotation
restart is the same session and keeps the lock exactly as it was: locked
stays locked (a door still opening counts as locked), open stays open. The
restart carries the state across explicitly (`DOORS_SHELL_RESUMED=locked|open`),
so the keyboard base mated or removed in a pocket cannot open the device. It shows the lock photograph, the time
(96 px) and date, and "Swipe up to open". It is **not security**: no code,
services keep running, `shell.open`/`shell.home` open it. It sits above apps,
launcher and keyboard and below the status bar and the alarm alert, so an
alarm can be stopped while locked. It takes the keys (focused, group
frozen). A swipe up of 140 px (100 landscape) opens it - the content follows
the finger, a short drag springs back, a tap only lifts the hint - as do
Enter, Space and Up. Opening: the closed door fades to the open door (0.3
s), which holds (0.22 s) with the package tagline, then fades to what is
underneath (0.32 s); reduced motion opens at once. Only image opacity is
animated: no layer, no blur.

### 31.5 Controls

The package's system menu, limited to real providers: Radio (radiod state,
opens Radio), Wi-Fi (netd `wifi.status`, opens Settings), Rotation (cycles
the stored mode), Display (cycles Normal/Night/Outdoor), Brightness (the
shell's backlight control; "Not available" without one), rows to Settings,
RIFT ("Mesh messages") and System ("About DOORS"), and Lock / Power (Power
opens System, whose power actions confirm). Bluetooth and Sound are in the
mock-up and not here: nothing provides them.

**Accepted 2026-09-25** by the product owner as it is, after the unit A gate
(docs/hardware/DEVICE_CONTROLS_GATE.md, build `09be665`); it replaces the
tiles of the paragraph above (feat/device-controls-diagnostics). Six tiles in
three rows - Wi-Fi and Bluetooth, LoRa radio and Battery, Rotation and
Display - then Brightness and Volume as two slider panels, then the rows and
Lock / Power. The LoRa radio tile replaces the Radio tile: a tap switches the
radio (`radio.set_enabled`) instead of opening the Radio app, and off to on
first asks "Connect an antenna before enabling the radio. / Transmitting
without an antenna may damage the RF output stage." with Cancel and Enable
radio (a glass dialog over a glass scrim). Bluetooth (sysd's controller list)
and Battery (sysd's power supply) are read-only tiles and say "Not
available" / "External power" on unit A. Volume is the system volume (the
speaker glyph mutes). They borrow the network and power glyphs until the DS
draws Bluetooth and battery glyphs. Landscape has no room at the foot for
three tile rows plus Lock / Power, so there Lock and Power move to the right
of the header row, the slider panels are 88 px and the rows 56 px. The
geometry is `ui/shell/controls_model.c`, host-tested for overlap and fit in
both orientations, and drawn on unit A's panel in both (VERIFIED 2026-09-25).
The owner accepted the antenna question as drawn, including the glass panel
letting the tiles behind show through its text.

### 31.6 Runtime art

`ui/assets/doors/*.bin`, installed to `/usr/share/doors/ui` and read by
`ui/shell/art.c` when a screen needs them (never compiled in): six
backgrounds (lock, open, home × portrait, landscape; RGB565, the panel's
own format, 1,399,564 bytes each, the package's static scrims baked in,
Floyd–Steinberg dithered) and thirteen 96 px RGB565A8 portal icons (27,660
bytes each). 8.76 MB in all. The B system glyphs (32 px A8) are compiled
in (`pos_glyphs.c`). Everything is generated by `tools/design/gen_doors_ui.py`
from the package (icons redrawn from its SVG, within 0.2/255 of its own
128 px export) and recorded in `MANIFEST.txt`; `tests/doors_ui_assets_test.sh`
holds art and sources together. Memory: the home photograph and the icons
stay loaded (1.73 MB); the lock and open photographs only while shown. A
missing or damaged file is logged and drawn around (plain background,
empty frame), never fatal.

### 31.7 Validation on the host

`tests/home_layout_test.c`, `tests/art_format_test.c` (make test),
`tests/shell_lock_test.c` and `tests/doors_shell_test.sh` (simulator:
lock/open lifecycle, twenty rounds with no art leaked, every app opens and
comes home, every icon pixel-exact where `shell.info` places it in both
orientations, no-art and frame-only fallbacks, a rotation restart keeps the
lock as it was - open or locked, through repeated restarts - a fresh start
locks; `tests/auto_rotation_shell_test.sh` the same across keyboard-presence
restarts).

### 31.8 Follow-ups (not part of the acceptance)

Recorded at acceptance; none of them blocks §31, and none is decided here.
Items 1-4 and 7 are answered by §32 (Amendment P, accepted 2026-09-23).

1. A DOORS-aligned default app theme, derived from the launcher and lock
   visual language (apps still default to Ice & Ember, §8).
2. Whether to keep the launcher's "Open a space" hint (§31.3).
3. Landscape launcher labels at 16 px against 20 px in portrait (§31.3).
4. Distinct glyphs for Display mode and Brightness in Controls; both use
   the package's sun today (§31.5).
5. Keyboard navigation on the launcher (there is none, as before §31).
6. The boot splash stays a separate task (§19; the package's boot image
   carries no wordmark, and U-Boot cannot draw live text).
7. The COMPACT landscape bar clipping the `RX` radio chip is unrelated to
   §31 and stays with §30's stage 1 follow-ups.

Amendment O (§31) accepted 2026-09-23; unit A visual gate PASS 2026-09-23 on `3c3d2b5`, portrait and landscape.

## 32. Amendment P — DOORS app theme and UI polish [ACCEPTED]

**ACCEPTED 2026-09-23** by the owner, on the v0.0.11 RC1 unit A gate
(`docs/hardware/V0.0.11_RELEASE_SMOKE.md`), which covered §32.8 in part: see that sheet.
**Proposed 2026-09-23** on branch `feat/doors-app-theme-polish`, validated in
the simulator and on the host before the device (§32.8). It answers §31.8
items 1-4 and 7 and changes nothing in §31's environment. Nothing in §1-§31
is renumbered.

**Why.** After §31 the launcher, lock and Controls are charcoal, warm white
and dark glass, and every app behind them was still Ice & Ember: blue-black
surfaces, an ice-blue accent on every primary button, selected segment and
focus outline. The threshold and the spaces behind it looked like two
products.

### 32.1 The `doors` theme

A sixth theme in `themes.json`, with the same 13 base tokens as the other
five and every derived and mode value by §4 and §6:

| token | `doors` Doors | reason |
| --- | --- | --- |
| bg | #0b0b0a | near-black with a warm cast, not Ice's blue-black |
| surface | #161513 | charcoal |
| surface_raised | #211f1c | one step up, for rows and pressed slabs |
| line | #36332e | a warm hairline |
| text_primary | #eeeae2 | the package's warm white (§31.1): app text and launcher text are one colour |
| text_secondary | #a8a399 | warm grey |
| accent_primary | #d8bf94 | pale sand, between the package's Files and Apps hues: the one warm fill in an app |
| accent_secondary | #b9b3a6 | stone |
| status_ok | #57c785 | Ice's, unchanged: status means the same in every theme |
| status_warn | #e3b341 | Ice's |
| status_error | #e5534b | Ice's |
| radio_rx | #8fc1e8 | a softened sky blue |
| radio_tx | #ee9960 | a softened ember |

Contrast (`themes.json → themes.doors.contrast`): Normal - text 16.4,
text_secondary 7.8, lowest accent/status/radio status_error 5.3,
text_on_accent at least 8.8; Outdoor - text 21, text_secondary 12.6, all
others at least 6.8; Night - text_primary 5.3, text_secondary 3.5,
status/radio at least 2.8. Every §4 invariant and §13 target holds, and
`tests/theme_test.c` holds the engine to every value. RX and TX stay the two
distinct hues §13 asks for in every mode, apart from the accent's sand and
from status_warn's amber.

There is deliberately no gradient, glow or shadow: the depth is the three
tonal steps bg, surface, surface_raised and the hairline, as in every theme.
No new style mechanism exists for this theme; it is a table row.

### 32.2 Default and fallback

`doors` is first in the theme order and is the §8 fallback, and so the
default of a device that stores no theme; the image's `settings.conf` ships
`theme=doors`. Ice & Ember and the other four are unchanged and stay
selectable in Settings. A stored `theme=ice` is honoured: a device keeps Ice
until someone chooses otherwise.

### 32.3 Where it reaches, and where it does not

Every app screen, the dialogs apps build from the shared roles (§17.5),
the system alert (§18), the status bar inside apps and the keyboard follow
the theme, as they always have. The environment (§31.1) does not: its
palette is the art's.

Fleet, Radar and Timber keep their own identity. Their gameplay drawing is
unchanged: Timber's felt and pieces are art in every theme, and Fleet and
Radar draw their game signals in `radio_rx` and `radio_tx`, which Doors
keeps as a blue and an ember, so their signal language survives on the
charcoal. What reaches them is only what reaches every app: background,
status bar, cards, buttons and focus.

### 32.4 Chips

- **Active chip.** `POS_STYLE_CHIP_ACTIVE`, an `accent_primary` fill with
  `text_on_accent`, for a chip that means selected, on, running or up and is
  not the radio: Settings' SELECTED theme and connected network, System's
  running services and interfaces that are up. Those used the RX chip, which
  §4 gives to the radio alone; the misuse was invisible because in Ice (and
  Brass) `accent_primary` equals `radio_rx` - which also means those chips are
  pixel-identical in both. The radio's own chips keep their roles, and the
  games' chips their colours.
- **Centred text.** A chip is a label, and a label draws from the top of its
  content box: the §7 chip showed its caption at the top of 36 px. Its
  padding now comes from the line of its caption font, so the text is
  centred - in every chip that uses the §7 style, Radar's and Timber's state
  chips included (their captions move down to the middle; nothing else of
  them changes).
- **The status bar's radio chip** draws in the symbol font (a 22 px line),
  not the caption font. Its height and padding are derived from that line
  under each chrome (`chrome_chip_box`, `ui/shell/chrome.h`): FULL 36 px with
  the text centred by 7 px; COMPACT **26 px** (was 24) centred by 2 px. The
  §31.8 item 7 defect was 24 px less 2 × 5 px of padding: 14 px of content
  for a 22 px line, so the label clipped the top of `RX` - the simulator
  never showed it because without radiod the chip says `--`, which sits
  mid-line. The chip is never taller than its bar less a 2 px hairline.

### 32.5 Launcher

- **"Open a space" is removed** (§31.3, §31.8 item 2). In the simulator it
  sat alone in portrait, in the band between DEVICE and the footer, naming
  nothing on the screen - "space" is not a word the launcher uses anywhere
  else, and every cell already carries its app's name - and in landscape it
  sat in the footer row between Lock and Controls at the height of their
  labels, where it read as a third action that does nothing. The layout
  already dropped it whenever apps filled its band. The lock screen's "Swipe
  up to open" stays: that one is an instruction.
- **Landscape labels stay 16 px** (§31.8 item 3). Measured from the font: at
  20 px "Calculator" is 92 px wide and "Calendar" 81 px, against the 87 px
  landscape cell that twelve apps in four panels leave on a 1160 px line.
  20 px would shorten "Calculator", which §31.3 forbids, and crowd
  "Calendar" to 3 px of its cell edges; the cell cannot widen without
  wrapping the row. Portrait keeps 20 px in its 124 px cells.

### 32.6 Controls

Display (Normal / Night / Outdoor) has its own glyph, a disc half filled -
the usual display-mode sign - drawn first-party in the package's line
language (`docs/design/doors-glyphs/mode.svg`, 48-unit canvas, 2-unit round
strokes) and rasterised to 32 px by the renderer that reproduces the
package's own 32 px glyphs to within 3/255 mean alpha
(`gen_doors_ui.py --compare`). Brightness keeps the package's sun. Nothing
else in Controls changes, and no behaviour does.

### 32.7 Validation on the host

`tests/theme_test.c` (Doors against `themes.json` in all three modes, the
§13 thresholds and the sanctioned pairs), `tests/fleet_theme_test.c`,
`tests/chrome_test.c` (the radio chip for every line height from 10 to 26 px
under every chrome), `tests/chrome_shell_test.sh` (the RX chip drawn from a
real radiod poll has as many rows of ink under COMPACT as under FULL; RX, TX,
OFF and `--` at home, in System and in Fleet, in both orientations, each
with a whole line in a chip inside its bar), `tests/home_layout_test.c`,
`tests/doors_ui_assets_test.sh` (the glyph regenerates byte for byte), and
every app and shell suite. Contact sheets of every screen in both
orientations: `docs/design/doors-app-theme/`.

### 32.8 Unit A gate (before acceptance)

The build's identity first. If the device stores `theme=ice`, select Doors
in Settings (or `pos shell theme doors normal`). Both orientations: the
launcher without the hint; Controls with the new Display glyph beside the
Brightness sun; System, Notes, Clock, Calculator, Settings and RIFT under
Doors; a landscape app with the radio in RX and the compact chip's `RX` whole;
Fleet, Radar and Timber recognisably themselves. Outdoor and Night once
each; Ice selected and back.

Amendment P (§32) accepted 2026-09-23 (v0.0.11 RC1 unit A gate, docs/hardware/V0.0.11_RELEASE_SMOKE.md).

## 33. Amendment Q — Files [ACCEPTED]

**ACCEPTED 2026-09-23** by the owner, on the unit A gate of build `2bccc5b`
(`docs/hardware/FILES_GATE.md`), together with §33.4's landscape launcher:
DEVICE wraps to a second line and the launcher scrolls to Lock and Controls.
Step 8 of the gate (Enter on the keyboard base) was not run.
**Proposed 2026-09-23** on branch `feat/files-app`, validated on the host
(§33.5); not yet run on unit A and not accepted. It adds one app and changes
no existing screen; its only effect outside the app is on the launcher
(§33.4). Nothing in §1-§32 is renumbered. The app itself is described in
`docs/apps/FILES.md`.

### 33.1 Components

Only existing PocketUI parts and roles: `pocketui_card` panels, 64 px list
rows with the whole row as the hit area and `POS_STYLE_SELECTED` for the
selection, 56 px primary and secondary buttons with the §9 disabled
treatment, the Up slab (72 x 56, `POS_STYLE_SLAB`, the back slab's size),
the single-line text field and the shell's keyboard (§17), and the §17.5
confirmation (Cancel accented). A row is a symbol glyph (folder in
`accent_primary`, file in `text_secondary`), the name in the row-title role
with an ellipsis, and a caption line (size or type, and the time). No new
role, token or colour.

### 33.2 Tall (portrait)

One column in the body: the path bar (Up, then the path shortened from the
front so its end stays visible), Sort and New folder side by side, the list
(the only thing that scrolls), a caption status line (what just happened, or
why this place is read-only), and Open, Rename, Copy, Move, Delete across the
foot in one row. While a file is carried the foot shows what is carried,
Paste here and Cancel instead. Nothing reaches into the corner squares
(§21.1, via `pocketui_layout_begin`).

### 33.3 Wide (landscape)

Chosen when the list keeps at least the portrait body width (528 px) beside
a 420 px details pane. Sort and New folder move up into the path bar; the
pane shows the selected entry's name (two lines, then an ellipsis), type and
size, modification time and access, with Open across the pane and the other
four actions in pairs under it (or the carry bar). The name entry puts the
field, Cancel and Create in one row, which is what fits above the keyboard;
the confirmation keeps the portrait width, centred.

### 33.4 Launcher

Files joins **DEVICE** (Settings, System, Files) in the files hue, with the B
package's `files` glyph in its portal (`icon-files.bin`, §31.6) and the icon
extension's `files` mask as its §20 fallback. That makes thirteen apps. In
portrait DEVICE still fits one row of four. In landscape thirteen cells in one
row would be 81 px, under `HOME_CELL_MIN_W` (84), so by §31.3's own rule the
panels wrap: CONNECTIONS, WORKSPACE and PLAY on the first line, DEVICE on a
second, and the launcher scrolls (about 100 px) to Lock and Controls. §31's
"neither orientation scrolls" held for twelve apps. **Accepted by the owner
2026-09-23**: with thirteen apps the landscape launcher wraps and scrolls, by
§31.3's own rule; no smaller minimum cell and no other group for Files.

### 33.5 Validation on the host

`tests/files_fs_test.c`, `tests/files_view_test.c`, `tests/files_lint.sh`,
`tests/files_shell_test.sh` (with `tests/files_app_test.c`: every screen in
both orientations, touch targets and the corner safe area checked), and the
launcher and shell suites with thirteen apps (`tests/home_layout_test.c`,
`tests/doors_shell_test.sh`, `tests/chrome_shell_test.sh`,
`tests/display_geometry_shell_test.sh`, `tests/app_icons_test.sh`,
`tests/doors_ui_assets_test.sh`).

### 33.6 Unit A gate (before acceptance)

The build's identity first. Both orientations: the launcher with Files in
DEVICE (and, in landscape, the second line and the scroll to the footer);
Files opened, browsed from `/root` to `/` and back; a folder made, renamed,
a file copied, moved and deleted on the SD card; `/etc` and
`/var/lib/pocketos` shown read-only; a text file opened.

Amendment Q (§33) accepted 2026-09-23 (Files unit A gate, docs/hardware/FILES_GATE.md), with the landscape launcher wrap of §33.4.

## 34. Amendment R — Camera [ACCEPTED]

**ACCEPTED 2026-09-25** by the owner, after the unit A gate
(`docs/hardware/CAMERA_GATE.md`, PASS: both orientations, the picture upright
and not mirrored, capture, review, keep and delete, the keyboard base
alongside). **Proposed 2026-09-24** on branch `feat/camera-app-design`,
validated on the host (§34.5). It adds one app and
changes no existing screen; outside the app it adds a fourth cell to DEVICE
on the launcher (§34.4). Nothing in §1-§33 is renumbered. The app is described
in `docs/apps/CAMERA.md`, its architecture in ADR-006 (accepted 2026-09-25).

### 34.1 Components

Only existing parts and roles. The picture sits on a `POS_STYLE_SLAB` box
(the surface fill shows while there is no picture); in its place, a panel of
`POS_STYLE_TITLE` and centred `POS_STYLE_TEXT_SECONDARY` says what is
happening. TAKE PHOTO is a primary button, 240 x 96: the one control that
matters is the largest. DELETE/KEEP, CANCEL/DELETE and TRY AGAIN (CHECK AGAIN
with no camera) are the §7 secondary and primary buttons at 64 px, with the §9
disabled treatment. The last photo is a 72 x 72 slab (pressed:
`POS_STYLE_SLAB_PRESSED`) holding a 64 x 64 thumbnail. One status line under
the picture in `POS_STYLE_TEXT_SECONDARY`, switched to
`POS_STYLE_STATUS_WARN_TEXT` for a stall, a capture in progress and a note.
No new role, token or colour.

Fullscreen (§30.8): the app declares NONE. The shell's header carries the
back slab and, at its right end, the hint: `SIMULATED` whenever the pictures
come from the fake backend, so a test pattern is never mistaken for a camera.

### 34.2 Tall (portrait, 528 x 1116)

The picture across the full width, in the photo's own shape (9:16, 528 x
938): what the preview shows is what is saved. Under it the status line, then
the shutter centred, the last photo to its left. In review the two buttons
share the shutter's row, each half the width. When the body is shorter the
picture gives way and the controls keep their 156 px.

### 34.3 Wide (landscape, 1192 x 452)

The picture at the full height, 16:9 (802 x 452), on the left. A column on
the right: the status line at the top, the last photo, the shutter in the
middle; in review the two buttons stacked where the shutter was; TRY AGAIN
in the upper button's place.

The shape is chosen from the body (§21.2); the picture's shape follows the
way the unit is held, because the sensor is fixed to it. Every control is
inside the box PocketUI's corner rule leaves (§21.3, `pocketui_layout_begin`).

### 34.4 Launcher

Camera is the fourteenth app: DEVICE, after Files, in the `tools` hue, with
the Doors icon-extension camera glyph (mask and portal art generated like
Wave's and Files'). In landscape DEVICE already sits on its own second line
(§33.4); it gains a fourth cell there. Portrait is unchanged but for the cell.

### 34.5 Validation on the host

`tests/camera_layout_test.c` (both shapes, corners, touch targets),
`tests/camera_state_test.c`, `tests/camera_shell_test.sh` with
`tests/camera_app_test.c` (every state in both orientations, touch targets
and the corner safe area checked, the real shell with the live test pattern
on screen), `tests/camera_lint.sh`, and the launcher and shell suites with
fourteen apps.

### 34.6 Unit A gate (before acceptance)

Only once a real camera backend exists (CAMERA_PLATFORM_RESEARCH.md §9). The
build's identity first. Both orientations: Camera in DEVICE; the preview
upright and not mirrored (U4) at a steady rate; a photo taken, kept,
reviewed, deleted; the file on the card exactly when the screen says so; no
camera (with the ISP daemon stopped) and back; leaving mid-capture; the
header without SIMULATED.

Amendment R (§34) accepted 2026-09-25 (Camera unit A gate, docs/hardware/CAMERA_GATE.md).

## 35. Amendment S — Zabbix [PROPOSED]

**PROPOSED 2026-09-25** on branch `experiment/zabbix-dashboard`, as an
experiment. It is validated on the host only (§35.5); it is not accepted and
not merged. It adds one app and changes no existing screen. Nothing in
§1-§34 is renumbered. The app is described in `docs/apps/ZABBIX.md`, its
architecture in ADR-007 (PROPOSED).

### 35.1 Components

Only existing parts and roles:

- **Tabs:** OVERVIEW, PROBLEMS, HOSTS and STATUS in RIFT's section-strip
  form (docs/apps/RIFT.md): `POS_STYLE_CAPTION` labels and the accent 2 px
  underline. Each tab takes an equal share of the width and is 64 px tall,
  a full §7 target; a tab only as wide as HOSTS would not be.
- **Cards:** `pocketui_card` panels. The worst open severity is a
  `POS_STYLE_HERO_40` word, and the per-severity counts use
  `POS_STYLE_VALUE` over `POS_STYLE_CAPTION`.
- **Lists:** rows are at least 64 px and carry the `POS_STYLE_DIVIDER` rule,
  pressed as `POS_STYLE_SLAB_PRESSED`. Every line is a one-line label cut
  with dots, so a row never grows.
- **Banner:** a `POS_STYLE_SLAB` box over every tab, shown only when the
  data is not simply fresh.
- **Buttons:** the §7 primary and secondary buttons.

**Severity, availability and connection** are the colour-only status text
roles (`POS_STYLE_STATUS_ERROR_TEXT`, `_WARN_TEXT`, `_OK_TEXT`) laid over the
label's own font role. They always go with a word (§2):

- DISASTER and HIGH: error.
- AVERAGE and WARNING: warning.
- INFO and N/C: secondary.
- DOWN: error; UP: OK; UNKNOWN: secondary.

No new role, token or colour is added.

**Fullscreen (§30.8):** the app declares NONE. The header carries BACK and,
while the data is made up, the hint `SIMULATED`.

### 35.2 Tall (portrait, 528 x 1116)

- **OVERVIEW** is a column of three cards, top to bottom:
  - the headline;
  - the severities, three by two;
  - the counts, the server and the update time.

  The three most severe problems follow as rows.
- **Lists** take the full height under the strip and their heading line (the
  count and how fresh it is).
- **A host's detail** replaces its list, with BACK in its first row.

### 35.3 Wide (landscape, 1192 x 452)

- **OVERVIEW:**
  - the headline and the severities (six in a line) share the first line of
    cards;
  - the counts span the width below;
  - the page scrolls to the top problems.
- **Lists** keep the two-line rows at full width.

Every control is inside the box PocketUI's corner rule leaves (§21.3,
`pocketui_layout_begin`).

### 35.4 Launcher

**By default the launcher is unchanged.** Zabbix is built into the shell only
when it is configured with `-DPOCKETOS_WITH_ZABBIX=ON`, which neither the
Buildroot package nor any existing suite does. Built in, it has no place in
the launcher's table and no Doors icon yet, so it appears under MORE with the
eye symbol. A DS §20 icon and a group are owner decisions
(docs/apps/ZABBIX.md §12).

### 35.5 Validation on the host

- **App test:** `zabbix_app_test` (69 checks). It covers both shapes, every
  tab, touch targets and the corner safe area, the offline banner with the
  data kept, auth, unconfigured and demo, a crashed helper, a large estate
  bounded, long names on one line, and a page scrolled by a finger drag that
  starts on text (landscape STATUS).
- **Shell test:** `tests/zabbix_shell_test.sh`, where the real shell draws
  the disaster in the error colour in both orientations.
- **Lint:** `tests/zabbix_lint.sh`.
- **Screenshots:** `ZABBIX_SHOTS=<dir>` on the app test gives every screen
  in both shapes.
- **Unit A:** docs/hardware/ZABBIX_UNIT_A_GATE.md, PASS on `7c725ba`
  (2026-09-25), both orientations, with a real Zabbix 7.4.15 server. Every
  page is clickable, with nothing to click, so that a drag on text scrolls
  it; the gate found this in landscape STATUS.
