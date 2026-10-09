# R0 baseline: YOLOX-Tiny 416 on `traffic6_r0` (trained 2026-10-06 to 2026-10-09)

Status: **research, measurement baseline.**
- Production Vision, `yolov8n.kmodel` and the nncase configuration are
  unchanged.
- Nothing was deployed to unit A or unit B.
- Nothing is merged.

R0 trains the frozen model and dataset configuration once and measures it.
Nothing was tuned before, during or after the run. The dataset, the class
mapping, the augmentation, the schedule and the training loop are the
committed ones.

Machine-readable record: [`data/traffic6_r0/r0_eval_summary.json`](data/traffic6_r0/r0_eval_summary.json)
(metrics, counts, checkpoint/ONNX/kmodel hashes). `run.json`, the
checkpoints, detections and the kmodel stay in the work directory
(`C:\K230-work\yolox-train`), outside git.

Evidence: every number here is VERIFIED, measured on this run, unless
marked otherwise.

## 1. Pre-run gate (all passed before the run started)

| Check | Result |
|---|---|
| Branch, tree | `research/yolox-traffic-training` at `73199e1`, clean (`run.json`: `dirty: false`) |
| Dataset | `manifest.json` sha256 `27b45431…16274b`, byte-identical to the committed copy; split manifests and annotation files match DATASET_TRAFFIC6_R0.md section 10 |
| Classes | `traffic6`: car, truck, bus, motorcycle, bicycle, person; json category ids 1-6; DOORS indices `[2, 7, 5, 3, 1, 0]` |
| Environment | Python 3.10.20, PyTorch `2.13.0+xpu`, Intel Arc B580 (Level Zero driver 1.14.36605, 11,874 MiB) |
| `xpu_check.py` | `"ok": true`, `torch.nonzero` mismatches 0 of 200 |
| YOLOX | `6ddff48…`, clean (pinned) |
| Disk | 679 GB free |
| Split isolation | The exp reads `traffic_train.json` and `traffic_val.json` only. The test split (`test/`, `traffic_test.json`) is unreachable from the training path. It was read once, after training, for section 5. |

## 2. Training

Command (README step 5, XPU), `DOORS_CLASS_SET=traffic6`:
`train_loop.py --device xpu --exp exps\yolox_tiny_traffic_416.py --out …\runs\traffic6_r0 --batch 16 --workers 4 --amp fp16`

| | |
|---|---|
| Epochs | 300 of 300, one session, no interruption, no resume |
| Wall time | 60 h 53 min (2026-10-06 11:11:57 to 2026-10-09 00:04:55 UTC) |
| Training / validation time | 217,251 s / 1,721 s (44 validations) |
| Iterations | 2,681 per epoch, 804,300 in total, batch 16 |
| Throughput | 58.7 images/s wall (session); 115 images/s steady GPU (median iteration). The 4-worker mosaic loader is the limit. |
| Epoch time | 724 s mean (607-926 s); 726 s with mosaic, 695 s without |
| Peak VRAM | 2,726 MiB reserved, 2,480 MiB allocated |
| Warnings | none |
| Non-finite values | none (the loop stops on any) |
| CPU fallback | none: every iteration's loss was on the XPU (`loss_on_device_every_iter`) |
| Image size | 416 base, multiscale 320-640 every 10 iterations (all 11 sizes seen in every epoch) |
| Optimizer | SGD, Nesterov, momentum 0.9, weight decay 5e-4 (not on BatchNorm and biases) |
| LR | `yoloxwarmcos`: 0 to 2.5e-3 over 5 epochs (13,405 iterations), cosine to 1.25e-4 at iteration 764,085, flat to the end |
| AMP | fp16 autocast with GradScaler |
| EMA | decay 0.9998, 804,300 updates |
| Seed | 20261004 |

**Loss** (epoch mean): 12.66 (epoch 1), 5.73 (10), 4.67 (100), 4.34 (200),
4.10 (284). At epoch 285 it rises to 4.36 because the L1 term (0.60) joins.
It then falls to 4.15 at epoch 300 (L1 0.57).

**Mosaic and L1 transition.** Upstream's rule was preserved unchanged,
including its off-by-one: mosaic in epochs 1-284 (42,896 of 42,896 samples
each epoch), none in 285-300 (0 samples), so 16 epochs without mosaic. L1
was off (0.0) before epoch 285 and on from 285. Validation ran every 10
epochs, then every epoch from 285. `last_mosaic_epoch_ckpt.pth` was written
at the switch.

**Validation history** (plain COCOeval, val mAP50:95, EMA weights): 0.238
(epoch 10), 0.380 (50), 0.414 (100), 0.441 (200), 0.449 (270), 0.448
(280), 0.464 (285), 0.471 (286), 0.478 (291 and 299), 0.477 (300). The
no-mosaic phase added +0.030.

**Checkpoint selection.** `best_ckpt.pth` is epoch 299, selected by the
**original plain-COCOeval val mAP50:95** (0.47781) of `train_loop.validate`,
the committed rule. It was not selected by the corrected metric of
section 4. Any corrected selection must use the validation split only,
never test. Epoch 299 beat epoch 291 by 0.00013 and the final epoch by
0.0005: the end of the run is flat.

## 3. Run record and checkpoints

`run.json` parses and holds the following:
- the DOORS commit and its clean state, and the YOLOX commit and its clean state
- the dataset manifest and its sha256, and the class mapping
- the seed, the B580 with its driver and runtime (`xpu` 20260000), Python, PyTorch and every package
- batch, image size and multiscale, epochs, optimizer, LR schedule, AMP and EMA
- 44 validations, the best-selection metric and rule, and every checkpoint's sha256
- `"pretrained_weights": null`, and no usernames or secrets

| File | Epoch | sha256 | Bytes |
|---|---|---|---|
| `best_ckpt.pth` | 299 | `a501b84c4c89a2f14507ff3434d3b4722fcfc76c7e73843b28ab8faaf0d55d6f` | 60,923,249 |
| `final_ckpt.pth` | 300 | `f1a1575935c39ddfa6b2c1c16dc3bf20ec036a5a2064ccc62c35749b268179c3` | 60,924,421 |
| `latest_ckpt.pth` | 300 | `64d7a126d225d94b1e5589a45cbd5449f6b5ff3ab92191ca9fe533adfd91d359` | 60,925,593 |
| `last_mosaic_epoch_ckpt.pth` | 284 | `21e88bcbe162c700b447a6536c58c64c2b336ca6a2065a21d3dbedda46e062a5` | 60,937,653 |

`final_ckpt.pth` and `latest_ckpt.pth` hold the same state. Their bytes
differ because `torch.save` names the archive's root folder after the
file.

## 4. Evaluation semantics (custom mixed-dataset metric)

`traffic_{val,test}.json` are plain COCO files. Plain COCOeval scores
every image as exhaustively labelled for all six classes. COCO is. Open
Images is not: it boxes only classes a human verified in the image.

`tools/vision/training/eval/eval_mixed.py` applies this rule:
- **COCO images:** all six classes are scored.
- **Open Images images:** a class is scored only where its **primary**
  label (Car, Truck, Bus, Motorcycle, Bicycle, Person) is human-verified
  present or absent (`human_labels` in `manifest_{val,test}.jsonl`).
- Everything else is left out: the boxes do not count as misses and the
  detections do not count as false positives.
- Neither of these makes a class scored:
  - a verified folded label (Van, Limousine, Taxi for car; Man, Woman, Boy,
    Girl for person), present or absent;
  - an existing box, which may come from such a label.

This is a **custom mixed-dataset metric, not the official Open Images
protocol**. It shares that protocol's core rule: a class is scored only
where it was verified. It differs in three ways:
- COCO AP@[.50:.95] and AP50 with 101-point interpolation, instead of
  Open Images AP@0.5
- group-of boxes are COCO crowd regions
- no hierarchy expansion of detections

Precision and recall are at score 0.35 (`decoder_check`'s 350 per mille)
and IoU 0.50, from pycocotools' matching, as in `train_loop.py`.

**Plain-COCO parity (VERIFIED twice).** The evaluator's plain-COCOeval
figure is bit-identical to `train_loop.validate`'s recorded value:
- on the epoch-10 checkpoint: 68,332 detections, every field, max
  absolute difference 0
- on `best_ckpt.pth`: 0.47781159669973094

**What the rule leaves out:**

| | car | truck | bus | motorcycle | bicycle | person |
|---|---|---|---|---|---|---|
| val: Open Images images scored (of 702) | 606 | 77 | 25 | 67 | 114 | 595 |
| val: boxes dropped (class not verified) | 11 | 0 | 0 | 0 | 0 | 156 |
| test: Open Images images scored (of 2,273) | 2,010 | 249 | 67 | 220 | 310 | 1,933 |
| test: boxes dropped (class not verified) | 36 | 0 | 0 | 0 | 0 | 454 |
| test: detections ≥ 0.35 left out | 40 | 39 | 13 | 4 | 8 | 563 |

- **Cost of the strict person rule (observed).** Many left-out person
  detections are correct detections of people boxed as Man, Woman or Boy
  in images where Person itself was never verified. The rule drops those
  true positives together with the unknowable false positives. This is
  why mixed person AP is slightly below plain.
- **Taxi assumption (ASSUMED, measured).** Taxi is a sibling of Car in
  Open Images, not a child. A verified "Car absent" is still read as no
  car of any kind. Car false positives that rest on this are those on a
  Car-absent, Taxi-unverified image that no other box explains: 2 of 142
  on val, 14 of 337 on test.

## 5. Results: `best_ckpt.pth` (epoch 299)

### Overall

| Split | Metric | mAP50 | mAP50:95 | Precision | Recall | TP / FP / GT |
|---|---|---|---|---|---|---|
| val (1,615 images) | **mixed** | **0.671** | **0.482** | **0.851** | **0.586** | 3,863 / 679 / 6,595 |
| val | plain (reference) | 0.663 | 0.478 | 0.845 | 0.591 | |
| test, held out (3,774 images) | **mixed** | **0.714** | **0.502** | **0.872** | **0.618** | 9,211 / 1,354 / 14,894 |
| test | plain (reference) | 0.703 | 0.496 | 0.866 | 0.626 | |

### Per class (mixed metric; plain AP50:95 in brackets)

| Class | val AP50 | val AP50:95 | val P | val R | val GT | test AP50 | test AP50:95 | test P | test R | test GT |
|---|---|---|---|---|---|---|---|---|---|---|
| car | 0.716 | 0.520 (0.523) | 0.850 | 0.625 | 1,290 | 0.790 | 0.597 (0.598) | 0.875 | 0.702 | 3,375 |
| truck | 0.507 | 0.369 (0.348) | 0.767 | 0.385 | 205 | 0.559 | 0.399 (0.367) | 0.747 | 0.440 | 430 |
| bus | 0.671 | 0.569 (0.561) | 0.846 | 0.592 | 130 | 0.706 | 0.559 (0.551) | 0.811 | 0.660 | 215 |
| motorcycle | 0.687 | 0.445 (0.441) | 0.913 | 0.556 | 171 | 0.734 | 0.488 (0.485) | 0.931 | 0.637 | 443 |
| bicycle | 0.748 | 0.565 (0.563) | 0.844 | 0.697 | 178 | 0.785 | 0.540 (0.537) | 0.886 | 0.719 | 527 |
| person | 0.698 | 0.426 (0.430) | 0.852 | 0.580 | 4,621 | 0.712 | 0.431 (0.440) | 0.873 | 0.591 | 9,904 |

- **Truck** is the weakest class: AP50:95 0.37 on val and 0.40 on test,
  with recall 0.39 and 0.44. It is also the class the corrected metric
  moves most: +0.02 and +0.03 AP50:95, because Open Images truck is
  verified in only 77 val and 249 test images.
- **Test runs above val.** The two splits differ in mix: the test split
  holds more large Open Images cars (DATASET_TRAFFIC6_R0.md section 5).
  Neither split selected anything on the other.

## 6. Small-object performance (proxy for the distant-vehicle use case)

No source has distance labels. This is **small-object performance**, by
the dataset's size buckets: side416 = sqrt(w × h) × 416 / max(W, H), in
model pixels after the 416 letterbox. The buckets are very small < 8,
small 8-16, medium 16-64 and large ≥ 64.

Recall is at 0.35 and IoU 0.50, with 95 % Wilson intervals. Buckets under
30 boxes are thin and only bound the recall.

**Held-out test, vehicles:**

| Class | Bucket | GT | AP50 | AP50:95 | Recall [95 % CI] |
|---|---|---|---|---|---|
| car | very small | 115 | 0.045 | 0.008 | **0.000** [0.000, 0.032] |
| car | small | 299 | 0.268 | 0.093 | **0.171** [0.132, 0.217] |
| car | medium | 978 | 0.646 | 0.391 | 0.530 [0.498, 0.561] |
| car | large | 1,983 | 0.946 | 0.786 | 0.907 [0.894, 0.919] |
| truck | very small | 4 | - | - | 0/4 (thin) |
| truck | small | 29 | 0.052 | 0.020 | **0/29** [0.000, 0.117] (thin) |
| truck | medium | 107 | 0.173 | 0.086 | 0.140 [0.087, 0.219] |
| truck | large | 290 | 0.733 | 0.549 | 0.600 [0.543, 0.655] |
| bus | very small | 2 | - | - | 0/2 (thin) |
| bus | small | 8 | - | - | **0/8** [0.000, 0.324] (thin) |
| bus | medium | 50 | 0.249 | 0.153 | 0.280 [0.175, 0.417] |
| bus | large | 155 | 0.867 | 0.711 | 0.826 [0.758, 0.877] |
| motorcycle | very small / small | 22 / 28 | - | - | 0/22, 1/28 (thin) |

**Validation, same classes:**
- car very small: 1/76, recall 0.013 [0.002, 0.071]
- car small: 0.205 [0.152, 0.270] of 176
- truck small: 0/20; bus small: 0/4

**All six classes, test:**

| Bucket | GT | Recall [95 % CI] | Mean AP50:95 |
|---|---|---|---|
| very small | 738 | 0.016 [0.009, 0.028] | 0.014 |
| small | 1,746 | 0.174 [0.157, 0.193] | 0.046 |
| medium | 6,210 | 0.563 [0.551, 0.576] | 0.262 |
| large | 6,200 | 0.873 [0.864, 0.881] | 0.667 |

The four vehicle classes pooled on test: very small 0/143 (CI to
0.026), small 0.143 [0.111, 0.183] of 364.

**Findings:**
- **Very-small cars are not detected at the 0.35 operating point:**
  0 of 115 on test, 1 of 76 on val. AP50 of 0.05-0.09 shows a few low-score
  hits only.
- **Small cars:** recall 0.17 (test) and 0.21 (val), well measured (299
  and 176 boxes).
- **Small trucks and buses:** none detected (0/29 and 0/8 on test). The
  counts are too thin for a recall value. The upper bounds are 0.12 and
  0.32.
- Detection is reliable only from medium size up. Large vehicles reach
  0.83-0.91 recall (car, bus), truck 0.60.

## 7. Error analysis (test, score ≥ 0.35, IoU 0.50, scored pairs only)

**False positives by class**, split by the nearest ground truth: confused
(IoU ≥ 0.5 with another class's box), localisation/duplicate (IoU 0.1-0.5
with any box) and background (no box above 0.1):

| Class | FP | Confused | Localisation / duplicate | Background |
|---|---|---|---|---|
| car | 337 | 87 (truck 70, motorcycle 7, bus 6, person 3, bicycle 1) | 164 | 86 |
| truck | 64 | 32 (car 23, bus 9) | 17 | 15 |
| bus | 33 | 8 (truck 7, car 1) | 12 | 13 |
| motorcycle | 21 | 5 (person 4, bicycle 1) | 13 | 3 |
| bicycle | 49 | 8 (motorcycle 6, person 2) | 31 | 10 |
| person | 850 | 12 | 516 | 322 |

**False negatives by class and size:**

| Class | FN | Very small | Small | Medium | Large |
|---|---|---|---|---|---|
| car | 1,007 | 115 | 248 | 460 | 184 |
| truck | 241 | 4 | 29 | 92 | 116 |
| bus | 73 | 2 | 8 | 36 | 27 |
| motorcycle | 161 | 22 | 27 | 65 | 47 |
| bicycle | 148 | 3 | 14 | 79 | 52 |
| person | 4,053 | 580 | 1,117 | 1,987 | 369 |

**Main confusion: car and truck** (car on a truck box 70, truck on a car
box 23; val 39 and 9). Bus and truck follow (9 + 7).

Inspected examples:
- The top "car" false positives on Car-absent Open Images images are
  pickups and a lorry front boxed as Truck. Pickups sit on the car/truck
  boundary.
- Of 150 large Open Images cars missed, 31 were detected as truck (25) or
  bus (6) instead. One was a van labelled Van, and so car, called truck. Most of
  the rest are heavily occluded or truncated close-ups: a grille filling
  the frame, cars hidden behind people.

**Missed small objects.**
- 363 of the 414 very small and small test cars were missed.
- The inspected misses are distant cars in street scenes, mostly from
  COCO: 103 of the 115 very-small misses and 204 of the 248 small misses.

**Suspicious background detections** (inspected):
- person on dog and parrot close-ups (COCO)
- car on a tyre rack, and on a bench close-up (COCO)

These are real model errors.

**Incomplete labelling that affects interpretation** (observed, not
corrected):
- *Open Images, class verified present but instances unboxed.*
  - Partly visible cars at the image border are counted as background car
    false positives although Car is verified present in the image.
  - A second pickup is unboxed beside a boxed one.
  - A partly visible person behind a car is unboxed.
  - The custom metric cannot remove these: the class is verified, so its
    boxes are taken as exhaustive.
- *COCO is not perfect either.* A person lying on a float in the sea (COCO
  test image) is unboxed and counts as a background person false positive.
- *Open Images Person rule.* See section 4: correct person detections are
  left out where only Man, Woman or Boy was verified.

**Data quality note.** One test image (`oi:test:ffe3c978a2f27cd9`) gives a
libjpeg "Corrupt JPEG data: bad Huffman code" warning in OpenCV. It
decodes and was scored, and Pillow decodes it without error. One other
(`oi:test:2e3ec3271c61954c`) has no JPEG end marker and also decodes. The
dataset was not changed.

## 8. Baseline comparison

**Direct comparison available: NO.**
- The 2026-10-04 evaluation frames, ground truth and harness
  (`out/detector-eval`, WSL `~/work/detector-eval`) are not on this PC.
- `eval/compare.sh` cannot run here. No substitute set was built.
- The `traffic6_r0` numbers above are on a different set, with different
  metrics. They are **not comparable** with the figures below.

**Historical context only** (2026-10-04 evaluation, TRAINING_PLAN section
1, DOCUMENTED, not re-measured):
- vehicle recall: upstream YOLOX-Tiny 416 a16 0.90, YOLOv8n 320 0.63
- upstream YOLOX-Tiny 416 a16 on the KPU: 53 ms, 5.89 MB

## 9. Export to K230

Pipeline unchanged: `convert.sh` in WSL2 (torch 2.1.2+cpu export, nncase
2.11.0), 416 × 416, int16 activations, uint8 weights, NoClip calibration on
100 seeded training images.

| | |
|---|---|
| Checkpoint | `best_ckpt.pth`, sha256 `a501b84c4c89a2f14507ff3434d3b4722fcfc76c7e73843b28ab8faaf0d55d6f` |
| ONNX | sha256 `48722fb6ca7d38c46c9e8cb120a9151f65fca92144913b860c98a9152ecdab9c`; opset 11; output `[1, 84, 3549]`; ops within upstream's set (`ops_beyond_upstream: []`); max difference vs PyTorch 4.0e-4 |
| Class rows | trained rows at DOORS indices `[2, 7, 5, 3, 1, 0]`, equal to the native head within 2.7e-8; all other 74 class rows score 0.0 |
| kmodel | sha256 `94a20ac01692d9a7b6dfc7f1497c1024141fcef997ff055c219a03e105ded268`; **5,894,104 bytes**; deterministic (two compiles identical); no unsupported-operator errors |
| Simulator vs float (5 check pictures) | box cosine ≥ 0.9999; score cosine 0.991-0.993 where objects are present; 0 non-finite values |
| DOORS decoder (`convert.sh`) | accepted all 5 outputs, 0 bad rows; labels seen: person (the 5 fixed check pictures hold people only) |
| DOORS decoder, extra vehicle check | 6 val images through the same kmodel (simulator) and `decoder_check`: car, truck, bus, motorcycle (`moto`), bicycle (`bike`), person all decoded with the right label; 0 bad rows |

- **CPU fallback.** `cpu_fallback_suspect` is null: no upstream reference
  kmodel is on this PC to compare against.
- **Untested here.** KPU latency, CMA use and CPU fallback can only be
  proven on unit B. They were not measured, by instruction: no hardware
  deployment.

## 10. What was not done

- No dataset change, rebalancing, negatives, augmentation or LR tuning.
  No threshold tuning, no R1.
- No hardware deployment. The unit B latency gate (README step 8) is open.
- No direct comparison with YOLOv8n or upstream YOLOX (section 8).
- The test split was used once, for this report. It selected nothing.
