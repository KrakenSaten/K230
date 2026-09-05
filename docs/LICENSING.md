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
| libgpiod 2.2 (Buildroot package, dynamically linked by radiod) | LGPL-2.1-or-later | DOCUMENTED (header SPDX) | Dynamic linking keeps PocketOS code separate; offer library source. Header copy in vendor/libgpiod is for host compile checks only. |
| nofrendo (bundled) | GPL-2.0 upstream | DOCUMENTED | Not needed by PocketOS. |
| libtmt, qrcodegen (bundled, no LICENSE copies) | MIT upstream | DOCUMENTED | Fetch upstream with LICENSE if ever used. |
| quirc (bundled) | ISC (LICENSE present) | VERIFIED locally | Fine if used, keep notice. |
| LVGL 9 (pinned commit 59dc7e4, vendor/lvgl) | MIT (LICENCE.txt present) | VERIFIED locally | Fine, keep notice. Bundled lodepng (zlib licence) used for screenshots. |
| SDL2 (simulator only, not shipped) | zlib | DOCUMENTED | Host-only. |
| IBM Plex Sans / Mono (converted to LVGL bitmaps in `ui/pocketui/fonts/`) | OFL-1.1 with Reserved Font Name "Plex" | VERIFIED (upstream `license.txt`, copy in `docs/legal/fonts/`) | Bitmaps are Modified Versions: symbols are `pos_font_*`, and the UI must never present them as "IBM Plex" (DS decision 2026-09-04). OFL text ships with the fonts. |
| lv_font_conv 1.5.3 (host tool) | MIT | DOCUMENTED | Host-only, run from a local Node 20 tarball; generated C files are committed so builds need neither. |
| cJSON 1.7.x (Buildroot package, used by pocketipc, pos, radiod) | MIT | DOCUMENTED | First PocketOS dependency: ~40 kB library, no transitive deps, justified in docs/api/pocketipc.md. |

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
