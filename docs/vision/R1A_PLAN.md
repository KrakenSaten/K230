# R1-A plan: no background objectness where Car is unknown (prepared 2026-10-09, not run)

Status: **prepared, not trained.**
- R0, `traffic6_r0` and the 2026-10-04 set are unchanged.
- Production Vision is unchanged.

Why: R0 misses medium cars in street scenes. The cause found was objectness
suppression of correctly classified cars, and it already happens in
PyTorch. The working hypothesis is that unboxed cars in Open Images images
where Car was never verified are trained as background (r0miss findings,
not demonstrated).

## 1. The one change

`train_loop.py --neg-obj-ignore oi-car-unverified` (`neg_obj_mask.py`).

**Flagged images.**
- Open Images training images whose human-verified labels do not hold Car,
  present or absent.
- 9,997 images, id list sha256 `9bb31b61…`, recorded in `run.json`.

**Ignored.**
- The negative objectness terms (target 0) of anchors whose centre lies in
  pixels that came from a flagged image.
- The mask follows mosaic, affine, flip, letterbox, the no-mosaic phase and
  multiscale.

**Kept.**
- Every positive term: box, objectness, class, L1, also inside flagged
  pixels.
- Everything in non-flagged pixels.
- `num_fg` normalisation.
- All bicycle and person annotations.

**Unchanged.**
- `traffic6_r0` byte for byte, classes and mapping, seed 20261004.
- The model and all hyperparameters: 300 epochs, batch 16, fp16,
  multiscale 320-640.
- The schedule, EMA, augmentation and upstream's no-mosaic rule.
- The checkpoint rule: best by plain-COCO val mAP50:95, as R0.
- Export (nncase 2.11.0, a16/w8).

The option defaults to off, and off is R0's training.

## 2. Verification done (2026-10-09)

| Check | Result |
|---|---|
| Option off vs committed `train_loop.py` (smoke set, CPU, 12 iterations incl. the no-mosaic/L1 epoch) | every logged loss, size, mosaic count and LR identical |
| No ignore mask / all-false mask vs YOLOX head (fixed inputs, with and without L1) | losses bit-identical |
| With a mask | objectness = YOLOX's minus exactly the ignored negative terms / num_fg; box, class, L1 bit-identical; ignored positives keep their objectness |
| Anchor order and centres | equal to the head's own grids (x, y, stride) |
| Masked dataset vs YOLOX `MosaicDetection`, same seeds | images and labels byte-identical (synthetic set: 40 mosaic + 40 no-mosaic; real `traffic6_r0`: 200 + 200) |
| Mask vs pixel source (synthetic, flagged red / other blue, through mosaic, affine, HSV, flip, letterbox, no-mosaic, close_mosaic flag) | 0 mismatches away from colour edges; deliberately broken flip or affine handling fails the test |
| Multiscale | mask sampled correctly at 320, 416, 640 |
| Mixup | refused (disabled in the exp) |
| Test suite | 58 tests pass (49 existing + 9 in `tests/test_neg_obj_mask.py`) |

**XPU smoke run.** 3 compressed epochs × 60 iterations, in
`runs/r1a_smoke`:
- All losses finite, no warnings.
- Epoch 1: 100 % mosaic. Epochs 2-3: 0 % mosaic, L1 on.
- Ignored share of negative objectness terms: 13 % with mosaic, 19 %
  without. Never 0 in any iteration.
- Peak VRAM 2.7 GB. GPU throughput as R0 (about 105 images/s steady).
- Loader cost unchanged within timing noise.

**Expected runtime:** about 61 h, as R0 (60.9 h). Measured 2026-10-06.

## 3. Fixed acceptance criteria (set before training)

**Decision set.**
- The `traffic6_r0` **val** split, scored with `eval/eval_mixed.py` (custom
  mixed metric, conf 0.35, IoU 0.5).
- `best_ckpt.pth` of R1-A against `best_ckpt.pth` of R0 (epoch 299), both
  selected by the same rule.

| | R0 val baseline | R1-A must reach |
|---|---|---|
| **P (primary)** car recall, medium (side416 16-64) | 212/408 = 0.520 [0.471, 0.568] | **≥ 0.570 (≥ 233/408)** and exact paired McNemar test on the 408 boxes p < 0.05 |
| **G1** vehicle background false positives (car/truck/bus/motorcycle, IoU < 0.1 with any box) on COCO val images (exhaustive labels) | 28 | **≤ 35** |
| **G2** all false positives, all classes, scored pairs | 679 | **≤ 747** (+10 %) |
| **G3** each class AP50:95 | car 0.520, truck 0.369, bus 0.570, motorcycle 0.445, bicycle 0.565, person 0.426 | **none lower by more than 0.010** |
| **G4** mAP50:95 | 0.4824 | **≥ 0.4774** |

**Reported, not gating:**
- small-car recall on val (R0 36/176 = 0.205)
- vehicle background false positives on Open Images val images (R0 13)

**Two separate kinds of uncertainty.**
- **Object sampling** (finite val boxes), quantified:
  - Wilson intervals per bucket.
  - The paired McNemar test for P: the same 408 boxes, each detected or
    not by each model.
  - Poisson reading for false positive counts: 28 has a 95 % range of about
    19-40. G1 is therefore a cap against clear growth. Passing it does not
    prove there is no growth.
- **Training-run variability**, unknown:
  - There is one R0 run, and XPU training is not bit-reproducible even with
    the same seed.
  - Nothing here estimates it. The criteria do not pretend to.

**Verdict:**
- **PASS:** P and G1-G4 all met.
- **FAIL:** P's gain below +0.02, or any guard broken.
- **INCONCLUSIVE:** P's gain between +0.02 and +0.05, or McNemar p ≥ 0.05,
  with the guards held. Before acting on it, repeat R0 once (same recipe,
  about 61 h) to measure run-to-run spread.

**Reused data. Neither of these decides the verdict.**
- The R0 held-out test split was read once for the R0 report and its errors
  were inspected. It is reported for R1-A as reused, confirmatory data, not
  as independent proof.
- The 2026-10-04 frames, including seq100, seq140 and seq160, were inspected
  to form the hypothesis. They are a diagnostic of the mechanism: objectness
  on the 12 known misses and the full harness comparison.
