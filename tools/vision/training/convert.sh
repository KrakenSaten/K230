#!/bin/bash
# Checkpoint -> ONNX -> kmodel -> DOORS decoder, with every hash recorded.
#
# Usage: convert.sh RUN_DIR CKPT DATA_DIR [REFERENCE_KMODEL]
#   RUN_DIR   the training run (outputs go to RUN_DIR/export/)
#   CKPT      checkpoint in RUN_DIR (best_ckpt.pth for a real run)
#   DATA_DIR  the dataset the run used (calibration comes from its train split)
#   REFERENCE_KMODEL  upstream yolox_tiny_416_a16.kmodel for the CPU-fallback heuristic
# Environment: TRAIN_ENV and CONVERT_ENV, files to source for the two
# environments (default: the 2026-10-04 evaluation venv, which has both).
# SKIP_COMPILE=1 reruns only the decoder check and hashes on RUN_DIR/export.
# DOORS_ROOT: the tree whose decoder sources decoder_check builds (default:
# the tree this script is in).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/PINS.env"
RUN=$(realpath "$1"); CKPT=$(realpath "$2"); DATA=$(realpath "$3"); REF=${4:-}
OUT=$RUN/export; mkdir -p "$OUT"
EXP=$HERE/exps/yolox_tiny_traffic_416.py
DEFAULT_ENV=/mnt/c/K230/out/detector-eval/env.sh
NCAL=${NCAL:-100}

export DOORS_TRAIN_DATA=$DATA DOORS_CLASS_SET=$(python3 -c "import json;print(json.load(open('$DATA/manifest.json'))['class_set'])")

if [ "${SKIP_COMPILE:-0}" = 1 ]; then
    # Rerun only the decoder check and the hash summary on an existing export.
    [ -s "$OUT/model.kmodel.json" ] || { echo "SKIP_COMPILE=1 but no $OUT/model.kmodel.json" >&2; exit 2; }
else
# 1. ONNX (training environment: torch + YOLOX at the pin)
( source "${TRAIN_ENV:-$DEFAULT_ENV}"; export PYTHONPATH=$YOLOX_DIR
  python "$HERE/export_onnx.py" --exp "$EXP" --ckpt "$CKPT" --out "$OUT/model.onnx" ) > "$OUT/export.log" 2>&1 \
  || { tail -30 "$OUT/export.log"; exit 1; }

# 2. Calibration list: NCAL training images, a fixed seeded pick (never val/test).
python3 - "$DATA" "$NCAL" > "$OUT/calib.txt" <<'EOF'
import json, random, sys
d, n = sys.argv[1], int(sys.argv[2])
m = json.load(open(d + "/manifest.json"))
imgs = sorted(i["file_name"] for i in json.load(open(d + "/annotations/traffic_train.json"))["images"])
random.Random(m["seed"]).shuffle(imgs)
print("\n".join(d + "/train2017/" + f for f in sorted(imgs[:n])))
EOF
# Simulator check pictures: the first 5 validation images.
python3 - "$DATA" > "$OUT/check.txt" <<'EOF'
import json, sys
d = sys.argv[1]
imgs = sorted(i["file_name"] for i in json.load(open(d + "/annotations/traffic_val.json"))["images"])
print("\n".join(d + "/val2017/" + f for f in imgs[:5]))
EOF

# 3. kmodel (conversion environment: nncase 2.11.0)
( source "${CONVERT_ENV:-$DEFAULT_ENV}"
  python "$HERE/compile_kmodel.py" --onnx "$OUT/model.onnx" --out "$OUT/model.kmodel" \
      --calib-list "$OUT/calib.txt" --check-images "$OUT/check.txt" --twice ${REF:+--reference "$REF"} ) \
  > "$OUT/compile.log" 2>&1 || { tail -30 "$OUT/compile.log"; exit 1; }
fi

# 4. DOORS decoder, unchanged sources, on the simulator output compile_kmodel.py
#    kept for each check picture (<stem>.sim.f32; simulator = KPU, bit-exact).
BIN=$OUT/decoder_check
bash "$HERE/decoder_check/build.sh" "$BIN"
python3 - "$OUT" <<'EOF' | tee "$OUT/decoder_check.log"
import json, subprocess, sys
from pathlib import Path
out = Path(sys.argv[1])
checks = json.loads((out / "model.kmodel.json").read_text())["sim_vs_float"]
res = {}
for name, c in checks.items():
    t = out / (Path(name).stem + ".sim.f32")
    w, h = c["frame_wh"]
    r = subprocess.run([str(out / "decoder_check"), str(t), "416", str(w), str(h), "350"],
                       capture_output=True, text=True)
    res[name] = {"exit": r.returncode, **json.loads(r.stdout)}
(out / "decoder_check.json").write_text(json.dumps(res, indent=1))
ok = all(v["exit"] == 0 and v["accepted"] and v["bad_rows"] == 0 for v in res.values())
labels = sorted({d["label"] for v in res.values() for d in v["dets"]})
print(json.dumps({"decoder_accepts_all": ok, "labels_seen": labels}))
sys.exit(0 if ok and res else 1)
EOF

# 5. Hash summary
python3 - "$RUN" "$CKPT" "$OUT" <<'EOF'
import hashlib, json, sys
from pathlib import Path
run, ckpt, out = map(Path, sys.argv[1:])
h = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
k = json.loads((out / "model.kmodel.json").read_text())
s = {"checkpoint": {"file": str(ckpt), "sha256": h(ckpt)},
     "onnx": {"file": str(out / "model.onnx"), "sha256": h(out / "model.onnx")},
     "kmodel": {"file": str(out / "model.kmodel"), "sha256": h(out / "model.kmodel"),
                "bytes": (out / "model.kmodel").stat().st_size},
     "kmodel_deterministic": k.get("deterministic"),
     "cpu_fallback_suspect": k.get("cpu_fallback_suspect"),
     "calibration_list_sha256": k["calibration"]["list_sha256"]}
(out / "hashes.json").write_text(json.dumps(s, indent=1))
print(json.dumps(s, indent=1))
EOF
