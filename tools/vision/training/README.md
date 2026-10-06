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
| `data/prepare_coco_traffic.py` | licence-filtered COCO subset, fetched, hashed, with manifest and attribution |
| `exps/yolox_tiny_traffic_416.py` | the YOLOX experiment (upstream tiny settings, our data, fixed seed) |
| `train.sh` | GPU training through YOLOX's `tools/train.py`, with `run.json` |
| `smoke_train.py` | smoke run of the same model, loader, loss and EMA on cpu, xpu or cuda (was `smoke_train_cpu.py`) |
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

**Limits today.**
- YOLOX's `Trainer` (so `train.sh`) is CUDA-only and YOLOX stays
  unpatched. On XPU, only `smoke_train.py` runs: no epochs, no no-aug
  phase, no val AP, no `run.json`.
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

```bash
bash convert.sh $W/runs/smoke1 $W/runs/smoke1/smoke_ckpt.pth $W/data/smoke_traffic4 /path/to/upstream/yolox_tiny_416_a16.kmodel
```
