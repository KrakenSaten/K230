# R0 training dataset: `traffic6_r0` (built and audited 2026-10-06)

Status: **research.** No training has run on this dataset. Production
Vision, `yolov8n.kmodel` and the nncase configuration are unchanged.

Built on the office PC (Windows 11, Arc B580) with:
- `tools/vision/training/data/build_traffic_dataset.py`
- `tools/vision/training/data/audit_traffic_dataset.py`

The licence terms and the Open Images decision are in
[DATASET_PROVENANCE.md](DATASET_PROVENANCE.md) section 4.

Machine-readable records committed beside this file:
- [`data/traffic6_r0/manifest.json`](data/traffic6_r0/manifest.json): rules, inputs, versions, counts, hashes
- [`data/traffic6_r0/stats.json`](data/traffic6_r0/stats.json): the audit

The images and the per-image manifests stay in the work directory (`C:\K230-work\yolox-train`), outside git.

Evidence: every number here is VERIFIED, measured on the built dataset,
unless marked otherwise.

## 1. Class mapping (one table, used everywhere)

| class_id | category_id (json) | name | DOORS index (export) | COCO category | Open Images labels folded in |
|---|---|---|---|---|---|
| 0 | 1 | car | 2 | 3 car | Car, Limousine, Van, Taxi |
| 1 | 2 | truck | 7 | 8 truck | Truck |
| 2 | 3 | bus | 5 | 6 bus | Bus |
| 3 | 4 | motorcycle | 3 | 4 motorcycle | Motorcycle |
| 4 | 5 | bicycle | 1 | 2 bicycle | Bicycle |
| 5 | 6 | person | 0 | 1 person | Person, Man, Woman, Boy, Girl |

- This is `traffic6` in `data/class_sets.py` and in the exp's `CLASS_SETS`.
- Train with `DOORS_CLASS_SET=traffic6`. The exp refuses a dataset made
  for another class set, and `dataset_checks.py` refuses any other mapping.
- VERIFIED: YOLOX's own `COCODataset` loaded `traffic_train.json` (42,888
  items) and `traffic_val.json` (1,615 items) with category ids 1-6 and
  these names.

## 2. Sources

| Source | Status | Candidates | Included | Rejected | Quarantined after download |
|---|---|---|---|---|---|
| COCO 2017, licence tier `allow` (CC BY 2.0, Flickr Commons) | approved (policy unchanged) | 73,624 | 12,822 | 60,784 | 18 |
| Open Images V7, per-image rules | approved under the existing policy (DATASET_PROVENANCE 4.4) | 149,522 | 35,455 | 113,826 | 241 |
| DOORS camera captures | none exist locally | - | 0 | - | - |
| Any other dataset | not introduced | - | 0 | - | - |

COCO rejections:
- licence tier exclude (NC/ND): 54,142
- tier review (CC BY-SA): 6,634
- stricter Flickr licence today: 8

Open Images rejections:
- person never verified: 80,445
- `Rotation` not 0: 18,264
- Vehicle, Land vehicle or Ambulance box: 7,589
- depiction: 6,899
- inside view: 2,992
- stricter Flickr licence today: 3
- licence other than CC BY 2.0: 0

Open Images images that have only person boxes are outside the selection
rule: 809,637, not downloaded.

COCO candidates are the images with a target box, under any licence, plus
the street-context negatives that were sampled.

Included images by licence: CC BY 2.0 47,876; "No known copyright
restrictions" (Flickr Commons, COCO) 401.

## 3. Splits

| Split | Images | COCO | Open Images | Trainable boxes | Crowd/group boxes (ignored by YOLOX) | No-target images |
|---|---|---|---|---|---|---|
| train | 42,888 | 10,408 | 32,480 | 262,050 | 8,907 | 482 |
| val | 1,615 | 913 | 702 | 6,762 | 290 | 34 |
| test (held out) | 3,774 | 1,501 | 2,273 | 15,384 | 910 | 58 |

Rules (deterministic; no manual choice; seed 20261004):
- **Open Images.** Its own subsets: train to train, validation to val,
  test to test.
- **COCO.** Let h be the first 8 hex digits of
  sha256("doors-split:SEED:coco-SRC:ID"). For val2017: h mod 2 = 0 goes to
  val, 1 to test. For train2017: h mod 20 = 0-1 goes to test, 2 to val,
  the rest to train.
  - The train2017 holdout is there because Open Images' selected
    validation/test images are mostly close-up vehicles.
  - Without it, the held-out test had 115 small or very small cars. With
    it, 414 (section 5).
- **COCO negatives.** Street-context images with no target (traffic
  light, stop sign, parking meter, hydrant, bench). They are sampled at
  0.15 per positive with the seed. Only 552 + 22 exist in the allowed tier,
  so all are used.

**Held-out test protection.**
- The test images are in `test/` and `annotations/traffic_test.json`.
  `exps/yolox_tiny_traffic_416.py` reads only `traffic_train.json` and
  `traffic_val.json` (VERIFIED).
- `test_set.sha256` lists the test files for `--exclude-sha256` in later
  builds.
- The test split must not be used for:
  - training or augmentation tuning
  - threshold tuning
  - checkpoint selection (`best_ckpt.pth` uses val)
  - calibration (training images only)

## 4. Class totals and balance

| Class | Images | Boxes | % of boxes | Boxes train / val / test | Train share | COCO / OI boxes | Full COCO train2017 boxes (reference) |
|---|---|---|---|---|---|---|---|
| car | 21,934 | 56,131 | 19.75 | 51,419 / 1,301 / 3,411 | 91.6 % | 7,836 / 48,295 | 43,533 |
| truck | 3,114 | 4,525 | 1.59 | 3,890 / 205 / 430 | 86.0 % | 1,845 / 2,680 | 9,970 |
| bus | 1,979 | 3,098 | 1.09 | 2,753 / 130 / 215 | 88.9 % | 1,159 / 1,939 | 6,061 |
| motorcycle | 4,592 | 9,575 | 3.37 | 8,961 / 171 / 443 | 93.6 % | 1,513 / 8,062 | 8,654 |
| bicycle | 10,696 | 26,372 | 9.28 | 25,667 / 178 / 527 | 97.3 % | 1,145 / 25,227 | 7,056 |
| person | 42,994 | 184,495 | 64.92 | 169,360 / 4,777 / 10,358 | 91.8 % | 46,199 / 138,296 | 257,253 |

- **Imbalance (flagged).** Train boxes, max over min:
  - all six classes: 61.5 (person over bus)
  - the four vehicle classes: 18.7 (car over bus)
  - full COCO train2017, for comparison: 42.4 and 7.2
- **Weakest classes: bus and truck.**
  - bus: 2,753 train boxes in 1,759 images
  - truck: 3,890 in 2,650
  - That is 45 % and 39 % of the full-COCO count. Upstream YOLOX-Tiny,
    the 2026-10-04 reference with vehicle recall 0.90, learnt these
    classes from the full COCO count.
  - Every other class has 66 % (person) to 364 % (bicycle) of its
    full-COCO count.
- **Bicycle is almost all Open Images train.** Val has 178 bicycle boxes
  and test 527.
- The reference counts are non-crowd boxes from `instances_train2017.json`
  (VERIFIED).

## 5. Object size and small-target coverage

**Convention.**
- side416 = sqrt(w × h) × 416 / max(W, H): the box's geometric-mean side
  in model pixels after YOLOX's 416 letterbox. It is normalised by the
  image size, so all source resolutions compare.

| Bucket | side416 | Why this edge |
|---|---|---|
| very small | < 8 px | smaller than one cell of YOLOX's finest output (stride 8) |
| small | 8-16 px | one to two stride-8 cells |
| medium | 16-64 px | |
| large | >= 64 px | two or more stride-32 cells |

These are not COCO's area thresholds (32² and 96² px at the original
resolution). The normalised area, w × h / (W × H), is reported beside
them.

Trainable boxes:

| Class | Split | Very small | Small | Medium | Large | side416 p5 / p50 / p95 | Area % p5 / p50 / p95 |
|---|---|---|---|---|---|---|---|
| car | train | 2,271 | 5,764 | 22,986 | 20,398 | 8.5 / 48.4 / 276.6 | 0.060 / 1.93 / 63.3 |
| car | val | 76 | 176 | 408 | 641 | 7.7 / 62.3 / 322.8 | 0.047 / 3.08 / 86.2 |
| car | test | 115 | 299 | 984 | 2,013 | 9.4 / 95.4 / 327.6 | 0.072 / 7.32 / 87.0 |
| truck | train | 40 | 199 | 1,120 | 2,531 | 14.5 / 110.1 / 305.0 | 0.176 / 9.88 / 74.6 |
| truck | test | 4 | 29 | 107 | 290 | | |
| bus | train | 11 | 78 | 655 | 2,009 | 19.3 / 136.1 / 319.7 | 0.306 / 15.6 / 81.6 |
| bus | test | 2 | 8 | 50 | 155 | | |
| motorcycle | train | 71 | 324 | 3,012 | 5,554 | 17.1 / 84.7 / 279.3 | 0.244 / 6.02 / 62.9 |
| motorcycle | test | 22 | 28 | 110 | 283 | | |
| bicycle | train | 526 | 1,764 | 10,829 | 12,548 | 11.8 / 62.5 / 201.8 | 0.116 / 3.24 / 32.7 |
| bicycle | test | 3 | 14 | 134 | 376 | | |
| person | train | 7,570 | 21,600 | 83,481 | 56,709 | 8.4 / 41.2 / 185.2 | 0.058 / 1.41 / 27.5 |
| person | test | 593 | 1,373 | 4,955 | 3,437 | | |

The full tables are in `stats.json` (`object_size`):
- val for every class
- width and height histograms at 416
- the area-% histogram
- truncation and border contact

**Small-vehicle scenes (car, truck, bus, motorcycle under 16 px):**

| Split | Images with one | Images with 3 or more | Where they sit (centre y, 0 = top) |
|---|---|---|---|
| train | 3,711 | 1,045 | 0-0.2: 907, 0.2-0.4: 2,465, 0.4-0.6: 3,360, 0.6-0.8: 1,656, 0.8-1: 370 |
| val | 104 | 35 | 20 / 71 / 80 / 74 / 42 |
| test | 180 | 63 | 45 / 103 / 219 / 117 / 23 |

**Partially visible objects.**
- Open Images flags truncation. The flag is set on 15,318 of 45,092
  Open Images car boxes in train (34 %), and 6,589 of 24,760 bicycle
  boxes.
- COCO has no truncation flag. As a proxy, boxes touching the image
  border: 15,686 car boxes in train, all sources.
- Open Images occlusion flags are in `stats.json`.

**What this proves and what it does not.**
- No source has distance labels. This is **small-object coverage**, a
  proxy for distant vehicles. It is not distance coverage.
- Small cars are well covered in training: 8,035 boxes under 16 px.
  The held-out test can measure small-car recall on 414 boxes.
- Small trucks and buses are scarce:
  - train: 239 and 89 boxes
  - test: 33 and 10 boxes
  - Small-object recall for them cannot be measured with useful precision
    on this test split.

## 6. Negatives and difficult scenes

| Split | No-target images | % | Dark (mean grey < 60) | Vehicle boxes in dark images | Low contrast (grey std < 30) | Vehicle boxes in them |
|---|---|---|---|---|---|---|
| train | 482 | 1.12 | 1,475 | 1,824 | 340 | 167 |
| val | 34 | 2.11 | 59 | 50 | 23 | 9 |
| test | 58 | 1.54 | 130 | 129 | 37 | 11 |

- **Gap: negatives.**
  - Clean no-target images are about 1 % of training: only COCO
    street-context pictures.
  - There are no road scenes without vehicles in quantity.
  - Open Images gives no trustworthy negatives. An image without a target
    box can still hold unverified, unboxed targets.
  - None were manufactured.
- **Difficult scenes.**
  - Dark and low-contrast are pixel statistics, a proxy for night and
    haze, not labels. Neither source labels weather or night.
  - About 1,800 vehicle boxes sit in dark training images.
  - Rain, fog and dusk are not measurable here.

## 7. Quality, duplicates and leakage

| Check | Result |
|---|---|
| Broken or undecodable files (full Pillow decode of every file) | 0 |
| Download failures (48,536 fetched) | 0 |
| EXIF orientation other than 1 (cv2 would rotate the pixels, not the boxes) | 0 |
| COCO size mismatch (file versus annotation) | 0 |
| Exact duplicates (same sha256) | 0 |
| Same Flickr photo in COCO and Open Images | 73 removed (72 train, 1 test) |
| Near duplicates across splits (dHash, Hamming distance 4 or less) | 25 removed (23 train, 2 val); the copy in the more protected split was kept |
| Near-duplicate pairs inside one split | 177 (175 train, 2 test), reported, kept |
| Cross-split leakage after the build (sha256, uid, source id, Flickr id) | 0 (`dataset_checks.py`) |
| Positive images left without a trainable box (only group boxes) | 161 quarantined |
| Annotation fixes | 443 duplicate boxes dropped (same class, IoU 0.95 or more; mostly Car+Taxi or Person+Man pairs); 7 boxes under 1 px dropped |
| Invalid class ids, boxes outside the image | 0 after the fixes (checked) |
| Re-hash of all 48,277 files against the manifests | 0 differences |

**Near-duplicate method.**
- 64-bit dHash: grey image, Pillow bilinear resize to 9 x 8, compare
  neighbouring pixels.
- Pairs at a Hamming distance of 4 or less are duplicates. The search is
  exact: banding by the pigeonhole principle, checked against brute force
  in a test.

**Threshold check.**
- All 73 known same-photo pairs (the same Flickr photo, encoded
  separately by COCO and Open Images) lie at a distance of 0-4: 42 at 0,
  19 at 1, 8 at 2, 3 at 3, 1 at 4. Recall on that ground truth is 73/73.
- Of 4 removed pairs inspected by eye, 1 was a true near duplicate and 3
  were unrelated pictures with a similar coarse layout. Each of those cost
  one training image.
- Of 5 unflagged cross-split pairs at a distance of 5, none was a
  duplicate.
- The method does not find crops or heavy edits.

**Finding: Open Images is not exhaustively labelled** (DOCUMENTED by Open
Images; measured here). It boxes only classes a human verified in the
image.
- Person: the build requires it to be verified.
- Other classes, in included Open Images images (neither boxed nor
  verified):

  | Split | car | truck | bus | motorcycle | bicycle |
  |---|---|---|---|---|---|
  | train | 31 % | 94 % | 93 % | 88 % | 61 % |
  | test | 10 % | 89 % | 97 % | 90 % | 86 % |

- **Training.** Such an image can hold an unboxed vehicle, which teaches
  suppression. Requiring car to be verified as well would drop 9,982 train
  images and 85 % of the bicycle boxes (25,667 to 3,887). It was not
  applied; it is a candidate ablation.
- **Evaluation.** On Open Images val/test, a detection of a class not
  verified in that image must not count as a false positive. Open Images'
  own protocol works this way. The manifests carry each image's
  `human_labels` for it.
- COCO is exhaustively labelled for all six classes.

## 8. Licence and provenance

| | Images |
|---|---|
| Approved and included | 48,277 (COCO 12,822, Open Images 35,455) |
| Rejected | 174,610 (COCO 60,784, Open Images 113,826) |
| Quarantined after download (file, duplicate, annotation reasons; not licence) | 259 |
| Included images without a provenance record, licence URL, source URL, or (Open Images) author | 0 (`dataset_checks.py`) |
| Rejected or quarantined images in any manifest | 0 (checked) |

- **Machine-readable records.**
  - `provenance.jsonl` holds every candidate: source, id, licence and URL,
    author, original and landing URL, annotation source, decision and
    reasons.
  - `manifest_{train,val,test}.jsonl` hold every included image with:
    - its provenance fields
    - the local path, split, size, sha256 and dHash
    - the boxes, with class id, class name and source label
  - `ATTRIBUTION.tsv` lists the training images with licence, author
    (Open Images) and source URL.
- **Uncertain.** No image is included as "uncertain". The pipeline has
  only include, reject and quarantine.
  - Every included image's licence is uploader-asserted: U2, U6 and U10
    apply to all of them alike.
  - 38 of the 200 spot-checked images could not be confirmed today
    (deleted or not public). They stay included on their dataset record,
    like the 48,000 unsampled images.
  - The 11 with a stricter licence today were rejected.

**Open Images conclusion.**
- Its per-image evidence is at least that of the COCO tier the policy
  already accepts:
  - the licence URL
  - the author
  - the Flickr page
  - spot check: 80/84 unchanged, against COCO's 70/78
- It is used under the existing policy, with the per-image rules of
  DATASET_PROVENANCE 4.4.
- The decision is reversible. `--no-openimages` rebuilds without it, and
  every image can be traced and removed.

**Remaining legal and provenance uncertainties.**
- U1: weights as an adaptation
- U2/U6: uploader-asserted labels, and the providers' disclaimers
- U10: 5-10 % of sampled labels are stricter today
- U11: Open Images files are re-encoded copies
- U7: TDM exception, not relied on

This is an engineering record, not legal advice.

## 9. Narrow-road relevance (DOORS camera)

The third-party data shows:
- many small cars (8,035 train boxes under 16 px)
- 1,045 training images with three or more small vehicles
- small vehicles spread over the whole image height, most in the middle
  band

It cannot show:
- the DOORS geometry: a narrow road filling a thin horizontal band
- the DOORS camera, optics, compression and mounting
- the DOORS lighting (dusk, night, rain, low sun)
- vehicles at known distances
- partially hidden vehicles at the road edge
- clean empty-road negatives

All of that needs the DOORS-specific held-out set (DATASET_PROVENANCE 5,
TRAINING_PLAN 4). No DOORS captures exist on this machine. None were
created.

## 10. Reproducibility

- **Command.** README step 4b. Seed 20261004. Workers do not change the
  output.
- **Inputs.** The sha256 of every metadata file is in `manifest.json`
  `inputs`:
  - COCO: `instances_train2017.json` `610fce49…`,
    `instances_val2017.json` `e8c7f790…`
  - Open Images: the 11 files of `download_v7.html`, 3.48 GB
- **Policy files.**
  - `coco_licence_policy.json` and the spot-check file are hashed with LF
    line ends, so a CRLF checkout gives the same hash.
- **Tools.** Python 3.10.20, Pillow 12.3.0, both recorded.
- **Determinism (VERIFIED).** A second, independent build from the same
  cache (`traffic6_r0_repro`) gave byte-identical files:
  - the three split manifests
  - the three annotation files
  - `provenance.jsonl`, `quarantine.jsonl`
  - `ATTRIBUTION.tsv`, `test_set.sha256`
  - the near-duplicate list and the image tree

  `manifest.json` differed only in the `--out` directory name in
  `command`.

| File | sha256 |
|---|---|
| manifest_train.jsonl | `5d48a30890a16fdd60625a0a9b0edee649810bd32ef6398ab52b31f37406f22d` |
| manifest_val.jsonl | `63b1ec5308fc77db860f85d1d189d9c4f7af3632d1cd136dc635ac8e2ec21562` |
| manifest_test.jsonl | `0599a4d14a20827be0b1769fd1f162823132c9de17f9f0c1eb04ee407efe3e74` |
| annotations/traffic_train.json | `f68474b0c34593f24934830a882a995ad8ba2e8233b6b1d94857d3ef1fba50fa` |
| annotations/traffic_val.json | `384c6abecb0d6470102d985e6c3c6909f6f553d0a8193baec868f48e34d538ad` |
| annotations/traffic_test.json | `67d3b1cc00fb21ef69c127129d957cab4feb8e4e54e1fef4176bd8705a844656` |
| provenance.jsonl | `199d7a060eb1ab59bfdc83ceba81c798a1ab56cfcecad437987d47a3f12643b3` |
| test_set.sha256 | `789367c68a5df6edaf21d3bb4fd11e1f38669c9f2d3fe2336a0d3ba1fd4633c1` |

The build fetches only missing files. It takes about 130 s from the cache
(16 workers) and about 1 h with downloads.

## 11. Disk

| | Size |
|---|---|
| Open Images metadata (11 files) | 3.48 GB |
| COCO annotations zip + 2 JSON | 0.74 GB (from 2026-10-04) |
| Downloaded images: COCO 12,848 / Open Images 35,699 | 2.13 GB / 12.36 GB |
| Prepared dataset (hard links into the cache + annotations + manifests) | 0.30 GB extra (14.70 GB apparent) |

## 12. Readiness gates

| Gate | Result | Evidence |
|---|---|---|
| A CLASS COVERAGE | PASS | All six classes in every split. Train: at least 2,753 boxes in at least 1,759 images per class (bus). Val: at least 130 boxes (bus). Test: at least 215 (bus). Bus and truck carry 45 % and 39 % of the full-COCO count; the other classes 66-364 %. Imbalance is flagged: max over min is 61.5 overall and 18.7 for vehicles. For an initial R0 run that measures the floor, the counts give each class a measurable AP. Bus and truck are the expected weak spots. |
| B SMALL-OBJECT COVERAGE | PASS (proxy only) | Train: 8,035 car boxes under 16 px (2,271 under 8), 3,711 images with a small vehicle, 1,045 with three or more. Test: 414 small or very small cars. Limits: small truck and bus are scarce (train 239 / 89, test 33 / 10). No distance labels. Not the DOORS geometry. |
| C TRAIN/VAL/TEST SEPARATION | PASS | Fixed rules (section 3): 42,888 / 1,615 / 3,774. The exp never reads the test split. `test_set.sha256` written. |
| D DUPLICATE/LEAKAGE CHECK | PASS | 0 exact duplicates. 73 same-photo and 25 cross-split near duplicates removed. 0 leakage in sha256, uid, source id and Flickr id. 0 broken files, all re-hashed. dHash recall 73/73 on known duplicates. |
| E LICENCE/PROVENANCE | PASS | 48,277 included: all CC BY 2.0 (47,876) or Flickr Commons (401), with full provenance. 174,610 rejected with reasons. The checker proves rejected images are not in any manifest. Open Images is accepted on evidence equal to the approved COCO tier. Residual U2/U6/U10/U11 risk is documented, not removed. |
| F MANIFEST REPRODUCIBILITY | PASS | The independent rebuild is byte-identical (section 10). Inputs, rules, seed, tools and hashes are recorded. |

**Gaps that are not gates, carried into R0:**
- Few clean negatives (about 1 %).
- Open Images label incompleteness (section 7). Evaluation on Open Images
  val/test must ignore unverified classes.
- The 2026-10-04 evaluation frames are not on this PC. No dataset file
  matched any of the 468 distinct local pictures (repo and vendor SDK
  trees). The frames themselves must be hash-checked against
  `manifest_*.jsonl` before `eval/compare.sh` results are trusted.
- No DOORS-specific test set.

## 13. Tests

`tests/test_dataset_pipeline.py`: 19 tests, about 1 s, synthetic fixture,
no network.
- Covered:
  - deterministic rebuild and the split rule
  - the class mapping (COCO ids and Open Images labels to class ids)
  - manifest parsing
  - provenance fields
  - rejected images (NC, BY-SA, wrong Open Images licence, unverified
    person) absent from manifests
  - an exact duplicate planted across splits: test keeps it, train loses it
  - a near duplicate planted across splits (re-encoded, brightened)
  - a missing file
  - box validation (duplicate, outside, under 1 px, invalid class)
  - the dHash banding against brute force
- The checker on broken copies must name each planted defect:
  - a row duplicated into another split
  - a provenance record removed
  - a rejected image put in a manifest
  - category id 7
  - a missing file and a changed file
  - a corrupt manifest line
