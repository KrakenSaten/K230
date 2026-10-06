#!/usr/bin/env python3
"""Epoch training of the DOORS YOLOX exp on xpu (Intel GPU), cuda or cpu.

YOLOX's Trainer is CUDA-only (see smoke_train.py) and YOLOX stays unpatched,
so this is the Trainer's recipe rebuilt from the exp's own pieces: model
(fresh init, no pretrained weights), mosaic loader, SGD optimizer,
yoloxwarmcos schedule, loss and ModelEMA. What it keeps from upstream
(yolox/core/trainer.py at the PINS.env commit):
  - lr is set after every step from progress+1; the first step runs at
    warmup_lr (0), like upstream.
  - mosaic closes and the L1 loss starts at the epoch where
    epoch+1 == max_epoch - no_aug_epochs (0-based epoch), and eval_interval
    becomes 1 from there. Upstream's rule makes it no_aug_epochs+1 epochs
    without mosaic (16 of 300); kept as is.
  - multiscale: a new input size within exp.random_size every 10 iterations.
  - validation on the EMA weights; best by COCO mAP50:95.
What differs, and why:
  - resume continues exactly: the raw model (upstream resumes from the EMA
    weights), EMA and its update count, optimizer, GradScaler, RNG states,
    sampler position and the current multiscale size are in the checkpoint.
  - latest_ckpt.pth is written after the epoch's validation, so a resumed
    run knows the best AP so far (upstream saves it before, and a resume
    could overwrite best_ckpt.pth with a worse model).
  - the first validation always writes best_ckpt.pth (upstream starts at
    best_ap 0 with a strict >, so a model at AP 0 never gets one).
  - last_mosaic_epoch_ckpt.pth holds the epochs actually completed
    (upstream stores epoch+1 before training that epoch).
  - the loader iterator is rebuilt at the mosaic switch and on resume, from
    the next unseen sample, so no batch queued with mosaic leaks into the
    no-mosaic phase.
  - validation is our own device-neutral loop (upstream's evaluator is
    CUDA-only) on YOLOX's postprocess and COCO conversion and pycocotools.
  - final_ckpt.pth after the last epoch; no last_epoch_ckpt.pth.

Checkpoints keep upstream's format where export_onnx.py reads it: "model" is
the EMA state_dict, all tensors on the CPU.

Usage:
  train_loop.py --exp EXP.py --out RUN_DIR [--device auto|cpu|xpu|cuda]
      [--batch 64] [--workers 4] [--amp off|bf16|fp16] [--no-multiscale]
      [--max-epoch N --warmup-epochs N --no-aug-epochs N --eval-interval N]
      [--resume latest|CKPT] [--stop-after-epoch N]
  train_loop.py --exp EXP.py --eval-only CKPT [--device ...]
The schedule options override the exp for short smoke runs only; a real run
leaves them out and uses the exp's (300 / 5 / 15 / 10).
"""
import argparse
import contextlib
import datetime
import hashlib
import io
import itertools
import json
import random
import sys
import time
import warnings
from pathlib import Path

import numpy as np
import torch

import run_record
from smoke_train import accel, guard_autocast_off, memory, resolve_device, sync

CKPT_FORMAT = "doors-yolox-train-v1"
EMA_DECAY = 0.9998  # upstream Trainer
BEST_METRIC = "map50_95"
BEST_RULE = ("val mAP50:95 (COCO AP@[.50:.95], all areas, maxDets 100) of the EMA weights on the validation "
             "split; the first validation always becomes best, later ones only when strictly greater")
SCHEDULE_KEYS = ("max_epoch", "warmup_epochs", "no_aug_epochs", "eval_interval")
# Settings a resume must share with the run it continues.
RESUME_KEYS = ("batch", "iters_per_epoch", "max_epoch", "warmup_epochs", "no_aug_epochs", "basic_lr_per_img",
               "warmup_lr", "min_lr_ratio", "scheduler", "amp", "multiscale", "seed", "class_set", "input_size")


# ---- schedule rules (pure; tests/test_train_loop.py)

def no_aug_epoch(epoch, max_epoch, no_aug_epochs):
    """True if 0-based epoch trains without mosaic and with the L1 loss.
    Upstream closes mosaic in before_epoch when epoch+1 == max_epoch -
    no_aug_epochs and starts a resumed run without it when start_epoch >=
    max_epoch - no_aug_epochs; both say this."""
    return epoch + 1 >= max_epoch - no_aug_epochs


def is_better(value, best):
    """Best-checkpoint rule: the first validation, then strictly greater."""
    return best is None or value > best


def is_mosaic(img_info, input_size):
    """MosaicDetection reports a mosaic sample's img_info as (H, 3), the CHW
    shape of the preprocessed image; a plain sample reports its original
    (height, width). Exact as long as no training image is 3 pixels wide
    (checked at start)."""
    h, w = img_info
    return (h == input_size[0]) & (w == 3)


class SkipSampler(torch.utils.data.Sampler):
    """The loader's infinite sampler, from sample `skip` on: a rebuilt
    iterator continues the index stream where training left it."""

    def __init__(self, inner, skip):
        self.inner, self.skip = inner, skip

    def __iter__(self):
        return itertools.islice(iter(self.inner), self.skip, None)

    def __len__(self):
        return len(self.inner)


def data_iter(loader, sampler, skip):
    loader.batch_sampler.sampler = SkipSampler(sampler, skip)
    return iter(loader)


def random_size(exp):
    """exp.random_resize without its CUDA tensor (single process)."""
    factor = exp.input_size[1] / exp.input_size[0]
    s = random.randint(*exp.random_size)
    return int(32 * s), 32 * int(s * factor)


# ---- state

def to_cpu(obj):
    if torch.is_tensor(obj):
        return obj.detach().cpu()
    if isinstance(obj, dict):
        return {k: to_cpu(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return type(obj)(to_cpu(v) for v in obj)
    return obj


def rng_state(dev):
    st, keys, pos, gauss, cached = np.random.get_state()
    return {"python": random.getstate(), "numpy": [st, keys.tolist(), pos, gauss, cached],
            "torch": torch.get_rng_state(), "device": accel(dev).get_rng_state() if accel(dev) else None}


def set_rng_state(s, dev):
    random.setstate(s["python"])
    st, keys, pos, gauss, cached = s["numpy"]
    np.random.set_state((st, np.array(keys, dtype=np.uint32), pos, gauss, cached))
    torch.set_rng_state(s["torch"])
    if accel(dev) and s["device"] is not None:
        accel(dev).set_rng_state(s["device"])


def state_hash(model):
    h = hashlib.sha256()
    for k, v in model.state_dict().items():
        h.update(k.encode())
        h.update(v.detach().cpu().contiguous().numpy().tobytes())
    return h.hexdigest()


def sha256(p):
    return run_record.sha256(p)


# ---- validation

def pr_at(coco_eval, conf, iou_index=0):
    """Precision and recall at one score threshold and IoU (default 0.50),
    from pycocotools' own matching (all areas, maxDets 100): total and per
    category id."""
    p = coco_eval.params
    n_area, n_img = len(p.areaRng), len(p.imgIds)
    per = {}
    for k, cat in enumerate(p.catIds):
        tp = fp = npos = 0
        for i in range(n_img):
            e = coco_eval.evalImgs[(k * n_area) * n_img + i]  # area index 0 = all
            if e is None:
                continue
            npos += int((~np.asarray(e["gtIgnore"], dtype=bool)).sum())
            if not len(e["dtScores"]):
                continue
            keep = (np.asarray(e["dtScores"]) >= conf) & ~np.asarray(e["dtIgnore"][iou_index], dtype=bool)
            matched = np.asarray(e["dtMatches"][iou_index])[keep] > 0
            tp += int(matched.sum())
            fp += int((~matched).sum())
        per[cat] = (tp, fp, npos)
    tp, fp, npos = (sum(v[j] for v in per.values()) for j in range(3))
    ratio = lambda a, b: a / b if b else 0.0  # noqa: E731
    return ({"precision": ratio(tp, tp + fp), "recall": ratio(tp, npos), "tp": tp, "fp": fp, "gt": npos},
            {c: {"precision": ratio(a, a + b), "recall": ratio(a, n), "tp": a, "fp": b, "gt": n}
             for c, (a, b, n) in per.items()})


def coco_metrics(coco_gt, dets, pr_conf):
    """mAP50:95, mAP50, precision/recall at pr_conf and per-class AP."""
    from pycocotools.cocoeval import COCOeval

    cats = {c["id"]: c["name"] for c in coco_gt.loadCats(coco_gt.getCatIds())}
    zero = {"map50_95": 0.0, "map50": 0.0, "precision": 0.0, "recall": 0.0, "detections": 0,
            "per_class": {n: {"ap50_95": 0.0, "ap50": 0.0} for n in cats.values()}}
    if not dets:  # pycocotools cannot load an empty result list
        return zero
    with contextlib.redirect_stdout(io.StringIO()):
        ev = COCOeval(coco_gt, coco_gt.loadRes(dets), "bbox")
        ev.evaluate()
        ev.accumulate()
        ev.summarize()
    total, per = pr_at(ev, pr_conf)
    prec = ev.eval["precision"]  # [T, R, K, A, M]
    per_class = {}
    for k, cat in enumerate(ev.params.catIds):
        def ap(x):
            x = x[x > -1]
            return float(x.mean()) if x.size else float("nan")
        per_class[cats[cat]] = {"ap50_95": ap(prec[:, :, k, 0, -1]), "ap50": ap(prec[0, :, k, 0, -1]),
                                "precision": per[cat]["precision"], "recall": per[cat]["recall"],
                                "gt": per[cat]["gt"]}
    return {"map50_95": float(ev.stats[0]), "map50": float(ev.stats[1]), "precision": total["precision"],
            "recall": total["recall"], "tp": total["tp"], "fp": total["fp"], "gt": total["gt"],
            "detections": len(dets), "per_class": per_class}


def validate(model, exp, loader, dev, pr_conf):
    """COCO metrics of model on the exp's validation split, on dev, fp32.
    Leaves the weights and the train/eval mode as they were (checked)."""
    from yolox.evaluators import COCOEvaluator
    from yolox.utils import postprocess

    # Only for its COCO-format conversion, which is device-neutral.
    conv = COCOEvaluator(loader, exp.test_size, exp.test_conf, exp.nmsthre, exp.num_classes)
    before, was_training = state_hash(model), model.training
    model.eval()
    dets, t0 = [], time.time()
    with torch.no_grad():
        for imgs, _, info_imgs, ids in loader:
            out = model(imgs.to(dev).float())
            out = postprocess(out, exp.num_classes, exp.test_conf, exp.nmsthre)
            dets.extend(conv.convert_to_coco_format(out, info_imgs, ids))
    sync(dev)
    seconds = time.time() - t0
    model.train(was_training)
    if state_hash(model) != before:
        raise SystemExit("validation changed the model weights")
    m = coco_metrics(loader.dataset.coco, dets, pr_conf)
    m.update(split=exp.val_ann, images=len(loader.dataset), pr_conf=pr_conf, pr_iou=0.5, seconds=round(seconds, 1),
             weights_unchanged=True)
    return m


# ---- training

def config_of(args, exp, ipe):
    return {"batch": args.batch, "iters_per_epoch": ipe, "max_epoch": exp.max_epoch,
            "warmup_epochs": exp.warmup_epochs, "no_aug_epochs": exp.no_aug_epochs,
            "eval_interval": exp.eval_interval, "basic_lr_per_img": exp.basic_lr_per_img,
            "warmup_lr": exp.warmup_lr, "min_lr_ratio": exp.min_lr_ratio, "scheduler": exp.scheduler,
            "amp": args.amp, "multiscale": args.multiscale, "seed": exp.seed, "class_set": exp.class_set,
            "input_size": list(exp.input_size)}


def schedule_of(exp, ipe, batch):
    """The schedule this run follows, in 1-based epochs and 1-based steps."""
    lr = exp.basic_lr_per_img * batch
    total = ipe * exp.max_epoch
    switch = exp.max_epoch - exp.no_aug_epochs  # 1-based epoch where mosaic closes
    return {"max_epoch": exp.max_epoch, "iters_per_epoch": ipe, "total_iters": total,
            "warmup_epochs": exp.warmup_epochs, "warmup_iters": ipe * exp.warmup_epochs,
            "no_aug_epochs": exp.no_aug_epochs,
            "mosaic_epochs": "1..%d" % (switch - 1), "no_mosaic_l1_epochs": "%d..%d" % (switch, exp.max_epoch),
            "min_lr_from_iter": total - ipe * exp.no_aug_epochs,
            "lr": lr, "warmup_lr": exp.warmup_lr, "min_lr": lr * exp.min_lr_ratio,
            "scheduler": exp.scheduler, "eval_interval": exp.eval_interval, "eval_interval_no_aug": 1}


def save(path, state):
    tmp = path.with_suffix(".tmp")
    torch.save(state, tmp)
    tmp.replace(path)


def write_json(path, obj):
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(obj, indent=1, default=str))
    tmp.replace(path)


def append_jsonl(path, rec):
    with open(path, "a") as f:
        f.write(json.dumps(rec, default=str) + "\n")


def optimizer_desc(optimizer, exp):
    g = optimizer.param_groups
    return {"type": type(optimizer).__name__, "nesterov": g[0].get("nesterov"), "momentum": exp.momentum,
            "weight_decay": exp.weight_decay,
            "groups": [{"params": len(x["params"]), "weight_decay": x.get("weight_decay", 0)} for x in g],
            "groups_meaning": ["BatchNorm weights, no decay", "conv/linear weights, decay", "biases, no decay"]}


def train(args):
    from yolox.exp import get_exp
    from yolox.utils import ModelEMA

    dev = resolve_device(args.device)
    exp = get_exp(args.exp, None)
    overrides = {k: getattr(args, k) for k in SCHEDULE_KEYS if getattr(args, k) is not None}
    for k, v in overrides.items():
        setattr(exp, k, v)
    exp.data_num_workers = args.workers
    random.seed(exp.seed)
    np.random.seed(exp.seed)
    torch.manual_seed(exp.seed)
    if dev.type == "cpu":
        torch.set_num_threads(args.threads)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    ck, resumed_from = None, None
    if args.resume:
        path = out / "latest_ckpt.pth" if args.resume == "latest" else Path(args.resume)
        ck = torch.load(path, map_location="cpu", weights_only=False)
        if ck.get("format") != CKPT_FORMAT:
            raise SystemExit("%s is not a %s checkpoint" % (path, CKPT_FORMAT))
        resumed_from = {"file": path.name, "sha256": sha256(path), "start_epoch": ck["start_epoch"]}
        exp.eval_interval = ck["config"]["eval_interval"]  # the no-aug switch below sets 1 again
    elif (out / "run.json").exists():
        raise SystemExit("%s already holds a run; use --resume latest or a new --out" % out)

    model = exp.get_model().to(dev)  # fresh init; replaced below on resume
    optimizer = exp.get_optimizer(args.batch)
    start_epoch = 0
    if ck:
        model.load_state_dict(ck["train_model"])
        optimizer.load_state_dict(ck["optimizer"])
        start_epoch = ck["start_epoch"]
    # Mosaic already closed by the run this one continues?
    closed = start_epoch > 0 and no_aug_epoch(start_epoch - 1, exp.max_epoch, exp.no_aug_epochs)
    loader = exp.get_data_loader(batch_size=args.batch, is_distributed=False, no_aug=closed)
    sampler = loader.batch_sampler.sampler
    narrow = [i for i in loader.dataset._dataset.coco.dataset["images"] if i["width"] == 3]
    if narrow:
        raise SystemExit("training images 3 px wide break the mosaic counter: %s" % narrow[:3])
    ipe = args.iters_per_epoch or len(loader)
    lr_sched = exp.get_lr_scheduler(exp.basic_lr_per_img * args.batch, ipe)
    ema = ModelEMA(model, EMA_DECAY) if exp.ema else None
    amp_dtype = {"bf16": torch.bfloat16, "fp16": torch.float16}.get(args.amp)
    scaler = torch.amp.GradScaler(dev.type, enabled=args.amp == "fp16")
    autocast = (lambda: torch.autocast(dev.type, dtype=amp_dtype)) if amp_dtype else contextlib.nullcontext
    if amp_dtype and dev.type != "cuda":
        guard_autocast_off(dev)
    val_loader = exp.get_eval_loader(batch_size=args.batch, is_distributed=False)

    config = config_of(args, exp, ipe)
    best, best_epoch, samples_seen, tsize = None, None, 0, tuple(exp.input_size)
    if ck:
        bad = {k: (ck["config"].get(k), config[k]) for k in RESUME_KEYS if ck["config"].get(k) != config[k]}
        if bad:
            raise SystemExit("the resume differs from the run it continues (checkpoint, now): %s" % bad)
        if ema:
            ema.ema.load_state_dict(ck["model"])
            ema.updates = ck["ema_updates"]
        if scaler.is_enabled():
            scaler.load_state_dict(ck["scaler"])
        best, best_epoch, samples_seen = ck["best_ap"], ck["best_epoch"], ck["samples_seen"]
        tsize = tuple(ck["input_size"])
    if closed:
        model.head.use_l1 = True
        exp.eval_interval = 1
    it = data_iter(loader, sampler, samples_seen)
    if ck:
        set_rng_state(ck["rng"], dev)  # after the iterator, which draws its base seed

    runj = out / "run.json"
    session = {"started": datetime.datetime.now(datetime.timezone.utc).isoformat(),
               "command": " ".join([Path(sys.executable).name] + sys.argv), "device": dev.type,
               "device_name": accel(dev).get_device_name(dev) if accel(dev) else "cpu",
               "workers": args.workers, "start_epoch": start_epoch + 1, "resumed_from": resumed_from}
    if ck:
        rec = json.loads(runj.read_text())
    else:
        rec = run_record.record(args.exp, exp.data_dir, session["command"])
        man = rec["dataset"]["manifest"]
        rec.update({
            "format": "doors-yolox-run-v1", "seed": exp.seed, "image_size": list(exp.input_size),
            "multiscale": {"enabled": args.multiscale, "every_iters": 10,
                           "range_px": [32 * exp.random_size[0], 32 * exp.random_size[1]]},
            "batch": args.batch, "epochs": exp.max_epoch, "amp": args.amp, "device": dev.type,
            "optimizer": optimizer_desc(optimizer, exp),
            "lr": {"basic_lr_per_img": exp.basic_lr_per_img, "lr": exp.basic_lr_per_img * args.batch,
                   "warmup_lr": exp.warmup_lr, "min_lr_ratio": exp.min_lr_ratio, "scheduler": exp.scheduler},
            "ema": {"enabled": bool(exp.ema), "decay": EMA_DECAY},
            "class_mapping": {"class_set": man.get("class_set"), "classes": man.get("classes"),
                              "doors_index": man.get("doors_index"),
                              "val_categories": {c["id"]: c["name"] for c in
                                                 val_loader.dataset.coco.dataset["categories"]}},
            "schedule": schedule_of(exp, ipe, args.batch), "schedule_overrides": overrides,
            "iters_per_epoch_override": args.iters_per_epoch, "config": config,
            "best_checkpoint": {"file": "best_ckpt.pth", "metric": BEST_METRIC, "rule": BEST_RULE,
                                "value": None, "epoch": None},
            "validation": [], "sessions": []})
    rec["sessions"].append(session)
    write_json(runj, rec)

    def state(completed, metrics):
        return to_cpu({
            "format": CKPT_FORMAT, "start_epoch": completed,
            "model": (ema.ema if ema else model).state_dict(), "train_model": model.state_dict(),
            "optimizer": optimizer.state_dict(), "ema_updates": ema.updates if ema else None,
            "scaler": scaler.state_dict() if scaler.is_enabled() else None,
            "best_ap": best, "best_epoch": best_epoch, "curr_ap": metrics and metrics[BEST_METRIC],
            "metrics": metrics, "samples_seen": samples_seen, "input_size": list(tsize),
            "use_l1": bool(model.head.use_l1), "rng": rng_state(dev), "config": config,
            "doors": {"class_set": exp.class_set, "classes": exp.class_names}})

    print(json.dumps({"start_epoch": start_epoch + 1, "max_epoch": exp.max_epoch, "iters_per_epoch": ipe,
                      "resumed_from": resumed_from, "device": session["device_name"]}), flush=True)
    seen_warnings, epochs_done, t_start = [], [], time.time()
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        for epoch in range(start_epoch, exp.max_epoch):
            # ---- before_epoch: upstream's switch (see no_aug_epoch)
            switched = False
            if no_aug_epoch(epoch, exp.max_epoch, exp.no_aug_epochs) and not closed:
                closed = True
                loader.close_mosaic()
                model.head.use_l1 = True
                exp.eval_interval = 1
                if epoch > 0:
                    save(out / "last_mosaic_epoch_ckpt.pth", state(epoch, None))
                    switched = True
                it = data_iter(loader, sampler, samples_seen)  # no queued mosaic batch
            t_epoch, iter_s, losses, mosaic_n, sizes = time.time(), [], [], 0, set()
            lr_first = optimizer.param_groups[0]["lr"]
            for i in range(ipe):
                ti = time.time()
                inps, targets, info, _ = next(it)
                mosaic_n += int(is_mosaic(info, exp.input_size).sum())
                inps = inps.to(dev, non_blocking=True).float()
                targets = targets.to(dev, non_blocking=True).float()
                targets.requires_grad = False
                inps, targets = exp.preprocess(inps, targets, tsize)
                with autocast():
                    outputs = model(inps, targets)
                loss = outputs["total_loss"]
                if loss.device.type != dev.type:  # a silent CPU path would show here
                    raise SystemExit("loss on %s, expected %s" % (loss.device, dev))
                step_lr = optimizer.param_groups[0]["lr"]
                optimizer.zero_grad()
                scaler.scale(loss).backward()
                scaler.step(optimizer)
                scaler.update()
                if ema:
                    ema.update(model)
                samples_seen += args.batch
                progress = epoch * ipe + i
                lr = lr_sched.update_lr(progress + 1)
                for g in optimizer.param_groups:
                    g["lr"] = lr
                # l1_loss is a plain 0.0 while the L1 loss is off
                rec_i = {k: v.item() if torch.is_tensor(v) else float(v) for k, v in outputs.items()}
                if not all(np.isfinite(v) for v in rec_i.values()):
                    raise SystemExit("non-finite loss at epoch %d iter %d: %s" % (epoch + 1, i + 1, rec_i))
                sync(dev)
                iter_s.append(time.time() - ti)
                sizes.add(tsize[0])
                rec_i.update(epoch=epoch + 1, iter=i + 1, step=progress + 1, step_lr=step_lr, next_lr=lr,
                             size=tsize[0], mosaic=int(is_mosaic(info, exp.input_size).sum()),
                             it_s=round(iter_s[-1], 4))
                losses.append(rec_i)
                append_jsonl(out / "iters.jsonl", rec_i)
                if args.multiscale and (progress + 1) % 10 == 0:
                    tsize = random_size(exp)
                for w in caught:
                    msg = "%s: %s" % (w.category.__name__, w.message)
                    if msg not in seen_warnings:
                        seen_warnings.append(msg)
                        print("warning:", msg, flush=True)
                caught.clear()
            epoch_s = time.time() - t_epoch

            # ---- after_epoch: validate, then checkpoint (latest knows the best)
            completed = epoch + 1
            metrics, new_best = None, False
            if completed % exp.eval_interval == 0:
                metrics = validate(ema.ema if ema else model, exp, val_loader, dev, args.pr_conf)
                new_best = is_better(metrics[BEST_METRIC], best)
                if new_best:
                    best, best_epoch = metrics[BEST_METRIC], completed
            st = state(completed, metrics)
            save(out / "latest_ckpt.pth", st)
            written = ["latest_ckpt.pth"] + (["last_mosaic_epoch_ckpt.pth"] if switched else [])
            if new_best:
                save(out / "best_ckpt.pth", st)
                written.append("best_ckpt.pth")
            if completed == exp.max_epoch:
                save(out / "final_ckpt.pth", st)
                written.append("final_ckpt.pth")
            steady = iter_s[1:] or iter_s
            erec = {"epoch": completed, "mosaic_samples": mosaic_n, "samples": ipe * args.batch,
                    "use_l1": bool(model.head.use_l1), "l1_loss_mean": float(np.mean([r["l1_loss"] for r in losses])),
                    "total_loss_mean": float(np.mean([r["total_loss"] for r in losses])),
                    "lr_first_step": lr_first, "lr_last_step": losses[-1]["step_lr"], "lr_next": losses[-1]["next_lr"],
                    "ema_updates": ema.updates if ema else None, "samples_seen": samples_seen, "sizes": sorted(sizes),
                    "seconds": round(epoch_s, 1), "images_per_s": round(ipe * args.batch / epoch_s, 1),
                    "steady_images_per_s": round(args.batch / float(np.median(steady)), 1),
                    "memory": memory(dev), "eval_interval": exp.eval_interval, "metrics": metrics,
                    "best": new_best, "best_ap": best, "best_epoch": best_epoch, "written": written}
            append_jsonl(out / "epochs.jsonl", erec)
            epochs_done.append(erec)
            print(json.dumps({k: v for k, v in erec.items() if k != "memory"}
                             | {"metrics": metrics and {k: metrics[k] for k in
                                                        ("map50_95", "map50", "precision", "recall")}}),
                  flush=True)
            if metrics:
                rec["validation"].append({"epoch": completed, **metrics})
            if args.stop_after_epoch and completed == args.stop_after_epoch and completed < exp.max_epoch:
                session["stopped_after_epoch"] = completed
                break

    sess_s = time.time() - t_start
    session.update(finished=datetime.datetime.now(datetime.timezone.utc).isoformat(), seconds=round(sess_s, 1),
                   epochs=[e["epoch"] for e in epochs_done], warnings=seen_warnings, memory=memory(dev),
                   images_per_s=round(sum(e["samples"] for e in epochs_done) / sess_s, 1) if epochs_done else None,
                   loss_on_device_every_iter=True)
    rec["best_checkpoint"].update(value=best, epoch=best_epoch)
    rec["checkpoints"] = run_record.checkpoints(out)
    rec["completed"] = (out / "final_ckpt.pth").exists()
    write_json(runj, rec)
    print(json.dumps({"session": session, "best": rec["best_checkpoint"],
                      "checkpoints": {k: v["sha256"][:16] for k, v in rec["checkpoints"].items()}}, indent=1))


def eval_only(args):
    """Validate a checkpoint's "model" weights (the EMA ones) on any device."""
    from yolox.exp import get_exp

    dev = resolve_device(args.device)
    exp = get_exp(args.exp, None)
    exp.data_num_workers = args.workers
    ck = torch.load(args.eval_only, map_location="cpu", weights_only=False)
    model = exp.get_model().to(dev)
    model.load_state_dict(ck["model"])
    model.eval()
    m = validate(model, exp, exp.get_eval_loader(batch_size=args.batch, is_distributed=False), dev, args.pr_conf)
    m.update(checkpoint=args.eval_only, sha256=sha256(args.eval_only), device=dev.type,
             checkpoint_metrics=ck.get("metrics") and {k: ck["metrics"][k] for k in ("map50_95", "map50")})
    print(json.dumps(m, indent=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exp", required=True)
    ap.add_argument("--out")
    ap.add_argument("--device", default="auto", help="auto (xpu, then cuda, then cpu) | cpu | xpu | cuda")
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--workers", type=int, default=4, help="data loader workers (0: fully seeded, slow)")
    ap.add_argument("--threads", type=int, default=4, help="CPU threads (cpu device only)")
    ap.add_argument("--amp", default="off", choices=("off", "bf16", "fp16"), help="fp16 adds a GradScaler")
    ap.add_argument("--no-multiscale", dest="multiscale", action="store_false",
                    help="keep the input at exp.input_size (upstream resizes every 10 iterations)")
    for k in SCHEDULE_KEYS:
        ap.add_argument("--" + k.replace("_", "-"), type=int, help="override exp.%s (smoke runs only)" % k)
    ap.add_argument("--iters-per-epoch", type=int, help="cap iterations per epoch (debug/tests only)")
    ap.add_argument("--resume", help="latest (RUN_DIR/latest_ckpt.pth) or a checkpoint of this run")
    ap.add_argument("--stop-after-epoch", type=int, help="stop normally after this 1-based epoch")
    ap.add_argument("--pr-conf", type=float, default=0.35,
                    help="score threshold for precision/recall (0.35: decoder_check's 350 per-mille)")
    ap.add_argument("--eval-only", metavar="CKPT", help="validate CKPT and exit")
    args = ap.parse_args()
    if args.eval_only:
        return eval_only(args)
    if not args.out:
        ap.error("--out is required for training")
    train(args)


if __name__ == "__main__":
    main()
