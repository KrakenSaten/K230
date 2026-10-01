# Photo

The photo library on its own: the photos Camera took, as a grid of
thumbnails; one photo fitted to the screen with what is known about it;
NEWER and OLDER; DELETE after a confirmation; EXPORT to Files; a slideshow.

Status: **Photo 0.1, branch `feat/photo-app` (from master `749f4f1`), not
merged.** Host-tested end to end against the real library helper, in portrait
and landscape, under ASan/UBSan for the pure-C parts; cross-built for the K230
with the pinned Xuantie toolchain. **Unit B gate PASS on `0f3c411`,
2026-10-01** (`docs/hardware/PHOTO_GATE.md`: real Camera JPEGs in both
orientations, no camera ever opened by Photo, delete and a refused delete,
slideshow 4.0 s, rotation, 43 opens flat); unit B was restored afterwards.
Left for the owner's eyes: the icon and picture quality on a lit scene.
Layout and place: DS §44 (Amendment AB), PROPOSED.

## Camera takes, Photo shows

Camera stays the capture app. Photo is where the photos are looked at. There
is **one library and one gallery**:

- **One library.** Photo reads Camera's photo folder,
  `$POCKETOS_STATE_DIR/camera` (`/var/lib/pocketos/camera`), with Camera's
  names (`IMG_<yyyymmdd>_<hhmmss>_<nnnn>.jpg`, `IMG_<nnnn>.jpg`, `.ppm` on a
  host without libjpeg). No second store, no index, no thumbnail files: every
  photo Camera ever took is in Photo, and nothing Photo does changes the
  storage format.
- **One gallery.** Photo hosts Camera's own gallery - the model
  `apps/camera/camera_gallery.c` and its screen
  `apps/camera/camera_gallery_screen.c` - in its body, standalone. It adds no
  library, decoding or delete code of its own: `apps/photo/photo_app.c` is
  the app descriptor, a helper session and a timer (`tests/photo_lint.sh`
  holds that). Camera's PHOTOS still opens the same gallery inside Camera, so
  the two entry points cannot drift apart.

What "standalone" changes, and all it changes: there is no CAMERA button
(Camera's gallery has one on the grid, the empty library and the failure
screen, to go back to the live picture). The shell has no way for one app to
open another, so Photo's way out is the shell's back slab; to take a photo, go
home and open Camera.

## What it does (0.1)

- **Grid.** Thumbnails, newest first, a page at a time (12 in portrait on
  unit A's panel, 10 in landscape); NEWER and OLDER turn pages; the status
  line says how many photos and which page. A file that cannot be read shows
  "Cannot show". SLIDESHOW starts from the page's first photo.
- **One photo.** A tap shows it fitted to the screen (the whole photo, its
  shape kept), with three lines: its name; when it was taken; its size, file
  size, format, and "damaged" or "simulated" when that is so. NEWER and OLDER
  step through the whole library and stop at either end; BACK returns to the
  grid on that photo's page.
- **Delete.** DELETE asks "Delete this photo?" with CANCEL and DELETE; only
  the second DELETE removes the file. The next older photo takes its place and
  the grid has one fewer at once. A delete the filesystem refuses says "The
  photo could not be deleted" and keeps the photo; a photo already gone
  (deleted by hand meanwhile) is removed from the list.
- **Export.** EXPORT copies the photo into `~/Pictures` for Files, as in
  Camera (docs/apps/CAMERA.md, "Export").
- **Slideshow.** One photo after another every 4 s, wrapping round at the
  oldest; a photo that cannot be shown is skipped; a tap stops it.
- **States said in words.** "Opening photos", "No photos yet" with "Photos
  you take with Camera appear here.", "Photos unavailable" (the helper
  missing, crashed, not responding, or the folder unreadable) with TRY AGAIN.

### Order

Newest first by the sequence number in the name, which Camera makes one
higher than any photo in the folder - the order the photos were taken
whatever the clock did. Ties fall back to the name.

### Metadata

What Camera writes and the gallery reads back (docs/apps/CAMERA.md,
"Metadata"): the name and file size; width x height upright; when it was
taken (EXIF `DateTimeOriginal`, else the date in the name, else "Date
unknown: the clock was not set"); JPEG or PPM; "damaged" when the decoder
recovered only part of it; "simulated" for the fake camera's pictures. No
EXIF framework was added.

## What it is not

No editing, filters, cropping, rotation by hand, drawing, albums, favourites,
tags, search, sharing or upload, cloud sync, AI classification, face tagging,
RAW, video, or browsing other folders. Only Camera's library.

## Architecture

| Part | Where | Role |
| --- | --- | --- |
| `photo_app.c` | apps/photo | the app: a `camera_session`, Camera's gallery screen created standalone, a 33 ms timer that polls it, destroy |
| `camera_gallery.c` | apps/camera | the gallery's state machine (pure C), now with `standalone` |
| `camera_gallery_screen.c` | apps/camera | the gallery's LVGL screen, now with `gallery_ui_set_standalone()` |
| `camera_session.c` | apps/camera | runs `pos-camera library`: the process boundary, sealed shared memory, slot ownership, watchdog, death signal (ADR-006) |
| `camera_layout.c` | apps/camera | the gallery's shapes |
| `pocketcam_store.c`, `pocketcam_image.c`, `pocketcam_exif.c` | core/pocketcam | the folder, the bounded decoder, the metadata - in the helper only |
| `pos_camera.c` | tools/camera | `pos-camera library`, the only process that reads, decodes, exports or deletes a photo |

**No camera, ever.** Photo starts only the library helper, which never opens
a video device (`tests/photo_app_test.c` checks there is never a `session`
helper). So Photo does not reach the camera-stack lock-up of
docs/KNOWN_ISSUES.md, which repeated camera open/close triggers.

**Nothing on the LVGL thread touches a file.** Listing, decoding, exporting
and deleting are the helper's; the screen copies finished pictures out of the
shared memory in the tick that learns of them. The only wait on the LVGL
thread is leaving: the helper gets 300 ms (`GALLERY_LEAVE_GRACE_MS`) before
SIGKILL.

**Lifetime.** Opening Photo starts the helper; leaving ends it and frees
every picture; the helper also dies with the shell (PR_SET_PDEATHSIG).

## Memory and a large library

Bounded exactly as Camera's gallery (docs/apps/CAMERA.md, "Memory"):

- **Decoding:** one picture at a time per slot, never at full size - a JPEG
  is scaled in libjpeg's DCT and streamed a line at a time; memory is the
  destination and one line. A picture over 8192 px a side or 16 M pixels is
  refused before anything is allocated. At most three pictures are out at
  once.
- **Held by the screen:** one page of thumbnails (12 x 170 x 170 x 2 bytes,
  0.7 MB in portrait; 24 cells at most), the photo view's one picture (at
  most 2 MB), and only while the slideshow runs its two pictures. All of it
  is freed when Photo is closed. No thumbnail cache on disk, none across
  visits.
- **The list:** at most 1000 names (`CAMERA_LIBRARY_MAX`, 48 bytes each,
  twice: about 96 KB); with more, the grid shows the newest 1000 and says
  "Newest 1000 of N photos". Camera itself stops at 500 photos
  (`POCKETCAM_STORE_MAX_FILES`), so the cap is only reached by photos copied
  in by hand.

Measured on the host (x86-64, WSL2, `photo_app_test`, 2026-09-30): 1100
photos listed and the grid up in 110-300 ms; twenty opens and closes, each
with a page of thumbnails and a photo, left the shell's descriptors at 4 -> 4,
the heap 45-59 KB higher (LVGL's own allocations settling; the bound is
128 KB, well under one page of thumbnails at 0.7 MB) and no child; the
slowest close 41 ms, closing mid-slideshow 7-10 ms. None of this is a K230
number (docs/hardware/PHOTO_GATE.md).

## Storage safety

- **Only photos are listed:** a regular file whose name is a Camera name.
  Folders, links, dot files (a capture's `.IMG_....tmp`), names with spaces
  and anything else in the folder are never listed, shown or deleted.
- **Delete is confined to the folder:** the helper takes a name, not a path,
  and accepts only a Camera name (no `/`, no `..`); it removes only a regular
  file (`lstat`, then `unlink` - a link or folder under a photo's name is
  refused with EPERM; neither is ever listed); never recursively; the folder
  is fsync'd after. The app's own client never sends a name with a `/` or a
  space, and the helper refuses one anyway (`tests/photo_library_test.c`
  feeds it raw lines).
- **Damage and races:** an empty, cut short, garbled or foreign file is shown
  as what it is ("Cannot show", "The file is damaged and cannot be shown");
  a file deleted behind the gallery's back is "The file is gone"; a hung
  decode is the watchdog's (8 s); a crashed helper is the failure screen with
  TRY AGAIN. The shell is never affected.
- **Partially written captures:** Camera writes to `.IMG_....tmp` and renames
  (docs/apps/CAMERA.md, "Atomic"), so a half-written photo is never under a
  photo's name. Camera and Photo never run at once (one app at a time).

## Tests

Host only; none needs a K230.

- `tests/photo_library_test.c` (make; 58 checks): the gallery model
  standalone - no CAMERA on opening, the empty library, the grid, the failure
  screen, kept across TRY AGAIN; one photo as first and last at once; NEWER
  and OLDER at both ends; DELETE with its confirmation, a refused delete, a
  delete; the slideshow started and stopped; Camera's gallery still with
  CAMERA. The store: only regular photo files listed; `../`, absolute paths,
  a path through a photo's name, spaces, other names refused; a link and a
  folder under a photo's name refused with both they and what they point at
  or hold intact; a missing photo; a read-only folder; 1100 photos capped at
  the newest 1000 with 1100 counted. The helper itself, fed raw `delete`
  lines past the client: the same refusals, a delete, and a second delete of
  the same photo.
- `tests/photo_app_test.c` (SDL build, 183 checks): Photo hosted like the shell hosts it,
  in portrait and landscape, against the real library helper - the empty
  library, one photo, many (newest first by number, not by date), every
  target usable and clear of the corners, NEWER/OLDER to both ends, the three
  lines about a photo, tall and wide photos keeping their shape; damaged,
  empty, cut short, text-under-a-photo's-name and vanished files; DELETE with
  CANCEL, a delete, a refused delete, deleting the last photo; the slideshow;
  a missing helper and TRY AGAIN; 1100 photos; closing on a photo and in a
  slideshow; twenty opens and closes with no helper, descriptor or heap left.
- `tests/photo_shell_test.sh`: the above, then the real shell opening Photo in
  both orientations on a library of seven photos, the thumbnails on screen,
  no warnings, no helper left, the library byte-identical afterwards.
- `tests/photo_lint.sh` (make test): the boundaries above, statically.
- `make photo-test`, `make photo-san-test` (Photo's suite and Camera's
  library suites under ASan and UBSan, the helper built the same way).
- Camera's suites all run unchanged (`camera_*`, `pocketcam_*`,
  `camera_shell_test.sh`).

## Left for later

- **Camera's PHOTOS -> Photo.** Camera's gallery entry could open Photo once
  the shell has a way for one app to open another; until then both host the
  one gallery, so nothing diverges. Moving `camera_gallery*.c` and the
  gallery half of `camera_layout.c` out of `apps/camera` (to a shared
  `apps/gallery` or `ui/`) is a rename with no behaviour change, left out to
  keep Camera untouched.
- **Enlarging small pictures.** "Contain" fills the photo box, so a picture
  smaller than the screen is enlarged (nearest neighbour). Camera's own
  photos (1080 x 1920) are always larger than the box and are only ever
  reduced; this matters only for small files copied in by hand.
- **Keyboard.** As Camera's gallery: touch only for now.
