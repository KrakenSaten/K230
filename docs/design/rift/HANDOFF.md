# RIFT for Doors — implementation handoff

Status: APPROVED design direction, 2026-09-19. Design only; no LVGL code in this package.
Target: LILYGO T-Display K230 running Doors. Design System: `docs/design/POCKETOS-DS-v0.1.md`, tokens `docs/design/themes.json`.

## 1. Canvas and orientation

| Mode | Size | Hardware assumption | Input |
| --- | --- | --- | --- |
| Portrait | 568 × 1232 | no keyboard base | touch-first; Doors touch keyboard (296 px) slides over the bottom when the command line or a field takes focus |
| Landscape | 1232 × 568 | keyboard base attached | physical keyboard; no software keyboard, no reserved space, no composer slab; touch still works for selection and visible actions |

Orientation is a hardware mode, not a preference. Landscape is a recomposition (split panes, extra columns), never a rotation or stretch. Nothing exists in landscape that portrait cannot show; it is only visible at once.

## 2. Chrome

Portrait: status bar 56 · app header 72 (Doors back slab 72×56, title, right caption) · section strip 56 · command line 56 → 240 px chrome, 992 px data.
Landscape: status bar 56 · merged header+strip 56 (back slab 72×44, title 24/600 in a 120 px cell, four captions, right caption or segmented filter) · command line 56 → 168 px chrome, 400 × 1232 data.

Status bar and back slab are Doors-owned; RIFT changes nothing there. The RX ring on the radio chip is DS §12 motion, unchanged.

## 3. Navigation

Four sections, fixed order: `ACTIVITY · NODES · COMMS · NET`. COMMS merges direct messages, contacts and channels into one list; a channel is a row like a contact (`#` glyph).
Section strip: caption style (Mono 14, uppercase, 0.1 em), gap 32; inactive `text_secondary`; active `accent_primary`, weight 500, 2 px underline in `focus` on the strip's bottom rule. No pills, no fills. Unread count on COMMS is the unread pill (see §6).
Hierarchy: section → selected row (expanded, in place) → DETAIL screen (pushed, header caption "NODES › DETAIL"). In landscape the expanded row's content lives in the right pane instead.
Command line (bottom, 56 px): `›` in `accent_primary`, 2 px caret, placeholder in body Sans `text_secondary` naming the touch action ("Search nodes, or /msg /path /advert", "Message HYTTA"), right-aligned caption hint. Commands: `/nodes /msg <name> /path <name> /trace <name> /advert /join #ch`; free text sends to the open conversation. It is an accelerator: every action is reachable by touch without it. Landscape hint text lists keys: `TAB PANE · ↑↓ SELECT · ENTER OPEN · ESC BACK · / COMMAND`.

## 4. Sizes and touch (RIFT-DEV-1, approved)

| Element | Size |
| --- | --- |
| Data row (nodes, conversations, path history, hop table) | 36 px (hop table in landscape detail: 32) |
| Row header / group label | 28 / 24 px caption |
| Section strip, command line, segmented filter, action buttons | 56 px |
| Primary button (portrait detail bottom) | 64 px |
| Panel key/value row | 56 px |
| Screen horizontal padding | 20; landscape pane padding 20/16 |
| Panel | hairline `line`, radius 6, caption in the top rule at 16 px from left |

Conditions of RIFT-DEV-1: a 36 px row tap only selects; the full row width is the hit area; nothing destructive or immediate fires from a row; selection expands into a 56 px action bar (MESSAGE · PATH · DETAIL ›) or drives the landscape context pane; primary navigation and actions stay ≥ 56. The exception binds RIFT only.

## 5. Colour and theme

RIFT owns no colour. It reads Doors tokens through `pos_theme_color()` and declares one manifest alias: `rift_link → radio_rx`.

| Meaning | Token | Where |
| --- | --- | --- |
| focus, selection, active section, primary action, self marker ring | `accent_primary` / `focus` | outline 2 px, underline, primary button, on-path pills |
| heard direct, RX activity, unread pill fill, DIRECT word | `radio_rx` (alias `rift_link`) | glyph fill, hop count "DIR", route captions |
| relayed | `text_secondary` | hollow glyph, hop lines, hop count |
| unknown, not reported, not measurable, stale age | `text_muted` | dashed glyph, literal `?` or `—` |
| path changed, no ack, retry, backend reconnecting | `status_warn` | caption text only |
| radio service absent | `status_error` | error panel outline + caption (DS §9) |
| cached values while backend is down | `disabled_fg` | value + the word "cached" |
| unread pill text, text on filled chips/buttons | `text_on_accent` | |

Colour never carries meaning alone: every state has a glyph and a word. Verified in all five themes and ice/night (`shots/theme-proof.png`). Do not hard-code orange; carbon's accent happens to be orange.

## 6. Semantic vocabulary

Glyph (8 × 8, radius 2, in rows and captions):
- direct: filled `radio_rx`
- relayed: 1.5 px hollow `text_secondary`
- unknown path: 1.5 px dashed `text_muted`
- stale (not heard > 12 h): filled `text_muted`
- this device: filled `text_primary` with 2 px `focus` ring
- channel: `#` Mono glyph, `text_secondary`

State words, always in this order: **state · hop count · uncertainty**, e.g. `RELAYED · 9 HOPS · 1 UNKNOWN HOP`, `DIRECT`, `NO PATH`, `? HOPS`. Hop count column shows `DIR` (in `radio_rx`) for direct, a number for relayed, `?` for unknown.

Unknown data rule: never 0, never a bar, never "good". `?` in `text_muted` when the backend could report but has not (SNR, hops, RSSI). `—` when the value cannot exist (end-to-end RSSI over relays). Signal values are always labelled with the hop they belong to ("LAST HOP −88 · RPT-NORD").

Unread pill: Mono 14/500, `radio_rx` fill, `text_on_accent` text, padding 0 5, radius 2.
Message routing captions (per message, Mono 14): `RECEIVED · PATH 9 · 1 UNKNOWN HOP`, `DELIVERED · ACK 41 s`, `NO ACK · 3 ATTEMPTS · LAST 12:47` (warn), `RECEIVED · PATH CHANGED FROM 8 HOPS` (warn). Own messages carry a 2 px right rule in `accent_primary`; received carry a 2 px left rule in `text_secondary` (direct: `radio_rx`). No bubbles.

## 7. Path representation

The full hop list (hashes and resolved names) is stored and never truncated; only the rendering compresses.

- **Hop strip** (rows): 6 px squares joined by 1 px lines. Self filled `text_primary`; relay hollow `text_secondary`; unknown hop dashed `text_muted`; target filled (`radio_rx` if direct, else `text_secondary`; `text_muted` if stale). Direct = self — line in `radio_rx` — target. More than 4 relays: first two relays, `+n` (Mono 12), last relay. The numeric hop count is always printed in its own column beside the strip. Widths: 112 px portrait, 150 px landscape.
- **Inline chain** (expanded row, context panes, comms header): Mono 14, `K230 › RPT-NORD › 7f › ? › RPT-7 › c2 › 4a › e1 › 0b › HYTTA`; self and target `text_primary`, direct hop `radio_rx`, unknown hop `?` in a dashed `text_muted` box; wraps with `overflow-wrap:anywhere`.
- **Hop ladder** (portrait detail): 40 px rows, vertical rail; per hop: index, glyph, name (Sans) or hash (Mono), note, evidence. Unknown hop row shaded `surface`; rail segments touching it in `text_muted`.
- **Hop table** (landscape detail): 32 px rows, columns `# · HOP · KIND · EVIDENCE · SIGNAL · HEARD`. KIND ∈ SELF · DIRECT · RELAY · UNKNOWN · TARGET.
- **Horizontal chain** (landscape NET): 96 px columns per hop: index, pill, kind, evidence; connectors 1 px solid, dashed on either side of an unknown hop.
- **Hop rings** (NET): ring = observed hop count from this device, not distance. Portrait: 68 px rows per ring (0 SELF, 1 DIRECT, 2…9, `?` NO PATH), node pills 26 px; on-path pills `surface` fill + `accent_primary` border; rail 2 px in `accent_primary`, `text_muted` through unknown rings. Landscape: rings as columns under the horizontal chain, path nodes removed from the columns (they are in the chain). Only observed paths are drawn; no inferred links.
- **Path history**: 36 px rows `age · hops · via … · tag` (`current` `text_primary`, `changed` `status_warn`).

## 8. Shared components (implementation inventory)

Reused from PocketUI unchanged: status bar + radio chip, app header + back slab, hairline panel with caption in rule, key/value row, segmented control (56), primary/secondary/emphasis buttons, inline action (44), toast, empty state, error panel, text field, focus outline.

New, RIFT-scoped, all built from LVGL primitives and token roles:
- `rift_section_strip` (56; landscape variant merged into header row)
- `rift_cmdline` (56; prompt, caret, placeholder, right hint; is the composer in landscape)
- `rift_link_glyph` (8 px; direct/relayed/unknown/stale/self)
- `rift_hop_strip` (compress rule first-2 · +n · last)
- `rift_node_row` (36; glyph · name · role · unread · strip · hops · rssi [· snr] · heard; selected = 2 px focus outline, `surface` fill, radius 6, 12 px inset, 56 px action bar)
- `rift_group_label` (24; "HEARD < 12 H · 12", "NOT HEARD > 12 H · 16")
- `rift_hop_histogram` (4 bins, 6 px tracks, unknown bin dashed)
- `rift_airtime_bars` (activity; direct in `radio_rx`, relayed `text_secondary`)
- `rift_state_line` (glyph + 20 px Mono state word + muted uncertainty + right warn tag)
- `rift_path_chain_inline`, `rift_path_ladder`, `rift_hop_table`, `rift_path_chain_h`, `rift_hop_rings`, `rift_path_history`
- `rift_msg_line` (time · glyph · sender · route caption; body; state caption; left/right rule)
- `rift_conversation_row` (36; glyph · name · preview · unread · route)
- `rift_unread_pill`

## 9. Responsive behaviour (summary of `shots/responsive-rules.png`)

Same in both modes: navigation, tokens, type, row semantics, touch rules, glyphs, state words, path data.
Portrait: single column, stacked panels; detail via expanded row or pushed screen; touch keyboard over the bottom on focus; comms thread has a 56 px field + SEND for touch.
Landscape panes (border-box): NODES 660 list / 572 context · NODE DETAIL 400 identity+link+history+actions / 832 hop table · COMMS 372 list / 560 thread / 300 route context · NET full-width chain over ring columns. Extra columns: SNR, 150 px strip, KIND/EVIDENCE/SIGNAL/HEARD. Command line always visible, is the composer, shows key hints. One Doors focus group: TAB moves pane, arrows move selection, ENTER opens, ESC back; focus ring = the same 2 px outline.

## 10. Keyboard assumptions

Portrait: no physical keyboard; Doors touch keyboard per DS §17.3 appears on focus of the command line or a text field; Done/Enter sends or runs the command; the command line stays visible above the sheet.
Landscape: physical keyboard present; the key stream enters the one Doors focus group (DS §17.4); no software keyboard is ever shown; `/` from a list focuses the command line; typing in COMMS goes to the open conversation.

## 11. Decisions left to code

1. Sort and grouping thresholds: 12 h stale boundary, "fresh" ordering by last heard, group counts.
2. Hop-strip compression threshold (design: compress when relays > 4) and exact +n rendering when a compressed segment contains the unknown hop.
3. Path-change detection and warn duration (design shows "CHANGED 2H" until the next advert confirms).
4. Whether `/trace` exists in the backend; the NET "RELAY LOAD" filter is named but not designed.
5. Ring membership when a node has several observed paths (design: last observed path).
6. Per-message SNR/RSSI on channels: hidden by default, tap a line to reveal (portrait channel screen, turn 1).
7. Behaviour of the command line when the backend is reconnecting (design: input queued, disabled_fg treatment).
8. Portrait keyboard-up layout and ACTIVITY landscape follow the rules in §9; not mocked.
