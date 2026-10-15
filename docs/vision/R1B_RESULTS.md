# R1-B results (2026-10-12 to 2026-10-15): **FAIL**

Plan and fixed criteria:
- [R1B_PLAN.md](R1B_PLAN.md) (change, pilot)
- [R1A_PLAN.md](R1A_PLAN.md) section 3 (final criteria, unchanged)
- pilot: [R1B_PILOT.md](R1B_PILOT.md), INCONCLUSIVE; continued by owner decision

Untouched: R0 (shipped as "R0 · Beta"), R1-A and `traffic6_r0`. Nothing was
exported, deployed or merged.

Records:
- [`data/traffic6_r0/r1b_criteria_val.json`](data/traffic6_r0/r1b_criteria_val.json)
- run, evaluations and memory history in `C:\K230-work\yolox-train` (`runs/traffic6_r1b`,
  `eval/traffic6_r1b_best`, `runs/r1b_memory.csv`, `runs/r1b_interruptions.md`,
  `runs/r1b_supervisor.log`)

## 1. Run (model-independent record)

- **Code and configuration.** Commit `daf1d61` (clean), `--neg-obj-ignore
  oi-car-unverified-outside-boxes`. Otherwise R0's recipe: 300 epochs,
  batch 16, fp16, multiscale.
- **Epochs.** All 300 completed exactly once, losses finite.
- **Schedule.** Mosaic epochs 1-284, no mosaic and L1 from epoch 285.
- **Mask share.** 0.080-0.083 in mosaic epochs, 0.109-0.111 without mosaic,
  as the offline census predicted.
- **Training time.** 227,023 s (63.1 h) over completed epochs, plus 1,850 s of
  validation.
- **Best checkpoint.** Epoch 296, plain val mAP50:95 0.46900 (R0 0.47781,
  R1-A 0.44699). Same selection rule as R0.
  - The evaluator reproduces the value exactly.
  - `best_ckpt.pth` sha256 `a3675f01a431ca7a6b0c611e70999aad323feb1300d1bbac8b3859d8fcfc1c03`.
  - `final_ckpt.pth` sha256 `3c163ebc2bc905c9c97378c844a54d191bf170b9f396ca0eda949ce9b1e45991`.

**Interruptions** (details in `runs/r1b_interruptions.md`). Each was resumed
from a verified checkpoint, with configuration unchanged.

| # | When | Epoch | Event | Cause | Handling |
|---|---|---|---|---|---|
| 1 | 2026-10-12 08:24 | 6 | `UR_RESULT_ERROR_DEVICE_LOST` in YOLOX SimOTA | unknown; not assumed unrelated to R1-B | one resume (pilot) |
| 2 | 2026-10-12 14:47 | 24 | process terminated, empty stderr | Windows user sign-out (event log) | supervisor stopped; resumed by owner decision |
| 3 | 2026-10-14 08:18 | 132 | `UR_RESULT_ERROR_OUT_OF_RESOURCES` raised in `neg_obj_mask.inside_boxes` | host virtual-memory (commit) exhaustion; other applications held about 34 GB of a 46.8 GB limit | supervisor stopped; owner freed memory and rebooted with a 34 GB page file; one authorised restart |

**Memory in session 5** (monitor, 2,097 one-minute samples).
- Main process: 10.45 → 11.52 GB over 35 h.
- Loader workers grew about 0.1 GB/h while they persisted, 5.4 → 7.9 GB by
  epoch 262. They reset to 5.2 GB when the loader was rebuilt at the epoch-285
  mosaic switch.
- Training tree peak: 25.0 GB, during validation with 8 workers.
- Minimum commit free: 6.55 GB, on 2026-10-15 at 12:44, driven by other
  applications (about +10 GB that morning).

There was no recurrence. This slow worker growth (YOLOX loader workers,
present in R0 and R1-A as well, which were not monitored) may have
contributed to #3, but is not established as its cause.

## 2. Fixed criteria (val, custom mixed metric, conf 0.35, IoU 0.5; R1-B best vs R0 best)

| | R0 | R1-B | Required | Result |
|---|---|---|---|---|
| **P** medium-car recall | 212/408 = 0.520 | 210/408 = 0.515 | ≥ 233/408 and McNemar p < 0.05 | **not met**: −0.005; 28 boxes gained, 30 lost; p = 0.90 |
| **G1** COCO vehicle background FP | 28 | 24 | ≤ 35 | met |
| **G2** all FP | 679 | 711 | ≤ 747 | met |
| **G3** class AP50:95 drop ≤ 0.010 | | bicycle −0.030, truck −0.027; car +0.001, bus +0.001, motorcycle +0.003, person −0.002 | | **broken** |
| **G4** mAP50:95 | 0.4824 | 0.4733 | ≥ 0.4774 | **broken** |

**Verdict: FAIL.** P's gain is below +0.02, and G3 and G4 are broken.

**Reported, not gating:**
- Small-car recall: 0.205 → 0.188 (of 176).
- Open Images vehicle background FP: 13 → 19.
- Precision 0.851 → 0.844. Recall 0.586 → 0.584.

**Reused held-out test (confirmatory, not independent).**
- mAP50:95 0.502 → 0.505.
- Precision 0.872 → 0.866. FP 1,354 → 1,437.
- Medium cars 518 → 535 of 978. Small 51 → 45 of 299. Very small 0 → 1 of 115.
- AP50:95: truck 0.399 → 0.413, bicycle 0.540 → 0.534, the others within
  0.007.

The direction of truck AP differs between val and test. That is consistent
with differences of this size being within the unmeasured run-to-run spread.

## 3. What R1-B showed

**R1-A's failure did not recur.** False positives stayed at R0's level
(711 against 679). R1-A had 2,281, almost all bicycle and person
duplicates.

**The mechanism moved on the reused street frames (diagnostic only).** On
the 12 cars R0 missed in seq100, seq140 and seq160:
- R1-B's objectness was 0.08-0.63 (R0 0.02-0.18, R1-A 0.14-0.82).
- Objectness × car score on the best box (float, before NMS) clears 0.35 for
  5 of 12 (R0 0, R1-A 6).

**It did not improve val medium-car recall** (210 against 212), the decision
metric.

**Conclusion.** Neither R1-A nor R1-B shows that ignoring background
objectness in unverified Open Images pixels improves medium-car recall on
val. The street-frame effect has not generalised to val.

**Limitations.**
- One run per arm. Run-to-run spread is unknown: XPU training is not
  bit-reproducible, and YOLOX workers reseed from `uuid4`.
- Five sessions with three interruptions. Each resume restores the model,
  EMA, optimiser, scaler and RNG, but not the worker augmentation draws.
- The diagnostic frames and the test split are reused data.
