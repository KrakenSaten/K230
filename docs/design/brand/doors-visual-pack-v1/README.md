# DOORS visual pack v1

Full-screen photographic backgrounds (boot, lock, open, launcher, system menu)
in both orientations, the direction-B app icons and system icons, and the UI
layer specifications that go with them.

**Imported only.** Nothing in this folder is used by the build, the image or
any UI. Nothing was converted, resized or re-encoded. Nothing was seen on the
panel: every claim below is about files, not about unit A.

**Integrated 2026-09-22 (branch `feat/doors-visual-refresh`, DS §31,
PROPOSED).** This folder is still never shipped and nothing in it was
changed. `tools/design/gen_doors_ui.py` derives the runtime art from it into
`ui/assets/doors/` (backgrounds from `device/backgrounds`, portal icons
redrawn from `source/app-icons/svg` and the glyph SVGs, glyphs from
`device/system-icons/png32`), and `ui/assets/doors/MANIFEST.txt` records the
hash of every source used. The "Before implementation" points below were
decided there: the app icons are drawn per app (not per category) in the
package's frame and hues, the layouts were re-measured in the Doors fonts,
and the launcher and system menu have their own amendment (§31).

## Provenance

Two archives from the product owner, supplied together on 2026-09-22.

| | Visual package v1 | B production package v2 |
| --- | --- | --- |
| Archive | `DOORS_Visual_Package_v1.zip`, 34,774,236 bytes, SHA-256 `c4782ebee0622a68a5b54629d6c28cc0d1add94535065505182a36519ba1ff5d` | `DOORS_B_Production_Package_v2.zip`, 22,623,682 bytes, SHA-256 `1eb0533fba3fbbf8f7878afeb32adee6518d08b0e950e6934a7ae6958f8a61c9` |
| Contents | 51 files (34 PNG, 8 SVG), dated 2026-09-21 | 132 files (87 PNG, 36 SVG), dated 2026-09-21 |
| What it is | boot, lock, open and a text-only 3 × 3 launcher (direction A) | the selected direction **B** ("Framed spaces"): grouped launcher, system menu, 9 app icons, 17 system icons; carries v1's boot, lock and open forward unchanged |
| Kept as | `originals/DOORS_Visual_Package_v1/` | `originals/DOORS_B_Production_Package_v2/` |
| Supplier's notes | `START_HERE.md` (Norwegian), `INTEGRATION.md`, `PROVENANCE.md`, `PROJECT_HANDOFF_PROMPT.md` | `START_HERE.md` (Norwegian), `INTEGRATION.md`, `PROJECT_HANDOFF_PROMPT.md` |
| Author, copyright, licence | not stated | not stated |

Every file matches its supplier `asset_manifest.json` in bytes, SHA-256 and
pixel size. The photographic artwork is AI-generated (the supplier's
`PROVENANCE.md`; the eight masters and `approved_B.png` also carry a C2PA
content-credentials chunk, `caBX`, that names "OpenAI Media Service"). Like the rest of Doors, the
packages are the owner's material with no licence granted and not for
external redistribution (docs/LICENSING.md). The `PROJECT_HANDOFF_PROMPT.md`
files ask for an implementation; that is **not** done here.

## Folder layout

```
doors-visual-pack-v1/
├── README.md        this manifest
├── SHA256SUMS       every file under device/ source/ reference/ originals/ (sha256sum -c)
├── device/          device-ready: native pixel size, candidates for conversion into the image
│   ├── backgrounds/{portrait,landscape}/{boot,lock,open,launcher}.png
│   ├── app-icons/png{48,64,96,128}/<9 icons>.png
│   └── system-icons/png32/<17 icons>.png
├── source/          master artwork: never shipped, input for any re-export
│   ├── backgrounds/{portrait,landscape}/{boot,lock,open,launcher}.png
│   ├── app-icons/svg/<9 icons>.svg
│   └── system-icons/svg/<17 icons>.svg
├── reference/       UI specifications, not assets
│   ├── ui-layers/{portrait,landscape}/{boot,lock,open,launcher,system}.svg
│   └── layout/{b_ui_layout.json,base_ui_layout.json}
└── originals/       both packages exactly as supplied (183 files)
```

Every file in `device/`, `source/` and `reference/` is a byte copy of one file
in `originals/` (table below), so none of them adds to the repository's size.
When the packages disagree, B wins: it is the selected direction. Each file is
copied once: a byte-identical duplicate is listed under Duplicates instead.
`.gitattributes` exempts this folder from end-of-line conversion so the hashes
stay true.

| Organised | Copied from |
| --- | --- |
| `device/backgrounds/<o>/<s>.png` | `DOORS_B_Production_Package_v2/backgrounds/<o>/<s>.png` (= v1's) |
| `device/app-icons/png<n>/*.png` | `DOORS_B_Production_Package_v2/icons/png<n>/` |
| `device/system-icons/png32/*.png` | `DOORS_B_Production_Package_v2/glyphs/png32/` |
| `source/backgrounds/<o>/<s>.png` | `DOORS_Visual_Package_v1/masters/<o>/<s>.png` |
| `source/app-icons/svg/*.svg` | `DOORS_B_Production_Package_v2/icons/svg/` |
| `source/system-icons/svg/*.svg` | `DOORS_B_Production_Package_v2/glyphs/svg/` |
| `reference/ui-layers/<o>/*.svg` | `DOORS_B_Production_Package_v2/overlays/<o>/*.svg` (boot, lock, open = v1's) |
| `reference/layout/*.json` | `DOORS_B_Production_Package_v2/{b_ui_layout,base_ui_layout}.json` |

## Device-ready assets

All PNGs here are 8-bit, not interlaced, decoded in full with chunk CRCs
checked (`tools/design/pngstrict.py`).

### Backgrounds: static, opaque, full screen

8-bit RGB, no alpha, only `IHDR`/`IDAT`/`IEND` (no ICC, gamma or
chromaticity chunk). No text, clock, status or icon is baked in; all of that
is live UI drawn over them (`reference/`). Portrait and landscape are separate
compositions, not rotations; every file was viewed and is upright.

| Category | File | Use | Pixels | Bytes | SHA-256 |
| --- | --- | --- | --- | --- | --- |
| Boot | `portrait/boot.png` | closed door, warm light seam; DOORS wordmark, tagline and startup status drawn over it | 568 × 1232 portrait | 649,947 | `c6394bf46fd2bb3d…` |
| Boot | `landscape/boot.png` | same, landscape composition | 1232 × 568 landscape | 809,097 | `dcdcfec8c26e4a02…` |
| Lock | `portrait/lock.png` | closed door; clock, date, status and unlock hint drawn over it | 568 × 1232 portrait | 787,001 | `8da11d019d17d728…` |
| Lock | `landscape/lock.png` | same | 1232 × 568 landscape | 824,815 | `0e613324e3b5b151…` |
| Unlock transition | `portrait/open.png` | end state after unlock: doors open onto a mountain lake; small clock and tagline | 568 × 1232 portrait | 1,002,314 | `8629239e1ca5b091…` |
| Unlock transition | `landscape/open.png` | same | 1232 × 568 landscape | 1,037,580 | `ba5eba764fdf8dbe…` |
| Launcher, System menu | `portrait/launcher.png` | darkened open scene under the B launcher panels and the B system menu | 568 × 1232 portrait | 800,134 | `1fc2986a65815db1…` |
| Launcher, System menu | `landscape/launcher.png` | same | 1232 × 568 landscape | 689,904 | `5918d431d8195465…` |

Full hashes are in `SHA256SUMS`. Decoded, each is 699,776 pixels: 1.33 MiB as
RGB565, 2.67 MiB as XRGB8888.

### App icons: UI assets, transparent

`app-icons/png{48,64,96,128}/{radio,mesh,network,tools,ai,files,games,settings,apps}.png`,
36 files. RGBA, full colour: a dark metal portal frame with gradients and a
coloured line symbol inside (colours in `b_ui_layout.json`). Square canvas,
portrait frame, transparent margins; the alpha is the frame, so it is the same
for all nine icons of one size.

| Size | Opaque / partial / clear px | Frame (ink box) |
| --- | --- | --- |
| 48 × 48 | 1,200 / 144 / 960 | x 8–39, y 3–44 |
| 64 × 64 | 2,268 / 196 / 1,632 | x 10–53, y 4–59 |
| 96 × 96 | 5,084 / 292 / 3,840 | x 16–79, y 6–89 |
| 128 × 128 | 9,240 / 392 / 6,752 | x 21–106, y 8–119 |

These are **categories**, not Doors apps. Only `radio` and `settings` are also
app ids; `mesh`, `network`, `tools`, `ai`, `files`, `games`, `apps` name no
existing app. Launcher groups (B): Connections = radio, mesh, network;
Workspace = tools, ai, files; Device & play = games, settings, apps.

### System icons: UI assets, transparent

`system-icons/png32/*.png`, 17 files, RGBA 32 × 32, one line symbol in warm
white `#eeeae2`, antialiased on transparent (4–112 opaque and 114–283 partial
pixels each). The colour is baked, but the shape is entirely in the alpha.

| Icons | Used in the B layouts for |
| --- | --- |
| `wifi` | status bar (24 px), Wi-Fi quick control (42 px) |
| `bluetooth`, `radio`, `sound` | quick controls (42 px); `sound` also Volume (28 px) |
| `sun` | Brightness slider (28 px) |
| `display`, `network`, `info` | Display & sleep, Connections, About DOORS rows (32 px) |
| `lock`, `power` | Lock and Power actions (32 px) |
| `ai`, `apps`, `files`, `games`, `mesh`, `settings`, `tools` (+ `radio`, `network`) | the app-icon symbols without the frame; not drawn by the B layouts |

## Source (master) artwork: never for the device

| File | What it is | Pixels | Orientation |
| --- | --- | --- | --- |
| `source/backgrounds/portrait/boot.png` | generated master of the portrait boot background | 852 × 1847 | portrait |
| `source/backgrounds/portrait/{lock,open,launcher}.png` | generated masters | 852 × 1846 | portrait |
| `source/backgrounds/landscape/{boot,lock,open,launcher}.png` | generated masters | 1846 × 852 | landscape |
| `source/app-icons/svg/*.svg` (9) | vector masters of the app icons: gradients, no text, no external references | 128 × 128 | — |
| `source/system-icons/svg/*.svg` (17) | vector masters of the system icons: strokes in `#eeeae2`, no text | 48 × 48, viewBox `0 -6 48 48` | — |

Masters are opaque RGB. Each was compared with the native backgrounds at the
same relative positions: it matches its own-named background (mean difference
0.8–2.2 of 255) and no other (8.8 or more), so names and orientation agree.
The supplier's `tools/build_package.py` and `tools/build_b.py` (in
`originals/`) re-export from these with ImageMagick, Node `sharp` and Nimbus
Sans; nothing in this repository uses them.

## Reference: specifications, not assets

| File | What it is |
| --- | --- |
| `reference/ui-layers/<o>/{boot,lock,open}.svg` | live UI over boot, lock, open: scrims, text, status glyphs, progress. Text is `<text>` in "Nimbus Sans"; y is a **baseline** |
| `reference/ui-layers/<o>/{launcher,system}.svg` | the B launcher (grouped panels, 9 portal icons, focus bracket) and B system menu (4 quick controls, 2 sliders, 3 rows, Lock, Power) |
| `reference/layout/b_ui_layout.json` | B launcher and system menu: palette, groups, text anchors (x, baseline, font px), control bounds, both orientations |
| `reference/layout/base_ui_layout.json` | boot, lock, open (and v1's superseded launcher): same fields |

All sample values (17:24, 86 %, 40 % progress, 60 % / 40 % sliders,
"Connected") are placeholders. Only in `originals/`, all reference only:

| File (in `originals/…/`) | Pixels | What it is |
| --- | --- | --- |
| `B2/screens/<o>/{boot,lock,open,launcher,system}.png` | native | complete screens with sample data (visual truth; never a background) |
| `B2/overlays/<o>/*.png` | native, RGBA | the ui-layers rendered on transparent (previews, not live UI) |
| `B2/B_Portrait_overview.png`, `B_Landscape_overview.png`, `B_Icon_family.png` | 616 × 640, 948 × 900, 1296 × 144 | contact sheets, 16-bit RGB |
| `B2/references/approved_B.png` | 1148 × 1371 | the approved B concept |
| `v1/Portrait_overview.png`, `Landscape_overview.png` | 1232 × 640, 1280 × 616 | v1 contact sheets (show the superseded launcher), 16-bit RGB |
| `v1/screens/`, `v1/overlays/` launcher files | native | superseded v1 text launcher |
| `*/fonts/NimbusSans-Regular.otf` + `LICENSE.txt` | — | URW Nimbus Sans, AGPL-3 with font exception; preview font only, must not ship without an owner decision |

(`v1` = `DOORS_Visual_Package_v1`, `B2` = `DOORS_B_Production_Package_v2`.)

## Duplicates

183 supplied files, 152 distinct contents: 31 files are byte-identical copies.

- **Across packages (29 files):** B2 carries v1's 8 backgrounds, boot/lock/open
  screens (6), overlays (6 PNG + 6 SVG), both font files, and `ui_layout.json`
  as `base_ui_layout.json`.
- **Within B2 (2 files):** `backgrounds/{portrait,landscape}/system.png` are
  byte-identical to `launcher.png`. `b_ui_layout.json` names `system.png`; use
  `device/backgrounds/<o>/launcher.png`.
- By design, not duplicates: each PNG icon is a render of the SVG beside it,
  and each screen is its background with its ui-layer on top (supplier).

## Missing expected assets

- **Unlock transition frames:** none. Lock and open are separate stills, not
  pixel-registered; the supplier says to switch directly.
- **DOORS mark / logo:** none. "DOORS" is live text in the ui-layers. The
  existing mark is in `../doors-threshold/brand/`.
- **Status indicators:** no battery, charging, Wi-Fi strength or off-state
  assets. The battery is two rectangles in the ui-layers; `wifi` and
  `bluetooth` have one state each.
- **Icons for existing apps:** calculator, calendar, clock, fleet, notes,
  radar, rift, system, timber and wave have no B icon. B's launcher is 9
  categories, so which app sits behind each is the owner's call.
- **Launcher- and layout-size exports:** see mismatches.

## Dimension and format mismatches

- **App icons:** the B launcher draws them at 104 px (portrait) and 100 px
  (landscape). No export has that size (48, 64, 96, 128).
- **System icons:** the B layouts draw them at 24, 28, 32 and 42 px. Only 32 px
  is exported. The PNG canvas also sits 4 px higher than the layouts' icon
  origin (the SVG viewBox starts at y −6), so a 32 px PNG goes at
  (x, y − 4) to match the ui-layer.
- **Masters** are not exactly 1.5 × native (1846 not 1848 on the long side),
  and portrait `boot` is 852 × 1847, 1 px taller than the other portrait
  masters. The native exports are exact, so this does not affect `device/`.
- **Reference sheets:** the five overview PNGs are 16-bit RGB with `cHRM` and
  `bKGD`; the screens carry `cHRM` (sRGB/D65), `bKGD` and `tIME`. None is a
  device asset; none was converted.
- **v1 → B:** v1's launcher (text-only, order Tools-AI-Games /
  Settings-Files-Apps) is superseded by B (Tools-AI-Files /
  Games-Settings-Apps).
- **Concept → production:** `approved_B.png` and the production system layer
  differ in small details (e.g. the Connections row symbol: mesh in the
  concept, globe in production). Production is what `device/` holds.

## Before implementation

What the files do not decide, for the session that integrates them:

- `tools/design/png2xrgb.py` (the U-Boot splash converter) accepts the
  portrait backgrounds as they are (tried on boot and lock) and refuses the
  landscape ones: the splash is portrait 568 × 1232 only. U-Boot cannot draw
  live text, so a splash made from `boot.png` would have no wordmark or status.
- The app icons are full colour with baked palette colours, not tintable A8
  masks like the current launcher icons, so they would not follow the five
  Doors themes. The system icons can be used as A8 masks (the shape is the
  alpha).
- The layouts are measured with Nimbus Sans; Doors ships IBM Plex. Baselines
  and widths must be re-measured with the Doors fonts.
- The B launcher and system menu change the launcher tile geometry (DS §14 C7,
  §20) and add screens, so they need a DS amendment of their own.
