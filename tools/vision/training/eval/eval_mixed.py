#!/usr/bin/env python3
"""Score a DOORS YOLOX checkpoint on a traffic6 split with mixed COCO / Open
Images semantics, per class and per object size, with an error breakdown.

Why: traffic_{val,test}.json are plain COCO files, and plain COCOeval (what
train_loop.validate uses) treats every image as exhaustively labelled for all
six classes. COCO is; Open Images is not. It boxes only classes a human
verified in the image (DATASET_TRAFFIC6_R0.md section 7), so an unboxed car in
an image where nobody checked for cars is not a false positive.

Rule. Class c is scored on an image:
  - COCO image: always (exhaustively labelled for all six classes).
  - Open Images image: only when the human-verified image-level labels in the
    split manifest (`human_labels`) hold c's PRIMARY label (Car, Truck, Bus,
    Motorcycle, Bicycle, Person) as present (1) or absent (0).
Otherwise the (image, class) pair is left out entirely: its boxes are not
counted as misses and its detections are not counted as false positives
(the dropped boxes are counted in the output).
Not enough to score a class:
  - a verified folded label (Van, Limousine, Taxi -> car; Man, Woman, Boy,
    Girl -> person), present or absent: Van present means the vans are
    boxed, not every car; Man present says nothing about unboxed women;
  - an existing box of the class, which may come from such a label.
Assumption (ASSUMED, measured in the output): Taxi is a sibling of Car in
Open Images' hierarchy, not a child, so "Car absent" does not formally rule
out a taxi. A verified "Car absent" is still taken as no car of any kind;
`errors.car_fp_on_car_absent_taxi_unverified` counts the car false positives
that rest on this.

This is a CUSTOM mixed-dataset metric, not the official Open Images
challenge protocol. It shares that protocol's core rule (score a class only
on images where it was verified) but differs: COCO AP@[.50:.95] and AP50
with 101-point interpolation instead of Open Images AP@0.5; group-of boxes
are COCO crowd regions (iscrowd=1, detections on them ignored) instead of
Open Images' group-of matching; no hierarchy expansion of detections.
The plain-COCOeval figure is computed beside it for reference.

best_ckpt.pth of a train_loop run is selected by the plain-COCOeval val
mAP50:95 (train_loop.validate), not by this metric. A selection by this
metric must use the validation split only, never test.

Sizes are the dataset's (DATASET_TRAFFIC6_R0.md section 5): side416 =
sqrt(w*h) * 416 / max(W, H); very small < 8, small 8-16, medium 16-64,
large >= 64. COCOeval's area ranges are reused by setting area = side416**2
on boxes and detections.

Precision and recall are at --pr-conf (0.35: decoder_check's 350 per mille)
and IoU 0.50, from pycocotools' own matching (maxDets 100), like train_loop.

Usage (Windows, XPU environment; the same two variables as train_loop.py):
  eval_mixed.py --exp EXP.py --ckpt best_ckpt.pth --split val|test --out DIR
      [--device auto|cpu|xpu|cuda] [--batch 16] [--workers 4] [--dets DETS.json]
--dets reuses a previous run's detections instead of running the model.
Writes DIR/{split}_dets.json and DIR/{split}_metrics.json and prints a summary.
The held-out test split is read only when --split test is given.
"""
import argparse
import contextlib
import hashlib
import io
import json
import math
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "data"))

from openimages_traffic import OI_FOLD, OI_NAME, OI_PRIMARY  # noqa: E402

SPLITS = {"val": "traffic_val.json", "test": "traffic_test.json"}
SIZE_EDGES = (8.0, 16.0, 64.0)
BUCKETS = ("very_small", "small", "medium", "large")
EPS = 1e-9
# COCOeval area ranges on side416**2; both ends are inclusive there, so the
# upper edges sit just below the next bucket's lower edge.
AREA_RNG = [[0.0, 1e10],
            [0.0, SIZE_EDGES[0] ** 2 - EPS],
            [SIZE_EDGES[0] ** 2, SIZE_EDGES[1] ** 2 - EPS],
            [SIZE_EDGES[1] ** 2, SIZE_EDGES[2] ** 2 - EPS],
            [SIZE_EDGES[2] ** 2, 1e10]]
AREA_LBL = ["all"] + list(BUCKETS)
# Open Images display name -> DOORS class, and the primary display name.
FOLD_BY_NAME = {OI_NAME[k]: v for k, v in OI_FOLD.items()}
PRIMARY_NAME = {c: OI_NAME[k] for c, k in OI_PRIMARY.items()}


# ---- pure rules (tests/test_eval_mixed.py)

def side416(w, h, img_w, img_h):
    return math.sqrt(max(w, 0.0) * max(h, 0.0)) * 416.0 / max(img_w, img_h)


def bucket(side):
    for name, edge in zip(BUCKETS, SIZE_EDGES):
        if side < edge:
            return name
    return BUCKETS[-1]


def evaluable_classes(source, human_labels, classes):
    """The classes an image is scored for: all on COCO; on Open Images only
    those whose primary label is human-verified (see the module docstring).
    Folded child labels and existing boxes never make a class scored."""
    if source != "openimages_v7":
        return set(classes)
    labels = human_labels or {}
    return {c for c in classes if PRIMARY_NAME[c] in labels}


def wilson(k, n, z=1.96):
    """95 % Wilson score interval for k successes in n."""
    if not n:
        return [None, None]
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return [round(max(0.0, c - h), 4), round(min(1.0, c + h), 4)]


def iou_xywh(a, b):
    ax1, ay1, bx1, by1 = a[0] + a[2], a[1] + a[3], b[0] + b[2], b[1] + b[3]
    iw = max(0.0, min(ax1, bx1) - max(a[0], b[0]))
    ih = max(0.0, min(ay1, by1) - max(a[1], b[1]))
    inter = iw * ih
    u = a[2] * a[3] + b[2] * b[3] - inter
    return inter / u if u > 0 else 0.0


# ---- inputs

def load_split(data_dir, split):
    """COCO-format gt (dict), manifest rows by image id, the image folder."""
    data_dir = Path(data_dir)
    gt = json.loads((data_dir / "annotations" / SPLITS[split]).read_text(encoding="utf-8"))
    rows = {}
    with open(data_dir / ("manifest_%s.jsonl" % split), encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            rows[r["image_id"]] = r
    ids = {i["id"] for i in gt["images"]}
    if ids != set(rows):
        raise SystemExit("%s and manifest_%s.jsonl list different images" % (SPLITS[split], split))
    folders = {r["path"].split("/", 1)[0] for r in rows.values()}
    if len(folders) != 1:
        raise SystemExit("images of one split lie in several folders: %s" % folders)
    return gt, rows, folders.pop()


def evaluability(gt, rows):
    """image id -> evaluable class names, plus a summary of what was left out."""
    cat_name = {c["id"]: c["name"] for c in gt["categories"]}
    classes = [cat_name[k] for k in sorted(cat_name)]
    ev = {i: evaluable_classes(r["source"], r.get("human_labels"), classes) for i, r in rows.items()}
    dropped = Counter()
    for a in gt["annotations"]:
        c = cat_name[a["category_id"]]
        if not a["iscrowd"] and c not in ev[a["image_id"]]:
            dropped[c] += 1
    oi = [i for i, r in rows.items() if r["source"] == "openimages_v7"]
    summary = {"images": len(rows), "coco_images": len(rows) - len(oi), "openimages_images": len(oi),
               "openimages_scored_per_class": {c: sum(c in ev[i] for i in oi) for c in classes},
               "openimages_left_out_per_class": {c: sum(c not in ev[i] for i in oi) for c in classes},
               "gt_boxes_dropped_class_not_verified": {c: dropped[c] for c in classes},
               "rule": "custom mixed-dataset metric. COCO: all classes. Open Images: a class is scored only where "
                       "its primary label (Car, Truck, Bus, Motorcycle, Bicycle, Person) is human-verified present "
                       "or absent; folded child labels and existing boxes do not make it scored."}
    return ev, classes, summary


# ---- inference (the same path as train_loop.validate)

def run_model(exp, ckpt, data_dir, split, folder, device, batch, workers):
    import torch
    from yolox.data import COCODataset, ValTransform
    from yolox.evaluators import COCOEvaluator
    from yolox.utils import postprocess
    from smoke_train import resolve_device, sync

    dev = resolve_device(device)
    ck = torch.load(ckpt, map_location="cpu", weights_only=False)
    model = exp.get_model().to(dev)
    model.load_state_dict(ck["model"])  # the EMA weights, as train_loop validates them
    model.eval()
    if split == "val":
        # train_loop.validate's own loader: same dataset, transform, order, batch
        exp.data_num_workers = workers
        loader = exp.get_eval_loader(batch_size=batch, is_distributed=False)
    else:
        # The same construction as the exp's get_eval_dataset/get_eval_loader,
        # on the held-out split (the exp itself never reads it).
        ds = COCODataset(data_dir=data_dir, json_file=SPLITS[split], name=folder, img_size=exp.test_size,
                         preproc=ValTransform(legacy=False))
        loader = torch.utils.data.DataLoader(ds, batch_size=batch, sampler=torch.utils.data.SequentialSampler(ds),
                                             num_workers=workers, pin_memory=True)
    conv = COCOEvaluator(loader, exp.test_size, exp.test_conf, exp.nmsthre, exp.num_classes)
    dets, t0 = [], time.time()
    with torch.no_grad():
        for imgs, _, info_imgs, ids in loader:
            out = model(imgs.to(dev).float())
            out = postprocess(out, exp.num_classes, exp.test_conf, exp.nmsthre)
            dets.extend(conv.convert_to_coco_format(out, info_imgs, ids))
    sync(dev)
    for d in dets:
        d.pop("segmentation", None)
    return dets, {"device": dev.type, "seconds": round(time.time() - t0, 1),
                  "checkpoint_epoch": ck.get("start_epoch"),
                  "checkpoint_metrics": ck.get("metrics") and {k: ck["metrics"][k] for k in ("map50_95", "map50")}}


# ---- scoring

def coco_objects(gt, dets):
    """pycocotools objects with area = side416**2 on boxes and detections."""
    from pycocotools.coco import COCO

    size = {i["id"]: (i["width"], i["height"]) for i in gt["images"]}
    g = json.loads(json.dumps(gt))
    for a in g["annotations"]:
        a["area"] = side416(a["bbox"][2], a["bbox"][3], *size[a["image_id"]]) ** 2
    with contextlib.redirect_stdout(io.StringIO()):
        cg = COCO()
        cg.dataset = g
        cg.createIndex()
        cd = cg.loadRes([dict(d) for d in dets]) if dets else None
    if cd is not None:
        for d in cd.dataset["annotations"]:
            d["area"] = side416(d["bbox"][2], d["bbox"][3], *size[d["image_id"]]) ** 2
    return cg, cd


def run_cocoeval(cg, cd, cat_id, img_ids):
    from pycocotools.cocoeval import COCOeval

    with contextlib.redirect_stdout(io.StringIO()):
        ev = COCOeval(cg, cd, "bbox")
        ev.params.catIds = [cat_id]
        ev.params.imgIds = sorted(img_ids)
        ev.params.areaRng = AREA_RNG
        ev.params.areaRngLbl = AREA_LBL
        ev.evaluate()
        ev.accumulate()
    return ev


def ap_of(ev, area_index):
    p = ev.eval["precision"][:, :, 0, area_index, -1]  # [T, R]
    def m(x):
        x = x[x > -1]
        return float(x.mean()) if x.size else None
    return m(p), m(p[0])


def counts_at(ev, area_index, conf, iou_index=0):
    """tp, fp, gt at one score and IoU for one class and area range, plus the
    per-detection and per-box outcomes (area 'all' only)."""
    n_img = len(ev.params.imgIds)
    tp = fp = npos = 0
    for i in range(n_img):
        e = ev.evalImgs[area_index * n_img + i]
        if e is None:
            continue
        npos += int((~np.asarray(e["gtIgnore"], dtype=bool)).sum())
        if not len(e["dtScores"]):
            continue
        keep = (np.asarray(e["dtScores"]) >= conf) & ~np.asarray(e["dtIgnore"][iou_index], dtype=bool)
        matched = np.asarray(e["dtMatches"][iou_index])[keep] > 0
        tp += int(matched.sum())
        fp += int((~matched).sum())
    return tp, fp, npos


def outcomes(ev, conf, iou_index=0):
    """Per detection (score >= conf): matched or not; per non-ignored gt box:
    matched by a detection >= conf or not. Area 'all'."""
    n_img = len(ev.params.imgIds)
    det_fp, gt_fn = [], []
    for i in range(n_img):
        e = ev.evalImgs[i]
        if e is None:
            continue
        scores = np.asarray(e["dtScores"])
        dm = np.asarray(e["dtMatches"][iou_index]) if len(scores) else np.zeros(0)
        di = np.asarray(e["dtIgnore"][iou_index], dtype=bool) if len(scores) else np.zeros(0, bool)
        for d, s, m, ig in zip(e["dtIds"], scores, dm, di):
            if s >= conf and not ig and m == 0:
                det_fp.append(d)
        gm = np.asarray(e["gtMatches"][iou_index]) if len(e["gtIds"]) else np.zeros(0)
        gi = np.asarray(e["gtIgnore"], dtype=bool)
        score_of = dict(zip(e["dtIds"], scores))
        for g, m, ig in zip(e["gtIds"], gm, gi):
            if ig:
                continue
            if m == 0 or score_of.get(int(m), 0.0) < conf:
                gt_fn.append(g)
    return det_fp, gt_fn


def ratio(a, b):
    return a / b if b else None


def score(gt, rows, dets, conf):
    ev_by_img, classes, ev_summary = evaluability(gt, rows)
    cat_id = {c["name"]: c["id"] for c in gt["categories"]}
    cg, cd = coco_objects(gt, dets)
    size = {i["id"]: (i["width"], i["height"]) for i in gt["images"]}
    result = {"evaluability": ev_summary, "pr_conf": conf, "pr_iou": 0.5, "per_class": {}, "sizes": {}}
    pooled = Counter()
    fp_ids, fn_ids = {}, {}
    for c in classes:
        img_ids = [i for i, s in ev_by_img.items() if c in s]
        if cd is None:
            result["per_class"][c] = {"images_scored": len(img_ids), "ap50_95": 0.0, "ap50": 0.0}
            continue
        ev = run_cocoeval(cg, cd, cat_id[c], img_ids)
        ap, ap50 = ap_of(ev, 0)
        tp, fp, npos = counts_at(ev, 0, conf)
        pooled.update(tp=tp, fp=fp, gt=npos)
        result["per_class"][c] = {"images_scored": len(img_ids), "gt": npos, "ap50_95": ap, "ap50": ap50,
                                  "precision": ratio(tp, tp + fp), "recall": ratio(tp, npos),
                                  "recall_ci95": wilson(tp, npos), "tp": tp, "fp": fp, "fn": npos - tp}
        result["sizes"][c] = {}
        for a, lbl in enumerate(AREA_LBL[1:], start=1):
            ap_b, ap50_b = ap_of(ev, a)
            tp_b, fp_b, n_b = counts_at(ev, a, conf)
            result["sizes"][c][lbl] = {"gt": n_b, "ap50_95": ap_b, "ap50": ap50_b, "recall": ratio(tp_b, n_b),
                                       "recall_ci95": wilson(tp_b, n_b), "tp": tp_b, "fn": n_b - tp_b,
                                       "precision": ratio(tp_b, tp_b + fp_b), "fp": fp_b,
                                       "thin": n_b < 30}
        fp_ids[c], fn_ids[c] = outcomes(ev, conf)
    aps = [v["ap50_95"] for v in result["per_class"].values() if v.get("ap50_95") is not None]
    ap50s = [v["ap50"] for v in result["per_class"].values() if v.get("ap50") is not None]
    result.update(map50_95=float(np.mean(aps)) if aps else None, map50=float(np.mean(ap50s)) if ap50s else None,
                  precision=ratio(pooled["tp"], pooled["tp"] + pooled["fp"]),
                  recall=ratio(pooled["tp"], pooled["gt"]), tp=pooled["tp"], fp=pooled["fp"], gt=pooled["gt"])
    # Pooled size buckets over all classes and over the four vehicle classes.
    for group, members in (("all_classes", classes), ("vehicles", ["car", "truck", "bus", "motorcycle"])):
        result["sizes"][group] = {}
        for lbl in BUCKETS:
            s = [result["sizes"][c][lbl] for c in members if c in result["sizes"]]
            tp_b, n_b = sum(x["tp"] for x in s), sum(x["gt"] for x in s)
            aps_b = [x["ap50_95"] for x in s if x["ap50_95"] is not None]
            result["sizes"][group][lbl] = {"gt": n_b, "recall": ratio(tp_b, n_b), "recall_ci95": wilson(tp_b, n_b),
                                           "mean_ap50_95_of_classes_with_gt": float(np.mean(aps_b)) if aps_b else None,
                                           "classes_with_gt": len(aps_b)}
    result["errors"] = errors(gt, rows, cg, cd, ev_by_img, classes, fp_ids, fn_ids, size, conf)
    return result


def errors(gt, rows, cg, cd, ev_by_img, classes, fp_ids, fn_ids, size, conf):
    """False positives split into confusion / localisation / background, misses
    by size, and what the Open Images rule left out."""
    if cd is None:
        return None
    cat_name = {c["id"]: c["name"] for c in gt["categories"]}
    gts_by_img = defaultdict(list)
    for a in gt["annotations"]:
        gts_by_img[a["image_id"]].append(a)
    out = {"false_positives": {}, "confusion_pred_to_gt": Counter(), "missed_by_size": {}, "examples": {}}
    for c in classes:
        kinds, examples = Counter(), defaultdict(list)
        for d in cd.loadAnns(fp_ids.get(c, [])):
            others = gts_by_img[d["image_id"]]
            best_any = max((iou_xywh(d["bbox"], g["bbox"]) for g in others), default=0.0)
            other_cls = [(iou_xywh(d["bbox"], g["bbox"]), cat_name[g["category_id"]]) for g in others
                         if cat_name[g["category_id"]] != c and not g["iscrowd"]]
            conf_hit = max(other_cls, default=(0.0, None))
            src = rows[d["image_id"]]["source"]
            if conf_hit[0] >= 0.5:
                k = "confused_with_" + conf_hit[1]
                out["confusion_pred_to_gt"]["%s->%s" % (c, conf_hit[1])] += 1
            elif best_any >= 0.1:
                k = "localisation_or_duplicate"
            else:
                k = "background"
            kinds[k] += 1
            kinds["%s_%s" % (k.split("_with_")[0], src)] += 1
            examples[k].append((round(d["score"], 3), rows[d["image_id"]]["uid"], [round(x, 1) for x in d["bbox"]]))
        out["false_positives"][c] = dict(kinds, total=len(fp_ids.get(c, [])))
        out["examples"]["fp_" + c] = {k: sorted(v, reverse=True)[:8] for k, v in examples.items()}
        miss = Counter()
        miss_ex = []
        for g in cg.loadAnns(fn_ids.get(c, [])):
            b = bucket(side416(g["bbox"][2], g["bbox"][3], *size[g["image_id"]]))
            miss[b] += 1
            miss[b + "_" + rows[g["image_id"]]["source"]] += 1
            if b in ("very_small", "small"):
                miss_ex.append((rows[g["image_id"]]["uid"], b, [round(x, 1) for x in g["bbox"]]))
        out["missed_by_size"][c] = dict(miss, total=len(fn_ids.get(c, [])))
        out["examples"]["missed_small_" + c] = miss_ex[:8]
    out["confusion_pred_to_gt"] = dict(out["confusion_pred_to_gt"].most_common())
    # The Taxi assumption: car false positives on Open Images images where Car
    # is verified absent but Taxi (not a child of Car) was never verified, and
    # that no box of another class explains (IoU < 0.5 with every non-car
    # box): only those could be an unlabelled taxi. A car detection on a
    # pickup boxed as Truck is a class confusion, not a taxi.
    def explained(d):
        return any(cat_name[g["category_id"]] != "car" and not g["iscrowd"] and iou_xywh(d["bbox"], g["bbox"]) >= 0.5
                   for g in gts_by_img[d["image_id"]])
    taxi = [d for d in cd.loadAnns(fp_ids.get("car", []))
            if (rows[d["image_id"]].get("human_labels") or {}).get("Car") == 0
            and "Taxi" not in (rows[d["image_id"]].get("human_labels") or {}) and not explained(d)]
    out["car_fp_on_car_absent_taxi_unverified"] = {
        "count": len(taxi), "of_car_fp": len(fp_ids.get("car", [])),
        "examples": sorted(((round(d["score"], 3), rows[d["image_id"]]["uid"], [round(x, 1) for x in d["bbox"]])
                            for d in taxi), reverse=True)[:8]}
    # Detections the Open Images rule left out: they would be false positives
    # under plain COCOeval. Counted at pr_conf, by class.
    left = Counter()
    left_ex = defaultdict(list)
    for d in cd.dataset["annotations"]:
        c = cat_name[d["category_id"]]
        if d["score"] >= conf and c not in ev_by_img[d["image_id"]]:
            left[c] += 1
            left_ex[c].append((round(d["score"], 3), rows[d["image_id"]]["uid"], [round(x, 1) for x in d["bbox"]]))
    out["openimages_left_out_detections"] = dict(left)
    out["examples"]["openimages_left_out"] = {c: sorted(v, reverse=True)[:8] for c, v in left_ex.items()}
    return out


def naive(gt, dets, conf):
    """Plain COCOeval over all images and classes, as train_loop.validate."""
    import train_loop
    from pycocotools.coco import COCO

    with contextlib.redirect_stdout(io.StringIO()):
        cg = COCO()
        cg.dataset = json.loads(json.dumps(gt))
        cg.createIndex()
    m = train_loop.coco_metrics(cg, [dict(d) for d in dets], conf)
    return {k: m[k] for k in ("map50_95", "map50", "precision", "recall")} | {"per_class": m["per_class"]}


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exp", required=True)
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--split", required=True, choices=sorted(SPLITS))
    ap.add_argument("--out", required=True)
    ap.add_argument("--device", default="auto")
    ap.add_argument("--batch", type=int, default=16)
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--pr-conf", type=float, default=0.35)
    ap.add_argument("--dets", help="detections of an earlier run of this script (skips inference)")
    a = ap.parse_args()

    from yolox.exp import get_exp

    exp = get_exp(a.exp, None)
    exp._check_dataset()
    gt, rows, folder = load_split(exp.data_dir, a.split)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    if a.dets:
        dets, inference = json.loads(Path(a.dets).read_text()), {"reused": a.dets}
    else:
        dets, inference = run_model(exp, a.ckpt, exp.data_dir, a.split, folder, a.device, a.batch, a.workers)
        (out / ("%s_dets.json" % a.split)).write_text(json.dumps(dets))
    res = {"split": a.split, "annotations": SPLITS[a.split],
           "annotations_sha256": sha256(Path(exp.data_dir) / "annotations" / SPLITS[a.split]),
           "manifest_sha256": sha256(Path(exp.data_dir) / ("manifest_%s.jsonl" % a.split)),
           "checkpoint": Path(a.ckpt).name, "checkpoint_sha256": sha256(a.ckpt), "inference": inference,
           "test_conf": exp.test_conf, "nmsthre": exp.nmsthre, "test_size": list(exp.test_size),
           "detections": len(dets), "size_buckets_side416": {"very_small": "<8", "small": "8-16",
                                                              "medium": "16-64", "large": ">=64"}}
    res["mixed"] = score(gt, rows, dets, a.pr_conf)
    res["plain_cocoeval_reference"] = naive(gt, dets, a.pr_conf)
    (out / ("%s_metrics.json" % a.split)).write_text(json.dumps(res, indent=1, default=str))
    m = res["mixed"]
    print(json.dumps({"split": a.split, "mixed": {k: m[k] for k in ("map50_95", "map50", "precision", "recall")},
                      "plain": {k: res["plain_cocoeval_reference"][k] for k in ("map50_95", "map50")},
                      "per_class": {c: {k: v.get(k) for k in ("ap50_95", "ap50", "precision", "recall", "gt")}
                                    for c, v in m["per_class"].items()}}, indent=1, default=str))


if __name__ == "__main__":
    main()
