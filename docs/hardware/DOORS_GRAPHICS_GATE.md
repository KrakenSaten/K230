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
