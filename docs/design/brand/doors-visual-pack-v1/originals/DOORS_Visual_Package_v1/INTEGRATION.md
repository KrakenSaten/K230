# Integration specification

## Visual rules

Use the approved restrained photographic direction: dark metal doors, narrow stone jambs, natural light, quiet grey-green mountains. Keep the doors large within the display. No arches, lanterns, medieval ornament, neon, app icons or new visual directions.

Use `backgrounds/{portrait,landscape}/{boot,lock,open,launcher}.png` at 1:1 pixels in the matching viewport. The portrait scenes are composed separately; do not rotate or crop a landscape background to produce portrait. All runtime backgrounds are opaque RGB PNG, 8 bits per channel.

The `screens/` images are layout references with sample UI. They must not become the production background: that would freeze clock, battery and progress. Likewise the transparent PNG overlays are previews, not live controls. The SVG overlays preserve editable text and status geometry.

## Runtime layers

1. Native background image.
2. Optional UI scrims as specified in the corresponding SVG: a subtle dark header, an extra 20% launcher shade, and a footer shade on open.
3. Text, status glyphs, focus, progress and controls drawn by the existing UI framework.

Read `ui_layout.json` for exact text anchors, baselines, font pixel sizes, cell bounds and colours. SVG text y coordinates are **baselines**, not top positions. Convert using the actual project font metrics. Font widths must be checked after substitution. Use the SVG for glyph paths and scrim opacity values. The small preview wordmark is editable text, not a replacement brand logo.

## Screen behavior

| Screen | Background | Runtime content | Entry/exit |
|---|---|---|---|
| Boot | Fine warm light seam | DOORS, tagline, service-backed progress/status | Exit when existing startup state says ready |
| Lock | Fully closed door | Local time/date, real Wi-Fi/battery, unlock hint | Existing tap/key/authentication policy |
| Open | Bright mountain view through doorway | Small clock/status, tagline | Brief presentation state or standalone wallpaper |
| Launcher | Darkened open scene | Time/date, 3 × 3 text menu, focus | Existing app routing and input model |

Do not invent progress percentages from timers. If startup provides no measurable progress, use existing indeterminate feedback or plain status text. Only show Wi-Fi connected or battery percent when valid telemetry exists. If time is unavailable, use the project's existing placeholder. Display locale-aware date and timezone.

Treat unlock as visual behavior only; preserve whatever authentication/lock semantics the project already has. A door graphic does not add security. Navigation in open/launcher must remain available if the decorative transition is disabled.

Launcher order, row-major: Radio, Mesh, Network / Tools, AI, Games / Settings, Files, Apps. Use real app identifiers from the repository rather than guessing routes. Preserve focus and input plumbing. The light vertical bar plus muted field indicates focus; labels stay readable without colour alone. Portrait and landscape retain the same logical selection. For unavailable categories use the existing disabled/hidden behavior and update the grid consistently.

The JSON cell bounds are suggested hit/focus areas; the reference text belongs at the listed baseline. Do not infer touch regions from the photographic door handles.

## Transitions and resource budget

No intermediate animation frames are supplied. The closed and open images are visually related but not pixel-registered animation endpoints. Start with a direct switch. An optional short fade may be evaluated on hardware, but it is not a door-opening animation. Avoid real-time 3D, blur, particles or per-frame image decoding.

Each native background is 568 × 1232 = 699,776 pixels:

- RGB565: 1,399,552 bytes (~1.33 MiB) per decoded image.
- RGB888: 2,099,328 bytes (~2.00 MiB).
- ARGB8888: 2,799,104 bytes (~2.67 MiB).

These are pixel-buffer sizes, not total process memory. Decoder scratch space, framework object costs, draw buffers and GPU/display copies are additional. Two simultaneous ARGB8888 images alone need ~5.34 MiB. Load only the active orientation/screen and at most the next required image if measured headroom permits. Do not preload all 8 backgrounds or the high-resolution masters.

Use the repository's own LVGL/image conversion pipeline if it requires compiled assets. Inspect its version, byte order, colour depth and decoder configuration before converting; this package does not assume any of those. If converting to RGB565, inspect gradients, the boot light seam and dark textures on the physical AMOLED panel for banding/crushed blacks.

Avoid filesystem reads/PNG decoding inside UI repaint callbacks. Use the existing resource-loading and lifetime rules. Unload obsolete images safely after widgets stop referring to them. Update the clock at the needed cadence (one minute for HH:MM), telemetry when it changes, and dirty only the affected regions where the framework permits.

## Focused acceptance checks

- All four screens fit the actual display with no crop or unexpected rotation.
- Clock/date, status and launcher labels remain legible at actual size.
- No baked sample telemetry appears in production.
- Every visible launcher item opens a real target or clearly reports availability.
- Touch and existing keyboard navigation work in both orientations.
- Focus survives orientation changes and returning from apps.
- Boot status reflects actual readiness; lock semantics remain intact.
- Open/launcher transitions have no stale image pointer or failed-allocation crash.
- Measure one cold load, one screen change and memory at the worst two-image point.
- Confirm dark material visibility and warm seam on the actual AMOLED.

## Rebuilding this handoff

Install ImageMagick (`convert`, `montage`), Node.js with `sharp` resolvable, and Nimbus Sans from URW Base35. Run `python3 tools/build_package.py` from the extracted package. Existing `masters/` are the input; the original image-generation workspace is not needed when these files are present. Font lookup for the script's final copy step follows common Linux URW paths; adapt those two copy paths on other systems. The script regenerates background exports, overlays, screen previews, contact sheets and `ui_layout.json`. Regenerate hashes if changing packaged files.
