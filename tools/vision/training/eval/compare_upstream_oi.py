#!/usr/bin/env python3
"""Exploratory: R0 vs upstream YOLOX-Tiny 416 on the Open Images half of the R0
training-diagnostic sample (diag_missed_medium.py --split train, seed 20261017).

- The sample is re-derived with the recorded seed and checked against the recorded id
  list hash. Only its Open Images images are used: upstream was trained on COCO
  train2017 and never on Open Images.
- R0: its saved detections for the sample (no rerun).
- Upstream: the archived release-0.1.1rc0 `yolox_tiny.pth`, YOLOX's own exps/default/
  yolox_tiny.py, through the same eval dataset (ValTransform(legacy=False), 416
  letterbox); upstream's normal inference: 80-class argmax, class-aware NMS 0.65,
  score floor 0.01. COCO classes are mapped by name to the traffic6 category ids
  (car 2 -> 1, truck 7 -> 2, bus 5 -> 3, motorcycle 3 -> 4, bicycle 1 -> 5,
  person 0 -> 6); detections of the other 74 COCO classes are not traffic6 detections
  and are only counted.
- Scoring: eval_mixed's car evaluation on these images (car verified in each; group-of
  boxes are crowd regions), medium = side416 in [16, 64), hit = score >= 0.35 at
  IoU >= 0.5, pycocotools greedy matching (area all, maxDets 100).
Writes DIR/report.json, DIR/upstream_dets.json and a discordant-case sheet.
"""
import argparse
import hashlib
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
import diag_missed_medium as dm  # noqa: E402
import eval_mixed as em  # noqa: E402

COCO_TO_TRAFFIC6 = {2: 1, 7: 2, 5: 3, 3: 4, 1: 5, 0: 6}  # COCO class index -> traffic6 category id
TRAFFIC_COLS = [2, 7, 5, 3, 1, 0]  # COCO columns in traffic6 order (car, truck, bus, motorcycle, bicycle, person)
MEDIUM_AREA = em.AREA_LBL.index("medium")


def upstream_infer(exp_file, ckpt, data_dir, ids, device):
    import torch
    from yolox.data.datasets.coco_classes import COCO_CLASSES
    from yolox.exp import get_exp
    from yolox.utils import postprocess, xyxy2xywh
    from smoke_train import resolve_device, sync

    exp = get_exp(exp_file, None)
    dev = resolve_device(device)
    ck = torch.load(ckpt, map_location="cpu", weights_only=False)
    model = exp.get_model().to(dev)
    model.load_state_dict(ck["model"], strict=True)
    model.eval()
    ds = dm.eval_dataset(data_dir, "traffic_train.json", "train2017", exp.test_size)
    pos = {iid: k for k, iid in enumerate(ds.ids)}
    sub = torch.utils.data.Subset(ds, [pos[i] for i in sorted(ids)])
    loader = torch.utils.data.DataLoader(sub, batch_size=16, sampler=torch.utils.data.SequentialSampler(sub),
                                         num_workers=4, pin_memory=True)
    raw, dets, other = {}, [], Counter()
    with torch.no_grad():
        for imgs, _, info_imgs, iids in loader:
            out = model(imgs.to(dev).float())  # [B, rows, 85]: cx cy w h obj cls80 (input pixels, sigmoid)
            o = out.float().cpu().numpy()
            for k, iid in enumerate(iids):
                h, w = float(info_imgs[0][k]), float(info_imgs[1][k])
                cls = o[k, :, 5:]
                rest = np.delete(cls, TRAFFIC_COLS, axis=1).max(1, keepdims=True)
                # rows in diag_missed_medium's layout: box, obj, the six traffic classes, then the best other class
                raw[int(iid)] = (np.concatenate([o[k, :, :5], cls[:, TRAFFIC_COLS], rest], 1),
                                 min(exp.test_size[0] / h, exp.test_size[1] / w))
            pp = postprocess(out.clone(), exp.num_classes, exp.test_conf, exp.nmsthre)
            for k, (p, iid) in enumerate(zip(pp, iids)):
                if p is None:
                    continue
                p = p.cpu()
                h, w = float(info_imgs[0][k]), float(info_imgs[1][k])
                r = min(exp.test_size[0] / h, exp.test_size[1] / w)
                boxes = xyxy2xywh(p[:, :4].clone() / r)
                for b, s, c in zip(boxes, (p[:, 4] * p[:, 5]), p[:, 6].long()):
                    c = int(c)
                    if c in COCO_TO_TRAFFIC6:
                        dets.append({"image_id": int(iid), "category_id": COCO_TO_TRAFFIC6[c],
                                     "bbox": [float(v) for v in b], "score": float(s)})
                    elif float(s) >= 0.35:
                        other[COCO_CLASSES[c]] += 1
    sync(dev)
    return raw, dets, dict(other.most_common()), {"exp": "exps/default/yolox_tiny.py", "num_classes": exp.num_classes,
                                                  "test_conf": exp.test_conf, "nmsthre": exp.nmsthre,
                                                  "test_size": list(exp.test_size)}


def car_eval(gt, rows, dets):
    ev_by_img, _, _ = em.evaluability(gt, rows)
    car = {c["name"]: c["id"] for c in gt["categories"]}["car"]
    img_ids = [i for i, s in ev_by_img.items() if "car" in s]
    cg, cd = em.coco_objects(gt, dets)
    return em.run_cocoeval(cg, cd, car, img_ids)


def summary(ev, conf=0.35):
    tp, fp, n = em.counts_at(ev, 0, conf)
    mtp, mfp, mn = em.counts_at(ev, MEDIUM_AREA, conf)
    ap, ap50 = em.ap_of(ev, 0)
    map_, map50 = em.ap_of(ev, MEDIUM_AREA)
    return {"all_sizes": {"gt": n, "tp": tp, "fp": fp, "recall": tp / n, "precision": tp / max(1, tp + fp),
                          "ap50_95": ap, "ap50": ap50},
            "medium": {"gt": mn, "tp": mtp, "fp_medium_sized_dets": mfp, "recall": mtp / mn,
                       "precision": mtp / max(1, mtp + mfp), "ap50_95": map_, "ap50": map50}}


def curve(ev):
    """Medium-car recall and all-size car false positives over score thresholds."""
    out = []
    for t in np.round(np.arange(0.05, 0.96, 0.01), 2):
        tp, fp, n = em.counts_at(ev, 0, t)
        mtp, _, mn = em.counts_at(ev, MEDIUM_AREA, t)
        out.append({"t": float(t), "fp_all": fp, "tp_all": tp, "medium_recall": mtp / mn})
    return out


def sign_test(pos, neg):
    n = pos + neg
    if not n:
        return 1.0
    k = min(pos, neg)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--r0-exp", required=True)
    ap.add_argument("--r0-dets", required=True, help="train_sample_dets.json of the R0 training diagnostic")
    ap.add_argument("--r0-records", required=True, help="missed_medium_cars.json of the R0 training diagnostic")
    ap.add_argument("--upstream-exp", required=True)
    ap.add_argument("--upstream-ckpt", required=True)
    ap.add_argument("--expect-sample-sha", required=True)
    ap.add_argument("--seed", type=int, default=20261017)
    ap.add_argument("--train-images", type=int, default=1000)
    ap.add_argument("--out", required=True)
    ap.add_argument("--device", default="auto")
    a = ap.parse_args()

    from yolox.exp import get_exp

    r0exp = get_exp(a.r0_exp, None)
    r0exp._check_dataset()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    gt, rows, sel = dm.select_train_sample(r0exp.data_dir, a.train_images, a.seed)
    if sel["image_ids_sha256"] != a.expect_sample_sha:
        raise SystemExit("re-derived sample differs from the recorded one")
    oi = {i for i, r in rows.items() if r["source"] == "openimages_v7"}
    gt = {"images": [i for i in gt["images"] if i["id"] in oi],
          "annotations": [x for x in gt["annotations"] if x["image_id"] in oi], "categories": gt["categories"]}
    rows = {i: r for i, r in rows.items() if i in oi}
    r0_dets = [d for d in json.loads(Path(a.r0_dets).read_text()) if d["image_id"] in oi]
    r0_recs = {int(k): v for k, v in json.loads(Path(a.r0_records).read_text()).items()}

    raw_up, up_dets, other_cls, up_settings = upstream_infer(a.upstream_exp, a.upstream_ckpt, r0exp.data_dir, oi, a.device)
    (out / "upstream_dets.json").write_text(json.dumps(up_dets))
    ckpt_sha = hashlib.sha256(Path(a.upstream_ckpt).read_bytes()).hexdigest()

    ev0, evu = car_eval(gt, rows, r0_dets), car_eval(gt, rows, up_dets)
    b0, bu = dm.medium_car_outcomes(gt, rows, r0_dets), dm.medium_car_outcomes(gt, rows, up_dets)
    if {g["id"] for g in b0["medium"]} != {g["id"] for g in bu["medium"]}:
        raise SystemExit("medium car sets differ")
    if not b0["missed"] <= set(r0_recs):
        raise SystemExit("R0 misses here are not all in the training-diagnostic records")
    medium = b0["medium"]
    hit0 = {g["id"]: g["id"] not in b0["missed"] for g in medium}
    hitu = {g["id"]: g["id"] not in bu["missed"] for g in medium}

    # recall by size band
    bands = {}
    for lo, hi in dm.BANDS:
        s = [g for g in medium if lo <= em.side416(g["bbox"][2], g["bbox"][3], *b0["size"][g["image_id"]]) < hi]
        bands["%d-%d" % (lo, hi)] = {"gt": len(s), "r0": sum(hit0[g["id"]] for g in s), "upstream": sum(hitu[g["id"]] for g in s)}

    # paired outcomes, car level and image level
    cell = Counter(("both" if hit0[i] and hitu[i] else "r0_only" if hit0[i] else "upstream_only" if hitu[i] else "neither")
                   for i in hit0)
    per_img = defaultdict(lambda: [0, 0])
    for g in medium:
        per_img[g["image_id"]][0] += hit0[g["id"]]
        per_img[g["image_id"]][1] += hitu[g["id"]]
    up_more = sum(u > r for r, u in per_img.values())
    r0_more = sum(r > u for r, u in per_img.values())

    # categories of the misses (upstream: same tree; 'other_coco' = best non-traffic COCO class)
    dm_classes = dm.CLASSES
    dm.CLASSES = dm_classes + ["other_coco"]
    try:
        up_recs = {}
        for g in medium:
            if hitu[g["id"]]:
                continue
            o, ratio = raw_up[g["image_id"]]
            rec = dm.classify(g, o, ratio, bu["final_car"][g["image_id"]], bu["matched_to"], bu["ignored"])
            rec.pop("_near_rows", None)
            up_recs[g["id"]] = rec
    finally:
        dm.CLASSES = dm_classes
    cat = lambda r: r["category"] + ("|" + r["factor"] if r.get("factor") else "")  # noqa: E731
    up_cats = Counter(cat(r) for r in up_recs.values())
    r0_cats = Counter(cat(r0_recs[i]) for i in b0["missed"])
    recovered_from = Counter(cat(r0_recs[i]) for i in hit0 if hitu[i] and not hit0[i])
    lost_as = Counter(cat(up_recs[i]) for i in hit0 if hit0[i] and not hitu[i])

    # comparable false-positive level (exploratory; no threshold is selected)
    c0, cu = curve(ev0), curve(evu)
    fp0 = summary(ev0)["all_sizes"]["fp"]
    at_fp = [x for x in cu if x["fp_all"] <= fp0]
    match_fp = min(at_fp, key=lambda x: x["t"]) if at_fp else None

    report = {
        "sample": {"seed": a.seed, "image_ids_sha256": sel["image_ids_sha256"], "openimages_images": len(oi),
                   "medium_cars": len(medium), "images_with_medium_cars": len(per_img)},
        "upstream": {"checkpoint": Path(a.upstream_ckpt).name, "sha256": ckpt_sha, **up_settings,
                     "preprocessing": "ValTransform(legacy=False): BGR 0-255, 416 letterbox top-left, pad 114",
                     "class_mapping": "COCO index -> traffic6 id %s" % COCO_TO_TRAFFIC6,
                     "non_traffic_dets_ge_0.35": other_cls},
        "r0": {"detections": "saved train_sample_dets.json (same preprocessing, 6-class argmax)"},
        "at_0.35": {"r0": summary(ev0), "upstream": summary(evu)},
        "medium_recall_by_band": bands,
        "paired_cars": dict(cell),
        "paired_images": {"upstream_more": up_more, "r0_more": r0_more, "equal": len(per_img) - up_more - r0_more,
                          "sign_test_p_exploratory": sign_test(up_more, r0_more)},
        "missed_categories": {"r0": dict(r0_cats.most_common()), "upstream": dict(up_cats.most_common())},
        "upstream_recovered_r0_category": dict(recovered_from.most_common()),
        "r0_only_upstream_category": dict(lost_as.most_common()),
        "comparable_fp": {"r0_fp_all_at_0.35": fp0, "upstream_lowest_threshold_with_fp_le_r0": match_fp},
        "curves": {"r0": c0, "upstream": cu},
    }

    # discordant sample for visual review (seeded), sheet stays local
    rnd = random.Random(20261016)
    up_only = sorted(i for i in hit0 if hitu[i] and not hit0[i])
    r0_only = sorted(i for i in hit0 if hit0[i] and not hitu[i])
    pick = rnd.sample(up_only, min(8, len(up_only))) + rnd.sample(r0_only, min(4, len(r0_only)))
    report["discordant_sample"] = [{"gt_id": i, "uid": rows[next(g for g in medium if g["id"] == i)["image_id"]]["uid"],
                                    "kind": "upstream_only" if i in up_only else "r0_only"} for i in pick]
    report["discordant_sheet"] = sheet(pick, medium, rows, r0exp.data_dir, b0, bu, set(up_only), out)
    (out / "report.json").write_text(json.dumps(report, indent=1, default=str))
    print(json.dumps({k: v for k, v in report.items() if k != "curves"}, indent=1, default=str))


def sheet(ids, medium, rows, data_dir, b0, bu, up_only, out):
    import cv2

    g_of = {g["id"]: g for g in medium}
    tiles = []
    for i in ids:
        g = g_of[i]
        img = cv2.imread(str(Path(data_dir) / rows[g["image_id"]]["path"]))
        H, W = img.shape[:2]
        for dets, col in ((b0["final_car"][g["image_id"]], (0, 0, 255)), (bu["final_car"][g["image_id"]], (255, 128, 0))):
            for d in dets:
                if d["score"] >= 0.35:
                    x, y, w, h = map(int, d["bbox"])
                    cv2.rectangle(img, (x, y), (x + w, y + h), col, 1)
        x, y, w, h = g["bbox"]
        cv2.rectangle(img, (int(x), int(y)), (int(x + w), int(y + h)), (0, 200, 0), 2)
        side = max(w, h) * 3 + 60
        cx, cy = x + w / 2, y + h / 2
        crop = img[int(max(0, cy - side / 2)):int(min(H, cy + side / 2)), int(max(0, cx - side / 2)):int(min(W, cx + side / 2))]
        s = 300 / max(crop.shape[:2])
        crop = cv2.resize(crop, None, fx=s, fy=s)
        can = np.full((330, 300, 3), 30, np.uint8)
        can[:crop.shape[0], :crop.shape[1]] = crop
        best = lambda dets: max([d["score"] for d in dets if dm.iou_xyxy(dm.xyxy(d["bbox"])[None], dm.xyxy(g["bbox"]))[0] >= 0.5] or [0])  # noqa: E731
        cv2.putText(can, "%s r0 %.2f up %.2f" % ("UP-only" if i in up_only else "R0-only", best(b0["final_car"][g["image_id"]]),
                                                 best(bu["final_car"][g["image_id"]])), (3, 322), 0, 0.42, (255, 255, 255), 1)
        tiles.append(can)
    while len(tiles) % 4:
        tiles.append(np.full((330, 300, 3), 30, np.uint8))
    page = np.vstack([np.hstack(tiles[k:k + 4]) for k in range(0, len(tiles), 4)])
    cv2.imwrite(str(out / "discordant_sheet.jpg"), page, [cv2.IMWRITE_JPEG_QUALITY, 85])
    return "discordant_sheet.jpg"


if __name__ == "__main__":
    main()
