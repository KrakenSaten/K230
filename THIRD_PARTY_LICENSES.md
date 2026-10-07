# Third-party licences

Doors' own code is licensed under the Apache License 2.0 (LICENSE, ADR-013).
**Everything listed here keeps its own licence**; none of it is relicensed by
Doors. This file is the inventory: what each component is, where it is, how
it reaches a user, and how its licence sits with distributing Doors under
Apache-2.0. The licence texts the image ships are in THIRD_PARTY_NOTICES.txt
(generated from third_party/notices/SOURCES); machine learning models are in
MODEL_LICENSES.md; the full audit, with evidence and blockers, is
docs/licensing/APACHE_2_READINESS.md. Audited 2026-10-01 at origin/master
`426b1d8`.

`id` is the entry in third_party/notices/SOURCES, where there is one.

## Compatibility classes

| Class | Meaning |
| --- | --- |
| OK | Permissive; nothing to do beyond keeping the upstream files as they are. |
| OK WITH ATTRIBUTION | Permissive; a distribution must carry its notice (done by THIRD_PARTY_NOTICES.txt where it ships). |
| SEPARATE LICENSE | Distributed beside Doors under its own, different terms (copyleft or font licence), with that licence's obligations (source offer, licence text, naming rules). It does not become Apache-2.0, and Doors does not become it. |
| REVIEW REQUIRED | Terms missing, unclear or not yet met; must be settled before the distribution it concerns. |
| UNKNOWN - DO NOT REDISTRIBUTE | Author, licence or provenance not established from a primary source. Not relicensed, not guessed; kept out of public distributions until settled. |
| DO NOT REDISTRIBUTE | Terms are known and do not allow it, or are reserved; keep it out of public distributions. |

## 1. Compiled into Doors binaries

Source is fetched at the pinned commit when the package is assembled
(`vendor/` is git-ignored); nothing of it is committed here except its licence
text.

| Component | `id` | Purpose | Upstream | Licence | In repo | In image | Obligations | Class |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RadioLib 7.7.1 (`034126e`) | `radiolib` | SX1262 driver in radiod | github.com/jgromes/RadioLib | MIT | licence text only | `/usr/sbin/radiod` | keep notice | OK WITH ATTRIBUTION |
| ggwave v0.4.3 (`a38e38b`) | `ggwave` | Wave's data-over-sound codec in pos-wave | github.com/ggerganov/ggwave | MIT | licence text only | `/usr/bin/pos-wave` | keep notice | OK WITH ATTRIBUTION |
| Reed-Solomon (Mike Lubinets), in ggwave | `reed-solomon` | error correction in ggwave | via ggwave | MIT | licence text only | `/usr/bin/pos-wave` | keep notice | OK WITH ATTRIBUTION |
| Ooura FFT, in ggwave | `ooura-fft` | FFT in ggwave | kurims.kyoto-u.ac.jp/~ooura/fft.html | author's permissive terms (docs/legal/third-party/ooura-fft.txt) | terms only | `/usr/bin/pos-wave` | keep the header's package reference | OK WITH ATTRIBUTION |
| MeshCore (`3ca7e3f`, RIFT tree) | `meshcore` | MeshCore protocol in meshcored, meshcore-frame, libmeshcore | github.com/KrakenSaten/RIFT (MeshCore fork) | MIT | licence text only | `/usr/sbin/meshcored` | keep notice | OK WITH ATTRIBUTION |
| Ed25519 (Orson Peters), in MeshCore | `ed25519` | signatures | via MeshCore | Zlib | licence text only | `/usr/sbin/meshcored` | keep notice | OK WITH ATTRIBUTION |
| Arduino Cryptography Library (`37a76b8`) | `arduinolibs-crypto` | AES/SHA for MeshCore | github.com/rweather/arduinolibs | MIT | licence text only | `/usr/sbin/meshcored` | keep notice | OK WITH ATTRIBUTION |
| K230 Linux SDK code (Canaan): `libmmz`, and the AI2D set-up adapted from its `yolo` and `ai_demo` samples | `canaan-k230-sdk` | KPU shared memory and preprocessing in pos-vision | github.com/kendryte/k230_linux_sdk at `22d02c6` | BSD-2-Clause (the SDK's root LICENSE; `libmmz` has no file header) | licence text (docs/legal/third-party/canaan-k230-linux-sdk-LICENSE.txt) | `/usr/bin/pos-vision` | keep notice | OK WITH ATTRIBUTION |
| nncase runtime 2.11.0 for K230 (`libNncase.Runtime.Native`, `libnncase.rt_modules.k230`, `libfunctional_k230`, static) | - | KPU inference in pos-vision | github.com/kendryte/nncase release archive `nncase_k230_v2.11.0_runtime_linux.tgz` | nncase's repository is Apache-2.0 at `v2.11.0`; **the prebuilt runtime archive carries no licence file**, and whether the K230 modules are covered by it is not shown | no | `/usr/bin/pos-vision` (static) | Apache-2.0 section 4 if confirmed: licence text, notices | **UNKNOWN - DO NOT REDISTRIBUTE** (B5) |

## 2. Fonts and generated assets built into Doors

| Component | `id` | Purpose | Upstream | Licence | In repo | In image | Obligations | Class |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| IBM Plex Sans / Mono, converted to LVGL bitmaps (`ui/pocketui/fonts/pos_font_*.c`, 24 files, lv_font_conv 1.5.3) | `ibm-plex` | the UI's type | github.com/IBM/plex | OFL-1.1, Reserved Font Name "Plex" (docs/legal/fonts/IBMPlex-OFL-1.1.txt) | generated C files | compiled into `/usr/bin/doors-shell` | ship the OFL text and copyright; never present the bitmaps as "IBM Plex" or "Plex" (they are Modified Versions, named `pos_font_*`); the bitmaps stay OFL, not Apache-2.0 | SEPARATE LICENSE |
| LVGL (`59dc7e4`, liblvgl 9.5.0; vendor SDK package) | `lvgl` | UI toolkit | github.com/lvgl/lvgl | MIT | no (vendor clone) | `/usr/lib/liblvgl*.so` | keep notice | OK WITH ATTRIBUTION |
| lv_port_linux (`b492d73`) | `lv-port-linux` | LVGL Linux port | github.com/lvgl/lv_port_linux | MIT | no | `/usr/lib/liblvgl_linux.so` | keep notice | OK WITH ATTRIBUTION |
| LodePNG, in LVGL | `lodepng` | PNG decoding | via LVGL | Zlib | no | liblvgl | keep notice | OK WITH ATTRIBUTION |
| TJpgDec, in LVGL | `tjpgd` | JPEG decoding | via LVGL | TJpgDec licence (BSD-style) | no | liblvgl | keep notice | OK WITH ATTRIBUTION |
| ThorVG, in LVGL | `thorvg` | vector graphics | via LVGL | MIT | no | liblvgl_thorvg | keep notice | OK WITH ATTRIBUTION |
| Montserrat (LVGL built-in font) | `montserrat` | LVGL default font | via LVGL | OFL-1.1 | no | liblvgl | ship OFL text | SEPARATE LICENSE |
| Font Awesome 5 glyphs (`LV_SYMBOL_*`) | `font-awesome-5` | symbols | via LVGL | OFL-1.1 for the font glyphs | no | liblvgl | ship licence text | SEPARATE LICENSE |
| DejaVu Sans subset (LVGL) | `dejavu-sans` | Persian/Hebrew font | via LVGL | Bitstream Vera / Arev; DejaVu changes public domain | no | liblvgl | ship licence text | OK WITH ATTRIBUTION |
| Source Han Sans SC subset (LVGL) | `source-han-sans-sc` | CJK font | via LVGL | OFL-1.1 | no | liblvgl | ship OFL text | SEPARATE LICENSE |
| unscii-8 (LVGL) | `unscii-8` | bitmap font | via LVGL | public domain | no | liblvgl | none | OK |
| Noto Color Emoji artwork (Google), 2D/png/72 at `e20cbc2`, 1683 images converted to RGB565A8 (`apps/rift/ui/rift_emoji_px.bin`, with the generated index `rift_emoji_img.c` and `rift_emoji_seq.c`) | `noto-color-emoji` | RIFT's colour emoji | github.com/googlefonts/noto-emoji | Apache-2.0 (docs/legal/third-party/noto-color-emoji.txt) | converted images | compiled into `/usr/bin/doors-shell` | keep notice; the converted images stay under Google's Apache-2.0 grant, not Doors' | OK WITH ATTRIBUTION |
| Region flags (googlei18n/region-flags `743e1f4`, bundled in noto-emoji), 262 flags converted with the emoji | `region-flags` | RIFT's flag emoji | github.com/googlefonts/region-flags | public domain or otherwise exempt (docs/legal/third-party/region-flags.txt) | converted images | compiled into `/usr/bin/doors-shell` | none; the text ships anyway | OK |

## 3. Third-party code adapted inside first-party files

Each of these files says so in its header with a compound SPDX expression
and is listed in docs/licensing/spdx-exempt.txt.

| File(s) | Adapted from | Licence | Ships | Class |
| --- | --- | --- | --- | --- |
| `services/meshcored/mesh_runtime.cpp` (packetScore) | MeshCore `RadioLibWrappers.cpp` | MIT (`meshcore` notice) | meshcored | OK WITH ATTRIBUTION |
| `tools/meshcore-frame/mcf_frame.cpp`, `mcf_report.cpp` | MeshCore `Mesh.cpp`, `BaseChatMesh.cpp` | MIT | host tool only | OK WITH ATTRIBUTION |
| `tests/meshcore_core_test.cpp`, `tests/meshcore_frame_test.cpp` | MeshCore's own test suites, its test key pair and vectors | MIT | tests only | OK WITH ATTRIBUTION |
| `core/pocketvision/vision_kpu_nncase.cpp` | K230 SDK `yolo/src/utils.cc`, `ai_demo` `Utils::affine` | BSD-2-Clause (`canaan-k230-sdk` notice) | pos-vision | OK WITH ATTRIBUTION |
| `ui/pocketui/pos_keymap.c` (key-name and shifted-symbol tables) | LILYGO launcher `k230_phone_ui/src/ui_hardware.c` | **none stated** by LILYGO | doors-shell | **UNKNOWN - DO NOT REDISTRIBUTE** (B3; header says NOT CLEARED) |
| `platforms/k230/configs/k230_pocketos_defconfig` (removed 2026-10-02) | LILYGO `k230_canmv_t_display_rm69a10_defconfig`, copied with one line added | none stated by LILYGO | build configuration | **RESOLVED**: no copy kept; `apply_to_sdk.sh` composes it from the vendor file at the pinned BSP commit plus the Doors fragment `platforms/k230/configs/k230_pocketos.fragment` |
| `apps/calendar/cal_date.c` (days_from_civil) | Howard Hinnant's date algorithms | public domain ("Consider these donated to the public domain", howardhinnant.github.io/date_algorithms.html) | doors-shell | OK |
| `tests/display_geometry_test.c` (one calibration formula) | LVGL `lv_evdev.c` | MIT | tests only | OK |

Facts and constants taken from documentation or vendor code - pin numbers,
register addresses, the TCA8418 map, YOLO/RetinaFace/ArcFace tensor layouts
and anchor rules, COCO's class names, PP-OCR post-processing constants,
BT.601 coefficients, FIPS/RFC test vectors, HTML entity names - are not
treated as copied code. The audit lists them (APACHE_2_READINESS.md §3).

## 4. Libraries Doors loads at run time (Buildroot packages)

Linked dynamically, nothing of them compiled into Doors. Their licences and
sources are collected by Buildroot's `make legal-info` (docs/legal/ holds the
first run, 2026-09-04; open item 3 is to archive it per release).

| Component | Used by | Licence (Buildroot metadata) | Class |
| --- | --- | --- | --- |
| cJSON 1.7.18 | pocketipc, pos, services | MIT | OK WITH ATTRIBUTION |
| libgpiod 2.2 | radiod | LGPL-2.1-or-later | SEPARATE LICENSE (dynamic; offer source) |
| alsa-lib 1.2.13 | pos-wave, pos-record, pos-mp3, pos-video | LGPL-2.1-or-later | SEPARATE LICENSE (dynamic; offer source) |
| libdrm 2.4.124, libevdev 1.13.1 | doors-shell (via LVGL) | MIT | OK WITH ATTRIBUTION |
| libcurl 8.12.1 | pos-zabbix, pos-browser | curl | OK WITH ATTRIBUTION |
| OpenSSL 3.4.1 | via libcurl | Apache-2.0 | OK WITH ATTRIBUTION |
| libjpeg 9f | pos-camera, pos-browser | IJG | OK WITH ATTRIBUTION |
| libpng 1.6.46 | pos-browser | Libpng-2.0 | OK WITH ATTRIBUTION |
| FreeType 2.13.3 | via FFmpeg (`--enable-libfreetype`) | FTL or GPL-2.0+ (dual; the FTL is the permissive choice) | OK WITH ATTRIBUTION |
| FFmpeg 4.4.4 (vendor-patched; libavformat, libavcodec, libavutil, libswscale, libswresample) | pos-mp3, pos-video | LGPL-2.1-or-later, plus the libjpeg licence. **Built `--disable-gpl --disable-nonfree --disable-version3`; `CONFIG_GPL 0`, `CONFIG_NONFREE 0`, `CONFIG_VERSION3 0`; no x264/x265/fdk-aac** (VERIFIED in the built `config.h`, 2026-10-01) | SEPARATE LICENSE (dynamic; offer source and the vendor's patches) |

## 5. The platform the image is built on (not Doors code)

| Component | Licence | In image | Class |
| --- | --- | --- | --- |
| Linux kernel (`ruyisdk/linux-xuantie-kernel` `7d4e1f4`) with the LILYGO BSP patches | GPL-2.0 | yes | SEPARATE LICENSE (source offer) |
| Doors kernel patches `platforms/k230/patches/linux/0070-0073` (author "Doors"; 0071 carries two tables from the kernel's `kmb_dsi.c`) | GPL-2.0, as modifications of GPL-2.0 files; **not Apache-2.0** | yes, in the kernel | SEPARATE LICENSE |
| U-Boot 2022.10 with the LILYGO overlay | GPL-2.0+ | yes | SEPARATE LICENSE |
| OpenSBI 1.4 | BSD-2-Clause | yes | OK WITH ATTRIBUTION |
| BusyBox, util-linux, e2fsprogs, bluez, dbus, wpa_supplicant, hostapd and the other Buildroot packages (docs/legal/manifest.csv, 96 rows) | various; GPL-3.0+: readline, dosfstools, parted, umtprd; LGPL-3.0+: live555; EPL-2.0: paho-mqtt; no AGPL | yes | SEPARATE LICENSE (legal-info) |
| K230 Linux SDK (`kendryte/k230_linux_sdk` `22d02c6`) | BSD-2-Clause (Canaan, root LICENSE) | build system and vendor packages | OK WITH ATTRIBUTION |
| SDK vendor packages without licence metadata: `libnncase`, `gsl-lite`, `vvcam`, `face_detect`, `ai2d_kpu`, `nonai2d`, `libmmz` | not in their package files (`gsl-lite` is MIT upstream) | yes | **UNKNOWN - DO NOT REDISTRIBUTE** (B6; docs/LICENSING.md item 5) |
| LILYGO T-Display-K230 (`bb831ab`): BSP overlay, boot scripts, rootfs overlay | **no licence** in the repository (files that modify GPL code are GPL by derivation) | yes | **UNKNOWN - DO NOT REDISTRIBUTE** (B7; item 2) |
| LILYGO launcher `k230_phone_ui` (with bundled nofrendo GPL-2.0, RadioLib MIT, quirc ISC, libtmt and qrcodegen MIT, and two kmodels) | **no licence** for LILYGO's own code | **yes** (`BR2_PACKAGE_K230_PHONE_UI=y`; disabled at boot by `/etc/default/k230_phone_ui`) | **UNKNOWN - DO NOT REDISTRIBUTE** (B7) - get LILYGO's terms or drop the package |
| Realtek `rtl8723ds-bt` firmware | PROPRIETARY (Buildroot metadata) | yes | **DO NOT REDISTRIBUTE** (B8: proprietary, terms not collected, hardware absent) |
| `rtl8723ds`, `rtl8189fs`, `aic8800` drivers and `aic8800*` firmware | GPL-2.0 drivers; firmware terms not collected | yes | drivers: SEPARATE LICENSE (GPL-2.0); firmware: **UNKNOWN - DO NOT REDISTRIBUTE** (B8) |
| Xuantie toolchain runtime (glibc, libstdc++, libgcc_s) | LGPL-2.1+ / GPL-3.0 with runtime exception | yes | REVIEW REQUIRED (B9: licences known, texts and source offer not collected) |
| YOLOv8n model `yolov8n.kmodel` (`yolov8n-kmodel`), installed by the Doors package for Vision | Ultralytics weights AGPL-3.0; the SDK states no terms for the file | yes, internal images only (owner, 2026-09-28) | **DO NOT REDISTRIBUTE** outside the project (MODEL_LICENSES.md; docs/LICENSING.md item 10) |
| Vendor models in the image (`face_detection_320.kmodel`, `xiaozhi_kws.kmodel`, `test.kmodel`) | none stated | yes, via vendor packages | see MODEL_LICENSES.md |

## 6. Build and host-only tools (not distributed)

| Component | Licence | Class |
| --- | --- | --- |
| Buildroot 2025.02.1 | GPL-2.0 | OK (build tool; not shipped beyond legal-info) |
| lv_font_conv 1.5.3 (generated `pos_font_*.c`) | MIT | OK |
| SDL2 (simulator only) | Zlib | OK |
| Python 3, Node 20 (host) | PSF, MIT | OK |

## 7. Third-party material in the repository's documents (not shipped)

| Path | What | Licence | Class |
| --- | --- | --- | --- |
| `docs/legal/licenses/**`, `docs/legal/*.csv` | Buildroot legal-info output | each package's own | OK (licence texts) |
| `docs/legal/fonts/IBMPlex-OFL-1.1.txt`, `docs/legal/third-party/*` | licence texts | as named | OK |
| `docs/design/brand/doors-visual-pack-v1/originals/*/fonts/NimbusSans-Regular.otf` (2 copies, with `LICENSE.txt`) | URW base35 font, preview only | AGPL-3.0 with font exception (its LICENSE.txt) | SEPARATE LICENSE; excluded from the public-source candidate with the rest of `originals/` |
| `docs/design/PocketOS Design System.html` | bundled design page embedding React and IBM Plex WOFF2 subsets and a design-tool runtime | React MIT, Plex OFL-1.1; runtime unknown | **UNKNOWN - DO NOT REDISTRIBUTE** (B2); excluded from the public-source candidate |
| `docs/design/rift/support.js` | the design tool's runtime ("GENERATED from dc-runtime"), which the RIFT design document loads | none stated | **UNKNOWN - DO NOT REDISTRIBUTE** (B2); excluded from the public-source candidate. The rest of the RIFT package is the owner's (made with Claude) and Apache-2.0 |
| `docs/design/Repository connection and design directions.zip` (the identical `apps/fleet/` copy was removed 2026-10-02) | design-canvas export: React, the same runtime, AI-generated (OpenAI) reference images | React MIT; rest none stated | **UNKNOWN - DO NOT REDISTRIBUTE** (B2); excluded from the public-source candidate |
| `docs/design/brand/doors-threshold/**`, `doors-icon-extension/**`, `doors-visual-pack-v1/**` | the owner's artwork packages, made with ChatGPT (part AI-generated) | not third-party: Apache-2.0, except the brand assets, reserved (docs/licensing/BRAND.md) | **RESOLVED** (B1, docs/licensing/B1_ARTWORK.md); per-file classes in docs/licensing/asset-inventory.txt; the reference-only parts stay excluded |

## 8. Test data

All fixtures are synthesised by the tests (no binary fixture is tracked).
MeshCore's test key pair and vectors are MIT (§3); FIPS 180, RFC 4648 and RFC
6234 vectors are published test data. `docs/hardware/hwcheck-unitA/` holds
radio captures from unit A that include other people's over-the-air packets
(encrypted payloads, node names in some screenshots under
`docs/hardware/shots/`): not a licence question, but reviewed before the
repository is public (readiness audit §9).
