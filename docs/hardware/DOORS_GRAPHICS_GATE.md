# Doors graphics: pending hardware gate

Branch `rebrand/doors-graphics`. Everything that can be checked without a
panel has been (docs/design/brand/README.md). What remains is one short look
at unit A. Until it passes, the splash byte order and orientation stay
DOCUMENTED, and DS Amendment C (§19) stays **PROPOSED**.

Not flashed, not merged. No change to VERSION, internal names or Phase 2.

## Before the bench

1. Build an image from the branch tip (`apply_to_sdk.sh`, `build_image.sh`);
   the build runs `verify_image.sh`.
2. `platforms/k230/scripts/verify_splash.sh <sysimage-sdcard.img>` must say
   `SPLASH: PASS` with SHA-256 `434f4a6c…8f94`.
3. Put the vendor splash back in the SDK overlay afterwards
   (docs/design/brand/README.md, "How it reaches the board").
4. Flash with the bench tool. Keep a serial console open for the U-Boot log.

## Checklist

What "right" looks like, so each item is a yes or no. Record every result as
VERIFIED (operator) or FAIL, with a photo where it helps.

| # | Check | Pass | Tells you, if it fails |
| --- | --- | --- | --- |
| 1 | Splash visible | U-Boot log has `RM69A10 direct XRGB8888 logo.xrgb full-screen OSD4` and no `size mismatch`; the panel shows the Doors splash, not the LILYGO one and not black | size or copy problem (`verify_splash.sh` should have caught it) |
| 2 | Portrait orientation | the mark is above the word "Doors", both upright | rows are stored bottom-up, or the layer is rotated |
| 3 | RGB channel order | the mark is light blue (`#8ccfff`), the word near-white, the background black | light orange (`#ffcf8c`) means R and B are swapped: fix the converter's byte order, not the art |
| 4 | No mirroring, clipping or stretching | "Doors" reads left to right; the mark's gap is at its lower **left**; the group is centred with even black margins, nothing cut off | pixels are stored right-to-left, or the layer size/offset is wrong |
| 5 | Splash-to-shell handoff | the splash holds until the Doors shell draws; no white, garbage or torn frame in between; `dmesg` shows `preserving boot splash` | panel/DRM handoff issue, unrelated to the file itself |
| 6 | System mark appearance | System app, scroll to the identity panel: the mark sits before "Doors" in the first row, crisp, same height band as the text, not blurred or blocky | A8 drawing or layout on the device |
| 7 | Mark follows the theme | Settings → Theme: the mark changes with each of the five themes, matching the back chevron's colour | the tint role is not repainting |
| 8 | Outdoor and Night | Outdoor: mark and name clearly legible. Night: dim like the rest of the screen, still recognisable | DS §13 levels on the AMOLED |
| 9 | Short app regression | launcher; open and close Radio, Fleet, Notes, Settings, System; one reboot; no crash report (`pos logs --crashes`), no ERROR in `shell.log` | the change touched something else |

Pass: all nine. Then Amendment C can be put to the owner for acceptance, and
the byte-order and orientation rows in docs/design/brand/README.md move to
VERIFIED.

## Validation done without hardware (2026-09-15)

Source `e999ab1`, from fresh WSL clones. The commit that adds this section
changes nothing but this sheet.

| Check | Result |
| --- | --- |
| `make all`, `make test` (host, -Werror) | rc 0; 3,491 ok, 0 FAIL, 0 warnings (master 8070379: 3,401) |
| New gates inside it | `boot_splash_test` (conversion, determinism, sizes, decode round trip), `brand_mark_test`, `splash_image_test`: 0 failures, none NOT RUN |
| Shell (SDL) build and shell tests | 0 warnings; 350 ok, 0 FAIL (master: 317); `system_brand_shell_test` 33 ok across 5 themes × 3 modes, live switch, reduced motion |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0, 4 warnings, all vendor ggwave, as on master |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings, `pos_brand_mark` linked |
| Image build | rc 0; `verify_image.sh` IMAGE GATE: PASS |
| `verify_splash.sh` | SPLASH: PASS, `/logo.xrgb` 2,799,104 bytes, SHA-256 `434f4a6cf697544764ccbd111d4b3a60eaf5ced5f86e41be731f1f7ce4f58f94` |
| Boot-partition splash decoded as B, G, R rows from the top | equals the source PNG's pixels; content x 184–383, y 517–715 |
| Shared SDK overlay afterwards | vendor splash restored, SHA-256 `9fd79fee…` |

Candidate image, not flashed and not in the repository:
`~/work/doors-gfx-image-e999ab1/` in the WSL build host,
`sysimage-sdcard.img` 763,363,328 bytes, SHA-256
`25f2f273a8f74ea2c2a1c59f4a6dbb0df00b5f1c3887103940a1b3996836359c`
(`.gz` 204,497,237 bytes, `f1ed26169244cc9b7a95c2cd5a27cd9e47cc5beef17926a2f9d3a8063a3f017a`).
BUILD_INFO says PocketOS 0.0.9 / `e999ab1`: VERSION is unchanged on purpose.
Flash it as is, or build again from the branch tip and repeat "Before the
bench".

Still unverified, and only unit A can settle them: all nine items above. The
host and simulator runs cover item 9's code, not the device.
