# Camera

A live picture, a shutter, and a look at the photo just taken: keep it, or
delete it.

Status: **working on unit A, on branch `feat/camera-app-design` (rebased on
v0.0.12), not merged.** The real V4L2 backend is written and gated: live
preview and JPEG stills on unit A in portrait and landscape, keep and delete,
the keyboard base alongside (`docs/hardware/CAMERA_GATE.md`, PASS on a
hand-installed build `e3d3f71`). Host-tested end to end on the fake backend.
Architecture ADR-006 and layout DS §34 (Amendment R) are PROPOSED. The
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

## What it is not (v1)

No video, filters, QR scanning, AI, gallery, sharing or upload. No resolution
selector, exposure control or camera switch: the board has one sensor with
one mode, and which ISP controls work through V4L2 is not known yet. No review
of photos from earlier visits (that needs a decoder and is the gallery's
job); Files can browse the folder, read-only, like all of Doors' own data.

## States

| State | Shows | Taps |
| --- | --- | --- |
| `CAMERA_INIT` | panel "Starting the camera", shutter disabled | - |
| `CAMERA_PREVIEW` | the live picture (or "Waiting for the picture" until the first one); the last photo if there is one | TAKE PHOTO (once a picture has arrived and the stream is not stalled), the last photo |
| `CAMERA_CAPTURING` | the last preview picture, "Taking the photo..." then "Saving the photo...", shutter disabled | - |
| `CAMERA_REVIEW` | the photo, its name, DELETE and KEEP; while confirming, "Delete this photo?", CANCEL and DELETE | KEEP, DELETE, CANCEL |
| `CAMERA_ERROR` | a panel that says what went wrong: busy, the helper missing, not responding, crashed, lost | TRY AGAIN |
| `CAMERA_NO_DEVICE` | "No camera" and why (none found, or not built in) | CHECK AGAIN |

The back slab in the shell's header leaves from any state; leaving always
closes the camera. A rotation restarts the shell, which closes the camera the
same way and opens it again in the new orientation.

## Architecture

The app never opens the camera. A helper process owns it for as long as the
screen is open (ADR-006, PROPOSED):

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

- **Where:** `$POCKETOS_STATE_DIR/camera`, i.e. `/var/lib/pocketos/camera`, on
  the root filesystem until the data partition of
  `docs/STORAGE_PLAN_v0.0.3.md` exists.
- **Names:** `IMG_<yyyymmdd>_<hhmmss>_<nnnn>.jpg` when the wall clock is valid
  (PocketClock's floor, 2024-01-01), `IMG_<nnnn>.jpg` when it is not;
  `<nnnn>` is one past the highest number in the folder, so names never
  collide and sort in the order taken.
- **Format:** JPEG, quality 88, the still turned upright (1080 x 1920 in
  portrait). A host without libjpeg headers writes PPM instead, named `.ppm`.
- **Atomic:** written to `.IMG_....tmp`, flushed, fsync'd, renamed, then the
  folder fsync'd; any failure removes the temporary; the next start removes
  what a power cut left.
- **Limits:** at most 500 photos and 64 MiB, and the filesystem keeps 48 MiB
  free. Past a limit the shutter still works but the capture is refused
  before the camera is touched, with a note ("Storage is full" or "The photo
  limit is reached"); nothing is ever deleted to make room.
- **Disk full mid-write:** the write fails with ENOSPC, the temporary is
  removed, the note says storage is full, the preview goes on.

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

DS §34 (Amendment R, PROPOSED). Fullscreen (NONE chrome): the shell's header
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
- `make camera-san-test`: the four unit suites again under ASan and UBSan,
  with the helper built the same way.
