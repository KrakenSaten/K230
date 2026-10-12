# R1-B plan: keep background objectness inside labelled boxes (prepared 2026-10-12, not run)

Status: **prepared, not trained.**
- R0 (shipped as "R0 · Beta" in Doors 0.3.6) and R1-A (FAIL,
  [R1A_RESULTS.md](R1A_RESULTS.md)) are the references. Their artifacts are
  unchanged.
- `traffic6_r0` is unchanged.

## 1. The change against R1-A

R1-A ignored every negative objectness term in pixels from the 9,997 Open
Images training images without a verified Car. It failed, with bicycle and
person duplicates around labelled objects (85 % of the extra false positives
had their centre inside a labelled box).

R1-B ignores those terms **only outside every labelled training box**:
`train_loop.py --neg-obj-ignore oi-car-unverified-outside-boxes`
(`neg_obj_mask.py`).

| | Treatment |
|---|---|
| Anchor centre inside any labelled (non-crowd) training box of its sample | negative kept, as in R0 |
| In a flagged image's pixels and outside every box | negative ignored (eligible) |
| Unflagged pixels, all positives, box/class/L1 losses, `num_fg` normalisation | as in R0 |

**What counts as "labelled box".**
- These are the targets the loss itself receives, after YOLOX's augmentation
  and the multiscale scaling. No second geometry is involved.
- Up to 120 per sample. Boxes YOLOX drops (side ≤ 1 px) retain nothing,
  because YOLOX trains them as background too.

**Group-of / crowd boxes (choice and reason).**
- YOLOX's `COCODataset` loads `iscrowd=False` only (`coco.py:96`). Group-of
  boxes never reach the loss: in R0 and R1-A training they are plain image
  area, and the evaluation treats them as ignore regions.
- R1-B follows that. A crowd box retains nothing by itself, so it does not
  turn into a background negative merely because it exists.
- In flagged pixels its area is eligible like any non-box pixel. In
  unflagged images it stays exactly as in R0.
- VERIFIED on 300 flagged training images with crowd boxes: YOLOX's loaded
  labels exclude every crowd box (0 mismatches). Flagged images hold 2,493
  crowd boxes in 1,724 images.

**Known limit.** Unlabelled cars that overlap a labelled box (for example a
car behind a labelled cyclist) are still trained as background. Their share
is unknown, because they are unlabelled.

## 2. Offline counts (training data only, no model)

`traffic6_r0` training samples through the real pipeline: 1,200 mosaic and
600 no-mosaic samples, multiscale 320-640, seed 20261012.

| Phase | R1-A eligible anchors | R1-B eligible anchors | Restored (inside boxes) | R1-B eligible inside YOLOX's ±1.5-stride candidate region |
|---|---|---|---|---|
| mosaic | 12.9 % | **8.1 %** | 37 % of R1-A's | 1.3 % of R1-B's |
| no mosaic | 17.9 % | **11.3 %** | 37 % of R1-A's | 0.8 % of R1-B's |

- **The census matches the real run.** It reproduces R1-A's logged share
  (13.0 % mosaic, 16.5 % no mosaic).
- **R1-B is not a no-op.** It still ignores 63 % of R1-A's terms.
- **Box-only retention costs little.** Hardly any eligible anchor lies in
  SimOTA's candidate region outside its box.

Counts are of anchors, because offline there is no assignment. The few
positives among them are never ignored in training.

## 3. Correctness checks (2026-10-12)

| Check | Result |
|---|---|
| Inside a box never ignored, outside follows the pixel mask, at 320 / 416 / 640 | pass |
| R1-B ignores a subset of R1-A; a box in unflagged pixels changes nothing; padding retains nothing | pass |
| Retention with real augmented mosaic labels vs an independent rasterisation of those labels, 3 sizes | identical |
| Loss with an R1-B mask: objectness drops exactly the R1-B terms; box, class, L1 bit-identical | pass |
| R1-A tests (mask through mosaic, affine, HSV, flip, letterbox, no-mosaic, multiscale; off = YOLOX) | unchanged, pass |
| Test suite | 62 tests pass |
| XPU smoke (3 compressed epochs × 30 iterations, mosaic then no-mosaic + L1) | finite, no warnings; ignored share 8.8 % mosaic, 9.9-13 % no mosaic; rule recorded in `run.json` |

## 4. Pilot: 20 epochs of the real 300-epoch schedule

The command is the full R0 recipe with the R1-B flag, with no schedule
overrides. It stops normally after epoch 20.

```powershell
cd C:\K230-wt\yolox-training\tools\vision\training; $env:DOORS_TRAIN_DATA='C:\K230-work\yolox-train\data\traffic6_r0'; $env:DOORS_CLASS_SET='traffic6'; $env:PYTHONPATH='C:\K230-work\yolox-train\src\YOLOX'; C:\K230-work\yolox-train\venv-xpu\Scripts\python.exe train_loop.py --device xpu --exp exps\yolox_tiny_traffic_416.py --out C:\K230-work\yolox-train\runs\traffic6_r1b --batch 16 --workers 4 --amp fp16 --neg-obj-ignore oi-car-unverified-outside-boxes --stop-after-epoch 20
```

**What the pilot does.**
- **Schedule.** The 300-epoch warm-cosine LR (warm-up epochs 1-5), mosaic and
  multiscale. The no-mosaic/L1 phase starts at epoch 285, so the pilot
  never enters it.
- **Validation.** Plain val at epochs 10 and 20, as R0 and R1-A logged it.
  The held-out test is never read: `train_loop.py` cannot reach it.
- **Runtime.** About 4.3 h (R0 epochs 1-20: 4.14 h + 82 s validation; R1-A:
  4.46 h).
- **Resume, if approved.** The same command with `--resume latest` in place
  of `--stop-after-epoch 20`. A resume with any other setting is refused.

## 5. Early-rejection thresholds (fixed before the pilot)

The thresholds compare plain val from `epochs.jsonl` with R0 at the same
epoch. The pilot is **REJECTED** if any of these holds:

| # | Condition | R0 e10 → limit | R0 e20 → limit | R1-A, for scale |
|---|---|---|---|---|
| 1 | a non-finite loss or a crash (the loop stops itself) | | | none |
| 2 | mask share outside 0.05-0.11 in any mosaic epoch (`neg_obj_ignored_frac`; census 0.081, smoke 0.088) | | | 0.129-0.131 |
| 3 | val false positives > 1.15 × R0 | 623 → > 716 | 710 → > 816 | 1,096 / 1,280 |
| 4 | bicycle AP50:95 < R0 − 0.02 | 0.289 → < 0.269 | 0.359 → < 0.339 | 0.255 / 0.327 |
| 5 | person AP50:95 < R0 − 0.02 | 0.215 → < 0.195 | 0.282 → < 0.262 | 0.209 / 0.264 |

R1-A would have been rejected on 2, 3 and 4.

**Reading the result.**
- **Passing** only justifies **considering** a continuation to 300 epochs.
  That is the owner's decision. Passing is not evidence of improvement.
- **Car metrics.** The pilot judges no car metric. Medium-car recall can only
  be judged at the end, on the unchanged criteria of R1A_PLAN.md section 3
  (P, G1-G4) with R1-B in R1-A's place.
- **Uncertainty.** There is one R0 run. YOLOX's loader workers reseed from
  `uuid4`, so early-epoch spread between runs is unknown. The thresholds are
  sized to catch an R1-A-sized failure, not small differences. A result
  near a threshold goes to the owner as it is.
- **Reused data.** The held-out test and the 2026-10-04 frames are not used
  in the pilot.
