# Vision - hardware gate

**Run on unit B (K230-B, 192.168.10.187, console COM12) on 2026-09-28, by
the owner's choice: unit A was off the bench. IN PROGRESS, stopped by a
whole-unit freeze (below).** Unit B carries the v0.0.13 dev image
`ee39407` with the Doors userspace of build `7591725` deployed by
`deploy.sh`, and `/usr/bin/pos-vision` hand-installed from `2e2f30b`
(md5 `78a359a2ece1f4c3133cf8b25b0e41cb`; the build id is not compiled into
it). Rollback of everything the deploy replaced:
`/root/rollback-vision/RESTORE.sh`. Rotation: landscape (display 270).

## Results so far (unit B)

| # | Step | Result |
| --- | --- | --- |
| 1 | `pos-vision probe` | PASS (LIVE-READ): `camera v4l2 preview 640x360 mount 90`, `model yolov8n.kmodel backend nncase input 320x320 classes 80 rows 2100`. The ISP's self path accepts planar BGR (`BG3P`) at 640 x 360: the assumed format holds. Model sha256 `0b4bcdd3…2004a09` |
| 2 | `pos-vision bench 100` | Runs: 100 frames, 19.4 fps through the whole pipeline; mean AI2D 1.1 ms, KPU 20.4 ms, decode + NMS + track 8.2 ms; helper 38-42 % CPU, 4.7 MB RSS. 0 boxes: the scene was dark (a Camera still of the same moment is black but for one lit edge). The bench then printed no refused rows - see the defect |
| 3 | Open Vision (7591725 helper) | **FAIL, fixed**: "Vision stopped / The detector stopped making sense" within a second. The kmodel's class scores on a dark frame ran -0.0091 .. 0.0187, so ~1,800 of 2,100 rows had every score a hair below zero and the decoder refused them as not a probability; ten such frames end the session. Fixed in `2e2f30b` (scores within 0.1 of [0, 1] clamped; the bench now prints refused rows and the tensor's ranges). After the fix: 0 rows refused over 120 frames |
| 3b | Open Vision (fixed helper) | PASS (LIVE-READ, screenshot `out/vision-gate/caps/v2-landscape-live.png`): live picture, status `20.7 fps KPU 31 ms pre 2 post 10 CPU 36% 5 MB`, the ACROSS line drawn, DOWN 0 / UP 0, and one confirmed track `#2 person 40%` over most of the dark frame (a real person close to the lens or a low-light false positive: not decidable from the picture). `top`: `pos-vision` 49 % CPU, 5.5 MB RSS; `doors-shell` 10 % CPU, 16 MB RSS |
| - | **Whole-unit freeze** | Vision left open and streaming: within about two minutes of the 08:50:05 camera open the unit stopped answering SSH, ping and its serial console. The console (loglevel 8, logged from 08:43) printed nothing after the open: no release, oops, panic, RCU stall or watchdog line. Still hung at 09:01 (a newline on COM12 got no answer). Needs a power cycle |

Not yet run: boxes on real, lit objects, ids across motion, counting,
ten open/close cycles, Camera afterwards, leftovers.

The freeze is new in one respect: the known camera lock-up
(docs/KNOWN_ISSUES.md) came a few seconds after a camera close, never
during streaming. Here the camera was streaming, with the KPU running
every frame. Whether Vision's pipeline causes it, or it is the same vendor
ISP fault meeting a longer stream, is open. The first thing after the power
cycle is to separate the two: `pos-vision bench 3000` (about 2.5 minutes of
streaming and inference, no shell app) with the console logged, then the
same length of Camera preview alone.

## The original plan (unit A)

Every step is on unit A with the shell started normally and
a second machine on SSH for the reads. Record PASS/FAIL, the build id and
the evidence class (LIVE-READ, OWNER) per step. The camera lock-up of
docs/KNOWN_ISSUES.md ("Whole-unit lock-up after repeated camera
open/close") applies to Vision exactly as to Camera: keep the open/close
count bounded, log the console (`ser_log.ps1` on COM9), and expect to need
the owner for a power cycle.

## Before

```sh
cat /etc/doors-release; doors shell info | head -5    # the build under test
ls -l /usr/bin/pos-vision /usr/share/doors/vision/    # helper and model in place
sha256sum /usr/share/doors/vision/yolov8n.kmodel      # the vendor file's hash
df -h /; free -m                                      # 5.5 MB helper + 3.5 MB model fit
pgrep -a isp_media_server                             # the ISP daemon is up
```

The model is put on the unit by `tools/vision/install-model.sh <ip>` after
`deploy.sh`; it is not in the package (docs/LICENSING.md item 10).

## The gate

| # | Step | Pass when |
| --- | --- | --- |
| 1 | `pos-vision probe` with the Vision screen closed | prints `camera v4l2 preview 640x360 mount 90` and `model yolov8n.kmodel backend nncase input 320x320 classes 80 rows 2100`, exit 0. If the camera line fails: `pos-vision probe --config fmt=bgr,size=1280x720` (the vendor's size), then `--config fmt=bgr,preview=/dev/video1`; note which the ISP accepts |
| 2 | `pos-vision bench 100` pointed at a person and a cup | frames flow; boxes name `person` and `cup` with sensible confidences; the summary line gives fps and the mean pre / infer / post ms - record all four |
| 3 | Open Vision from the launcher (DEVICE, the eye) in portrait | "Starting", then the live picture within 2 s, upright and not mirrored; the status line shows fps, KPU ms, CPU and MB within 2 s; header without SIMULATED |
| 4 | A person in front of the camera | an outline follows them with `#N person NN%`; the id stays the same while they move about; it survives them stepping out for less than a second; a new id when they return after longer |
| 5 | A cup, a phone, a chair | each gets its own box and name; boxes align with the objects (the outline within the object's edges, not offset or mirrored) - compare against the picture |
| 6 | COUNT across | LINE: ACROSS (the default); walk the person down through the line and back: DOWN 1, then UP 1; stand on the line and sway: no change |
| 7 | COUNT down | LINE: DOWN; walk left to right and back: RIGHT 1, LEFT 1 |
| 8 | RESET | both counts 0 |
| 9 | Rotate | Settings to landscape (or the keyboard base) and back: the wide layout with the picture at full height and the column on the right; boxes still on the objects; the counts start over (the shell restarted) |
| 10 | Measured | with a person in view for a minute: note fps, KPU ms, pre ms, post ms from the status line; `top -d 2` for `pos-vision` %CPU and `doors-shell` %CPU; `grep VmRSS /proc/$(pgrep pos-vision)/status` and the shell's |
| 11 | Repeated open and close | ten cycles of open Vision, 5 s, back: each time a picture and boxes; `pgrep pos-vision` empty after each close; the console logged throughout |
| 12 | Camera afterwards | open Camera: preview and a photo as before (NV16 untouched); close it |
| 13 | Vision after Camera | open Vision again: boxes as before |
| 14 | No model | move the kmodel aside, open Vision: "No camera or detector" with the reason, CHECK AGAIN; put it back, CHECK AGAIN: live |
| 15 | No crashes, no leftovers | `pos system crashes` shows nothing new; `pos logs shell` has no ERROR from the session; no `pos-vision` or `pos-camera` process; nothing under `/proc/*/fd` holds `/dev/video*` |

## Result on unit A

To be filled in: build id, rotation mode at the start, PASS/FAIL per step,
the numbers of steps 2 and 10, and anything found on the way (the format
and size the ISP's self path really accepted in step 1 goes into
docs/apps/VISION.md and, if it differs from the default, into
`VISION_CAMERA_CONFIG` in tools/vision/pos_vision.c).
