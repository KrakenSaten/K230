# Dataset provenance: DOORS Traffic detector (YOLOX-Tiny 416)

Status: **research, 2026-10-04**. This is an engineering record, not legal
advice. Nothing here changes production Vision; `yolov8n.kmodel` stays the
shipped model.

The machine-readable companion is
[`tools/vision/training/provenance/sources.json`](../../tools/vision/training/provenance/sources.json).
The per-image licence policy is
[`provenance/coco_licence_policy.json`](../../tools/vision/training/provenance/coco_licence_policy.json).

Evidence classes (AGENTS.md):
- **VERIFIED** = read from the primary source, or measured here, on 2026-10-04
- **DOCUMENTED** = secondary source
- **ASSUMED** = not verified

## 1. Goal and rule

The goal is a YOLOX-Tiny detector whose every input has known terms, so the
weights can later ship with DOORS under a clear notice.

The rule: an input is used only when its terms are known and allow
commercial use and adaptation. Being publicly downloadable is not enough. An
input with unclear terms is excluded or held for an owner decision.

## 2. Inputs at a glance

| Input | Licence / terms | Usable for training | Status |
|---|---|---|---|
| YOLOX code @ `6ddff48` | Apache-2.0 (VERIFIED) | yes | **used** |
| YOLOX released checkpoints (0.1.1rc0) | not stated; issue #1865 unanswered (VERIFIED) | not as an initialisation | **excluded** |
| ImageNet-pretrained backbones | ImageNet terms: non-commercial research (DOCUMENTED) | no; YOLOX trains from scratch | **excluded** |
| COCO 2017 annotations | CC BY 4.0, COCO Consortium (VERIFIED) | yes | **used** |
| COCO 2017 images, CC BY 2.0 / no known restrictions | per image, Flickr (VERIFIED field) | yes (conservative tier) | **used** |
| COCO 2017 images, CC BY-SA 2.0 | per image | only after an owner decision | **held** |
| COCO 2017 images, NC / ND licences | per image | no | **excluded** |
| Open Images V7 | annotations CC BY 4.0; images "listed as" CC BY 2.0 (VERIFIED) | yes, after metadata filter | **planned** |
| DOORS camera captures | owner's own; GDPR applies | test set first | **planned** |
| 2026-10-04 evaluation frames | vendor pictures + DOORS frames | **never** (hash-excluded) | evaluation only |
| BDD100K | commercial use needs a UC Berkeley licence (DOCUMENTED) | no | **excluded** |
| KITTI | CC BY-NC-SA 3.0 (DOCUMENTED) | no | **excluded** |
| Cityscapes | non-commercial (DOCUMENTED) | no | **excluded** |
| nuScenes, Waymo Open, Mapillary Vistas | non-commercial terms (ASSUMED, not re-read) | no | **excluded** |
| UA-DETRAC, MIO-TCD, Objects365, Pascal VOC, Roboflow Universe | terms not verified, or per-uploader claims | no, until verified | **excluded** |

## 3. COCO 2017: exactly what is used

### 3.1 Terms (VERIFIED, cocodataset.org/#termsofuse, read 2026-10-04)

- **Annotations.** They "belong to the COCO Consortium and are licensed
  under a Creative Commons Attribution 4.0 License".
- **Images.** The Consortium does not own their copyright. Use must abide by
  the Flickr Terms of Use. Users "accept full responsibility for the use of
  the dataset".
- **Per-image licences.** Every image record carries a `license` id into
  the file's `licenses` table: eight Flickr licences, from CC BY-NC-SA 2.0
  to "No known copyright restrictions".

### 3.2 What the pipeline takes

| Part | Used | How |
|---|---|---|
| `instances_train2017.json` (sha256 `610fce49…`) | yes | training split, filtered |
| `instances_val2017.json` (sha256 `e8c7f790…`) | yes | validation split only, filtered; never trained on |
| train2017 / val2017 images | only the filtered ones | fetched one by one from `coco_url`, sha256 recorded |
| test2017, captions, keypoints, panoptic, stuff | no | - |
| Categories | car, truck, bus, motorcycle (+ bicycle, person for `traffic6`) | relabelled 1..N |
| Other annotations in a kept image | dropped | a kept image may show a dog, but it is not labelled |

The zip `annotations_trainval2017.zip` has sha256 `113a836d…0268`
(252,907,541 bytes). COCO publishes no checksum; ours is recorded in the
dataset manifest.

### 3.3 Licence policy (matched on the licence URL, never on the id)

| Licence | Tier | Why |
|---|---|---|
| CC BY 2.0 | **allow** | commercial use and adaptation permitted; attribution on sharing |
| No known copyright restrictions (Flickr Commons) | **allow** | institution states none known; keep the source link |
| United States Government Work | **allow** | no US copyright (status abroad can differ); 0 images in COCO 2017 |
| CC BY-SA 2.0 | **review** | if weights were an adaptation, share-alike would bind them |
| CC BY-ND 2.0 | exclude | no derivatives; whether training makes one is unsettled |
| CC BY-NC 2.0, BY-NC-SA 2.0, BY-NC-ND 2.0 | exclude | non-commercial |

### 3.4 What that leaves (VERIFIED, prepare_coco_traffic.py on the full annotation files)

All of COCO train2017 (118,287 images) by licence:

| Licence | Images | Tier |
|---|---|---|
| CC BY-NC-SA 2.0 | 32,878 | exclude |
| CC BY-NC-ND 2.0 | 32,184 | exclude |
| CC BY 2.0 | 19,470 | allow |
| CC BY-NC 2.0 | 16,397 | exclude |
| CC BY-SA 2.0 | 10,901 | review |
| CC BY-ND 2.0 | 5,992 | exclude |
| No known copyright restrictions | 465 | allow |

**Only 16.9 % of COCO train2017 is usable** under the conservative policy.

Traffic subset (`traffic4`, allowed tier):

| Split | Images with a vehicle | Vehicle boxes | car | truck | bus | motorcycle | Street-context negatives available |
|---|---|---|---|---|---|---|---|
| train (train2017) | **3,077** | **11,918** | 7,575 | 1,772 | 1,099 | 1,472 | 9,223 |
| val (val2017) | 143 | 551 | 353 | 74 | 68 | 56 | 405 |

Adding CC BY-SA (`--allow-review`) gives 4,861 training images and 18,904
boxes (+58 %).

Small vehicles in the allowed train split, as box width after the 416
letterbox (cars):

| <8 px | 8-16 | 16-32 | 32-64 | 64-128 | ≥128 |
|---|---|---|---|---|---|
| 676 | 1,745 | 2,112 | 1,672 | 881 | 429 |

COCO is rich in small cars. It is poor in the DOORS scene: a narrow road
seen from a handheld or a window, often at dusk.

**Consequence:** 3,077 images is about 2.6 % of what upstream YOLOX-Tiny was
trained on. From-scratch training on that alone is expected to fall short of
the upstream checkpoint (ASSUMED; to be measured). Open Images (section 4)
is therefore part of the plan, not an option.

### 3.5 Attribution and redistribution

- **Annotations.** CC BY 4.0 credit to the COCO Consortium, stating that the
  annotations were filtered and relabelled. It goes in MODEL_LICENSES /
  NOTICE when a model ships.
- **Images.** CC BY 2.0 requires attribution when the work, or an
  adaptation of it, is shared. The pipeline writes `ATTRIBUTION.tsv`: every
  training image with its licence and Flickr URL. COCO stores no author
  names, so author-level attribution would need a Flickr lookup per image
  (open item U3).
- **Images are never redistributed.** Only the weights are.

## 4. Open Images V7 (planned, main volume source)

- **Terms (VERIFIED, factsfigures_v7 page).** "The annotations are licensed
  by Google LLC under CC BY 4.0 license. The images are listed as having a
  CC BY 2.0 license." Google adds that it makes no representations about
  each image's licence status and that users should verify it.
- **Per-image metadata (DOCUMENTED).** The image list carries `License`,
  `Author`, `AuthorProfileURL` and `OriginalURL`. That is better than COCO
  for attribution.
- **Relevant boxable classes.** Car, Truck, Bus, Motorcycle, Van, Taxi,
  Ambulance and Vehicle (parent class). The folding into the DOORS classes
  must be decided: Van and Taxi fold into car, Ambulance into truck or car,
  and the generic "Vehicle" boxes are ignored. Box counts were not measured
  (the metadata is several GB and was not downloaded).
- **To do before use:** a `prepare_openimages_traffic.py` with the same
  manifest format. It keeps only rows whose licence URL is CC BY 2.0 and
  records the author and URL per image. Not written yet; it needs a download
  approval.

## 5. DOORS camera captures (planned)

- **Purpose.** These are the **test set** for the real use case: narrow
  roads, vehicles at measured distances (10 / 20 / 35 m), partially visible
  vehicles, empty-road negatives, dusk, night, rain and low sun.
- **Ownership.** The owner shoots them, so the copyright is the owner's.
  The labels are made by the project.
- **Privacy (GDPR).** Number plates and faces are personal data.
  - Keep the raw footage local and never in git.
  - Blur it before any sharing.
  - Record the place, date and purpose.
  - The Norwegian Data Protection Authority (Datatilsynet) is the authority
    to check with if footage is ever used beyond private testing.
- **Split rule.** Test captures come from sessions and places that are
  never used for training. A later training capture must use different days
  and locations.

## 6. Splits and leakage guard

| Split | Source | Used for |
|---|---|---|
| train | COCO train2017 (filtered) + later Open Images train | training, calibration images |
| val | COCO val2017 (filtered) (+ Open Images validation) | epoch selection (best_ckpt) |
| test | DOORS captures + the 2026-10-04 evaluation set | the final comparison only |

`prepare_coco_traffic.py --exclude-sha256` drops any file whose sha256 is on
the test list. The smoke run used the 170 frames of the 2026-10-04
evaluation. Kmodel calibration uses training images only (the 2026-10-04
evaluation calibrated on COCO val2017, which is not done here).

## 7. Remaining licence uncertainties

| # | Uncertainty | Effect | Proposed handling |
|---|---|---|---|
| U1 | Whether trained weights are an "adaptation" of the training images (copyright law, unsettled in the EU and the US) | decides whether CC BY attribution, and SA terms, attach to the weights | conservative policy (no NC/ND/SA), keep attribution anyway |
| U2 | Flickr licence labels are uploader-asserted; COCO and Open Images do not verify them | a mislabelled image could slip in | accept as residual risk; keep per-image records so an image can be removed and the model retrained |
| U3 | COCO has no author names | CC BY attribution by name is not possible from COCO alone | ATTRIBUTION.tsv with Flickr URLs; optional Flickr API lookup |
| U4 | "No known copyright restrictions" is a statement, not a licence | small risk | keep (465 images), or drop with the policy file if the owner prefers |
| U5 | CC BY-SA 2.0 images (+58 % COCO data) | share-alike may bind the weights | owner decision; default off |
| U6 | Open Images: Google disclaims the per-image licence status | as U2 | as U2 |
| U7 | The EU text-and-data-mining exception (DSM Directive art. 4) and its opt-out, and Norway's implementation | could permit or limit training on lawfully accessed images | not relied on; noted only |
| U8 | Privacy in DOORS captures | GDPR obligations | section 5 |
| U9 | YOLOX upstream checkpoint terms | not used, so no effect | none |
