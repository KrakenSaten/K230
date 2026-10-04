#!/bin/bash
# Score a DOORS-trained kmodel on the 2026-10-04 evaluation set, beside
# YOLOv8n 320 (baseline) and upstream YOLOX-Tiny 416 a16, with the same
# frames, ground truth, decoder rules (conf 0.35, NMS 0.65) and scorer.
#
# The evaluation harness and its frames are NOT in git: they live in
# C:\K230\out\detector-eval (scripts) and WSL ~/work/detector-eval (frames
# as model inputs, gt.json, simulator/KPU outputs). See
# out/detector-eval/DETECTOR_EVAL_2026-10-04.md. This script only drives it.
#
# The nncase simulator is bit-exact with unit B's KPU (verified 2026-10-04),
# so recall/FP/confidence here are KPU results. KPU latency is NOT: it needs
# unit B (out/detector-eval/run_device.sh + kbench), see README "Gate".
#
# Usage: compare.sh KMODEL NAME     NAME must contain 416 (input size) and be
#                                   new, e.g. doors_yolox_tiny_416_r1
set -euo pipefail
K=$(realpath "$1"); NAME=$2
case "$NAME" in *416*) ;; *) echo "NAME must contain 416" >&2; exit 2 ;; esac
E=$HOME/work/detector-eval
H=/mnt/c/K230/out/detector-eval
source "$H/env.sh"
[ ! -e "$E/kmodel/$NAME.kmodel" ] || cmp -s "$K" "$E/kmodel/$NAME.kmodel" || { echo "$NAME exists with other content" >&2; exit 2; }
cp "$K" "$E/kmodel/$NAME.kmodel"
cd "$H"
python sim_all.py "$NAME" | tail -1
python score.py 0.35 yolov8n_320 yolox_tiny_416_a16 "$NAME"
python "$(dirname "$(realpath "$0")")/conf_summary.py" "$E/score_0.35.json" yolov8n_320 yolox_tiny_416_a16 "$NAME"
ls -l "$E/kmodel/yolox_tiny_416_a16.kmodel" "$E/kmodel/$NAME.kmodel"
