# Licensing register

Started 2026-09-04. Commercial distribution of PocketOS is possible, so every
component's licence must be known before it is redistributed.

## First-party

PocketOS licence: not yet decided (product owner). Until then source files
carry "License: see LICENSE (TBD)" and nothing is published.

## Vendor and reference material

| Component | Licence | Evidence | Consequence |
| --- | --- | --- | --- |
| kendryte/k230_linux_sdk | BSD-style (Canaan Bright Sight, 2024), LICENSE file present | VERIFIED locally | Usable; keep the notice. |
| Buildroot 2025.02.1 and its packages | Various; Buildroot `make legal-info` produces the list | DOCUMENTED | Run legal-info per image and archive the output. |
| Linux kernel + LILYGO BSP kernel patches | GPL-2.0 | DOCUMENTED (kernel licence; patches are derivative) | Kernel source for shipped images must be offered. |
| U-Boot 2022.10 + overlay | GPL-2.0+ | DOCUMENTED | Same as kernel. |
| Xinyuan-LilyGO/T-Display-K230 (BSP scripts, launcher `k230_phone_ui`) | **No LICENSE file, no headers** | VERIFIED locally 2026-09-04 | No right to copy code from the launcher (including its RadioLib Linux HAL) until LILYGO states a licence. Use as documentation only, or ask LILYGO. |
| Xinyuan-LilyGo/T-Display-K230_canmv_rt | GPL-3.0 per README header | VERIFIED locally | Reference only. Not linked into PocketOS. |
| RadioLib 7.7.1 (vendor/RadioLib, compiled into radiod) | MIT (license.txt present) | VERIFIED locally | Fetched from upstream at tag 7.7.1; the launcher copy is not used. |
| ggwave v0.4.3 (vendor/ggwave at a38e38b, compiled into pos-wave) | MIT (LICENSE, "Copyright (c) 2020 Georgi Gerganov") | VERIFIED locally 2026-09-13 | Fetched from upstream at tag ggwave-v0.4.3. Keep the notice; the package carries LICENSE. |
| ggwave's Reed-Solomon (src/reed-solomon, Mike Lubinets) | MIT permission text (its own LICENSE; the word "MIT" does not appear) | VERIFIED locally 2026-09-13 | Keep the notice; the package carries it. |
| ggwave's FFT (src/fft.h, Takuya Ooura's FFT package, fft4g-derived `rdft`) | Author's terms: use, copy, modify and distribute for any purpose including commercial use, without fee; refer to the package when modifying. The file itself carries only the copyright line and the package URL | VERIFIED 2026-09-13 on the author's page https://www.kurims.kyoto-u.ac.jp/~ooura/fft.html ("License" section, verbatim in docs/legal/third-party/ooura-fft.txt) | **Resolved** for use and redistribution. Keep the header (copyright and package reference) intact; ship the notice with pos-wave (see "Audio milestone", item B). |
| alsa-lib 1.2.13 (Buildroot package, dynamically linked by pos-wave) | LGPL-2.1-or-later | DOCUMENTED (docs/legal/manifest.csv) | Already in the image for alsa-utils; dynamic linking. |
| libgpiod 2.2 (Buildroot package, dynamically linked by radiod) | LGPL-2.1-or-later | DOCUMENTED (header SPDX) | Dynamic linking keeps PocketOS code separate; offer library source. Header copy in vendor/libgpiod is for host compile checks only. |
| nofrendo (bundled) | GPL-2.0 upstream | DOCUMENTED | Not needed by PocketOS. |
| libtmt, qrcodegen (bundled, no LICENSE copies) | MIT upstream | DOCUMENTED | Fetch upstream with LICENSE if ever used. |
| quirc (bundled) | ISC (LICENSE present) | VERIFIED locally | Fine if used, keep notice. |
| LVGL 9 (pinned commit 59dc7e4, vendor/lvgl) | MIT (LICENCE.txt present) | VERIFIED locally | Fine, keep notice. Bundled lodepng (zlib licence) used for screenshots. |
| SDL2 (simulator only, not shipped) | zlib | DOCUMENTED | Host-only. |
| IBM Plex Sans / Mono (converted to LVGL bitmaps in `ui/pocketui/fonts/`) | OFL-1.1 with Reserved Font Name "Plex" | VERIFIED (upstream `license.txt`, copy in `docs/legal/fonts/`) | Bitmaps are Modified Versions: symbols are `pos_font_*`, and the UI must never present them as "IBM Plex" (DS decision 2026-09-04). OFL text ships with the fonts. |
| lv_font_conv 1.5.3 (host tool) | MIT | DOCUMENTED | Host-only, run from a local Node 20 tarball; generated C files are committed so builds need neither. |
| cJSON 1.7.x (Buildroot package, used by pocketipc, pos, radiod) | MIT | DOCUMENTED | First PocketOS dependency: ~40 kB library, no transitive deps, justified in docs/api/pocketipc.md. |

## Audio milestone: pos-wave's third-party code (2026-09-13)

`pos-wave` (branch `feature/audio-ggwave`) links ggwave, which carries two
pieces of other authors' code, and dynamically links alsa-lib. Classified as
(a) resolved, (b) acceptable for source development but blocking
distribution, (c) unresolved:

| Item | Finding | Class |
| --- | --- | --- |
| ggwave (Georgi Gerganov) | MIT; LICENSE in the repository and copied into the package source | **(a) resolved** |
| Reed-Solomon (Mike Lubinets, in ggwave) | MIT permission text in its own LICENSE, copied into the package source | **(a) resolved** |
| FFT (Takuya Ooura, in ggwave `src/fft.h`) | Provenance: added to ggwave in upstream commit f5e08d9 (2022-06-04), header names the author's FFT package page; the code is that package's radix-4,2 `rdft`, reduced to float arrays. Terms: stated on the author's page, not in the file - permissive, including commercial use and redistribution of modified code, with a request to refer to the package when modifying, which the ggwave header does. Recorded verbatim with the retrieval date in docs/legal/third-party/ooura-fft.txt | **(a) resolved** |
| alsa-lib 1.2.13 | LGPL-2.1-or-later, dynamic linking, already a Buildroot package in the image with its source in legal-info | **(a) resolved** |
| **B. Notices in a distributed image** | The `pocketos` Buildroot package declares no `POCKETOS_LICENSE_FILES`, so `make legal-info` collects none of the MIT notices for code compiled into PocketOS binaries (ggwave, Reed-Solomon, and - already before this milestone - RadioLib in radiod), nor the Ooura terms, and nothing in the image carries them. MIT and the Ooura terms both expect the notice to travel with copies | **(b) acceptable for source development; blocks distribution** until the package ships these notices (open item 7) |
| PocketOS's own licence | Not decided (open item 1) | **(c) unresolved**; a release blocker independent of audio |

## Image manifest (PocketOS 0.0.1, 2026-09-04)

Generated with Buildroot `make legal-info` for `k230_pocketos_defconfig`;
`docs/legal/manifest.csv` lists 96 target packages with licence and source
archive, `docs/legal/licenses/` holds the licence texts, and the SDK output
`legal-info/sources/` (716 MB, not committed) holds the source archives that
must be offered with a distributed image.

Findings:

- Copyleft components that require a source offer: Linux kernel, U-Boot,
  BusyBox, the Realtek Wi-Fi drivers (GPL-2.0 kernel modules), alsa-utils,
  bluez, dbus, e2fsprogs, util-linux, xz, wireless_tools, spi-tools, and the
  GPL-3.0+ userspace tools readline, dosfstools, parted, umtprd. No AGPL.
- LGPL libraries linked by PocketOS code: libgpiod2 (radiod), glib and
  friends indirectly. Dynamic linking; keep relinking possible.
- `rtl8723ds-bt` is marked PROPRIETARY (Realtek Bluetooth firmware) and
  supports a chip this board does not have; candidate for removal from the
  PocketOS defconfig together with `aic8800` and `rtl8723ds`.
- Licence metadata missing in the vendor SDK packages: `libnncase` (nncase
  upstream is Apache-2.0), `gsl-lite` (MIT upstream), `lvgl` (MIT, see
  above). Vendor local packages (`k230_phone_ui`, `vvcam`, `face_detect`,
  `ai_demo`) do not appear in the manifest at all and need manual entries.
- GPLv3 anti-tivoisation is not an issue as long as users can flash their
  own images to the SD card, which is the intended update path.

## Open items

1. Decide the PocketOS licence.
2. Ask LILYGO to add a LICENSE to the T-Display-K230 repository, or treat it as
   all-rights-reserved documentation.
3. Add `make legal-info` to the image build and archive the result per release
   (first run done 2026-09-04, see above).
4. Decide whether to drop `rtl8723ds`, `rtl8723ds-bt` and `aic8800` from the
   PocketOS defconfig (hardware absent; one proprietary blob less).
5. Add manual manifest entries for vendor local packages.
6. ~~Confirm the licence terms of Ooura's FFT (ggwave `src/fft.h`), which the
   file itself does not state, before a release ships pos-wave.~~ Resolved
   2026-09-13 from the author's page (see "Audio milestone"; verbatim terms in
   docs/legal/third-party/ooura-fft.txt).
7. **Release blocker:** ship the third-party notices for code compiled into
   PocketOS binaries - RadioLib (radiod), ggwave and its Reed-Solomon code
   (pos-wave), and the Ooura FFT terms - with any distributed image, for
   example as `POCKETOS_LICENSE_FILES` plus a notices file installed in the
   image. Source development and bench deployment are not affected.
