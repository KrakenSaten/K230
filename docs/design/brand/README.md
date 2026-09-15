# Doors brand assets

The artwork for the product name Doors (ADR-005), brand direction
**Threshold**: doorway, portal, opening, transition. This file is the
inventory of the graphics package, what was checked in it, and where each
asset is used in the Doors UI.

## Provenance

| | |
| --- | --- |
| Supplied by | the product owner, 2026-09-15, for the graphics integration session |
| Package | `Doors-Threshold-Production.zip`, 453,639 bytes, SHA-256 `8e0f25b81d4edf1202cfe90a966f8a2dabf5d9dae18ac14e8576b79466481620` |
| Contents | 60 files, dated 2026-09-13 inside the archive |
| Kept as | `doors-threshold/`, byte-for-byte as supplied. `.gitattributes` exempts the folder from end-of-line conversion, so every hash below is the hash of the committed file |
| Author, copyright, licence | not stated anywhere in the package. The supplier's own notes are `doors-threshold/ASSET-NOTES.md` |

Nothing in `doors-threshold/` is edited. A change to the artwork is a new
package from the owner, and the files and hashes here are replaced together.

## Inventory

Every PNG was decoded in full (header, chunk CRCs, every pixel). Every PNG is
8-bit and not interlaced, and carries only `IHDR`, `IDAT` and `IEND`: no ICC
profile, gamma, text or EXIF chunk. Every SVG is vector paths only: no
`<text>`, `<image>`, `<style>`, `<font>`, `@font-face`, `font-family`, `href`,
`url(`, base64 data, script, gradient or filter.

Status: **used** = converted into the image by this repository; **reference**
= kept for design work, nothing ships from it; **deferred** = usable later,
needs work described below; **not used** = superseded by another file in the
package for the way Doors draws it.

### `boot/`

| File | Format | Pixels | Alpha | What it is | Status |
| --- | --- | --- | --- | --- | --- |
| `doors-boot-568x1232.png` | PNG, RGB | 568 × 1232 portrait | none, opaque | the boot composition: pure black, mark `#8ccfff` 100 × 120 at (234, 517), "Doors" wordmark `#e9eef3` 200 px wide from (184, 661); visible content x 184–383, y 517–715, centred; 242 colours: black, the two artwork colours, and 239 more from the wordmark's antialiased edges (the mark is exactly 6,600 px of one colour) | **used**: the boot splash |
| `doors-boot-568x1232.svg` | SVG, 6 paths | viewBox 568 × 1232 | — | vector master of the PNG | reference (master) |

SHA-256 of the splash source: `e98aacc22f8e9fd8ab18dc20bd8219021ccfd7bba6f7851fadfdb4ff537a1690`.

### `brand/`

| File | Format | Pixels | Alpha | What it is | Status |
| --- | --- | --- | --- | --- | --- |
| `doors-mark.png` | PNG, RGBA, white | 32 × 32; mark 20 × 24 at (6, 4) | binary: 264 opaque, 760 clear, 0 partial | **compact mark**, tintable | **used**: the System identity mark |
| `doors-mark.svg` | SVG, 1 path, `currentColor` | 32 × 32 | — | master of the above | reference (master) |
| `doors-mark-16.png` | PNG, RGBA, white | 16 × 16; mark 10 × 12 at (3, 2) | binary: 66 opaque | small optical size (3 px threshold gap) | reference |
| `doors-mark-16.svg` | SVG, `currentColor` | 16 × 16 | — | master | reference |
| `doors-mark-dark.png` / `.svg` | PNG RGBA / SVG, `#e9eef3` | 32 × 32 | binary | the mark with a fixed light colour | not used: same shape as `doors-mark.png`, colour baked in |
| `doors-mark-light.png` / `.svg` | PNG RGBA / SVG, `#06080b` | 32 × 32 | binary | the mark with a fixed dark colour | not used: Doors has no light surfaces (DS §1) |
| `doors-primary-dark.png` | PNG, RGBA | 544 × 184; lockup 480 × 120 at (32, 32) | 20,567 opaque, 1,866 partial (antialiased) | **primary logo**: mark `#8ccfff` + wordmark `#e9eef3` | reference |
| `doors-primary-dark.svg` | SVG, 6 paths | 544 × 184 | — | master | reference |
| `doors-primary-light.png` / `.svg` | PNG RGBA / SVG, `#06080b` | 544 × 184 | as above | primary logo for light backgrounds | reference |
| `doors-primary-mono.png` / `.svg` | PNG RGBA white / SVG `currentColor` | 544 × 184 | as above | tintable primary logo | reference |

### `icons/`

| Files | Format | Pixels | Alpha | Status |
| --- | --- | --- | --- | --- |
| `svg/{calculator,calendar,clock,fleet,notes,radar,radio,settings,system,timber}.svg` | SVG, one group: 1.5 stroke in `currentColor`, square caps, miter joins, no fill | 24 × 24 | — | deferred (masters) |
| `png-24/<same ten>.png` | PNG, RGBA, white | 24 × 24 | antialiased: 0–23 opaque and 142–228 partial pixels each | deferred |
| `png-32/<same ten>.png` | PNG, RGBA, white | 32 × 32 | antialiased: 59–232 opaque and 64–284 partial pixels each | deferred |

Mapping, by file name to `struct pocketos_app.id`: `radio`, `system`, `fleet`,
`radar`, `timber`, `notes`, `clock`, `calendar`, `calculator`, `settings`. The
eleventh app on master, **Wave (`wave`), has no icon in the package.**

### `mockups/` — reference only

All PNG RGB, opaque, rendered from the SVG beside them (outlined text).

| File | Pixels | What it shows |
| --- | --- | --- |
| `doors-launcher-568x1232` | 568 × 1232 | the launcher with the ten stroke icons in 32 px cells; ten tiles, so no Wave |
| `doors-launcher-selected-568x1232` | 568 × 1232 | one tile with the existing 2 px focus outline |
| `doors-settings-568x1232` | 568 × 1232 | a 10 × 12 mark beside the Appearance caption |
| `doors-status-bar-568x56` | 568 × 56 | the status bar with the `DOORS` caption: what Phase 1 already ships |
| `doors-system-about-568x1232` | 568 × 1232 | a logo-placement study; the package notes say it is illustrative, not the current System screen |

### `reference/` and notes

| File | Format | What it is | Status |
| --- | --- | --- | --- |
| `doors-mini-style-sheet.png` / `.svg` | PNG RGB 1600 × 1240 / SVG | style sheet: lockup, marks, icon family, boot and launcher thumbnails, the five theme accents | reference |
| `IBM-Plex-OFL.txt` | text, CRLF | SIL OFL 1.1 for IBM Plex. Same licence text as `docs/legal/fonts/IBMPlex-OFL-1.1.txt`, which the image already ships; only line wrapping differs | reference |
| `ASSET-NOTES.md` | Markdown | the supplier's notes: geometry, placement, theme accents | reference |

### Duplicates and obsolete variants

Nothing is marked obsolete by the package and nothing was removed. The
duplication is by design: each PNG is a render of the SVG beside it,
`doors-mark-dark` and `doors-mark-light` are `doors-mark` with a colour baked
in, and the mockup PNGs are renders of their SVGs. Doors draws the mark
through a tint, so only the white `doors-mark.png` is converted.

## Fonts, third-party artwork, licensing

- **No font files.** All lettering is IBM Plex Sans or Mono glyph outlines
  already converted to paths (`ASSET-NOTES.md`); no SVG references a font.
  Nothing needs a font at build or run time.
- **What ships contains no lettering except the splash wordmark.** The compact
  mark is one ten-point rectilinear polygon (`M0 24V0H20V24H10V20H16V4H4V24Z`).
  The splash carries the word "Doors" as outlined Plex Sans.
- **IBM Plex.** The OFL covers the font software. The splash contains outlines
  of five letters in a picture, not a font, so it is not a Modified Version
  and the Reserved Font Name "Plex" rule (docs/LICENSING.md) is not engaged:
  nothing presents a font called Plex. The image already carries the OFL text
  and IBM's copyright for the UI fonts (THIRD_PARTY_NOTICES.txt, `ibm-plex`),
  so the splash adds no notice obligation. That reading is the engineering
  view; the owner approves legal wording.
- **Third-party artwork: none found.** No embedded raster, logo or trademark
  artwork. The mockups print the device names "LILYGO T-Display K230" and
  "Kendryte K230" as text, which is description, not branding. The icons are
  simple geometric outlines; they were not compared against every public icon
  library, so their originality rests on the supplier.
- **The package's own licence.** None is stated. It is treated like the rest
  of Doors: the owner's material, no licence granted, not for external
  redistribution until the owner decides (docs/LICENSING.md).

## Where the assets go

| Doors UI | Asset | Result |
| --- | --- | --- |
| Boot splash | `boot/doors-boot-568x1232.png` | `platforms/k230/rootfs_overlay/logo.xrgb`, see Boot splash |
| System identity | `brand/doors-mark.png` | `ui/pocketui/pos_brand_mark.c`, see System identity |
| Status bar | none | unchanged: `mockups/doors-status-bar-568x56` shows the `DOORS` caption Phase 1 already ships |
| Launcher | none | deferred, see Launcher |
| App icons | `icons/` | deferred, see App icons |
| Settings, Appearance | `brand/doors-mark-16` | not in scope: not requested, mockup only |

## Boot splash

U-Boot on the T-Display K230 shows `/logo.xrgb` from the boot partition, full
screen on OSD layer 4, before Linux starts (VERIFIED on unit A:
docs/hardware/BRINGUP_SESSION_2026-09-07.md). The file has no header and must
be exactly **568 × 1232 × 4 = 2,799,104 bytes**, or U-Boot prints
`logo.xrgb size mismatch` and shows nothing (DOCUMENTED: vendor U-Boot overlay,
`board/canaan/common/logo/k230_logo.c`). The vendor splash that booted on unit
A is 2,799,104 bytes.

**Byte order: DRM XRGB8888, stored `B, G, R, X` per pixel**, rows from the
top, pixels from the left. Each pixel is the little-endian word `0xXXRRGGBB`.
DOCUMENTED, not yet seen on glass for this file: U-Boot programs the layer
with format `0x03`, DMA map `0x40` and address mode `0x1100`
(`display_logo.c`, `vo_osd4_logo_test`), the same three values the kernel's
`canaan_vo.c` writes for `DRM_FORMAT_XRGB8888` and for no other format. The X
byte is written `0xFF`, as in every pixel of the vendor file.

`tools/design/png2xrgb.py SOURCE.png OUTPUT.xrgb` converts and
`png2xrgb.py --check FILE.xrgb` validates. Standard library only. The
conversion copies stored pixel values and nothing else: no scaling, cropping,
rotation, dithering or colour management. It refuses a source that is not
568 × 1232 portrait, not 8-bit RGB/RGBA, not fully opaque, interlaced, or
tagged with an ICC profile, gamma or chromaticities, and it never writes over
its source. `tests/boot_splash_test.sh` (in `make test`) proves the byte
order, the row order, every PNG filter type, the refusals and determinism.

**What ships.** `platforms/k230/rootfs_overlay/logo.xrgb`, made by

```sh
python3 tools/design/png2xrgb.py docs/design/brand/doors-threshold/boot/doors-boot-568x1232.png \
    platforms/k230/rootfs_overlay/logo.xrgb
```

| | |
| --- | --- |
| Source | `boot/doors-boot-568x1232.png`, SHA-256 `e98aacc22f8e9fd8ab18dc20bd8219021ccfd7bba6f7851fadfdb4ff537a1690` |
| Splash | 568 × 1232 portrait, XRGB8888 stored B, G, R, `0xFF`, no header |
| Size | 2,799,104 bytes |
| SHA-256 | `434f4a6cf697544764ccbd111d4b3a60eaf5ced5f86e41be731f1f7ce4f58f94` |
| Proof | decoded back to RGB, it equals the source's pixels exactly; content box x 184–383, y 517–715, as in the source |

The test fails if the splash is not exactly the conversion of that source, if
either hash changes, or if `git archive` would ship it altered
(`tests/package_sync_test.sh`). `.gitattributes` marks `*.xrgb` binary.

**How it reaches the board.** `apply_to_sdk.sh` merges
`platforms/k230/rootfs_overlay` into the vendor's
`board/canaan/k230-soc/rootfs_overlay`, replacing the vendor `logo.xrgb`, and
the vendor `post-image.sh` copies that file into the boot partition (DOCUMENTED,
`gen_boot_ext4`). As with the vendor file, Buildroot also copies it into the
root filesystem as `/logo.xrgb`, which nothing reads. Consequences:

- Only a flashed image changes the splash. `deploy.sh` does not write the boot
  partition.
- The apply never deletes from the vendor overlay, so an SDK tree applied from
  this commit keeps the Doors splash even if an older commit is applied later.
  Reset the SDK tree to get the vendor splash back.
- **Not yet seen on glass.** The size is certain. The byte order and
  orientation are DOCUMENTED, not VERIFIED, until a unit boots an image with
  this file.

## System identity

The System app's identity panel starts with the row "Doors  0.0.9 · <build>".
That row now carries the compact mark before the name. DS §19 (Amendment C,
proposed, pending the owner's approval) is the rule for it.

| | |
| --- | --- |
| Source | `brand/doors-mark.png`, SHA-256 `5540077f5a0c14b1e56f629983fbbf4eacf55ad8da3d321944240d0ded432714` |
| Generated | `ui/pocketui/pos_brand_mark.c` by `tools/design/gen_brand_mark.py`, committed like the fonts |
| Format | LVGL `LV_COLOR_FORMAT_A8`, 20 × 24, the 32 × 32 canvas trimmed at (6, 4) with only transparent pixels dropped; 264 px lit, no partial alpha; not scaled |
| Colour | none in the asset. `POS_STYLE_BRAND_MARK` sets `image_recolor` to `accent_primary`, and LVGL draws an A8 image in its recolour, so it follows all five themes and the three display modes, live |
| Layout | the key/value row is unchanged: 64 px, divider, body label, value style. The mark sits at the panel's content edge, 8 px before the name, so only the name moves, by 28 px. No other screen changes |
| Motion | none, so reduced motion changes nothing |

Tests:

- `tests/brand_mark_test.sh` (make test): the committed C file is the
  generator's output for the committed PNG; the mask is pixel-exact to the
  SVG path rasterised independently; the generator refuses the fixed-colour
  variant, an image without alpha and an empty one; only the System app uses
  the mark, through its role.
- `tests/system_brand_shell_test.sh` (shell tests): finds the compiled mask in
  screenshots, in the accent `themes.json` gives for each of the 15 theme and
  mode pairs, at the content edge (x 41, or 42 with Outdoor's 2 px hairline),
  with clear space, the 8 px gap and the name after it; again after a live
  switch to carbon/night; again, unmoved, with reduced motion. Checked against
  three deliberate breakages (mark in `text_primary`, a 12 px gap, the old
  plain row), each of which fails it.

On the device: LVGL 9.5.0 with `LV_DRAW_SW_SUPPORT_A8 1` and `LV_USE_IMAGE 1`
(VERIFIED in the SDK sysroot's `lv_conf.h`); PocketTimber's contact shadow
uses the same recoloured-A8 drawing. **Not yet seen on glass.**

## Launcher — deferred

Nothing in the package puts a logo in the launcher: its launcher mockups keep
the existing grid and add only the stroke icons, and the status bar keeps the
`DOORS` caption. A launcher logo would need room above or inside the grid,
which changes launcher geometry (DS §14 C7) and needs a DS amendment, so none
is added.

The launcher mockups show ten tiles. Master has eleven apps (Wave was added
after the mockups were drawn), so they are not a layout for today's launcher.

## App icons — deferred

The ten icons are inventoried above and map one-to-one onto app ids. They are
not integrated, because every route to showing them changes an interface this
session must not touch:

1. **App API.** `struct pocketos_app.icon` is `const char *`, an `LV_SYMBOL_*`
   glyph or short text (`ui/shell/app.h`). An image needs either a new field,
   with a decision on `POCKETOS_APP_API_VERSION`, or a shell-owned table from
   `app.id` to image that leaves the API alone.
2. **PocketUI.** `pocketui_tile()` takes the icon as text and draws it as a
   label in `POS_STYLE_SYMBOL_LARGE` (Montserrat 32, which the style's own
   comment calls temporary "until DS icons"). An image tile needs a new
   variant, tinted through a style role as the System mark is.
3. **Design System.** DS §11 sets app icons on a 24 px grid at 1.5 px, and
   §14 C7 puts a 24 px icon in the tile. The package draws them in a 32 px
   cell at 2 px (the 24 px master scaled 4/3). Either `png-24` is used, which
   matches the DS but is smaller than the mockups, or the DS is amended for
   32 px.
4. **Conversion.** No SVG rasteriser is available to a standard-library build,
   so the PNGs are the practical source. They are white on transparent and
   antialiased, so each converts to an A8 mask from its alpha channel with no
   loss, by the same method as the System mark.
5. **Wave has no icon.** The owner needs to supply one before the set is
   complete.
6. **Validation.** Screenshots in all five themes and the three display modes,
   and one look on the device, where LVGL renders to RGB565.
