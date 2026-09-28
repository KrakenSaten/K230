# Vision - hardware gate

**Run on unit B (K230-B, Wi-Fi .187 then .140 after a power cycle, console COM12) on 2026-09-28, by
the owner's choice: unit A was off the bench. Everything but repeated open/close
PASSES; the camera lock-up froze the unit five times (below). The owner
accepted the lock-up as the known vendor fault (2026-09-28, option 1) and sent
the prototype to review with that caveat: READY TO REVIEW, not merged.**
Unit B last carried `pos-vision` md5 `d434be09ad76c463883ab8c84a1ca34c` and
`doors-shell` md5 `6330c83470ad2641648ce5ca7aead5d5`, both hand-installed from
`543dc0e` over the deployed `7591725` (the deployed shell kept as
`/root/rollback-vision/doors-shell.7591725`), and was left hung after the last
freeze. Unit B carries the v0.0.13 dev image
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

### Second session (unit B after the owner's power cycle, Wi-Fi now 192.168.10.140)

The unit the owner restarted at about 14:10 UTC was K230-B (its pinned host
key, Wi-Fi MAC `88:3b:dc:b7:9e:d3`, build `7591725`), not unit A: unit A's
Wi-Fi MAC `…9e:c7` did not appear on the network in a four-minute sweep
and its console (COM9) was not attached. The gate went on on unit B.

| # | Step | Result |
| --- | --- | --- |
| 4 | Scene | Still dark: a Camera still at 14:17 is black. Every `person` box so far is a full-frame low-light false positive (35-50 %) |
| 5 | `pos-vision bench 3000`, helper alone, memory probe every second | PASS: 3000 frames in 103.7 s, **28.9 fps** through the pipeline (so the sensor delivers at least that; it is 30 fps), mean AI2D 1.1 ms, **KPU 19.2 ms**, decode + NMS + track 8.1 ms; 0 rows refused; no freeze; temperature 55-57 °C; the helper's RSS 4.9 MB throughout |
| 6 | Vision app open five minutes (14:19:55-14:25:55), sampled every 5 s | PASS: no freeze; `pos-vision` 38-50 % CPU, **RSS 6.1 MB flat**; `doors-shell` 8-18 % CPU, RSS 16.5 MB; load 1.1-1.9; 56-60 °C; MemFree and CmaFree both drift down ~250 KB a minute while streaming (about 1.5 MB over the five minutes) |
| 7 | Leave Vision (`doors app home`) | **Whole-unit freeze.** The console's last lines are the camera's normal release (`vvcam_mipi_release`, `vvcam_isp_release:187`, 14:26:39), then nothing: no oops, panic or watchdog, no SSH, no ping. This is the signature of the pre-existing camera lock-up in docs/KNOWN_ISSUES.md, reached here on the fourth camera release of this boot (bench, still, bench, app) rather than after 21-40 as with Camera on unit A |

Measured, in one place:

| What | Value |
| --- | --- |
| Camera | 30 fps sensor; ≥ 28.9 fps delivered to the pipeline (bench 3000) |
| Inference (KPU) | 17-20 ms bench, 31 ms in the app's status line (the shell competes for the CPU) |
| Preprocess (AI2D) | 1.1 ms |
| Decode + NMS + track | 7-10 ms |
| Total | 28.9 fps helper alone; 20.7 fps with the app open |
| CPU | `pos-vision` 38-50 %, `doors-shell` 8-18 % |
| RSS | `pos-vision` 4.9-6.1 MB, flat; `doors-shell` 16.5 MB |

Not run: boxes on real lit objects, ids across motion, counting (the scene
is dark); ten open/close cycles and Camera afterwards (each freeze costs a
power cycle at the bench).

The two freezes: the first came while Vision was streaming (no release
line), the second a few seconds after a clean release - the known
lock-up's own pattern. The helper alone streamed 104 s and the app five
minutes without a freeze, and memory stayed flat, so nothing points at
Vision's own code; but two freezes in two short sessions is far sooner than
Camera's 21-40 opens, and the one difference in the camera path is the
planar BGR format on the self path (Camera asks for NV16). That is the one
experiment left that needs no code: `--config fmt=nv16` cannot feed the
KPU, so the useful comparison is Camera open/close cycles on the same boot
versus Vision cycles, counted until the freeze, with the console logged.

### Third session (unit B after a second power cycle, Wi-Fi 192.168.10.187, lit room, owner walking)

| # | Step | Result |
| --- | --- | --- |
| 8 | Preview | PASS (LIVE-READ, `out/vision-gate/caps/s-*.png`, `d-*.png`): the room lit, the live picture upright and not mirrored |
| 9 | DETECT: boxes, class, confidence | PASS with a caveat: the owner is boxed tightly standing and walking (`#8 person 76%`, `#7 person 83%`, `#126 person 83%`); the coat on the chair draws low-confidence false classes (`baseball-glove 53%`, `backpack 35%`) and often a wide false `person` (36-83 %) over the dark left edge |
| 10 | Colour | **FAIL, fixed** (`99739f5`): Vision's preview showed the red blanket and the warm wall blue while Camera showed them red and orange: the ISP's BG3P planes are R, G, B, not B, G, R. The KPU input was never affected. After the fix the preview matches Camera (`caps/cycle1.png`) |
| 11 | COUNT, ACROSS line | Counted DOWN 1, UP 1 while the owner walked sideways: not real crossings - a horizontal line sees the box centre move as the body enters and leaves the frame edge |
| 12 | COUNT, DOWN line (vertical, LEFT/RIGHT), switched by an injected tap after RESET | PASS: LEFT and RIGHT climbed together 0/0 -> 6/6 over ~75 s of walking back and forth (`caps/sheet-d.png`). Exact per-pass accuracy is the owner's to confirm; the wide false box on the left can add a count when it merges with the real person |
| 13 | TRACK | PASS (indirect): a crossing counts only when one confirmed id is seen on both sides of the line, so each of the twelve counts is one id carried through several hundred pixels of motion; a new pass through the frame gets a new id, as designed. Captures 4 s apart cannot show a single id frame by frame |
| 14 | Exit Vision, then Camera | PASS: the helper gone 1.1 s after leaving (host-side, incl. SSH), `/dev/video*` free, no freeze; Camera opened with a live, natural-colour picture (`caps/camera-after.png`), closed cleanly |
| 15 | Ten open/close cycles | **FAIL at cycle 4 (whole-unit freeze).** Cycles 1-3 clean: helper up, one video descriptor, 5.4-6.1 MB, gone right after leaving, the camera free, the shell 14.9-15.4 MB, no crash, no shell restart. Cycle 4: the console's last lines are that cycle's camera open (16:41:50), then nothing; the unit stopped answering during the stream. The eighth camera open of the boot |

Measured (all sessions):

| What | Value |
| --- | --- |
| Camera | 30 fps sensor, ≥ 28.9 fps delivered to the pipeline |
| Inference (KPU) | 17-20 ms (bench); 18-49 ms in the app's status line, mostly 18-28 |
| Preprocess (AI2D) | 1.1-2 ms |
| Decode + NMS + track | 6-10 ms (up to 19-25 on a busy frame) |
| Total | 28.2-28.9 fps helper alone; 23-28 fps with the app open |
| CPU | `pos-vision` 38-50 %, `doors-shell` 8-18 % |
| RSS | `pos-vision` 4.9-6.1 MB, flat over five minutes; `doors-shell` 15-16.5 MB |
| Crashes, helper leaks | none: no crash file, no shell restart, no helper or video descriptor left after any clean close |

**The blocker: the camera lock-up.** Three freezes on unit B today, one per
boot, silent on the console at loglevel 8: during a stream (boot 1, about
the eighth camera open), a few seconds after a clean release (boot 2, the
fourth release), during a stream (boot 3, the eighth open). Camera alone
reached the same lock-up after 21-40 opens on unit A (docs/KNOWN_ISSUES.md).
Nothing in Vision's own code is implicated - the helper ran 104 s and the app
five minutes without it, memory flat - but whether Vision's BG3P stream or
the KPU running alongside makes the vendor fault come sooner, or unit B is
simply more prone than unit A, is not known: that needs Camera-only cycles
counted on unit B, which costs power cycles at the bench.

### Fourth and fifth sessions (unit B, 2026-09-28 evening): isolating the lock-up

Every run below is a separate process, one camera open and close each, with
the console logged; "froze" means the unit stopped answering SSH, ping and
its console, the last console line a normal `vvcam_isp_release`.

| Test (unit B) | Result |
| --- | --- |
| Camera app, open/close cycles (NV16, Vision's own cycle timing) | 40 of 40, no freeze |
| `pos-vision bench 60` on the fake camera: the KPU, AI2D and model load/unload alone, no ISP | 30 of 30, no freeze |
| `pos-camera bench --config fmt=bgr`: the ISP streaming BG3P, no KPU | 30 of 30, no freeze (and two libjpeg segfaults in Camera's still encoder right after boot, unrelated to Vision, not seen again) |
| `pos-vision bench 60` on the real camera: BG3P + KPU in one process, run to completion | 30 of 30, no freeze |
| `pos-vision bench` stopped by SIGTERM mid-stream (helper as of `99739f5`) | froze on run 5; the helper took ~420 ms to leave after SIGTERM |
| The same with `543dc0e` (SIGTERM/SIGINT blocked for life, checked between frames; the app's grace 1000 ms) | **froze on run 19**, after 18 clean runs; the helper left on its own 430-480 ms after SIGTERM every time |

So the signal fix in `543dc0e` did **not** remove the freeze: its commit
message states a cause that this last run disproves (or at best shows was
not the whole of it). The change is kept because it is sound on its own - a
stop never interrupts a KPU or AI2D wait, and the app no longer SIGKILLs a
helper that is still closing the camera - but it is not the fix.

What the counts do and do not show: runs stopped part-way froze the unit 2
times in 24; runs to completion 0 times in 30; Camera 0 in 40. With numbers
this small the difference is suggestive, not proven. Every freeze is the
same silent one as the known camera lock-up (docs/KNOWN_ISSUES.md), in the
vendor vvcam/ISP stack, with nothing on the console to follow; finding its
cause needs kernel-side debugging (lockup detectors, sysrq, a JTAG probe)
or a vendor fix, and every attempt costs a power cycle at the bench.

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
