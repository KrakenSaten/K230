#!/usr/bin/env python3
"""Smoke training on cpu, xpu (Intel GPU) or cuda: proves the pipeline end
to end on a small dataset. It is NOT a way to train the real model.

YOLOX's Trainer is CUDA-only (yolox/core/trainer.py and launch.py set cuda
devices, a CUDA GradScaler/autocast and NCCL; the data prefetcher uses CUDA
streams; exp.random_resize allocates a CUDA tensor), and run_record.py
refuses a modified YOLOX tree, so YOLOX is not patched. This runs the same
pieces by hand: the exp's model (fresh init, no pretrained weights), its
mosaic data loader, its optimizer and warm-cosine schedule, YOLOX's loss and
EMA, and with --multiscale the Trainer's every-10-iterations input resize.
The model and loss code are device-neutral (VERIFIED on xpu 2026-10-06).
It writes a checkpoint in the Trainer's own format ({"model": EMA
state_dict, ...}, CPU tensors) so export_onnx.py treats it like a real one.

Without --fixed-lr a short run sits inside YOLOX's 5-epoch warmup at a
near-zero lr; with it, a few hundred iterations show whether the loss falls
at all (labels and loss wired right).

Usage: smoke_train.py --exp EXP.py --out RUN_DIR [--device auto|cpu|xpu|cuda]
           [--iters 60] [--batch 4] [--fixed-lr 0.002] [--amp off|bf16|fp16]
           [--multiscale] [--workers 0] [--parity]
"""
import argparse
import contextlib
import copy
import json
import random
import time
import warnings
from pathlib import Path

import numpy as np
import torch


def resolve_device(name):
    if name == "auto":
        if hasattr(torch, "xpu") and torch.xpu.is_available():
            name = "xpu"
        elif torch.cuda.is_available():
            name = "cuda"
        else:
            name = "cpu"
    dev = torch.device(name)
    if dev.type != "cpu" and not accel(dev).is_available():
        raise SystemExit("device %s is not available" % name)
    return dev


def accel(dev):
    """torch.xpu or torch.cuda for an accelerator device, None for the CPU."""
    return getattr(torch, dev.type) if dev.type in ("xpu", "cuda") else None


def sync(dev):
    if accel(dev):
        accel(dev).synchronize()


def mib(n):
    return round(n / 2**20, 1)


def memory(dev):
    a = accel(dev)
    if not a:
        return {}
    free, total = a.mem_get_info()
    return {"allocated_mib": mib(a.memory_allocated()), "reserved_mib": mib(a.memory_reserved()),
            "peak_allocated_mib": mib(a.max_memory_allocated()),
            "peak_reserved_mib": mib(a.max_memory_reserved()),
            "device_used_mib": mib(total - free), "device_total_mib": mib(total)}


def guard_autocast_off(dev):
    """YOLOX keeps its label-assignment cost in fp32 with
    `torch.cuda.amp.autocast(enabled=False)` (yolo_head.py), which only
    turns off CUDA autocast; under xpu autocast binary_cross_entropy then
    refuses to run. Make that exact call turn autocast off on dev as well,
    so mixed precision keeps upstream's fp32 region without patching YOLOX.
    Only installed for mixed precision on a non-CUDA device."""
    cuda_autocast = torch.cuda.amp.autocast

    def autocast(enabled=True, *a, **k):
        return cuda_autocast(enabled, *a, **k) if enabled else torch.autocast(dev.type, enabled=False)

    torch.cuda.amp.autocast = autocast


def parity(model, inps, targets, dev):
    """One fp32 training forward/backward of copies of the model on dev and
    on the CPU, from the same weights and batch: the device must compute
    what the CPU computes (a wrong kernel trains without crashing)."""
    res = {}
    for d in (dev, torch.device("cpu")):
        m = copy.deepcopy(model).to(d).train()
        o = m(inps.to(d), targets.to(d))
        o["total_loss"].backward()
        res[d.type] = ({k: v.item() for k, v in o.items() if torch.is_tensor(v)},
                       torch.cat([p.grad.detach().cpu().double().flatten()
                                  for p in m.parameters() if p.grad is not None]))
    (ld, gd), (lc, gc) = res[dev.type], res["cpu"]
    rec = {"loss_" + dev.type: ld, "loss_cpu": lc,
           "total_loss_rel_diff": abs(ld["total_loss"] - lc["total_loss"]) / abs(lc["total_loss"]),
           "grad_cosine": float(torch.nn.functional.cosine_similarity(gd, gc, dim=0)),
           "grad_rel_norm_diff": float((gd - gc).norm() / gc.norm())}
    rec["ok"] = rec["total_loss_rel_diff"] < 1e-3 and rec["grad_cosine"] > 0.999
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--device", default="auto", help="auto (xpu, then cuda, then cpu) | cpu | xpu | cuda")
    ap.add_argument("--iters", type=int, default=60)
    ap.add_argument("--batch", type=int, default=4)
    ap.add_argument("--threads", type=int, default=4, help="CPU threads (cpu device only)")
    ap.add_argument("--workers", type=int, default=0, help="data loader workers")
    ap.add_argument("--amp", default="off", choices=("off", "bf16", "fp16"),
                    help="autocast dtype; fp16 adds a GradScaler")
    ap.add_argument("--multiscale", action="store_true",
                    help="resize the input every 10 iterations within exp.random_size, as the Trainer does")
    ap.add_argument("--parity", action="store_true",
                    help="first compare one training step on the device against the CPU (stops on a mismatch)")
    ap.add_argument("--fixed-lr", type=float, default=0.0,
                    help="skip warmup/cosine and use this lr: a learning-signal check on few images")
    args = ap.parse_args()

    from yolox.exp import get_exp
    from yolox.utils import ModelEMA

    dev = resolve_device(args.device)
    exp = get_exp(args.exp, None)
    exp.data_num_workers = args.workers
    random.seed(exp.seed)
    np.random.seed(exp.seed)
    torch.manual_seed(exp.seed)
    if dev.type == "cpu":
        torch.set_num_threads(args.threads)

    model = exp.get_model().to(dev)
    model.train()
    loader = exp.get_data_loader(batch_size=args.batch, is_distributed=False, no_aug=False)
    iters_per_epoch = max(1, len(loader))
    # The schedule is set as if the smoke run were exp.max_epoch long; it
    # only has to run, not converge.
    optimizer = exp.get_optimizer(args.batch)
    lr_sched = exp.get_lr_scheduler(exp.basic_lr_per_img * args.batch, iters_per_epoch)
    ema = ModelEMA(model, 0.9998)
    amp_dtype = {"bf16": torch.bfloat16, "fp16": torch.float16}.get(args.amp)
    scaler = torch.amp.GradScaler(dev.type, enabled=args.amp == "fp16")
    autocast = (lambda: torch.autocast(dev.type, dtype=amp_dtype)) if amp_dtype else contextlib.nullcontext
    if amp_dtype and dev.type != "cuda":
        guard_autocast_off(dev)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    log = []
    it = iter(loader)
    tsize = exp.input_size
    par = None
    if args.parity and dev.type != "cpu":
        inps, targets, _, _ = next(it)
        inps, targets = exp.preprocess(inps.float(), targets.float(), tsize)
        par = parity(model, inps, targets, dev)
        (out / "parity.json").write_text(json.dumps(par, indent=1))
        print(json.dumps({"parity": par}), flush=True)
        if not par["ok"]:
            raise SystemExit("%s does not match the CPU on a training step" % dev)
    seen_warnings = []
    t0 = time.time()
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        for i in range(args.iters):
            ti = time.time()
            inps, targets, _, _ = next(it)
            inps = inps.to(dev, non_blocking=True).float()
            targets = targets.to(dev, non_blocking=True).float()
            targets.requires_grad = False
            inps, targets = exp.preprocess(inps, targets, tsize)
            with autocast():
                outputs = model(inps, targets)
            loss = outputs["total_loss"]
            # A silent CPU path would show here (YOLOX's only one is the
            # OOM fallback in label assignment, which needs CUDA).
            if loss.device.type != dev.type:
                raise SystemExit("loss on %s, expected %s" % (loss.device, dev))
            optimizer.zero_grad()
            scaler.scale(loss).backward()
            scaler.step(optimizer)
            scaler.update()
            ema.update(model)
            lr = args.fixed_lr or lr_sched.update_lr(i + 1)
            for g in optimizer.param_groups:
                g["lr"] = lr
            rec = {k: v.item() for k, v in outputs.items() if torch.is_tensor(v)}
            sync(dev)
            rec.update(iter=i + 1, lr=lr, size=tsize[0], it_s=round(time.time() - ti, 4),
                       s=round(time.time() - t0, 1))
            log.append(rec)
            if args.multiscale and (i + 1) % 10 == 0:
                tsize = (32 * random.randint(*exp.random_size),) * 2
            if (i + 1) % 10 == 0 or i == 0:
                print(json.dumps({**rec, **memory(dev)}), flush=True)
            for w in caught:
                msg = "%s: %s" % (w.category.__name__, w.message)
                if msg not in seen_warnings:
                    seen_warnings.append(msg)
                    print("warning:", msg, flush=True)
            caught.clear()
    total_s = time.time() - t0

    state = {k: v.detach().cpu() for k, v in ema.ema.state_dict().items()}
    ck = {"start_epoch": 1, "model": state, "optimizer": None, "best_ap": 0.0, "curr_ap": None,
          "smoke": {"iters": args.iters, "batch": args.batch, "device": dev.type, "amp": args.amp,
                    "multiscale": args.multiscale, "fixed_lr": args.fixed_lr}}
    torch.save(ck, out / "smoke_ckpt.pth")
    (out / "smoke_log.json").write_text(json.dumps(log, indent=0))

    steady = [r["it_s"] for r in log[10:]] or [r["it_s"] for r in log]
    first, last = log[0]["total_loss"], log[-1]["total_loss"]
    summary = {
        "device": dev.type,
        "device_name": accel(dev).get_device_name(dev) if accel(dev) else "cpu",
        "torch": torch.__version__,
        "iters": args.iters, "batch": args.batch, "amp": args.amp, "multiscale": args.multiscale,
        "workers": args.workers, "fixed_lr": args.fixed_lr,
        "sizes_seen": sorted({r["size"] for r in log}),
        "total_s": round(total_s, 1),
        "first_iter_s": log[0]["it_s"],
        "steady_iter_s_median": round(float(np.median(steady)), 4),
        "steady_images_per_s": round(args.batch / float(np.median(steady)), 1),
        "overall_images_per_s": round(args.iters * args.batch / total_s, 1),
        "total_loss_first": first, "total_loss_last": last,
        "total_loss_first10_mean": float(np.mean([r["total_loss"] for r in log[:10]])),
        "total_loss_last10_mean": float(np.mean([r["total_loss"] for r in log[-10:]])),
        "memory": memory(dev),
        "parity": par,
        "warnings": seen_warnings,
    }
    (out / "smoke_summary.json").write_text(json.dumps(summary, indent=1))
    print(json.dumps(summary, indent=1))
    print("smoke done: %d iters in %.0f s, total_loss %.2f -> %.2f" % (args.iters, total_s, first, last))
    if not all(np.isfinite(r["total_loss"]) for r in log):
        raise SystemExit("non-finite loss")


if __name__ == "__main__":
    main()
