# Integration — selected B direction

## Asset selection

Use opaque native backgrounds under live UI. Portrait and landscape are separately composed, not runtime rotations. The new launcher and system menu share the same subdued mountain background within each orientation. Boot, lock and open remain from the previous package. `base_ui_layout.json` applies only to those three screens; use `b_ui_layout.json` for the B launcher and system menu.

Each SVG overlay expresses actual pixel coordinates, text baselines, scrims, icon placement and focus strokes. JSON exposes text and control bounds; consult SVG for graphics and exact panel styling. Baselines are not top-left text positions. The layout is a reference rather than framework source code.

Render `icons/` as app icons, and `glyphs/` as system symbols. Use the chosen PNG size at or near native scale or convert SVG offline with the existing asset build pipeline. Avoid scaling assets per frame. Images have transparent outer margins; do not infer hit targets from the opaque pixels. Keep the portal frame and glyph together, and apply focus outside the image so it remains dynamic. Color palette is in `b_ui_layout.json`.

Do not apply a preview overlay in addition to recreating its scrim: that would double-darken the background. Preview layers contain sample data, not runtime state.

## Grouping

| Group | Entries |
|---|---|
| Connections | Radio, Mesh, Network |
| Workspace | Tools, AI, Files |
| Device & play | Games, Settings, Apps |

Portrait stacks the three fixed frames vertically. Landscape places them side by side, preserving group order and entry order. App IDs in this handoff are conceptual keys; resolve them against the actual project registry. Do not invent app routing or active apps. Use the existing disabled/hidden pattern for unavailable features and the existing focus manager for directional navigation.

A sage corner bracket and underline mark focus. Color is supplementary; frame geometry and text remain readable without color. The 48 px exports are compact alternatives; prefer 96/128 px or appropriate offline sizing for the shown launcher. Check strokes at actual device scale.

## System menu

Top controls: Wi-Fi, Bluetooth, Radio, Sound. These open or operate the corresponding existing system functions; wire them according to actual capabilities. The preview does not define new service behavior. Bluetooth Off must not appear connected. Unknown telemetry should use the project's unknown state.

Sliders: brightness and volume. Preview values 60% and 40% are illustrative. Values must come from real supported providers, clamp to valid ranges and show failures rather than pretending a hardware change succeeded. Use existing throttling/debouncing while dragging.

Detail entries: Display & sleep, Connections, About DOORS. Actions: Lock, Power. Lock follows existing lock/authentication semantics. Power opens the existing confirmation/action menu; it must not immediately power off merely from selecting the tile.

Landscape reflows quick controls to the left, sliders and detail entries to the right, and actions below the quick controls. Preserve logical focus when orientation changes. Use 32 px slider hit zones as the reference minimum; increase invisibly if the device's existing touch standards require it.

## Font and performance

Use the existing project font when suitable, then verify widths and date localization. Nimbus Sans Regular is supplied for reference reproduction with its license. No runtime SVG renderer is required: SVG is editable source and PNG is already available. Reuse existing status glyphs if they improve consistency while retaining sizes/spacing.

One native frame contains 699,776 pixels. A decoded RGB565 image is 1,399,552 bytes (~1.33 MiB), RGB888 2,099,328 bytes (~2.00 MiB), and ARGB8888 2,799,104 bytes (~2.67 MiB), excluding decoder scratch space and display buffers. Load active backgrounds only; do not preload the ten previews or both orientations. Avoid decoding in redraw callbacks. Keep resource lifetime tied to screen ownership, and release only after widgets stop referencing images.

No registered door animation is supplied. Use a direct change initially; separate generated backgrounds may have geometric differences. No realtime blur, 3D or shader effects are needed.

## Validation

Native dimensions, PNG decode/CRC, alpha icon exports and in-bounds control regions are checked in this delivery. Preview layouts were inspected visually. During integration validate the actual project build, launcher routes, input/focus, time/telemetry, slider behavior, safe Power action, rotation, decode cost and memory headroom. Inspect dark detail, contrast and RGB565 banding on the AMOLED device.

## Rebuild

`tools/build_b.py` recreates B icons, glyphs, layouts and previews using Python 3, Node with `sharp`, ImageMagick (`convert`, `montage`), and installed Nimbus Sans. Run from the extracted package. Existing backgrounds, base previews, overlays and base_ui_layout.json are self-contained inputs; the previous workspace is not required. The first-import fallback in the script is used only when these inputs do not exist. It does not regenerate the photographic artwork. Rebuild the file inventory after modifications.
