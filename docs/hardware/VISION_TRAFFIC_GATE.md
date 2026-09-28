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

## Result

To be filled in on the bench: the build's identity (branch tip and the
two md5s), rotation at the start, PASS/FAIL per step with the evidence
class (LIVE-READ, OWNER), the numbers of step 14, and anything found.
