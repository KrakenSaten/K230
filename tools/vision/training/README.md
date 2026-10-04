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
| `requirements-*.txt` | pinned Python packages |
| `provenance/` | `sources.json` (every input and its terms), `coco_licence_policy.json` |
| `data/class_sets.py` | class sets and their DOORS (COCO-80) indices |
| `data/prepare_coco_traffic.py` | licence-filtered COCO subset, fetched, hashed, with manifest and attribution |
| `exps/yolox_tiny_traffic_416.py` | the YOLOX experiment (upstream tiny settings, our data, fixed seed) |
| `train.sh` | GPU training through YOLOX's `tools/train.py`, with `run.json` |
| `smoke_train_cpu.py` | CPU smoke run of the same model, loader, loss and EMA |
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
DOORS_TRAIN_DATA=$W/data/smoke_traffic4 PYTHONPATH=$YOLOX_DIR python smoke_train_cpu.py --exp exps/yolox_tiny_traffic_416.py --out $W/runs/smoke1 --iters 60 --batch 4
```

```bash
bash convert.sh $W/runs/smoke1 $W/runs/smoke1/smoke_ckpt.pth $W/data/smoke_traffic4 /path/to/upstream/yolox_tiny_416_a16.kmodel
```
