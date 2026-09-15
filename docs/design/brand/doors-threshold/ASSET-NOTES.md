# Doors / Threshold — production assets

Approved identity, prepared from the supplied 2026-09-13 CONTEXT.md and screenshots. This package contains artwork only.

## Contents

| Folder | Contents |
| --- | --- |
| `boot` | Native 568 × 1232 PNG and self-contained vector SVG source |
| `brand` | Primary dark/light/monochrome lockups; tintable compact mark; dark/light marks; 16 px mark |
| `icons/svg` | Ten separate transparent, single-colour SVG masters |
| `icons/png-24` | Ten white-on-transparent 24 × 24 PNGs |
| `icons/png-32` | Ten white-on-transparent 32 × 32 PNGs, matching the launcher cell |
| `mockups` | Native-size launcher, selected-tile example, status bar, Settings and System/About applications; SVG + PNG |
| `reference` | Compact style sheet; SVG + PNG |

## Artwork specification

- **Mark:** 20 × 24 unit silhouette; 4-unit stems, 12-unit inner aperture, 6-unit lower-left threshold gap. Flat geometry, square corners. Minimum clear space: one stem width. The 32 px mark canvas places the 20 × 24 mark at (6, 4); the 16 px version uses integer edges and a 3 px threshold gap.
- **Primary lockup:** 100 × 120 px mark, 32 px horizontal gap, 348 px wide outlined wordmark. The 544 × 184 canvas includes 32 px outer clear space. Dark version uses the existing Ice accent and primary text colour. Light version is dark monochrome for contrast.
- **Icons:** 24 × 24 SVG viewBox, 1.5-unit geometric outline, square caps and miter joins. The same master renders at 32 × 32 with a 2 px stroke. No tile backgrounds, shadows or gradients are baked into icon assets.
- **Tint:** icon SVGs and monochrome brand SVGs use `currentColor`; their standalone default is black. Transparent PNGs of those assets use neutral white, independent of any theme. Dark/light brand variants have explicit foreground colours and transparent backgrounds.
- **Typography:** IBM Plex Sans and IBM Plex Mono. All lettering in the delivered SVGs is outlined, so preview rendering does not depend on locally installed fonts. Existing UI sizes and roles remain unchanged.

## Native boot composition

`boot/doors-boot-568x1232.png` is an opaque RGB PNG, exactly 568 × 1232 px, with pure black background. Its SVG has the same width, height and viewBox, contains only vectors, and has no external font/image dependencies.

The mark is 100 × 120 px at (234, 517). The wordmark is 200 px wide, starts at x = 184 and y = 661, and has approximately 54.12 px visible height. There is a 24 px gap below the mark. The visible group is centred horizontally and vertically to within one pixel. Colours are #000000, #8ccfff and #e9eef3, with antialiasing at letter edges. No loading indicator or small text is present. This was composed from vector geometry, not stretched from the earlier concept raster.

No raw XRGB8888 file or conversion is included.

## Existing UI application

The launcher retains the supplied row order: **Radio / System; Fleet / Radar; Timber / Notes; Clock / Calendar; Calculator / Settings**. It uses the existing 56 px status bar, 20 px margins/gaps, 254 × 150 px tiles, radius 6, 12 px tile inset, 32 px icon cell and IBM Plex Sans 20 labels. The last row ends at y = 906, leaving 326 px below.

The separate selected-tile image demonstrates the existing 2 px focus outline with the same fill and unchanged icon. It does not imply new keyboard navigation. Status branding remains the small `DOORS` mono caption. Settings retains its supplied sections and positions; a 10 × 12 px Threshold mark appears beside the Appearance caption, without moving its text or controls.

The System/About image is a restrained logo-placement study using the existing shell and component grammar. No System screenshot was supplied; its interior arrangement is illustrative, not a claim to reproduce the current System screen. Hardware values shown come from CONTEXT.md.

## Existing theme accents

| Theme | `accent_primary` |
| --- | --- |
| Ice & Ember | #8ccfff |
| Brass & Verdigris | #63c1ad |
| Olive & Chalk | #e6e2d6 |
| Slate & Lavender | #b7a6ff |
| Carbon & Signal Orange | #ff7a1a |

Icons and UI marks inherit the existing theme accent; foreground wordmarks inherit primary text. Preserve existing Normal, Outdoor and Night behaviour and all status/RX/TX semantics. No new palette or per-game icon colour is introduced. Timber's in-game wood and felt artwork is unchanged.

## Verification and credits

PNG dimensions, transparency, SVG parsing, boot bounds and the absence of external SVG dependencies were checked. Icons were visually inspected at 32 px in Normal and Night colours for all five themes. Mockups are desktop vector renders, not firmware or physical AMOLED captures; they do not reproduce LVGL bitmap-font rasterization exactly.

Typography: IBM Plex, IBM Corp., SIL Open Font License 1.1. See `reference/IBM-Plex-OFL.txt`. Font outlines are included in the artwork; installable font files are not included.
