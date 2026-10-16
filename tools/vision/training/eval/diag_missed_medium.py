#!/usr/bin/env python3
"""Exploratory diagnostic: why does a checkpoint miss medium cars on the val split?

Evaluation basis (the same as eval_mixed.py / R1A_PLAN.md P):
  - custom mixed COCO / Open Images metric: car is scored on COCO images and on Open
    Images images with Car human-verified (eval_mixed.evaluability)
  - medium: side416 = sqrt(w*h)*416/max(W,H) in [16, 64) (eval_mixed.bucket)
  - a car box is detected when pycocotools (area 'all', maxDets 100) matches it at IoU
    0.50 to a car detection scoring >= 0.35 (eval_mixed.outcomes)
This is the validation split used for diagnosis: findings are exploratory, not
confirmatory.

The saved detections are post-NMS (YOLOX postprocess: score = objectness x the
argmax class confidence, only the argmax class is kept, class-aware NMS 0.65). To see
objectness, the full class vector and what NMS removed, the unchanged checkpoint is
rerun on val through exp.get_eval_loader (as train_loop.validate) and the decoded
head rows of the affected images are kept. The rerun's post-NMS detections must
reproduce the saved ones (checked, reported as parity).

Each missed medium car g gets ONE category, first match in this order:
  1 matching_conflict  a final car detection >= 0.35 with IoU >= 0.5 to g exists, but
                       pycocotools matched it to another box (or ignored it)
  2 nms_suppressed     a pre-NMS row with IoU >= 0.5 to g, argmax class car and score
                       >= 0.35 exists, yet no final car detection >= 0.35 has IoU >= 0.5
  3 wrong_class        a pre-NMS row with IoU >= 0.5 to g has a non-car argmax class
                       and objectness x that class >= 0.35
  4 poor_localisation  a pre-NMS row aimed at g (g is its best-IoU car box) with argmax
                       car and score >= 0.35 has IoU in [0.1, 0.5) only
  5 below_threshold    the best car score (objectness x car confidence, any argmax)
                       among rows with IoU >= 0.5 lies in [0.10, 0.35)
  6 no_useful_candidate that best car score is < 0.10, or no row reaches IoU 0.5
Categories 5 and 6 are split by the factor that falls short at that best row:
low_objectness when objectness <= car confidence, else low_car_confidence.
Overlapping explanations are kept as separate flags per car (crowding, occluding
boxes, Open Images occluded/truncated flags, border contact, source, size).

Usage:
  diag_missed_medium.py --exp EXP --ckpt CKPT --saved-dets VAL_DETS.json --out DIR
      [--compare-dets OTHER_VAL_DETS.json --compare-name NAME] [--device xpu] [--sample 24]
"""
import argparse
import json
import math
import random
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "data"))
import eval_mixed as em  # noqa: E402

CONF, IOU, USEFUL = 0.35, 0.5, 0.10
CLASSES = ["car", "truck", "bus", "motorcycle", "bicycle", "person"]


def iou_xyxy(boxes, b):
    """IoU of rows of boxes [N, 4] (x1 y1 x2 y2) with one box b (x1 y1 x2 y2)."""
    ix = np.clip(np.minimum(boxes[:, 2], b[2]) - np.maximum(boxes[:, 0], b[0]), 0, None)
    iy = np.clip(np.minimum(boxes[:, 3], b[3]) - np.maximum(boxes[:, 1], b[1]), 0, None)
    inter = ix * iy
    ua = (boxes[:, 2] - boxes[:, 0]) * (boxes[:, 3] - boxes[:, 1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return np.where(ua > 0, inter / np.maximum(ua, 1e-9), 0.0)


def xyxy(b):
    return np.array([b[0], b[1], b[0] + b[2], b[1] + b[3]], dtype=np.float64)


# ---------------------------------------------------------------- evaluation basis

def medium_car_outcomes(gt, rows, dets):
    """pycocotools car evaluation as eval_mixed: medium car ids, missed ids, and per
    image the final car detections with the box id pycocotools matched each to."""
    ev_by_img, classes, _ = em.evaluability(gt, rows)
    car = {c["name"]: c["id"] for c in gt["categories"]}["car"]
    img_ids = [i for i, s in ev_by_img.items() if "car" in s]
    size = {i["id"]: (i["width"], i["height"]) for i in gt["images"]}
    cg, cd = em.coco_objects(gt, dets)
    ev = em.run_cocoeval(cg, cd, car, img_ids)
    _, fn = em.outcomes(ev, CONF)
    medium = [a for a in cg.loadAnns(cg.getAnnIds(imgIds=img_ids, catIds=[car], iscrowd=False))
              if em.bucket(em.side416(a["bbox"][2], a["bbox"][3], *size[a["image_id"]])) == "medium"]
    matched_to = {}  # det id -> matched gt id (0 = none), area 'all', IoU 0.5
    ignored = set()
    n = len(ev.params.imgIds)
    for i in range(n):
        e = ev.evalImgs[i]
        if e is None:
            continue
        for d, m, ig in zip(e["dtIds"], e["dtMatches"][0], e["dtIgnore"][0]):
            matched_to[d] = int(m)
            if ig:
                ignored.add(d)
    final = defaultdict(list)
    for d in cd.loadAnns(cd.getAnnIds(catIds=[car])):
        final[d["image_id"]].append(d)
    return {"medium": medium, "missed": set(fn) & {a["id"] for a in medium}, "img_ids": img_ids,
            "matched_to": matched_to, "ignored": ignored, "final_car": final, "cg": cg, "size": size}


# ---------------------------------------------------------------- diagnostic inference

def rerun(exp_path, ckpt, device, keep_ids):
    """Decoded head rows (cx cy w h obj cls..., input pixels) for keep_ids, plus the
    rerun's post-NMS detections in COCO format for the parity check."""
    import torch
    from yolox.evaluators import COCOEvaluator
    from yolox.exp import get_exp
    from yolox.utils import postprocess
    from smoke_train import resolve_device, sync

    exp = get_exp(exp_path, None)
    dev = resolve_device(device)
    ck = torch.load(ckpt, map_location="cpu", weights_only=False)
    model = exp.get_model().to(dev)
    model.load_state_dict(ck["model"])
    model.eval()
    loader = exp.get_eval_loader(batch_size=16, is_distributed=False)
    conv = COCOEvaluator(loader, exp.test_size, exp.test_conf, exp.nmsthre, exp.num_classes)
    raw, dets = {}, []
    with torch.no_grad():
        for imgs, _, info_imgs, ids in loader:
            out = model(imgs.to(dev).float())
            for k, iid in enumerate(ids):
                if int(iid) in keep_ids:
                    h, w = float(info_imgs[0][k]), float(info_imgs[1][k])
                    r = min(exp.test_size[0] / h, exp.test_size[1] / w)
                    raw[int(iid)] = (out[k].float().cpu().numpy().copy(), r)
            pp = postprocess(out.clone(), exp.num_classes, exp.test_conf, exp.nmsthre)
            dets.extend(conv.convert_to_coco_format(pp, info_imgs, ids))
    sync(dev)
    for d in dets:
        d.pop("segmentation", None)
    return raw, dets, {"test_conf": exp.test_conf, "nmsthre": exp.nmsthre, "test_size": list(exp.test_size)}


BANDS = ((16, 24), (24, 32), (32, 48), (48, 64))


def eval_dataset(data_dir, json_file, folder, test_size):
    """exp.get_eval_dataset's construction (COCODataset + ValTransform(legacy=False)) on
    any annotation file: no training augmentation."""
    from yolox.data import COCODataset, ValTransform

    return COCODataset(data_dir=data_dir, json_file=json_file, name=folder, img_size=test_size,
                       preproc=ValTransform(legacy=False))


def same_preprocessing(exp, n=8):
    """Data-path check without running the model: eval_dataset() on traffic_val.json
    must give byte-identical tensors to exp.get_eval_loader's dataset."""
    ref = exp.get_eval_loader(batch_size=16, is_distributed=False).dataset
    ours = eval_dataset(exp.data_dir, exp.val_ann, "val2017", exp.test_size)
    same = all(np.array_equal(ref[i][0], ours[i][0]) and tuple(ref[i][2]) == tuple(ours[i][2]) for i in range(n))
    return {"items_compared": n, "identical": bool(same)}


def select_train_sample(data_dir, n, seed):
    """Seeded sample of training images with >= 1 scored medium car, half COCO and half
    Open Images. Eligibility uses annotations only, never model output."""
    data_dir = Path(data_dir)
    gt = json.loads((data_dir / "annotations" / "traffic_train.json").read_text(encoding="utf-8"))
    rows = {}
    with open(data_dir / "manifest_train.jsonl", encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            rows[r["image_id"]] = r
    ev_by_img, _, _ = em.evaluability(gt, rows)
    size = {i["id"]: (i["width"], i["height"]) for i in gt["images"]}
    car = {c["name"]: c["id"] for c in gt["categories"]}["car"]
    has_medium = {a["image_id"] for a in gt["annotations"]
                  if a["category_id"] == car and not a["iscrowd"] and "car" in ev_by_img[a["image_id"]]
                  and em.bucket(em.side416(a["bbox"][2], a["bbox"][3], *size[a["image_id"]])) == "medium"}
    rnd = random.Random(seed)
    picked, info = [], {"method": "per source: sorted eligible image ids, random.Random(seed).sample, equal halves",
                        "seed": seed, "eligibility": "car scored (eval_mixed.evaluability) and >= 1 non-crowd medium car",
                        "eligible_images": {}, "sampled_images": {}}
    for src in ("coco2017", "openimages_v7"):
        elig = sorted(i for i in has_medium if rows[i]["source"] == src)
        take = sorted(rnd.sample(elig, min(n // 2, len(elig))))
        picked += take
        info["eligible_images"][src], info["sampled_images"][src] = len(elig), len(take)
    keep = set(picked)
    sub = {"images": [i for i in gt["images"] if i["id"] in keep],
           "annotations": [a for a in gt["annotations"] if a["image_id"] in keep], "categories": gt["categories"]}
    info["image_ids_sha256"] = __import__("hashlib").sha256(",".join(map(str, sorted(keep))).encode()).hexdigest()
    return sub, {i: rows[i] for i in keep}, info


def rerun_ids(exp, ckpt, device, json_file, folder, ids):
    """rerun() on the images `ids` of one annotation file, through eval_dataset()."""
    import torch
    from yolox.evaluators import COCOEvaluator
    from yolox.utils import postprocess
    from smoke_train import resolve_device, sync

    dev = resolve_device(device)
    ck = torch.load(ckpt, map_location="cpu", weights_only=False)
    model = exp.get_model().to(dev)
    model.load_state_dict(ck["model"])
    model.eval()
    ds = eval_dataset(exp.data_dir, json_file, folder, exp.test_size)
    pos = {iid: k for k, iid in enumerate(ds.ids)}
    sub = torch.utils.data.Subset(ds, [pos[i] for i in sorted(ids)])
    sub.class_ids = ds.class_ids  # convert_to_coco_format reads dataloader.dataset.class_ids
    loader = torch.utils.data.DataLoader(sub, batch_size=16, sampler=torch.utils.data.SequentialSampler(sub),
                                         num_workers=exp.data_num_workers, pin_memory=True)
    conv = COCOEvaluator(loader, exp.test_size, exp.test_conf, exp.nmsthre, exp.num_classes)
    raw, dets = {}, []
    with torch.no_grad():
        for imgs, _, info_imgs, iids in loader:
            out = model(imgs.to(dev).float())
            for k, iid in enumerate(iids):
                h, w = float(info_imgs[0][k]), float(info_imgs[1][k])
                raw[int(iid)] = (out[k].float().cpu().numpy().copy(), min(exp.test_size[0] / h, exp.test_size[1] / w))
            dets.extend(conv.convert_to_coco_format(postprocess(out.clone(), exp.num_classes, exp.test_conf, exp.nmsthre),
                                                    info_imgs, iids))
    sync(dev)
    for d in dets:
        d.pop("segmentation", None)
    return raw, dets


def strata(base, recs, rows):
    """Recall and category counts per (source, side416 band) of the medium cars."""
    out = {}
    for src in ("coco2017", "openimages_v7"):
        for lo, hi in BANDS:
            s = [g for g in base["medium"] if rows[g["image_id"]]["source"] == src
                 and lo <= em.side416(g["bbox"][2], g["bbox"][3], *base["size"][g["image_id"]]) < hi]
            miss = [g for g in s if g["id"] in base["missed"]]
            cats = Counter("%s%s" % (recs[g["id"]]["category"], "|" + recs[g["id"]]["factor"] if recs[g["id"]].get("factor") else "")
                           for g in miss)
            out["%s|%d-%d" % (src, lo, hi)] = {"gt": len(s), "detected": len(s) - len(miss),
                                               "recall": round((len(s) - len(miss)) / len(s), 3) if s else None,
                                               "missed_categories": dict(cats.most_common())}
    return out


def parity(saved, rerun_dets):
    key = lambda d: (d["image_id"], d["category_id"])  # noqa: E731
    a, b = defaultdict(list), defaultdict(list)
    for d in saved:
        a[key(d)].append(d)
    for d in rerun_dets:
        b[key(d)].append(d)
    worst_score = worst_box = 0.0
    count_mismatch = 0
    for k in set(a) | set(b):
        x = sorted(a.get(k, []), key=lambda d: -d["score"])
        y = sorted(b.get(k, []), key=lambda d: -d["score"])
        if len(x) != len(y):
            count_mismatch += 1
            continue
        for p, q in zip(x, y):
            worst_score = max(worst_score, abs(p["score"] - q["score"]))
            worst_box = max(worst_box, max(abs(u - v) for u, v in zip(p["bbox"], q["bbox"])))
    return {"saved": len(saved), "rerun": len(rerun_dets), "image_class_groups_with_count_mismatch": count_mismatch,
            "max_abs_score_diff": worst_score, "max_abs_box_diff_px": worst_box}


# ---------------------------------------------------------------- classification

def classify(g, rows_raw, ratio, finals, matched_to, ignored):
    gb = xyxy(g["bbox"])
    o = rows_raw
    cx, cy, w, h = (o[:, k] / ratio for k in range(4))
    boxes = np.stack([cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2], 1)
    obj, cls = o[:, 4], o[:, 5:5 + len(CLASSES)]
    arg = cls.argmax(1)
    s_car = obj * cls[:, 0]
    s_max = obj * cls.max(1)
    ious = iou_xyxy(boxes, gb)
    loc = ious >= IOU
    rec = {"best_iou_any_row": float(ious.max())}
    # final detections at g
    fin = [(d, float(iou_xyxy(xyxy(d["bbox"])[None], gb)[0])) for d in finals]
    fin_hit = [(d, v) for d, v in fin if v >= IOU and d["score"] >= CONF]
    fin_low = [(d, v) for d, v in fin if v >= IOU and d["score"] < CONF]
    rec["final_car_below_thr_at_g"] = max([d["score"] for d, _ in fin_low], default=None)
    if loc.any():
        k = int(np.argmax(np.where(loc, s_car, -1)))
        rec.update(best_row_iou=float(ious[k]), best_car_score=float(s_car[k]), best_obj=float(obj[k]),
                   best_car_conf=float(cls[k, 0]), best_row_argmax=CLASSES[int(arg[k])])
    else:
        rec.update(best_row_iou=None, best_car_score=0.0, best_obj=None, best_car_conf=None, best_row_argmax=None)
    other = loc & (arg != 0) & (s_max >= CONF)
    rec["wrong_class_rows"] = dict(Counter(CLASSES[int(c)] for c in arg[other]))
    # 1 matching conflict
    conflict = [d for d, _ in fin_hit if matched_to.get(d["id"], 0) not in (0, g["id"]) or d["id"] in ignored]
    if fin_hit and conflict:
        rec["category"] = "matching_conflict"
        rec["detail"] = {"matched_to_other_box": [matched_to.get(d["id"], 0) for d in conflict]}
        return rec
    # 2 NMS suppression
    qual = loc & (arg == 0) & (s_car >= CONF)
    if qual.any() and not fin_hit:
        k = int(np.argmax(np.where(qual, s_car, -1)))
        sup = max(((d, float(iou_xyxy(xyxy(d["bbox"])[None], boxes[k])[0])) for d in finals if d["score"] >= s_car[k]),
                  key=lambda t: t[1], default=(None, 0.0))
        rec["category"] = "nms_suppressed"
        rec["detail"] = {"candidate_score": float(s_car[k]), "candidate_iou_to_g": float(ious[k]),
                         "suppressor_iou_to_candidate": sup[1],
                         "suppressor_iou_to_g": float(iou_xyxy(xyxy(sup[0]["bbox"])[None], gb)[0]) if sup[0] else None,
                         "suppressor_matched_other_box": bool(sup[0] and matched_to.get(sup[0]["id"], 0) not in (0, g["id"]))}
        return rec
    # 3 wrong class
    if other.any():
        k = int(np.argmax(np.where(other, s_max, -1)))
        rec["category"] = "wrong_class"
        rec["detail"] = {"class": CLASSES[int(arg[k])], "score": float(s_max[k]), "iou_to_g": float(ious[k])}
        return rec
    # 4 poor localisation (rows aimed at g)
    near = (ious >= 0.1) & (ious < IOU) & (arg == 0) & (s_car >= CONF)
    if near.any():
        rec["category"] = "poor_localisation"
        rec["detail"] = {"best_car_score_near": float(s_car[near].max()), "best_iou_near": float(ious[near].max())}
        rec["_near_rows"] = boxes[near]
        return rec
    # 5 / 6 by the best car score among located rows
    s = rec["best_car_score"]
    base = "below_threshold" if s >= USEFUL else "no_useful_candidate"
    if rec["best_obj"] is None:
        rec["category"], rec["factor"] = base, "no_row_at_iou_0.5"
    else:
        rec["category"] = base
        rec["factor"] = "low_objectness" if rec["best_obj"] <= rec["best_car_conf"] else "low_car_confidence"
    return rec


def flags(g, gt_by_img, ann_meta, size):
    """Overlapping, non-exclusive descriptors of a missed car."""
    W, H = size
    gb = xyxy(g["bbox"])
    others = [a for a in gt_by_img[g["image_id"]] if a["id"] != g["id"]]
    cars = [a for a in others if a["category_id"] == g["category_id"] and not a["iscrowd"]]
    car_iou = [float(iou_xyxy(xyxy(a["bbox"])[None], gb)[0]) for a in cars]
    m = np.zeros((max(1, int(math.ceil(gb[3] - gb[1]))), max(1, int(math.ceil(gb[2] - gb[0])))), bool)
    for a in others:
        ab = xyxy(a["bbox"])
        x1, y1 = int(max(0, ab[0] - gb[0])), int(max(0, ab[1] - gb[1]))
        x2, y2 = int(min(m.shape[1], ab[2] - gb[0])), int(min(m.shape[0], ab[3] - gb[1]))
        if x2 > x1 and y2 > y1 and a["category_id"] != g["category_id"]:
            m[y1:y2, x1:x2] = True
    meta = ann_meta.get(g["id"], {})
    return {"side416": round(em.side416(g["bbox"][2], g["bbox"][3], W, H), 1),
            "aspect_w_over_h": round(g["bbox"][2] / max(g["bbox"][3], 1e-6), 2),
            "cars_overlapping_iou_0.1": sum(v >= 0.1 for v in car_iou),
            "max_iou_other_car": round(max(car_iou, default=0.0), 2),
            "cars_in_image": len(cars) + 1,
            "covered_by_other_class_boxes": round(float(m.mean()), 2),
            "touches_border": bool(gb[0] <= 1 or gb[1] <= 1 or gb[2] >= W - 1 or gb[3] >= H - 1),
            "oi_occluded": meta.get("occluded"), "oi_truncated": meta.get("truncated"),
            "source_label": meta.get("source_label")}


def annotation_meta(rows, gt):
    """json annotation id -> manifest annotation fields, matched by image and bbox."""
    by_img = defaultdict(list)
    for a in gt["annotations"]:
        by_img[a["image_id"]].append(a)
    out = {}
    for iid, r in rows.items():
        for ma in r.get("annotations") or []:
            for a in by_img[iid]:
                if a["id"] not in out and all(abs(u - v) < 0.02 for u, v in zip(a["bbox"], ma["bbox"])):
                    out[a["id"]] = {k: ma.get(k) for k in ("occluded", "truncated", "source_label")}
                    break
    return out


# ---------------------------------------------------------------- sample sheets (local only)

def sheets(sample, recs, rows, data_dir, finals, raw, out):
    import cv2
    tiles = []
    for g in sample:
        r, rec = rows[g["image_id"]], recs[g["id"]]
        img = cv2.imread(str(Path(data_dir) / r["path"]))
        H, W = img.shape[:2]
        x, y, w, h = g["bbox"]
        side = max(w, h) * 3 + 60
        cx, cy = x + w / 2, y + h / 2
        x1, y1 = int(max(0, cx - side / 2)), int(max(0, cy - side / 2))
        x2, y2 = int(min(W, cx + side / 2)), int(min(H, cy + side / 2))
        vis = img.copy()
        for d in finals.get(g["image_id"], []):
            if d["score"] >= CONF:
                b = list(map(int, xyxy(d["bbox"])))
                cv2.rectangle(vis, tuple(b[:2]), tuple(b[2:]), (0, 0, 255), 1)
        o, ratio = raw[g["image_id"]]
        cxs, cys, ws, hs = (o[:, k] / ratio for k in range(4))
        bx = np.stack([cxs - ws / 2, cys - hs / 2, cxs + ws / 2, cys + hs / 2], 1)
        k = int(np.argmax(np.where(iou_xyxy(bx, xyxy(g["bbox"])) >= IOU, o[:, 4] * o[:, 5], -1)))
        if iou_xyxy(bx[k:k + 1], xyxy(g["bbox"]))[0] >= IOU:
            b = list(map(int, bx[k]))
            cv2.rectangle(vis, tuple(b[:2]), tuple(b[2:]), (0, 220, 255), 1)
        b = list(map(int, xyxy(g["bbox"])))
        cv2.rectangle(vis, tuple(b[:2]), tuple(b[2:]), (0, 200, 0), 2)
        crop = vis[y1:y2, x1:x2]
        s = 300 / max(crop.shape[:2])
        crop = cv2.resize(crop, None, fx=s, fy=s, interpolation=cv2.INTER_NEAREST if s > 2 else cv2.INTER_LINEAR)
        can = np.full((340, 300, 3), 30, np.uint8)
        can[:crop.shape[0], :crop.shape[1]] = crop
        t1 = "%s %s" % (rec["category"][:18], rec.get("factor", "")[:16])
        t2 = "obj %s car %s s %.2f %s" % (
            "-" if rec["best_obj"] is None else "%.2f" % rec["best_obj"],
            "-" if rec["best_car_conf"] is None else "%.2f" % rec["best_car_conf"], rec["best_car_score"],
            r["source"][:4])
        cv2.putText(can, t1, (3, 318), 0, 0.4, (255, 255, 255), 1)
        cv2.putText(can, t2, (3, 334), 0, 0.4, (0, 255, 255), 1)
        tiles.append(can)
    pages = []
    for p in range(0, len(tiles), 12):
        t = tiles[p:p + 12]
        while len(t) % 4:
            t.append(np.full((340, 300, 3), 30, np.uint8))
        page = np.vstack([np.hstack(t[i:i + 4]) for i in range(0, len(t), 4)])
        f = out / ("sample_sheet_%d.jpg" % (p // 12 + 1))
        cv2.imwrite(str(f), page, [cv2.IMWRITE_JPEG_QUALITY, 85])
        pages.append(f.name)
    return pages


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exp", required=True)
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--split", default="val", choices=("val", "train"),
                    help="val: the saved detections' split; train: a seeded training sample (--train-images, --seed)")
    ap.add_argument("--saved-dets", help="val: saved detections of --ckpt (required for val)")
    ap.add_argument("--train-images", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=20261017)
    ap.add_argument("--val-dets", help="train: saved val detections of --ckpt, for the stratum comparison")
    ap.add_argument("--val-records", help="train: missed_medium_cars.json of the val diagnostic")
    ap.add_argument("--compare-dets")
    ap.add_argument("--compare-name", default="other")
    ap.add_argument("--out", required=True)
    ap.add_argument("--device", default="auto")
    ap.add_argument("--sample", type=int, default=24)
    a = ap.parse_args()

    from yolox.exp import get_exp

    exp = get_exp(a.exp, None)
    exp._check_dataset()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    exp.data_num_workers = 4
    if a.split == "val":
        if not a.saved_dets:
            ap.error("--saved-dets is required for --split val")
        gt, rows, _ = em.load_split(exp.data_dir, "val")
        saved = json.loads(Path(a.saved_dets).read_text())
        base = medium_car_outcomes(gt, rows, saved)
        missed = [x for x in base["medium"] if x["id"] in base["missed"]]
        keep = {g["image_id"] for g in missed}
        raw, rerun_dets, settings = rerun(a.exp, a.ckpt, a.device, keep)
        par = parity(saved, rerun_dets)
        selection = None
    else:
        # training sample: no saved detections exist; the data path is checked instead
        gt, rows, selection = select_train_sample(exp.data_dir, a.train_images, a.seed)
        par = {"same_preprocessing_as_get_eval_loader": same_preprocessing(exp)}
        raw, dets = rerun_ids(exp, a.ckpt, a.device, exp.train_ann, "train2017", rows.keys())
        settings = {"test_conf": exp.test_conf, "nmsthre": exp.nmsthre, "test_size": list(exp.test_size)}
        (out / "train_sample_dets.json").write_text(json.dumps(dets))
        base = medium_car_outcomes(gt, rows, dets)
        missed = [x for x in base["medium"] if x["id"] in base["missed"]]
        selection["medium_cars_in_sample"] = dict(Counter(rows[g["image_id"]]["source"] for g in base["medium"]))

    gt_by_img = defaultdict(list)
    for x in gt["annotations"]:
        gt_by_img[x["image_id"]].append(x)
    meta = annotation_meta(rows, gt)
    recs = {}
    for g in missed:
        o, ratio = raw[g["image_id"]]
        rec = classify(g, o, ratio, base["final_car"][g["image_id"]], base["matched_to"], base["ignored"])
        rec.pop("_near_rows", None)
        rec.update(flags(g, gt_by_img, meta, base["size"][g["image_id"]]), gt_id=g["id"], image_id=g["image_id"],
                   uid=rows[g["image_id"]]["uid"], source=rows[g["image_id"]]["source"])
        recs[g["id"]] = rec

    cats = Counter(r["category"] for r in recs.values())
    split = Counter((r["category"], r.get("factor")) for r in recs.values() if r["category"] in
                    ("below_threshold", "no_useful_candidate"))
    by_source = Counter((r["category"], r["source"]) for r in recs.values())
    medium_src = Counter(rows[x["image_id"]]["source"] for x in base["medium"])
    missed_src = Counter(r["source"] for r in recs.values())
    hit = [x for x in base["medium"] if x["id"] not in base["missed"]]
    hit_flags = [flags(x, gt_by_img, meta, base["size"][x["image_id"]]) for x in hit]

    def describe(fl):
        n = len(fl)
        if not n:
            return {}
        return {"n": n, "side416_median": float(np.median([f["side416"] for f in fl])),
                "with_overlapping_car_iou_0.1": sum(f["cars_overlapping_iou_0.1"] > 0 for f in fl) / n,
                "covered_by_other_class_>=0.3": sum(f["covered_by_other_class_boxes"] >= 0.3 for f in fl) / n,
                "touches_border": sum(f["touches_border"] for f in fl) / n,
                "oi_occluded": sum(f["oi_occluded"] == 1 for f in fl) / n,
                "oi_truncated": sum(f["oi_truncated"] == 1 for f in fl) / n,
                "cars_in_image_median": float(np.median([f["cars_in_image"] for f in fl]))}

    report = {
        "basis": {"checkpoint": a.ckpt, "saved_dets": a.saved_dets, "metric": "eval_mixed custom mixed metric, car",
                  "medium": "side416 in [16, 64)", "score_threshold": CONF, "iou": IOU, "useful_candidate": USEFUL,
                  "matching": "pycocotools greedy by score, area all, maxDets 100", **settings,
                  "medium_cars": len(base["medium"]), "detected": len(hit), "missed": len(missed),
                  "medium_by_source": dict(medium_src), "missed_by_source": dict(missed_src)},
        "split": a.split, "train_selection": selection,
        "parity_rerun_vs_saved": par,
        "categories": dict(cats.most_common()),
        "categories_by_source": {"%s|%s" % k: v for k, v in sorted(by_source.items())},
        "below_or_none_by_factor": {"%s|%s" % k: v for k, v in sorted(split.items(), key=str)},
        "wrong_class_classes": dict(Counter(r["detail"]["class"] for r in recs.values() if r["category"] == "wrong_class")),
        "missed_flags": describe([recs[g["id"]] for g in missed]),
        "detected_flags": describe(hit_flags),
    }
    if a.compare_dets:
        other = medium_car_outcomes(gt, rows, json.loads(Path(a.compare_dets).read_text()))
        ids = [x["id"] for x in base["medium"]]
        r0 = {i: i not in base["missed"] for i in ids}
        r1 = {i: i not in other["missed"] for i in ids}
        cell = {"both_detect": [i for i in ids if r0[i] and r1[i]], "only_base": [i for i in ids if r0[i] and not r1[i]],
                "only_%s" % a.compare_name: [i for i in ids if r1[i] and not r0[i]],
                "both_miss": [i for i in ids if not r0[i] and not r1[i]]}
        comp = {k: len(v) for k, v in cell.items()}
        rec_ids = cell["only_%s" % a.compare_name]
        comp["recovered_by_base_category"] = dict(Counter(recs[i]["category"] for i in rec_ids))
        comp["both_miss_by_base_category"] = dict(Counter(recs[i]["category"] for i in cell["both_miss"]))
        lost = []
        for i in cell["only_base"]:
            g = next(x for x in base["medium"] if x["id"] == i)
            fin = other["final_car"][g["image_id"]]
            best = max([d["score"] for d in fin if iou_xyxy(xyxy(d["bbox"])[None], xyxy(g["bbox"]))[0] >= IOU], default=0.0)
            lost.append("compare_car_det_below_thr" if best >= 0.01 else "no_compare_car_det_at_iou_0.5")
        comp["lost_by_compare_final_dets"] = dict(Counter(lost))
        g_of = {x["id"]: x for x in base["medium"]}
        comp["lost_flags"] = describe([flags(g_of[i], gt_by_img, meta, base["size"][g_of[i]["image_id"]])
                                       for i in cell["only_base"]])
        comp["recovered_flags"] = describe([recs[i] for i in rec_ids])
        report["comparison"] = comp
        for k, v in cell.items():
            for i in v:
                if i in recs:
                    recs[i]["comparison"] = k

    # stratified sample for visual review (seeded), sheets stay local
    rnd = random.Random(20261016)
    per_cat = defaultdict(list)
    for g in missed:
        per_cat[recs[g["id"]]["category"]].append(g)
    # at least 2 per category (all if fewer), the rest proportional to category size
    sample, total = [], len(missed)
    base_n = {c: min(2, len(v)) for c, v in per_cat.items()}
    left = max(0, a.sample - sum(base_n.values()))
    for c in sorted(per_cat):
        n = min(len(per_cat[c]), base_n[c] + round(left * len(per_cat[c]) / total))
        sample += rnd.sample(per_cat[c], n)
    report["sample"] = [{"gt_id": g["id"], "uid": rows[g["image_id"]]["uid"], "category": recs[g["id"]]["category"]}
                        for g in sample]
    report["sample_sheets"] = sheets(sample, recs, rows, exp.data_dir, base["final_car"], raw, out)
    report["strata"] = strata(base, recs, rows)
    if a.split == "train" and a.val_dets and a.val_records:
        # the saved val results, re-scored from saved files only (no inference on val)
        vgt, vrows, _ = em.load_split(exp.data_dir, "val")
        vbase = medium_car_outcomes(vgt, vrows, json.loads(Path(a.val_dets).read_text()))
        vrecs = {int(k): v for k, v in json.loads(Path(a.val_records).read_text()).items()}
        if set(vrecs) != vbase["missed"]:
            raise SystemExit("val records do not match the saved val detections")
        report["val_strata"] = strata(vbase, vrecs, vrows)
        report["val_categories"] = dict(Counter(r["category"] for r in vrecs.values()).most_common())
    (out / "missed_medium_cars.json").write_text(json.dumps(recs, indent=1, default=str))
    (out / "report.json").write_text(json.dumps(report, indent=1, default=str))
    print(json.dumps({k: report[k] for k in ("basis", "train_selection", "parity_rerun_vs_saved", "categories",
                                             "below_or_none_by_factor", "wrong_class_classes", "missed_flags",
                                             "detected_flags", "strata") + (("val_strata",) if "val_strata" in report else ())} |
                     ({"comparison": {k: v for k, v in report["comparison"].items() if not k.endswith("flags")}}
                      if a.compare_dets else {}), indent=1, default=str))


if __name__ == "__main__":
    main()
