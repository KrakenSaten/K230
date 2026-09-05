# Design System v0.1: technical feasibility review

Date: 2026-09-04. Reviewed against PocketOS 0.0.1 (`ui/pocketui`,
`ui/shell`, `apps/*`) and LVGL at the pinned commit 59dc7e4. Inputs:
`POCKETOS-DS-v0.1.md` (normative), `themes.json` (spot-checked against the
§4 and §6 rules: `ice` derived and mode values reproduce exactly),
`PocketOS Design System.html` (viewed in a browser; fonts fall back because
the bundle references font assets that are not embedded).

No code was changed for this review.

## 1. Verdict

Feasible on LVGL 9 with two required deviations (fractional stroke widths
and the "Spectrum" wording) and one open licensing check (IBM Plex reserved
font name). The theme engine as specified maps directly onto LVGL shared
styles plus `lv_obj_report_style_change`. The biggest cost is not the engine
but rebuilding every existing component and app on shared styles, because
the current code sets colours as local styles, which the live switch cannot
reach. Estimated effort: three focused steps on the simulator, one hardware
verification pass.

## 2. What already matches

| Area | Status |
| --- | --- |
| Canvas 568x1232, dark base, screenshot-driven review loop | matches |
| Status bar 56 px, header 72 px, back slab 72x56, screen padding 20, launcher 2 columns 150 px tiles | matches |
| Panel radius: current 16 px | must become 6 (§7) |
| Semantic naming of colours (`tokens.accent`, `text_dim`) | concept matches, names and count do not |
| Status bar radio chip driven by `radio.status` | matches; states OFF and NA need adding |
| Apps use no hardware and no theme-specific constants | matches in spirit; they call `pocketui_tokens()` and set local styles, which §8 forbids |
| Motion events available (`radio.state`, `radio.rx`, app open/close) | matches; no animation code exists yet |

## 3. What needs to change

- Token model: 10 ad-hoc tokens become the 13 base + 7 derived tokens of §4,
  with `hairline_px` and `type_default_px`. New module `ui/pocketui/pos_theme.{c,h}`.
- Style architecture: one shared `lv_style_t` per token role (background,
  panel border, text primary, text secondary, caption, value, chip RX/TX/OFF/NA,
  button primary/secondary/emphasis, meter on/off, …). Components add these
  styles; nothing calls `lv_obj_set_style_*_color` with a token value.
  `pocketui.c` currently does exactly that in 14 places and must be
  rewritten; `shell.c` (status bar, header, back button) and both apps
  (`radio_app.c`, `system_app.c`) as well.
- Typography: Montserrat is replaced by IBM Plex Sans and Mono. These are
  not LVGL built-ins; they must be converted with `lv_font_conv` to C
  files and committed under `ui/pocketui/fonts/`. Weight is baked into a
  bitmap font, so the §3 table needs 12 font files: Sans 16/400, 20/400,
  20/600, 24/600, 40/600, 48/600; Mono 10/400, 14/400, 16/500, 20/400,
  24/400, 32/400.
- Persistence: no settings store exists. Needs a small key/value file
  (`/etc/pocketos/settings.conf` on the device, `$POCKETOS_CONFIG_DIR`
  override for the simulator), read synchronously before the first frame.
- Screens: Settings, Radio detail, Notifications, First boot and Theme are
  new. The launcher stays and is restyled per C7.
- Components: toggle, segmented control, segmented meter, spectrum (see
  deviation D2), track slider, notification item, toast, empty state,
  progress dots, inline action, emphasis button are all new.

## 4. What is missing from the design (needs a decision, not a redesign)

- The launcher screen and the current System app are not in the reference
  set. C7 covers tiles; the System app can be expressed as a Settings-style
  panel list without new components.
- The `mini` 10 px mono style is used only in theme-row miniatures; C2
  already allows omitting the labels. Recommendation: omit, draw the
  miniature with primitives only.
- Notification data has no producer yet (no notification service). The
  screen can be built against a static model in the simulator; wiring to
  `pos-supervise` crash-loop markers is a later, separate step.
- Settings rows for Wi-Fi, Mesh, Hotspot, Storage, Battery, Firmware and
  Update reference services that do not exist (netd, updater). They can
  render with placeholder values marked as such; the rows themselves are
  fine to build.

## 5. Conflicts with the current architecture

None structural. Two adjustments:

- ADR-002 says apps talk to services over pocketipc and never touch
  hardware; the DS adds a second rule, apps never touch colours. This is
  consistent and should be added to `docs/ARCHITECTURE.md` when
  implemented, together with `POS_EVENT_THEME_CHANGED`.
- §8 "token alias declared in its manifest" presupposes an app manifest
  that does not exist (ROADMAP: "useful soon"). Until then the rule is
  simply "apps use shared styles only", enforced by review and by a grep
  test that fails on `lv_color_hex` outside `pos_theme.c`.

## 6. LVGL limitations and concerns

Verified in the pinned source:

- `lv_obj_report_style_change(NULL)` exists and refreshes all objects:
  live switching works as specified. Text letter spacing and line spacing
  are style properties. Dashed strokes exist in the line draw descriptor.
  A8 images can be recoloured through `image_recolor`, which gives the
  "currentColor" icon behaviour. `lv_theme_set_apply_cb` allows applying
  role styles by widget type.

Concerns:

- D1, fractional widths: LVGL borders and outlines are integer pixels. The
  DS uses 1.5 px for error outlines, toast outlines, inline actions and
  icon strokes. These MUST NOT be implemented as 1.5; use 2 px on the
  568x1232 panel. Icon strokes are pre-rendered as A8 bitmaps at the
  target size, so 1.5 px exists only in the source SVG.
- Letter spacing is integer pixels: 0.1 em at 14 px is 1 px, 0.08 em is
  1 px, hero −2 % at 48 px is −1 px, −1 % at 40 px rounds to 0. Visible
  difference to the reference is negligible.
- Uppercase captions: LVGL has no text transform; captions are uppercased
  in the component code at set-text time.
- Dashed panel border (empty state): no style property; implemented with a
  custom draw event drawing four dashed lines. Small, contained.
- Theme rows in their own theme tokens (§10.5): these cannot use the shared
  styles (which hold the current theme). The Theme screen owns five private
  style sets computed from the five Normal tables. This is the one place
  where the engine, not an app, uses per-theme colours directly; it is
  allowed by §8 because it is the engine.
- RGB565 rendering (the vendor path uses 16-bit colour): the darkest steps
  collapse. `bg` #06080b, `surface` #10151b and `surface_raised` #19212a
  quantise to 2-bit-apart values; Night `surface` #080b0e versus `bg`
  #000000 differs by one 5-bit step. Hairline-on-bg structure survives, but
  surface-versus-bg distinctions in Night mode may be invisible on the
  AMOLED. Hardware item H1. If it fails, the fix is a per-mode rule
  tweak (raise Night `surface` mix toward 0.35), not a redesign.
- Fonts in flash and RAM: 12 bitmap fonts, ASCII plus Latin-1, 4 bpp, are
  roughly 20 kB (10 px) to 200 kB (48 px) each, about 900 kB total in the
  shell binary. Acceptable on a 763 MB SD image; fonts live in `.rodata`,
  so RAM cost is only what is paged in.
- Performance: five themes times three modes is 13 tokens times 15
  tables, computed on demand in microseconds. Style change on switch
  re-layouts every object once; on a 568x1232 RGB565 surface this is one
  full redraw, well under 100 ms in the simulator, to be measured on the
  K230 (H2).

## 7. Semantic token and theme architecture review

The five themes are one system: every component references roles, and the
theme tables only supply values. This holds as long as the derived tokens
and mode transforms live in one function (`pos_theme_resolve(theme, mode)`)
and the JSON is the single source of the Normal tables. Recommendation:
generate a C table from `themes.json` at build time with a small Python
script (Python is already a build dependency through LVGL), and unit-test
`derive()`, `outdoor()` and `night()` against the JSON's precomputed
values so the two cannot drift. The §4 invariants and §13 contrast
thresholds become one native test over the same data.

## 8. Live switch, persistence, fallback, modes, inheritance, motion

- Live switch: shared styles rewritten, `lv_obj_report_style_change(NULL)`,
  then `POS_EVENT_THEME_CHANGED` broadcast (LVGL custom event on the screen
  plus a `shell.theme` pocketipc event so `pos` can drive it in tests).
  Instant, no cross-fade, as specified.
- Persistence: two keys, written on change, read before `lv_init` output.
  File I/O is synchronous and tiny.
- Fallback: parse failure, unknown id, or invariant failure logs a warning
  through pocketlog and uses `ice` + `normal`; stored value untouched.
  Boot never depends on appearance because the shell starts even if the
  settings file is unreadable.
- Modes: computed from the Normal table by rule; `type_default_px` swaps
  the body font pointer in the shared text styles, `hairline_px` the border
  width. Layout tolerance for 20 px body (C4) is verified by simulator
  screenshots of every screen in Outdoor.
- Component inheritance: implemented through role styles; a component
  variant (secondary button) adds a second style rather than overriding
  colours. Pressed and focus states use LVGL state selectors
  (`LV_STATE_PRESSED`, `LV_STATE_FOCUSED`) on the same styles.
- Motion: `lv_anim` on a ring object's size and opacity for TX/RX; an
  `lv_timer` toggling fill for Connecting; a 200 ms translate animation for
  shake; fade plus 8 px slide via `lv_obj_fade_in` and a translate anim on
  the app root. Reduced-motion is a settings key that makes the animation
  helpers set the end state immediately.

## 9. Deviations that should NOT be implemented as specified

- D1: 1.5 px strokes become 2 px (integer pixels, see §6).
- D2: the "Spectrum" widget. The SX1262 cannot produce a spectrum; the
  widget shows channel RSSI samples over time (or across the three fixed
  frequencies of the axis captions). The visual is unchanged, but the
  caption and code name should be "RSSI HISTORY" (or "CHANNEL RSSI"), per
  the charter rule that PocketOS never describes the SX1262 as a spectrum
  analyser. This is a wording change and needs the product owner's OK.
- D3: `hairline_px` applies to `line` borders only; the `surface_raised`
  dividers stay 1 px in all modes, otherwise Outdoor doubles every divider
  and the spec's "no hairline under 2 px" is read as structure only. Confirm
  intent.
- D4: the mini 10 px font (C2): omit the labels; ship 11 fonts, not 12.

## 10. Licensing

IBM Plex is OFL-1.1 with the Reserved Font Name "Plex". Bitmap conversions
are Modified Versions and MUST NOT be presented under the reserved name.
Symbols and file names in the code (`pos_font_sans_16`) are fine; the
Theme screen and About screen must not say "IBM Plex". OFL text goes into
`docs/legal/` with the converted fonts. `lv_font_conv` is MIT and a
host-only tool. Check status: DOCUMENTED from the OFL text, to be
confirmed against IBM Plex's own LICENSE.txt before conversion.

## 11. Hardware-dependent items

- H1: RGB565 quantisation of the dark steps on the RM69A10 (Night mode
  surfaces, Normal `bg`/`surface` separation).
- H2: full-screen restyle time on the K230 when switching theme.
- H3: touch target size 64 versus 72 with gloves (C1).
- H4: AMOLED brightness versus Outdoor contrast in sunlight; Night
  `status_error` visibility (C3).
- H5: whether 1 px hairlines render crisply through the vendor DRM path
  and rotation (the vendor patches GDMA rotation; a 1 px line may become
  soft if scaled).

Everything else is verifiable on the simulator with screenshots.

## 12. Affected files and modules

New: `ui/pocketui/pos_theme.{c,h}`, `ui/pocketui/pos_styles.{c,h}`,
`ui/pocketui/fonts/*.c` (generated), `ui/pocketui/icons/*.c` (A8),
`ui/pocketui/widgets/*.c` (toggle, segmented, meter, rssi_history, slider,
toast, notification_item, empty_state, progress_dots),
`ui/shell/settings.{c,h}` (persistence), `apps/settings/`, `apps/theme/`,
`apps/notifications/`, `apps/firstboot/`, `tools/design/gen_theme_table.py`,
`tools/design/gen_fonts.sh`, `tests/theme_test.c`, `tests/contrast_test.py`.

Changed: `ui/pocketui/pocketui.{c,h}` (rewritten on shared styles),
`ui/shell/shell.c` (status bar cells, chip states, header, motion hooks,
`shell.theme` IPC), `ui/shell/lv_conf.defaults` (fonts), `apps/radio`,
`apps/system`, `docs/api/shell.md`, `docs/ARCHITECTURE.md`,
`docs/design/POCKETUI.md` (superseded notice), `docs/LICENSING.md`.

## 13. Implementation plan (small steps, each ends green on the simulator)

Simulator now:

1. Theme engine core: `pos_theme` with base tokens, `mix`, `derive`,
   `outdoor`, `night`, invariants; generated C table from `themes.json`;
   `tests/theme_test.c` reproducing every value in the JSON. No UI change.
2. Fonts: convert IBM Plex (11 files), swap `lv_conf.defaults`, add OFL to
   `docs/legal/`. Screenshots to confirm rendering.
3. Shared styles and live switch: `pos_styles`, `pos_theme_color()`,
   `lv_obj_report_style_change`, `POS_EVENT_THEME_CHANGED`, `shell.theme`
   IPC (`pos shell theme <id> [mode]`). Rewrite `pocketui.c`, status bar,
   header, launcher tiles (C7) and the two existing apps on shared styles.
   Add the grep test that forbids colour literals outside `pos_theme.c`.
4. Persistence and fallback: settings file, read-before-first-frame,
   corrupt-file test, unknown-id test.
5. Components batch 1: panel with caption-in-rule, key/value row 64,
   primary/secondary/emphasis/inline buttons, chips with four states,
   toggle, segmented control, pressed outline rule.
6. Screens: Settings (10.1) and Theme (10.5) including per-row private
   styles. Screenshots for five themes and three modes.
7. Components batch 2: segmented meter, RSSI history (D2), track slider,
   toast, empty state, progress dots, notification item.
8. Screens: Radio detail (10.2), Notifications (10.3) on a static model,
   First boot (10.4). Screenshot matrix: five themes times Radio, `ice`
   times three modes, compared with reference 3a to 3f.
9. Motion table and reduced-motion switch; headless test asserts end
   states only.
10. Contrast and invariant test in `make test`; docs updates; ROADMAP.

After hardware arrives:

11. H1 to H5 on the panel; adjust Night rules or touch sizes per findings
    (rule tweaks only, no visual language change).

## 14. Test plan

- Unit: `theme_test` (every JSON value, invariants, fallback paths, mix
  rounding); `contrast_test` (thresholds §13 per theme and mode).
- Static: forbid `lv_color_hex`, `lv_color_make`, `lv_palette_*` outside
  `pos_theme.c`; forbid direct font symbols outside `pos_styles.c`.
- Headless UI: extend `tests/shell_ipc_test.sh` with `pos shell theme`
  switching and screenshot capture for each theme and mode; check the PNG
  is produced and that `shell.info` reports the active theme and mode.
- Visual: screenshot matrix committed under `docs/design/shots/` and
  compared by eye against 3a to 3f; later a pixel-diff gate once the
  screens are stable.
- Hardware: H1 to H5 with `pos shell screenshot` on the device and a
  photograph of the panel for each mode.

## 15. Unresolved issues (need the product owner)

- D2 wording for the "Spectrum" widget.
- D3 divider width in Outdoor.
- C1 touch target 64 versus 72: implement 64 now, revisit after H3.
- Whether the launcher keeps tiles (C7) or becomes a list; recommendation:
  tiles as specified in C7.
- Font licence confirmation (§10) before conversion.
- Settings store location and format (recommend a flat `key=value` file
  under `/etc/pocketos/`, no new dependency).

## 16. Recommendation

Proceed, in the order above, with steps 1 to 4 as the first batch. They
change no visuals yet, are fully testable, and remove the structural
blocker (local colour styles) before any screen is built. Steps 5 to 10
follow once D2 and D3 are decided.
