# Local vendor packages that legal-info does not list

Buildroot's `make legal-info` writes a manifest row only for a package that
has a source archive (`<PKG>_SOURCE`; Buildroot 2025.02.1
`package/pkg-generic.mk`). The K230 SDK's local packages
(`SITE_METHOD = local`) have none, so they never appear in
`manifest.csv`, even in a fresh run, and neither does the preinstalled
external toolchain. These are the manual entries for the 0.3.5 image,
researched on 2026-10-07 from the build tree (SDK `22d02c6`, BSP `bb831ab`)
and the files themselves. Evidence classes as in AGENTS.md. Their source is
the SDK repository at `22d02c6` (BSP overlay at `bb831ab` where noted); the
licence decisions they need are in docs/licensing/APACHE_2_READINESS.md
§14.2.

| Package | What it installs | Licence, by evidence | Class |
| --- | --- | --- | --- |
| `libmmz` | nothing in the target (static; linked into `pos-vision`) | no file header; the SDK's root LICENSE, BSD-2-Clause, by location [ASSUMED coverage]; notices entry `canaan-k230-sdk` | resolvable |
| `display` | `/usr/lib/libdisplay.so` (used by vendor tools, not by Doors) | no headers; SDK BSD-2-Clause by location [ASSUMED coverage] | resolvable |
| `face_detect` | `/root/app/face_detect/`: `face_detect.elf`, `face_detection_320.kmodel`, `test.jpg`, scripts | sources: Canaan BSD-2-Clause headers [VERIFIED]; **the model and the photo: no terms** | code resolvable; model and photo need Canaan or removal |
| `ai2d_kpu` | `/root/app/ai2d_kpu/`: `ai2d_kpu.elf`, `test.kmodel`, test vectors | `main.cpp` no header; bundled `gsl-lite.hpp` MIT [VERIFIED]; **the model and vectors: no terms** | needs Canaan or removal |
| `nonai2d` | `nonai2d.ko` | `SPDX-License-Identifier: GPL-2.0-only`, `MODULE_LICENSE("GPL")` [VERIFIED] | GPL-2.0 source offer (source in the SDK) |
| `vvcam` | kernel modules `vvcam_*.ko`; `/usr/bin/isp_media_server`; `/etc/vvcam/*`; sensor libraries | kernel side VeriSilicon MIT and Vivante MIT/GPL-2.0-or-later, `MODULE_LICENSE("GPL")` [VERIFIED]; user-side sources without headers (SDK BSD-2-Clause by location, ASSUMED); **`isp_media_server` is a prebuilt binary with no source and no terms** | modules: GPL source offer; `isp_media_server`: needs Canaan |
| `aic8800` | 70 firmware files under `/lib/firmware/aic8800*`; `aic8800_fdrv.ko`, `aic_load_fw.ko`, `aic_btusb.ko` | drivers GPL (RivieraWaves, AICSemi; module licence GPL) [VERIFIED]; **firmware: no terms** | hardware absent: owner decision to drop (B8) |
| `rtl8723ds`, `rtl8723ds-bt` (BSP overlay packages) | `8723ds.ko`; BT firmware under `/lib/firmware/rtlbt`, `rtl_bt`; `/usr/sbin/rtk_hciattach` | driver GPL-2.0 (`LICENSE_FILES = COPYING`); firmware `PROPRIETARY` in Buildroot, upstream `wsyco/RTL8723DS_BT_Linux` states no licence; `rtk_hciattach` is BlueZ-derived GPL-2.0-or-later [VERIFIED] | hardware absent: owner decision to drop (B8) |
| `vg_lite` | `libvg_lite.so`, `libvg_lite_util.so`, 5 demo binaries | test directory carries a Vivante MIT-style notice [VERIFIED]; library headers not checked | not established |
| `mvx_player`, `camera_rtsp_demo`, `librtsp_server`, `audio_demo`, `audio_rec_play` | vendor demo programs and libraries (`/usr/bin/mvx_*`, `/root/app/camera_rtsp_demo`, `/usr/lib/librtsp_server.so`, `/usr/bin/audio_demo`, `/usr/bin/audio_rec_play`, `/usr/bin/audio.pcm`) | not researched beyond "no licence metadata"; none is linked by a Doors binary [VERIFIED, `readelf -d`] | not established |
| external toolchain (Xuantie-900 V3.0.2, B-20250410) | 41 runtime files: glibc 2.33, GCC 14.1.1 runtime libraries, `ldd` | LGPL-2.1-or-later, GPL-2.0-or-later, GPL-3.0-or-later WITH GCC-exception-3.1; texts in THIRD_PARTY_NOTICES.txt [VERIFIED from the fork's sources] | texts resolved; source offer: owner decision (B9) |

Two packages that do have a row carry no licence in it: `libnncase`
("unknown"; generic runtime Apache-2.0, K230 modules not stated, B5) and
`gsl-lite` ("unknown"; MIT, its archive's LICENSE). `lvgl` also says
"unknown" (the vendor `lvgl.mk` sets no licence); LVGL is MIT and its
notices ship.
