# R0's missed medium cars on val: diagnostic (2026-10-16, exploratory)

**Status: exploratory.** The val split has been used to select checkpoints and
to judge R1-A and R1-B. It is now also used for diagnosis. Nothing here is
confirmatory evidence.

Unchanged: no training, no change to thresholds, matching rules, models or
recorded verdicts. The test split was not read.

Records:
- [`data/traffic6_r0/r0_medium_miss_diag.json`](data/traffic6_r0/r0_medium_miss_diag.json)
- script `tools/vision/training/eval/diag_missed_medium.py`
- per-car records and the review sheets in `C:\K230-work\yolox-train\diag\r0_medium_val`
  (not in git: they hold dataset imagery)

## 1. Evaluation basis

| | |
|---|---|
| Checkpoint | R0 `best_ckpt.pth`, epoch 299, sha256 `a501b84c…` |
| Data | `traffic6_r0` val: `traffic_val.json` `384c6abe…`, `manifest_val.jsonl` `63b1ec53…` |
| Metric | custom mixed metric (`eval_mixed.py`): car scored on COCO images and on Open Images images with Car human-verified |
| Medium | side416 = sqrt(w·h)·416/max(W, H) in [16, 64) |
| Hit | pycocotools greedy matching by score, area "all", maxDets 100, IoU ≥ 0.5, car score ≥ 0.35 |
| Result reproduced | 408 medium cars, 212 detected, **196 missed** (262 COCO / 146 Open Images; missed 131 / 65) |

**Raw outputs.** The saved detections are post-NMS. YOLOX keeps only each
row's argmax class, with score = objectness × that class's confidence,
then class-aware NMS at 0.65.
- The unchanged checkpoint was rerun on val through `exp.get_eval_loader`.
  The decoded head rows were kept for the affected images.
- **Parity.** The rerun's post-NMS detections equal the saved ones: 33,102
  detections, max score and box difference 0.0.

## 2. Categories (mutually exclusive, first match in this order)

"Row" means a pre-NMS head output. Car score = objectness × car confidence.

| # | Category | Definition | Count |
|---|---|---|---|
| 1 | matching conflict | a final car detection ≥ 0.35 at IoU ≥ 0.5 was matched to another box | 1 |
| 2 | NMS suppressed | a row at IoU ≥ 0.5 with argmax car and score ≥ 0.35 exists, but no final detection | 2 |
| 3 | wrong class | a row at IoU ≥ 0.5 has a non-car argmax and score ≥ 0.35 | 1 (truck) |
| 4 | poor localisation | a row aimed at the car (its best-IoU car box) with argmax car and score ≥ 0.35 reaches only IoU 0.1-0.5 | 43 |
| 5 | below threshold | best car score among rows at IoU ≥ 0.5 is 0.10-0.35 | 62 |
| 6 | no useful candidate | that best score is < 0.10, or no row reaches IoU 0.5 | 87 |

**Categories 5 and 6, by the factor that falls short** (objectness ≤ car
confidence counts as low objectness):

| | Low objectness | Low car confidence | No row at IoU 0.5 |
|---|---|---|---|
| below threshold (62) | 61 | 1 | |
| no useful candidate (87) | 70 | 3 | 14 |

**What the rows show (measured).**
- **Below threshold:** the best row is well placed and called car, but
  objectness is low.
  - median IoU 0.72, all 62 with argmax car
  - car confidence p25/50/75 = 0.62 / 0.68 / 0.71
  - objectness 0.21 / 0.34 / 0.41
- **No useful candidate, with a located row (73):** also placed and mostly
  called car, but objectness is near zero.
  - median IoU 0.66, 61 with argmax car
  - car confidence 0.37 / 0.53 / 0.63
  - objectness 0.01 / 0.03 / 0.08
- **No row at IoU 0.5 (14):** small cars, side416 median 19.9; best row
  IoU 0.36-0.46.
- **Poor localisation (43):** confident car rows (score median 0.57) at
  IoU 0.21-0.43.
  - 27 of 43 overlap another labelled car (IoU ≥ 0.1)
  - 35 of 43 are COCO

**Dominant mechanism (measured).** In **135 of 196 misses (69 %)** a
well-placed box exists and its class is car or near-car, but **objectness
is too low**. This is the failure seen on the 2026-10-04 street frames,
reused here only as context. It also holds broadly on val.

## 3. Overlapping descriptors (measured, missed vs detected medium cars)

| | Missed (196) | Detected (212) |
|---|---|---|
| side416 median | 26.7 | 37.3 |
| overlaps another car (IoU ≥ 0.1) | 23 % | 27 % |
| ≥ 30 % covered by other-class boxes | 39 % | 28 % |
| touches image border | 22 % | 35 % |
| Open Images "occluded" flag | 23 % | 22 % |
| Open Images "truncated" flag | 13 % | 17 % |

**Recall by size within "medium"** (COCO / Open Images):

| side416 | COCO | Open Images |
|---|---|---|
| 16-24 | 0.30 (29/97) | 0.38 (8/21) |
| 24-32 | 0.52 (29/56) | 0.34 (11/32) |
| 32-48 | 0.71 (52/73) | 0.61 (28/46) |
| 48-64 | 0.58 (21/36) | 0.72 (34/47) |

Size is the strongest measured factor.

**Not associated (measured, size-stratified where noted).**
- An image with a car group-of box: recall 0.56 (19/34) against 0.52.
- A labelled object ≥ 10× the car's area in the image: 16-32 px 0.37 vs
  0.39; 32-64 px 0.66 vs 0.68.
- The Open Images occluded flag (above).

**Training data fact (measured, not linked to val misses).**
- 2,588 car group-of boxes in 2,245 training images, 2,196 of them Open
  Images, median 10.6 % of the image.
- YOLOX's loader drops crowd boxes (`coco.py:96`), so those regions are
  trained as background.

## 4. Visual review (26 misses, stratified, seed 20261016; judgements are uncertain)

| Category | Sampled |
|---|---|
| no useful candidate | 9 |
| below threshold | 7 |
| poor localisation | 6 |
| NMS suppressed | 2 |
| wrong class | 1 |
| matching conflict | 1 |

**Observed (visual, uncertain).**
- **Background cars partly hidden by unlabelled occluders.** Most misses are
  cars behind people, poles, fences, foliage or other cars: a night street
  behind umbrellas, an auto show behind visitors, behind willow foliage,
  through fence bars, beside a bus.
- **Crowding.** Poor-localisation cases are crowded rows (a marina car
  park, a car-show row, stacked traffic). The confident box covers a
  neighbour or a fragment.
- **Possible annotation ambiguity, about 6 of 26** (Wilson 95 % about
  11-42 %):
  - 3 cars seen as reflections or through glass: a side mirror, a
    windscreen, a car window
  - an airport tug labelled car
  - a bus-like vehicle labelled car
  - a pickup labelled car, predicted truck

**Not supported by the measurable proxy.** The "background behind a
foreground subject" impression shows no recall difference against a large
labelled object (§3). Fences, poles and foliage are unlabelled, so that
proxy cannot test occlusion by them.

## 5. R0 vs R1-B on the same 408 medium cars (saved val predictions)

| | R1-B detects | R1-B misses |
|---|---|---|
| **R0 detects** | 182 | 30 |
| **R0 misses** | 28 | 168 |

- **R1-B recovered 28.** R0's categories for them: 17 below threshold, 5
  poor localisation, 5 no useful candidate, 1 wrong class.
- **R1-B lost 30.** For 27 of them R1-B still has a car detection at IoU ≥ 0.5,
  now below 0.35; 3 have none.
- **Both miss 168.** R0's categories for them: 82 no useful candidate, 45
  below threshold, 38 poor localisation, 3 other.

**Reading.**
- R1-B moved cars across the threshold in both directions, mostly from the
  near-threshold population.
- It recovered only 5 of R0's 87 deep misses (objectness about 0.03).
- One run per variant: this does not separate R1-B's effect from
  run-to-run variation.

## 6. Measured, inferred, unknown

**Measured:**
- the category counts and score factors above
- parity with the saved detections
- the size dependence
- the absent associations in §3
- the R0/R1-B 2×2 table

**Inferred:**
- Low objectness on correctly placed car boxes is the dominant proximate
  failure on val medium cars.
- Ignoring background objectness in unverified Open Images pixels (R1-A,
  R1-B) did not address it.

**Unknown:**
- *Why* objectness is low. Candidates, none tested here:
  - unlabelled cars in training images (rate not measured)
  - YOLOX's label assignment for small and medium objects
  - occlusion by unlabelled objects
  - a train/val difference
- Whether R0 shows the same suppression on **training** medium cars. That
  separates a training-signal problem from a generalisation problem.
- The true share of annotation-ambiguous misses: an estimate from 26 cases
  only.

## 7. Recommended next action (one, no training)

**Measure R0 on training data with the same diagnostic.**
- Run `diag_missed_medium.py`'s rules (same checkpoint, metric, thresholds,
  categories) on a seeded sample of training images containing medium cars,
  for example 1,000 images, split by source.
- **If** training medium cars show the same low-objectness misses, the
  model never learns to rate such cars as objects. Look at the training
  signal next (labels or assignment).
- **If** training medium cars are found and val ones are not, it is a
  generalisation gap. Training-signal changes such as R1-A/B are then the
  wrong lever.

Cost: inference only, minutes on the B580, no new data, no use of val or
test. Proposing another training experiment before this measurement is not
justified by the evidence.
