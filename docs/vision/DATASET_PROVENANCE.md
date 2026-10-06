# Dataset provenance: DOORS Traffic detector (YOLOX-Tiny 416)

Status: **research, 2026-10-04; Open Images and the R0 dataset added 2026-10-06**. This is an engineering record, not legal
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
| Open Images V7 | annotations CC BY 4.0; images "listed as" CC BY 2.0 (VERIFIED) | yes, per-image filter (section 4) | **used** (from 2026-10-06, R0 dataset) |
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

## 4. Open Images V7 (audited 2026-10-06; used for the R0 dataset)

The full numbers and the readiness gates are in
[DATASET_TRAFFIC6_R0.md](DATASET_TRAFFIC6_R0.md). This section records the
terms, the evidence per image and the decision.

### 4.1 Terms (VERIFIED, factsfigures_v7.html, read 2026-10-06)

- **Annotations.** Licensed by Google LLC under CC BY 4.0.
- **Images.** "listed as having a CC BY 2.0 license".
- **Google's disclaimer.** Google tried to identify CC BY images but makes
  no representations or warranties about each image's licence status. The
  user should verify the licence of each image.

### 4.2 Evidence per image (VERIFIED, read from the metadata files)

The image CSVs (`train-images-boxable-with-rotation.csv` etc.) carry, per
image: `ImageID`, `OriginalURL` (Flickr file), `OriginalLandingURL` (Flickr
photo page), `License` (URL), `Author`, `AuthorProfileURL`, `Title`,
`OriginalSize`, `OriginalMD5`, `Rotation`.

| Evidence | COCO allow tier (used since 2026-10-04) | Open Images V7 |
|---|---|---|
| Licence per image | `license` id into the file's table (Flickr label) | `License` URL (Flickr label) |
| Who asserted the licence | the Flickr uploader | the Flickr uploader |
| Provider's warranty | none: users "accept full responsibility" | none: "no representations or warranties" |
| Author name | no | yes (`Author`, `AuthorProfileURL`) |
| Flickr photo page | no (only the static file URL) | yes (`OriginalLandingURL`) |
| Candidates in the Traffic selection with a CC BY 2.0 label | 19,470 of 118,287 train2017 images | 149,522 of 149,522 (100 %) |
| Candidates without author or source URL | n/a (never recorded) | 0 |

- **The stored file is not the Flickr original.** The CVDF mirror's files
  are re-encoded: for 2 of 2 checked images the MD5 differs from
  `OriginalMD5` and the file is smaller. The link from a stored file to its
  Flickr photo is the `ImageID` record, not byte identity. The pipeline
  records its own sha256 per file. (COCO's files are COCO's own copies
  too.)

### 4.3 Spot check of today's Flickr licence (VERIFIED 2026-10-06)

`provenance/flickr_licence_spotcheck.py` asked Flickr's public oEmbed
endpoint for the current licence of a fixed sample: 100 selected images per
source, chosen by sha256 of the seed and the image uid. The result is
committed as `provenance/flickr_spotcheck_2026-10-06.json`.

| | COCO allow tier | Open Images V7 |
|---|---|---|
| Same licence today | 70 | 80 |
| More restrictive today | 8 (5 All Rights Reserved, 3 BY-NC-ND) | 3 (All Rights Reserved, BY-NC-SA, BY-SA) |
| More permissive today | 0 | 1 (CC0) |
| Photo deleted or not public | 19 | 15 |
| No usable answer | 3 | 1 |

- A first attempt read the photo page HTML. Flickr sends anonymous clients
  to Explore, so that HTML held other photos' licences. Those results were
  discarded.
- CC licences cannot be revoked for copies already obtained under them. A
  stricter licence today therefore does not show that the label was wrong
  at collection time, but it does not show that it was right either.
- **Handling.** The 11 sampled images with a stricter licence today
  (8 COCO, 3 Open Images) are rejected: `flickr_licence_now_restrictive`.
  Deleted photos keep their dataset record as the only evidence, as do the
  unsampled images.
- **What it shows.** Open Images' labels hold up at least as well as the
  COCO tier this policy already accepts. In both sources about 5-10 % of
  the still-visible photos carry a stricter licence today (new
  uncertainty U10).

### 4.4 Decision

Under the rule in section 1 and the accepted residual risk U2, an Open
Images image meets the same evidence standard as a COCO allow-tier image. It
also carries more attribution data. Open Images is therefore **used**, per
image and only under these rules (`data/openimages_traffic.py`). The images
are not called licence-safe: U2, U6 and U10 apply to them as to COCO.

| Rule | Rejection reason |
|---|---|
| `License` is exactly `https://creativecommons.org/licenses/by/2.0/` | `licence_not_cc_by_2.0` |
| `Author` and `AuthorProfileURL` present | `missing_author` |
| `OriginalURL` and `OriginalLandingURL` present | `missing_original_url` |
| Spot check shows no stricter licence today | `flickr_licence_now_restrictive` |

Annotation rules (not licence; they keep wrong or missing boxes out):

| Rule | Rejection reason |
|---|---|
| `Rotation` is 0 (nan or 90/180/270 means the boxes may not match the stored pixels) | `rotation_not_zero` |
| No Vehicle, Land vehicle or Ambulance box (a target that maps to no single class) | `ambiguous_vehicle_box` |
| No target box that is a depiction (drawing, toy, poster) | `depiction` |
| No target box taken from inside the object | `inside_view` |
| Person (or Man/Woman/Boy/Girl) human-verified, present or absent: Open Images boxes only verified classes, so an unverified person may be unboxed | `person_unverified` |
| Group-of boxes are kept as `iscrowd=1`; YOLOX ignores them, like COCO crowd boxes | - |

**Class folding** follows Open Images' own hierarchy
(`bbox_labels_600_hierarchy.json`): Car, Limousine, Van (children of Car)
and Taxi go to car. Man, Woman, Boy and Girl (children of Person) go to
person. Truck, Bus, Motorcycle and Bicycle map one to one.

**Selection rule.** An image is a candidate when it has a box of car, truck,
bus, motorcycle or bicycle. Person-only images are outside the rule:
809,637 of them, not downloaded. COCO already supplies persons.

**Splits.** These are Open Images' own: train to train, validation to val,
test to test.

| Subset | Candidates | Selected | Main rejection reasons |
|---|---|---|---|
| train | 126,504 | 32,640 | person_unverified 70,417; rotation 15,896; depiction 6,324; inside 2,326 |
| validation | 5,768 | 716 | person_unverified 2,602; ambiguous 1,791; rotation 620 |
| test | 17,250 | 2,340 | person_unverified 7,426; ambiguous 5,593; rotation 1,748 |

The counts overlap: one image can fail several rules. The exact counts per
reason are in the dataset's `stats.json`.

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

The R0 dataset (`build_traffic_dataset.py`, 2026-10-06):

| Split | Source | Used for |
|---|---|---|
| train | COCO train2017 (allow tier) + Open Images train | training, calibration images |
| val | half of COCO val2017 (allow tier) + Open Images validation | epoch selection (best_ckpt) |
| test (held out) | the other half of COCO val2017 + Open Images test | the final comparison only; the exp never reads it |
| DOORS test (later) | DOORS captures + the 2026-10-04 evaluation set | the real use case; not built yet |

- **COCO val2017 split.** The first 8 hex digits of
  sha256("doors-split:SEED:coco-val2017:ID"), taken mod 2: 0 goes to val,
  1 to test.
- **Leakage guard.** These are removed:
  - the same sha256 in two places
  - the same Flickr photo id in COCO and Open Images
  - a near duplicate across splits (64-bit dHash, Hamming distance 4 or less)

  The copy kept is chosen by split (test, then val, then train), then by
  source (COCO first), then by uid. `dataset_checks.py` fails the build if
  any sha256, uid, source id or Flickr id is in two splits.
- **Exclusion list.** `--exclude-sha256` still drops listed files. The
  2026-10-04 evaluation frames are not on the office PC, so the R0 build
  ran without that list (DATASET_TRAFFIC6_R0.md, limitations). Those
  frames are vendor pictures and DOORS camera frames, not COCO or Open
  Images files, but this was not checked by hash.
- **Calibration.** Kmodel calibration uses training images only.

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
| U10 | Licence label drift, measured 2026-10-06: of the still-visible sampled photos, 8/78 (COCO allow tier) and 3/84 (Open Images) carry a stricter Flickr licence today | the label at collection time cannot be confirmed; about 5-10 % of images may have been, or become, mislabelled | sampled ones with contrary evidence rejected; residual risk as U2; a full per-image check would take about 48,500 oEmbed calls (not done) |
| U11 | The CVDF mirror's Open Images files are re-encoded (MD5 differs from `OriginalMD5`) | a stored file cannot be shown byte-identical to the Flickr original | the ImageID record links them; own sha256 recorded |
