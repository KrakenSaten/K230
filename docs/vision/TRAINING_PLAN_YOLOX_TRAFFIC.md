# Training plan: DOORS-owned YOLOX-Tiny 416 for Traffic

Status: **research, 2026-10-04.**
- Production Vision is unchanged.
- `yolov8n.kmodel` is not replaced.
- No migration branch exists and nothing is merged.

This work lives on the local branch `research/yolox-traffic-training`.
- Pipeline: [`tools/vision/training/`](../../tools/vision/training/README.md)
- Data terms: [DATASET_PROVENANCE.md](DATASET_PROVENANCE.md)
- Earlier evaluation this plan builds on: `out/detector-eval/DETECTOR_EVAL_2026-10-04.md` (not in git)

Evidence classes: VERIFIED / DOCUMENTED / ASSUMED (AGENTS.md).

## 1. Why

The 2026-10-04 evaluation found YOLOX-Tiny 416 with int16 activations the
best Traffic detector on the K230:
- vehicle recall 0.90 against YOLOv8n's 0.63
- 53 ms on the KPU
- drop-in for the DOORS decoder

Its released weights have no stated terms, so they cannot ship. Training
our own from YOLOX's Apache-2.0 code, on data with known terms, removes
that blocker. Accuracy has to be re-earned.

## 2. Provenance rules (binding for this pipeline)

1. **Code.** YOLOX at `6ddff4824372906469a7fae2dc3206c7aa4bbaee` (Apache-2.0, VERIFIED). Pinned in `PINS.env`. `run_record.py` refuses another commit or a dirty tree.
2. **Initialisation.** None: no YOLOX checkpoint and no ImageNet backbone.
   - `get_model()` runs YOLOX's own initialisation.
   - `train.sh` passes no `-c`.
   - `run.json` records `"pretrained_weights": null`.
3. **Data.** Only inputs marked *used* in DATASET_PROVENANCE.md. COCO images are filtered per image by licence URL (CC BY 2.0 and "no known copyright restrictions" by default).
4. **Separation.**
   - The test set never enters training. `--exclude-sha256` is the guard.
   - Validation picks the checkpoint.
   - Calibration uses training images only.
5. **Records.** Every run keeps:
   - `run.json`: commits, packages, GPU, exp settings, seed, dataset manifest, checkpoint hashes
   - `manifest.json`, `images.tsv` and `ATTRIBUTION.tsv` of its dataset
   - `export/hashes.json`: checkpoint, ONNX and kmodel

## 3. Classes

**Default `traffic4`:** car, truck, bus, motorcycle.
- Trained as labels 0-3.
- Exported to COCO indices 2, 7, 5 and 3 of an 80-score output. The export
  widens each level's class conv to 80 outputs; untrained rows have zero
  weights and bias −30, so they score about 1e-13.
- VERIFIED in the smoke export: untrained classes score 0.0, and the
  trained classes equal the native head to 4e-9.
- DOORS' `vision_decode`, `vision_labels` and `vision_traffic` therefore
  read it with no code change.

**Finding: Traffic counts more than four classes.** `vision_traffic.c`
counts six: car, truck, bus, moto, bike and person (VERIFIED in the
source). A `traffic4` model loses the bike and person counters, and
DETECT/TRACK would see vehicles only. Options:
- **`traffic6`** adds bicycle and person: 11,748 COCO training images
  instead of 3,077, dominated by persons.
- Run the Traffic model only in TRAFFIC mode, with the general model elsewhere.
- Accept that Traffic counts vehicles only.

This is an owner decision before the real run. The pipeline supports all
three: `DOORS_CLASS_SET`.

**Would fewer classes help?** Possibly; it has to be measured, not assumed.
- COCO's car/truck boundary is inconsistent for pickups and vans, and
  per-class confusions cost recall at a fixed threshold.
- The DOORS tracker already groups car/truck/bus with a 3-sighting vote
  (`vision_track.c` group 1), so a merged class loses little in counting.
- The `vehicle2` set (vehicle = car + truck + bus, plus motorcycle) is
  the ablation.
- Its catch: Traffic would report every four-wheeler as "car".
- **Recommendation:** train `traffic4` first, then `vehicle2` with the same
  data and seed, and compare vehicle-group recall on the DOORS test set.

## 4. Dataset plan

| Need from the brief | Source | Coverage today |
|---|---|---|
| Ordinary vehicles | COCO (allowed tier) | 3,077 train images, 11,918 boxes (VERIFIED) |
| Small / distant vehicles | COCO; Open Images | COCO has many: 4,533 cars under 32 px at 416 in the allowed tier (VERIFIED) |
| Partially visible | COCO/Open Images (truncation is common; Open Images has an `IsTruncated` flag, DOCUMENTED) | not measured |
| Empty-road negatives | COCO street-context images with no vehicle (traffic light, stop sign, hydrant, bench, bicycle, person); DOORS captures | 9,223 COCO candidates; 15 % ratio by default (`--neg-ratio`) |
| Difficult lighting | Open Images; DOORS captures at dusk/night | weak in COCO (ASSUMED) |
| Narrow-road scenes like DOORS | DOORS captures | none yet |

**Volume problem.** The allowed COCO tier is 2.6 % of COCO train2017's
image count. That is too little for a from-scratch detector to match the
upstream one (ASSUMED). The plan:

1. **R0 (pipeline proof):** COCO allowed tier only, 300 epochs. Cheap; it
   shows the floor.
2. **R1 (real candidate):** COCO allowed tier + Open Images V7 vehicle
   images with CC BY 2.0. Open Images needs its own prepare script and a
   download approval (several GB of metadata, then only the chosen images).
3. **R2 (domain):** R1 + a separate DOORS training capture (never the test
   sessions), when the owner has shot one.
4. **Optional, owner decision:** CC BY-SA COCO images: +1,784 images and
   +6,986 boxes (U5 in DATASET_PROVENANCE.md).

**Test set to build before R1 is judged.** DOORS camera, narrow roads:
- distances of 10, 20 and 35 m, paced or measured
- day, dusk and night; rain if possible
- empty road; partially hidden vehicles
- at least 300 labelled vehicles and 100 empty frames (ASSUMED sizing)

Label it with any tool. Keep it local and blurred, with hashes in the
exclusion list.

## 5. Training configuration

From `exps/yolox_tiny_traffic_416.py` (= upstream `yolox_tiny` except data,
classes and seed):

| Item | Value |
|---|---|
| Model | YOLOX-Tiny: depth 0.33, width 0.375, SiLU; 5.03 M parameters |
| Input | 416 x 416 (test); multiscale 320-640 in training (`random_size` 10-20) |
| Augmentation | mosaic p = 1.0, scale 0.5-1.5; no mixup; HSV p = 1.0; flip 0.5; rotation 10°; translate 0.1; shear 2.0; last 15 epochs without mosaic |
| Optimiser | SGD, momentum 0.9, Nesterov, weight decay 5e-4 (YOLOX `get_optimizer`) |
| LR | 0.01 / 64 per image (batch 64: 0.01), 5 warm-up epochs, cosine to 5 % (`yoloxwarmcos`) |
| Epochs | 300 (`DOORS_MAX_EPOCH`) |
| EMA | yes (decay 0.9998); checkpoints hold EMA weights |
| Precision | fp16 mixed (`--fp16`) |
| Seed | 20261004 (`DOORS_SEED`); turns on cudnn deterministic in YOLOX |
| Evaluation | COCO mAP on val every 10 epochs; `best_ckpt.pth` by val AP |

Ablations after R1, one change each:
- `mosaic_scale` 0.25-1.5, for more small vehicles
- `vehicle2` classes
- 150 epochs against 300
- with and without the CC BY-SA images

## 6. Conversion (proven path)

Trained YOLOX → ONNX (opset 11, decode in graph, `[1, 84, 3549]` at 416) →
nncase 2.11.0 → kmodel.

| Setting | Value | Why |
|---|---|---|
| Activations | int16 | uint8 collapses YOLOX (0 vehicles, 2026-10-04) |
| Weights | uint8 | int16 weights fell back to the CPU (49.7 s per inference) |
| Calibration | 100 training images, seeded pick, NoClip | pinned by list sha256 |
| Input | u8 NCHW RGB, top-left letterbox, pad 114, swapRB | DOORS' AI2D contract today |

Checks in `convert.sh`:

- **Unsupported operators.**
  - The exported graph has the same shape and ONNX op set as upstream
    YOLOX-Tiny 80-class; `export_onnx.py` refuses any extra op.
  - nncase stops with an error on an unsupported operator; a finished
    compile means none.
- **CPU fallback.** No documented way to read it from the kmodel.
  - Heuristic (ASSUMED): kmodel size against the upstream a16 kmodel (the
    CPU-fallback build was 3.4x).
  - The header word at 0x38 is recorded but means nothing on its own: it
    read 65, 66 and 67 for graphs that differed only in the class tail.
  - Proof: the unit B KPU timing gate. About 53 ms is expected; seconds
    would mean CPU.
- **Decoder.** DOORS' unchanged `vision_decode`/`vision_nms`/labels/traffic
  sources are compiled on the host and fed the simulator output.
- **Determinism.** The kmodel is compiled twice and the two sha256 compared.
- **Accuracy of the quantization.** Simulator against float ONNX: score and
  box cosine on 5 validation images.

## 7. Smoke run (2026-10-04, VERIFIED)

Laptop: i7-8665U (4 cores), 16 GB, no GPU. WSL2 Ubuntu 22.04, Python 3.10.12,
torch 2.1.2+cpu, YOLOX @ 6ddff48.

The smoke run proves the pipeline, not a model: 300 CPU iterations teach
YOLOX almost nothing.

**Data.** `prepare_coco_traffic.py --limit-train 160 --limit-val 24`:
- train: 184 images (160 with vehicles + 24 street negatives), 663 boxes
  (car 433, truck 98, motorcycle 78, bus 54)
- val: 28 images, 91 boxes
- Licences: CC BY 2.0 180 + 27; no known restrictions 4 + 1. No other
  licence got through.
- Exclusion list: the 170 frames of the 2026-10-04 evaluation; 0 dropped
  (none is a COCO file).
- 38 MB fetched. Manifest, `images.tsv` and `ATTRIBUTION.tsv` were written.
- The selection is seeded and deterministic.

**Training** (`smoke_train_cpu.py`, YOLOX model/loader/loss/optimiser/EMA,
fresh init):

| Run | Iterations x batch | LR | Time | total_loss first → last | Checkpoint sha256 |
|---|---|---|---|---|---|
| smoke1 | 60 x 4 | YOLOX schedule (all warmup, ≤ 4e-5) | 112 s | 17.6 → 19.3 (no learning expected) | `c5da0058…a26c928` |
| smoke2 | 300 x 4 | fixed 0.002 (learning-signal check) | 1,204 s* | 17.6 → 10.7 (iou 4.70 → 3.88, conf 11.9 → 5.4) | `4c15327e…85782b7` |

\* shared the CPU with an nncase compile. Throughput alone: 2.1 images/s.

**Export** (smoke2, final design):
- ONNX `28e5b2ec…0807`, `[1, 84, 3549]`, op set identical to upstream's
- max difference to PyTorch 3e-5
- trained columns vs the native 4-class head: 4e-9
- untrained columns 0.0

**Kmodel** (nncase 2.11.0, int16 activations / uint8 weights, 100
seeded training images for calibration):

| Item | Value |
|---|---|
| sha256 | `89acd4ea635f5203f6f6cabf4394df75260fd25a49b214ef27030e25fd381138` |
| Size | 5,907,768 B (upstream YOLOX-Tiny 416 a16: 5,894,104 B, ratio 1.002) |
| Compile time | 303 s |
| Deterministic | yes: the second compile gave the same sha256 |
| Unsupported operators | none (compile completed) |
| CPU-fallback heuristic | not suspect; the device gate is still open |
| Simulator vs float | box cosine ≥ 0.9986. Score cosine 0.85-0.92, meaningless here because every score is ≤ 0.03; upstream's trained model gave 0.997 |
| DOORS decoder | accepted all 5 outputs, 3549 rows, 0 bad rows, 0 detections ≥ 0.35 (expected for an untrained model) |

**Class-tail designs tried** (all from the same smoke checkpoint; the last
one is kept):

| Tail | ONNX ops | kmodel | Outcome |
|---|---|---|---|
| One-hot MatMul | + MatMul | 5,868,504 B, deterministic | new op on the KPU path: dropped |
| Stored [1, 3549, 80] zero constant + Concat | upstream set | 6,942,448 B (+1.1 MB) | dropped |
| x*0 columns + Concat | upstream set | still compiling after 20 min | stopped, dropped |
| **Class conv widened to 80 outputs** | upstream set, upstream shape | 5,907,768 B | **kept** |

**Rerun.** A clean single pass of the final `convert.sh` (fresh directory,
23 min on the laptop) gave identical results:
- checkpoint `4c15327e…`, ONNX `28e5b2ec…`, kmodel `89acd4ea…`
- so the kmodel was built four times (two runs, two compiles each), always
  byte-identical

Conversion is reproducible for a given checkpoint and calibration list.

**DOORS label path.** At a 0.1 % threshold (diagnostic only), DOORS' own
decoder, labels and Traffic mapping turned one simulator output into:
- car → Traffic `car` (28)
- motorcycle → `moto` (3)
- bus → `bus` (1)

The scatter lands on the right names.

**Finding: non-finite box sizes in the kmodel output.**
- On one of the 5 check pictures, 176 rows had `inf` in box width or
  height (the `exp` outputs). Float ONNX had none there.
- Those rows scored ≤ 0.0003, so nothing reaches a threshold.
- DOORS' decoder skips such rows by design and counted 15 among its
  low-threshold candidates.
- Cause (ASSUMED): `exp` of an undertrained size logit overflows in the
  KPU's reduced-precision path. A trained head's size logits stay small.
- `compile_kmodel.py` now records `nonfinite_kmodel_values` per picture.
  For a real candidate it must be 0, or limited to rows far below the
  threshold. This is a gate item (section 9).

## 8. GPU and time estimate

Measured: CPU 2.1 images/s (VERIFIED, smoke). A real run on this laptop
would take months; a GPU is required.

The GPU figures below are an **ESTIMATE**. YOLOX-Tiny training is usually
limited by the CPU-side mosaic loader, not the GPU. Assumed throughput: 300-600
images/s on one 24 GB GPU (RTX 4090 / A10G / L4 class) with 8-16 CPU
workers.

| Run | Images | Epochs | Image passes | One 24 GB GPU | CPU laptop |
|---|---|---|---|---|---|
| R0: COCO allowed tier | ≈3,540 | 300 | 1.06 M | **0.5-1 h** | ≈6 days |
| R1: + Open Images vehicles (assume 30-60 k images) | 33-63 k | 300 | 10-19 M | **5-18 h** | not feasible |
| Upstream scale, for scale | 118 k | 300 | 35 M | 16-33 h | - |

- **Memory.** Batch 64 at up to 640 px multiscale, fp16, fits in 16-24 GB
  (ASSUMED). Use 8-16 CPU cores and fast local disk.
- **Cost.** Cloud single-GPU time at about $1-2/h (ASSUMED) puts R1 under
  $50.
- **First step on the GPU host.** Run one epoch, read images/s from
  `train.log` and recompute this table before booking the full run.

## 9. Validation plan (when a real candidate exists)

1. **Same set as 2026-10-04.** `eval/compare.sh` scores the new kmodel beside:
   - YOLOv8n 320
   - upstream YOLOX-Tiny 416 a16

   It reports vehicle TP/FN/FP and recall, FP list, smallest TP, the
   synthetic small-car ladder and the TP confidence distribution. Simulator
   = KPU (bit-exact, VERIFIED 2026-10-04).
2. **DOORS test set** (section 4). The same metrics per distance bucket and
   lighting, plus false positives on empty-road frames.
3. **Host conversion checks** for the candidate (`model.kmodel.json`):
   - `nonfinite_kmodel_values` is 0 on the check pictures
   - score cosine against float of about 0.99 or better (upstream: 0.997)
4. **Unit B gate** (kbench, 200 runs, median):
   - KPU ms and post ms
   - CMA use
   - kmodel size (upstream reference: 53.2 ms, 5.89 MB)
5. **Decision rule** (proposal):
   - The candidate is "competitive" when its vehicle recall on both sets is
     at least YOLOv8n's and within 0.05 of upstream YOLOX-Tiny 416.
   - Its empty-road FP must be no higher than YOLOv8n's.
   - Below that, production Vision is not migrated.

## 10. DeskBuddy follow-up (documented only, not trained)

**Next model:** YOLOX-Nano, 320 x 320, person only. It gives lightweight
person-presence detection for DeskBuddy and is kept separate from Traffic.

- **Architecture.** Upstream `yolox_nano`: depth 0.33, width 0.25,
  depthwise convolutions. Input 320; multiscale range scaled to match.
- **Classes.** `person1` (already in `data/class_sets.py`). Exported to COCO
  index 0, so the DOORS decoder still reads it unchanged.
- **Data.**
  - COCO allowed-tier person images: 10,704 training images with 45,375
    person boxes, and 484 for validation (VERIFIED, `person1` tally).
  - Desk and indoor negatives instead of street negatives: the negative
    rule in `prepare_coco_traffic.py` needs a person variant.
  - DOORS desk captures for the test set (same privacy rules).
- **Conversion.** As section 6. The 2026-10-04 evaluation found Nano also
  needs int16 activations: 24.7 ms, 1.39 MB, recall equal to YOLOv8n on
  persons with the fewest FP.
- **Separate artefacts.** Its own exp file, dataset, run and kmodel. Nothing
  is shared with the Traffic model beyond the pipeline code.

## 11. Decisions needed from the owner before R1

1. **Class set:** `traffic4` (brief) or `traffic6` (keeps Traffic's bike and person counters), section 3.
2. **CC BY-SA COCO images:** in or out (+58 % COCO vehicle boxes), DATASET_PROVENANCE U5.
3. **Open Images V7:** approve the metadata download (several GB) and the prepare script.
4. **GPU host:** where R0/R1 run (cloud single 24 GB GPU suggested; laptop cannot).
5. **DOORS test capture:** when and where, with the privacy handling of DATASET_PROVENANCE section 5.

## 12. What was not done or tested

- No GPU training, no real candidate, no comparison numbers for a DOORS model.
- No unit B run: KPU latency and the CPU-fallback proof wait for a candidate.
- Open Images: prepare script not written; no download.
- The GPU path (`train.sh` through YOLOX's Trainer, tensorboard, fp16,
  multiscale, val AP) is not exercised. The CPU smoke run uses YOLOX's
  model, loader, loss, optimiser, scheduler and EMA, but its own loop.
- No DOORS test-set capture.
