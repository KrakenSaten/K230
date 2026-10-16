# R0's medium cars on training images vs val (2026-10-16, exploratory, inference only)

Follows [R0_MEDIUM_MISS_DIAG.md](R0_MEDIUM_MISS_DIAG.md) (val). Same tool,
`tools/vision/training/eval/diag_missed_medium.py --split train`, with the
same rules.

Unchanged: no training, no change to models, thresholds or verdicts. Val was
not rerun: its numbers come from the saved detections and per-car records.
The test split was not read.

Records:
- [`data/traffic6_r0/r0_medium_miss_train_diag.json`](data/traffic6_r0/r0_medium_miss_train_diag.json)
- per-car records, detections and review sheets in
  `C:\K230-work\yolox-train\diag\r0_medium_train` (not in git)

## 1. Sample and reproducibility

| | |
|---|---|
| Checkpoint | R0 `best_ckpt.pth`, sha256 `a501b84c…` (unchanged) |
| Eligible images | training images where car is scored (eval_mixed rule) with ≥ 1 non-crowd medium car (side416 16-64): COCO 1,223, Open Images 9,070. Annotations only; model output never used for selection. |
| Selection | per source, sorted eligible ids, `random.Random(20261017).sample`, 500 + 500 |
| Sample | 1,000 images (id list sha256 `4233ccb2…`) with 2,325 medium cars: COCO 1,174, Open Images 1,151 |
| Preprocessing | `COCODataset` + `ValTransform(legacy=False)`, 416 letterbox, no training augmentation. Byte-identical to `exp.get_eval_loader`'s tensors on the compared items (checked without running the model on val). |
| Scoring | as val: score ≥ 0.35, IoU ≥ 0.5, pycocotools greedy matching (area all, maxDets 100), medium = side416 in [16, 64), car scored only where verified |

**Categories** (first match, as in the val report):
1. matching conflict
2. NMS removal
3. wrong class
4. poor localisation
5. weak candidate: best car score 0.10-0.35 at IoU ≥ 0.5, split by the
   smaller factor (low objectness or low car confidence)
6. no meaningful candidate: score < 0.10 at IoU ≥ 0.5 (a geometric match
   only), or no row reaches IoU 0.5

## 2. Results

**Overall.**
- Training sample: 1,476 / 2,325 = **0.635**.
- Re-weighted to val's source × size mix: **0.644**.
- Val: 212 / 408 = **0.520**.

**By matched stratum** (recall with 95 % Wilson interval; then the missed
cars' shares: no meaningful / weak / poor localisation / other):

| Stratum | Train recall | Val recall | Train shares | Val shares |
|---|---|---|---|---|
| COCO 16-24 | 0.52 (221/422) [0.48, 0.57] | **0.30** (29/97) [0.22, 0.40] | .39 / .40 / .17 / .04 | .54 / .29 / .13 / .03 |
| COCO 24-32 | 0.66 (166/253) [0.60, 0.71] | 0.52 (29/56) [0.39, 0.64] | .32 / .29 / .34 / .05 | .30 / .33 / .33 / .04 |
| COCO 32-48 | 0.74 (234/317) [0.69, 0.78] | 0.71 (52/73) [0.60, 0.80] | .30 / .33 / .22 / .16 | .48 / .05 / .48 / .00 |
| COCO 48-64 | 0.79 (144/182) [0.73, 0.84] | **0.58** (21/36) [0.42, 0.73] | .39 / .21 / .26 / .13 | .20 / .27 / .47 / .07 |
| OI 16-24 | 0.45 (114/254) [0.39, 0.51] | 0.38 (8/21) [0.21, 0.59] | .39 / .33 / .25 / .04 | .38 / .54 / .08 / .00 |
| OI 24-32 | 0.58 (130/225) [0.51, 0.64] | 0.34 (11/32) [0.20, 0.52] | .35 / .40 / .22 / .03 | .48 / .38 / .14 / .00 |
| OI 32-48 | 0.67 (265/393) [0.63, 0.72] | 0.61 (28/46) [0.46, 0.74] | .35 / .30 / .26 / .09 | .44 / .44 / .11 / .00 |
| OI 48-64 | 0.72 (202/279) [0.67, 0.77] | 0.72 (34/47) [0.58, 0.83] | .25 / .35 / .30 / .10 | .46 / .38 / .15 / .00 |

**Composition.** Val has more COCO 16-24 px cars (24 % against 18 %) and
fewer Open Images 16-24 px (5 % against 11 %). The re-weighted figure
removes this difference.

**All 849 training misses:**

| Category | Count | Share |
|---|---|---|
| no meaningful candidate | 298 | 35 % (245 low objectness, 7 low car confidence, 46 no row at IoU 0.5) |
| weak candidate | 289 | 34 % (284 low objectness, 5 low car confidence) |
| poor localisation | 204 | 24 % |
| matching conflict | 28 | 3 % |
| NMS removal | 18 | 2 % |
| wrong class | 12 | 1 % (8 truck, 2 bus, 2 person) |

## 3. Visual check (25 training misses, stratified by category; judgements uncertain)

- **Weak candidates:** real, visible cars (parked rows, behind foliage,
  under an archway, behind people). The best row sits on the car with
  objectness 0.2-0.5. These are genuine weak responses.
- **No meaningful candidate:** two kinds of case.
  - heavily occluded cars, behind crowds at car shows
  - questionable labels: a "car" box on a window behind a cake, a car seen
    through another car's windscreen

  The near-zero rows are not read as recognised cars.
- **Matching conflicts** include adjacent or overlapping taxi boxes.
- **Wrong class:** a white van and a white pickup labelled car, predicted
  truck.

The sample was stratified by category, so these counts do not estimate rates
among all misses.

## 4. Reading

**Measured.**
- The failure pattern **also occurs on training images.** R0 misses about
  36 % of the medium cars it was trained on, under clean evaluation
  preprocessing.
- The mix of failure types is similar to val: mostly absent or weak
  objectness responses and localisation; class, NMS and matching errors are
  rare.
- Recall rises with size in every source on both splits.
- **Val is worse than train**: 0.520 against 0.644 at val's mix. Matched
  strata with non-overlapping intervals: COCO 16-24 (0.30 against 0.52)
  and COCO 48-64 (0.58 against 0.79). Open Images 24-32 nearly so. The
  other strata are within uncertainty.

**Diagnostic, not proof of cause.** Both a component already present on
training images and an additional train-to-val gap fit these numbers.
Alternative explanations, none isolated by this measurement:
- **Model capacity or resolution:** YOLOX-Tiny at 416, where 16-32 px cars
  are a few stride-8 cells. Size dependence on both splits fits this.
- **Inconsistent or ambiguous labels:** occluded or reflected cars,
  unlabelled cars elsewhere (negatives). The model cannot fit them, which
  lowers objectness on similar cars.
- **Label assignment for small objects** (SimOTA candidate radius and
  dynamic k).
- **Training-time vs evaluation conditions:** most epochs showed these
  images only inside mosaics.
  - The last 16 epochs (285-300) showed them un-mosaicked: letterboxed,
    flipped with probability 0.5, colour-jittered with probability 7/8, at
    multiscale 320-640.
  - So they were rarely seen as an exact clean 416 frame.
  - Corrected 2026-10-16; this first said "never at clean 416".
- **The train-to-val gap:** generalisation, val composition (COCO val
  images include val2017), annotation-style differences, and the small
  val strata.

## 5. Next action (one, inference only)

**Separate capacity from data or recipe with a reference model.**
- Run upstream YOLOX-Tiny 416 (the same architecture and input size,
  COCO-trained) through the same diagnostic. Use its checkpoint `yolox_tiny.pth`,
  sha256 `9de513de…`, already in the 2026-10-04 archive.
- Score only **Open Images** medium cars, in this training sample and on
  val. Upstream never saw Open Images. On COCO images its results would be
  contaminated, because it trained on COCO train2017.

**Reading the result.**
- If upstream finds substantially more Open Images medium cars than R0's
  0.45-0.72, the architecture and input size are not the binding limit.
  The training data or recipe is.
- If upstream is similar or worse, capacity and resolution remain a live
  explanation, and data-only changes are unlikely to help much.
- A domain difference (COCO-trained on Open Images) makes "similar or
  worse" weaker evidence than "better".

**Cost.** Minutes of inference. The only new code is a class-index mapping
(COCO car = 2). No training. The val reuse is exploratory.
