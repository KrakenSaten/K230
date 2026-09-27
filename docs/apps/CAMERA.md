# Camera

A live picture, a shutter, and a look at the photo just taken: keep it, or
delete it. Then a small gallery of every photo in the library: a grid, one
photo with what is known about it, a slideshow, delete, and export to Files.

Gallery status: **branch `feat/camera-gallery` (on master `dd3809b`), not
merged. Unit A gate NOT PASSED: unit A hung (console and network dead) during
repeated camera-to-gallery cycling, and hung again, silently, after 40
camera-only open/close cycles with no gallery involved: a camera-stack
lock-up the gallery makes easier to reach, not a gallery defect**
(`docs/hardware/CAMERA_GALLERY_GATE.md`). Everything the gate reached before
that passed on unit A with the real GC2093 and libjpeg 9f. Host-tested end to
end on the fake backend, with and without libjpeg (IJG 9f, the image's own
version, under ASan/UBSan too); built with the pinned Xuantie toolchain.

Status: **working on unit A, on branch `feat/camera-app-design` (rebased on
v0.0.12), not merged.** The real V4L2 backend is written and gated: live
preview and JPEG stills on unit A in portrait and landscape, keep and delete,
the keyboard base alongside (`docs/hardware/CAMERA_GATE.md`, PASS on a
hand-installed build `e3d3f71`). Host-tested end to end on the fake backend.
Architecture ADR-006 and layout DS §34 (Amendment R) ACCEPTED by the owner
2026-09-25. The
hardware findings are `docs/hardware/CAMERA_PLATFORM_RESEARCH.md` (§10 for the
unit A measurements).

## What it does (v1)

- **Preview.** Opening the app starts the camera; the live picture fills the
  body's width in portrait (9:16, 528 x 938) and its height in landscape
  (16:9, 802 x 452), the shape of the photo it will become.
- **Take a photo.** TAKE PHOTO under the picture (portrait) or beside it
  (landscape). The still is taken at the sensor's full size (1920 x 1080 on
  the GC2093), turned upright for the way the unit is held, and saved.
- **Review.** Right after a capture the photo is shown with DELETE and KEEP.
  KEEP goes back to the preview. DELETE asks "Delete this photo?" with CANCEL
  and DELETE; only the second DELETE removes the file.
- **The last photo.** After a Keep, a thumbnail left of the shutter (above it
  in landscape) opens the last photo of this visit in review again.
- **Storage.** Photos are written to `/var/lib/pocketos/camera/`
  (`$POCKETOS_STATE_DIR/camera`), atomically, within limits (below).
- **States said in words.** "Starting the camera", "Waiting for the picture",
  "Waiting for the camera..." (a stalled stream: the shutter waits),
  "Taking the photo..." / "Saving the photo...", "No camera" with CHECK AGAIN,
  an error panel with TRY AGAIN, and short notes such as "Storage is full:
  the photo was not taken" and "Photo deleted".
- **Simulated pictures are labelled.** Under the fake backend the header
  says SIMULATED.

## What it is not

No video, filters, QR scanning, AI, sharing over the network or upload. No
resolution selector, exposure control or camera switch: the board has one
sensor with one mode, and which ISP controls work through V4L2 is not known
yet. The gallery is deliberately small: no albums, tags, search, editing,
zoom, rotation by hand or multi-select.

## States

| State | Shows | Taps |
| --- | --- | --- |
| `CAMERA_INIT` | panel "Starting the camera", shutter disabled | - |
| `CAMERA_PREVIEW` | the live picture (or "Waiting for the picture" until the first one); the last photo if there is one | TAKE PHOTO (once a picture has arrived and the stream is not stalled), the last photo |
| `CAMERA_CAPTURING` | the last preview picture, "Taking the photo..." then "Saving the photo...", shutter disabled | - |
| `CAMERA_REVIEW` | the photo, its name, DELETE and KEEP; while confirming, "Delete this photo?", CANCEL and DELETE | KEEP, DELETE, CANCEL |
| `CAMERA_ERROR` | a panel that says what went wrong: busy, the helper missing, not responding, crashed, lost | TRY AGAIN |
| `CAMERA_NO_DEVICE` | "No camera" and why (none found, or not built in) | CHECK AGAIN |

PHOTOS (right of the shutter in portrait, under it in landscape) opens the
gallery from `CAMERA_INIT`, `CAMERA_PREVIEW`, `CAMERA_ERROR` and
`CAMERA_NO_DEVICE` - so photos can be seen with no camera at all - and is
hidden while a photo is being taken or reviewed.

The back slab in the shell's header leaves from any state; leaving always
closes the camera. A rotation restarts the shell, which closes the camera the
same way and opens it again in the new orientation.

## Architecture

The app never opens the camera. A helper process owns it for as long as the
screen is open (ADR-006, accepted 2026-09-25):

| Part | Where | Role |
| --- | --- | --- |
| `camera_app.c` | apps/camera | the screen (LVGL); polls the session every 33 ms; copies pictures into its own buffers |
| `camera_state.c` | apps/camera | the state machine; pure C, no LVGL, no processes |
| `camera_layout.c` | apps/camera | the two shapes; pure arithmetic |
| `camera_session.c` | apps/camera | starts `pos-camera`, the shared memory, the line protocol, the watchdog; pure C |
| `pocketcam.c` | core/pocketcam | the backend seam |
| `pocketcam_v4l2.c` | core/pocketcam | the real camera: preview `/dev/video2` 640x360 NV16, stills `/dev/video1` 1920x1080 NV16 while the preview runs, black start-up frames dropped, the preview node closed and reopened rather than restarted |
| `pocketcam_fake.c` | core/pocketcam | the fake backend (below) |
| `pocketcam_convert.c` | core/pocketcam | NV12/NV16/RGB565 to RGB565 or RGB888, turned, mirrored, scaled |
| `pocketcam_store.c` | core/pocketcam | the photo folder: names, atomic writes, limits |
| `pocketcam_codec.c` | core/pocketcam | JPEG through libjpeg (`POCKETCAM_JPEG=1`), else PPM |
| `pocketcam_proto.h` | core/pocketcam | the session protocol and the shared-memory layout |
| `pos_camera.c` | tools/camera | `pos-camera session`, `probe` and `snap` |

**Pictures** travel in a sealed memfd (four 1024 x 1024 RGB565 slots: three
for the preview, one for the review) that the session maps read-only.
Ownership of a slot goes by message - `frame <slot>` from the helper,
`release <slot>` back - and the app copies the newest picture out and releases
it in the same timer tick, so no LVGL image ever points into memory the
helper writes. The helper converts only what it sends, at most 10 frames a
second.

**Lifetime.** The helper gets SIGTERM when the shell dies
(PR_SET_PDEATHSIG) and leaves on end of file; destroy() sends `quit` and
SIGTERM and gives it 300 ms before SIGKILL. Every wait on the helper has a
deadline (camera_session.h): 3 s for its first line, 10 s to open the camera,
4 s of silence while streaming, 20 s for a capture, 3 s for any other reply.
A missed deadline kills it and ends in `CAMERA_ERROR` ("not responding").

**Backends.** `pos-camera` takes `--backend`, else
`$POCKETOS_CAMERA_BACKEND`, else `v4l2` (settings through `--config` or
`$POCKETOS_CAMERA_CONFIG`: `preview=`, `still=`, `size=`, `still_size=`,
`mount=`; the defaults are unit A's). The simulator's shell passes `fake`
by default (CMake, SDL builds only); the device never does.

## Storage

(What the gallery adds - the export folder, metadata - is under "The
gallery".)

- **Where:** `$POCKETOS_STATE_DIR/camera`, i.e. `/var/lib/pocketos/camera`, on
  the root filesystem until the data partition of
  `docs/STORAGE_PLAN_v0.0.3.md` exists.
- **Names:** `IMG_<yyyymmdd>_<hhmmss>_<nnnn>.jpg` when the wall clock is valid
  (PocketClock's floor, 2024-01-01), `IMG_<nnnn>.jpg` when it is not;
  `<nnnn>` is one past the highest number in the folder, so names never
  collide and sort in the order taken.
- **Format:** JPEG, quality 88, the still turned upright (1080 x 1920 in
  portrait), with a small EXIF block (see "Metadata"). A host without libjpeg
  headers writes PPM instead, named `.ppm`, with the same facts as comments.
- **Atomic:** written to `.IMG_....tmp`, flushed, fsync'd, renamed, then the
  folder fsync'd; any failure removes the temporary; the next start removes
  what a power cut left.
- **Limits:** at most 500 photos and 64 MiB, and the filesystem keeps 48 MiB
  free. Past a limit the shutter still works but the capture is refused
  before the camera is touched, with a note ("Storage is full" or "The photo
  limit is reached"); nothing is ever deleted to make room.
- **Disk full mid-write:** the write fails with ENOSPC, the temporary is
  removed, the note says storage is full, the preview goes on.

## The gallery

### What it does

- **Grid.** Thumbnails of the library, newest first, a page at a time (as
  many cells of about 160 px as the body holds: 12 in portrait - 3 x 4 of
  170 px - and 10 in landscape on unit A); NEWER and OLDER
  turn pages; the status line says how many photos and which page. A file that
  cannot be read shows "Cannot show" in its cell. CAMERA goes back to the live
  picture; SLIDESHOW starts from the page's first photo.
- **One photo.** A tap on a thumbnail shows the photo fitted to the screen,
  upright, with three lines: its name; when it was taken; its size, file size,
  format, and "damaged" or "simulated" when that is so. NEWER and OLDER step
  through the library; BACK returns to the grid on that photo's page.
- **Delete.** DELETE asks "Delete this photo?" with CANCEL and DELETE; only the
  second DELETE removes the file. The next older photo takes its place.
- **Export.** EXPORT copies the photo into `~/Pictures` (below) and says
  where: "Saved to Files: /root/Pictures/IMG_....jpg", or "Already in Files:
  <path>" when a file there holds exactly this photo, byte for byte. A
  different file under the photo's name (another photo that got the same
  number, or one of the owner's) is kept, and the copy is saved as
  `IMG_..._nnnn-2.jpg` (then -3, ...).
- **Slideshow.** One photo after another every 4 s (`GALLERY_SLIDE_MS`, fixed),
  over the whole body with a status line under it ("Slideshow | 3 of 12 | tap
  to stop"), wrapping round at the oldest. The next photo is prepared while the
  current one is shown. A photo that cannot be read, or was deleted meanwhile,
  is skipped; when none can be shown it stops and says so. A tap on the
  picture or beside it stops it (the status line itself does not).
- **Failures said in words.** "Opening photos", "No photos yet", "Photos
  unavailable" (the helper missing, crashed, not responding, or the folder
  unreadable) with TRY AGAIN and CAMERA.

### Order

Newest first by the sequence number in the name (`<nnnn>`), which the store
makes one higher than any photo in the folder. It follows the order the
photos were taken whatever the wall clock did - a board without an RTC takes
dated and undated photos in the same visit - and ties fall back to the name.

### Metadata

Only what is actually known, written into the file as standard EXIF (JPEG) or
as `# doors-...` comment lines (PPM), read back by the gallery
(`core/pocketcam/pocketcam_exif.h`):

| Fact | Where it comes from |
| --- | --- |
| name, file size | the file |
| width x height | the picture's header (upright, after its EXIF orientation) |
| taken | EXIF `DateTimeOriginal`, written only when the wall clock was valid (PocketClock's floor); else the date in the name; else "Date unknown: the clock was not set". A date before 1995 (EXIF's start: an unset camera clock's 1970 or 1980), in the file or the name, or a name date that is not a real date, counts as unknown |
| orientation | EXIF `Orientation`, always 1 for Camera's own photos (they are turned upright when encoded); others 1..8 are honoured |
| software | EXIF `Software` "Doors <version>" |
| simulated | EXIF `ImageDescription` "Simulated picture" under the fake backend |

Nothing else is written: no make or model (the v4l2 backend does not identify
the sensor at run time), no exposure, no GPS. The file's modification time is
not shown as a capture time: after a cold boot without a network it is 1970.
Photos taken before this branch have no EXIF; their date comes from the name
when it has one.

### Architecture

| Part | Where | Role |
| --- | --- | --- |
| `camera_gallery.c` | apps/camera | the gallery's state machine; pure C: views, pages, which picture to ask for next and where each answer goes, the slideshow's clock |
| `camera_gallery_screen.c` | apps/camera | the gallery's screen (LVGL); copies finished pictures out of the shared memory into its own buffers |
| `camera_layout.c` | apps/camera | also the gallery's shapes (`camera_gallery_layout_compute`) and PHOTOS |
| `camera_session.c` | apps/camera | also runs `pos-camera library` (`cfg.library`) |
| `pocketcam_image.c` | core/pocketcam | header probe (JPEG markers and PPM, no decoder needed) and the bounded decoder |
| `pocketcam_exif.c` | core/pocketcam | EXIF and PPM-comment metadata, written and read |
| `pocketcam_store.c` | core/pocketcam | also the library's list and the export |
| `pos_camera.c` | tools/camera | also `pos-camera library` |

**One helper at a time, and no camera while browsing.** PHOTOS ends the
camera's helper (the camera is closed) and starts `pos-camera library`, which
never opens a camera; CAMERA ends it and opens the camera again exactly as a
fresh visit does. Both run under ADR-006's option C: the same process
boundary, sealed shared memory, slot ownership by message, watchdog and death
signal. Every file the gallery reads, decodes, exports or deletes is handled
by that helper, never on the LVGL thread; the only waits on the LVGL thread
are the bounded ones when a helper is ended (300 ms, then SIGKILL).

**Protocol** (`pocketcam_proto.h`): `list`, `picture <slot> <w> <h>
<cover|contain> <name>`, `export <name>`, `delete <name>`; answers `listed`
(names in slot 3), `image` / `imgfail`, `exported` / `expfail`, `deleted` /
`delfail`. Up to three pictures are out at once, one per preview slot.
Answers for a page or photo no longer shown are dropped and their slots given
back.

**Memory.** Nothing is decoded at full size. A JPEG is scaled inside libjpeg's
DCT to the smallest n/8 that still covers the box, then streamed a line at a
time into the destination, turned for the EXIF orientation; memory is the
destination and one line. A picture larger than 8192 px a side or 16 M pixels
is refused before anything is allocated. The screen holds one page of
thumbnails (15 x 170 x 170 x 2 bytes, 0.9 MB in portrait), the photo view's
picture (at most one slot, 2 MB) and, only while the slideshow runs, its two
pictures; all of it is freed when the gallery is left. The camera's own
preview and review buffers are freed while the gallery is open.

**Damage.** A JPEG cut short or with garbled data shows what libjpeg recovered
and says "damaged"; one whose header cannot be read shows "The file is damaged
and cannot be shown"; a file deleted meanwhile, "The file is gone". A decode
that hangs is the watchdog's (8 s), and a helper that crashes ends on the
failure panel with TRY AGAIN; the shell is never affected. The helper answers
requests in order, so each answer starts the window of what waits behind it: a
delete or export queued behind slow decodes is not taken for a hung helper.
DELETE of a photo that is already gone removes its entry.

### Storage and Files

- **One library**, unchanged: `$POCKETOS_STATE_DIR/camera`
  (`/var/lib/pocketos/camera`). Every earlier capture is in it and in the
  gallery. It stays Doors' own data, which moves to `/data` with the rest of
  the state when `docs/STORAGE_PLAN_v0.0.3.md` lands, and which Files shows
  read-only (Files > / > var > lib > pocketos > camera): nothing but Camera
  can rename or delete a photo there, so the library cannot be confused.
- **Export: `$HOME/Pictures`** (`/root/Pictures`), created when first needed.
  This is where photos meant for the owner go: Files opens in `$HOME`, so the
  folder is on its first screen, and it is writable there - copy, move,
  rename, delete. An export is a copy: written to a temporary name of its own
  (`.<name>.<pid>.export`), synced, linked into place so it never replaces a
  file, given the photo's own modification time (Files sorts it by when it was
  taken), and refused when it would leave less than 48 MiB free. A temporary
  that a killed export left is removed when the next library helper starts. It
  is not a second library: the gallery never lists it.
- No change to Files was needed.

## Transfer to another device (designed, not implemented)

Nothing in Doors today moves a file off the unit except by hand (the SD
card, `scp` over Wi-Fi). EXPORT is the first step: a photo in `~/Pictures` is
an ordinary file every later transport can pick up. Sending one to the
LILYGO T-Deck needs work that belongs to neither Camera nor this branch:

- **Transport.** radiod (SX1262, LoRa) is a few hundred bytes a second at best
  and duty-cycled: a 300 KB JPEG is minutes of airtime and would need a
  thumbnail-sized re-encode (e.g. 160 x 90, quality 60, ~5 KB). Wi-Fi (netd)
  is the realistic carrier but has no peer-to-peer service. Either way it is a
  new, versioned protocol with chunking, acknowledgement and resume, a
  cross-platform decision (the T-Deck side is another firmware), and an ADR.
- **Interface this branch leaves ready.** `pocketcam_store_export()` (copy out
  of the library, no-replace, space-checked) and `pocketcam_image_decode()`
  (any size, bounded memory) are what a future `transfer.*` service would
  call: pick a photo by name, produce the bytes to send (the file, or a
  re-encoded small version), and hand them to the transport. The gallery would
  gain one action, SEND, beside EXPORT, answered like `exported`/`expfail`.
- **Needed from the platform first:** a transfer service API (public name
  `transfer.*`, not K230-specific), a peer model (who is the T-Deck, pairing),
  and a re-encode entry point in pocketcam for small previews.

## QR codes (feasibility; not implemented)

No QR or barcode decoder exists in the repository, in `vendor/`, or in the
image as far as the documented package list goes (the Buildroot package
depends on cjson, libgpiod2, lvgl, libdrm, libevdev, alsa-lib, jpeg and
libcurl). The candidates:

- **quirc** (ISC, about 4 000 lines of C, no dependencies): QR only, decodes
  a greyscale frame. The right size for this board. It would be a new
  third-party dependency - vendored at a pinned commit, a THIRD_PARTY_NOTICES
  entry and the licence review of docs/LICENSING.md - which this branch was
  asked not to add without a proposal.
- **ZBar** (LGPL-2.1, QR and 1-D barcodes): larger, a shared library in the
  image and an LGPL obligation; more than a first step needs.
- **OpenCV's QRCodeDetector**: C++ and far too heavy for this.

How it would fit without destabilising the camera: in the helper, on the
preview frames it already converts (the Y plane of NV16 is the greyscale
quirc wants, 640 x 360 is enough for a QR at arm's length), at most a few
times a second, answering `qr <text>` on the existing protocol; or on a still
in the library helper (`scan <name>`). The app would show the text and, since
the shell has no clipboard, offer nothing more than showing it until one
exists. Decision needed from the owner: whether quirc may be vendored.

## The fake backend

`--backend fake` (or `POCKETOS_CAMERA_BACKEND=fake`), scripted by `--fake` or
`POCKETOS_CAMERA_FAKE`, comma-separated:

| Key | Effect |
| --- | --- |
| `open=ok\|nodev\|busy\|fail\|hang`, `open_delay=MS` | how opening ends |
| `size=WxH`, `still=WxH`, `format=nv16\|nv12`, `period=MS` | the stream |
| `frames=N` | nothing after N frames (a stall) |
| `lost_after=N` | the camera goes away after N frames |
| `malformed_at=N` | frame N claims fewer bytes than it needs |
| `delay_at=N:MS` | frame N is late |
| `hang_at=N` | asking for frame N never returns (a stuck driver) |
| `crash_at=N` | the helper aborts at frame N |
| `capture=ok\|fail\|lost\|hang`, `capture_delay=MS` | how a still ends |
| `mount=R[m]` | the sensor mounting it reports |

Frames are eight colour bars with an orange marker in the sensor's top-left
corner (so a test can tell which way a picture was turned) and a grey square
that moves with the frame number; every pixel is a pure function of its
position and the frame number. Unknown keys are refused. The fake measures
nothing about a real camera.

## Keyboard (documented for later, not implemented)

Space or Enter: shutter. In review: K keep, D delete then Enter to confirm,
Escape cancel. Backspace: leave (the shell's back).

## Layout

DS §34 (Amendment R, ACCEPTED 2026-09-25). Fullscreen (NONE chrome): the shell's header
carries the back slab and the hint. Only existing roles: a slab behind the
picture, the §7 primary and secondary buttons at 64 px (the shutter 240 x 96),
a 72 px slab for the last photo, title and secondary text for the panel.

## Tests

Host only; none needs unit A.

- `tests/pocketcam_test.c` (103 checks): frame validation; the fake and every
  fault; the v4l2 backend on a host with no camera and its refusal of bad
  settings; conversion in all four turns and both fits,
  checked against the fake's own pattern; the encoder; the store's names,
  atomic write, limits, a full disk, a disk that fills mid-photo, leftovers.
- `tests/camera_state_test.c` (66): every state, event and tap.
- `tests/camera_layout_test.c` (57): both shapes, corners, short and huge
  bodies, safe and non-overlapping targets.
- `tests/camera_session_test.c` (79): the real helper on the fake - pictures
  in the right orientation, the 10 fps cap, no slot leak, capture, review,
  delete, full disk, mid-write ENOSPC, no camera, busy, missing helper, lost
  camera, late and damaged frames, the watchdog on a hung open and a hung
  driver, a crash, a kill from outside, the shell dying (by end of file and
  by the death signal alone), fifty opens and closes with no descriptor or
  child left.
- `tests/camera_app_test.c` (84, SDL build): the app hosted like the shell
  hosts it, in portrait and landscape; take, keep, review again, delete with
  confirmation, the file on disk exactly when the screen says so; no camera,
  missing helper, full disk, crash and Try again; closing mid-capture; twenty
  opens and closes.
- `tests/camera_shell_test.sh`: the above, then the real shell opening Camera
  in both orientations with the live picture on screen, no warnings, and no
  helper left.
- `tests/camera_lint.sh`: the boundaries (no LVGL below the screen, no device
  access in the app, the fake never the device default, destroy order,
  PocketUI's corner rule, v1 scope).
- `make camera-san-test`: the unit suites again under ASan and UBSan, with
  the helper built the same way (`POCKETCAM_JPEG=1` runs them with libjpeg).

The gallery's own:

- `tests/pocketcam_gallery_test.c` (77 checks; 90 with libjpeg): EXIF written
  and read back, an unset clock writing no date, every truncation of a block,
  a big-endian block with a looping pointer, PPM comments; every refusal of
  the reader (missing, empty, text, 0 x 0, too large, 16-bit, cut header,
  folder, garbage markers); a JPEG's size from its header alone; fit sizes;
  decoding synthetic four-colour pictures with every quadrant checked,
  contain and cover, a file cut short; with libjpeg also EXIF orientations 3, 6
  and 8, a 1920 x 1080 photo as a thumbnail and a screen picture, a JPEG cut in
  half and one garbled; the library's order (numbers over dates, no other
  names, folders or links), a capped list, and export (whole, with the
  photo's time, no-replace, another file under the name going to -2, the same
  name and size with other bytes kept, found again under the name it got, a
  name that is a path, a missing photo, a full disk leaving nothing behind,
  the sweep of a killed export's temporaries, the default folder); dates
  before 1995 unknown.
- `tests/camera_gallery_test.c` (90): opening, listing, the empty library,
  the helper crashing, a missing helper, an unreadable folder; pages, three
  requests at most, answers for a page no longer shown dropped; the photo view,
  its navigation, its three lines with and without dates in the file or the
  name (a 1970 or 13th-month name date is no date); delete with its confirmation and its failure, deleting the last
  photo; export and each failure; the slideshow's interval, order, wrap,
  one request and two pictures at most over twelve photos, skipping damaged
  photos, stopping when none can be shown, one photo, no photos.
- `tests/camera_layout_test.c` (now 126): the gallery in both shapes, with
  corners, short and huge bodies, the slideshow's picture inside its touch
  box; PHOTOS in the camera's layout.
- `tests/camera_session_test.c` (now 131): the parser's new lines, and the
  real library helper: never a camera backend on its command line, the list,
  three pictures at once and a fourth waiting, thumbnails and fitted pictures
  upright with the fake's marker where it belongs, damaged and missing files,
  sixty pictures in a row with no slot lost, export, delete (and again, of a
  photo already gone), an empty library, no child or descriptor left; a
  delete queued behind three slow pictures (`POCKETCAM_TEST_DECODE_MS`)
  answered, not killed as hung.
- `tests/camera_app_test.c` (now 185): the gallery tapped in portrait and
  landscape - PHOTOS closes the camera and starts the library helper, the
  grid with a damaged file, the photo view and its lines, NEWER/OLDER, EXPORT
  into `$HOME/Pictures`, DELETE with CANCEL, the slideshow skipping the damaged
  file, a tap on the slideshow and one beside its picture stopping it,
  CAMERA bringing the live picture back with the camera's helper; no
  camera but photos, a missing library helper and TRY AGAIN, closing
  mid-slideshow, ten camera-gallery trips with one helper at a time.

## On unit A (gallery)

Run 2026-09-27 on unit A with build `f6fe537` (`docs/hardware/CAMERA_GALLERY_GATE.md`).
**Not passed: the unit hung** (serial console and network dead) during the
9th-10th of ten Camera -> PHOTOS -> photo -> back cycles, about 27 camera
opens after the deploy. After a power cycle, a serial-captured reproduction
hung it again after 40 camera-only open/close cycles with no gallery,
silently (no kernel message at loglevel 8). On the real GC2093 and the
image's libjpeg 9f:

- a 1080 x 1920 photo as a thumbnail in about 65 ms, as the screen picture in
  about 135 ms; a page of 9-10 thumbnails in 0.75-1.1 s; a 4032 x 3024
  progressive JPEG in about 0.58 s, the helper at 37 MB while it decodes;
- the slideshow's interval 3934-4058 ms (median 4020) over five minutes and
  80 photos, unreadable ones skipped, shell RSS flat after the first 10 s;
- ten PHOTOS <-> CAMERA trips: the sensor released on every PHOTOS (the library
  helper holds no video node), taken again on every CAMERA, one helper at a
  time, no zombies, no supervisor restart;
- EXPORT to `/root/Pictures` byte-identical with the photo's time; "Already in
  Files"; a same-name, same-size file with other bytes kept and the copy
  saved as `-2`; the copies in Files under Home > Pictures; the EXIF block
  read the same by Windows GDI+ and an independent parser;
- orientations 3, 6 and 8, damaged, truncated, empty, text and over-size
  files, a file removed behind the gallery's back, delete with CANCEL, the
  final photo deleted, an all-unreadable slideshow, rotation both ways during
  a slideshow and a photo view, leaving from the grid, a photo and a running
  slideshow.

Still needed: the camera-stack lock-up investigated on its own (master's
binaries under the same loop; the vendor vvcam/ISP release path), and the
owner's own eyes and fingers. The
unset-clock case is covered by the host suites and by undated names on the
unit; the device clock was not changed.
