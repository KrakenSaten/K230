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

A *geometrically matching row* is not the same as a *meaningful detection*.
- YOLOX trains the class loss only on positive (assigned) anchors
  (`yolo_head.py:391-395`). Every grid cell still regresses some box.
- So a row whose objectness is near zero can overlap a car by chance of
  geometry. Its "car confidence" is then unconstrained by training.
- Car confidence is informative only where objectness is non-trivial.

- **Below threshold** (car score 0.10-0.35): a weak but meaningful candidate.
  The row is placed on the car, objectness is the smaller factor.
  - median IoU 0.72, all 62 with argmax car
  - car confidence p25/50/75 = 0.62 / 0.68 / 0.71
  - objectness 0.21 / 0.34 / 0.41
- **No meaningful candidate, geometric match only (73):** a row overlaps
  the car at IoU ≥ 0.5, but objectness is near zero. This is **not** evidence
  that the model recognised the car. The car-confidence values below are
  reported, not interpreted.
  - median IoU 0.66, 61 with argmax car
  - car confidence 0.37 / 0.53 / 0.63
  - objectness 0.01 / 0.03 / 0.08
- **No row at IoU 0.5 (14):** small cars, side416 median 19.9; best row
  IoU 0.36-0.46.
- **Poor localisation (43):** confident car rows (score median 0.57) at
  IoU 0.21-0.43.
  - 27 of 43 overlap another labelled car (IoU ≥ 0.1)
  - 35 of 43 are COCO

**Dominant pattern (measured; corrected 2026-10-16).**
- **No meaningful candidate (87, 44 %):** the model effectively outputs
  nothing for the car. Objectness is near zero, or no row matches.
- **Weak candidate (62, 32 %):** objectness is the smaller factor.
- **Localisation (43, 22 %).**

The earlier wording ("a well-placed box exists and its class is car") read
recognition into the 73 near-zero rows; that reading is withdrawn. The
2026-10-04 street frames are context only.

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

**No association detected by this analysis** (size-stratified where noted).
This does not show that these factors have no effect:
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
- **Possible annotation ambiguity in 6 of the 26 reviewed cases.** The 26
  were sampled by category, not at random from all misses. The count is
  therefore not an estimate of the ambiguity rate among all 196 misses, and
  carries no confidence interval. The cases:
  - 3 cars seen as reflections or through glass: a side mirror, a
    windscreen, a car window
  - an airport tug labelled car
  - a bus-like vehicle labelled car
  - a pickup labelled car, predicted truck

**No association detected by the measurable proxy.** The "background behind
a foreground subject" impression shows no recall difference against a large
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
- the subgroup comparisons in §3 (no association detected)
- the R0/R1-B 2×2 table

**Inferred:**
- Most val medium-car misses are absent or weak objectness responses (149 of
  196), not class or NMS errors.
- Ignoring background objectness in unverified Open Images pixels (R1-A,
  R1-B) did not address it.

**Unknown:**
- *Why* objectness is low. Candidates, none tested here:
  - unlabelled cars in training images (rate not measured)
  - YOLOX's label assignment for small and medium objects
  - occlusion by unlabelled objects
  - a train/val difference
- Measured since on training images (R0_MEDIUM_MISS_TRAIN_DIAG.md).
- The true share of annotation-ambiguous misses: an estimate from 26 cases
  only.

## 7. Next action

Done on 2026-10-16: the same measurement on training images. See
[R0_MEDIUM_MISS_TRAIN_DIAG.md](R0_MEDIUM_MISS_TRAIN_DIAG.md).
- The pattern also occurs there: about 36 % of training medium cars are
  missed.
- Val is worse in some strata.
- The follow-up recommendation is in that report.
