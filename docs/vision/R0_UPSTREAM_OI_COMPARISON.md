# R0 vs upstream YOLOX-Tiny 416 on the Open Images training sample (2026-10-16, exploratory, inference only)

Follows [R0_MEDIUM_MISS_TRAIN_DIAG.md](R0_MEDIUM_MISS_TRAIN_DIAG.md).
Script: `tools/vision/training/eval/compare_upstream_oi.py`.

Unchanged: no training, no change to checkpoints, thresholds or earlier
results. Val was not rerun; the test split was not read.

Records:
- [`data/traffic6_r0/r0_upstream_oi_comparison.json`](data/traffic6_r0/r0_upstream_oi_comparison.json)
- upstream detections and the discordant-case sheet in
  `C:\K230-work\yolox-train\diag\upstream_vs_r0_oi` (not in git)

## 1. Identities and preprocessing

| | R0 | Upstream |
|---|---|---|
| Checkpoint | `best_ckpt.pth`, sha256 `a501b84c…` (saved predictions reused) | `yolox_tiny.pth`, release `0.1.1rc0`, sha256 `9de513de…aee8f0`, equal to the 2026-10-04 record; epoch 297 |
| Architecture | YOLOX-Tiny, depth 0.33, width 0.375, SiLU, 6 classes | the same, 80 COCO classes; strict load into YOLOX `6ddff48` `exps/default/yolox_tiny.py` |
| Preprocessing | `ValTransform(legacy=False)`: BGR 0-255, 416 letterbox top-left, pad 114 | the same |
| Class decision | argmax over 6 classes | argmax over 80 classes |
| NMS and floor | class-aware NMS 0.65, score floor 0.01 | the same |

**Why upstream uses the non-legacy preprocessing.** YOLOX's README lists
release `0.1.1rc0` `yolox_tiny.pth` in the current (non-legacy) model table.
The legacy Tiny weights are a different file (`yolox_tiny_32dot8.pth`).
`docs/updates_note.md` says only old weights need `--legacy`. The 2026-10-04
evaluation fed this checkpoint the same way: swapRB, mean 0, std 1.

**Class mapping.** COCO class indices map by name to traffic6 ids: car 2 → 1,
truck 7 → 2, bus 5 → 3, motorcycle 3 → 4, bicycle 1 → 5, person 0 → 6.
- The other 74 COCO classes are not traffic6 detections. Upstream produced
  296 of them at ≥ 0.35 on these images, mostly traffic lights, handbags,
  backpacks and umbrellas.
- Changing one index does not make the models equivalent:
  - upstream's car competes with 79 classes;
  - its car/truck boundary follows COCO, while Open Images "Car" includes Van
    and Taxi;
  - of the medium Van-labelled cars, upstream calls 9 of 14 truck or bus;
    of the Car-labelled ones, 34 of 1,109.

**Production path.** This checkpoint's kmodel (`8c304651…`) ran through the
unchanged DOORS decoder in the unit A A/B gate (DOCUMENTED,
`docs/hardware/VISION_MODEL_AB_GATE.md` on master). Here both models are
evaluated as float PyTorch.

## 2. Sample and scoring

**Sample.**
- The Open Images half of the training-diagnostic sample: re-derived with
  seed 20261017, id list sha256 `4233ccb2…` (checked).
- 500 images, all with Car human-verified, holding 1,151 medium cars.

**Scoring** (as before):
- custom mixed metric, group-of boxes as crowd regions
- medium = side416 in [16, 64)
- hit = car score ≥ 0.35 at IoU ≥ 0.5
- pycocotools greedy matching (area all, maxDets 100)

**Not an independent benchmark.** R0 was trained on these exact images,
with their Open Images labelling conventions. Upstream never saw them, and
was trained on COCO with COCO conventions.

## 3. Results

| Medium cars (1,151) | R0 | Upstream |
|---|---|---|
| Recall at 0.35 | **0.618** (711) | 0.573 (659) |
| 16-24 px (254) | 114 | 120 |
| 24-32 px (225) | 130 | 129 |
| 32-48 px (393) | **265** | 234 |
| 48-64 px (279) | **202** | 176 |
| AP50 (medium) | 0.763 | 0.630 |

R0's 711 equals the Open Images strata of the training diagnostic exactly.

| All car sizes (1,890 cars) | R0 | Upstream |
|---|---|---|
| False positives at 0.35 | **162** | 398 |
| Precision at 0.35 | **0.88** | 0.73 |
| AP50 | 0.748 | 0.616 |
| Medium recall at ≤ 162 false positives | 0.618 (threshold 0.35) | 0.478 (threshold 0.54) |

The matched-FP row is exploratory; no threshold is selected.

**Paired medium cars.**
- Both 589; R0 only 122; upstream only 70; neither 370.
- **By image:** R0 finds more medium cars in 92 images, upstream in 51, equal
  in 357. An exploratory sign test gives p = 0.0008. Images, not cars, are
  the unit; cars in one image are not independent.

**Miss categories** (same decision tree; for upstream "wrong class" includes
any non-car COCO class):

| | R0 (440 misses) | Upstream (492 misses) |
|---|---|---|
| no meaningful candidate | 151 | 208 |
| weak candidate (low objectness / low class) | 149 / 0 | 88 / 7 |
| poor localisation | 112 | 143 |
| wrong class | 3 | 21 |
| NMS removal | 8 | 14 |
| matching conflict | 17 | 11 |

The 70 upstream-only cars were, for R0: 41 weak (low objectness), 16 poor
localisation, 9 no meaningful candidate, 4 other.

**Discordant sample** (8 upstream-only, 4 R0-only, seeded; visual,
uncertain). Mostly real, partly hidden cars on both sides:
- in parked rows, behind pedestrians, in a crowded market
- partly visible taxi roofs
- one blurred car seen through glass (ambiguous)

Each model recovers different hard cases.

**Limits of the precision comparison.**
- Open Images boxes every car only where Car is verified present. Unboxed
  border or background cars still occur (observed earlier).
- Some of upstream's extra false positives may be real cars, or
  convention mismatches.
- R0 learned these images' conventions; upstream did not.

## 4. Reading

**Supported.**
- On these sampled Open Images cars, upstream does **not** detect more than
  R0. It is level at 16-32 px and lower above 32 px, with more false
  positives.
- This comparison gives **no evidence** that the same architecture at 416
  detects substantially more of these cars under another training setup.

**Not supported.**
- **Capacity as the cause of R0's misses:** similar or lower upstream
  performance does not show that. Two confounds separate the models:
  - R0 trained on these images (in-sample advantage);
  - upstream faces a COCO → Open Images domain and convention shift.
- **Upstream being worse in general:** this is one domain. On the
  2026-10-04 street frames upstream found 82 of 91 vehicles and R0 47 (a
  different set, consensus ground truth partly built from upstream's
  outputs). That is context only.

**Remaining unknowns.**
- Whether capacity or resolution limits R0.
- Why objectness is low for missed medium cars on both splits.
- How R0 and upstream compare on scenes neither trained on.

**Correction to R0_MEDIUM_MISS_TRAIN_DIAG.md** ("training images were never
seen clean at 416"):
- In the no-mosaic phase (epochs 285-300), `TrainTransform` showed each
  image un-mosaicked: letterboxed, flipped with probability 0.5,
  colour-jittered (`augment_hsv` leaves all three gains at zero with
  probability 1/8), and resized by multiscale 320-640.
- So images were seen un-mosaicked, but rarely as an exact clean 416 frame.
  That report's wording is corrected.

## 5. Recommendation: stop this diagnostic series here

Further inference comparisons on the data available cannot separate capacity
from data or recipe:
- every labelled set at hand is either in one model's training data (COCO
  for upstream, `traffic6_r0` train for R0),
- or already explored (val),
- or labelled by model consensus (the 2026-10-04 frames).

**Missing measurement.** An independent, exhaustively labelled street-scene
evaluation set that neither model trained on, with size-stratified cars,
ideally DOORS-camera frames. DATASET_TRAFFIC6_R0 section 12 already lists
this as a gap. Building it is a data-collection decision for the owner, not
another model experiment.
