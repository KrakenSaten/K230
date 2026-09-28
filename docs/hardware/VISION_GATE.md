# Vision - Unit A hardware gate

Branch `feat/vision-app`. **Not run yet: unit A was off the bench when the
branch was made (no answer on 192.168.10.171 or .157, no console on COM9,
2026-09-28).** Every step is on unit A with the shell started normally and
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
