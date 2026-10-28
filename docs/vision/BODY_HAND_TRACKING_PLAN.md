# Vision: body pose, hands and fingers - follow-up design

Status: **PROPOSED, not implemented.** A plan for a second delivery after
the GENERAL 80 / COLOR / LINE TRACE work (docs/apps/VISION.md). Nothing here
is in the code; `tests/vision_lint.sh` still holds "no pose" for Vision, and
that rule changes only with this work, on purpose.

Evidence classes as in AGENTS.md. Sources read on 2026-10-26: the pinned K230
Linux SDK's `buildroot-overlay/package/ai_demo` (code and READMEs, read-only),
and the upstream projects' own pages. No model was downloaded, converted or
run for this plan, and no K230 performance figure is estimated: the numbers
that decide inference rates are listed as measurements to make (section 6).

## 1. What can be built, with what

| Stage | Candidate | Contract (from the vendor code, VERIFIED unless marked) | Licence of the weights | Status for Doors |
| --- | --- | --- | --- | --- |
| Hand detection | SDK `hand_det.kmodel` (`sq_hand_det`, README: YOLOv5 on a 1.0 MobileNetV2) | letterboxed square input, pad 114, centred; three YOLOv5 heads (strides 8/16/32, 3 anchors each, anchors in `hand_detection.h:140-142`), 6 values a row, score = obj x cls, sigmoid ASSUMED baked in; boxes under 10 % of the frame dropped (`hand_detection.cc:172`); NMS 0.4 | none stated (SDK, `kmodel_v2.10.0.tgz` / `v2.11.0` from download.kendryte.com) | UNKNOWN - EXTERNAL ONLY, like FACE's model |
| 21 hand landmarks | SDK `handkp_det.kmodel` (`sq_handkp_det`, README: ResNet50) | a crop of the hand box resized by AI2D without padding; output `[1, 42]`: 21 (x, y) in 0..1 of the crop | none stated | UNKNOWN - EXTERNAL ONLY |
| Body pose | SDK `yolov8n-pose.kmodel` (`pose_detect`) | letterboxed, pad 114; output `[1, 56, 8400]` transposed: box, score, 17 COCO keypoints x (x, y, conf); NMS 0.45, threshold 0.3 (README) | Ultralytics YOLOv8-pose: **AGPL-3.0** (ultralytics.com/license) | on the refused list's terms, like yolov8n: internal bench only |
| Upstream alternatives | MediaPipe palm detector + hand landmarker (21 points, Apache-2.0 code); BlazePose (33 points); MMPose RTMPose/RTMO (Apache-2.0 code) | TFLite / PyTorch: each needs an nncase 2.11 compile and its own pre- and post-processing | weights' terms not confirmed from a primary source for any of them (MediaPipe's model cards could not be read as text; Hand5 for RTMPose includes data whose terms need checking, InterHand2.6M is CC BY-NC 4.0) | not usable until the terms are settled and a compile is proposed |

The kmodels' tensor shapes are read at run time by the vendor code and were
not inspected (no file on this PC): shapes, sizes, sha256 and whether they
load on the nncase 2.11.0 runtime are UNKNOWN until a unit or the simulator
opens them. The keypoint order 0 = wrist, then four points per finger (thumb
1-4, index 5-8, middle 9-12, ring 13-16, little 17-20) is ASSUMED from the
vendor's drawing and angle code; it matches MediaPipe's documented order.

## 2. The pipeline (one helper, as now)

Everything stays in `pos-vision`, one KPU user, the camera owned as today
(ADR-006). The screen only draws what the helper says.

1. **Find the hands** on the upright frame (`vision_turn_planes`, already
   there) with the hand detector at its own input size. Decode with a new
   `vision_hand.c` beside `vision_face.c` (anchors computed, tested against
   the vendor's table, as FACE's were).
2. **Crop each hand square**, the vendor's rule: centre of the box, half side
   `1.26 x max(w, h) / 2`, clamped to the frame. Doors should **pad instead of
   clamp** (AI2D's pad, value 114) so a hand at the edge keeps a square crop
   and its landmarks are not stretched; that differs from the vendor and needs
   its own check against the model's accuracy.
3. **Landmarks per crop**: AI2D crops and resizes the square straight from the
   frame already in KPU memory (`vision_kpu_infer_window`, the FAR pass's
   mechanism, extended to a square of any size), one run per hand, at most
   two hands a frame (bounded like FACE's 16 faces).
4. **Back to the picture**: `x = crop_x + px * crop_side`, `y = crop_y + py *
   crop_side` in frame pixels, then `vision_box_unturn` and the view mapping
   the boxes already use (`vision_geom.c`), so dots land where the screen
   draws them. With a padded crop, the pad offset comes off first.
5. **Body pose** is a separate mode (BODY), not a stage in front of the hands:
   the 17 COCO points include wrists but no hand box, and the vendor pipeline
   finds hands directly. A later option, only if the hand detector misses
   small hands: a hand window centred on the wrist point, sized from the
   forearm (elbow to wrist), fed to the landmark net the same way.
6. **Ids**: hands and bodies through the existing tracker (`vision_track.c`,
   class 0, NORMAL's parameters), so a hand keeps an id across frames.

Protocol: one new event, e.g. `hand <seq> <n> <id>:<x0>:<y0>:...:<x20>:<y20>`
(view pixels), bounded by the line length (2 hands x 21 points x ~9 chars fit
VISION_LINE_MAX); `pose` likewise with 17 points and per-point confidence.

## 3. What to expect, honestly

- **Distance and resolution.** The preview is 640 x 360 and the detectors see
  it scaled to their input; a hand a metre away is a few dozen pixels, and the
  landmark crop is then up-sampled from that. Finger points will be coarse
  beyond arm's length. The FAR range's lesson applies: pixels, not thresholds,
  set the distance.
- **Occlusion.** The landmark net always returns 21 points; there is no
  per-point confidence or handedness output in the vendor model (VERIFIED: a
  `[1, 42]` tensor). A fist or a hand turned away still yields points, so the
  screen must not present them as measured fingers when the hand box is small
  or the points collapse. A plausibility check (bone lengths against the palm
  size) is the minimum.
- **Rotation.** The vendor crop is axis-aligned; MediaPipe rotates the crop to
  the hand. A tilted hand is a harder input for an unrotated landmark net.
- **Light.** The same camera limits as every mode: the night-time gates saw
  nothing in a dark room.

## 4. Gestures later: evidence over time, not one frame

A "wave" is a claim about motion over time. It must not be generic motion
called waving. The vendor's dynamic-gesture demo shows the shape such logic
takes (VERIFIED, `dynamic_gesture/main.cc`): a trigger pose (open hand) held,
a direction from the wrist to the middle fingertip in angle bands, a change
accepted only after repeated agreement, a window of at most 20 results, and
a 2 s timeout. For Doors, a wave would need all of:

- the same tracked hand id throughout (no id switch mid-gesture);
- an open hand (all five fingers extended by the joint-angle rule) in most
  frames of the window;
- the wrist's horizontal position reversing direction at least twice
  (left-right-left) within about 1-2 s, each swing larger than a fraction of
  the palm width - normalised by hand size, so distance does not change it;
- little vertical travel compared with horizontal;
- and nothing said when the frame rate through the landmark net is too low
  to sample the swings (section 6 decides that figure).

The gesture logic is pure C over a ring of per-hand samples, testable with
synthetic tracks the way the tracker and the speed lines are.

## 5. Faces and identity: not here

Owner recognition is RECOGNIZE's and, for DeskBuddy, the separate DeskBuddy
presence work. This plan creates no second enrolment, no second biometric
store and no identity claim. A future "whose hand" question would ask
RECOGNIZE's face track, never a new profile.

## 6. Measurements on the K230 before choosing rates

None of these can be estimated on the host; each is a unit measurement with
`pos-vision bench` (extended to the new nets) and the app's `stats`:

1. Each model loads on the nncase 2.11.0 runtime; input/output shapes as
   printed by the runtime; CMA taken and given back at close.
2. KPU time per run: hand detector at its input size; landmark net per crop;
   pose net. Whether either falls back to the CPU (seconds, not milliseconds).
3. AI2D crop-and-resize time per hand crop (the window pass).
4. End-to-end frame rate with 0, 1 and 2 hands in view, and the helper's CPU
   share and RSS.
5. Landmark stability: the spread of each point over 30 frames of a still
   hand at 30 cm, 60 cm and 1 m, in good and in dim light.
6. Detection range: the largest distance at which a hand is found at all, and
   at which the landmarks pass the plausibility check.

From these: whether landmarks run every frame or every Nth frame with the
detector's box tracked between, and the frame rate below which gestures are
not offered.

## 7. Before any of it ships

- A licence decision on each model file (MODEL_LICENSES.md), as for READ,
  FACE and RECOGNIZE: the SDK states none for the hand nets, and YOLOv8-pose
  is AGPL-3.0.
- `tests/vision_lint.sh`'s "no pose" rule changed deliberately in the same
  change, with the privacy statement kept: nothing leaves the unit.
- docs/apps/VISION.md "What it is not" updated.
