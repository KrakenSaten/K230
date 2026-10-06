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

**Update 2026-10-06.** The R0 dataset is built: `traffic6_r0`, COCO allow
tier + Open Images V7, train 42,888 / val 1,615 / held-out test 3,774. The
counts, gates and gaps are in
[DATASET_TRAFFIC6_R0.md](DATASET_TRAFFIC6_R0.md). R0 therefore uses
`traffic6` and Open Images. The table and plan below are the 2026-10-04
state.

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

**Training** (`smoke_train_cpu.py`, now `smoke_train.py --device cpu`; YOLOX model/loader/loss/optimiser/EMA,
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

1. **Class set:** `traffic4` (brief) or `traffic6` (keeps Traffic's bike and person counters), section 3. *Decided 2026-10-06: `traffic6`.*
2. **CC BY-SA COCO images:** in or out (+58 % COCO vehicle boxes), DATASET_PROVENANCE U5.
3. **Open Images V7:** approve the metadata download (several GB) and the prepare script. *Done 2026-10-06. It is accepted under the existing policy (DATASET_PROVENANCE 4.4) and is in `traffic6_r0`. `--no-openimages` reverts it.*
4. **GPU host:** where R0/R1 run (cloud single 24 GB GPU suggested; laptop cannot).
5. **DOORS test capture:** when and where, with the privacy handling of DATASET_PROVENANCE section 5.

## 12. What was not done or tested

- No real training run, no real candidate, no comparison numbers for a DOORS model. The only GPU runs are the Arc B580 smoke runs (sections 13 and 14).
- No unit B run: KPU latency and the CPU-fallback proof wait for a candidate.
- Open Images: audited, downloaded (35,699 images) and built into `traffic6_r0` (DATASET_TRAFFIC6_R0.md). No training on it yet.
- The CUDA path (`train.sh` through YOLOX's Trainer, tensorboard, val AP)
  is not exercised. The CPU and XPU smoke runs use YOLOX's model, loader,
  loss, optimiser, scheduler and EMA, but their own loop.
- No DOORS test-set capture.

## 13. Intel Arc B580 (XPU) smoke run (2026-10-06, VERIFIED)

The question: can the office PC's Arc B580 run this pipeline through
upstream PyTorch XPU? No real training was started.

**Machine.**
- Windows 11 Pro 10.0.26300
- Ryzen 7 9700X (8 cores), 31 GB RAM
- Arc B580, 12 GB, driver 32.0.101.8531 (Level Zero 1.14.36605)
- WSL 2.7.13.0 with Ubuntu 22.04.5. WSL sees `/dev/dxg`, but has no
  Intel compute runtime, so training runs natively on Windows.

**Stack.**
- Python 3.10.20 (uv)
- torch 2.13.0+xpu, torchvision 0.28.0+xpu
- Intel SYCL runtime / dpcpp-cpp-rt / MKL 2026.0.0, intel-pti 0.17.0
- `requirements-train.txt` unchanged
- YOLOX @ 6ddff48, unmodified (clean tree)
- No IPEX

### Finding: torch 2.14.x+xpu computes wrongly on this machine

With torch 2.14.0 and 2.14.1+xpu (SYCL runtime 2026.1):
- `torch.nonzero` and boolean-mask indexing return wrong indices, for
  example `[1, 1, 6]` instead of `[1, 3, 6]`.
- 277 of 450 random cases were wrong.
- YOLOX's label assignment then raised a device assert and
  `UR_RESULT_ERROR_DEVICE_LOST`, or crashed the process.
- It happens with the Level Zero V2 adapter and with the legacy one.

The same test passes, 0 wrong, on 2.8.0, 2.10.0, 2.12.1 and 2.13.0+xpu.
The cause is ASSUMED to be the 2026.1 runtime against this driver. A newer
Arc driver may fix it; that is not tested, because drivers were not changed.

The fixes:
- `PINS.env` pins `TORCH_XPU_VERSION=2.13.0`.
- `xpu_check.py` tests nonzero and masks against the CPU and stops on any
  mismatch. It trips on 2.14.0 (119 of 200 wrong).

### CUDA assumptions in YOLOX (audit)

| Where | Assumption | Effect on XPU |
|---|---|---|
| `core/trainer.py` | `cuda:{rank}` device, `torch.cuda.set_device`, `torch.cuda.amp.GradScaler`/`autocast`, CUDA `DataPrefetcher` | Trainer unusable |
| `core/launch.py` | asserts `torch.cuda.is_available()`, NCCL | unusable |
| `data/data_prefetcher.py` | CUDA streams, `.cuda()` | unusable |
| `exp/yolox_base.py` `random_resize` | `torch.LongTensor(2).cuda()` | reimplemented in the loop |
| `exp/yolox_base.py` loader | `pin_memory=True` | works (pins for XPU) |
| `evaluators/coco_evaluator.py` | `torch.cuda.FloatTensor`, `.cuda()` | val AP needs its own eval on XPU |
| `models/yolo_head.py` assignment | `torch.cuda.amp.autocast(enabled=False)` around BCE | under XPU autocast BCE refuses to run; shimmed in the loop |
| `models/yolo_head.py` OOM fallback | matches the "CUDA out of memory" string, then `.cuda()` | an XPU OOM is fatal, with no silent CPU fallback (acceptable) |
| `models/yolo_head.py` | `torch.cuda.empty_cache()` per image | no-op |
| `utils/dist.py`, `utils/metric.py` | CUDA sync and memory helpers | not used by the loop |
| `train.sh` | requires `nvidia-smi` | refuses XPU |

The model, loss, EMA, optimiser, scheduler and mosaic loader are
device-neutral. YOLOX is not patched: `run_record.py` refuses a dirty tree.

### Changes in this pipeline

- **`smoke_train.py`** (was `smoke_train_cpu.py`):
  - `--device auto|cpu|xpu|cuda`
  - `--amp off|bf16|fp16`, where fp16 uses `torch.amp.GradScaler`
  - `--multiscale`: the Trainer's resize every 10 iterations
  - `--workers`
  - `--parity`: one training step on the device against the CPU, from the same weights
  - The checkpoint is saved with CPU tensors.
  - Under XPU AMP it maps YOLOX's `torch.cuda.amp.autocast(enabled=False)`
    to the device, so the assignment cost stays fp32 as upstream intends.
- **`xpu_check.py`**: device, memory, matmul vs CPU, nonzero/mask vs CPU,
  timing and a training step.
- **`setup_env_xpu.ps1`**: the Windows XPU environment, tested on a fresh
  directory.
- **`PINS.env`**: `TORCH_XPU_VERSION`, `TORCHVISION_XPU_VERSION`.

Not changed: the architecture, head, class mapping, dataset, augmentation,
export, nncase settings and decoder.

### Basic XPU check (`xpu_check.py`)

| Check | Result |
|---|---|
| Device | Arc B580, 11,874 MB |
| fp32 matmul 4096² | 13.3 TFLOPS; CPU 20x slower |
| Max relative error vs CPU | 3e-6 |
| Allocator | 192 MB allocated, as expected |
| nonzero / masks | 0 of 200 wrong |
| Training step | ran, loss fell |

### Data

The smoke set was rebuilt with the documented recipe; it was not on this PC.
- `prepare_coco_traffic.py --limit-train 160 --limit-val 24`
- train 184 images / 663 boxes, val 28 / 91, 37 MB (as on 2026-10-04)
- COCO annotation sha256 equal to `sources.json`
- No exclusion list: the 2026-10-04 one is not on this PC, and it dropped 0 files then.
- Image list sha256: train `6215f81e…6b1ca8`, val `60390a0c…347ff8`

### Training (`xpu_smoke1`)

Settings:
- from scratch
- 300 iterations x batch 16
- fp16 autocast + GradScaler
- multiscale 320-640 (all 11 sizes seen)
- fixed lr 0.002, 4 loader workers

| Item | Value |
|---|---|
| CPU parity, first step | loss rel. diff 1e-7; gradient cosine 1.0000000, rel. norm diff 1e-4 |
| total_loss | 18.5 → 10.2 (mean of first/last 10: 16.4 → 9.9); iou 4.79 → 3.51 |
| Runtime | 119.5 s |
| Steady iteration | median 0.126 s, so 127 images/s; 0.118 s at 320, 0.16 s at 640 |
| One-off compile | about 5-9 s the first time each input size appears; 81 s of the 119 s |
| Peak memory (PyTorch) | 2,429 MiB allocated, 2,638 MiB reserved |
| Device total in use | 7.8 GB, including the desktop and other processes |
| CPU fallback / unsupported ops | none; the loss stays on xpu every iteration |
| Warnings | none in the fp16 run; fp32 runs show YOLOX's `torch.cuda.amp.autocast` FutureWarning |
| Checkpoint sha256 | `20a27671d4143bdaaaec35a6541a5bfd76923f04fdc201cbabc6a19b35c5f7e7` |

Shorter probes at batch 16, 40 iterations, 416:

| Precision | Images/s | Reserved |
|---|---|---|
| fp32 | 59 | 2.2 GB |
| bf16 | 131 | 1.1 GB |
| fp16 | 129 | 1.1 GB |

Iteration time barely changes with input size. The CPU mosaic loader is
probably the limit (ASSUMED); try more workers on the 8-core CPU.

### Conversion

The existing `convert.sh` ran unchanged in WSL, with the pinned torch
2.1.2+cpu and nncase 2.11.0:

| Item | Value |
|---|---|
| Checkpoint | the torch 2.13 XPU checkpoint loads in torch 2.1.2 |
| ONNX | `dc08d58d7ae369b339d3b471156ba0e54b104a9ad92de6d06efaf4360ae1d6a6`, `[1, 84, 3549]`, op set equal to upstream's, max diff to PyTorch 4e-4, trained columns vs native head 5e-8, untrained 0.0 |
| kmodel | `5a3a38a4976cc4bb0f9ebe4dffaeeb3d42677ab11136a71426fafa82d5cd2615`, 5,907,096 B (1.002 of upstream a16's 5,894,104 B) |
| Determinism | compiled twice, the same sha256 |
| Compile time | 41 s |
| Unsupported operators | none |
| Simulator vs float | box cosine ≥ 0.9988; score cosine 0.93-0.97 (all scores ≤ 0.09, untrained) |
| Non-finite values | 1 kmodel value on 1 of 5 pictures (the section 7 finding) |
| DOORS decoder | accepted 5/5 outputs: 3549 rows, 0 bad rows, 0 detections ≥ 0.35 (expected) |
| Label path | at 0.1 % only car, truck, bus and motorcycle appear |

The `--reference` kmodel is not on this PC, so `cpu_fallback_suspect` was
not computed. The size ratio above is the same heuristic by hand.

### Verdict and the gap before real training

The Arc B580 trains this model correctly and fast enough:
- R0 needs about 1.06 M image passes, so about 2.5 h at about 125
  images/s (ESTIMATE, smoke data).
- The trained checkpoint goes through the proven conversion unchanged.

What blocks a real run is the pipeline, not the GPU. `train.sh` and
YOLOX's Trainer cannot use XPU. A real XPU run needs a training loop in
this tree with the Trainer's features:
- epochs and the warm-cosine schedule
- the last 15 epochs without mosaic, with L1 loss
- val AP, using a device-neutral evaluation
- `best_ckpt.pth` and `latest_ckpt.pth`
- `run.json` with the XPU device and driver (`run_record.py` records CUDA only)

Section 14 closes this gap with `train_loop.py`.

## 14. Epoch training loop on the Arc B580 (2026-10-06, VERIFIED)

The question: does `train_loop.py` give the Arc B580 the Trainer's full
recipe, so a real run can start once the dataset exists? No real training
was started. Both runs use the smoke set from section 13 and a compressed
schedule; the real run keeps the exp's 300 / 5 / 15 / 10.

### What `train_loop.py` keeps from upstream (YOLOX @ 6ddff48)

| Feature | Upstream Trainer | `train_loop.py` |
|---|---|---|
| Model, loss, loader, optimizer, scheduler | the exp's own | the same calls, YOLOX unpatched |
| lr | set after each step from `progress+1`; the first step at `warmup_lr` 0 | the same |
| Mosaic → no mosaic | closes when `epoch+1 == max_epoch - no_aug_epochs`; L1 on, `eval_interval` 1 | the same rule, so 16 epochs without mosaic at 300/15 (upstream's off-by-one, kept) |
| EMA | `ModelEMA(0.9998)`, every iteration | the same |
| Multiscale | new size every 10 iterations in `random_size` | the same, without the CUDA tensor |
| Best | AP50:95 of the EMA weights | the same metric |

### Where it differs, and why

| Upstream | `train_loop.py` | Why |
|---|---|---|
| Resume loads the EMA weights into the raw model; no scaler, RNG, sampler position or input size | all of them restored | an exact continuation: on the CPU a stopped and resumed run equals a straight run bit for bit (tests) |
| `latest` saved before the epoch's eval | after it | a resumed run knows the best AP so far |
| `best_ap` starts at 0 with a strict `>` | the first validation always becomes best | a model at AP 0 still gets a `best_ckpt.pth` |
| `last_mosaic_epoch` stores `epoch+1` before training it | stores the epochs completed | resuming from it does not skip an epoch |
| Up to `workers × 2` mosaic batches already queued at the switch | the loader iterator is rebuilt from the next unseen sample | no mosaic sample after the switch |
| `COCOEvaluator` (CUDA tensors) | own loop: device forward, YOLOX `postprocess` and COCO conversion, pycocotools | runs on xpu, cuda and cpu |
| `last_epoch_ckpt.pth` | `final_ckpt.pth` after the last epoch | the deliverable has a fixed name |

Checkpoints keep upstream's layout where `export_onnx.py` reads it:
`"model"` is the EMA state_dict, all tensors on the CPU. They add
`train_model`, `optimizer`, `ema_updates`, `scaler`, `rng`, `samples_seen`,
`input_size`, `config`, `metrics`, `best_ap`/`best_epoch` (about 61 MB).

**Best checkpoint:** val mAP50:95 (COCO AP@[.50:.95], all areas,
maxDets 100) of the EMA weights on the validation split only. The first
validation always becomes best; later ones only when strictly greater.

**Precision/recall:** at score ≥ 0.35 (decoder_check's 350 ‰) and IoU 0.50,
counted from pycocotools' own matching, over all classes. Per-class AP50:95,
AP50, P and R are in each record.

With more than 0 loader workers, augmentation is not bit-reproducible:
YOLOX reseeds each worker from `uuid4`. With `--workers 0` a run is
fully seeded.

### Runs

Both: Arc B580, torch 2.13.0+xpu, fp16 + GradScaler, batch 16, multiscale
320-640, 4 workers, 12 iterations per epoch (184 images), DOORS tree
`804a55a` (clean), YOLOX pinned and clean.

| | `xpu_loop1` | `xpu_loop2` |
|---|---|---|
| Purpose | the short full loop (3-5 epochs preferred) | enough steps for real detections, so validation and best selection see non-zero values |
| Epochs (warmup / no-mosaic) | 5 (1 / epochs 4-5) | 30 (2 / epochs 27-30) |
| Eval | every epoch | every 5, then every epoch from 27 |
| Stop / resume | stopped after epoch 2, resumed at 3 | stopped after epoch 15, resumed at 16 |
| Wall time (2 sessions) | 49 + 85 s | 132 + 172 s |
| Median iteration | 0.112 s = 142 images/s | 0.122 s = 132 images/s |
| Peak reserved (PyTorch) | 1,752 MiB | 2,716 MiB |
| total_loss, first → last 10 iterations | 17.0 → 15.4 (L1 added at epoch 4) | 17.5 → 11.7 (min 9.3; L1 adds about 1.5 from epoch 27) |
| Warnings / non-finite / loss off the device | none / 0 / never | none / 0 / never |

Most wall time is not training: the first use of each input size compiles
kernels (5-9 s), and each validation spawns its loader workers on Windows
(about 12 s for 28 images). The validation overhead stays small in a real
run, where an epoch is much longer.

### Evidence (from `epochs.jsonl`, `iters.jsonl`, `run.json`)

**Warmup and cosine (`xpu_loop2`).**
- lr starts at 0.
- Warmup rises quadratically to 0.0025 at step 24.
- The cosine then falls: 0.00246 (epoch 5), 0.00207 (10), 0.00068 (20), 0.00021 (25), as the first step of each epoch.
- From step 324, lr stays at the minimum, 0.000125.

**Mosaic and L1.**
- Per-sample count from `img_info`: a mosaic sample reports `(416, 3)`, and no image of the set is 3 px wide (checked at start).
- Mosaic samples per epoch: 192 of 192 in epochs 1-26, then 0 from epoch 27.
- `use_l1` and `l1_loss` are 0 before the switch and on from epoch 27 (mean 1.56).
- `xpu_loop1` shows the same at epoch 4.

**EMA.** `ema_updates` is 12 × epoch (360 at the end) and continues across the resume.

**Resume (`xpu_loop2`).**

| Item | Value |
|---|---|
| Stopped after | epoch 15 (`latest_ckpt.pth` `854eb666…`) |
| Resumed at | epoch 16 |
| lr, epoch 16 first step | 0.0012379, the value epoch 15 set next |
| total_loss | 9.51 at the last step before → 10.84 at the first step after; a fresh model starts at 17.8 |
| `ema_updates`, `samples_seen` | continue: 192, 3,072 |

The same holds for `xpu_loop1` (stop 2 → resume 3).

**Validation (`xpu_loop2`).**

| Epoch | Detections | mAP50:95 | mAP50 | Best |
|---|---|---|---|---|
| 5 | 0 | 0 | 0 | yes (first) |
| 10 | 208 | 0 | 0 | no |
| 15 | 408 | 0.00011 | 0.00021 | yes |
| 20 | 2,528 | 0.00143 | 0.01045 | yes |
| 25 | 1,348 | 0.00062 | 0.00321 | no |
| 27-30 | 635-1,048 | 0.00057-0.00140 | 0.0029-0.0066 | no |

Precision and recall at 0.35 are 0: no detection scores that high this
early.

`--eval-only` on `best_ckpt.pth` gives identical results on xpu and cpu:
- mAP50:95 0.0014259 and mAP50 0.0104467, both equal to the value recorded during training
- at 0.05: P 0.0079, R 0.011 (1 TP, 125 FP, 91 GT)
- per class AP50: car 0.0026, truck 0, bus 0, motorcycle 0.039
- the weights hash is unchanged after every validation

**Checkpoints (`xpu_loop2`).**

| File | sha256 | Epoch |
|---|---|---|
| `best_ckpt.pth` | `e95b690f2ed1ca21ee928a3d3d52909eb4b2433c4aad8554bdb2ca8ac9e47d9f` | 20 |
| `final_ckpt.pth` | `2fd0346872aa3202…` | 30 |
| `latest_ckpt.pth` | `62430c49517f3fd2…` | 30 |
| `last_mosaic_epoch_ckpt.pth` | `7b6fd3a0dd42dfe5…` | 26 |

`run.json` parses and records the commits, Python, torch, XPU runtime,
device and driver, seed, sizes, batch, epochs, optimizer groups, lr, AMP,
dataset manifest sha256, class mapping, schedule (and its overrides),
checkpoint hashes, every validation, the best rule and value, and each
session. It holds no user name or home path.

### Conversion of `best_ckpt.pth` (unchanged `convert.sh`, WSL, torch 2.1.2 + nncase 2.11.0)

| Item | Value |
|---|---|
| ONNX | `ddb4dc62fbd0a70f32c03e77030cc61122a1ef21c38ca7dff3756dfc1b9d4dc4` |
| ONNX shape and ops | `[1, 84, 3549]`; op set equal to upstream's (`ops_beyond_upstream` empty) |
| ONNX accuracy | max diff to PyTorch 2.7e-4; untrained classes 0.0 |
| kmodel | `61bf327d21b7779aa8cfc9b950dfd323875d218471a31d17b4c2a76faaec923c` |
| kmodel size | 5,894,096 B (upstream a16: 5,894,104) |
| PTQ | int16 activations, uint8 weights |
| Determinism | compiled twice, the same sha256 |
| Unsupported operators | none |
| Simulator vs float | box cosine ≥ 0.998; score cosine 0.93-0.97 (max score 0.11) |
| Non-finite values | 0 |
| DOORS decoder at 350 ‰ (the pipeline's check) | 5/5 accepted, 3549 rows, 0 bad rows, 0 detections |
| DOORS decoder at 50 ‰ | 5/5 accepted, 0 bad rows |
| Labels at 1 ‰ | only car, truck, bus and motorcycle (class mapping intact) |

Below 50 ‰ the decoder skips some rows as bad (31 at 1 ‰ on one picture).
They are low-score rows whose predicted size exceeds the decoder's sanity
limit of 4 × 416 px, up to 3,530 px. Before training converges the size
logits are still wild. Those rows have no non-finite values, no
out-of-range scores and no off-frame boxes. This is the decoder's guard
working, not a conversion fault.

### Guard

`xpu_check.py`:
- 2.13.0+xpu passes: 0 of 200 wrong.
- 2.14.0+xpu and 2.14.1+xpu are rejected: 119 of 200 wrong, exit 1.

The 2.14 venv was made from the uv cache for this check and then deleted.

### Tests (`tests/test_train_loop.py`, 15 tests, about 65 s on the CPU)

- The switch rule against upstream's literal before_train/before_epoch, for fresh and every resumed start.
- The real recipe switches at epoch 285.
- lr shape from `schedule_of`.
- The best rule.
- Precision/recall on a hand-made COCO case.
- Sampler skip, RNG round trip, the mosaic counter.
- On the smoke data:
  - straight 4 epochs vs stop + resume at the switch epoch and inside the no-mosaic phase: model, EMA, optimizer, lr, sizes and per-epoch logs bit-identical
  - phase evidence
  - checkpoints and `run.json`
  - `--eval-only` reproduces the stored metrics

Each of these mutations made the tests fail:
- no optimizer restore
- the resume epoch off by one
- the switch off by one
- no L1 at the switch
- `>=` in the best rule

### Not done

- No real training, no dataset build, nothing on unit A/B.
- The CUDA path of `train_loop.py` is written but has not run: there is no NVIDIA GPU here.
- Bit-exact resume is proven on the CPU only. On XPU with 4 workers the evidence is continuity: lr, EMA count, samples and loss level.
