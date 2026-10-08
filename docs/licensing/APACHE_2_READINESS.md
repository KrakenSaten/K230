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
  public **source** release (status 2026-10-02, §14.1; B1 detail in
  docs/licensing/B1_ARTWORK.md): the design-tool
  exports (B2) are now kept out of the public-source candidate, and the
  copied LILYGO defconfig (B3a) is replaced by a Doors fragment composed with
  the vendor file at apply time, and the artwork (B1) is resolved: the owner
  made it with ChatGPT from the owner's own material; it is Apache-2.0
  except the Doors mark, lockups and boot splash, which are reserved
  (docs/licensing/BRAND.md). Still blocking: the keyboard tables copied from
  LILYGO's launcher (B3b), deferred until the owner is at a unit, which need
  the remaining keys read on hardware or LILYGO's licence.
- **The flashable image is further from ready.** Besides B3b it ships
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
| `face_det.kmodel` (vendor `face_detection_320.kmodel`) | FACE, RECOGNIZE, DeskBuddy | none stated | no | not by Doors; the vendor's copy left the image in 0.3.5 | UNKNOWN; EXTERNAL ONLY for Doors |
| `text_det.kmodel`, `text_rec.kmodel`, `text_dict.txt` (canmv `ai_poc`) | READ | none stated | no | no | UNKNOWN; EXTERNAL ONLY |
| `face_embed.kmodel` (canmv `face_recognition.kmodel`) | RECOGNIZE, DeskBuddy | none stated | no | no | UNKNOWN; EXTERNAL ONLY |
| `xiaozhi_kws.kmodel`, `test.kmodel` | none (vendor demos) | none stated | no | no (launcher removed, PR #47; `ai2d_kpu` off, 0.3.5) | NOT USED by Doors |

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
| Threshold package (`docs/design/brand/doors-threshold/`): mark, lockups, 10 icons, boot art | made by the owner with ChatGPT, supplied 2026-09-15 (owner, 2026-10-02; docs/licensing/B1_ARTWORK.md) | `logo.xrgb` (boot splash), `pos_brand_mark.c`, icon masks in `pos_app_icons.c`, portal glyphs in `ui/assets/doors/icon-*.bin` | icons: Apache-2.0; mark, lockups, boot art: **reserved** (BRAND.md) |
| Icon extension (`doors-icon-extension/`) | made by the owner with ChatGPT, supplied 2026-09-15 | Wave, Files, Camera, Recorder, Gallery icons | Apache-2.0 |
| DOORS visual pack v1 and B package v2 (`doors-visual-pack-v1/`) | made by the owner with ChatGPT, supplied 2026-09-22; photographic art AI-generated (PROVENANCE.md; OpenAI content credentials) | the six backgrounds `bg-*.bin`, the portal frame, the system glyphs (`pos_glyphs.c`) | Apache-2.0 (to the extent rights subsist in the AI images) |
| `docs/design/doors-app-icons/` (12 SVGs and their PNGs), `docs/design/doors-glyphs/mode.svg` | drawn in this repository (their README) | 11 app icons, the mode glyph | ORIGINAL DOORS - Apache-2.0 |
| Desk stand model (`hardware/stand/k230-desk-stand.stl`) | the owner's own design, Apache-2.0 authorized by the owner (2026-10-08) | not shipped | ORIGINAL DOORS - Apache-2.0 |
| PocketTimber sprites (`docs/design/timber-art/`) | generated by `tools/timber_blender.py`, procedural materials only | compiled into the shell | GENERATED FOR DOORS - Apache-2.0 |
| Screenshots (`docs/design/shots/`, `docs/apps/**`, `docs/hardware/shots/`, ...) | simulator and unit captures of Doors | not shipped | Apache-2.0; a Doors mark they show keeps BRAND.md's terms; some show other people's mesh node names (C2) |
| RIFT design package (`docs/design/rift/`) | made by the owner with Claude from the owner's own material, 2026-09-19 (owner, 2026-10-02); content credentials "Claude provided this file" | not shipped | Apache-2.0; the design tool's runtime `support.js` stays excluded (B2) |

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

Subject to B3b for the one file it names, the following are original or
properly attributed and can be published under their stated licences:

- all first-party code and tests under `apps/`, `core/`, `services/`,
  `protocols/`, `tools/`, `ui/` and `tests/`, except `ui/pocketui/pos_keymap.c`
  (B3b); `pos_brand_mark.c` is published under the brand terms
  (BRAND.md), the other generated art under Apache-2.0;
- the Makefile, CMake files, `platforms/k230/` scripts, package files, the
  defconfig fragment, init scripts, kernel patches (GPL-2.0), `rootfs_overlay/`
  text files; `logo.xrgb` under the brand terms (BRAND.md);
- `third_party/notices/`, `THIRD_PARTY_NOTICES.txt`, `docs/legal/`;
- the project's own documentation, ADRs, hardware records and app documents;
  first-party design material (`doors-app-icons`, `doors-glyphs`,
  `timber-art`, `themes.json`, the DS documents, the desk stand model in
  `hardware/stand/`), and simulator screenshots
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
| B1 | the owner-supplied artwork packages and what is generated from them | was: no stated author or terms | docs/licensing/B1_ARTWORK.md | **RESOLVED 2026-10-02**: made by the owner with ChatGPT from the owner's own material (OpenAI's terms assign the output to the user); Apache-2.0, except the brand assets, reserved with permission to redistribute them unmodified as part of Doors (docs/licensing/BRAND.md). |
| B2 | `docs/design/rift/support.js`, `docs/design/PocketOS Design System.html`, `docs/design/Repository connection and design directions.zip` (the `apps/fleet/` copy is removed) | Design-tool runtime and exports that embed it; terms not stated | §14.1 | **RESOLVED for the public-source candidate**: excluded by docs/licensing/public-source-exclude.txt, kept in the private repository. The rest of the RIFT package is the owner's and re-admitted. |
| B3 | (a) `platforms/k230/configs/k230_pocketos_defconfig`; (b) `ui/pocketui/pos_keymap.c` key-name and shifted-symbol tables | Copied from the LILYGO repository, which states **no licence** | §14.1 | (a) **RESOLVED**: composed at apply time. (b) **DEFERRED (hardware), still blocking**: 59 of 65 keys have only LILYGO's code as their source; read them on a unit when the owner is there (KEYBOARD_BRINGUP §6) or obtain LILYGO's licence. |

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
| `docs/design/rift/support.js` | the design tool's runtime ("GENERATED from dc-runtime/src/*.ts") | no | excluded. The rest of the RIFT package - `HANDOFF.md`, `README.md`, `shots/`, `RIFT for Doors.dc.html` - was made by the owner with Claude from the owner's own material and is re-admitted under Apache-2.0 (owner, 2026-10-02; B1_ARTWORK.md) |

**B1 - artwork: resolved.**

Reference-only parts stay excluded (B1R): Threshold `mockups/` and
`reference/`, the icon extension's overview sheet, and the visual pack's
`originals/` (the supplier-side scripts, prompts and the AGPL Nimbus Sans
fonts). Nothing builds from them; their rights are clear now, and they can
be re-admitted without the fonts if wanted.

**Resolved 2026-10-02.** The owner's answers, the evidence for each and
the resulting licences are in **docs/licensing/B1_ARTWORK.md**; the brand
terms are in **docs/licensing/BRAND.md**, and NOTICE states them. Every
asset has a class in `docs/licensing/asset-inventory.txt`, held complete by
`tests/license_audit_test.sh`.

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
- **B3b keymap: DEFERRED (hardware), still blocking.** Deferred by the
  owner on 2026-10-02: the owner is not at the device, so no unit is
  touched and no keyboard capture is prepared or run.
  `ui/pocketui/pos_keymap.c` holds two tables whose only source for 59 of
  65 keys is LILYGO's launcher code (`ui_hardware.c`); Doors itself verified
  six codes and legends on unit A (Shift 7, Z 18, Q 20, A 29, J 34, W 39),
  two of which corrected the vendor. LILYGO's hardware documentation gives
  the controller's pins and address, not the matrix or the legends, so there
  is no independent written source. When the owner is at a unit with the
  keyboard base: the procedure in docs/hardware/KEYBOARD_BRINGUP_2026-09-10.md
  §6 (no new code; every key pressed, its primary and orange shifted keycap
  legend read), then the tables rebuilt from that record alone. The
  alternative is LILYGO's licence (docs/LICENSING.md item 2). The file says
  NOT CLEARED and carries no SPDX tag until then.

### Flashable image (in addition to B3b)

| # | Path / component | Issue | Evidence | Required action |
| --- | --- | --- | --- | --- |
| B4 | `/usr/share/doors/vision/yolov8n.kmodel` (`pocketos.mk`) | AGPL-3.0 weights (Ultralytics) without published weights or conversion; owner decision limits it to internal images | docs/LICENSING.md item 10; MODEL_LICENSES.md | **Resolved for 0.3.5 (2026-10-07):** not installed; the package refuses the SDK's YOLO kmodels by name and hash, and the vendor `yolo` package. Vision runs its model-free modes and says why the detector's are off. |
| B5 | nncase 2.11.0 runtime, static in `/usr/bin/pos-vision` (generic runtime and the K230 modules `libnncase.rt_modules.k230`, `libfunctional_k230`) | the prebuilt archive `nncase_k230_v2.11.0_runtime_linux.tgz` carries no licence file | §14.2 | **Partly resolved (0.3.5):** the generic runtime is Apache-2.0 at tag `v2.11.0` and its notices ship, with itlib-small-vector and gsl-lite (MIT); the runtime's Python wheel (unused) is removed from the image. **Still blocking:** the K230 modules, built from a tree upstream says is not open source: no terms found for these binaries. Supporting evidence only: Canaan's `nncase-kpu` 2.11.0 compiler plug-in on PyPI, from the same closed tree, is Apache-2.0. Needs Canaan's statement (draft inquiry in docs/licensing/inquiries/). |
| B6 | vendor packages without licence metadata: `libnncase`, `gsl-lite`, `vvcam`, `face_detect`, `ai2d_kpu`, `nonai2d`, `libmmz`, `display`, and the vendor local packages legal-info never lists (`vg_lite`, `mvx_player`, `camera_rtsp_demo`, `librtsp_server`, `audio_demo`, `audio_rec_play`); their kmodels in `/root/app/` | no licence in the package files; models without terms | §14.2 | **Code: resolved by evidence** (gsl-lite MIT; libmmz, display, face_detect code: the SDK's BSD-2-Clause; nonai2d and vvcam's kernel modules GPL-2.0 with source in the SDK). **Still blocking:** `face_detection_320.kmodel`, `test.kmodel` and test data, vvcam's binary-only `isp_media_server`, and the demo packages' unverified files: vendor terms, or the owner drops them (§14.2). |
| B7 | LILYGO launcher `k230_phone_ui` and the LILYGO overlay files | no licence | docs/LICENSING.md item 2 | Launcher **resolved** (removed from the image, PR #47). **Still open:** LILYGO's BSP overlay (boot scripts such as `S40k230_pocketos_defconfig`, the board defconfig the image is composed from); LILYGO's terms are needed for those. |
| B8 | `rtl8723ds-bt` (PROPRIETARY, also ships the GPL-2.0+ `rtk_hciattach`), `rtl8723ds`, `aic8800` (70 firmware blobs, no terms) | firmware for chips the board does not have (RTL8189FTV only) | §14.2 | **Resolved (0.3.5, owner 2026-10-07):** the Doors fragment turns off `BR2_PACKAGE_RTL8723DS`, `_RTL8723DS_BT` and `_AIC8800` (absent hardware; `rtl8189fs`, the board's Wi-Fi, kept), and the package removes what they left in an existing target tree. The vendor post-build script writes the boot script's module loads from the selected packages, so it now loads only `8189fs`. |
| B9 | glibc 2.33 and the GCC 14.1.1 runtime libraries from the Xuantie-900 toolchain V3.0.2; legal-info archive | runtimes not in legal-info; no per-release archive of sources and licences | §14.2 | **Texts resolved (0.3.5):** LGPL-2.1, GPL-2.0, glibc's LICENSES, GPL-3.0 and the GCC Runtime Library Exception 3.1 ship in the notices, copied from the fork at named commits. legal-info re-run for 0.3.5 (§14.2). **Still open:** the source offer: an owner decision whether the fork's branch heads count as corresponding source (no V3.0.2 tag), or XuanTie's exact source; and `POCKETOS_REDISTRIBUTE = YES` once the source repository is cleared. |

### 14.2 Image blockers researched for 0.3.5 (2026-10-07)

From the 0.3.5 build tree (SDK `22d02c6`, BSP `bb831ab`) and the original
sources at exact versions. Evidence class in brackets.

**Resolved by evidence (done in 0.3.5):**

- nncase generic runtime 2.11.0: Apache-2.0, `kendryte/nncase` tag `v2.11.0`
  (tag object `2be57e3`) [VERIFIED]; the archive's objects match
  `src/Native/src/runtime` at the tag [VERIFIED by name; that the binary was
  built from the tag ASSUMED]. Notices `nncase-runtime`,
  `itlib-small-vector` (MIT, from the archive's header), `gsl-lite` (MIT,
  v0.41.0 archive LICENSE) [VERIFIED].
- Toolchain runtime licence texts: glibc 2.33 `COPYING.LIB` and
  `LICENSES` from `XUANTIE-RV/glibc@29dd660` (branch `riscv-glibc-2.33-thead`;
  its `version.h` says 2.33), GCC `COPYING3` and `COPYING.RUNTIME` from
  `XUANTIE-RV/gcc@c2e0bcc` (branch `xuantie-gcc-14.1.1`; `BASE-VER` 14.1.1)
  [VERIFIED]. These are the licences' texts only; the commits are not
  established as the build's source (B9). The toolchain itself ships no
  licence text [VERIFIED].
  Provenance and hashes: notices entries `glibc*`, `gcc-runtime*`.
- Vendor package code with established terms (manifest entries in
  docs/legal/LOCAL_PACKAGES.md): `gsl-lite` MIT; `libmmz` and `display`
  (no headers; the SDK root BSD-2-Clause by location [ASSUMED coverage]);
  `face_detect` sources Canaan BSD-2-Clause; `nonai2d` GPL-2.0-only and
  `vvcam`'s kernel modules VeriSilicon/Vivante MIT and GPL-2.0, source in
  the SDK [VERIFIED].
- `make legal-info` re-run on the 0.3.5 build (docs/legal/manifest.csv).

**Needs the owner's decision (documented, not done):**

1. B5: ship the nncase K230 modules and the wheel on the reading that a
   release asset of an Apache-2.0 repository is Apache-2.0 (with a recorded
   risk), or build public images without the KPU (pos-vision without
   `POCKETVISION_KPU`, no `libnncase`; COLOR, EDGE and LINE TRACE would need
   the helper to work without the KPU at all), or ask Canaan.
2. B6: ~~drop `face_detect` and `ai2d_kpu`~~ **done** (owner, 2026-10-07:
   unused vendor demos; nothing starts them and no Doors binary references
   their files [VERIFIED]). Still to decide: `camera_rtsp_demo`,
   `librtsp_server`, `mvx_player`, `audio_demo`, `audio_rec_play`,
   `vg_lite`. **Camera needs `isp_media_server` at run time** [VERIFIED]:
   the vendor boot script `S31canaan_isp` starts it, and Doors' camera reads
   the vvcam ISP it drives (`core/pocketcam/pocketcam_v4l2.c` lines 7-8;
   ADR-006; CAMERA_PLATFORM_RESEARCH.md lines 39 and 53). It cannot be
   dropped without losing Camera and Vision; it is binary-only (V6.5.0) and
   no terms were found.
3. B8: ~~unset `RTL8723DS`, `RTL8723DS_BT` and `AIC8800`~~ **decided and done** (owner, 2026-10-07).
4. B9: the corresponding source. XuanTie's V3.0.2 page offers binaries,
   manuals and a ReleaseNote.pdf that needs a sign-in to download (not
   read); no source archive or revision for build B-20250410 is published,
   and GitHub's newest XuanTie toolchain tag is V3.0.1. The public fork
   commits the notices' texts come from (glibc `29dd660`, gcc `c2e0bcc`)
   are **not** established as this build's source (glibc `29dd660` is dated
   after the build). No written offer can be made until the corresponding
   source is in hand.
5. B3b: when to read the keys on a unit. The KEYBOARD_BRINGUP §6 procedure
   used the vendor launcher's keyboard test page, which PR #47 removed, so it
   needs a card that still has the launcher or a small raw-key view in Doors.

**Needs the vendor:** Canaan (the nncase K230 modules in the runtime
archive, `isp_media_server`), XuanTie (the corresponding source of the
toolchain build), LILYGO (BSP overlay and keymap). Draft inquiries for the
owner: docs/licensing/inquiries/.

**Also done in 0.3.5 (owner, 2026-10-07):** the nncase Python wheel
(unused; it carried the K230 modules) is removed from the image after the
build; `face_detect` and `ai2d_kpu` with both vendor models are off;
`/lib/libasan.so.8` (copied by the vendor post-build outside any package,
used by nothing) and libgfortran (no user; Fortran off) are gone; the vendor
developer script `root/script/sensor.sh` is no longer in the image; the
notices no longer label `ldd` GPL (it is LGPL-2.1-or-later).

### 14.3 Owner decision: publish v0.3.5 (2026-10-07)

The product owner decided to publish Doors 0.3.5 publicly - the source in
this public repository and the image as a GitHub release - **with the
items below still open**, and to stop the vendor investigation for this
release. This records that decision; it does not resolve any of them, and
no permission or source material was obtained that §14.2 does not list:

- the nncase K230 modules linked into pos-vision
  (`libnncase.rt_modules.k230`, `libfunctional_k230`): no terms found for
  these binaries; Canaan's Apache-2.0 compiler plug-in on PyPI is
  supporting evidence only;
- vvcam's binary-only `isp_media_server` V6.5.0, which Camera needs: no
  terms found;
- the toolchain runtime (glibc 2.33, the GCC 14.1.1 runtime): its licence
  texts ship, but XuanTie publishes no source for build B-20250410, so the
  corresponding source is not in hand and no written offer is made;
- LILYGO's BSP overlay (boot scripts, the board defconfig the image is
  composed from) and the two keymap tables in the shell: no licence
  stated.

What the release does carry: Doors' own code under Apache-2.0 (LICENSE,
NOTICE), the third-party notices, and the source material Buildroot's
legal-info collects (docs/releases/v0.3.5.md, "Download and source
material"), described there as not complete corresponding source for the
whole image.

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
| C8 | the owner's private LAN addresses (192.168.x) and the units' MAC addresses in hardware records under `docs/hardware/` (test files use made-up values) | review or redact before publishing; not a licence question. No keys, passwords or tokens were found in the candidate (scan 2026-10-02) |

## 15. Recommended actions before the repository becomes public

1. ~~Settle B1~~ - resolved 2026-10-02 (docs/licensing/B1_ARTWORK.md).
2. Settle B3b (deferred, hardware): when the owner is at a unit, read the
   remaining 59 keys and rebuild the keymap tables from that record, or
   obtain LILYGO's licence.
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
| **SOURCE REPOSITORY** | **NOT READY** - only B3b remains (keymap tables; deferred until the owner is at a unit). B1, B2 and B3a are resolved. Publication must use an exported tree (R1). |
| **FLASHABLE IMAGE** | **NOT READY** - B3b, B5 (K230 modules, wheel), B6 (vendor models and binaries), B7 (LILYGO overlay), B9 (source offer). B4 and B8 resolved in 0.3.5; B5, B6 and B9 partly (§14.2) |

Scale used: READY / READY AFTER CLEANUP / NOT READY. "Ready after
cleanup" would mean only the C items remained; for the source that
happens once B3b is settled.

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
| `tests/license_audit_test.sh` with the asset inventory (every asset classified, no stale rule, reference/export/third-party assets out of the candidate) | `a2a0fb4` | 0 failures; candidate assets: 108 ORIGINAL, 103 CAPTURE-B1, 158 SUPPLIED-VECTOR, 16 SUPPLIED-BRAND, 16 AI-RASTER, 35 DERIVED-B1, 1 DERIVED-BRAND (image files; the generated C art is counted in B1_ARTWORK.md) |
| `tests/notices_test.sh` | `a2a0fb4` | 0 failures |
| public-source export | `a2a0fb4` | 1,851 files; none under `originals/`, `mockups/`, no `.dc.html`, `support.js`, `.zip` or `.otf` |
| export cleanliness scan (private keys, credentials, Wi-Fi names, LAN addresses, MACs) | `a14029f` | no keys or credentials; LAN addresses and MACs in hardware records (C8) |
| `tests/license_audit_test.sh` after B1 (new classes; every brand file listed in BRAND.md with no Apache-2.0 tag; NOTICE names the brand terms) | `cb86af8` | 0 failures; candidate assets: 108 ORIGINAL, 158 OWNER-VECTOR, 16 OWNER-AI-RASTER, 16 BRAND, 35 DERIVED-ART, 1 DERIVED-BRAND, 103 CAPTURE |
| `tests/notices_test.sh` (NOTICE changed; pocketos.hash regenerated) | `cb86af8` | 0 failures |
| public-source export | `cb86af8` | 1,852 files, 8 exclusions, none of the excluded groups present |
| `tests/license_audit_test.sh`, `tests/notices_test.sh` after re-admitting the RIFT package | `d35b9be` | 0 failures each; candidate assets add 11 OWNER-AI-RASTER (RIFT shots) and 1 OWNER-DESIGN |
| public-source export | `d35b9be` | 1,866 files; `docs/design/rift/` present without `support.js`; no `.zip`, `.otf`, `originals/` or `mockups/` |

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
