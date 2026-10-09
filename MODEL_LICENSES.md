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

No model file is committed to this repository. From Doors 0.3.6 the
`pocketos` Buildroot package installs **one** model, the project's own R0
detector (an experimental beta, below), from a build input kept outside git
and checked against `tools/vision/r0-model.sha256`. From 0.3.5 on it installs
no other: `yolov8n.kmodel`, which images up to 0.3.0 carried for internal
use, is not in the image, and the package refuses any of the SDK's
Ultralytics YOLO kmodels, and the upstream YOLOX-Tiny file of the bench A/B,
anywhere in the target, by name and by hash (`pocketos.mk`,
`tools/vision/refused-models.sha256`). No vendor model is in the image either
(0.3.5 turned their packages off).

## Status values

| Status | Meaning |
| --- | --- |
| REDISTRIBUTABLE | Terms established from a primary source and they allow redistribution in a public image. |
| EXTERNAL ONLY | Doors can use it if the user supplies it; it is neither committed nor installed by Doors. |
| UNKNOWN - DO NOT REDISTRIBUTE | No licence statement from the publisher, or terms that are not settled for public redistribution. |
| NOT USED / REFERENCE ONLY | Looked at, not used by any Doors feature. |
| OWNER-LICENSED - THIRD-PARTY QUESTIONS OPEN | The project's own model, distributed under the licence the owner chose for it (its row). That choice covers the Doors project's own rights in the model. It does not settle anyone else's rights in what the model was made from: the row and "R0: the licence decision" below list those, each as a concrete condition or as an unresolved interpretation. |

## Matrix

| Model file (on the unit) | Feature | Origin | Author / provider | Licence | Redistribution / commercial use / modification | Attribution | Committed | In the Doors image | Status |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `/usr/share/doors/vision/det-r0-traffic6-yolox-tiny-416.kmodel` (5,894,104 B, sha256 `94a20ac0…d268`, pinned in `tools/vision/r0-model.sha256`) | DETECT, TRACK, TRAFFIC: Vision's detector from 0.3.6, selected on a fresh installation and named "R0 · Beta". Not DeskBuddy: its provider runs `pos-vision`'s default, `yolov8n.kmodel` (docs/apps/VISION.md) | R0: YOLOX-Tiny 416 trained by the Doors project on the `traffic6_r0` dataset, 300 epochs (2026-10-06 to 10-09) **from no pretrained weights** (`run.json`: `"pretrained_weights": null`; YOLOX's released checkpoints and ImageNet backbones excluded), checkpoint `best_ckpt.pth` epoch 299 (`a501b84c…5d6f`), ONNX `48722fb6…ab9c`, compiled with nncase 2.11.0 and Canaan's `nncase-kpu` 2.11.0 compiler plug-in (Apache-2.0 on PyPI). Records: docs/vision/R0_TRAFFIC6_RESULTS.md, DATASET_TRAFFIC6_R0.md, DATASET_PROVENANCE.md on `research/yolox-traffic-training` (`c710dee`) | the Doors project | **The weights: Apache-2.0** (owner decision, 2026-10-09; "R0: the licence decision" below). What it was made from: YOLOX code Apache-2.0 (`yolox` notice); COCO 2017 and Open Images V7 annotations CC BY 4.0; 48,277 Flickr images under CC BY 2.0 (47,876) or "No known copyright restrictions" (401), per image, uploader-asserted; NC, ND and BY-SA images excluded | Apache-2.0 for the weights; no documented third-party condition found that forbids it. Third-party rights are not settled by that choice: attribution is owed under CC BY if the weights are an adaptation (U1, unresolved), and the per-image list cannot name the photographers of the 10,408 COCO images (concrete gap under CC BY 2.0 section 4(b), if U1 applies); see below | THIRD_PARTY_NOTICES.txt `yolox` and `r0-training-data` (in the image); the per-image list `ATTRIBUTION.tsv` (sha256 `2bc99bc8b2098a4f628fa8f3d0c08af5252a0d4cbdfb1eddaad9f9062e594c3e`, 7,742,997 B) as an asset of the GitHub release that carries the model | No (a build input outside git; `apply_to_sdk.sh` takes it from `POCKETOS_VISION_R0_KMODEL`) | **Yes, from 0.3.6** (release candidate) | **OWNER-LICENSED - THIRD-PARTY QUESTIONS OPEN** |
| `/usr/share/doors/vision/yolov8n.kmodel` (3,495,296 B, sha256 `0b4bcdd3…2004a09`, pinned in `tools/vision/yolov8n.kmodel.sha256` for the bench tool `install-model.sh`) | DETECT, TRACK, TRAFFIC (with TRACK's count line); DeskBuddy's provider (DETECT) | K230 Linux SDK `buildroot-overlay/package/yolo/utils/yolov8n.kmodel` at `22d02c6` | Ultralytics (YOLOv8n weights); compiled for nncase 2.11 by Canaan (conversion not published) | The SDK states **no licence for the file**. Ultralytics publishes YOLOv8 and its weights under **AGPL-3.0**, with a commercial licence as the alternative | Under AGPL-3.0: allowed, with AGPL source obligations that cannot be fully met here (the exact weights and conversion settings are unpublished) | none shipped (its notice left the image with it) | No | **No, from 0.3.5** (images up to 0.3.0 carried it, internal images only, owner 2026-09-28); refused by `pocketos.mk`. Without it Vision offers COLOR, EDGE and LINE TRACE and says why the rest is off | **UNKNOWN - DO NOT REDISTRIBUTE** (docs/LICENSING.md item 10); replaced in Vision by the Doors-trained R0 (0.3.6) |
| `/usr/share/doors/vision/face_det.kmodel` | FACE; RECOGNIZE's detector; DeskBuddy's provider (FACE, RECOGNIZE) | The vendor's `face_detection_320.kmodel` (584,576 B): SDK `face_detect` / `ai_demo`; also in the canmv_rt tree and the launcher's `models/` | Canaan demo; the demo's README names RetinaFace on a 0.25 MobileNet | **None stated** | Unknown | Unknown | No | Not by Doors. The vendor's copy was in the image at `/root/app/face_detect/` up to 0.3.0; **not from 0.3.5** (`face_detect` off) | **UNKNOWN - DO NOT REDISTRIBUTE**; for Doors: EXTERNAL ONLY |
| `/usr/share/doors/vision/text_det.kmodel` | READ (text detector) | canmv_k230 `src/rtsmart/libs/kmodel/ai_poc/kmodel/ocr_det.kmodel` (2,958,504 B, sha256 `b8a71660…7b79fc`) | Canaan (canmv examples) | **None stated** for the file. The LILYGO canmv_rt README's header comment says "@License: GPL 3.0" for that repository; nothing ties it to the model | Unknown | Unknown | No | No | **UNKNOWN - DO NOT REDISTRIBUTE**; EXTERNAL ONLY |
| `/usr/share/doors/vision/text_rec.kmodel` | READ (text recogniser) | same tree, `ocr_rec_int16.kmodel` (13,008,216 B, sha256 `7a307f86…aa8648c`) | Canaan | **None stated** | Unknown | Unknown | No | No | **UNKNOWN - DO NOT REDISTRIBUTE**; EXTERNAL ONLY |
| `/usr/share/doors/vision/text_dict.txt` | READ (recogniser alphabet, 6,549 entries) | same tree, `ai_poc/utils/dict_ocr.txt` (32,521 B, sha256 `8288453b…a74c8fb`) | Canaan; upstream origin not stated | **None stated** | Unknown | Unknown | No | No | **UNKNOWN - DO NOT REDISTRIBUTE**; EXTERNAL ONLY |
| `/usr/share/doors/vision/face_embed.kmodel` | RECOGNIZE (face embedding); DeskBuddy's provider (owner recognised) | same tree, `face_recognition.kmodel` (46,333,280 B, sha256 `2409a30f…78218472`) | Canaan; source weights not stated | **None stated** | Unknown | Unknown | No | No | **UNKNOWN - DO NOT REDISTRIBUTE**; EXTERNAL ONLY |
| `/usr/share/doors/vision/det-upstream-yolox-tiny-416.kmodel` (sha256 `8c304651…e354`) | the bench A/B only: a second choice under MODEL when a unit has it (docs/apps/VISION.md) | upstream YOLOX-Tiny 416 (`yolox_tiny.pth`, 0.1.1rc0) compiled for the 2026-10-04 detector evaluation | Megvii (weights) | **None stated** for the released checkpoints (YOLOX issue #1865 unanswered) | Unknown | Unknown | No | **No**: refused by hash in the target (`tools/vision/refused-models.sha256`) | **UNKNOWN - DO NOT REDISTRIBUTE**; EXTERNAL ONLY (bench units, by hand) |
| `xiaozhi_kws.kmodel` (369,560 B) | none (the vendor launcher's keyword spotter) | LILYGO `k230_phone_ui` package | LILYGO / unknown | **None stated** (the LILYGO repository has no licence) | Unknown | Unknown | No | No: the launcher package is purged from the image (chore/remove-vendor-launcher, PR #47) | NOT USED by Doors |
| `test.kmodel` (2,286,808 B) | none (the vendor `ai2d_kpu` test) | SDK `ai2d_kpu` package | Canaan | **None stated** | Unknown | Unknown | No | No, from 0.3.5 (`ai2d_kpu` off); up to 0.3.0 through `BR2_PACKAGE_AI2D_KPU=y` | NOT USED by Doors; as image content **UNKNOWN - DO NOT REDISTRIBUTE** (vendor material, B6/B7) |
| `ocr_rec.kmodel`, `ocr_det_int16.kmodel`, `door_lock/mbface.kmodel`, `yolov5n`, `yolo11n`, `yolo26n`, the self-learning `recognition.kmodel` / `embedding.kmodel` | none | SDK and canmv trees | various | not established | - | - | No | No | NOT USED / REFERENCE ONLY |

## R0: the licence decision

**Decision (owner, 2026-10-09):** the Doors project distributes the R0 model
weights (`det-r0-traffic6-yolox-tiny-416.kmodel`) under the **Apache License
2.0**, the licence of Doors' own code (LICENSE). The third-party notices and
the attribution stay as they are: THIRD_PARTY_NOTICES.txt (`yolox`,
`r0-training-data`) in the image, and the per-image list beside the release.

**Publication (owner, 2026-10-09):** the owner decided to publish Doors
0.3.6 with R0 under Apache-2.0, accepting the residual uncertainties below,
including the COCO attribution gap. That is a publication decision, not
proof that third-party rights are resolved.

**Attribution list.** `ATTRIBUTION.tsv` of the `traffic6_r0` dataset, sha256
`2bc99bc8b2098a4f628fa8f3d0c08af5252a0d4cbdfb1eddaad9f9062e594c3e`, 7,742,997
bytes, 42,888 rows (10,408 COCO, 32,480 Open Images) under a three-line
header naming the annotation licensors. It is published as an asset of the
GitHub release that carries R0 (from v0.3.6), with its checksum in that
release's SHA256SUMS. It is not in the image or in this repository.

**Checked against the provenance review** (docs/vision/DATASET_PROVENANCE.md
on `research/yolox-traffic-training`; no new research). Evidence classes as
in AGENTS.md.

| Input | Documented condition | Conflict with Apache-2.0 for the weights? |
| --- | --- | --- |
| YOLOX code at `6ddff48` (training, export) | Apache-2.0; keep its licence and notices (VERIFIED, licence text at the pin) | None: same licence; no YOLOX code is in the weights or the image |
| YOLOX released checkpoints, ImageNet backbones | not used: R0 was trained from no pretrained weights (`run.json` `"pretrained_weights": null`, VERIFIED) | None: not an input |
| COCO 2017 and Open Images V7 annotations | CC BY 4.0: credit the licensor, link the licence, say what was changed (VERIFIED, both publishers' terms) | **None found.** CC BY 4.0 places no condition on the licence of something made from the material beyond attribution and not stopping recipients from complying; Apache-2.0 does neither. Whether the weights are made from the annotations in the licence's sense is U1 (below); the credit is given either way (`r0-training-data`) |
| Flickr images under CC BY 2.0 (47,876 in the dataset; 42,550 training rows) | section 4(a), "You may not sublicense the Work", and the other 4(a) terms bind the **images themselves**; section 4(b) asks, for the Work "or any Derivative Works", credit "by conveying the name ... of the Original Author if supplied", and the URI the licensor names where reasonably practicable (VERIFIED, creativecommons.org/licenses/by/2.0/legalcode, read 2026-10-09) | **No conflict:** no image is distributed, so section 4(a) has nothing to attach to; Apache-2.0 does not stop the credit 4(b) asks for. **Concrete gap, if U1 applies:** the 10,070 CC BY 2.0 COCO rows carry no author name and only the static image URL (COCO records neither, U3); the Open Images rows carry author, profile and photo page |
| "No known copyright restrictions" (Flickr Commons; 401 in the dataset, 338 training rows) | a statement by the institution, not a licence (U4, DOCUMENTED) | None: no condition is stated; whether the statement is right is the residual risk the policy accepted |
| COCO's images clause | images "must abide by the Flickr Terms of Use"; users "accept full responsibility" (VERIFIED, COCO terms) | **Unresolved interpretation, not a found restriction:** the provenance review did not read Flickr's Terms of Use for a condition on models trained from the photos, and none is cited |
| Excluded data (NC, ND, BY-SA, BDD100K, KITTI, Cityscapes and others) | not used (dataset gate E, VERIFIED) | None: not inputs |
| Compiler: nncase 2.11.0 and Canaan's `nncase-kpu` plug-in | Apache-2.0 on PyPI (DOCUMENTED, THIRD_PARTY_LICENSES.md) | None found |

**Unresolved interpretations** (the decision above does not resolve them):
- U1: whether trained weights are an adaptation or derivative of the
  training pictures and annotations at all (unsettled in EU and US law).
  It decides whether the CC BY attribution duties apply to the weights.
- U2, U6, U10: the per-image licence labels are the uploaders' own; COCO and
  Google warrant nothing; 8 of 78 (COCO) and 3 of 84 (Open Images) sampled
  still-visible photos carry a stricter Flickr licence today.
- U4: "No known copyright restrictions" is a statement, not a licence.
- U11: the Open Images files are re-encoded mirror copies.
- COCO's pointer to Flickr's Terms of Use (above).
- People appear in the training pictures; no licence above addresses
  personal or image rights of the people shown.

**Concrete gap, owner to accept or close:** if U1 makes the weights a
Derivative Work of the CC BY 2.0 COCO pictures, section 4(b) asks for the
photographer's name where supplied, and the list has none for 10,070 COCO
images. Closing it needs a per-photo Flickr lookup (provenance open item U3),
not done.

## What follows

- **Doors source.** No model is committed, so the models do not block
  publishing the source repository. The source does name where the
  models came from and their hashes; that is description, not
  redistribution.
- **A public image.** `yolov8n.kmodel` no longer blocks it (not installed
  from 0.3.5; the helper offers DETECT, TRACK and TRAFFIC only when a
  detector file is present). From 0.3.6 the image carries R0, whose open
  points (its row) are the owner's to decide before an image with it is
  published. The vendor models still in the image
  were `face_detection_320.kmodel` (`face_detect`) and `test.kmodel`
  (`ai2d_kpu`); both packages are off from 0.3.5, so no model of unknown
  terms is left in the image
  (docs/LICENSING.md item 5); so do the other blockers in
  docs/licensing/APACHE_2_READINESS.md.
- **Replacing a model** with one whose terms are known is what R0 is: a
  detector trained by the project on licence-filtered data
  (research/yolox-traffic-training). Its row says what is still open.

## Evidence

- Model files, sizes and hashes: docs/apps/VISION.md ("The model", READ,
  FACE, RECOGNIZE), measured on unit B 2026-09-28 to 09-30 (VERIFIED);
  the image's model files listed from the built target tree
  `output/k230_pocketos_defconfig/target` on 2026-10-01 (VERIFIED).
- No licence file in the SDK's `yolo`, `face_detect`, `ai2d_kpu` packages,
  in the canmv `ai_poc` model tree, or in the LILYGO repository (VERIFIED
  locally, docs/LICENSING.md items 2, 10, 11).
- Ultralytics' licence for YOLOv8: AGPL-3.0, https://github.com/ultralytics/ultralytics
  (LICENSE) and https://www.ultralytics.com/license (DOCUMENTED; the notice
  text that recorded it, third_party/notices/texts/yolov8n-kmodel.txt, left
  the tree with the model in 0.3.5 and is in the git history).
- The canmv_rt README header comment "@License: GPL 3.0"
  (vendor/T-Display-K230_canmv_rt/README.md line 9, VERIFIED locally).
