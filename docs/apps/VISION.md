# Vision

The camera's live picture with what the K230's KPU sees in it: a box, a
class and a confidence around every object a YOLOv8n detector finds, a
persistent id on every object as it moves, and one line across the picture
that counts what crosses it, each way.

Status: **merged and released in v0.2.0 / v0.2.1** (the model in the image
for internal use, docs/LICENSING.md item 10). **TRAFFIC mode on branch
`feat/vision-traffic`** (2026-09-28, from v0.2.1): the tracker and the
counting line hardened against weak detections, and a second mode that
counts traffic per class and measures speed between two lines - see
"Traffic mode" below, and docs/hardware/VISION_TRAFFIC_GATE.md for its
hardware gate. The prototype's own history: host-tested end to end on the
fake camera and a fake detector;
built for riscv64 with the pinned Xuantie toolchain against the nncase 2.11
runtime in the pinned SDK's sysroot, and through the pinned Buildroot flow.
**Hardware gate on unit B, 2026-09-28** (docs/hardware/VISION_GATE.md):
live preview, model load, KPU inference (17-20 ms, 28-29 fps through the
helper, 23-28 fps with the app open), boxes on real people, ids carried
through line crossings, LEFT/RIGHT counting, Camera working afterwards, no
crashes or leaked helpers - all pass. **Caveat accepted by the owner:**
stopping Vision can reach the known camera lock-up (a whole-unit freeze, a
power cycle to recover) far sooner than Camera does (docs/KNOWN_ISSUES.md).
The camera path is Camera's own (ADR-006), reused, not redesigned; the
launcher place and the icon are for the owner to confirm (DS §38,
PROPOSED).

**Branch `feat/vision-next` (2026-09-29, from master 8063533):** the modes
in groups behind a picker, per-mode settings kept between opens - see
"Modes and settings" - and the work that follows on it.

## Modes and settings

MODE opens a picker over the picture (DS §38.9) with the modes in groups:

| Group | Modes |
| --- | --- |
| GENERAL | DETECT (every object the detector knows, boxed), TRACK (the same with ids and a counting line) |
| ROAD | TRAFFIC |
| PEOPLE | FACE, RECOGNIZE |
| TEXT | READ |
| TOOLS | COLOR, EDGE, LINE TRACE |

A mode appears only when the helper can run it: right after `ready` it
says so in a `caps` line (the modes by their protocol words). A mode whose
model is not on the unit is never offered, and a group with nothing to
offer is not shown. A stored mode the helper cannot run falls back to
DETECT for this open and stays the stored wish, so it comes back with the
model.

DETECT and TRACK run the same pipeline in the helper (`mode detect` and
`mode track`); DETECT shows class and confidence, TRACK adds the ids, the
counting line and its counters. TRAFFIC's choices - its count line, its
speed lines and the distance between them - are its own, in TRAFFIC's
SETUP sheet; TRACK's line is a separate setting.

**Stored settings.** `$POCKETOS_STATE_DIR/vision/settings.v1` (default
`/var/lib/pocketos/vision/`), settings.conf's `key=value` format, written
by `apps/vision/vision_store.c` (the only file of the app that touches the
filesystem) whenever a choice changes, atomically (temporary file, fsync,
rename), 0600 in a 0700 directory. Merely opening Vision writes nothing.

| Key | Values |
| --- | --- |
| `mode` | `detect` `track` `traffic` `face` `recognize` `read` `color` `edge` `trace` |
| `track.line` | `off` `across` `down` |
| `traffic.line` | `off` `across` `down` |
| `traffic.orient` | `across` `down` - the way the speed lines lie |
| `traffic.speed` | `off` `narrow` `wide` |
| `traffic.distance_cm` | 100 200 500 1000 1500 2000 3000 5000 |
| `color.tol` | `low` `med` `high` |
| `edge.hard`, `trace.dark` | `0` `1` |

A value this build could not have written leaves that setting at its
default (the rest still read); an unknown key is ignored; a file larger
than any this build writes is not read at all. `apps/vision/vision_settings.c`
is the format (pure C, `tests/vision_settings_test.c`).

For screenshots, `POCKETOS_VISION_SHEET=modes|setup` opens the screen with
the picker or the setup showing (`tests/vision_shell_test.sh`).

## Traffic 2.0 (feat/vision-next)

TRAFFIC's SETUP sheet has five rows: DETECTION RANGE (NEAR, NORMAL, FAR),
COUNT LINE, SPEED LINES, DISTANCE (between the speed lines) and SHOW
(LABELS, SPEEDS, TRAILS, each on or off). Every choice is stored
(`traffic.range`, `traffic.labels`, `traffic.speeds`, `traffic.trails`).

### Detection range

A pipeline preset, never a distance in metres: nothing is calibrated for
how far an object is (`core/pocketvision/vision_range.c`).

| Range | Confidence | Smallest box | Zoom pass | Confirm after | Kept unseen |
| --- | --- | --- | --- | --- | --- |
| NEAR | 0.35 | 1/8 of the picture's smaller side (45 px of 360) | no | 2 | 10 frames |
| NORMAL (default) | 0.35 | - | no | 2 | 15 frames |
| FAR | 0.35 | - | the centre, at the model's own resolution | 3 | 20 frames |

- **What limits the distance** is pixels: the 640 x 360 picture goes into the
  320 x 320 model at half size. FAR runs the model a second time on a window
  at the picture's centre of the model's own size (320 x 320: twice the
  pixels per object), with the AI2D engine cropping the frame already in its
  memory (`vision_kpu_infer_window`), and merges what it finds: a box cut by
  the window's edge is dropped (the full pass sees that object whole); a box
  that is the same object as a full-pass box (IoU 0.5, or 85 % inside, same
  class or class group) keeps the surer of the two; the rest are added.
- **FAR does not lower the threshold.** A lower threshold finds more of
  everything, the false boxes included. It confirms a track one sighting
  later instead, and keeps a far object (few pixels, flickering) longer.
- **NEAR** drops what is small. Its threshold was 0.45 at first; on the
  road picture below unit B's KPU scored the nearest white car at 0.44 and
  NEAR lost it, so NEAR keeps NORMAL's threshold and filters by size only.
- **The evidence** (unit B, 2026-09-29 night, the KPU through
  `pos-vision bench --image`, 20 frames each; the scenes are the vendor
  SDK's own test pictures, cover-fitted to the camera's 640 x 360 and
  never committed; the unit's own camera faced a dark room):

  | Picture | NEAR | NORMAL | FAR | Notes |
  | --- | --- | --- | --- | --- |
  | road (highway, cars to the horizon) | 1 | 4 | 5 | FAR found a 15 x 12 px car NORMAL scored 0.11, and raised two 25 x 15 px cars from 0.44 / 0.40 to 0.76 / 0.69 |
  | traffic (city street) | 2 | 10 | 12 | 3 traffic lights in every range but NEAR |
  | bus, car, person, empty street | same | same | same | the zoom pass's boxes all merged: no extra box, no false positive |

  FAR costs one more KPU run: inference 34.6 ms a frame instead of 17.6
  (decode 7-8 ms more), so about half the frame rate through the detector.
  The horizon's smallest cars (under ~10 px) stay invisible in any range:
  YOLOv8n at 320 does not see them.
- In TRACK and the other modes the pipeline is NORMAL's; the range is
  TRAFFIC's setting. Changing it drops the tracks (a track keeps the side
  of the line it was on, so nothing counted counts again) and any speed in
  progress; the counts stay.

### Recent statistics

The helper keeps the last five minutes of crossings (by direction and
traffic class) and speed measurements in a ring of 512 events
(`core/pocketvision/vision_window.c`): nothing grows with time or traffic.
It says them in a `recent` line after each event and once a second. When
more than 512 events happen inside five minutes, the oldest go and the
line says so; the screen then says `(latest only)` rather than a number
that is not the five minutes. The status in TRAFFIC:

    NORMAL  24.1 fps  KPU 18 ms  3 tracks
    5 min: 12  IN 7  OUT 5  avg 43.2 km/h (6)
    car 8  truck 1  bus 0  moto 1  bike 2  person 0

The two counters stay the totals since RESET (`IN (DOWN) 42`).

### Display

LABELS puts the id, class and direction on each box (`#7 car > 43%`),
SPEEDS the measured speed (`43.2 km/h`), TRAILS a dotted trail of where
each track has been (8 points, one every 6 px it moves, let go 8 det lines
after its track; `apps/vision/vision_trails.c`, fed from the det lines the
screen already gets). TRACK has its own TRAILS button.

### Tracking (Tracking 2.0)

Three changes to the tracker (`core/pocketvision/vision_track.c`), each
from a defect seen on a real sequence, and nothing more:

1. **No second track for one object.** A box left over after matching that
   is the same object as a track matched in this frame (IoU at least 0.5,
   or either 85 % inside the other, compatible class) is not made a track:
   the detector's partial box beside its whole one, or two boxes of one
   person just under suppression's 0.65. Counted in `dup_births`. Two
   people side by side below that overlap still get a track each.
2. **Found where it was last seen.** A third matching pass compares a
   still-unfound track with where it was last *seen* rather than predicted:
   an object that stopped or turned while hidden is found where it was
   instead of being lost to a prediction that drifted on. Its size gate is
   the track's own (2x from the last frame, 3x only once it has coasted), so
   the walker's box that flipped to one pinned at the picture's edge
   (2.4x, unit B 2026-09-29) still does not take the walker's id.
3. **A reach that grows while hidden.** The distance pass's radius grows by
   5 % of the box's larger side per frame unseen, from 75 % up to 150 % at
   most.

**Evidence** (unit B's KPU, `pos-vision bench --images ... --tracks`, the
vendor SDK's 278-frame street sequence of cyclists and cars, every second
frame: 139 frames at ~12 frames a second of the scene; analysed with the
gate's `trackstats.py`: confirmed ids, re-identifications - an id ending
and a new one of the same class group born nearby within 12 frames - and
same-frame duplicates, two confirmed tracks of one group at IoU > 0.5):

| Tracker | Range | Confirmed ids | Duplicate frames | Re-identifications | Short tracks | Median life |
| --- | --- | --- | --- | --- | --- | --- |
| before | NORMAL | 29 | 40 | 8 | 6 | 25 frames |
| after | NORMAL | 24 | 15 | 7 | 4 | 30 frames |
| before | FAR | 30 | 41 | 8 | 10 | 30 frames |
| after | FAR | 26 | 21 | 6 | 6 | 45 frames |

The duplicates left are, on inspection, two distant cars whose boxes
converge (both tracks 30 sightings old) and a car leaving at the picture's
edge; no ground truth exists for the sequence, so these figures compare
the tracker with itself, not with truth. `tests/vision_track_test.c` holds
each rule (a mutant of each is caught) and every earlier one.

### Speed

Unchanged in substance, now in SETUP: the speed is the distance between
the two speed lines over the time between a track's two crossings; only
the two timestamps and the configured distance make a speed, never how
fast a box moves on the picture. The distance must be measured on the
ground by the owner; the screen shows it with every speed.

## READ (text, feat/vision-next)

READ finds lines of printed text in the picture and reads them, on the
unit, with nothing sent anywhere. Each line gets a box and its words on
the picture; the words of every line, in reading order, stand under or
beside the picture; LINES and SURE (the mean confidence) are the counters.
HOLD keeps the last read on the screen while the camera moves on (READ
again goes back to live reading).

**Pipeline** (helper, `read_text()` in `tools/vision/pos_vision.c`): at
most one read every 600 ms, the preview going on between reads. The
upright picture goes to a DB-style text detector (a probability map), the
map is grouped into regions (`core/pocketvision/vision_text.c`: a grid of
at most 256 cells a side, 4-connected, threshold 0.3, box score 0.5, each
box grown by the DB unclip rule, reading order), and each of the six
surest regions, grown by a margin of a third of its height at each end
(less cut off the first and last characters), is cropped by AI2D straight
into a CTC recogniser; greedy CTC decoding with the dictionary's last
class as the blank. The line's text is sent percent-encoded; the app
shows characters outside ASCII as `?` because the UI font carries no
others. No word breaks: the recogniser's dictionary has no space, and a
gap heuristic was tried and dropped as unreliable.

**Models: not in the image and not in this repository.** READ is offered
only when all three files exist:

| File (default path, override) | Tried with | Shape (VERIFIED on unit B) | Size, time |
| --- | --- | --- | --- |
| `/usr/share/doors/vision/text_det.kmodel` (`POCKETOS_VISION_TEXT_DET`) | the canmv SDK's `ai_poc/kmodel/ocr_det.kmodel`, sha256 `b8a71660…7b79fc` | u8 `[1,3,512,512]` -> f32 `[1,512,512,2]` | 2,958,504 B, 84-109 ms |
| `/usr/share/doors/vision/text_rec.kmodel` (`POCKETOS_VISION_TEXT_REC`) | `ai_poc/kmodel/ocr_rec_int16.kmodel`, sha256 `7a307f86…aa8648c` | u8 `[1,3,32,512]` -> f32 `[128,1,6549]` | 13,008,216 B, ~67 ms a line |
| `/usr/share/doors/vision/text_dict.txt` (`POCKETOS_VISION_TEXT_DICT`) | `ai_poc/utils/dict_ocr.txt`, sha256 `8288453b…a74c8fb` | 6549 entries, one a line; the blank is the class after the last | 32,521 B |

The pairing matters: the SDK's other recogniser, `ocr_rec.kmodel`
(`[1,152,6625]`), does not match this dictionary and reads garbage. The
architecture is ASSUMED to be PaddleOCR's (DB detector, CRNN/SVTR-style
recogniser with CTC; Apache-2.0 upstream), from the shapes and the
dictionary; the SDK states neither the source weights nor the conversion
settings, and carries **no licence statement** for these files. They are
therefore not embedded or packaged (docs/LICENSING.md item 11); for the
gate they were linked into place on unit B from `/tmp`.

**Measured on unit B** (2026-09-29, KPU): a five-line read of a printed
sign picture 480-540 ms end to end (detector ~100 ms, ~67 ms per line);
through the app, helper 20-33 % CPU (one core of two), shell unchanged,
CMA back to its idle level after close (the text nets release the KPU
memory pool when the last net closes - an earlier build leaked it, fixed
before this gate). Results: `EXIT12`, `SERIALNO4711-AB`, `DOORS`, `K230`,
`Parking08-18` from the sign picture (SURE 98 %); the vendor's Chinese test
photo read exactly (shown as `?`); the live camera in a dark room reads
nothing and says so. Missing models: READ is not in the picker. A model
that fails to load: `readfail`, and the screen says "Cannot read:" with the reason.

### Replay (recorded pictures)

For checking a mode on the real display and KPU with a known scene, the
helper can play PPM pictures (P6, 8-bit) instead of the camera:
`POCKETOS_CAMERA_BACKEND=image` and `POCKETOS_VISION_CAMERA_CONFIG=` a
comma-separated list of files, each shown for `POCKETOS_VISION_REPLAY_MS`
(default 100). The picture is shown as it is (never turned), labelled
SIMULATED, and every mode runs on it. A list that does not load is
`nodevice`. The shell passes its environment through, so on a unit the
two lines go in `/etc/default/doors-shell` for as long as the check runs.

## What it does

- **DETECT.** Opening the app starts the camera and loads the model. The
  live picture fills the width in portrait and the height in landscape, in
  the preview's own shape (640 x 360, turned upright as the unit is held).
  Every object the detector finds gets an outline, its class name and its
  confidence; the status line says the frame rate through the detector,
  the KPU's time per frame, the preprocessing and post-processing times,
  and the helper's CPU share and resident set.
- **TRACK.** An object seen frame after frame keeps one id (`#7 person
  83%`). An id appears once the object has been seen twice, survives up to
  fifteen missed frames (about 0.6 s) coasting on the object's smoothed
  motion, and is never reused. A detection the overlap cannot match is
  still the same object when it is of the same size and within three
  quarters of a box-side of where the track expected it (a dropout, or a
  jump); a spurious single box never grabs one that way. At most 32
  objects are tracked at once; more are dropped, never queued.
- **COUNT.** LINE cycles the counting line through ACROSS (horizontal, at
  mid-height: counts DOWN and UP), DOWN (vertical, at mid-width: counts
  LEFT and RIGHT) and OFF. A confirmed track counts once when its centre
  has settled clearly on the other side - clearly is a dead band of a
  quarter of the box's smaller side (at least 4 px, at most a sixteenth
  of the frame's smaller side, so a person filling the picture can still
  cross), settled is two
  sightings in a row, so neither a wobble on the line nor a one-frame jump
  across it counts - in the direction it went, and counts again only when
  it really comes back. RESET zeroes the counts and forgets the tracks.
- **TRAFFIC.** MODE switches to a second mode that looks for traffic only
  - car, truck, bus, motorcycle, bicycle, person - counts it per class and
  in total (IN and OUT across the counting line), says on every box which
  way it has gone, and measures a speed for every track that crosses two
  speed lines a known distance apart. See "Traffic mode".
- **States said in words.** "Starting" while the camera and the model open;
  "Waiting for the picture"; "Waiting for the camera..." for a stalled
  stream; "Vision stopped" with the reason and TRY AGAIN; "No camera or
  detector" with the reason and CHECK AGAIN. Under the fake backend the
  header says SIMULATED.

## What it is not

No segmentation, pose, recording, network or cloud. The lines are in a few
fixed places and the distance from a short list. The choices are stored
(above); the counts and speeds are not: they live as long as the screen
is open. No speed is ever inferred from how fast a box moves on the
picture.

## Traffic mode

MODE: TRAFFIC. What changes, all of it in the helper and the app's words:

- **Only traffic is tracked.** After suppression, detections whose class
  name is not one of `car`, `truck`, `bus`, `motorcycle`, `bicycle`,
  `person` are dropped before the tracker sees them, so a chair never
  takes a track slot or a count. The mapping is by the detector's class
  names (`vision_traffic_map_names`), not by COCO indices: another
  detector with the same names drops in.
- **A vehicle keeps its track when the detector changes its mind.** The
  tracker matches across a class group - car, truck and bus are one group,
  motorcycle and bicycle another, a person is only a person - and the
  track changes class only after three sightings of the other class in a
  row, so the label does not flicker.
- **Counts per class.** Every crossing of the counting line is counted for
  its traffic class and in total, in the direction it went: A to B (DOWN
  on ACROSS, LEFT on DOWN) is IN, the other way OUT. The counters show
  `IN (DOWN) 4` and `OUT (UP) 2`; the status shows `car 3  truck 1  bus 0
  moto 0  bike 0  person 2`.
- **Direction on the box.** Each confirmed box carries the way it has
  gone since it was first seen (`#7 car > 43%`: `<` `>` `^` `v` on the
  picture as the owner holds it), once the displacement is clearly more
  than jitter (half the box's smaller side, at least 16 px).
- **Speed between two lines.** SPEED cycles OFF, NARROW (two lines at 40 %
  and 60 % of the picture) and WIDE (25 % and 75 %), parallel to the
  counting line; DIST cycles the ground distance between them through 1,
  2, 5, 10, 15, 20, 30 and 50 m. A track that crosses line A and then
  line B (or B then A) in the same direction gets a speed: the distance
  over the time between the two crossings, from the helper's monotonic
  clock, `km/h = distance / seconds x 3.6` (kept as km/h x10, integer).
  The box then says `#7 car > 43.2 km/h`; the status says the current
  speed (while that track lives), the last, the highest and the mean.
- **What is refused, and counted as refused:** the same line crossed
  again before the other (it turned round), the two crossings in opposite
  directions, less than 100 ms between them (two boxes that swapped ids),
  more than 30 s (it stopped, or the id was replaced by a new track's), a
  track gone before the second line, a result above 300 km/h. A new id
  starts from nothing: it can never inherit the first crossing of the
  track it replaced.
- **Controls:** MODE, LINE, SPEED, DIST, RESET. RESET zeroes the counts
  and the speeds and forgets the tracks. Changing LINE, SPEED or DIST
  drops any measurement in progress and (for LINE) the counts.

Where it lives: `core/pocketvision/vision_traffic.c` (the class map, the
per-class counts, the speed slots - one per track, bounded by
`VISION_MAX_TRACKS` - and every refusal; pure C, integer, the clock fed
in, tested in `tests/vision_traffic_test.c`), used by the helper; the app
only carries the choices and the words (`vision_model.c`).

## Optional modes: COLOR, EDGE, TRACE

MODE cycles on past TRAFFIC. In these three the detector idles (the model
stays loaded; the KPU is not run) and the helper works on the preview
picture it has already drawn for the screen - in place, in the shared
memory slot, at the preview's own rate (at most ten a second) - before
saying `frame`. The core is `core/pocketvision/vision_pixels.c`: integer
only, no allocation (three static luma rows and a centroid per row,
bounded by the widest picture a slot holds), tested on synthetic pictures
in `tests/vision_pixels_test.c`.

- **COLOR.** SAMPLE takes the colour at the picture's middle; a tap on the
  picture takes it under the finger (a 5 x 5 mean). Every pixel within
  the tolerance (TOL: LOW 48, MED 96, HIGH 160 - the sum of the three
  8-bit channel differences) is painted green (magenta when the target is
  greenish, so it stays visible); the status says the target, the share
  of the picture that matched and the matches' centroid, which the screen
  marks with a small accent square.
- **EDGE.** Sobel on the luma: the picture becomes its edge magnitude in
  grey (SOFT) or black and white above a threshold of 40 (HARD); the
  status says the share of strong edges. No contours: a picture of edges
  is what the mode is for.
- **TRACE.** The dominant dark (LINE: DARK) or light line on the picture:
  per row, the centroid of the dark pixels when there is a plausible run
  of them (between 2 % and 60 % of the width), a least-squares line
  through the centroids, the offset of the bottom quarter's centroid from
  the middle (`line left 12%`), the lean (`leans right 45%`: dx per rows
  going down) and how many rows carry the line; the centroids are marked
  in yellow. A line needs an eighth of the rows. Meant for a tape on a
  floor in front of the unit, as a line follower sees it.
- **SHAPE is not implemented.** Circle, rectangle and triangle detection
  needs contours and polygon fitting, which is more than a pass over the
  picture; it is left for its own change.

The choices (mode, tolerance, soft/hard, dark/light) survive Try again
like the others; nothing is stored.

## The model

**YOLOv8n, 320 x 320, quantized, as `yolov8n.kmodel` from the pinned
vendor SDK** (`vendor/T-Display-K230/k230_linux_sdk/buildroot-overlay/package/yolo/utils/yolov8n.kmodel`,
3,495,296 bytes, sha256 in the gate sheet once measured). Why this one and
not another:

- the SDK ships it compiled for exactly this runtime (nncase 2.11.0, the
  `libnncase` package the image already builds) and a Linux program that
  runs it on the KPU with AI2D preprocessing (`package/yolo`), so every step
  of the pipeline has a vendor reference on this platform;
- its input is `[1, 3, 320, 320]` uint8 and its output `[1, 84, 2100]`
  float - the YOLOv8 detection head for 320 x 320 (40² + 20² + 10² rows,
  4 box values and 80 COCO scores each), which is what `vision_decode.c`
  reads; the helper refuses a model of any other shape;
- the SDK also carries `yolov5n`, `yolo11n` and `yolo26n`; YOLOv8n is the
  one asked for, and the one whose head the decoder is written for.

**The image carries the model; this repository does not.** The helper
reads it from `/usr/share/doors/vision/yolov8n.kmodel` (override with
`$POCKETOS_VISION_MODEL` or `--model`). The `pocketos` package installs it
there, copying the pinned SDK's own file at build time, so a freshly
flashed unit runs Vision with nothing added. The install refuses any file
whose sha256 differs from `tools/vision/yolov8n.kmodel.sha256`
(`0b4bcdd3…2004a09`). The model is licensed differently from Doors: the
vendor's `package/yolo` sources carry Canaan's BSD-style header, but the
kmodel is compiled from Ultralytics' YOLOv8n weights, whose licence is
AGPL-3.0, and the SDK names no terms for the file. The owner decided on
2026-09-28 to use it under the AGPL-3.0 **in internal images only**
(docs/LICENSING.md, item 10, which stays open for distribution outside the
project). Its notice and the licence text are in THIRD_PARTY_NOTICES.txt.
Doors' own code (the decoder, tracker, counter, helper and app) contains
nothing of the model. `tools/vision/install-model.sh <unit ip>` is still
there for a unit that lacks the file: userspace deployed by hand onto an
older image, a damaged file, or another model to try
(`POCKETOS_VISION_MODEL_FILE`).

The COCO class names are compiled in (`core/pocketvision/vision_labels.c`),
in the order the vendor's `coco_labels.txt` lists them.

## The camera and KPU pipeline

Everything below runs in the helper process, `pos-vision`, once per frame,
synchronously, in this order:

| Step | Where | What |
| --- | --- | --- |
| Capture | `core/pocketcam/pocketcam_v4l2.c` (Camera's backend, one new config key) | `/dev/video2`, the ISP's self path, 640 x 360, **planar `BG3P`** (`fmt=bg3p`): the format the vendor's KPU demos read, three 8-bit planes AI2D takes as they are - R, G, B in memory despite the driver's name (VERIFIED on unit B). Camera itself keeps NV16. |
| Upright | `vision_geom.c` (`vision_turn_planes`, `vision_box_unturn`), `pos_vision.c` | the camera is mounted turned (90 degrees), so the sensor frame shows the scene sideways (portrait) or upside down (landscape, screen up). The frame is turned clockwise by the preview's own turn (`pocketcam_view_rotation` of the mount and the display's rotation from `view`) into a buffer, so the detector sees the scene upright as the screen shows it, and the decoded boxes are turned back into frame pixels before suppression, so tracks, lines, counts and `det` boxes are unchanged. Measured on unit A's window scene (2026-09-29): the same frame found 0 of 7 parked cars as taken (upside down) and 7 of 7 at 59-73 % upright; the turn costs 2.4 ms in landscape and 4.6 ms in portrait, inference and frame rate unchanged. |
| Into AI2D memory | `vision_kpu_nncase.cpp` | the (upright) frame is copied (691 KB) into a tensor from the runtime's shared pool and written back from the cache. A V4L2 buffer cannot be handed to AI2D directly: the runtime needs a physical address it does not have for an MMAP buffer. With the turn this is the second copy of a frame in the pipeline (the vendor's demo makes one, of a 2.7 MB frame). |
| Preprocess | AI2D, hardware | resize into the top-left of 320 x 320 keeping the aspect (0.5 x: 320 x 180), pad the rest with 114 - the vendor's `padding_resize_one_side_set`. |
| Infer | KPU, through the nncase interpreter | one run; the first output mapped and its 2100 x 84 floats copied out. |
| Decode | `vision_decode.c` | per row: the best class score, the threshold (0.35), the box centre and size back through the letterbox ratio, clipped to the frame; NaN, infinities, empty and absurd boxes skipped and counted; a tensor of the wrong shape refused before a value is read. At most 256 candidates, the best kept. |
| Suppress | `vision_nms.c` | class-aware greedy NMS at IoU 0.65 (the vendor's default), at most 32 detections. |
| Nested | `vision_nms.c` | a box at least 85 % inside a larger box of its class is the same object seen twice (a partial box beside the whole one) and is dropped; unit B counted a walker twice without this. |
| Filter | `vision_traffic.c` | in TRAFFIC mode only: detections of a class with no traffic name are dropped here. |
| Track | `vision_track.c` | greedy IoU matching (0.2) against each track's prediction (last box + smoothed motion), same class or same group, and an area within 2x of the track's (3x after a dropout: a box that suddenly spans half the picture is another object, or a merge); then a distance pass for confirmed tracks the overlap lost (same size, within 3/4 of a box side); new ids for the unmatched; coasting with decaying motion, expiry after 15 misses; confirmation after 2 sightings; 32 tracks at most. Frame pixels throughout. |
| Count | `vision_line.c` | the lines, chosen on the picture, unmapped into frame pixels (`vision_geom.c`) and checked against every seen track's centre, with the dead band and the two-sighting settle; crossings reported by id. |
| Traffic | `vision_traffic.c` | the count line's crossings per class; the speed lines' crossings timed per track. |
| Say | `pocketvision_proto.h` | one `det` line per frame with every shown track mapped into picture pixels (`vision_geom.c`, the same turn, mirror and cover-fit the converter draws with), its direction and speed; `count` and `traffic` when they change; `stats` once a second. |
| Preview | `core/pocketcam/pocketcam_convert.c` | at most every 100 ms, the frame turned and scaled to RGB565 at the picture's size into shared memory, as Camera does. |

Frames that arrive while a frame is being processed wait in the driver's
four buffers or are dropped by it; the helper never queues them, so it
never falls behind. The frame rate through the detector is therefore
1 / (copy + AI2D + KPU + decode + track), whatever the sensor does.

The stream is Camera's: opened the same way, closed and reopened rather
than restarted (the lock-up of CAMERA_GATE.md), never touched by the app.
Whether the ISP's self path delivers `BG3P` at 640 x 360 - the vendor asks
it for 1280 x 720 - is UNKNOWN until unit A; `--config` (or
`$POCKETOS_VISION_CAMERA_CONFIG`) can name another node, size or format
without a rebuild, and `pos-vision probe` says what it got.

## Architecture

One helper per screen, as Camera (ADR-006): the app never opens the camera
or the detector; `pos-vision` does, for exactly as long as the Vision
screen is open, and leaves with the shell (PR_SET_PDEATHSIG). Camera's
helper and Vision's are never alive together - each lives only while its
own screen is - so the sensor has one owner at a time, and the driver
refuses a second opener anyway (EBUSY at REQBUFS, CAMERA_PLATFORM_RESEARCH.md
U8). Vision is the "second user" ADR-006 names as a trigger to revisit
`camerad`, but not a concurrent one; the decision stands as written.

| Part | Where | Role |
| --- | --- | --- |
| `vision_app.c` | apps/vision | the screen (LVGL): the picture, 24 outline objects made once and moved, the line, the counters, LINE / RESET / TRY AGAIN; polls the session every 33 ms |
| `vision_model.c` | apps/vision | the state machine and every word on the screen; pure C |
| `vision_layout.c` | apps/vision | the two shapes; pure arithmetic |
| `vision_session.c` | apps/vision | starts `pos-vision`, the shared memory, the line protocol, the watchdog (hello 3 s, open 15 s, silence 4 s, reply 3 s); pure C |
| `pocketvision.h` | core/pocketvision | the types and every bound |
| `vision_decode.c`, `vision_nms.c`, `vision_track.c`, `vision_line.c`, `vision_geom.c`, `vision_labels.c` | core/pocketvision | the pipeline after the detector: plain C, no allocation, no clock, no I/O, integer but for the tensor's floats |
| `vision_kpu.h` | core/pocketvision | the detector's C interface |
| `vision_kpu_nncase.cpp` | core/pocketvision | the KPU through nncase and AI2D: the only C++ in the Vision path and the only file in Doors that includes an nncase header |
| `vision_kpu_fake.c` | core/pocketvision | the host stand-in: a tensor of the detector's shape from a script of boxes |
| `pocketvision_proto.h` | core/pocketvision | the helper protocol (Camera's transport and shared memory, Vision's lines) |
| `pos_vision.c` | tools/vision | `pos-vision session`, `probe` and `bench` |

`pos-vision bench` also lists every vehicle candidate from 0.10 with its
size in frame and model pixels and whether the threshold takes it, and
ends with a tally. `--turn R` turns the frame upright as the session does
(bench has no screen to take the rotation from: landscape with the screen
up is 180, portrait 90); `--save FILE.ppm` writes frame 60 - once the auto
exposure has settled - with that frame's vehicle boxes drawn in (green at
the threshold, red below), `--save-every N` one more every N frames;
`--image FILE.ppm` feeds a saved picture to the detector instead of the
camera, so one scene can be compared as taken and changed.

Pictures travel as Camera's do: a sealed memfd of four 1024 x 1024 RGB565
slots, the helper writing a slot it owns, the app copying the newest one
out and releasing it in the same timer tick. Boxes travel as text, in
picture pixels, so the app draws them with no geometry of its own.

### The protocol

`core/pocketvision/pocketvision_proto.h` is the reference (version 2).
Commands: `view w h rotation`, `start`, `stop`, `release slot`,
`mode detect|track|traffic|color|edge|trace`, `line x0 y0 x1 y1` (per-mille of
the view) or `line off`, `speed ax0 ay0 ax1 ay1 bx0 by0 bx1 by1` or
`speed off`, `distance cm`, `range near|normal|far`, `color r g b` or `color off`, `sample x y`,
`tol n`, `edge threshold`, `trace dark|light`, `reset`, `quit`. Events:
`color r g b matched_pm cx cy`, `edge strong_pm`,
`trace found offset_pm slope_pm rows` (the pixel modes, with every
preview), `hello`,
`ready camera pw ph simulated model in_w in_h classes`, `caps mode...`, `nodevice`,
`nomodel`, `error what text`, `frame slot seq w h`,
`det seq n id:cls:conf:x:y:w:h:dir:kmh10...` (dir 0 none, 1 left, 2
right, 3 up, 4 down on the picture; kmh10 the speed measured on that
track or 0), `count ab ba`,
`traffic ab ba cur last max mean n rejected c0ab:c0ba ... c5ab:c5ba`,
`recent window_s crossed ab ba c0 .. c5 speeds mean_kmh10 saturated`,
`stats fps_x10 infer_ms pre_ms post_ms cpu_pct rss_kb bad dropped`,
`malformed n`, `stall ms`, `stopped`, `lost`, `bye`.

### Malformed model output

A tensor that is not `[1, 4 + classes, rows]` for the model's declared
input is refused before a value is read; one whose values are not numbers
is read row by row, each bad row skipped and counted. A frame where more
than half the rows are bad is said as `malformed` and otherwise ignored;
ten such frames in a row end the session with `error model ...` and exit
5, which the screen shows as "The detector stopped making sense" with TRY
AGAIN. A model of the wrong shape never opens (`nomodel`).

## Bounds

| What | Bound |
| --- | --- |
| Candidates after decode | 256, the best kept |
| Detections after NMS | 32 |
| Tracks | 32; extra detections dropped and counted |
| Line states per track | 3 (the count line, speed lines A and B) |
| Speed measurements in flight | 32 slots, one per track; a crossing with no slot is refused |
| Boxes on one `det` line, and outline objects on screen | 24 |
| Protocol line | 2048 bytes (24 boxes of at most 52 characters) |
| Preview slots | 3 + 1 unused, 2 MiB each (Camera's) |
| Model input | 4096 px a side (a corrupt tensor cannot make a larger box) |

Nothing is allocated per frame anywhere in the helper or the app after
open; the fake detector allocates its tensor once.

## The fake detector

`--kpu SCRIPT` (or `POCKETOS_VISION_KPU_SCRIPT`), comma-separated, on the
host build only (the device build links nncase and has no fake):

| Key | Effect |
| --- | --- |
| `in=WxH`, `classes=N` | the pretend model (default 320 x 320, 80) |
| `box=CLS:CONF:X:Y:W:H[:DX:DY]` | a detection in frame pixels, per-mille confidence, moved by (DX, DY) a frame; up to 16 |
| `dup=N` | every box N more times, shifted a pixel (suppression's work) |
| `until=N` | an empty scene after frame N |
| `malformed_at=N`, `bad_from=N` | NaN on frame N, or on every frame from N |
| `shape_at=N` | frame N claims the wrong shape |
| `fail_at=N` | frame N's run fails |
| `delay=MS` | pretend inference time |

The camera fake is Camera's (`docs/apps/CAMERA.md`), with one new key,
`format=bg3p`.

## Layout

DS §38 (Amendment V, PROPOSED). Fullscreen (NONE chrome). Tall: the
picture across the width in the turned preview's shape (9:16), then the
status line, two counter slabs side by side, LINE and RESET side by side.
Wide: the picture at the full height (16:9) on the left, a 240 px column
on the right with the status (two lines), the two counters and the two
buttons stacked. TRY AGAIN takes LINE's place. Only existing roles: a slab
behind the picture, the §9 focus ring as the box outline, captions on
slabs as the box tags, `POS_STYLE_VALUE` on slabs for the counts, §7
buttons.

## Tests

Host only; none needs unit A. `make vision-test` runs them all,
`make vision-san-test` again under ASan and UBSan.

- `tests/vision_decode_test.c` (31 checks): the head's row count; a
  tensor built the way the head lays it out decodes to the boxes put in,
  letterbox undone, clipped at the edges; the wrong shape, the wrong size,
  NaN, infinities, empty and absurd boxes, impossible scores; a tensor of
  NaN; the candidate bound keeping the best; NMS merging, ordering, class
  separation and bounds; IoU.
- `tests/vision_track_test.c` (67 checks): one id across frames and across
  motion, a jump across the frame is a new object, another class is a new
  object, expiry after exactly `VISION_TRACK_MAX_MISSES`, ids never
  reused, the list bounded with drops counted; a five-frame dropout and
  an object that stopped while unseen keep their id; reacquisition by
  distance and its limits (size, radius, confirmed only); a new object
  after an expired track; the class groups and the three-sighting vote;
  the line's sides and the box-scaled dead band; a crossing counted once
  in its direction, the way back counted the other way, a touch, a wobble
  and a one-frame jitter never counted, an unconfirmed or newborn track
  never counted, two objects opposite ways and three the same way, a
  prediction across the line not a crossing and the object found again
  beyond it counted once, an expired track and a newcomer beyond the
  line one count, left/right and up/down on both line orientations, two
  lines with independent states, forgetting, a disabled line.
- `tests/vision_pixels_test.c` (26 checks): RGB565 both ways and the
  luma; a colour sampled, its matches painted and located under a tight
  and a wide tolerance, a green target painted magenta, no match, the
  tolerance's ceiling; a vertical and a horizontal step edge found on
  their rows and columns alone, grey and thresholded, a flat picture
  edgeless; a leaning dark line traced with its lean and offset and its
  centroids marked, a straight light one, a blank, an all-dark picture, a
  short dash; pictures too small or too wide refused.
- `tests/vision_traffic_test.c` (39 checks): the class names and the
  groups, filtering, per-class and total counts by direction, reset; a
  speed from A then B and from B then A, the current speed retired with
  its track, last/max/mean; every refusal (the same line twice, opposite
  directions, too short, stale, gone between the lines, a replaced id, an
  impossible result, the distance's bounds, a changed distance); 32
  tracks at once with a slot each and the 33rd refused; the mean's count
  saturating.
- `tests/vision_geom_test.c` (24 checks): every mapping held against
  `pocketcam_to_rgb565()` by painting one pixel and finding it, for all
  four turns, mirrored and not, scaled and cut on either axis; boxes
  clamped or reported outside; the way back for the line.
- `tests/vision_model_test.c` (88 checks): every state's words, the line
  modes and direction names, stats and counts from the session, the two
  modes and their buttons, the speed lines in both orientations, the
  distances, the traffic report's words, every failure's sentence, the
  choices kept across Try again; both layouts in both modes on the
  reference panel with every control inside the safe box, at least the
  touch minimum, not overlapping, the picture in the frame's shape.
- `tests/vision_session_test.c` (78 checks) against the real helper on
  the fake camera and the fake detector: every event line parsed and the
  malformed ones refused (the nine-field box, the traffic report); a
  picture through the shared memory; two objects tracked with ids that
  persist across thirty frames; the person walking down the picture
  counted once, downward; in TRAFFIC a car through both speed lines and
  the count line on one id with its direction, counted IN as a car and
  measured against the helper's own clock, the chair never tracked,
  reset, and the chair back in DETECT; the pixel modes on the fake
  camera's picture: EDGE empties the boxes and says its edges, soft and
  hard, pictures keep coming, COLOR says nothing without a target and a
  report once the middle is sampled, TRACE answers, DETECT tracks the box
  again; stats; reset; stop; two
  malformed tensors said and survived; a detector giving nonsense ended
  with exit 5; a failed run; no camera, a busy camera, a bad detector
  script, a missing helper, a camera that goes away, a hung helper killed
  by the watchdog, a crashing one; thirty opens and closes with no
  descriptor or child left behind.
- `tests/vision_settings_test.c`: the defaults; every key written and
  read back; TRACK's and TRAFFIC's lines apart; every refusal (an unknown
  mode, a distance off the list or with junk after it, bools that are not
  0/1, an orientation of off, an overlong line) leaving its default with
  the rest read; fields out of range put back; the store on a scratch
  state directory (0600 file, 0700 directory, no temporary left, a damaged
  value, an oversized file).
- `tests/vision_model_test.c` also holds the picker (groups, what caps
  offer, choosing, a stored mode the helper cannot run), TRAFFIC's setup
  (every cell, the distance both ways, the sheet closing on an error), the
  settings each tap stores, and the sheet on both reference pictures.
- `tests/vision_shell_test.sh` (needs `SHELL_BIN`): the real shell opening
  Vision in both orientations on the fake camera and detector, the picker
  and the setup covering the picture, a stored mode obeyed, a damaged one
  falling back, and nothing written by merely opening.
- `tests/pocketcam_test.c` gains the planar BGR format (three checks).
- `tests/vision_lint.sh` holds the boundaries: no LVGL below the screen,
  no camera or detector in the app, nncase in one file behind a C
  interface, the core pure and bounded, the screen blocking on nothing,
  the shell linking none of it, Camera untouched.
- The launcher, icon, chrome and art suites now expect eighteen apps.

## On unit A (to do)

The gate sheet, `docs/hardware/VISION_GATE.md`, has to show, with the
build's identity first: `pos-vision probe` opening `/dev/video2` in BG3P
and the model; `pos-vision bench 100` with its frame rate and the three
timings; the app in both orientations with boxes on real objects (a person,
a cup, a phone) that follow them and keep their ids; a crossing counted in
the right direction on both lines; repeated open and close (mindful of the
pre-existing camera lock-up in KNOWN_ISSUES: a bounded count, the console
logged); Camera working after Vision; no helper left; the helper's CPU
share and resident set from `stats`, and the shell's from `top`.
