# Vision TRAFFIC mode - hardware gate

Branch `feat/vision-traffic` (from v0.2.1, `c9c2105`). The tracker and the
counting line hardened, and the TRAFFIC mode (docs/apps/VISION.md, "Traffic
mode"; DS §38.7 PROPOSED). Unit B has the session's priority; unit A is
not needed.

The camera lock-up of docs/KNOWN_ISSUES.md applies unchanged: every
camera open and close is counted, the console is logged when it can be,
and a freeze costs the owner a power cycle. Nothing here reflashes: the
helper and the shell are hand-installed over the deployed userspace with
a rollback script, exactly as the Vision prototype's gate did.

## Before

```sh
cat /etc/doors-release | head -3                      # the build under test
md5sum /usr/bin/pos-vision /usr/bin/doors-shell       # the hand-installed pair
ls /root/rollback-vision-traffic/                     # RESTORE.sh, the originals
ls -l /usr/share/doors/vision/yolov8n.kmodel          # the model (in the image since v0.2.1)
```

## The gate

Functional validation on a controlled moving target (the owner walking,
a hand-held object, a phone playing a video of traffic held in front of
the lens) is what a bench can do; **it is not real-world traffic
accuracy**, which needs a roadside session with counted vehicles and a
reference speed, and is recorded separately when it happens.

| # | Step | Pass when |
| --- | --- | --- |
| 1 | Open Vision (DEVICE, the eye) | "Starting", the live picture within 2 s, DETECT mode: MODE, LINE, RESET buttons; status line with fps and KPU ms |
| 2 | MODE -> TRAFFIC | five buttons (MODE reads TRAFFIC, LINE, SPEED: OFF, DIST: 10 m, RESET); three status lines; counters `IN (DOWN) 0` / `OUT (UP) 0`; the picture still dominant |
| 3 | Detection | a person in view is boxed `#N person v 8x%` (the mark once they have moved); a chair or a cup is never boxed in TRAFFIC (it is in DETECT) |
| 4 | Stable ids through short dropouts | the id stays while the person moves, turns, and steps behind an obstacle for under half a second; a new id only after a longer absence |
| 5 | Directional counting | LINE: ACROSS; walk down through the line and back: IN 1 then OUT 1, `person 2` in the status; LINE: DOWN, walk left-right-left: two counts, `LEFT`/`RIGHT` named on the counters |
| 6 | No double-counting around the line | stand on the line and sway, step across and straight back within a frame or two: no count; each real pass counts exactly once (ten passes, ten counts) |
| 7 | Two-line calibrated speed | SPEED: WIDE, DIST: 2 m (or the real distance between the two lines on the floor, measured with a tape): walk through both lines at a steady pace timed with a stopwatch; the box says `km/h` after the second line, the status says the same speed as current and last; compare with distance / stopwatch x 3.6 - within 15 % is PASS for a walk; a turn between the lines gives no speed |
| 8 | Refusals | walk to the first line and back: no speed; cross the two lines in opposite directions (impossible without recrossing) is not needed - the refusal counter in the `traffic` line (`rejected`) rises on the turn |
| 9 | RESET | counts, per-class counts and speeds to zero, ids start over |
| 10 | Rotate | Settings -> landscape (or the keyboard base) and back: the wide layout with the picture at full height, the column with two counters side by side and rows of two buttons; back to portrait |
| 11 | Repeated open/close | five cycles of open Vision, TRAFFIC, 5 s, back: a picture and boxes every time, no helper left (`pidof pos-vision` empty); bounded because of the lock-up |
| 12 | Exit Vision, open Camera | preview and a photo as before |
| 13 | Services | `doors shell info` answers; sysd, netd, radiod, meshcored (if enabled) up; nothing new in `pos system crashes` |
| 14 | Measured | with a person in view for a minute in TRAFFIC: fps and KPU ms from the status; `top -d 2`: `pos-vision` %CPU, `doors-shell` %CPU; `VmRSS` of both; the active track count |

## Result on unit B, 2026-09-28 (autonomous session, no owner at the bench)

**Unit B carried the v0.2.1 image (`BUILD_ID=9dc66c2`, freshly flashed:
no earlier rollback directory, a new host key) at the start, in landscape
under automatic rotation.** The gate hand-installed the branch's helper
and shell over the image's own (the originals in
`/root/rollback-vision-traffic/`, `RESTORE.sh` puts them back):

| Build | pos-vision md5 | doors-shell md5 |
| --- | --- | --- |
| `790fe87` (the first cut) | `e69d8242ace…` (riscv64, nncase, 0 warnings) | `eb5f6a5304…` |
| `71944ca` (speed-line fill fix, shell only) | same | `d52dee4692…` |

The room was dark: every picture in this gate is black but for sensor
noise, so **nothing was detected, tracked, counted or timed on the
hardware**. Detection, tracking, counting and speed are validated on the
host against the real helper with the fake camera and detector
(`tests/vision_session_test.c`, `tests/vision_track_test.c`,
`tests/vision_traffic_test.c`) and are **NOT validated on real traffic or
a real moving target** - that needs an owner at a lit bench (steps 3-8)
and is recorded below when it happens. What the bench could do:

| # | Step | Result |
| --- | --- | --- |
| - | `pos-vision probe` | PASS (LIVE-READ): `camera v4l2 preview 640x360 mount 90`, `model yolov8n.kmodel backend nncase input 320x320 classes 80 rows 2100` |
| - | `pos-vision bench 60` | PASS: 60 frames in 4.1 s, 14.6 fps with the first frame's start-up, mean AI2D 1.1 ms, **KPU 17.7 ms**, decode + NMS + track 7.1 ms; 0 boxes (dark) |
| 1 | Open Vision, DETECT | PASS (LIVE-READ, `caps/p1-detect-landscape.png`): live picture within 2 s, `23.1 fps  KPU 20 ms  pre 1  post 8  CPU 39%  5 MB`, DETECT / LINE: ACROSS / RESET in the column, the counting line drawn |
| 2 | MODE -> TRAFFIC | PASS (`caps/p2-traffic-landscape.png`): five buttons (TRAFFIC, LINE: ACROSS, SPEED: OFF, DIST: 10 m, RESET), three status lines (`27.4 fps  KPU 23 ms  0 tracks  0 total` / the six classes / `SPEED off  (10 m)`), `IN (DOWN) 0` / `OUT (UP) 0`, the picture 802 x 452 |
| 3-4 | Detection, ids through dropouts | NOT TESTED on hardware (dark room, no target); host-validated |
| 5-6 | Directional counting, no double-count | NOT TESTED on hardware; host-validated (track and session suites) |
| 7-8 | Two-line speed, refusals | NOT TESTED on hardware; host-validated (traffic and session suites) |
| - | SPEED: NARROW / WIDE, DIST, LINE: DOWN | PASS (`caps/p3-speed-narrow.png`, `p4-speed-wide.png`, `p5-dist-15.png`, `p6-line-down.png`): each tap changed the button and the status; **the speed lines did not draw on `790fe87`** (the chip role carries no fill opacity) - fixed in `71944ca`, after which they draw in both orientations (`caps/p10-portrait-speed-wide.png`, `p11-portrait-line-down.png`) |
| 9 | RESET | PASS (`caps/p7-reset.png`, `p12-portrait-reset.png`): counts stay 0, ids start over (nothing to count) |
| 10 | Rotate | PASS: `shell.rotation mode=portrait` restarted the shell into portrait; Vision opened with the tall layout (`caps/p8-portrait-detect.png`: three buttons in a row; `p9-portrait-traffic.png`: 3 + 2), the picture 436 x 776; back to `mode=automatic` (landscape) at the end |
| 11 | Five open/close cycles (TRAFFIC each time) | PASS: a helper each time, gone after leaving, `/dev/video2` held by `isp_media_server` alone afterwards, the shell's pid unchanged (no restart), **no freeze** in this gate's 10 camera opens (bench, probe, 5 cycles, Camera, the timed run, the rotation runs) |
| 12 | Camera afterwards | PASS (`caps/p13-camera-after.png`): `pos-camera` up with a live (dark) picture, closed cleanly |
| 13 | Services | PASS: sysd, netd, radiod, meshcored and doors-shell up by pid; no helper left; **0 ERROR lines** in the shell log |
| 14 | Measured, TRAFFIC, 60 s, no tracks | `pos-vision` **51 % CPU, 5.6 MB RSS**; `doors-shell` 13 %, 17.2 MB; CPU 62 % usr overall; status 25-28 fps, KPU 18-29 ms (`caps/p14-traffic-60s.png`) |

So: the mode, its layout in both orientations, every control, the lines,
reset, the cycles, Camera afterwards, the services and the cost are
LIVE-READ PASS on unit B; **the counting and speed logic on a real
target is still to be shown**, on the steps above, by the owner.

### The pixel modes (`85c7383`: pos-vision `59fbf7c852…`, doors-shell `b7a3c43256…`)

Landscape, the same dark room, from DETECT through TRAFFIC into each mode
by MODE taps (`caps/q1-color.png` .. `q8-back-to-detect.png`):

| Mode | Result |
| --- | --- |
| COLOR | PASS (LIVE-READ): `Tap the picture or SAMPLE` at first; SAMPLE took `#010003` from the dark middle and painted 99.9 % of the picture green with the accent mark at the centroid (391, 220); TOL: HIGH the same; status `10.0 fps  5 ms` (the pass) |
| EDGE | PASS: a black picture of edges, `8.9 fps  13 ms  edges 0.0%  soft`, HARD the same; `pos-vision` **20 % CPU** (the KPU idle), `doors-shell` 4 % |
| TRACE | PASS: `8.9 fps  6 ms  no dark line  (0 rows)`, LINE: LIGHT the same |
| Back to DETECT | PASS: boxes and lines back, one helper (6.0 MB RSS), gone after leaving, 0 ERROR lines |

What the pixel passes find on a lit scene (a coloured object, edges, a
tape on the floor) is not shown here; the passes themselves are held by
`tests/vision_pixels_test.c` on synthetic pictures.

The unit was left on `85c7383`'s helper and shell in automatic rotation
(landscape), at home; `/root/rollback-vision-traffic/RESTORE.sh` restores
the v0.2.1 pair.

## Real-target gate on unit B, 2026-09-29 (the owner walking, lit room)

Landscape, TRAFFIC, LINE: DOWN (the vertical line at the middle of a
doorway; the counters read `IN (LEFT)` / `OUT (RIGHT)`), the owner walking
round trips between the curtain side (left of the picture) and the door
side (right). Every run below was read frame by frame: `vlog.py` in
`out/vision-traffic-gate/` drives `pos-vision session` the way the shell
does and logs every `det`, `count` and `traffic` line with the helper's
timestamps (`vlog-cross*.log`, `analyze.py`); the Vision screen was
closed for those runs. Evidence class LIVE-READ throughout.

| Run | Helper | Passes | Counted | What the log showed |
| --- | --- | --- | --- | --- |
| screen, `85c7383` | `59fbf7c8…` | 5 round trips = 10 | **1 / 2** (LEFT / RIGHT) | one id per pass, no dropouts (e.g. 25 sightings left, 7 on the line, 25 right) and still no count: the person's box widened to 687 of 802 px near the lens, so the box-scaled dead band (a quarter of the smaller side, 109 px) was wider than the centre ever moved past the line. **Defect 1: the band needs a cap from the frame** |
| 3 | band capped (frame/16) | 10 | 4 / 4 | 8 of 10; lost: one pass with a single clear sighting 41 px past the line before the person left the frame (**defect 2: one decisive sighting must count**), one where the detector merged the owner with an orange coat on a chair into one 538-637 px "person" box born on the far side (the detector, not the tracker) |
| 5 | + one far sighting settles | 10 | **3 / 7** | right-to-left passes lost to the coat merge (3 of 5); two double counts where a partial box and the whole box both crossed (**defect 3: nested duplicates**) |
| 6 (coat removed) | + nested boxes dropped | 10 | **9 / 6** | every pass counted, but three passes counted three times within 200 ms: the detector's box flipped for a frame or two to a box pinned at the picture's left edge (x = 0, 580-660 px wide, 2.4 times the area) and back, moving the centre 270 px; the flip box was matched into the walker's track (**defect 4: a match must be of the track's size**) |
| 7 | + size gate (2x frame to frame, 3x coasting) | 4 round trips = 8 | **4 / 3** | **no double count**: every counted pass counted exactly once, the merge boxes live as their own short tracks that never cross. One pass lost: as the walker crossed, its id teleported onto a static box at the left edge - **defect 5, a tracker bug**: a track expiring in the coasting loop shifted the following tracks down but not the prediction table, so the next coasting track took the expired one's box |

Defects 1-5 are fixed on the branch after `eddf4d5` (commits below), each
with a test reproducing the logged sequence; defect 5's test fails on the
old line (mutation-checked). **The build with all five fixes
(`65f6bf5a…`) is installed on unit B but was not run against the owner:
the owner ended the session after run 7.** So on hardware the best
observed result is run 7: 7 of 8 passes, 0 double counts, the eighth
lost to the bug then fixed.

| # | Step | Result |
| --- | --- | --- |
| 3 | one stable id through movement | PASS (LIVE-READ): `#15 person > 69%` held over a 9 s burst standing and turning (`caps/g2-track-*.png`); in the logs every pass is one id with no gap over 120 ms (e.g. run 7 id 1: 91 sightings, 2651-5650 ms) |
| 4 | 5 left-to-right, 5 right-to-left | **NOT PASSED as specified.** Best run 7: 8 passes, 7 counted, 0 double counts, with the fix for the lost pass installed afterwards and unverified on the target |
| 5 | pause on the line, no count | PASS in the logs where it occurred (run 3 id 1: 7 sightings in the band, no count; run 6 sway rows) - not a separate scripted step |
| 6 | short disappearance, same id | PASS in passing: no pass had a detection gap over 120 ms; the 15-frame coasting was exercised by the flip frames (the walker's own box came back to its id in run 7, ids 1, 3, 11, 13, 15, 19) |
| 7-9 | two-line speed | **NOT TESTED** on the target (the owner ended the session) |
| 10 | performance during tracking | `stats` in the logs: 30.0 fps through the helper (the screen closed), KPU 17-18 ms, AI2D 1 ms, post 6-9 ms, helper 46-47 % CPU, 5.4 MB RSS; with the screen open (run 1 captures): 25-28 fps, KPU 22-30 ms |
| - | exit Vision, Camera, services | PASS: Camera live afterwards, sysd/netd/radiod/meshcored/doors-shell up, only `isp_media_server` on `/dev/video2`, no helper left, 0 ERROR lines |

Detector behaviour seen, outside Doors' code: two boxes for one person
(a partial beside the whole), a box merging the person with a coat or
with whatever sits at the picture's left edge (pinned at x = 0), and
widths flipping by 2-3x between frames. The tracker and the counter now
tolerate these as far as the logs show; they do not cure them.
