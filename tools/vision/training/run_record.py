#!/usr/bin/env python3
"""Write or finish the provenance record of a training run (run.json).

  run_record.py start RUN_DIR --exp EXP --data DATA --cmd "..."
      records: DOORS tree commit, YOLOX source commit and dirty state, Python
      and every installed package, GPU/driver, the exp file's sha256 and its
      resolved settings, the dataset manifest's sha256, seed, command.
  run_record.py finish RUN_DIR
      adds the sha256 and size of every *.pth under RUN_DIR and the end time.

The record is what MODEL_LICENSES / NOTICE will cite; keep it with the
checkpoint.
"""
import argparse
import datetime
import hashlib
import json
import os
import platform
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PINS = HERE / "PINS.env"


def sh(cmd, cwd=None):
    try:
        return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=60).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return None


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def pins():
    out = {}
    for line in PINS.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            out[k] = v.strip('"')
    return out


def exp_settings(exp_file):
    from yolox.exp import get_exp

    exp = get_exp(exp_file, None)
    keep = ("class_set", "class_names", "num_classes", "depth", "width", "act", "input_size", "test_size",
            "random_size", "multiscale_range", "mosaic_prob", "mosaic_scale", "enable_mixup", "mixup_prob",
            "mixup_scale", "hsv_prob", "flip_prob", "degrees", "translate", "shear", "max_epoch",
            "warmup_epochs", "warmup_lr", "no_aug_epochs", "basic_lr_per_img", "scheduler", "min_lr_ratio",
            "weight_decay", "momentum", "ema", "seed", "test_conf", "nmsthre", "data_dir", "train_ann",
            "val_ann", "exp_name", "output_dir", "data_num_workers")
    return {k: getattr(exp, k, None) for k in keep}


def start(a):
    run = Path(a.run_dir)
    run.mkdir(parents=True, exist_ok=True)
    p = pins()
    yolox_dir = os.environ.get("YOLOX_DIR", p.get("YOLOX_DIR", ""))
    import torch

    rec = {
        "started": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "command": a.cmd,
        "pins": p,
        "doors_tree": {"commit": sh(["git", "rev-parse", "HEAD"], HERE),
                       "dirty": bool(sh(["git", "status", "--porcelain", "--", str(HERE)], HERE))},
        "yolox": {"dir": yolox_dir,
                  "commit": sh(["git", "rev-parse", "HEAD"], yolox_dir) if yolox_dir else None,
                  "dirty": bool(sh(["git", "status", "--porcelain"], yolox_dir)) if yolox_dir else None},
        "python": sys.version,
        "platform": platform.platform(),
        "packages": sh([sys.executable, "-m", "pip", "freeze"]).splitlines(),
        "torch": {"version": torch.__version__, "cuda": torch.version.cuda,
                  "cudnn": torch.backends.cudnn.version() if torch.backends.cudnn.is_available() else None,
                  "devices": [torch.cuda.get_device_name(i) for i in range(torch.cuda.device_count())]},
        "nvidia_smi": sh(["nvidia-smi", "--query-gpu=name,driver_version,memory.total", "--format=csv,noheader"]),
        "exp": {"file": a.exp, "sha256": sha256(a.exp), "settings": exp_settings(a.exp)},
        "dataset": {"dir": a.data, "manifest_sha256": sha256(Path(a.data) / "manifest.json"),
                    "manifest": json.loads((Path(a.data) / "manifest.json").read_text())},
        "pretrained_weights": None,
    }
    rec["dataset"]["manifest"].pop("inputs", None)
    if rec["yolox"]["commit"] != p.get("YOLOX_COMMIT"):
        raise SystemExit("YOLOX at %s is %s, PINS.env wants %s"
                         % (yolox_dir, rec["yolox"]["commit"], p.get("YOLOX_COMMIT")))
    if rec["yolox"]["dirty"]:
        raise SystemExit("the YOLOX tree has local changes; train from the pinned commit only")
    (run / "run.json").write_text(json.dumps(rec, indent=1, default=str))
    print("recorded", run / "run.json")


def finish(a):
    run = Path(a.run_dir)
    rec = json.loads((run / "run.json").read_text())
    rec["finished"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    rec["checkpoints"] = {str(p.relative_to(run)): {"sha256": sha256(p), "bytes": p.stat().st_size}
                          for p in sorted(run.rglob("*.pth"))}
    (run / "run.json").write_text(json.dumps(rec, indent=1, default=str))
    print(json.dumps(rec["checkpoints"], indent=1))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="what", required=True)
    s = sub.add_parser("start")
    s.add_argument("run_dir")
    s.add_argument("--exp", required=True)
    s.add_argument("--data", required=True)
    s.add_argument("--cmd", required=True)
    f = sub.add_parser("finish")
    f.add_argument("run_dir")
    a = ap.parse_args()
    start(a) if a.what == "start" else finish(a)


if __name__ == "__main__":
    main()
