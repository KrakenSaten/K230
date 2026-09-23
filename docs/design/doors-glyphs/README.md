# First-party DOORS glyphs

System glyphs the approved visual package (`../brand/doors-visual-pack-v1`,
read-only) does not have, drawn in its line language so they sit beside its
own: a 48-unit canvas with the package's `viewBox="0 -6 48 48"`, 2-unit
strokes with round caps and joins, in the package white `#eeeae2`.
`tools/design/gen_doors_ui.py` rasterises them to 32 px A8 masks in
`ui/pocketui/pos_glyphs.c` with the same renderer it checks against the
package's own 32 px exports (`gen_doors_ui.py --compare`).

| File | Glyph | Used for |
| --- | --- | --- |
| `mode.svg` | a disc, half filled | Controls → Display (Normal / Night / Outdoor), DS §31.5. The package's sun stays on Brightness. |
