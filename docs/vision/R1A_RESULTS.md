# R1-A results (trained 2026-10-09 to 2026-10-12): **FAIL**

Plan and fixed criteria: [R1A_PLAN.md](R1A_PLAN.md), commit `c710dee`. Nothing was
changed during the run.
- R0, `traffic6_r0` and the 2026-10-04 set are unchanged.
- Nothing was exported to a device or merged.

Records:
- [`data/traffic6_r0/r1a_criteria_val.json`](data/traffic6_r0/r1a_criteria_val.json)
- run, detections and diagnostics in `C:\K230-work\yolox-train` (`runs/traffic6_r1a`,
  `eval/traffic6_r1a_best`, `r1a_eval`)

## 1. Run (VERIFIED)

- **Command.** R0's command plus `--neg-obj-ignore oi-car-unverified`. That is
  the only configuration difference in `run.json`.
- **Start.** Fresh initialisation, seed 20261004, `pretrained_weights: null`,
  no resume.
- **Duration.** One 61.3 h session, 300/300 epochs, no warnings.
- **Schedule.** Mosaic epochs 1-284, no mosaic and L1 from epoch 285, as R0.
- **Mask.** 9,997 flagged images (ids sha256 `9bb31b61…`). Ignored share of
  negative objectness terms: median 13 % (mosaic phase) and 16.5 % (no-mosaic
  phase).
- **Selection.** `best_ckpt.pth`: epoch 287, plain-COCO val mAP50:95 0.4470
  (R0 0.4778). Selected by the same rule as R0.
  - Sha256 `16153983a1ec3859864420b4c5cc6c0a70366384cd2d21bcfd25fae8782d21d6`.
  - The evaluator reproduces 0.44698718061079895 exactly.
- **Validation history.** Plain val mAP50:95 was below R0 at every
  validation, by 0.011 at epoch 10 and 0.031 at the end.

## 2. Fixed criteria (val, custom mixed metric, conf 0.35, IoU 0.5)

| | R0 | R1-A | Required | Result |
|---|---|---|---|---|
| **P** medium-car recall | 212/408 = 0.520 | 216/408 = 0.529 | ≥ 233/408 and McNemar p < 0.05 | **not met**: +0.010; 30 boxes gained, 26 lost; p = 0.69 |
| **G1** COCO vehicle background FP | 28 | 18 | ≤ 35 | met |
| **G2** all FP | 679 | 2,281 | ≤ 747 | **broken** |
| **G3** class AP50:95 drop ≤ 0.010 | | bicycle −0.094, person −0.041, motorcycle −0.018, truck −0.014, car −0.003, bus +0.007 | | **broken** |
| **G4** mAP50:95 | 0.4824 | 0.4551 | ≥ 0.4774 | **broken** |

**Verdict: FAIL.** P's gain is below +0.02, and three guards are broken.

**Reported, not gating.**
- Small-car recall: 0.205 → 0.153 (of 176).
- Open Images vehicle background FP: 13 → 14.
- Val precision 0.851 → 0.629. Recall 0.586 → 0.587.

**Reused held-out test (confirmatory, not independent).**
- mAP50:95 0.502 → 0.473.
- Precision 0.872 → 0.526. All FP 1,354 → 8,430.
- Medium-car recall 518 → 519 of 978. Small 51 → 49 of 299.

## 3. Why it failed (evidence)

**Where the new false positives are.**
- They are bicycle and person boxes, mostly localisation/duplicate, on Open
  Images images:
  - bicycle: 6 → 648
  - person: 78 → 480
- COCO images barely changed: bicycle 4 → 26, person 235 → 247.
- The flagged images hold 88 % of the Open Images bicycle boxes.

**Mechanism (code-level, and consistent with this pattern).**
- Every anchor near a labelled bicycle or person that SimOTA does not assign
  to it is a negative objectness term.
- Those terms teach "one box per object".
- R1-A ignored all negatives in flagged pixels, including those around
  labelled objects. The duplicate suppression went with them.

## 4. The original hypothesis: diagnostic only

On the 12 cars R0 missed in seq100, seq140 and seq160:
- R1-A's objectness rose from 0.02-0.18 to 0.14-0.82.
- Objectness × car score on the best box (float, before NMS) clears 0.35 for
  6 of 12. R0 cleared it for none.

These frames were used to form the hypothesis, so this supports the
mechanism, unflagged negatives suppressing such cars, only on reused data. It
did not carry over to val medium cars (+4 of 408). The full 2026-10-04 harness
comparison was not run: the verdict is decided on val, and R1-A will not be
deployed.

## 5. Limitations

- **One run per arm.** XPU training is not bit-reproducible. YOLOX's loader
  workers also reseed from `uuid4`
  (`yolox/data/dataloading.py:110`), so augmentation draws differ between
  runs even with the same seed. Run-to-run spread is unknown.
  - It cannot explain a 3.4× rise in false positives, or the consistent
    mAP gap at every validation.
  - It could matter for small differences, such as P's +0.010.
- **What the mask removed.** It removed objectness negatives near labelled
  objects as well as the intended "possibly unlabelled car" negatives. The
  experiment tested that combined change, not the narrower idea.
- **Reused data.** The test split and the 2026-10-04 frames are reused. Only
  val decided the verdict.
