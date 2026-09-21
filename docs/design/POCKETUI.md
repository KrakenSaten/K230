# PocketUI design handoff (v0, SUPERSEDED)

Superseded on 2026-09-04 by `POCKETOS-DS-v0.1.md`, the approved Design
System. Kept for history only; nothing here is normative.

Status: v0 as implemented on 2026-09-04 in `ui/pocketui` and `ui/shell`.
This document is the contract between design work (Phase 2) and the code.
Anything changed here must be changed in `ui/pocketui/pocketui.c` too; the
simulator screenshots are the review medium (`docs/design/shot-*.png`).

## Canvas

- Panel: 568 x 1232 px portrait, 4.1" AMOLED, roughly 330 ppi. Design at
  1:1; the simulator shows it at 50 %.
- AMOLED: true black is free, large bright areas cost power and risk burn-in.
  The dark theme is therefore the base, not a variant.
- Outdoor use: the panel is bright, but reflections and gloves matter more
  than resolution. Prefer size and contrast over detail.

## Tokens (normal mode)

| Token | Value | Use |
| --- | --- | --- |
| bg | #0b0e12 | screen background |
| surface | #161b22 | status bar, cards, tiles |
| surface_hi | #1f2630 | pressed and highlighted surfaces |
| text | #e6edf3 | primary text |
| text_dim | #8b949e | labels, secondary text |
| accent | #2fd6c8 | cyan: primary actions, active radio state, icons |
| accent_2 | #8a7cff | violet: transmit state, secondary emphasis |
| ok | #3fb950 | success |
| warn | #d29922 | warnings |
| error | #f85149 | errors only, never decoration |

Rules: one accent per screen region; red is reserved for error state;
brightness carries meaning (brighter = more important), not colour alone.

## Layout constants

| Name | Value |
| --- | --- |
| status bar height | 56 px (FULL); 32 px COMPACT under an app in landscape; the shell decides (DS §30) |
| screen padding | 20 px |
| card / tile radius | 16 px |
| minimum touch target | 72 px |
| launcher grid | 2 columns, 20 px gutter, tiles 150 px high |
| app header | 72 px, back button 72 x 56 px at the left |

Type: Montserrat (LVGL built-in) at 14, 16 (default), 20, 24, 28, 32 px.
Values in key/value rows use 20 px; app titles 24 px; tile icons 32 px.
Fonts are bitmap in LVGL, so every size used must be enabled in
`ui/shell/lv_conf.defaults`.

## Components that exist

- Status bar: product name left, hint text, radio state with icon, clock.
- Launcher tile: icon top-left, label bottom-left, pressed state = lighter
  surface plus 2 px accent outline.
- Card: rounded surface with 20 px padding, vertical flex, 10 px row gap.
- Key/value row: dim label left, 20 px value right, value truncates with dots.
- Primary button: full width, accent background, dark text, 18 px vertical
  padding.
- App frame: header with back button and title, scrollable body.

Missing for v0.1 (design needed): toggle, list with dividers, dialog,
toast/notification, progress and signal meters, text input with on-screen
keyboard, empty state, error state.

## Modes (planned, tokens only)

- Normal: as above.
- Outdoor / high contrast: bg #000000, text #ffffff, accent stays cyan but
  larger type (default 20 px), thicker outlines, no dim text below #b0b8c0.
- Night / low light: bg #000000, all colours dimmed to roughly 40 %, accent
  shifted towards amber to protect night vision; no pure white.

Modes are a token swap at runtime; layouts do not change.

## Motion (planned)

From the charter, mapped to states the shell already knows:

| Event | Motion |
| --- | --- |
| LoRa TX (`radio.state` = tx) | outward pulse on the radio icon, 300 ms |
| LoRa RX (`radio.rx` event) | inward pulse, 300 ms |
| Connecting (network) | breathing at 1.5 s period |
| Error | 2 px horizontal shake, 200 ms, once |
| App open / close | 150 ms fade plus 8 px slide; no bounce |

Motion never blocks input, and every animation has a reduced-motion
equivalent (instant state change).

## Screens to design next

1. Settings (network, display mode, system, about).
2. Radio detail (profile editor with the region guard's limits visible).
3. Notifications and the crash-loop notice from `pos-supervise`.
4. First-boot and no-service states (radiod unavailable).

## Review loop

Designs are checked against the simulator: build the shell, run it with
`--open <app> --screenshot`, compare. Screenshots are the artefact reviewed,
not mockups, so the design source of truth is code plus this document.
