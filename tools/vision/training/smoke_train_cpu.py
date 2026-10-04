#!/usr/bin/env python3
"""CPU smoke training: proves the pipeline end to end on a machine with no
GPU. It is NOT a way to train the real model.

YOLOX's Trainer is CUDA-only (yolox/core/trainer.py sets cuda devices), so
this runs the same pieces by hand: the exp's model (fresh init, no
pretrained weights), its mosaic data loader, its optimizer and warm-cosine
schedule, YOLOX's loss and EMA. It writes a checkpoint in the Trainer's own
format ({"model": EMA state_dict, ...}) so export_onnx.py treats it like a
real one.

Without --fixed-lr a short run sits inside YOLOX's 5-epoch warmup at a
near-zero lr; with it, a few hundred iterations show whether the loss falls
at all (labels and loss wired right).

Usage: smoke_train_cpu.py --exp EXP.py --out RUN_DIR [--iters 60] [--batch 4] [--fixed-lr 0.002]
"""
import argparse
import json
import random
import time
from pathlib import Path

import numpy as np
import torch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--iters", type=int, default=60)
    ap.add_argument("--batch", type=int, default=4)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--fixed-lr", type=float, default=0.0,
                    help="skip warmup/cosine and use this lr: a learning-signal check on few images")
    args = ap.parse_args()

    from yolox.exp import get_exp
    from yolox.utils import ModelEMA

    exp = get_exp(args.exp, None)
    exp.data_num_workers = 0
    random.seed(exp.seed)
    np.random.seed(exp.seed)
    torch.manual_seed(exp.seed)
    torch.set_num_threads(args.threads)

    model = exp.get_model()
    model.train()
    loader = exp.get_data_loader(batch_size=args.batch, is_distributed=False, no_aug=False)
    iters_per_epoch = max(1, len(loader))
    # The schedule is set as if the smoke run were exp.max_epoch long; it
    # only has to run, not converge.
    optimizer = exp.get_optimizer(args.batch)
    lr_sched = exp.get_lr_scheduler(exp.basic_lr_per_img * args.batch, iters_per_epoch)
    ema = ModelEMA(model, 0.9998)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    log = []
    it = iter(loader)
    t0 = time.time()
    for i in range(args.iters):
        inps, targets, _, _ = next(it)
        inps = inps.float()
        targets = targets.float()
        targets.requires_grad = False
        inps, targets = exp.preprocess(inps, targets, exp.input_size)
        outputs = model(inps, targets)
        loss = outputs["total_loss"]
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()
        ema.update(model)
        lr = args.fixed_lr or lr_sched.update_lr(i + 1)
        for g in optimizer.param_groups:
            g["lr"] = lr
        rec = {k: float(v) for k, v in outputs.items() if torch.is_tensor(v)}
        rec.update(iter=i + 1, lr=lr, s=round(time.time() - t0, 1))
        log.append(rec)
        if (i + 1) % 10 == 0 or i == 0:
            print(json.dumps(rec), flush=True)

    ck = {"start_epoch": 1, "model": ema.ema.state_dict(), "optimizer": None, "best_ap": 0.0,
          "curr_ap": None, "smoke": {"iters": args.iters, "batch": args.batch, "device": "cpu"}}
    torch.save(ck, out / "smoke_ckpt.pth")
    (out / "smoke_log.json").write_text(json.dumps(log, indent=0))
    first, last = log[0]["total_loss"], log[-1]["total_loss"]
    print("smoke done: %d iters in %.0f s, total_loss %.2f -> %.2f"
          % (args.iters, time.time() - t0, first, last))
    if not all(np.isfinite(r["total_loss"]) for r in log):
        raise SystemExit("non-finite loss")


if __name__ == "__main__":
    main()
