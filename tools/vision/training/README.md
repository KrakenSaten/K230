# DOORS YOLOX training pipeline (research)

This directory trains DOORS' own YOLOX detector from scratch and converts it
to a K230 kmodel. The first target is YOLOX-Tiny 416 for Traffic.

**It does not touch production Vision.**
- `yolov8n.kmodel`, `pocketos.mk` and `core/pocketvision` are unchanged.
- The one link to the product is that `decoder_check/` compiles the
  decoder's sources, as they are, into a host test.

Background:
- [docs/vision/TRAINING_PLAN_YOLOX_TRAFFIC.md](../../../docs/vision/TRAINING_PLAN_YOLOX_TRAFFIC.md): the plan, estimates and gates
- [docs/vision/DATASET_PROVENANCE.md](../../../docs/vision/DATASET_PROVENANCE.md): what data may be used, and why

## Layout

| Path | What |
|---|---|
| `PINS.env` | YOLOX commit, torch/nncase versions, seed |
| `setup_env.sh` | creates the training (GPU or CPU) and conversion environments |
| `setup_env_xpu.ps1` | creates the Intel GPU (XPU) training environment on Windows |
| `xpu_check.py` | proves PyTorch computes correctly on the Intel GPU before any training |
| `requirements-*.txt` | pinned Python packages |
| `provenance/` | `sources.json` (every input and its terms), `coco_licence_policy.json` |
| `data/class_sets.py` | class sets and their DOORS (COCO-80) indices |
| `data/prepare_coco_traffic.py` | licence-filtered COCO subset, fetched, hashed, with manifest and attribution (smoke sets) |
| `data/build_traffic_dataset.py` | the R0 dataset: `traffic6` from COCO + Open Images V7, train/val/test, provenance, dedup and leakage checks |
| `data/openimages_traffic.py` | Open Images V7 metadata: class folding, per-image licence/attribution/annotation checks |
| `data/dataset_checks.py` | consistency, provenance and leakage checks of a built dataset (run by the build and the audit) |
| `data/audit_traffic_dataset.py` | `stats.json` + `DATASET_REPORT.md`: classes, sizes, negatives, duplicates, provenance, disk |
| `provenance/flickr_licence_spotcheck.py` | licence Flickr shows today for a fixed sample; result in `flickr_spotcheck_2026-10-06.json` |
| `tests/test_dataset_pipeline.py` | focused tests of the dataset build with planted bad cases (no network) |
| `exps/yolox_tiny_traffic_416.py` | the YOLOX experiment (upstream tiny settings, our data, fixed seed) |
| `train.sh` | GPU training through YOLOX's `tools/train.py`, with `run.json` |
| `smoke_train.py` | smoke run of the same model, loader, loss and EMA on cpu, xpu or cuda (was `smoke_train_cpu.py`) |
| `train_loop.py` | epoch training on xpu, cuda or cpu: the Trainer's recipe, device-neutral val, latest/best/final checkpoints, exact resume, `run.json` |
| `tests/test_train_loop.py` | focused tests of `train_loop.py` (schedule switch, best rule, metrics, bit-exact resume on the smoke set) |
| `run_record.py` | writes and finishes `run.json` (versions, commits, settings, dataset and checkpoint hashes) |
| `export_onnx.py` | checkpoint to ONNX `[1, 84, rows]`; the class convs are widened to 80 so trained classes sit at their COCO indices |
| `compile_kmodel.py` | nncase 2.11.0: int16 activations, uint8 weights, pinned calibration, determinism and simulator checks |
| `decoder_check/` | runs DOORS' own `vision_decode`/`vision_nms`/labels/traffic on simulator output |
| `convert.sh` | export + compile + decoder check + `hashes.json` in one step |
| `eval/compare.sh` | scores a kmodel against YOLOv8n 320 and upstream YOLOX-Tiny 416 on the 2026-10-04 set |

## Recipe

Run everything on Linux (WSL2 is fine) with Python 3.10. `W` is a work
directory outside git: datasets, runs and environments never go into the
repository.

### 1. Environments

```bash
bash setup_env.sh train-gpu $W
```

```bash
bash setup_env.sh convert $W
```

On a machine without a GPU, use `train-cpu` instead of `train-gpu` (smoke
runs only).

### 2. COCO annotations

The annotations are CC BY 4.0. COCO publishes no checksum; we record ours.

```bash
mkdir -p $W/coco && cd $W/coco && curl -sSfO http://images.cocodataset.org/annotations/annotations_trainval2017.zip && unzip -q annotations_trainval2017.zip annotations/instances_train2017.json annotations/instances_val2017.json
```

The expected sha256 values are in `provenance/sources.json`.

### 3. Test-set exclusion list

Never train on test frames. Hash every file of the evaluation and test sets:

```bash
sha256sum /path/to/test/frames/* > $W/test_set.sha256
```

### 4. Dataset

Licence-filtered, fetched, hashed:

```bash
python data/prepare_coco_traffic.py --coco-ann $W/coco/annotations --out $W/data/traffic4 --class-set traffic4 --exclude-sha256 $W/test_set.sha256
```

- Read `stats.json` and `manifest.json` before training.
- `--no-download` only selects and reports.
- `--allow-review` adds CC BY-SA images, and only after an owner decision.

### 4b. The R0 dataset (`traffic6`, COCO + Open Images V7)

The real dataset. Rules, numbers and readiness gates are in
[docs/vision/DATASET_TRAFFIC6_R0.md](../../../docs/vision/DATASET_TRAFFIC6_R0.md).
Open Images metadata (3.5 GB, 11 CSV/JSON files; their sha256 are in `inputs` of [docs/vision/data/traffic6_r0/manifest.json](../../../docs/vision/data/traffic6_r0/manifest.json)):

```bash
mkdir -p $W/openimages/meta && cd $W/openimages/meta && for f in v7/oidv7-class-descriptions-boxable.csv 2018_04/bbox_labels_600_hierarchy.json 2018_04/train/train-images-boxable-with-rotation.csv 2018_04/validation/validation-images-with-rotation.csv 2018_04/test/test-images-with-rotation.csv v6/oidv6-train-annotations-bbox.csv v5/validation-annotations-bbox.csv v5/test-annotations-bbox.csv v5/train-annotations-human-imagelabels-boxable.csv v5/validation-annotations-human-imagelabels-boxable.csv v5/test-annotations-human-imagelabels-boxable.csv; do curl -sSfLO https://storage.googleapis.com/openimages/$f; done
```

Build (downloads only the selected images into `$W/raw`, about 48,500), then audit:

```bash
python data/build_traffic_dataset.py --coco-ann $W/coco/annotations --oi-meta $W/openimages/meta --raw $W/raw --out $W/data/traffic6_r0 --licence-spotcheck provenance/flickr_spotcheck_2026-10-06.json --workers 16
```

```bash
python data/audit_traffic_dataset.py $W/data/traffic6_r0 --verify-files --raw $W/raw --meta $W/openimages/meta
```

- `--no-download` selects and writes the candidate provenance only.
- Train with `DOORS_CLASS_SET=traffic6`; the exp refuses a dataset made for another class set.
- `test/` and `traffic_test.json` are the held-out test split. The exp never
  reads them; use them only for the final comparison.
- Tests: `python -m unittest tests.test_dataset_pipeline -v` (seconds, no network).

### 5. Training (GPU)

```bash
bash train.sh $W/data/traffic4 $W/runs/traffic4_r1 1 64
```

- `run.json` records everything needed to repeat the run.
- `train.sh` never loads a checkpoint: training is from scratch.
- GPU training is reproducible in recipe, not bit for bit. CUDA kernels are
  not deterministic even with `cudnn.deterministic`.

### 6. Conversion

`best_ckpt.pth` lies under `RUN/train/`:

```bash
source $W/convert.env && TRAIN_ENV=$W/train.env CONVERT_ENV=$W/convert.env bash convert.sh $W/runs/traffic4_r1 $W/runs/traffic4_r1/train/best_ckpt.pth $W/data/traffic4 /path/to/upstream/yolox_tiny_416_a16.kmodel
```

The output in `RUN/export/` has these files:

| File | Content |
|---|---|
| `model.onnx` + `.json` | ONNX sha256, op list (refused if beyond upstream's), max diff vs PyTorch and vs the native head, untrained-class maximum |
| `model.kmodel` + `.json` | kmodel sha256, nncase/.NET, PTQ config, calibration list sha256, determinism, simulator vs float, CPU-fallback heuristic |
| `decoder_check.json` | what DOORS' decoder makes of the simulator output |
| `hashes.json` | checkpoint, ONNX and kmodel sha256 in one place |

`TRAIN_ENV` and `CONVERT_ENV` are shell files to source. Write `train.env`
yourself, for example `source $W/venv-train/bin/activate`. Both default to
the 2026-10-04 evaluation's environment, which has both stacks.

### 7. Comparison

On the evaluation set:

```bash
bash eval/compare.sh $W/runs/traffic4_r1/export/model.kmodel doors_yolox_tiny_416_r1
```

### 8. Gate on unit B

The simulator cannot measure KPU latency and CMA, so this gate runs on the
device. Use the evaluation's `kbench` (out/detector-eval/run_device.sh): 200
runs, median.

Expected:
- about 53 ms, like upstream YOLOX-Tiny 416 a16
- seconds per run would mean a CPU fallback

Never send SIGTERM to a running kbench. It froze unit B on 2026-10-04.

## Smoke run (2026-10-04, this laptop, no GPU)

See the training plan, section 7, for the numbers. Reproduce it:

```bash
python data/prepare_coco_traffic.py --coco-ann $W/coco/annotations --out $W/data/smoke_traffic4 --limit-train 160 --limit-val 24 --exclude-sha256 $W/eval_set.sha256
```

```bash
DOORS_TRAIN_DATA=$W/data/smoke_traffic4 PYTHONPATH=$YOLOX_DIR python smoke_train.py --device cpu --exp exps/yolox_tiny_traffic_416.py --out $W/runs/smoke1 --iters 60 --batch 4
```

## Intel Arc (XPU) smoke run (2026-10-06, Arc B580, Windows 11)

Training runs natively on Windows with upstream PyTorch XPU (no IPEX).
Export and conversion stay on the pinned Linux path in WSL2. Numbers are
in the training plan, section 13.

**Limits.**
- YOLOX's `Trainer` (so `train.sh`) is CUDA-only and YOLOX stays
  unpatched. On XPU, train with `train_loop.py` (step 5 below).
- Use torch `TORCH_XPU_VERSION` (2.13.0) from `PINS.env`. 2.14.x computes
  `torch.nonzero` wrongly on the B580 (driver 32.0.101.8531).
  `xpu_check.py` stops on it.

**1. Environment** (PowerShell, ASCII-only work path; needs git and uv):

```powershell
pwsh -ExecutionPolicy Bypass -File setup_env_xpu.ps1 -Work C:\K230-work\yolox-train
```

It ends with `xpu_check.py`, which must print `"ok": true`.

**2. Smoke dataset.** Build it as above, into `C:\K230-work\yolox-train\data\smoke_traffic4`.

**3. Smoke training.** CPU parity first, then fp16 with multiscale:

```powershell
$env:DOORS_TRAIN_DATA='C:\K230-work\yolox-train\data\smoke_traffic4'; $env:PYTHONPATH='C:\K230-work\yolox-train\src\YOLOX'; C:\K230-work\yolox-train\venv-xpu\Scripts\python.exe smoke_train.py --device xpu --exp exps\yolox_tiny_traffic_416.py --out C:\K230-work\yolox-train\runs\xpu_smoke1 --iters 300 --batch 16 --fixed-lr 0.002 --workers 4 --amp fp16 --multiscale --parity
```

**4. Conversion** in WSL2. Use `setup_env.sh train-cpu` and `convert`.
If `python3.10-venv` is missing, create the two venvs with uv instead.
Then run:

```bash
TRAIN_ENV=$W/train.env CONVERT_ENV=$W/convert.env bash convert.sh /mnt/c/K230-work/yolox-train/runs/xpu_smoke1 /mnt/c/K230-work/yolox-train/runs/xpu_smoke1/smoke_ckpt.pth /mnt/c/K230-work/yolox-train/data/smoke_traffic4
```

The checkpoint holds CPU tensors. The pinned torch 2.1.2 loads it.

**5. Epoch training** (`train_loop.py`, training plan section 14). The
real recipe is the exp's: 300 epochs, 5 warmup, the last 16 without
mosaic and with L1, val every 10, then every epoch. Leave the schedule
options out:

```powershell
$env:DOORS_TRAIN_DATA='C:\K230-work\yolox-train\data\traffic4'; $env:PYTHONPATH='C:\K230-work\yolox-train\src\YOLOX'; C:\K230-work\yolox-train\venv-xpu\Scripts\python.exe train_loop.py --device xpu --exp exps\yolox_tiny_traffic_416.py --out C:\K230-work\yolox-train\runs\traffic4_r1 --batch 16 --workers 4 --amp fp16
```

- Batch 16 is the size verified on the B580: 2.7 GB peak reserved at 640 px.
- lr scales with the batch (`basic_lr_per_img`).
- Upstream's 64 is untested here. Check the VRAM before using it.

The run writes these files to `--out`:
- `latest_ckpt.pth` every epoch
- `best_ckpt.pth` on a new best val mAP50:95 of the EMA weights (the first validation always counts)
- `final_ckpt.pth` after the last epoch
- `last_mosaic_epoch_ckpt.pth` at the mosaic switch
- `epochs.jsonl` and `iters.jsonl`
- `run.json`

Rules:
- To continue after a stop, rerun the same command with `--resume latest`.
  A resume with a different batch, schedule, AMP or seed is refused.
- `--stop-after-epoch N` ends normally after epoch N.
- `--eval-only CKPT` validates a checkpoint on any device.
- Convert `best_ckpt.pth` with `convert.sh`, as in step 4.

Smoke run of the whole loop, with a compressed schedule and a resume:

```powershell
$env:DOORS_TRAIN_DATA='C:\K230-work\yolox-train\data\smoke_traffic4'; $env:PYTHONPATH='C:\K230-work\yolox-train\src\YOLOX'; C:\K230-work\yolox-train\venv-xpu\Scripts\python.exe train_loop.py --device xpu --exp exps\yolox_tiny_traffic_416.py --out C:\K230-work\yolox-train\runs\xpu_loop1 --batch 16 --workers 4 --amp fp16 --max-epoch 5 --warmup-epochs 1 --no-aug-epochs 1 --eval-interval 1 --stop-after-epoch 2
```

Then run the same command with `--resume latest` in place of
`--stop-after-epoch 2`.

**Tests.** About a minute on the CPU, with the same two environment
variables set:

```powershell
C:\K230-work\yolox-train\venv-xpu\Scripts\python.exe -m unittest discover -s tests -v
```

```bash
bash convert.sh $W/runs/smoke1 $W/runs/smoke1/smoke_ckpt.pth $W/data/smoke_traffic4 /path/to/upstream/yolox_tiny_416_a16.kmodel
```
