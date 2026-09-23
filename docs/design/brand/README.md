# Doors brand assets

The artwork for the product name Doors (ADR-005), brand direction
**Threshold**: doorway, portal, opening, transition. This file is the
inventory of the graphics packages, what was checked in them, and where each
asset is used in the Doors UI.

A later delivery, the DOORS visual pack v1 (photographic boot, lock, open,
launcher and system-menu backgrounds, direction-B app and system icons,
supplied 2026-09-22), is imported in `doors-visual-pack-v1/` and is not used by
anything yet. Its own README is its inventory; the rest of this file is about
the two packages below.

## Provenance

Two packages, both from the product owner. Together they are the current Doors
artwork: the extension adds to the Threshold package and replaces none of it.

| | Threshold package | Icon extension |
| --- | --- | --- |
| Supplied | 2026-09-15, for the graphics integration session | 2026-09-15, for the app-icons milestone, as the latest package with the missing Wave icon |
| Archive | `Doors-Threshold-Production.zip`, 453,639 bytes, SHA-256 `8e0f25b81d4edf1202cfe90a966f8a2dabf5d9dae18ac14e8576b79466481620` | `Doors-Wave-Future-Icons.zip`, 145,561 bytes, SHA-256 `87e7b50b0bb751d80a2699f2cc6785c7cff233d1ba1e37e07eafe059c7f8a258` |
| Contents | 60 files, dated 2026-09-13 inside the archive | 43 files under `Doors-Icon-Extension/`, dated 2026-09-14 |
| Kept as | `doors-threshold/` | `doors-icon-extension/` |
| Notes by the supplier | `doors-threshold/ASSET-NOTES.md` | `doors-icon-extension/LES-MEG.md` (Norwegian) |
| Author, copyright, licence | not stated | not stated |

Both are kept byte-for-byte as supplied. `.gitattributes` exempts both folders
from end-of-line conversion, so every hash below is the hash of the committed
file. Nothing in them is edited. A change to the artwork is a new package from
the owner, and the files and hashes here are replaced together.

**How the two relate** (checked file by file): no file in the extension has
the name or the content of a Threshold file except `IBM-Plex-OFL.txt`, which
is byte-identical. The extension carries none of the ten earlier app icons, so
none of them changed; its overview draws the Radio and Radar icons with the
Threshold SVG paths verbatim. Nothing in the Threshold package is superseded.

## Inventory

Every PNG in both packages was decoded in full (header, chunk CRCs, every
pixel). Every PNG is 8-bit and not interlaced, with no ICC profile, gamma or
chromaticity chunk. The Threshold PNGs carry only `IHDR`, `IDAT` and `IEND`;
the extension's also carry `pHYs` and a `tEXt` `Software=www.inkscape.org`,
which say nothing about pixels. Every SVG is vector shapes only: no `<text>`,
`<image>`, `<style>`, `<font>`, `@font-face`, `font-family`, `href`, `url(`,
base64 data, script, gradient or filter.

The Threshold package, `doors-threshold/`:

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
| `svg/{calculator,calendar,clock,fleet,notes,radar,radio,settings,system,timber}.svg` | SVG, one group: 1.5 stroke in `currentColor`, square caps, miter joins, no fill | 24 × 24 | — | reference (masters) |
| `png-24/<same ten>.png` | PNG, RGBA, white | 24 × 24 | antialiased: 0–23 opaque and 142–228 partial pixels each; none opaque in six | not used: soft at this size, see App icons |
| `png-32/<same ten>.png` | PNG, RGBA, white | 32 × 32 | antialiased: 59–232 opaque and 64–284 partial pixels each | **used**: the launcher's app icons |

Mapping, by file name to `struct pocketos_app.id`: `radio`, `system`, `fleet`,
`radar`, `timber`, `notes`, `clock`, `calendar`, `calculator`, `settings`. The
eleventh app, Wave (`wave`), has no icon in this package; its icon is in the
extension.

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

### Icon extension, `doors-icon-extension/`

The Wave icon and twelve icons for apps Doors does not have. The supplier's
notes (`LES-MEG.md`, Norwegian) say: Wave is for communication by sound tones,
a compact waveform between two signal arcs, drawn to read apart from Radio
(antenna) and Radar (search circle); the twelve are proposals for future apps,
whose names describe uses and imply no new features and no change to the
launcher order; the family, stroke and themes are the existing Threshold ones;
existing UI, typography and launcher are unchanged.

| Files | Format | Pixels | Alpha | Status |
| --- | --- | --- | --- | --- |
| `svg/wave.svg` | SVG, 1 path, 1.5 stroke in `currentColor`, square caps, miter joins, no fill | 24 × 24 | — | reference (master) |
| `png-24/wave.png` | PNG, RGBA, white | 24 × 24 | 8 opaque, 116 partial | not used: soft at this size |
| `png-32/wave.png` | PNG, RGBA, white | 32 × 32 | 57 opaque, 139 partial; ink x 1–30, y 8–23 | **used**: Wave's launcher icon |
| `svg/{messages,contacts,files,map,compass,authenticator,bluetooth,wifi-scanner,recorder,camera,gallery,terminal}.svg` | SVG, paths, circles and rectangles in one group, same stroke rules | 24 × 24 | — | **deferred**: no such app exists |
| `png-24/<same twelve>.png` | PNG, RGBA, white | 24 × 24 | 0–26 opaque, 116–218 partial | **deferred** |
| `png-32/<same twelve>.png` | PNG, RGBA, white | 32 × 32 | 57–225 opaque, 92–259 partial | **deferred** |
| `Doors-Icon-Extension.png` / `.svg` | PNG RGB 1120 × 980 / SVG | overview: Wave at 24 and 32 px, the twelve, Wave beside Radio and Radar, the five theme accents; lettering outlined | reference |
| `IBM-Plex-OFL.txt` | text, CRLF | byte-identical to `doors-threshold/reference/IBM-Plex-OFL.txt` | reference |
| `LES-MEG.md` | Markdown, Norwegian | the supplier's notes, summarised above | reference |

The twelve deferred icons are not compiled into anything. One of them becomes
a launcher icon when its app exists, by the route Wave took (DS §20).
`wifi-scanner` has a hyphen, which is not a valid app id or C name; that app
would need an id such as `wifi_scanner` and the file mapped to it explicitly.

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
- **The extension.** Its overview sheet has lettering as outlined IBM Plex,
  the same case as the Threshold style sheet, and nothing from it ships. The
  Wave icon, like the other app icons, has no lettering.
- **The packages' own licence.** None is stated. They are treated like the
  rest of Doors: the owner's material, no licence granted, not for external
  redistribution until the owner decides (docs/LICENSING.md).

## Where the assets go

| Doors UI | Asset | Result |
| --- | --- | --- |
| Boot splash | `boot/doors-boot-568x1232.png` | `platforms/k230/rootfs_overlay/logo.xrgb`, see Boot splash |
| System identity | `brand/doors-mark.png` | `ui/pocketui/pos_brand_mark.c`, see System identity |
| Status bar | none | unchanged: `mockups/doors-status-bar-568x56` shows the `DOORS` caption Phase 1 already ships |
| Launcher, brand | none | no logo on the launcher, see Launcher |
| Launcher, app icons | `doors-threshold/icons/png-32/` (ten), `doors-icon-extension/png-32/wave.png` | `ui/pocketui/pos_app_icons.c`, see App icons |
| Future apps | `doors-icon-extension/` (twelve icons) | deferred: no such apps |
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
VERIFIED on unit A on 2026-09-15 (right colours and orientation on the panel);
the source basis: U-Boot programs the layer
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
either hash changes, or if the shipped file decoded as B, G, R rows from the
top is not the source's pixels. `tests/package_sync_test.sh` fails if
`git archive` would ship it altered. `.gitattributes` marks `*.xrgb` binary.

**In a built image.** `platforms/k230/scripts/verify_splash.sh <image>` reads
`/logo.xrgb` out of partition 1 and passes only when it is exactly 2,799,104
bytes and byte-identical to the committed splash. `tests/splash_image_test.sh`
(in `make test`) proves it refuses a missing splash, a 2,798,848-byte one and
one of the right size with different pixels. It is separate from
`verify_image.sh`, whose question is only whether the image can boot; a wrong
splash still boots.

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
  To get the vendor splash back, copy
  `k230_bsp/overlay/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/logo.xrgb`
  (SHA-256 `9fd79fee…`) over the SDK's copy.
- **Seen on glass** on unit A, 2026-09-15 (docs/hardware/DOORS_GRAPHICS_GATE.md).
  From power-on the splash shows and hands over cleanly to the Doors shell. After
  a warm `reboot` the vendor U-Boot's first panel bring-up leaves the panel
  dark, so no splash is seen then: a vendor boot-path limitation, not a fault in
  this file (docs/KNOWN_ISSUES.md).

### Orientation and channel order: what is known

| Claim | Class | Basis |
| --- | --- | --- |
| U-Boot loads `/logo.xrgb` and accepts only 2,799,104 bytes | DOCUMENTED; load VERIFIED | `k230_logo.c`; unit A's U-Boot log prints `2799104 bytes read` and `RM69A10 direct XRGB8888 logo.xrgb full-screen OSD4` for this file |
| The splash in a built image is the committed file | VERIFIED by build and on the device | `verify_splash.sh` on the image; `sha256sum /boot/logo.xrgb` on unit A |
| Bytes are B, G, R, X per pixel | VERIFIED (operator, unit A) | the mark shows light blue, the word near-white, on black; source basis: U-Boot's OSD4 values equal `canaan_vo.c`'s for `DRM_FORMAT_XRGB8888` only |
| Row 0 is the top of the panel in portrait, pixel 0 its left edge, no mirroring | VERIFIED (operator, unit A) | portrait and upright, not mirrored; source basis: U-Boot sets the layer to exactly 568 × 1232 from the display zone's origin with no rotation (`vo_osd4_logo_test`) |
| No clipping or stretching | VERIFIED (operator, unit A) | centred, nothing cut off; 1:1 file, layer size, window and stride set from the same 568 × 1232 |
| Earlier supporting evidence | INFERRED, now superseded | the vendor's own splash, decoded with this convention, reads upright with natural colours |
| Splash after a warm reboot | vendor limitation | U-Boot's first DSI bring-up after a warm reset leaves the panel dark; running the same init again lights this splash (docs/KNOWN_ISSUES.md) |

## System identity

The System app's identity panel starts with the row "Doors  <version> · <build>".
That row now carries the compact mark before the name. DS §19 (Amendment C,
accepted 2026-09-15) is the rule for it.

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
  plain row), each of which fails it. With `SHOTS_DIR=<dir>` it also keeps
  the screenshots and writes a contact sheet of the identity row.

`shots/system-mark-contact.png` is that sheet from the SDL simulator: columns
ice, brass, olive, slate, carbon; rows Normal, Outdoor, Night. Night is dim on
purpose (DS §13). A desktop render, not the AMOLED panel.

On the device: LVGL 9.5.0 with `LV_DRAW_SW_SUPPORT_A8 1` and `LV_USE_IMAGE 1`
(VERIFIED in the SDK sysroot's `lv_conf.h`); PocketTimber's contact shadow
uses the same recoloured-A8 drawing. **VERIFIED on unit A, 2026-09-15**
(operator): crisp, in front of "Doors", the back arrow's accent in all five
themes, legible in Outdoor and Night (docs/hardware/DOORS_GRAPHICS_GATE.md).

## Launcher

Nothing in the package puts a logo in the launcher: its launcher mockups keep
the existing grid and add only the stroke icons, and the status bar keeps the
`DOORS` caption. A launcher logo would need room above or inside the grid,
which changes launcher geometry (DS §14 C7) and needs a DS amendment, so none
is added.

The launcher mockups show ten tiles. Master has eleven apps (Wave was added
after the mockups were drawn); the launcher keeps its eleven tiles in its own
order, and the icons land in the same cells the mockups draw them in.

## App icons

Branch `rebrand/doors-app-icons`. DS §20 (Amendment D, **accepted
2026-09-15** after the unit A visual gate,
`docs/hardware/DOORS_APP_ICONS_GATE.md`) is the rule.

| App (`id`) | Launcher icon | Source |
| --- | --- | --- |
| Radio (`radio`) | `pos_app_icon_radio` | `doors-threshold/icons/png-32/radio.png` |
| System (`system`) | `pos_app_icon_system` | `doors-threshold/icons/png-32/system.png` |
| Fleet (`fleet`) | `pos_app_icon_fleet` | `doors-threshold/icons/png-32/fleet.png` |
| Radar (`radar`) | `pos_app_icon_radar` | `doors-threshold/icons/png-32/radar.png` |
| Timber (`timber`) | `pos_app_icon_timber` | `doors-threshold/icons/png-32/timber.png` |
| Notes (`notes`) | `pos_app_icon_notes` | `doors-threshold/icons/png-32/notes.png` |
| Clock (`clock`) | `pos_app_icon_clock` | `doors-threshold/icons/png-32/clock.png` |
| Calendar (`calendar`) | `pos_app_icon_calendar` | `doors-threshold/icons/png-32/calendar.png` |
| Calculator (`calculator`) | `pos_app_icon_calculator` | `doors-threshold/icons/png-32/calculator.png` |
| Settings (`settings`) | `pos_app_icon_settings` | `doors-threshold/icons/png-32/settings.png` |
| Wave (`wave`) | `pos_app_icon_wave` | `doors-icon-extension/png-32/wave.png` |

Every launcher app has its icon; no tile shows a glyph. Each app keeps its
`LV_SYMBOL_*` text icon in its descriptor, which the launcher no longer draws.

| | |
| --- | --- |
| Generated | `ui/pocketui/pos_app_icons.c` by `tools/design/gen_app_icons.py`, committed like the fonts; the header names each source file and its SHA-256. The generator's default sources are exactly the eleven files above, so none of the extension's icons for apps that do not exist is compiled in |
| Format | LVGL `LV_COLOR_FORMAT_A8`, 32 × 32 each, the whole canvas (not trimmed, so every icon keeps its place in the cell), alpha kept exactly with its antialiasing, not scaled |
| Why `png-32` | both packages' launcher size; at 24 px the 1.5 px strokes cover almost no whole pixel (none in six of the eleven, at most 23 in the rest), so the icons would be soft; 32 px is the cell the glyph icons filled before, so the tile does not change. DS §20.2 records it |
| Why not SVG | no SVG rasteriser in a standard-library build; the PNGs are the packages' own renders of the SVGs, white on transparent, so the alpha channel is the icon |
| Colour | none in the asset. `POS_STYLE_APP_ICON` sets `image_recolor` to `accent_primary` at full cover, the accent the glyph icons had, so the icons follow all five themes and three display modes, live |
| Data model | `struct pocketos_app` gains one appended, optional field `const lv_image_dsc_t *icon_mask`; `icon` stays. `POCKETOS_APP_API_VERSION` stays 0 |
| PocketUI | `pocketui_tile_mask(parent, mask, icon, label, cb, user)`: `pocketui_tile()` with the mask as an image in the icon's place; a NULL mask builds exactly the glyph tile, and `pocketui_tile()` is now that call |
| Geometry | unchanged: 254 × 150 tile, 12 px inset, icon top-left, label bottom-left, 2 columns, 20 px gutter; the icon is not clickable and joins no focus group |
| Size | 11,264 bytes of mask data (`.rodata`) plus eleven 40-byte descriptors on a 64-bit build; no heap for the pixels, because LVGL 9.5's image decoder hands an uncompressed A8 variable to the renderer in place (`lv_bin_decoder.c`) and the image cache is off (`LV_CACHE_DEF_SIZE 0`). Measured in `docs/hardware/DOORS_APP_ICONS_GATE.md` |
| Motion | none, so reduced motion changes nothing |

Tests:

- `tests/app_icons_test.sh` (make test): the eleven sources by path and hash,
  named in the C file; the committed C file is the generator's output; each
  mask equals its PNG's alpha decoded by a second reader; only Wave comes from
  the extension; the refusals (a colour under a visible pixel, no alpha,
  nothing visible, mixed sizes, a file name that is not an app id, two icons
  for one app); each launcher app's descriptor points at its own mask and none
  is without one; the masks are used nowhere else and the brand mark is not an
  app icon; the tile's image path, and its glyph path kept as the API's
  fallback; the role's colour.
- `tests/pocketui_tile_test.c` (run by the shell test): a mask tile and a
  glyph tile built side by side under a real pointer device: same tile and
  label boxes, icon at the 12 px inset, 32 × 32 unscaled, no clickable icon,
  no focus-group change, a tap on the icon, the label and the empty middle
  opens the app and one in the gutter does not, a press on the icon presses
  the tile, and the tint equals `accent_primary` and the glyph's colour in all
  15 theme and mode pairs.
- `tests/launcher_icons_shell_test.sh` (shell tests; retired with the tile
  launcher by DS §31, whose launcher `tests/doors_shell_test.sh` checks): in the running shell, for
  all 15 pairs, every tile's box, each app's own mask found at its tile's icon
  origin blended from `surface` to `accent_primary` (max error 2 measured,
  tolerance 4; the nearest wrong mask is 99 away), clear tile around it, the
  empty band, the label from the inset, and a failure for any tile without a
  mask; again after a live switch to carbon/night and after opening all
  eleven apps and coming home; with reduced motion the launcher is
  pixel-identical. With `SHOTS_DIR=<dir>` it keeps the screenshots and writes
  two contact sheets.

`shots/launcher-contact.png` is that run's launchers at half size, columns ice,
brass, olive, slate, carbon, rows Normal, Outdoor, Night.
`shots/launcher-icons-contact.png` is every tile's icon cell at twice size:
rows the 15 pairs in the same order, columns the eleven apps in launcher order,
Wave last. SDL simulator renders, not the AMOLED panel.

On the device: the same LVGL A8 drawing as the System mark, into RGB565.
**VERIFIED on unit A, 2026-09-15** (operator, build `3174471` deployed): all
eleven launcher apps show their Doors icon, Wave its waveform and no speaker
symbol, crisp and in place with nothing else moved; the icons tint and stay
legible through Ice & Ember / Normal, Carbon & Signal Orange / Outdoor and
Slate & Lavender / Night; a tap on the Wave icon opens Wave and Back returns
to the launcher (docs/hardware/DOORS_APP_ICONS_GATE.md).
