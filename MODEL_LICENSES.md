# Model licences

Doors' own code is Apache-2.0 (LICENSE). **No machine learning model is
covered by that licence.** Vision's code - the decoders, tracker, counter,
OCR and face pipelines in `core/pocketvision`, the helper `tools/vision`, the
app `apps/vision` and the DeskBuddy bridge `apps/deskbuddy_vision` - contains
no model data and is Apache-2.0 independently of the models it runs.

This file lists every model Doors uses, can use, or has looked at, with what
is known about its terms. The full audit is
docs/licensing/APACHE_2_READINESS.md §4. Audited 2026-10-01 at origin/master
`426b1d8`.

No model file is committed to this repository. A model reaches a unit in one
of two ways: the `pocketos` Buildroot package installs it (only
`yolov8n.kmodel`), or a vendor package of the K230 SDK installs it into the
image for the vendor's own demos.

## Status values

| Status | Meaning |
| --- | --- |
| REDISTRIBUTABLE | Terms established from a primary source and they allow redistribution in a public image. |
| EXTERNAL ONLY | Doors can use it if the user supplies it; it is neither committed nor installed by Doors. |
| UNKNOWN - DO NOT REDISTRIBUTE | No licence statement from the publisher, or terms that are not settled for public redistribution. |
| NOT USED / REFERENCE ONLY | Looked at, not used by any Doors feature. |

## Matrix

| Model file (on the unit) | Feature | Origin | Author / provider | Licence | Redistribution / commercial use / modification | Attribution | Committed | In the Doors image | Status |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `/usr/share/doors/vision/yolov8n.kmodel` (3,495,296 B, sha256 `0b4bcdd3…2004a09`, pinned in `tools/vision/yolov8n.kmodel.sha256`) | DETECT, TRACK, COUNT, TRAFFIC; DeskBuddy's provider (DETECT) | K230 Linux SDK `buildroot-overlay/package/yolo/utils/yolov8n.kmodel` at `22d02c6` | Ultralytics (YOLOv8n weights); compiled for nncase 2.11 by Canaan (conversion not published) | The SDK states **no licence for the file**. Ultralytics publishes YOLOv8 and its weights under **AGPL-3.0**, with a commercial licence as the alternative | Under AGPL-3.0: allowed, with AGPL source obligations that cannot be fully met here (the exact weights and conversion settings are unpublished) | AGPL-3.0 text and notice (shipped: THIRD_PARTY_NOTICES.txt, `yolov8n-kmodel`) | No | **Yes**: `pocketos.mk` copies the SDK's file, hash-pinned. Owner decision 2026-09-28: **internal images only** | **UNKNOWN - DO NOT REDISTRIBUTE** outside the project (docs/LICENSING.md item 10) |
| `/usr/share/doors/vision/face_det.kmodel` | FACE; RECOGNIZE's detector; DeskBuddy's provider (FACE, RECOGNIZE) | The vendor's `face_detection_320.kmodel` (584,576 B): SDK `face_detect` / `ai_demo`; also in the canmv_rt tree and the launcher's `models/` | Canaan demo; the demo's README names RetinaFace on a 0.25 MobileNet | **None stated** | Unknown | Unknown | No | Not by Doors. **The vendor's copy is in the image** at `/root/app/face_detect/` (`BR2_PACKAGE_FACE_DETECT=y`), and the launcher's copy under `/root/app/k230_phone_ui/models/` | **UNKNOWN - DO NOT REDISTRIBUTE**; for Doors: EXTERNAL ONLY |
| `/usr/share/doors/vision/text_det.kmodel` | READ (text detector) | canmv_k230 `src/rtsmart/libs/kmodel/ai_poc/kmodel/ocr_det.kmodel` (2,958,504 B, sha256 `b8a71660…7b79fc`) | Canaan (canmv examples) | **None stated** for the file. The LILYGO canmv_rt README's header comment says "@License: GPL 3.0" for that repository; nothing ties it to the model | Unknown | Unknown | No | No | **UNKNOWN - DO NOT REDISTRIBUTE**; EXTERNAL ONLY |
| `/usr/share/doors/vision/text_rec.kmodel` | READ (text recogniser) | same tree, `ocr_rec_int16.kmodel` (13,008,216 B, sha256 `7a307f86…aa8648c`) | Canaan | **None stated** | Unknown | Unknown | No | No | **UNKNOWN - DO NOT REDISTRIBUTE**; EXTERNAL ONLY |
| `/usr/share/doors/vision/text_dict.txt` | READ (recogniser alphabet, 6,549 entries) | same tree, `ai_poc/utils/dict_ocr.txt` (32,521 B, sha256 `8288453b…a74c8fb`) | Canaan; upstream origin not stated | **None stated** | Unknown | Unknown | No | No | **UNKNOWN - DO NOT REDISTRIBUTE**; EXTERNAL ONLY |
| `/usr/share/doors/vision/face_embed.kmodel` | RECOGNIZE (face embedding); DeskBuddy's provider (owner recognised) | same tree, `face_recognition.kmodel` (46,333,280 B, sha256 `2409a30f…78218472`) | Canaan; source weights not stated | **None stated** | Unknown | Unknown | No | No | **UNKNOWN - DO NOT REDISTRIBUTE**; EXTERNAL ONLY |
| `xiaozhi_kws.kmodel` (369,560 B) | none (the vendor launcher's keyword spotter) | LILYGO `k230_phone_ui` package | LILYGO / unknown | **None stated** (the LILYGO repository has no licence) | Unknown | Unknown | No | Not by Doors; **in the image** through the vendor launcher | NOT USED by Doors; image blocker as vendor material |
| `test.kmodel` (2,286,808 B) | none (the vendor `ai2d_kpu` test) | SDK `ai2d_kpu` package | Canaan | **None stated** | Unknown | Unknown | No | Not by Doors; **in the image** through `BR2_PACKAGE_AI2D_KPU=y` | NOT USED by Doors; image blocker as vendor material |
| `ocr_rec.kmodel`, `ocr_det_int16.kmodel`, `door_lock/mbface.kmodel`, `yolov5n`, `yolo11n`, `yolo26n`, the self-learning `recognition.kmodel` / `embedding.kmodel` | none | SDK and canmv trees | various | not established | - | - | No | No | NOT USED / REFERENCE ONLY |

## What follows

- **Doors source.** No model is committed, so the models do not block
  publishing the source repository. The source does name where the
  models came from and their hashes; that is description, not
  redistribution.
- **A public image.** Not possible while the image installs
  `yolov8n.kmodel` or carries the vendor models listed above. Before a
  public image: either settle item 10 for distribution (Ultralytics'
  commercial licence, or an AGPL-compliant model with published weights and
  conversion), or make DETECT's model EXTERNAL ONLY like READ's and
  RECOGNIZE's (stop installing it in `pocketos.mk`; the helper already offers
  a mode only when its file is present). The vendor models go with their
  packages (docs/LICENSING.md item 5).
- **Replacing a model** with one whose terms are known (for example a
  detector trained on a permissively licensed dataset and published with its
  weights) is a separate piece of work and is not done here.

## Evidence

- Model files, sizes and hashes: docs/apps/VISION.md ("The model", READ,
  FACE, RECOGNIZE), measured on unit B 2026-09-28 to 09-30 (VERIFIED);
  the image's model files listed from the built target tree
  `output/k230_pocketos_defconfig/target` on 2026-10-01 (VERIFIED).
- No licence file in the SDK's `yolo`, `face_detect`, `ai2d_kpu` packages,
  in the canmv `ai_poc` model tree, or in the LILYGO repository (VERIFIED
  locally, docs/LICENSING.md items 2, 10, 11).
- Ultralytics' licence for YOLOv8: AGPL-3.0, https://github.com/ultralytics/ultralytics
  (LICENSE) and https://www.ultralytics.com/license (DOCUMENTED, recorded in
  third_party/notices/texts/yolov8n-kmodel.txt).
- The canmv_rt README header comment "@License: GPL 3.0"
  (vendor/T-Display-K230_canmv_rt/README.md line 9, VERIFIED locally).
