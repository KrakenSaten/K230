# Licensing register

Started 2026-09-04. Commercial distribution of Doors (called PocketOS through
v0.0.9, ADR-005) is possible, so every component's licence must be known
before it is redistributed.

## First-party

Doors licence: **not yet decided** (product owner). The rename changes no
term below, and the owner's decisions recorded under the PocketOS name apply
to Doors unchanged. Until the owner chooses a licence:

- no licence is granted for Doors' own code, and there is no LICENSE file;
- **redistributing Doors outside the project, as source or as binaries, is
  not authorised** (owner, 2026-09-13);
- source files carry "Copyright (c) 2026 PocketOS authors. License: see
  LICENSE (TBD)" and nothing is published. The copyright line keeps the
  PocketOS name: who holds it is part of the licence decision, not of the
  rename (ADR-005, decision 7);
- the Buildroot package says the same: `POCKETOS_LICENSE = Not yet decided
  (Doors; no licence granted), ...` and `POCKETOS_REDISTRIBUTE = NO`, so
  `make legal-info` does not export Doors' source, and
  THIRD_PARTY_NOTICES.txt opens with the statement, naming the former name
  and the copyright lines.

This does not block internal development, bench deployment or merging; it is
the owner's release and public-distribution decision (open item 1).

## Vendor and reference material

| Component | Licence | Evidence | Consequence |
| --- | --- | --- | --- |
| kendryte/k230_linux_sdk | BSD-style (Canaan Bright Sight, 2024), LICENSE file present | VERIFIED locally | Usable; keep the notice. |
| Buildroot 2025.02.1 and its packages | Various; Buildroot `make legal-info` produces the list | DOCUMENTED | Run legal-info per image and archive the output. |
| Linux kernel + LILYGO BSP kernel patches | GPL-2.0 | DOCUMENTED (kernel licence; patches are derivative) | Kernel source for shipped images must be offered. |
| U-Boot 2022.10 + overlay | GPL-2.0+ | DOCUMENTED | Same as kernel. |
| Xinyuan-LilyGO/T-Display-K230 (BSP scripts, launcher `k230_phone_ui`) | **No LICENSE file, no headers** | VERIFIED locally 2026-09-04 | No right to copy code from the launcher (including its RadioLib Linux HAL) until LILYGO states a licence. Use as documentation only, or ask LILYGO. |
| Xinyuan-LilyGo/T-Display-K230_canmv_rt | GPL-3.0 per README header | VERIFIED locally | Reference only. Not linked into Doors. |
| RadioLib 7.7.1 (vendor/RadioLib, compiled into radiod) | MIT (license.txt present) | VERIFIED locally | Fetched from upstream at tag 7.7.1; the launcher copy is not used. |
| ggwave v0.4.3 (vendor/ggwave at a38e38b, compiled into pos-wave) | MIT (LICENSE, "Copyright (c) 2020 Georgi Gerganov") | VERIFIED locally 2026-09-13 | Fetched from upstream at tag ggwave-v0.4.3. Keep the notice; the package carries LICENSE. |
| ggwave's Reed-Solomon (src/reed-solomon, Mike Lubinets) | MIT permission text (its own LICENSE; the word "MIT" does not appear) | VERIFIED locally 2026-09-13 | Keep the notice; the package carries it. |
| ggwave's FFT (src/fft.h, Takuya Ooura's FFT package, fft4g-derived `rdft`) | Author's terms: use, copy, modify and distribute for any purpose including commercial use, without fee; refer to the package when modifying. The file itself carries only the copyright line and the package URL | VERIFIED 2026-09-13 on the author's page https://www.kurims.kyoto-u.ac.jp/~ooura/fft.html ("License" section, verbatim in docs/legal/third-party/ooura-fft.txt) | **Resolved** for use and redistribution. Keep the header (copyright and package reference) intact; ship the notice with pos-wave (see "Audio milestone", item B). |
| alsa-lib 1.2.13 (Buildroot package, dynamically linked by pos-wave) | LGPL-2.1-or-later | DOCUMENTED (docs/legal/manifest.csv) | Already in the image for alsa-utils; dynamic linking. |
| libgpiod 2.2 (Buildroot package, dynamically linked by radiod) | LGPL-2.1-or-later | DOCUMENTED (header SPDX) | Dynamic linking keeps Doors code separate; offer library source. Header copy in vendor/libgpiod is for host compile checks only. |
| nofrendo (bundled) | GPL-2.0 upstream | DOCUMENTED | Not needed by Doors. |
| libtmt, qrcodegen (bundled, no LICENSE copies) | MIT upstream | DOCUMENTED | Fetch upstream with LICENSE if ever used. |
| quirc (bundled) | ISC (LICENSE present) | VERIFIED locally | Fine if used, keep notice. |
| LVGL 9 (pinned commit 59dc7e4, vendor/lvgl; on the device the vendor SDK package `lvgl` at the same commit, `liblvgl.so.9.5.0`) | MIT (LICENCE.txt present); bundled components carry their own licences (COPYRIGHTS.md and the LICENSE files beside them) | VERIFIED locally 2026-09-13, against the SDK's source archive | The vendor package declares no licence (legal-info: "unknown"), so THIRD_PARTY_NOTICES.txt covers LVGL, lv_port_linux and every bundled component the vendor configuration compiles in: LodePNG, TJpgDec, ThorVG, and the Montserrat, Font Awesome 5, DejaVu Sans, Source Han Sans SC and unscii-8 fonts (see "Third-party notices"). |
| SDL2 (simulator only, not shipped) | zlib | DOCUMENTED | Host-only. |
| IBM Plex Sans / Mono (converted to LVGL bitmaps in `ui/pocketui/fonts/`) | OFL-1.1 with Reserved Font Name "Plex" | VERIFIED (upstream `license.txt`, copy in `docs/legal/fonts/`) | Bitmaps are Modified Versions: symbols are `pos_font_*`, and the UI must never present them as "IBM Plex" (DS decision 2026-09-04). The OFL text and copyright notice ship in the image, in THIRD_PARTY_NOTICES.txt. The boot splash (`platforms/k230/rootfs_overlay/logo.xrgb`) also carries the word "Doors" as Plex Sans outlines in a picture: no font ships for it and nothing presents a font called Plex, so it adds no notice (docs/design/brand/README.md; owner to confirm the wording). |
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
| B. Notices in a distributed image | Was: the `pocketos` package declared no licence files, so neither legal-info nor the image carried these notices. **Fixed** by the third-party notices work (next section) | **(a) resolved** |
| PocketOS's own licence | Not decided (open item 1) | **(c) unresolved**; a release blocker independent of audio |

## Third-party notices (2026-09-13)

**What ships.** `THIRD_PARTY_NOTICES.txt` at the repository root, installed in
the image as `/usr/share/doors/THIRD_PARTY_NOTICES.txt` (mode 0644; not
under share/doc, which Buildroot strips) with a symlink to it at
`/usr/share/pocketos/THIRD_PARTY_NOTICES.txt`, the path it had through
v0.0.9 (ADR-005 Phase 2), collected by `make legal-info` as the `pocketos`
package's licence file, and sent by the bench `deploy.sh`, link included. It
opens with Doors' own undecided status, then lists and reproduces in full:

| Material | Reaches the image as | Licence |
| --- | --- | --- |
| RadioLib 7.7.1 (034126e) | compiled into radiod | MIT |
| ggwave v0.4.3 (a38e38b) | compiled into pos-wave | MIT |
| Reed-Solomon (in ggwave) | compiled into pos-wave | MIT |
| Ooura FFT (in ggwave) | compiled into pos-wave | author's terms |
| MeshCore (3ca7e3f, from the RIFT tree) | compiled into meshcored | MIT |
| Ed25519, Orson Peters (in MeshCore) | compiled into meshcored | Zlib |
| Arduino Cryptography Library, rweather (37a76b8) | compiled into meshcored | MIT |
| IBM Plex Sans / Mono | bitmap fonts compiled into pocketos-shell | OFL-1.1, RFN "Plex" |
| LVGL 59dc7e4 | liblvgl, loaded by pocketos-shell | MIT |
| lv_port_linux b492d73 | liblvgl_linux | MIT |
| LodePNG, TJpgDec, ThorVG (in LVGL) | compiled into liblvgl / liblvgl_thorvg | Zlib; TJpgDec licence; MIT |
| Montserrat, Font Awesome 5, DejaVu Sans, Source Han Sans SC, unscii-8 (LVGL built-in fonts) | compiled into liblvgl; Montserrat and its Font Awesome glyphs used by the shell | OFL-1.1; OFL-1.1 for the glyphs; Bitstream Vera / Arev; OFL-1.1; public domain |

**How it stays true.** `third_party/notices/SOURCES` lists each entry and where
its text comes from; `tools/legal/gen_notices.sh` generates the file.
- Copied texts are kept as verbatim copies and checked byte for byte against
  the pinned upstream: RadioLib and ggwave git objects at their pins, and the
  LVGL and lv_port_linux source archives the SDK builds from.
- `platforms/k230/package/pocketos/pocketos.hash` is the package's Buildroot
  hash file: the sha256 of THIRD_PARTY_NOTICES.txt, which legal-info checks the
  collected file against (a mismatch fails legal-info). The generator writes
  both files together. Buildroot reads the hash only during legal-info and
  accepts a hash file with no line for a licence file, so the steps below also
  check it.
- `apply_to_sdk.sh` refuses to package if the notices are not current, if
  pocketos.hash does not match them, if any text differs from or cannot be
  read from its upstream, or if the vendor LVGL configuration compiles in
  bundled code with no entry. There is no override.
- `build_image.sh` checks after the build that the image carries the packaged
  file, that it matches pocketos.hash (in both the applied copy and the one
  Buildroot reads), and that the built LVGL configuration still matches.
- `tests/notices_test.sh` (make test) fails if the generated file is stale or
  its hash does not match, if a vendored tree the Makefile compiles, an
  embedded font or a package dependency has no entry, if the file stops being
  installed, collected, hashed or deployed, or if a licence for Doors
  itself appears. It also proves the hash check refuses a hand-edited file,
  regenerated notices without their new hash, a wrong hash and a missing hash
  file.

**Classification.**

| Item | Class |
| --- | --- |
| Notices for third-party code compiled into Doors binaries, and for LVGL and its bundled components | **(a) resolved** - shipped and verified |
| Libraries Doors and LVGL load from Buildroot packages with licence metadata (cJSON, libgpiod2, alsa-lib, libdrm, libevdev, FreeType, FFmpeg) | **(a) covered by `make legal-info`**, provided its output accompanies a distributed image (open item 3) |
| C and C++ runtime libraries from the external Xuantie toolchain (glibc, libstdc++, libgcc) | **(b) blocks distribution**: not in legal-info's manifest (open item 8) |
| Other vendor SDK packages without licence metadata: libnncase and gsl-lite ("unknown" in the manifest), and the vendor local packages absent from it (`k230_phone_ui`, `vvcam`, `face_detect`, `ai_demo`) | **(b) blocks distribution**: unchanged (open item 5) |
| Doors' own licence (PocketOS through v0.0.9) | **(c) undecided**; external redistribution not authorised (open item 1) |

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

1. Decide the Doors licence (owner), and with it who the copyright lines name
   (they say "PocketOS authors"). Until then no licence is granted and
   external redistribution of Doors, source or binaries, is not authorised.
2. Ask LILYGO to add a LICENSE to the T-Display-K230 repository, or treat it as
   all-rights-reserved documentation.
3. Add `make legal-info` to the image build and archive the result per release
   (first run done 2026-09-04, see above).
4. Decide whether to drop `rtl8723ds`, `rtl8723ds-bt` and `aic8800` from the
   defconfig, k230_pocketos_defconfig (hardware absent; one proprietary blob less).
5. Add manual manifest entries for vendor local packages.
6. ~~Confirm the licence terms of Ooura's FFT (ggwave `src/fft.h`), which the
   file itself does not state, before a release ships pos-wave.~~ Resolved
   2026-09-13 from the author's page (see "Audio milestone"; verbatim terms in
   docs/legal/third-party/ooura-fft.txt).
7. ~~Release blocker: ship the third-party notices for code compiled into
   PocketOS binaries - RadioLib (radiod), ggwave and its Reed-Solomon code
   (pos-wave), and the Ooura FFT terms - with any distributed image.~~
   Resolved 2026-09-13: THIRD_PARTY_NOTICES.txt, installed, collected by
   legal-info and verified at packaging, also covering the IBM Plex fonts,
   LVGL and LVGL's bundled components (see "Third-party notices").
8. **Distribution blocker:** collect the licences of the C and C++ runtime
   libraries the external toolchain puts in the image (glibc, libstdc++,
   libgcc_s); they are not in legal-info's manifest.
9. ~~**Before meshcored ships in an image:** add notices entries for the three
   trees it compiles — MeshCore (`vendor/RIFT` `src/` and its
   `lib/ed25519`, orlp's ref10 Ed25519) and rweather's `arduinolibs` Crypto.~~
   Resolved 2026-09-22 (product owner's decision to ship it): third_party/notices
   carries `meshcore` (MIT, Scott Powell / rippleradios.com), `ed25519` (Zlib,
   Orson Peters) and `arduinolibs-crypto` (MIT, Southern Storm Software), each
   text a verbatim copy of the licence at the commit protocols/meshcore pins,
   checked byte for byte by `gen_notices.sh --verify-upstream` and so on every
   `apply_to_sdk.sh`. The image package now builds and installs meshcored
   (`pocketos.mk`, `ENABLE_MESHCORED=1`); `S65meshcored` still ships disabled,
   switched on per unit (docs/services/MESHCORED.md). The rule itself stays
   **enforced, not merely documented**: `make install` refuses meshcored while
   any of the three entries is absent, and `apply_to_sdk.sh` refuses the
   package before assembling anything; `tests/notices_test.sh` executes both
   directions - the gate passing with the notices as they are, and refusing
   with an entry it cannot find - because an earlier version only read the
   Makefile's default, and `ENABLE_MESHCORED=1 make install` shipped the binary
   while the test said it could not. There is still no override: this is a
   licensing rule rather than a build preference.
