# Camera - unit A gate (2026-09-25)

**Unit A carries a hand-installed Camera build over the flashed v0.0.12
image:** `/usr/bin/doors-shell` and `/usr/bin/pos-camera` built from
`feat/camera-app-design` at **`e3d3f71`** (the last code commit; the branch
tip after it changes documents only), plus
`/usr/share/doors/ui/icon-camera.bin`. Everything else is the flashed image
(`/etc/doors-release` still says `0.0.12 BUILD_ID=a8b1a9f`). Rollback:
`/root/rollback-camera/RESTORE.sh` (puts the v0.0.12 shell back, removes the
helper and the icon, checks the shell's hash). Left at home, landscape,
rotation mode Automatic (as found), keyboard base attached. Unit A on Wi-Fi at
`192.168.10.171`.

| File | sha256 |
| --- | --- |
| doors-shell (e3d3f71) | `8ecc8335865781779cb9a8049a56ae6bf7ab160ced964ca291f7a4d59482ebc0` |
| pos-camera (e3d3f71) | `15bcd5618f1980be1d158b61feb27ed456cd26e0d3b8c7f8bca6427c0d2b3232` |
| icon-camera.bin | `60a142bf9bcf8d0926e29b39abac18079e4977f394ba50e8aa4b269c08a7051c` |

Built from a clean clone of the commit (cross toolchain of the pinned SDK,
`CMAKE_BUILD_TYPE=Release` shell as the Buildroot package builds it,
`POCKETCAM_JPEG=1`). The owner was at the bench for the orientation, colour
and keyboard steps; everything else was driven over SSH with touch events
injected into the touch controller and the panel read back with kmsgrab.

**Verdict: PASS**, with one lock-up found and fixed on the way (below).

## 1. The camera itself (v4l2-ctl, then pos-camera)

| Finding | Result |
| --- | --- |
| Working nodes | `/dev/video1`, `/dev/video2`, `/dev/video3` (vvcam MP, SP1, SP2) all stream NV16 at 640x360 and 1920x1080 |
| Chosen | preview `/dev/video2` 640x360 NV16, still `/dev/video1` 1920x1080 NV16, **at the same time** (a still while the preview runs works; its first frame is black, the next is exposed) |
| Frames | real frames; ~14 black frames after STREAMON while exposure starts, light after ~0.5-0.9 s; 29.7-29.9 fps |
| Colour | BT.601 limited range as the driver reports; natural against the room (owner) |
| Orientation | a frame needs a quarter turn clockwise on the native portrait panel, not mirrored; the shell's landscape is display rotation 270, so landscape needs a half turn (owner, printed text, both orientations) |
| Second opener | EBUSY at `VIDIOC_REQBUFS`, reported as "the camera is in use" |
| Driver quirks | `v4l2-ctl --list-ctrls` on `/dev/video1` spins and needs `kill -9`; `ENUM_FMT` on `/dev/video1` lists nothing yet `S_FMT` works. The backend does neither |
| Costs (`pos-camera bench`) | open 60 ms; convert 29.8 ms (528x938, turned) / 16.5 ms (802x452); still 190-220 ms; JPEG encode 0.37-0.52 s; 0.56-1.16 MB a photo |

## 2. The lock-up, and the fix

The first app gate took a photo (`IMG_20260924_224223_0001.jpg`, whole,
1.16 MB) and then **the whole unit locked up**: panel frozen on "Starting the
camera", touch dead, Wi-Fi gone; only a power cycle (owner) brought it back.
The shell log ends right after the photo; no crash report (there is no pstore
on this kernel).

The step after a capture is the preview starting again. The backend did that
as `STREAMOFF` then `STREAMON` on the same open `/dev/video2`, a sequence the
bench had never run and the vendor's camera app never runs (it closes and
reopens the node for every preview). Fix (`d10e27e`): the preview node is
closed on stop and after a still and opened afresh on start. `pos-camera soak`
repeats the app's sequence: 5/5, 30/30 and 35/35 cycles on unit A, no hang,
and no lock-up in any later step of this gate. That the restart was the cause
is **LIKELY**, not proven: proving it would cost another lock-up.

Also seen once: an open ~2 min after that power-on failed with EIO and worked
a minute later. The backend now retries an EIO open four times, 500 ms apart
(`e3d3f71`). After a reboot Camera opened 21 s after power-on with a live
picture.

## 3. The app

Run on `d10e27e` in both orientations (the scripted gate), then smoke on
`e3d3f71` in landscape.

| Step | Portrait | Landscape |
| --- | --- | --- |
| Open Camera, live preview, helper running | PASS | PASS |
| Picture upright, not mirrored (owner) | PASS | PASS |
| TAKE PHOTO: review with the photo's name, DELETE / KEEP | PASS (1080x1920 JPEG) | PASS (1920x1080 JPEG) |
| KEEP: preview again, last-photo thumbnail shown | PASS | PASS |
| Last photo reopens in review | PASS | PASS |
| DELETE asks; CANCEL keeps the file; DELETE, DELETE removes it | PASS | PASS |
| A second photo, kept; files whole (SOI/EOI), no `.tmp` left | PASS | PASS |
| Close and reopen x5; no helper at home | PASS | PASS |
| Services (radiod, meshcored, sysd, netd, isp_media_server) same PIDs; no crash reports; no ERROR in the shell log | PASS | PASS |
| Final smoke on `e3d3f71` (capture, delete, capture, keep, home) | - | PASS |

**Rotate while open:** rotation with Camera open closes Camera the ordinary
way (the helper leaves with it), the shell restarts in the new orientation
and comes back **at home**, as it does for every app (a rotation restart does
not reopen the app; `ui/shell/shell.c`, not Camera's). Reopened in the new
orientation: PASS.

## 4. Keyboard base and camera together

The camera's I2C controller is also routed to the keyboard's pins (BSP patch
0058, CAMERA_PLATFORM_RESEARCH.md U9).

| Check | Result |
| --- | --- |
| 10 keys pressed with Camera streaming; the shell's per-run count at the next restart | **10 delivered, 0 dropped**; preview kept moving (owner) |
| Typing "camera and keyboard 123" in Notes while `pos-camera soak` streamed and took 35 stills | every key arrived in order (owner and panel capture); soak 35/35 |
| Keyboard presence and rotation (Automatic -> landscape 270) through the whole gate | unchanged |

## 5. Not done here

- A fresh flash with the camera build (this is a hand install over v0.0.12).
- Power draw (U11), a DRM video plane (U12), and how the buffers are mapped
  (U13; conversion from them runs at the measured cost, which is what
  matters).
- Very low light: in a dark room the picture can stay black for up to 3 s
  after each (re)open, while near-black frames are held back.
- The note "camera and keyboard 123" and four gate photos are left on the unit.
