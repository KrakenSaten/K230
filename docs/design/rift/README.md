# RIFT for Doors — design package

Approved direction, 2026-09-19. Everything in this folder is design reference; `HANDOFF.md` is the contract for implementation.

## Files

| File | Role |
| --- | --- |
| `HANDOFF.md` | implementation handoff (dimensions, orientation, navigation, sizes, semantics, paths, theme, keyboard, open decisions) |
| `RIFT for Doors.dc.html` + `support.js` | design source; open the HTML in a browser. Turn 3 (landscape) at top, turn 2 (portrait) below, turn 1 (identity, first pass, states, theme proof) at the bottom as reference |
| `shots/*.png` | 1:1 exports of the approved screens |

## Screen inventory

| Name | File | Size | Status | Source id |
| --- | --- | --- | --- | --- |
| portrait-nodes | `shots/portrait-nodes.png` | 568 × 1232 | approved | 2a |
| portrait-node-detail | `shots/portrait-node-detail.png` | 568 × 1232 | approved | 2b |
| portrait-net | `shots/portrait-net.png` | 568 × 1232 | approved | 2c |
| portrait-comms | `shots/portrait-comms.png` | 568 × 1232 | approved | 2d |
| landscape-nodes | `shots/landscape-nodes.png` | 1232 × 568 | approved | 3a |
| landscape-node-detail | `shots/landscape-node-detail.png` | 1232 × 568 | approved | 3b |
| landscape-net | `shots/landscape-net.png` | 1232 × 568 | approved | 3c |
| landscape-comms | `shots/landscape-comms.png` | 1232 × 568 | approved | 3d |
| responsive-rules | `shots/responsive-rules.png` | sheet | approved | 3e |
| identity-sheet | `shots/identity-sheet.png` | sheet | approved (icon, mark, glyphs, strip, chip states) | 1a |
| theme-proof | `shots/theme-proof.png` | sheet | reference (node rows in 5 themes + night) | 1i |

Turn 1 screens 1b–1h (activity, first-pass nodes/detail/comms/channel/net, empty/radio-unavailable/reconnecting states) are reference only: their state vocabulary is approved, their 40/48 px chrome is superseded by turn 2.

Exports include a 1 px reference border (570 × 1234 / 1234 × 570); the design canvas is the inner 568 × 1232 / 1232 × 568.

## Not in this package (by decision)

ACTIVITY landscape, portrait with the touch keyboard up, RELAY LOAD view. These follow `HANDOFF.md` §9 and are left to implementation.
