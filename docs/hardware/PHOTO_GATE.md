# Photo 0.1 hardware gate

**Unit B is back on what it carried before the gate** (restored and verified
2026-10-01 05:06 UTC): userspace master `749f4f1` (`/etc/doors-release`
`BUILD_ID=749f4f1`), `/usr/bin/doors-shell` `0cc4b66` (hardware controls,
md5 `4cef0289…`), `/usr/bin/pos-camera` from `749f4f1` (md5 `3dd5db34…`),
`settings.conf` byte-identical, no `icon-photo.bin`, the photo library empty
as found, rotation `automatic`. **Photo is not on the unit now.** Rollback
kit and the gate's photos: `/root/rollback-photo/` (`RESTORE.sh` already
run; `Photos-gate/` keeps the five gate photos, 5.6 MB). To put Photo back:
`C:\K230\out\photo-gate\g2_deploy.sh` (stage from `g1_build.sh 0f3c411`).

**The gate below ran on `feat/photo-app` `0f3c411`**, hot-swapped onto
unit B: `doors-shell` `0f3c411` (md5 `64cb4889…`), `pos-camera` `0f3c411`
built with libjpeg (md5 `ebd4179f…`), `icon-photo.bin` (md5 `292bc3a4…`);
`doors shell info` build `0f3c411`, 26 apps. Run 2026-10-01 04:36-05:06 UTC
by Claude over SSH (Wi-Fi, 192.168.10.187), no flash. Touch by injected
taps on the panel's touch device (`touch_slot0_tap.py`, `rift_tap.py
swipe`), pictures by kmsgrab. Tooling, logs and captures:
`C:\K230\out\photo-gate` (`g*`, `p*`, `c*` scripts; `logs/`, `caps/`).

## Results: PASS (all sixteen steps), with two notes

| # | Result |
| --- | --- |
| P1 | **PASS.** Landscape: Photo in DEVICE after Video (Settings, System, Terminal, Vision, MP3, Video, Photo), the Gallery icon in the files colour on its portal (`caps/p1-launcher-landscape.png`). Portrait: the launcher scrolls (accepted since Recorder); after a swipe Photo is DEVICE's second row after MP3 and Video (`p1-launcher-portrait-scrolled.png`). Opened by a real tap on its cell in both orientations. The icon's look is for the owner's eyes |
| P2 | **PASS.** Grid newest first, "4 photos \| page 1 of 1" (landscape), "5 photos" (portrait, the two portrait photos first), the same order and count as Camera's PHOTOS; no CAMERA (portrait: the bottom-left place is empty, SLIDESHOW beside it) |
| P3 | **PASS.** Only `pos-camera library`, 0 video fds, no `session` helper at any time Photo was open (sampler: helper mode `library` only). The only video-node holder is pid 148, the vendor `isp_media_serve`, which always holds them |
| P4 | **PASS.** Real 1920 x 1080 / 1080 x 1920 JPEGs (930-937 KB / 519-534 KB, a dark room, so noisy): 4 thumbnails decoded within 297 ms of the helper starting (landscape), 5 within 323 ms (portrait) - about 65-75 ms each, as unit A measured for Camera's gallery |
| P5 | **PASS** (host-verifiable part). Portrait and landscape photos upright, filling the box with their shape kept, in both orientations (a wide photo letterboxed in portrait, a tall one pillarboxed). Quality on a lit scene is for the owner's eyes: the room was dark |
| P6 | **PASS.** e.g. `IMG_20261001_044121_0004.jpg` / `Taken 2026-10-01 04:41:21` / `1920 x 1080 \| 930 KB \| JPEG` (952,710 B); `IMG_20261001_050039_0006.jpg` / `1080 x 1920 \| 519 KB \| JPEG` (531,731 B). **Note:** the undated case ("Date unknown: the clock was not set") was not exercised: the unit's clock is set and was not changed; the host suites cover it |
| P7 | **PASS.** OLDER to the oldest ("4 of 4" / "5 of 5"), OLDER then off and a further tap changes nothing; NEWER back; NEWER off at "1 of N" |
| P8 | **PASS.** DELETE -> "Delete this photo?" (warn colour), CANCEL and DELETE, NEWER/OLDER off; CANCEL kept all four; DELETE, DELETE removed exactly `IMG_20261001_044058_0002.jpg` (md5 diff: that file only, nothing else changed); "3 of 3" on the next older |
| P9 | **PASS.** "The photo could not be deleted" (warn colour), the photo kept and still shown, 3 -> 3 files. **Note:** the sheet's `chmod 555` cannot refuse a delete on the unit - the shell runs as root, which ignores folder modes - so the shown photo was made immutable instead (`chattr +i`, BusyBox), which refuses `unlink` even for root; cleared afterwards |
| P10 | **PASS.** Slideshow full-body, "Slideshow \| 2 of 3 \| tap to stop", wrapping 0004 -> 0003 -> 0001 -> 0004; intervals 3997-4039 ms over 7 swaps; a tap on the picture stops it, back on the grid |
| P11 | **PASS.** Rotation stored with a photo open (landscape -> portrait), a running slideshow (portrait -> automatic = landscape with the keyboard base) and the grid (both ways): every time the shell applied it in place (same pid), came home, no `pos-camera`, no zombie. Rotation mode left `automatic`, as found |
| P12 | **PASS.** Back slab from the grid, a photo and a running slideshow: home at once, no helper, shell fds 11, no zombie |
| P13 | **PASS.** 43 opens with a photo each (23 + 20): shell fds 11 -> 11, no zombie, no helper; RSS 15,708 -> 16,348 kB over the first 20, then **flat at 16,348 kB for 20 more** (the allocator settling, not a leak) |
| P14 | **PASS.** Camera after Photo: two portrait photos taken and kept; Camera's own PHOTOS still has CAMERA and shows them first; CAMERA brings the live preview back with `pos-camera session`; leaving leaves nothing. Three camera opens in the whole gate (the known open/close lock-up was not approached) |
| P15 | **PASS.** EXPORT: "Saved to Files: /root/Pictures/IMG_20261001_044015_0001.jpg", byte-identical (`cmp`) with the photo's time; a second EXPORT made no second copy (its "Already in Files" note had gone before the capture). `/root/Pictures` did not exist before; removed after |
| P16 | **PASS.** At the end the three landscape photos were byte-identical to their state after P8 and the export copy still identical; only the deliberately deleted photo was gone. No ERROR or WARN in the shell log for the gate's period (47 Photo opens), no crash file, no new kernel message |

Not shown on the unit, for the owner: the icon's look (P1) and picture
quality on a lit scene (P5).

Deploy: the smallest set is `/usr/bin/doors-shell` (Photo is in the shell)
and `/usr/bin/pos-camera` (its delete now refuses anything but a regular
file), plus `ui/assets/doors/icon-photo.bin` into the installed art folder.
Keep a rollback of all three. Photo needs no service, no init script and no
new package: libjpeg is already in the image (Camera uses it).

## Before

- Note what `/var/lib/pocketos/camera` holds (names, sizes, `md5sum *`), so
  the library can be shown unchanged after steps that must not change it.
- Take a few photos with Camera first if the library is empty; include at
  least one portrait and one landscape photo, and one dated and one undated
  photo (an unset clock, or photos kept from before NTP).

## Steps

| # | What | Pass |
| --- | --- | --- |
| P1 | The launcher shows Photo in DEVICE after Video, with the Gallery icon in the files colour, in both orientations | the owner's eyes (DS §44.1) |
| P2 | Open Photo: the grid of Camera's photos, newest first, the same as Camera's PHOTOS shows | same order, same count |
| P3 | `ps`: one `pos-camera library`, **no** `pos-camera session`; no `/dev/video*` open by any process (`ls -l /proc/*/fd`) | the camera is never opened |
| P4 | Thumbnails of real 1080 x 1920 JPEGs: a page in about the time Camera's gallery took (0.75-1.1 s on unit A, CAMERA_GALLERY_GATE.md) | comparable |
| P5 | Open a portrait and a landscape photo: fills the photo box, shape kept, upright, no visible blockiness | the owner's eyes |
| P6 | The three lines: name, "Taken ..." (or "Date unknown: the clock was not set" for an undated one), "1080 x 1920  \|  <size>  \|  JPEG" | as the file |
| P7 | NEWER / OLDER through the library; both stop at the ends | |
| P8 | DELETE -> CANCEL keeps the file; DELETE -> DELETE removes exactly that file (`ls`), the next older is shown, the grid one fewer | only that file |
| P9 | A refused delete: `chattr +i` the shown photo (root ignores folder modes, so `chmod` cannot refuse it), DELETE twice -> "The photo could not be deleted", the file stays; `chattr -i` after | |
| P10 | Slideshow: 4 s a photo (time five), a tap stops it | 3.9-4.1 s |
| P11 | Rotate during the grid, a photo and a slideshow: the shell restarts and Photo comes back to the grid, no helper left behind (shell policy: rotation returns to home, as for every app) | no stray helper |
| P12 | Close from the grid, a photo and a running slideshow: back to the launcher at once; no `pos-camera` left | |
| P13 | Twenty opens and closes of Photo with a photo opened each time: shell RSS flat (`/proc/<pid>/status` VmRSS before and after), fd count of the shell unchanged (`ls /proc/<pid>/fd \| wc -l`), no zombies | flat |
| P14 | Camera still works after Photo: open Camera, take a photo, KEEP; PHOTOS in Camera still has CAMERA and goes back to the preview; the new photo appears first in Photo | |
| P15 | EXPORT from Photo: the copy in Files under Home > Pictures | as Camera's |
| P16 | The library afterwards: every file not deliberately deleted is byte-identical to "Before" | md5 |

## Not needed on the unit

Already covered on the host and not hardware-dependent: damaged, empty,
cut short, text and vanished files; links, folders, spaces and other names in
the folder; a delete through `../`, an absolute path or a link (the helper
fed raw lines); 1100 photos; a missing helper and TRY AGAIN
(`tests/photo_library_test.c`, `tests/photo_app_test.c`).

## Known risk

The camera-stack lock-up (docs/KNOWN_ISSUES.md: repeated camera open/close
freezes the unit) is not reachable from Photo, which never opens the camera.
Step P14 opens Camera once; do not loop it.
