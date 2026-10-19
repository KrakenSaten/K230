# Neon — optional system-wide DOORS theme

**Implemented in PR #98 following the owner decision of 2026-10-19.**
Select **Settings → Appearance → Neon**. Select Doors or any other listed
theme to switch back. The default remains Doors.

The implementation uses the existing theme registry, semantic tokens,
settings persistence and live change notifications across DOORS. It introduces
no Activity layout, navigation or routing changes. See the [palette,
implementation notes and rendered simulator evidence](../../neon/README.md)
and [design-system amendment §58](../../POCKETOS-DS-v0.1.md#58-optional-system-wide-neon-theme-accepted).

## Historical inspiration — 2026-10-17

The original proposal below records the exploration **as it stood on
2026-10-17**. Its RIFT-only preference and requests for future approval were
superseded by the owner's system-wide theme decision above. The original
[SVG artwork](neon-activity-landscape.svg) is preserved byte-for-byte. It is a
concept with fictional telemetry, **not simulator implementation evidence**.

---

# RIFT Neon Activity — landscape inspiration

**Status: EXPLORATION ONLY — NOT APPROVED / NOT IMPLEMENTED.**  
Date: 2026-10-17  
Target concept canvas: **1232 × 568 landscape** (LILYGO T-Display K230).  
Visual reference: [Neon Activity mockup](neon-activity-landscape.svg).

This proposal explores a distinct, high-contrast **Neon** visual language for RIFT, potentially usable across ACTIVITY, NODES, COMMS and NET. It is an inspiration piece, not a new binding design contract. The existing [approved RIFT handoff](../HANDOFF.md) and [Doors design system](../../POCKETOS-DS-v0.1.md) still take precedence.

## Direction

- AMOLED-friendly near-black background with restrained luminous cyan, green and amber/orange accents.
- Landscape-first information hierarchy: service state at a glance, recent air traffic, recently heard nodes, event activity and a clear RX LOG entry point.
- Dense but readable panels; text and icons carry meaning in addition to color.
- Use accent on high-value data and selected controls. Avoid lighting every border and creating distracting glow everywhere.
- Extend a cohesive **optional RIFT theme** across all sections only after product-owner review. Do not silently change the global Doors theme.

## What the current code can support

| Concept element | Grounding and limitation |
| --- | --- |
| Radio service, radiod/lease and TX availability | RIFT service status model; only display fields known to be reported |
| Nodes held | Service-reported stored node count; not the number currently online |
| Repeaters | The existing 0-hop scan and its retained answers; not a live global repeater total |
| Recent RX/TX summary | Current model activity feed supports per-event RX and TX entries; 5-minute counts can be bounded/partial |
| Air traffic histogram | Current **20-minute** model bins count receptions only, by message/advert/other; **do not chart TX in these bars** |
| Recently heard nodes | Last-heard age and measured RSSI available per node when reported; a relayed node's RSSI is not its end-to-end signal |
| RX LOG | Existing packet inspector and button; keep separate from Activity |
| Service issues | Surface state faults, malformed events and full node table when present |

Mockup values and names are **fictional**. Do not treat the illustrated counts, EU868 display, or service badges as verified live telemetry.

## Deliberately excluded from any implementation assumption

- No geographic distances or map positions inferred from signal, RSSI or hop counts.
- No instantaneous airtime, RSSI sensitivity or link-health scores without a defined data source.
- No invented GPS, service health, routes, or telemetry categories.
- No 60-minute history without extending the current 20-minute bins and reviewing the cost.
- No unsupported RX/TX equivalence: heard-on-air frames, app feed entries, and cumulative service counters differ.
- Do not replace the approved global navigation/chrome with mockup-only decorations.

## Before an implementation proposal

1. Review the current branch and approved RIFT UI handoff.
2. Decide whether Neon is RIFT-only (preferred initial scope) or a full Doors theme (requires design system approval).
3. Define token roles and verify contrast, night/outdoor modes, keyboard focus, accessibility, 36/56 px touch semantics and both orientations.
4. Choose actual responsive geometry fitting the landscape shell and command line; reserve status/chrome as specified in the current handoff.
5. Build representative ACTIVITY / NODES / COMMS / NET concept proofs and check LVGL rendering cost.
6. Implement behind explicit opt-in only after owner approval, with tests and device/simulator verification.

**No runtime C code, theme tables or approved specification is changed by this concept.**
