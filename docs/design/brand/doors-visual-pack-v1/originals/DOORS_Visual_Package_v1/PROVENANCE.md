# Artwork and UI provenance

Artwork was produced with the built-in image-generation tool from the user's approved DOORS references in this conversation. No CLI image API fallback was used. Native exports use ImageMagick Lanczos resizing; tiny source aspect-ratio rounding differences are normalized, without recomposing or cropping scenes. UI was recreated separately as SVG and rendered with sharp for native previews.

The editable SVG and ui_layout.json are authoritative for this handoff's live-UI placement. They regularize the previous generated mockups into reproducible dimensions and readable portrait text. The package is an asset handoff; no repository was inspected or modified.

## Prompt specification used for the final backgrounds

Common: restrained real photographic architecture, charcoal matte metal double door filling the display, narrow grey stone jambs, plain slim handles, straight-on fixed camera, small threshold floor. Natural materials, quiet contrast, no arches, lanterns, fantasy ornament, neon, bloom or HUD. No text, branding, clock, status, controls or icons baked into clean artwork.

Portrait lock: recompose the approved landscape door for 568:1232, tall double leaves, quiet upper area for a later clock; door closed, subdued natural ambient light.

Portrait boot: preserve portrait lock architecture and framing; darken and add a fine warm ivory seam between closed leaves with a small reflected light patch on the threshold.

Portrait open: use portrait lock framing; open both leaves inward, revealing muted green mountain hills, grey peaks and a lake under soft daylight, based on the approved open landscape reference.

Portrait launcher: preserve open scene and frame; substantially darken/compress contrast with a charcoal treatment so later text dominates, while the landscape remains faintly visible.

Landscape boot/lock/open: remove all text and UI from the corresponding approved complete screen, reconstruct the underlying material, preserve architectural composition and lighting at 1232:568.

Landscape launcher: remove all UI, labels, rules, selected cell and text from the approved launcher, retaining its dark mountain scene and architectural frame.

Generative edits can introduce small differences between related stills. Exact geometric registration is not certified and no animation claim is made.
