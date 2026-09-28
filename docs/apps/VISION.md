# Vision

The camera's live picture with what the K230's KPU sees in it: a box, a
class and a confidence around every object a YOLOv8n detector finds, a
persistent id on every object as it moves, and one line across the picture
that counts what crosses it, each way.

Status: **prototype on branch `feat/vision-app`, not merged.** Host-tested
end to end on the fake camera and a fake detector (every stage, the helper,
the session); built for riscv64 with the pinned Xuantie toolchain against
the nncase 2.11 runtime in the pinned SDK's sysroot; **not yet run on unit
A** (the unit was off the bench when the branch was made - see "On unit A"
at the end for what the gate has to show). The camera path is Camera's own
(ADR-006), reused, not redesigned; the launcher place and the icon are for
the owner to confirm (DS §38, PROPOSED).

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
  eight missed frames on the object's last motion, and is never reused.
  At most 32 objects are tracked at once; more are dropped, never queued.
- **COUNT.** LINE cycles the counting line through ACROSS (horizontal, at
  mid-height: counts DOWN and UP), DOWN (vertical, at mid-width: counts
  LEFT and RIGHT) and OFF. A confirmed track counts once when its centre
  goes clearly from one side to the other (a 4 px dead band on the line
  stops a wobble counting), in the direction it went, and counts again
  only when it really comes back. RESET zeroes the counts and forgets the
  tracks.
- **States said in words.** "Starting" while the camera and the model open;
  "Waiting for the picture"; "Waiting for the camera..." for a stalled
  stream; "Vision stopped" with the reason and TRY AGAIN; "No camera or
  detector" with the reason and CHECK AGAIN. Under the fake backend the
  header says SIMULATED.

## What it is not

No OCR, segmentation, pose, faces, recognition of people, recording,
network or cloud. No settings: one model, one input size, one confidence
threshold (0.35, the vendor's default), one line in one of two places.
Nothing is stored: the counts live as long as the screen is open.

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

**The model file is not in this repository and not in the image package.**
The helper reads it from `/usr/share/doors/vision/yolov8n.kmodel`
(`$POCKETOS_VISION_MODEL` or `--model` override); for the prototype it is
copied there by hand from the vendor tree at deploy time. The reason is
licensing, not size: the vendor's `package/yolo` sources carry Canaan's
BSD-style header, but the kmodel is a compiled derivative of Ultralytics'
YOLOv8n weights, whose own licence is AGPL-3.0, and the SDK says nothing
about the terms it ships the kmodel under (docs/LICENSING.md, open item
10). Until the owner decides, the file travels outside the package, and
Doors' own code (the decoder, tracker, counter, helper and app) contains
nothing of the model.

The COCO class names are compiled in (`core/pocketvision/vision_labels.c`),
in the order the vendor's `coco_labels.txt` lists them.

## The camera and KPU pipeline

Everything below runs in the helper process, `pos-vision`, once per frame,
synchronously, in this order:

| Step | Where | What |
| --- | --- | --- |
| Capture | `core/pocketcam/pocketcam_v4l2.c` (Camera's backend, one new config key) | `/dev/video2`, the ISP's self path, 640 x 360, **planar `BG3P`** (`fmt=bg3p`): the format the vendor's KPU demos read, three 8-bit planes AI2D takes as they are - R, G, B in memory despite the driver's name (VERIFIED on unit B). Camera itself keeps NV16. |
| Into AI2D memory | `vision_kpu_nncase.cpp` | the frame is copied (691 KB) into a tensor from the runtime's shared pool and written back from the cache. A V4L2 buffer cannot be handed to AI2D directly: the runtime needs a physical address it does not have for an MMAP buffer. This is the one copy of a frame in the pipeline (the vendor's demo makes the same one, of a 2.7 MB frame). |
| Preprocess | AI2D, hardware | resize into the top-left of 320 x 320 keeping the aspect (0.5 x: 320 x 180), pad the rest with 114 - the vendor's `padding_resize_one_side_set`. |
| Infer | KPU, through the nncase interpreter | one run; the first output mapped and its 2100 x 84 floats copied out. |
| Decode | `vision_decode.c` | per row: the best class score, the threshold (0.35), the box centre and size back through the letterbox ratio, clipped to the frame; NaN, infinities, empty and absurd boxes skipped and counted; a tensor of the wrong shape refused before a value is read. At most 256 candidates, the best kept. |
| Suppress | `vision_nms.c` | class-aware greedy NMS at IoU 0.65 (the vendor's default), at most 32 detections. |
| Track | `vision_track.c` | greedy IoU matching against each track's prediction (last box + last motion), same class only; new ids for the unmatched; expiry after 8 misses; confirmation after 2 sightings; 32 tracks at most. Frame pixels throughout. |
| Count | `vision_line.c` | the line, chosen on the picture, unmapped into frame pixels (`vision_geom.c`) and checked against every seen, confirmed track's centre. |
| Say | `pocketvision_proto.h` | one `det` line per frame with every shown track mapped into picture pixels (`vision_geom.c`, the same turn, mirror and cover-fit the converter draws with); `count` when it changes; `stats` once a second. |
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

Pictures travel as Camera's do: a sealed memfd of four 1024 x 1024 RGB565
slots, the helper writing a slot it owns, the app copying the newest one
out and releasing it in the same timer tick. Boxes travel as text, in
picture pixels, so the app draws them with no geometry of its own.

### The protocol

`core/pocketvision/pocketvision_proto.h` is the reference. Commands:
`view w h rotation`, `start`, `stop`, `release slot`, `line x0 y0 x1 y1`
(per-mille of the view) or `line off`, `reset`, `quit`. Events: `hello`,
`ready camera pw ph simulated model in_w in_h classes`, `nodevice`,
`nomodel`, `error what text`, `frame slot seq w h`,
`det seq n id:cls:conf:x:y:w:h...`, `count ab ba`,
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
| Boxes on one `det` line, and outline objects on screen | 24 |
| Protocol line | 1024 bytes |
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
- `tests/vision_track_test.c` (34 checks): one id across frames and across
  motion, a jump is a new object, another class is a new object, expiry
  after exactly `VISION_TRACK_MAX_MISSES`, ids never reused, the list
  bounded with drops counted; the line's sides and dead band; a crossing
  counted once in its direction, the way back counted the other way, a
  wobble on the line never counted, an unconfirmed or newborn track never
  counted, two objects opposite ways, a prediction across the line not a
  crossing, a disabled line.
- `tests/vision_geom_test.c` (24 checks): every mapping held against
  `pocketcam_to_rgb565()` by painting one pixel and finding it, for all
  four turns, mirrored and not, scaled and cut on either axis; boxes
  clamped or reported outside; the way back for the line.
- `tests/vision_model_test.c` (42 checks): every state's words, the line
  modes and direction names, stats and counts from the session, every
  failure's sentence; both layouts on the reference panel with every
  control inside the safe box, at least the touch minimum, not
  overlapping, the picture in the frame's shape.
- `tests/vision_session_test.c` (60 checks) against the real helper on
  the fake camera and the fake detector: every event line parsed and the
  malformed ones refused; a picture through the shared memory; two objects
  tracked with ids that persist across thirty frames; the person walking
  down the picture counted once, downward; stats; reset; stop; two
  malformed tensors said and survived; a detector giving nonsense ended
  with exit 5; a failed run; no camera, a busy camera, a bad detector
  script, a missing helper, a camera that goes away, a hung helper killed
  by the watchdog, a crashing one; thirty opens and closes with no
  descriptor or child left behind.
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
