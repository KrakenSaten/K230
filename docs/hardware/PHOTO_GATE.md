# Photo 0.1 hardware gate

**Not run.** Photo 0.1 (branch `feat/photo-app`) was built and tested on the
host only: the other session held both units on 2026-09-30, and this branch
was asked to touch neither. Everything below needs a K230 and is what the
host could not show. State the unit's build at the top when it is run
(which doors-shell, which pos-camera, rollback path).

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
| P9 | A refused delete: `chmod 555 /var/lib/pocketos/camera`, DELETE twice -> "The photo could not be deleted", the file stays; `chmod 755` after | |
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
