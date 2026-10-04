#!/bin/bash
# The real training run (GPU), through YOLOX's own tools/train.py.
#
# Usage: train.sh DATA_DIR RUN_DIR [GPUS] [BATCH]
#   DATA_DIR  built by data/prepare_coco_traffic.py
#   RUN_DIR   new directory for checkpoints, logs and run.json
#   GPUS      default 1; BATCH default 64 (upstream's total batch, lr scales with it)
# Environment: DOORS_CLASS_SET, DOORS_MAX_EPOCH, DOORS_SEED, DOORS_WORKERS
# (see exps/yolox_tiny_traffic_416.py), YOLOX_DIR (default from PINS.env).
#
# No checkpoint is ever loaded: from-scratch training is a provenance rule
# (docs/vision/TRAINING_PLAN_YOLOX_TRAFFIC.md), so -c/--ckpt and --resume
# are not passed through. To continue an interrupted run, restart it from
# scratch or extend this script with a resume from RUN_DIR's own
# latest_ckpt.pth (same run, same provenance) - not from any other file.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/PINS.env"
DATA=$(realpath "$1")
RUN=$(realpath -m "$2")
GPUS=${3:-1}
BATCH=${4:-64}
EXP="$HERE/exps/yolox_tiny_traffic_416.py"

[ -f "$DATA/manifest.json" ] || { echo "no dataset manifest in $DATA" >&2; exit 2; }
[ ! -e "$RUN/run.json" ] || { echo "$RUN already holds a run" >&2; exit 2; }
command -v nvidia-smi >/dev/null || { echo "no GPU (nvidia-smi); use smoke_train_cpu.py for a CPU smoke run" >&2; exit 2; }

export DOORS_TRAIN_DATA="$DATA" DOORS_OUTPUT_DIR="$RUN" YOLOX_DIR
export DOORS_SEED=${DOORS_SEED}
export PYTHONPATH="$YOLOX_DIR${PYTHONPATH:+:$PYTHONPATH}"
CMD="python tools/train.py -f $EXP -d $GPUS -b $BATCH --fp16 -expn train"
python "$HERE/run_record.py" start "$RUN" --exp "$EXP" --data "$DATA" --cmd "$CMD"
cd "$YOLOX_DIR"
$CMD 2>&1 | tee "$RUN/train.log"
python "$HERE/run_record.py" finish "$RUN"
