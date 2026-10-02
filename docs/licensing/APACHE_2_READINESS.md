# Apache-2.0 readiness audit

Branch `chore/apache-2-license`, from origin/master `426b1d8` (2026-10-01).
Owner decision: Doors is licensed under the Apache License 2.0
(ADR-013). This audit says what that licence can cover, what it cannot, and
what still stands between the repository - and separately the flashable
image - and a public release. It prepares; it publishes nothing.

Evidence classes as in AGENTS.md: **VERIFIED** (read or measured here),
**DOCUMENTED** (stated by an upstream or vendor source, cited),
**ASSUMED** (inferred; said so). Companion files: LICENSE, NOTICE,
THIRD_PARTY_LICENSES.md (inventory and classes), MODEL_LICENSES.md (models),
docs/LICENSING.md (the standing register), docs/licensing/spdx-exempt.txt.

## 1. Executive summary

- **Doors' own code can be Apache-2.0 today.** The history shows one author
  account (the owner's) plus Claude sessions the owner ran; no external
  contributor (§10). LICENSE (the ASF text, byte-identical), NOTICE, the
  SPDX headers, the package metadata and the image's
  `/usr/share/doors/{LICENSE,NOTICE}` are in place on this branch.
- **Third-party code compiled into Doors is permissive** (MIT, Zlib,
  BSD-2-Clause, Ooura's terms) and its notices ship. Fonts are OFL-1.1 and
  stay OFL. Dynamically linked libraries are permissive or LGPL; FFmpeg is
  built without GPL, nonfree or version3 parts (VERIFIED from its built
  configuration). No GPL code is compiled into a Doors binary.
- **Not everything in the repository is the project's to license.** For a
  public **source** release (status 2026-10-02, §14.1): the design-tool
  exports (B2) are now kept out of the public-source candidate, and the
  copied LILYGO defconfig (B3a) is replaced by a Doors fragment composed with
  the vendor file at apply time. Still blocking: the owner-supplied artwork
  with no stated author or terms (part AI-generated), which generates the
  shipped icons, glyphs, backgrounds and boot splash (B1), and the keyboard
  tables copied from LILYGO's launcher (B3b), which need the remaining keys
  read on hardware or LILYGO's licence.
- **The flashable image is further from ready.** Besides B1 to B3 it ships
  the YOLOv8n model (AGPL-3.0, internal use only by owner decision), a
  statically linked nncase runtime with no licence file, vendor packages and
  models with no licence metadata, LILYGO's unlicensed launcher, a
  proprietary Bluetooth firmware, and toolchain runtimes legal-info does not
  collect (B4 to B9).

**Result** (§15): SOURCE REPOSITORY **NOT READY**; FLASHABLE IMAGE
**NOT READY**. Specific licensing blockers remain for both (§14).

### Conditions the brief asked to stop on

None of these was acted on beyond documenting it; the branch changes no image
content and removes nothing.

| Condition | Found | Where |
| --- | --- | --- |
| Significant code copied from an incompatible source | **No significant code.** Two small LILYGO-derived items: a Buildroot defconfig (one line added to LILYGO's; replaced 2026-10-02, B3a) and two key tables (B3b, still blocked). | B3 |
| Substantial external contributors | No. | §10 |
| Proprietary or non-commercial material packaged | **Yes, in the image, from the vendor platform**: `rtl8723ds-bt` firmware (PROPRIETARY in Buildroot's metadata) and the LILYGO launcher (no licence). Not Doors code; both pre-date this branch. | B7, B8 |
| Unknown model or blob essential to distribution | No model is essential: Vision offers a mode only when its model is present. The image does carry models of unknown or internal-only terms. | B4, B6 |
| A licence condition that conflicts with the goal | The AGPL-3.0 YOLOv8n model in the image conflicts with a public image under the owner's internal-only decision; it does not touch the source. | B4 |

## 2. DOORS-owned / original material

**What is original** (VERIFIED from history and headers, §10): `apps/`,
`core/`, `services/`, `protocols/meshcore/` (the port layer and shims;
MeshCore itself is vendored at build time), `tools/`, `ui/` (except generated
assets, §6), `tests/`, the Makefile and CMake files, `platforms/k230/`
scripts, package files and init scripts, `third_party/notices/SOURCES` and
the statements in `third_party/notices/texts/` written here, and the
documentation under `docs/` written for the project (except the supplied
design packages, §6, and `docs/legal/`, which is third-party text).

**Source headers - the policy.**

1. A first-party source file carries, in its header comment:
   `Copyright (c) 2026 PocketOS authors.` and
   `SPDX-License-Identifier: Apache-2.0`. The copyright line keeps the
   PocketOS name (ADR-005 decision 7); the owner may change who it names
   later, separately.
2. On this branch the 874 files that carried the project header
   "Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD)." were
   converted by an exact-match allow-list: only that line, only tracked
   files, never under `third_party/`, `docs/legal/`, `vendor/` or the kernel
   patches. Markdown files got a sentence instead of a tag. 22 key build,
   packaging and tool files that had no header got one. All tagged files
   were audited again on 2026-10-02 (§14.1); one more was reclassified as
   mixed (`tests/display_geometry_test.c`, LVGL arithmetic).
3. Files that adapt someone else's code say so and carry a compound
   expression: `Apache-2.0 AND MIT` (5 files adapting MeshCore, 1 test
   reproducing an LVGL formula) or
   `Apache-2.0 AND BSD-2-Clause` (`core/pocketvision/vision_kpu_nncase.cpp`,
   Canaan's samples), with the upstream copyright and a pointer to the
   licence text.
4. `ui/pocketui/pos_keymap.c` carries **no** SPDX line and says "Licence NOT
   CLEARED" (B3).
5. Generated files carry no tag; their licence follows their inputs
   (`pos_font_*.c`: OFL; `pos_app_icons.c`, `pos_glyphs.*`,
   `pos_brand_mark.c`, `ui/assets/doors/*.bin`, `logo.xrgb`: the artwork's,
   B1; `pos_theme_table.h`: Apache-2.0 by its input, `themes.json`).
6. 93 first-party files without a header (88 test scripts, 5 init scripts)
   are listed as PENDING in docs/licensing/spdx-exempt.txt. They are covered
   by LICENSE as files of the project; the header is a follow-up, kept out
   of this branch to avoid touching the image's init scripts and a large
   test diff for appearance's sake.
7. `tests/license_audit_test.sh` holds all of this: no Apache-2.0 tag under
   the third-party, legal, patch, supplied-design or font paths or on a
   generated file; no first-party source file without the tag or an
   exemption with a reason; no "LICENSE (TBD)" left; well-formed SPDX
   expressions.

## 3. Third-party code

Inventory and classes: THIRD_PARTY_LICENSES.md §1-§4. Summary:

| Group | Licences | Shipped notice | Class |
| --- | --- | --- | --- |
| Compiled into binaries: RadioLib, ggwave (+ Reed-Solomon, Ooura FFT), MeshCore (+ Ed25519), Arduino Cryptography Library | MIT, Zlib, Ooura's terms | yes, verified byte for byte against the pinned upstream (`gen_notices.sh --verify-upstream`) | OK WITH ATTRIBUTION |
| Canaan K230 SDK code in pos-vision: `libmmz` (static) and the AI2D set-up adapted from the `yolo` and `ai_demo` samples | BSD-2-Clause (SDK root LICENSE; `libmmz` has no file header, so the repository licence is what covers it - DOCUMENTED) | **added on this branch** (`canaan-k230-sdk`); the text is the SDK's LICENSE with CRLF made LF, compared with the vendor file by the audit test | OK WITH ATTRIBUTION |
| nncase 2.11.0 K230 runtime (static in pos-vision) | nncase's repository: Apache-2.0 at `v2.11.0` (VERIFIED, raw LICENSE at the tag). The prebuilt archive the SDK downloads has **no licence file** (VERIFIED), and the K230 runtime modules are not shown to be built from that repository | none | **UNKNOWN - DO NOT REDISTRIBUTE** (B5) |
| Adapted inside first-party files (§2 item 3) | MIT, BSD-2-Clause | MeshCore and Canaan notices | OK WITH ATTRIBUTION |
| Copied from LILYGO (defconfig, keymap tables) | none stated | - | **UNKNOWN - DO NOT REDISTRIBUTE** (B3) |
| Dynamically linked Buildroot libraries (cJSON, libgpiod, alsa-lib, libdrm, libevdev, libcurl, OpenSSL, libjpeg, libpng, FreeType, FFmpeg) | MIT, LGPL-2.1+, curl, Apache-2.0, IJG, Libpng-2.0, FTL | legal-info | OK WITH ATTRIBUTION / SEPARATE LICENSE (LGPL: source offer) |

Facts and constants taken from documentation are not copied code: pin and
register maps (LILYGO pin map, TCA8418, SX1262 datasheets), YOLOv8, RetinaFace,
ArcFace and PP-OCR tensor layouts and constants, COCO's class names, BT.601
coefficients, FIPS/RFC test vectors, HTML entity names, Hinnant's
public-domain date algorithm (VERIFIED on the author's page). A hash
finaliser in `apps/timber/engine/timber_tower.c` uses two constants the code
calls "murmur"-like; they match a published public-domain integer hash
(ASSUMED; constants only, no code copied).

## 4. Vision / model licensing

Full matrix: MODEL_LICENSES.md. Summary:

| Model | Feature | Licence | Committed | In the Doors image | Status |
| --- | --- | --- | --- | --- | --- |
| `yolov8n.kmodel` | DETECT, TRACK, COUNT, TRAFFIC, DeskBuddy | Ultralytics weights AGPL-3.0; the SDK states nothing for the file | no | **yes** (internal, owner 2026-09-28) | UNKNOWN - DO NOT REDISTRIBUTE outside the project |
| `face_det.kmodel` (vendor `face_detection_320.kmodel`) | FACE, RECOGNIZE, DeskBuddy | none stated | no | not by Doors; **the vendor's copy is in the image** | UNKNOWN; EXTERNAL ONLY for Doors |
| `text_det.kmodel`, `text_rec.kmodel`, `text_dict.txt` (canmv `ai_poc`) | READ | none stated | no | no | UNKNOWN; EXTERNAL ONLY |
| `face_embed.kmodel` (canmv `face_recognition.kmodel`) | RECOGNIZE, DeskBuddy | none stated | no | no | UNKNOWN; EXTERNAL ONLY |
| `xiaozhi_kws.kmodel`, `test.kmodel` | none (vendor demos) | none stated | no | yes, via vendor packages | NOT USED by Doors |

Vision's code is Apache-2.0 regardless (with the BSD-2-Clause portions of
`vision_kpu_nncase.cpp`); it contains no model data. The audit test refuses
a committed model file, an installed model MODEL_LICENSES.md does not list,
an installed EXTERNAL ONLY model, and an installed UNKNOWN model other than
the owner-allowed internal one with its notice - each executed against a
scratch package that breaks the rule.

## 5. Fonts

| Font | Where | Licence | Status |
| --- | --- | --- | --- |
| IBM Plex Sans (Regular, SemiBold) and Mono (Regular, Medium) | 24 LVGL bitmap fonts `ui/pocketui/fonts/pos_font_*.c`, generated by `tools/design/gen_fonts.sh` with lv_font_conv 1.5.3 (the `Opts:` line of each file names its TTF) | OFL-1.1 with Reserved Font Name "Plex" (VERIFIED: upstream `license.txt`, copy in docs/legal/fonts/) | The bitmaps are **Modified Versions of the font** and stay OFL-1.1, not Apache-2.0. Shipped with the OFL text; named `pos_font_*`, never presented as Plex (DS decision). The Plex release used and its URL are not recorded (cleanup C4). |
| Montserrat, Font Awesome 5 glyphs, DejaVu Sans, Source Han Sans SC, unscii-8 | LVGL's built-in fonts in the vendor liblvgl | OFL-1.1; OFL-1.1 (glyphs); Bitstream Vera/Arev; OFL-1.1; public domain | shipped with notices |
| IBM Plex outlines in the boot splash wordmark | `logo.xrgb` | a picture, not a font; no font ships for it | no notice needed (docs/design/brand/README.md, engineering view) |
| Nimbus Sans Regular (URW base35) | two `.otf` copies in `docs/design/brand/doors-visual-pack-v1/originals/` with their LICENSE.txt | AGPL-3.0 with font exception | preview only, not shipped; remove before publishing or keep with its licence (C3) |
| IBM Plex WOFF2 subsets | embedded in `docs/design/PocketOS Design System.html` | OFL-1.1 | part of B2 |

## 6. Icons, art and brand assets

| Asset | Origin (evidence) | Reaches the image as | Class |
| --- | --- | --- | --- |
| Threshold package (`docs/design/brand/doors-threshold/`): mark, lockups, 10 icons, boot art | supplied by the owner 2026-09-15; author, copyright, licence **not stated** (docs/design/brand/README.md, Provenance) | `logo.xrgb` (boot splash), `pos_brand_mark.c`, icon masks in `pos_app_icons.c`, portal glyphs in `ui/assets/doors/icon-*.bin` | **UNKNOWN - DO NOT REDISTRIBUTE** (B1) |
| Icon extension (`doors-icon-extension/`) | as above, 2026-09-15 | Wave, Files, Camera, Recorder, Gallery icons | **UNKNOWN - DO NOT REDISTRIBUTE** (B1) |
| DOORS visual pack v1 and B package v2 (`doors-visual-pack-v1/`) | supplied by the owner 2026-09-22; author, copyright, licence not stated; photographic art **AI-generated** (supplier's PROVENANCE.md; C2PA "OpenAI Media Service" in the masters) | the six backgrounds `bg-*.bin`, the portal frame, the system glyphs (`pos_glyphs.c`) | **UNKNOWN - DO NOT REDISTRIBUTE** (B1) |
| `docs/design/doors-app-icons/` (12 SVGs and their PNGs), `docs/design/doors-glyphs/mode.svg` | drawn in this repository (their README) | 11 app icons, the mode glyph | ORIGINAL DOORS - Apache-2.0 |
| PocketTimber sprites (`docs/design/timber-art/`) | generated by `tools/timber_blender.py`, procedural materials only | compiled into the shell | GENERATED FOR DOORS - Apache-2.0 |
| Screenshots (`docs/design/shots/`, `docs/apps/**`, `docs/hardware/shots/`, ...) | simulator and unit captures of Doors | not shipped | ORIGINAL, but some show the B1 backgrounds and other people's mesh node names (C2) |
| RIFT design package (`docs/design/rift/`) | supplied 2026-09-19; C2PA "Anthropic Files" (made with Claude); terms not stated | not shipped | **UNKNOWN - DO NOT REDISTRIBUTE** (B2) |

Names and logos: the Apache licence grants no trademark rights (its section
6), NOTICE says so, and nothing here claims a registration. The names
"Doors" and "PocketOS" and the logos are the owner's to govern separately; a
short trademark statement is the owner's choice and is not written here.

## 7. LILYGO / K230 / BSP

| Item | Upstream | Licence | Doors' relation | Class |
| --- | --- | --- | --- | --- |
| Xinyuan-LilyGO/T-Display-K230 (`bb831ab`): BSP overlay, boot and rootfs scripts, launcher `k230_phone_ui` | github.com/Xinyuan-LilyGO/T-Display-K230 | **no LICENSE file and no headers** (VERIFIED 2026-09-04 and again 2026-10-01) | used at build time as an overlay; not copied, except the two items below; the launcher and overlay files are in the image | **UNKNOWN - DO NOT REDISTRIBUTE** (B7; docs/LICENSING.md item 2) |
| `platforms/k230/configs/k230_pocketos_defconfig` (until 2026-10-02) | LILYGO `k230_canmv_t_display_rm69a10_defconfig`, which derives from Canaan's `k230_canmv_defconfig` (BSD-2-Clause SDK) | none stated by LILYGO | was a verbatim copy plus one line; **removed**. `apply_to_sdk.sh` now composes the defconfig from the vendor file at the pinned BSP commit plus `platforms/k230/configs/k230_pocketos.fragment` (Doors-owned, Apache-2.0); the result is byte-identical to the removed file (sha256 `e0b0b6ce…`) | **RESOLVED (B3a)** |
| `ui/pocketui/pos_keymap.c` key-name and shifted-symbol tables | launcher `ui_hardware.c` (`tca8418_key_name`, `extension_keyboard_shift_symbol_for_code`) | none stated | copied tables, two entries corrected on hardware; attributed in `pos_keymap.h` | **UNKNOWN - DO NOT REDISTRIBUTE** (B3); file marked NOT CLEARED |
| Hardware constants (pins, registers, touch transform, 30 px side inset, amplifier GPIO, keyboard LEDs) | LILYGO pin map, DTS, launcher | facts | cited in comments (DOCUMENTED) | OK |
| `apply_to_sdk.sh` edit of the vendor `S99zz_k230_phone_ui` | LILYGO | - | patched in place in the SDK tree, never copied here (VERIFIED) | OK |
| Kendryte/Canaan K230 Linux SDK (`22d02c6`) | github.com/kendryte/k230_linux_sdk | BSD-2-Clause (root LICENSE, Canaan 2024) | build system; `libmmz` and adapted samples in pos-vision (§3) | OK WITH ATTRIBUTION |
| T-Display-K230_canmv_rt | LILYGO | README header comment "@License: GPL 3.0" | reference only; source of the READ/RECOGNIZE models tried | not linked |
| Doors kernel patches 0070-0073 | modify `lontium-lt9611.c`, `canaan_dsi.c`, `canaan_vo.c`; 0071 includes two tables from the kernel's `kmb_dsi.c` | **GPL-2.0** (derivative of GPL-2.0 files) | written for Doors, applied to the BSP kernel (ADR-011) | SEPARATE LICENSE; excluded from Apache-2.0 and from the header conversion |

Attribution obligations: Canaan's BSD-2-Clause notice with binaries that
contain its code (now in the notices); the GPL source offer for the kernel,
U-Boot and anything else GPL in the image, including Doors' kernel patches
(legal-info, item 3).

## 8. Multimedia

| Component | How Doors uses it | Licence | Evidence |
| --- | --- | --- | --- |
| FFmpeg 4.4.4 (vendor package with Canaan's VPU patches 0006-0012) | **linked dynamically** by pos-mp3 and pos-video; system library in the image (OpenCV selects it) | LGPL-2.1-or-later (+ libjpeg licence) | VERIFIED 2026-10-01 in `output/k230_pocketos_defconfig/build/ffmpeg-4.4.4/config.h`: `--disable-gpl --disable-nonfree --disable-version3`, `CONFIG_GPL 0`, `CONFIG_NONFREE 0`, `CONFIG_VERSION3 0`, `CONFIG_LIBX264 0`, `CONFIG_LIBX265 0`, `CONFIG_LIBFDK_AAC 0`; `.config`: `BR2_PACKAGE_FFMPEG_GPL` and `_NONFREE` not set |
| libjpeg 9f | linked dynamically (pos-camera, pos-browser) | IJG | Buildroot metadata |
| libpng, libcurl, OpenSSL | linked dynamically | Libpng-2.0, curl, Apache-2.0 | Buildroot metadata |
| alsa-lib | linked dynamically (audio helpers) | LGPL-2.1+ | Buildroot metadata |
| OpenCV 4.10 (+ contrib) | **not linked by Doors**; in the image for vendor tools | Apache-2.0 | Buildroot metadata |
| Opus, Speex, libogg, webp, libv4l | not linked by Doors; in the image | BSD/LGPL | Buildroot metadata |
| ggwave | **compiled in** (pos-wave) | MIT | §3 |

No codec is vendored as source in this repository, and none is statically
linked into a Doors binary. GPL components are not enabled in FFmpeg.

## 9. Tests and sample media

- No binary fixture is tracked. JPEG, PNG, WAV, MP3, HTML and radio frames
  are synthesised by the tests (VERIFIED, file list and test sources).
- Test vectors: FIPS 180, RFC 4648, RFC 6234 (published); MeshCore's test key
  pair, hashtag vector and ported cases (MIT, attributed in the two test
  files' headers).
- `docs/hardware/hwcheck-unitA/**` (36 files) holds logs from unit A,
  including received LoRa packets (other people's encrypted payloads), and
  `docs/hardware/shots/` shows other people's mesh node names. Not a licence
  question; review for publication (C2).
- `tests/fake_wpa_supplicant.c` reproduces wpa_supplicant's control-interface
  behaviour, not its code (VERIFIED by reading).

## 10. Contributors and history

From `git log origin/master` (713 commits, VERIFIED):

| Identity | Commits authored | Notes |
| --- | --- | --- |
| KrakenSaten `<270574703+KrakenSaten@users.noreply.github.com>` | 654 | the owner's GitHub account |
| andre `<270574703+KrakenSaten@...>` | 33 | the same address: the same account under an earlier name |
| Claude `<noreply@anthropic.com>` | 26 | AI sessions run by the owner; 696 commits also carry a Co-Authored-By Claude trailer |
| GitHub (committer only) | - | merge commits of PRs #11-#20, all from the owner's branches |

No other person authored or co-authored a commit. Upstream MeshCore test
cases ported into `tests/meshcore_core_test.cpp` were written by MeshCore's
contributors and come under its MIT licence, which is kept.

**Reading:** the evidence suggests all Doors material comes from the project
owner and AI tooling the owner directed, so relicensing needs no contributor
permission. That is a reading of the history, not a legal finding. Whether
and how copyright subsists in AI-assisted material is a legal question this
audit does not answer; the licence grants whatever rights the licensor holds.

**Going forward** (CONTRIBUTING.md): inbound = outbound under Apache-2.0
section 5, a Developer Certificate of Origin sign-off recommended, no CLA.

## 11. Image and release obligations

Done on this branch (VERIFIED by tests, §16):

- `make install` puts `LICENSE` and `NOTICE` in `/usr/share/doors/` (0644),
  beside `THIRD_PARTY_NOTICES.txt` and its link at `/usr/share/pocketos/`;
  the bench `deploy.sh` checks for and sends both; `build_image.sh` refuses a
  build whose installed copies differ from the package's.
- `POCKETOS_LICENSE` names `Apache-2.0 (Doors)` first, then the third-party
  licences (now including BSD-2-Clause for the Canaan code);
  `POCKETOS_LICENSE_FILES = LICENSE NOTICE THIRD_PARTY_NOTICES.txt`;
  `pocketos.hash` holds the sha256 of all three, written by
  `gen_notices.sh`, so legal-info refuses a changed one.
- `POCKETOS_REDISTRIBUTE` stays `NO`: legal-info does not export the package
  source until this audit clears the source repository (ADR-013 decision 7).

Still required before an image is distributed (each is a blocker, §14): the
legal-info output archived per release with the GPL/LGPL source offer, the
toolchain runtimes' licences, the vendor packages' terms, the model, the
launcher and firmware, and the artwork.

## 12. Items safe for public release

Subject to B1-B3 being settled for the paths they name, the following are
original or properly attributed and can be published under their stated
licences:

- all first-party code and tests under `apps/`, `core/`, `services/`,
  `protocols/`, `tools/`, `ui/` and `tests/`, except `ui/pocketui/pos_keymap.c`
  (B3) and the generated art files (`pos_app_icons.c`, `pos_glyphs.*`,
  `pos_brand_mark.c`, `ui/assets/doors/*.bin`) (B1);
- the Makefile, CMake files, `platforms/k230/` scripts, package files, the
  defconfig fragment, init scripts, kernel patches (GPL-2.0), `rootfs_overlay/`
  text files; not `logo.xrgb` (B1);
- `third_party/notices/`, `THIRD_PARTY_NOTICES.txt`, `docs/legal/`;
- the project's own documentation, ADRs, hardware records and app documents;
  first-party design material (`doors-app-icons`, `doors-glyphs`,
  `timber-art`, `themes.json`, the DS documents), and simulator screenshots
  that do not show B1 art.

## 13. Items that must stay external

- **All models** (MODEL_LICENSES.md): never committed; `yolov8n.kmodel` only
  in internal images until B4 is settled.
- `vendor/` (git-ignored): LILYGO's repositories, the K230 SDK, RadioLib,
  ggwave, LVGL, MeshCore/RIFT, Crypto, libgpiod headers. Doors references
  them by commit and fetches them at build time.
- The nncase runtime archive, the SDK's vendor packages, the LILYGO launcher
  and firmware blobs: platform material, not part of the Doors source.

## 14. Unknown items and blockers

### Source repository

| # | Path / component | Issue | Evidence | Required action |
| --- | --- | --- | --- | --- |
| B1 | `docs/design/brand/doors-threshold/**`, `doors-icon-extension/**`, `doors-visual-pack-v1/**`; and what is generated from them: `ui/pocketui/pos_app_icons.c` (Threshold/extension masks), `pos_glyphs.c/.h`, `pos_brand_mark.c`, `ui/assets/doors/*.bin`, `platforms/k230/rootfs_overlay/logo.xrgb` | Owner-supplied artwork with **no stated author, copyright or licence**; the photographic art is AI-generated (OpenAI) and the supplier's notes are prompts and hand-off texts. Apache-2.0 can only be granted by whoever holds the rights. | docs/design/brand/README.md and doors-visual-pack-v1/README.md ("Author, copyright, licence: not stated"); C2PA chunks; ui/assets/doors/MANIFEST.txt | Owner: state who made each package and confirm the right to license it (own work, tool output whose terms assign the output to the owner, or a commission with assignment); then choose its licence (Apache-2.0 like the code, or a separate one for brand art) and record it in docs/design/brand/README.md. Until then do not publish these paths or their generated outputs. |
| B2 | `docs/design/rift/**`, `docs/design/PocketOS Design System.html`, `docs/design/Repository connection and design directions.zip` (the `apps/fleet/` copy is removed) | Design-tool exports and a supplied design package with no stated terms | §14.1 | **RESOLVED for the public-source candidate**: excluded by docs/licensing/public-source-exclude.txt, kept in the private repository. |
| B3 | (a) `platforms/k230/configs/k230_pocketos_defconfig`; (b) `ui/pocketui/pos_keymap.c` key-name and shifted-symbol tables | Copied from the LILYGO repository, which states **no licence** | §14.1 | (a) **RESOLVED**: composed at apply time. (b) **OPEN**: 59 of 65 keys have only LILYGO's code as their source; read them on a unit (KEYBOARD_BRINGUP §6) or obtain LILYGO's licence. |

### 14.1 Source-repository work, 2026-10-02

**SPDX audit of the tagged files.** All 890 files carrying
`SPDX-License-Identifier: Apache-2.0` were checked for third-party,
vendor-derived, generated, patch or external content: by path (none under
`third_party/`, `docs/legal/`, `vendor/`, the kernel patches, the supplied
design packages or `ui/pocketui/fonts/`), by generator markers (two hits,
both hand-written files that describe generated data), and by provenance
wording (copied, verbatim, ported, adapted, taken from, upstream's, vendor's
table). One file was reclassified: `tests/display_geometry_test.c`
reproduces LVGL's `_evdev_calibrate` arithmetic "verbatim" and is now
`Apache-2.0 AND MIT` with LVGL's notice named. Reviewed and kept Apache-2.0,
as facts, interfaces or public-domain material rather than copied code:
`tests/fake/gpiod.h` (libgpiod v2's interface names and enum values,
written as a fake), `apps/calendar/cal_date.c` (Hinnant's public-domain
algorithm), the HTML entity and cp1252 tables in `core/web/web_html.c` (39
names and code points), COCO class names, ArcFace and RetinaFace constants,
TCA8418 and SX1262 register values, pin numbers and the vendor launcher's
touch calibration numbers cited in tests.

**The public-source candidate.** Publishing this repository with its
history would publish every file it ever held, including those excluded
below and the removed defconfig. The candidate is therefore a **tree**,
exported from a commit by `tools/legal/public_source_tree.sh` without the
paths in `docs/licensing/public-source-exclude.txt`; a public repository
would start from that tree with fresh history (requirement R1). Nothing is
deleted from the private repository. `tests/license_audit_test.sh` runs the
exporter and checks that every exclusion still names tracked files, that no
build, generator or test input is excluded, that no design-tool export,
design zip or supplied font is in the candidate, and that LICENSE, NOTICE,
the notices, the build files and the defconfig fragment are.

**B2 - design-tool exports: resolved for the candidate.**

| Path | What it is | Needed by build or tests | Done |
| --- | --- | --- | --- |
| `apps/fleet/Repository connection and design directions.zip` | design-canvas export (React, dc-runtime, AI-generated reference PNGs); travelled in the Buildroot package source | no | **removed** from `apps/`; the byte-identical copy in `docs/design/` is kept (sha256 `47285c5c…`) |
| `docs/design/Repository connection and design directions.zip` | the same export | no | excluded from the candidate |
| `docs/design/PocketOS Design System.html` | bundled page: design-tool runtime, React, Plex WOFF2 | no (the DS is the Markdown documents) | excluded |
| `docs/design/rift/` (`support.js`, `RIFT for Doors.dc.html`, `shots/`, `HANDOFF.md`, `README.md`) | the RIFT design package, made with Claude (C2PA), terms not stated | no; code comments cite `HANDOFF.md` sections | excluded; the owner may re-admit the two texts after confirming their origin (B1 question Q1) |

**B1 - artwork: what is in the candidate, and what it needs.**

Reference-only parts excluded (B1R): Threshold `mockups/` and `reference/`,
the icon extension's overview sheet, and the visual pack's `originals/`
(supplier scripts, prompts and the AGPL Nimbus Sans fonts). Nothing builds
from them.

Still in the candidate, because the build or its tests use them:

| Material | Origin | Used by | Original Doors? |
| --- | --- | --- | --- |
| `docs/design/brand/doors-threshold/{icons,brand,boot}/`, `ASSET-NOTES.md` | Threshold package, supplied by the owner 2026-09-15 | `gen_app_icons.py`, `gen_brand_mark.py`, `png2xrgb.py`; app_icons, brand_mark and boot_splash tests | **UNKNOWN** |
| `docs/design/brand/doors-icon-extension/{png-24,png-32,svg}/`, `LES-MEG.md` | icon extension, supplied 2026-09-15 | `gen_app_icons.py`, `gen_doors_ui.py`; app_icons test | **UNKNOWN** |
| `docs/design/brand/doors-visual-pack-v1/{device,source,reference}/`, `README.md`, `SHA256SUMS` | visual pack v1 and B package v2, supplied 2026-09-22; photographic art AI-generated (OpenAI C2PA) | `gen_doors_ui.py`; doors_ui_assets test | **UNKNOWN** |
| `ui/pocketui/pos_app_icons.c` | generated: Threshold/extension masks plus 11 first-party icons | doors-shell | mixed: UNKNOWN parts |
| `ui/pocketui/pos_glyphs.c/.h` | generated: B package glyphs plus first-party `mode.svg` | doors-shell | mixed: UNKNOWN parts |
| `ui/pocketui/pos_brand_mark.c` | generated from the Threshold mark | doors-shell | UNKNOWN |
| `ui/assets/doors/icon-*.bin` (29), `bg-*.bin` (6) | generated: B frame and glyphs, Threshold/extension/first-party icons; the AI-generated backgrounds | installed in the image | mixed / UNKNOWN |
| `platforms/k230/rootfs_overlay/logo.xrgb` | generated from the Threshold boot art | boot splash | UNKNOWN |
| screenshots that show the above (launcher, brand and theme sheets) | Doors captures | documentation only | follow the art's answer |
| `docs/design/doors-app-icons/`, `doors-glyphs/`, `timber-art/` | drawn or generated in this repository | generators, tests | **ORIGINAL DOORS** |

The owner's confirmation needed, per package (Threshold, icon extension,
visual pack v1 with B package v2; and the RIFT texts if they are to be
re-admitted):

- **Q1 Who made it**: the owner personally, with which tools (an image or
  design generator: which one, under which account), or another person or
  company (who; is "Astra", named in docs/design/astra-handoff, a person,
  a company or a tool?).
- **Q2 The terms of each tool used**: that they give the output's rights to
  the user and allow commercial use and redistribution, as in force when the
  work was made.
- **Q3 For another person's work**: a written assignment or licence that
  allows publishing it under the chosen licence.
- **Q4 Third-party inputs**: whether any photo, artwork, font or brand of
  someone else's was used as a reference or input beyond Doors' own
  screenshots and IBM Plex (already OFL).
- **Q5 The licence for the art**: Apache-2.0 like the code, a separate
  licence such as CC BY 4.0, or reserved brand assets (logo, mark, name)
  licensed separately or not at all.
- **Q6 AI-generated images**: acknowledgement that copyright may not subsist
  in them, and that they are published on that basis.

If the confirmation cannot be given, the art has to be replaced before the
source is published: first-party icons exist for 11 apps; the rest, the
backgrounds, the frame, the glyphs, the mark and the splash would need new,
original drawings, and the generated files regenerated. That is design work
and is not done here.

**B3 - LILYGO-derived material.**

- **B3a defconfig: resolved.** `platforms/k230/configs/k230_pocketos_defconfig`
  is removed. `apply_to_sdk.sh` composes it in the SDK from the vendor's
  `k230_canmv_t_display_rm69a10_defconfig` read with `git show` at the pinned
  BSP commit (line ends made LF), followed by the settings of
  `platforms/k230/configs/k230_pocketos.fragment` (today one line,
  `BR2_PACKAGE_POCKETOS=y`); a fragment setting that restates a vendor
  setting is refused. LVGL's commit, which the notices name and which used to
  be read from the copied file, is pinned in
  `platforms/k230/vendor_lvgl_commit.txt`, and the composed defconfig must
  build it. The composition was VERIFIED byte-identical to the removed file
  (sha256 `e0b0b6ce57bd…1c7d`), so the image configuration is unchanged.
  `package_sync_test` executes the composition against the vendor checkout,
  with a refused restating fragment as the control.
- **B3b keymap: still blocked.** `ui/pocketui/pos_keymap.c` holds two tables
  whose only source for 59 of 65 keys is LILYGO's launcher code
  (`ui_hardware.c`); Doors itself verified six codes and legends on unit A
  (Shift 7, Z 18, Q 20, A 29, J 34, W 39), two of which corrected the vendor.
  LILYGO's hardware documentation gives the controller's pins and address,
  not the matrix or the legends, so there is no independent written source.
  A clean replacement needs the remaining keys read on hardware: the
  procedure in docs/hardware/KEYBOARD_BRINGUP_2026-09-10.md §6 (no new code;
  an operator presses every key and reads its keycap's primary and orange
  shifted legend), then the tables rebuilt from that record alone. That
  needs a unit, the keyboard base and the owner's go-ahead. The alternative
  is LILYGO's licence (docs/LICENSING.md item 2). The file says NOT CLEARED
  and carries no SPDX tag until then.

### Flashable image (in addition to B1-B3)

| # | Path / component | Issue | Evidence | Required action |
| --- | --- | --- | --- | --- |
| B4 | `/usr/share/doors/vision/yolov8n.kmodel` (`pocketos.mk`) | AGPL-3.0 weights (Ultralytics) without published weights or conversion; owner decision limits it to internal images | docs/LICENSING.md item 10; MODEL_LICENSES.md | Before a public image: stop installing it (Vision's DETECT becomes EXTERNAL ONLY like READ) or obtain terms that allow distribution (Ultralytics' commercial licence, or a model with published weights). |
| B5 | nncase 2.11.0 K230 runtime, static in `/usr/bin/pos-vision` | prebuilt archive carries no licence file; the K230 modules' source is not shown | archive listing; `kendryte/nncase` LICENSE at `v2.11.0` (Apache-2.0) | Confirm with Kendryte/Canaan that the runtime archive is under the repository's Apache-2.0, then add a notices entry with its licence; otherwise do not ship pos-vision. |
| B6 | vendor packages without licence metadata: `libnncase`, `gsl-lite`, `vvcam`, `face_detect`, `ai2d_kpu`, `nonai2d`, `libmmz`; their kmodels in `/root/app/` | no licence in the package files; models without terms | docs/LICENSING.md item 5; image target tree | Add manual manifest entries with established licences, or drop the packages not needed by Doors (`face_detect`, `ai2d_kpu` demos) from the defconfig. |
| B7 | LILYGO launcher `k230_phone_ui` and the LILYGO overlay files | no licence; the launcher ships enabled-in-image, disabled at boot | docs/LICENSING.md item 2; `.config` `BR2_PACKAGE_K230_PHONE_UI=y` | Get LILYGO's terms, or remove the launcher from the image (Doors' shell already owns the panel). |
| B8 | `rtl8723ds-bt` (PROPRIETARY), `rtl8723ds`, `aic8800` and their firmware | proprietary blob for absent hardware; firmware terms not collected | docs/legal/manifest.csv; item 4 | Drop them from the defconfig (hardware absent), as item 4 proposes. |
| B9 | glibc, libstdc++, libgcc_s from the Xuantie toolchain; legal-info archive | runtimes not in legal-info; no per-release archive of sources and licences | items 3 and 8 | Collect the toolchain runtime licences; run and archive `make legal-info` per release, and set `POCKETOS_REDISTRIBUTE = YES` once the source repository is cleared. |

### Non-blocking cleanup

| # | Item | Action |
| --- | --- | --- |
| C1 | 93 first-party files without a header (docs/licensing/spdx-exempt.txt, PENDING) | add the two header lines in a follow-up |
| C2 | unit captures with third parties' radio payloads and node names (`docs/hardware/hwcheck-unitA/`, `docs/hardware/shots/`) | review or redact before publishing |
| C3 | Nimbus Sans OTF copies (AGPL-3.0 with font exception) under `doors-visual-pack-v1/originals/` | remove, or keep beside their LICENSE.txt |
| C4 | IBM Plex release and download URL used for the bitmaps not recorded | record them in `tools/design/gen_fonts.sh` |
| C5 | Statements written before ADR-013 in release notes and hardware sheets (v0.0.10-v0.2.1: "Doors' own licence is not decided") | kept as history (ADR-005 decision 6) |
| C6 | Copyright holder wording "PocketOS authors" | owner may rename the holder line; not required by the licence |
| C7 | docs/legal/manifest.csv is the 2026-09-04 (v0.0.1) legal-info run | regenerate with B9 |

## 15. Recommended actions before the repository becomes public

1. Settle B1: the owner answers Q1-Q6 for each artwork package and
   chooses the art's licence; record it in docs/design/brand/README.md. Or
   replace the art.
2. Settle B3b: read the remaining 59 keys on a unit (owner's go-ahead) and
   rebuild the keymap tables from that record, or obtain LILYGO's licence.
3. Publish only an exported tree (`tools/legal/public_source_tree.sh`), never
   this repository's history (R1).
4. Re-run `tests/license_audit_test.sh` and `tests/notices_test.sh`, then flip
   `POCKETOS_REDISTRIBUTE` to `YES`.
5. Do C2 (third-party radio captures) and C3.
6. For any public image, additionally B4-B9.
7. Only then: repository visibility and any release, as separate owner
   actions.

### Final classification

| | Result |
| --- | --- |
| **SOURCE REPOSITORY** | **NOT READY** - B1 (owner's rights confirmation for the artwork) and B3b (keymap tables) remain; B2 and B3a are resolved. Publication must use an exported tree (R1). |
| **FLASHABLE IMAGE** | **NOT READY** - B1-B9 |

Scale used: READY / READY AFTER CLEANUP / NOT READY. "Ready after
cleanup" would mean only the C items remained; for the source that
happens once B1 and B3b are settled.

Doors' own code is licensed and ready; what is not ready is material in the
repository and the image that is not Doors' own code, or whose ownership is
not stated.

## 16. Validation of this branch

Licensing and packaging checks only, run under WSL on a clean clone of
the branch (VERIFIED, 2026-10-01/02):

| Check | Commit | Result |
| --- | --- | --- |
| `tests/license_audit_test.sh` (SPDX placement and syntax, headers or exemptions, notices vs inventory, model packaging rules with negative controls) | final tip | 0 failures |
| `tests/notices_test.sh` (notices current, LICENSE is the ASF text, NOTICE, hashes of all three, hash refusal controls, package metadata) | `55c5133` | 0 failures |
| `gen_notices.sh --verify-upstream --sdk ... --strict` (15 copied licence texts against the pinned upstream and the SDK's archives) | `55c5133` | rc 0, all byte-identical |
| `tests/identity_test.sh` (executes `make install`: LICENSE and NOTICE in /usr/share/doors, 0644, nothing else added; deploy.sh checks for and sends every installed path) | `55c5133` | 0 failures |
| `tests/deploy_staging_test.sh` (the bench deploy archive carries LICENSE and NOTICE) | `55c5133` | 0 failures |
| host `make all` | `55c5133`, `47edddc` | rc 0 |
| `bash -n` of the changed packaging scripts (`apply_to_sdk.sh`, `build_image.sh`, `deploy.sh`) | final tip | rc 0 |
| `tests/license_audit_test.sh` with the public-source checks (exclusions tracked, no build/generator/test input excluded, exporter executed, no design export or supplied font in the candidate, no vendor defconfig kept) | `32eab38` | 0 failures |
| `tests/notices_test.sh` (LVGL pin file; the vendor board defconfig at the pinned BSP commit builds that LVGL) | `32eab38` | 0 failures |
| `tests/package_sync_test.sh` (snapshot rules for the fragment; the composition executed against the vendor checkout: equals vendor file + fragment, builds the pinned LVGL, refuses a restating fragment) | `32eab38`; baseline origin/master `426b1d8` | 0 failures on both |
| `tools/legal/public_source_tree.sh` export and `--list` | `61211d8`, `32eab38` | 1,849 files, 8 exclusions, no `.dc.html`, `support.js`, `.zip` or `.otf` |
| composed defconfig = removed `k230_pocketos_defconfig` | vendor `bb831ab` | byte-identical (sha256 `e0b0b6ce…1c7d`) |

Not run, by the owner's instruction on 2026-10-02: the riscv64 and DRM
cross builds, `apply_to_sdk.sh` / `build_image.sh` against the SDK, and
Buildroot legal-info. The changes they would exercise are an install line
pair, a file list for the notices check and a post-build `cmp`/hash loop;
`build_image.sh`'s new loop has not run against a real build.

A full `make test` was started before that instruction and stopped by it.
Its only failures were outside licensing and reproduce on origin/master
`426b1d8` or after a rebuild: `initscript_test` (an S90 timing group and
`tests/vision_shell_test.sh` recorded 100644 on master), build-id checks
in `radiod_mock_test` and `sysd_test` (binaries older than a checkout;
pass after rebuilding), and one Video timing check that failed in the
full-suite run and passes on a fresh build on both master and this branch.
None of them is caused by this branch, which changes comments, documents,
packaging lists and tests only.
