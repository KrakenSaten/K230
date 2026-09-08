# PocketTimber art (D1)

The visual asset pipeline for PocketTimber: the direction, the composition,
the canonical projection, the Blender scene that renders the sprites, the
materials and the light, the asset budget, how a block finds its sprite,
how the simulator draws it, and what still waits on hardware.

Status 2026-09-08: the direction, the projection and the pipeline are in
place and proven with a small proof set rendered by Blender in batch and
drawn by the simulator; the production set is not rendered. See "Verdict"
at the end and the art status in [POCKETTIMBER.md](POCKETTIMBER.md).

Isolation: this is D1 only. Nothing in the engine, the scoring, the collapse
or the tuning changed; the view model's projection constants changed to the
canonical ones and the table widget gained a sprite path beside its
placeholder. D2 is untouched.

## 1. Visual direction

A premium tabletop game seen from above and to the side, as a product
photograph would frame it: a tower of light hardwood blocks on a dark
green baize, under one soft warm light, inside PocketOS's dark instrument
chrome. Everything physical is warm and matte; everything that is
interface is a token and stays out of the picture.

- Wood, not plastic: a light-to-medium hardwood with visible but subtle
  grain along the block, worn edges, a matte finish. No saturation, no
  gloss, no scratches, no noise that vanishes at 36 px.
- Felt, not a gradient: a dark muted green with a low fibrous texture.
- Light, not drama: one warm key from the upper left, a cool neutral fill,
  a soft contact shadow under the base, block faces separated by tone, no
  specular highlights, no rim light.
- Chrome, not overlay: the HUD, the piece card and the controls are
  Design System panels and slabs on `bg`; the felt is confined to the
  viewport, a hairline-framed panel, which is the framed-scene ruling of
  D1 (the scene is content, not chrome).
- Restraint over decoration: no particles, no dust, no vignette, no
  bloom, no depth of field. The collapse is the engine's choreography,
  drawn with the same sprites.

What distinguishes it from PocketFleet (a grid of glyphs) and PocketRadar
(a scope of strokes): it is the only PocketOS app with a rendered physical
object in it, and the only one whose playfield has a colour of its own.

## 2. Audit of the P7 placeholder

What P7 draws, measured from the code and the screenshots:

| Item | P7 | Verdict |
| --- | --- | --- |
| Viewport | 528 x 600 panel, hairline in `line`, no fill | keep the frame; grow to 700 (below) |
| Projection | `sx = (x - y) * 40`, `sy = (x + y) * 20 - z * 22 / 154` | 2:1 in plan, but 22 px per layer is not a real camera (it implies 50 degrees of elevation against 30 for the top faces); replaced by the canonical projection |
| Tower footprint | 240 px wide, 18 layers 396 + 120 = 516 px tall | becomes 216 wide, 576 tall |
| Block | end face 40 x 22 (+20 shear), sprite bbox 160 x 102 | becomes 36 x 26 (+18), 144 x 98 |
| Faces | three flat tokens: `line` top, `surface_raised` +x, `surface` +y, `text_muted` hairline | placeholder; kept as the fallback path |
| Selection | 2 px `accent_primary` outline on the pulling end | keep: theme-aware, cheap, DS focus outline |
| Ghost | accent outlines on the top and end at 70 % | keep, plus the sprite at 40 % under it |
| Tell | 2 px nudge along the block's axis | keep |
| Tilt | the top's far edge raised 3 px per tilt step | placeholder only; real poses are rendered |
| Stability meter | ten `chip` segments, 10 px, RX on / OFF off | keep: DS segmented meter |
| HUD | two rows: SCORE, LAYERS; STABILITY caption + meter | one row (below) |
| Piece card | two rows: value + chip; PIECE, WORTH, TESTS | one row (below) |
| Text | mono 14 captions, mono 20 values, mono 16 buttons, sans 24 title, hero 48 on the result | keep: the DS hierarchy |
| Chrome | panels, slabs, primary and secondary buttons, chips, all tokens | keep; nothing names a colour |
| Layout total | 1110 px of a 1104 px body: the buttons sat on the bottom edge | fixed by the single-row cards |

Good enough to preserve: the screen structure and reading order, every
control and its size, the selection and ghost treatment, the meter, the
result screen. Placeholders: the block faces, the missing felt and shadow,
the tilt hack, the projection numbers, the two-row cards.

## 3. Composition (568 x 1232)

The shell owns the status bar (56) and the app header (72); the body is
1104 px with 24 px top padding and 20 px side padding, 528 px wide.

| Zone | Height | Content |
| --- | --- | --- |
| HUD | 85 | one row: SCORE (BEST in standby), LAYERS, and STABILITY as a caption over a ten-block meter, 196 px wide, right |
| gap | 22 | |
| Viewport | 700 | the felt, the shadow, the tower; a hairline panel, corners clipped |
| gap | 22 | |
| Piece card | 85 | one row: PIECE, AT, WORTH, TESTS, and the state chip |
| gap | 22 | |
| Controls A | 64 | the pull track (a `surface` slab with its caption); while placing, the three side buttons (56) instead |
| gap | 22 | |
| Controls B | 56 | TEST (secondary) and BEGIN / PULL / PLACE (primary), 1 : 2 |

Total 1102 of 1104. The thumb zone holds every control; the tower is only
tapped, never dragged, and a tap snaps to the nearest pullable block end
within 44 px, so nothing the player must hit is small.

**Playfield bounds:** the viewport, 528 x 700, centred. **Tower position:**
the base's far corner projects to (264, 576) inside it, so the base's near
corner clears the bottom by 16 px and an 18-layer tower leaves 108 px of
felt above it. **Maximum visible height:** 21 layers at this scale; above
that the view keeps the top in view and lets the base go out of it, which
is the camera the review chose (drag-to-pan is v0.2; a half-scale sprite
set for a whole-tower view is an OPTIONAL asset below). **Stability and
risk:** the meter in the HUD, the lean the tower visibly takes, the sway,
and the piece card's class chip; nothing is added to the picture.
**Collapse and result:** the choreography plays in the viewport with the
same sprites and the result screen replaces it when the last block rests,
unchanged from P7.

## 4. The canonical projection

A study of three restrained elevations, rendered from the same six-layer
tower (`study/projection-30.png`, `-35.png`, `-42.png`), all at azimuth
45 degrees so both front faces show:

| Elevation | Top-face shear per width | Px per layer at 36 px per width | 18 layers | 21 layers | Reads |
| --- | --- | --- | --- | --- | --- |
| 30 (2:1 dimetric) | 18 (exact 2:1) | 26.5 | 585 | 664 | tallest end faces, crisp 2:1 pixel edges, the classic miniature |
| 35.26 (true isometric) | 20.8 | 24.9 | 573 | 648 | the same look with slightly flatter blocks |
| 42 | 24.1 | 22.7 | 553 | 621 | most top surface, least vertical cost per layer, but the end faces the player pulls by shrink and the base diamond grows |

**Chosen: 30 degrees, 2:1 dimetric, orthographic.** The end faces are the
pull affordance and this keeps them tallest; the 2:1 slope lands on whole
pixels so stacked sprites do not shimmer; vertical efficiency is within a
few per cent of the others. The engine's coordinate system is unchanged:

```text
sx = ox + (x - y) * 36 / 256
sy = oy + (x + y) * 18 / 256 - z * 26 / 154
```

with x, y in Q8.8 widths and z in Q8.8 layers of 154. One width = 36 px
along a diagonal, its top face rises 18 px per width, a layer is 26.46 px
in the render and stacks at 26: the half pixel is under the layer above.
A block: end face 36 x 26 with an 18 px shear, top face 144 x 72, sprite
bounds 144 x 98, plus 2 px of transparent padding each side.

The engine draws +x toward the lower right. A right-handed camera at the
tower's near corner sees +x toward the lower left, so every sprite is
mirrored horizontally on export and the key light is placed in Blender so
that it reads from the upper left after the flip. Nothing in the engine
knows.

## 5. The Blender scene

`assets-src/timber-art.blend` holds the scene `TimberArt` alone, written
by `tools/timber_blender.py` with `bpy.data.libraries.write()`; the script
is the source of truth and rebuilds the scene from nothing. Blender 5.2
LTS, EEVEE, 64 samples, film transparent, view transform Standard, display
sRGB (no Filmic or AgX: sprite colours must be predictable).

| Element | Specification |
| --- | --- |
| Block | 75 x 25 x 15 mm, origin at its far-bottom corner (min x, min y, z 0); bevel modifier 0.8 mm, 3 segments, harden normals, faces smooth |
| Orientation | a y block is the same object turned +90 degrees about z and shifted one width in x, so its footprint starts at the cell |
| Poses | tilt about the block's own long axis: 0, +14, -22 degrees |
| Camera | orthographic; rotation (60, 0, 135) degrees; `ortho_scale` = render width in px / 2036.5 px per metre, which is 36 px per width along a diagonal; aimed at the block's centre for sprites |
| Key | sun, 3.2 W/m², colour (1.0, 0.95, 0.88), angle 6 degrees, travelling from direction (0.35, 0.75, 1.20) normalised: mostly from above, more from +y than +x, so the +y face (screen left after the flip) is lighter than the +x face |
| Fill | the world background only: (0.16, 0.175, 0.20), strength 1.0, cool and dim |
| Ground | a 2 x 2 m matte plane in the felt material, hidden for sprite renders |
| Sprite render | 208 x 160 px, then cropped to the alpha bounds with 2 px padding |

No camera is nudged per asset: the camera slides along its own axis to
centre each block and every anchor is computed from the projection, not
read off a picture.

## 6. Materials

**Wood** (`TimberWood0..2`): Principled BSDF, roughness 0.68, specular
level 0.25, no coat, no sheen. Base colour from a colour ramp between two
close tones driven by a noise texture (scale 1, detail 3, roughness 0.55)
through a mapping scaled (6, 90, 40) in object space, so the grain runs
along the block's length with slight wander and rotates with the block.
The ramp is cut at 0.35 and 0.65, which keeps the grain visible without
contrast that would read as stripes at 36 px.

| Tone | Face (sRGB) | Grain (sRGB) | Use |
| --- | --- | --- | --- |
| 0 base | #C9A670 | #A6804E | two thirds of the blocks |
| 1 lighter | #D6B682 | #B28F5E | a sixth |
| 2 darker | #BA9560 | #966D42 | a sixth |

The variants are the same species a shade apart, never a different wood.
A rare knot variant is OPTIONAL and not designed. Which tone a block gets
is decided by the engine's seeded variant (section 9).

**Felt** (`felt_tile.png`, 128 x 128, tileable): base #1B3A2A with three
octaves of wrapping value noise at ±10 levels and a two-octave streak ±4 along
one axis: fibrous, low contrast, no moiré at 1:1 because nothing in it is
periodic below the tile. Generated by the script with numpy, not rendered,
so it is exactly repeatable. Blocks do not cast shadows onto it in the
sprites; the base's contact shadow does (section 7).

## 7. Lighting and shadows

One warm key sun, the world as the only fill, no rim, no specular. The
three visible faces separate by tone alone: top brightest, +y face next,
+x face darkest, which is also how the placeholder ordered them, so the
two paths read the same. Each sprite carries its own occlusion in the
softness of its bevels and the shading of its faces; there is no baked
ambient occlusion between neighbours, because neighbours change.

**Contact shadow** (`tower_shadow.png`, A8): the base's footprint diamond
in screen space, grown half a width with a smooth falloff, alpha 150 at
the centre. Drawn once under the base in the `bg` token at 150/255, so it
is a darkening of the felt whatever the felt is. It is a contact shadow,
not a cast shadow: a cast shadow from a tower this tall under a
photographic key would run across the whole felt and move with every
pull, which is expense and clutter for nothing.

Shadows between blocks (a block on a thinned layer over air) are not
drawn. OPTIONAL later: a single-block contact shadow for rested blocks in
the collapse pile.

## 8. Asset budget

One sprite is 144 x 98 px ARGB8888 plus padding: 148 x 102 x 4 = 60 kB.

**REQUIRED for production**

| Asset | Count | Size |
| --- | --- | --- |
| Block sprites: 2 orientations x 3 tones x 3 poses | 18 | 1.1 MB |
| Felt tile 128 x 128 RGB888 | 1 | 49 kB |
| Tower contact shadow 304 x 196 A8 | 1 | 60 kB |
| Total | 20 | about 1.2 MB in RAM and flash |

**OPTIONAL**

- A rare knot tone (2 orientations x 3 poses): 6 sprites, 360 kB.
- A single-block contact shadow for rested blocks: 1, about 20 kB.
- A half-scale sprite set for a whole-tower view above 21 layers: 18
  sprites, 280 kB.
- A 256 x 256 felt if the 128 tile shows repetition on the panel: 196 kB.

**DO NOT PRE-RENDER**

- Selected block: the accent outline on the pulling end, drawn.
- Ghost: the block sprite at 40 % plus the accent outlines, drawn.
- Tested feedback: none in the picture; the piece card says it.
- Tells: the 2 px nudge, drawn.
- Result art: the result is Design System chrome over the live pile.
- Lean and sway: displacement of the sprites, computed.
- Any rotation at runtime: the three poses are the tumble.

**The proof set rendered here:** `block_x_t0_p0`, `block_y_t0_p0`, the
felt, the shadow, the projection study and a composition reference. Every
lookup for a tone or pose not rendered falls back to the flat base tone,
so the proof plays the whole game with two sprites.

## 9. Naming and mapping

Files: `rendered/block_<x|y>_t<tone>_p<pose>.png`, `rendered/felt_tile.png`,
`rendered/tower_shadow.png`, and `rendered/anchors.json` with every
sprite's size, anchor and bounds. C symbols: `timber_art_img_<key>`; the
table `timber_art_sprites[]` with `{ key, image, ax, ay }`.

The mapping is a function of engine state and nothing else, evaluated in
the draw callback and never stored:

| Input | Rule |
| --- | --- |
| Orientation, standing | the layer's axis: even layers x, odd y (`timber_layer_axis`) |
| Orientation, falling or fallen | the choreography's pose: pose < 3 is x, else y |
| Tone | `block.variant % 3`; the variant is drawn from the seed at generation and reseated by hash on placement, so the same seed shows the same wood |
| Pose | the choreography's `pose % 3`: 0 flat, 1 and 2 the two tilts |
| Anchor | the block's far-bottom corner projected by the view model, standing or falling |
| Fallback | a tone not rendered takes the base tone; a pose not rendered takes the flat block |

No art state exists outside the UI; the engine keeps its `variant` field
"for the view only" and nothing writes it back.

## 10. Simulator integration

- `docs/design/timber-art/tools/png2lvgl.py` converts the PNGs to LVGL 9
  image arrays and writes `timber_art_table.c` from `anchors.json`. It
  runs at CMake configure time (`ui/shell/CMakeLists.txt`) into the build
  tree; nothing generated is committed, and the PNGs are the only assets
  in git. Standard library only, so the build host needs python3 and
  nothing else; without it, or without the renders, the shell builds with
  `POCKETTIMBER_ART 0` and the placeholder.
- `apps/timber/ui/timber_art.[ch]`: the table and the lookups with their
  fallbacks.
- `apps/timber/ui/timber_table.c`: the felt tiled over the viewport, the
  shadow under the base, then every block back to front as a sprite
  anchored where the view model projects its far-bottom corner; the ghost
  as the held block's sprite at 96/255 under the accent outlines; the
  selection outline unchanged. `POCKETTIMBER_PLACEHOLDER=1` in the
  environment draws the P7 faces instead, for comparison.
- `apps/timber/ui/timber_view.[ch]`: the canonical constants and the
  anchor corner on every block shape; `tests/timber_view_test.c` follows.
- The collapse is unchanged: the same choreography positions drive the
  same sprites, with the pose choosing orientation and tilt.

**Review screenshots** (`docs/design/shots/`, 568 x 1232, from
`tests/timber_shell_test.sh` and the theme renders): `timber-idle.png`,
`timber-run.png` (a STUCK block selected at L1 RIGHT), `timber-pulling.png`,
`timber-placing.png` (the ghost on top), `timber-collapse.png`,
`timber-result.png`, `timber-pulling-reduced-motion.png`,
`timber-run-placeholder.png` (the P7 path, same state), and
`timber-run-<theme>.png` for the five themes.

## 11. Theme behaviour

The world is not themed: the wood, the felt and the shadow's shape are the
same in `ice`, `brass`, `olive`, `slate` and `carbon`, and in every mode.
What follows the theme is everything that is interface: the panel's
hairline, the selection and ghost outlines (`accent_primary`), the shadow's
colour (`bg`, a darkening), the HUD, the piece card, the controls. Night
mode does not dim the scene in this pass; a `bg` overlay at half opacity
over the viewport is the review's design for it and is one draw call when
wanted. The theme review renders (`docs/design/shots/timber-run-<theme>.png`)
are the check that a theme change recolours no wood.

## 12. Hardware-validation dependencies

Unchanged from the engine's gates, now with the art's own numbers:

| Gate | What the art assumes |
| --- | --- |
| Sprite-storm redraw budget | up to 54 blended 148 x 102 ARGB sprites plus a tiled felt and a shadow per moving frame at 25 Hz; nothing is cached |
| Sway readability | one unit of disturbance sways the top 3.6 px at 36 px per width |
| Panel colour | the wood tones and the felt were judged on a desktop sRGB display; the RM69A10 AMOLED and its RGB565 path (DS feasibility H1) may band the felt's ±10 levels |
| Scale | 36 px per width is 2.8 mm on the 330 ppi panel; whether grain reads at all there is a bench question |
| RAM and flash | 1.2 MB for the production set, unmeasured against the 512 MB or 1 GB question |

None of these change the pipeline; they change numbers in
`timber_view.h` and the render resolution, which the script derives.

## 13. Production-render checklist

1. `blender -b -P docs/design/timber-art/tools/timber_blender.py -- all`
   renders the 18 block sprites, the felt, the shadow, the study and the
   composition into `rendered/` and `study/`, and rewrites
   `assets-src/timber-art.blend` and `rendered/anchors.json`.
2. Check `anchors.json`: every block sprite's `bbox_px` is `[144, 98]`
   flat; the tilted poses are taller and their anchors differ.
3. Reconfigure the simulator (`cmake --build` re-runs the conversion when
   `anchors.json` changed) and run `tests/timber_shell_test.sh`.
4. Review the six screenshots and the five theme renders.
5. Judge the tones on the panel before rendering anything else.
6. Commit the PNGs and `anchors.json` only; never the generated C.

Known pipeline notes: the interactive Blender addon stopped answering
after the first scene-building call in the session that produced this
pass, and every render here came from batch mode, which is the intended
production path. `Image.save()` on a generated datablock writes a blank
buffer in Blender 5.2, so the script encodes PNGs itself.

## Verdict

**ART DIRECTION APPROVED FOR PRODUCTION**, on the proof set above: the
wood reads as wood at 36 px per width, the felt and the contact shadow
give the tower a table to stand on, the composition holds 18 layers with
room, the projection is a real camera the engine's coordinates already
match, the pipeline reproduces from one script, and the simulator draws
the game with the sprites while the placeholder stays a build option.

Art decisions still open, none of which change the direction:

- The two tilt poses and the two tone variants are specified and
  parametrised but not rendered or seen stacked; the `all` stage renders
  them and the composition should be judged again with them.
- Night mode: a `bg` overlay over the viewport or nothing.
- The rare knot tone, the single-block shadow and the half-scale set are
  OPTIONAL and undecided.

Decisions deferred to hardware: every row of section 12.
