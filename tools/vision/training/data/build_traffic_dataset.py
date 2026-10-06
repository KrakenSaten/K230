#!/usr/bin/env python3
"""Build the DOORS Traffic (traffic6) dataset from COCO 2017 and Open Images V7.

Every included image passes the licence policy, a file check and the leakage
checks; every candidate, included or not, is in provenance.jsonl with its
decision and reasons. Nothing is chosen by hand and nothing is random except
COCO's negative sampling, which is seeded.

Sources and splits (fixed rules, no shuffling across sources):

  COCO train2017 (licence tier 'allow')      -> train 85 %, val 5 %, test 10 %, by sha256(seed:image_id)
  COCO val2017   (licence tier 'allow')      -> val or test, half each, by sha256(seed:image_id)
  Open Images V7 train      (policy-checked) -> train
  Open Images V7 validation (policy-checked) -> val
  Open Images V7 test       (policy-checked) -> test

The held-out test split is written to OUT/test/ and annotations/traffic_test.json.
The YOLOX exp only reads traffic_train.json and traffic_val.json, so the
training loop never sees it.

Output (OUT):
  annotations/traffic_{train,val,test}.json   COCO format, categories 1..6
  train2017/ val2017/ test/                   hard links into RAW (no second copy)
  manifest_{train,val,test}.jsonl             one image per line (see write_outputs)
  provenance.jsonl                            every candidate: decision + reasons
  quarantine.jsonl                            removed after download (file, duplicates, leakage)
  manifest.json                               rules, inputs, versions, counts, hashes
  ATTRIBUTION.tsv                             training images: licence, author, source URL
  test_set.sha256                             the test files, for --exclude-sha256 elsewhere

Usage:
  build_traffic_dataset.py --coco-ann DIR --oi-meta DIR --raw DIR --out DIR
      [--seed 20261004] [--neg-ratio 0.15] [--exclude-sha256 FILE]
      [--near-dup-bits 4] [--workers 16] [--limit N] [--no-download] [--no-openimages]
"""
import argparse
import hashlib
import json
import os
import platform
import random
import re
import shutil
import sys
import urllib.request
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import class_sets  # noqa: E402
import openimages_traffic as oi  # noqa: E402
import prepare_coco_traffic as pct  # noqa: E402
import dataset_checks  # noqa: E402

CLASS_SET = "traffic6"
FORMAT = "doors-traffic-dataset-v2"
SPLIT_DIR = {"train": "train2017", "val": "val2017", "test": "test"}
SPLIT_PRIORITY = {"test": 0, "val": 1, "train": 2}  # who keeps a duplicate
FLICKR_ID = re.compile(r"(?:/photos/[^/]+/|staticflickr\.com/\d+/)(\d+)")


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def split_of_coco(seed, src, image_id):
    """COCO image -> split, fixed by seed, source split and id.

    val2017: half val, half test. train2017: 10 % test and 5 % val, so the
    held-out splits get COCO's street scenes and small vehicles too (Open
    Images' validation/test selection is mostly close-up vehicles)."""
    h = int(hashlib.sha256(("doors-split:%d:coco-%s:%d" % (seed, src, image_id)).encode()).hexdigest()[:8], 16)
    if src == "val2017":
        return "val" if h % 2 == 0 else "test"
    r = h % 20
    return "test" if r < 2 else "val" if r == 2 else "train"


def sha256_text_lf(path):
    """sha256 of a text file with LF line ends: the same on a CRLF checkout."""
    return sha256_bytes(Path(path).read_bytes().replace(b"\r\n", b"\n"))


def write_text_lf(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def flickr_photo_id(*urls):
    for u in urls:
        m = FLICKR_ID.search(u or "")
        if m:
            return m.group(1)
    return None


# ---------------------------------------------------------------- selection

def select_coco(ann_dir, seed, neg_ratio, limit):
    """Chosen COCO images (records) and the provenance rows of all candidates."""
    pol, lic_by_url, ok_tiers = pct.load_policy(False)
    rng = random.Random(seed)
    names = class_sets.names(CLASS_SET)
    target_cat = {class_sets.COCO_CATEGORY_ID[n] for n in names}
    records, prov, inputs = [], [], {}
    for src in ("train2017", "val2017"):
        ann_file = Path(ann_dir) / ("instances_%s.json" % src)
        inputs["coco/" + ann_file.name] = pct.sha256_file(ann_file)
        coco = json.loads(ann_file.read_text())
        images, anns, _ = pct.select(coco, CLASS_SET, lic_by_url, ok_tiers, neg_ratio, rng, 0)
        chosen = {i["id"] for i in images}
        if limit:
            keep = sorted(chosen)[:limit]
            chosen = set(keep)
        lic = {l["id"]: (l["url"], lic_by_url.get(l["url"], {"name": l["name"], "tier": "exclude"}))
               for l in coco["licenses"]}
        has_target = defaultdict(set)
        for a in coco["annotations"]:
            if a["category_id"] in target_cat:
                has_target[a["image_id"]].add(a["category_id"])
        anns_by = defaultdict(list)
        for a in anns:
            anns_by[a["image_id"]].append(a)
        for im in sorted(coco["images"], key=lambda i: i["id"]):
            url, p = lic[im["license"]]
            positive = im["id"] in has_target
            if not positive and im["id"] not in chosen:
                continue  # neither a target image nor a chosen negative: not a candidate
            uid = "coco:%s:%012d" % (src, im["id"])
            split = split_of_coco(seed, src, im["id"])
            reasons = []
            if p["tier"] != "allow":
                reasons.append("coco_licence_%s" % p["tier"])
            elif im["id"] not in chosen:
                reasons.append("over_limit")
            row = {"uid": uid, "source": "coco2017", "source_id": "%s/%d" % (src, im["id"]),
                   "split": split, "kind": "positive" if positive else "negative",
                   "licence": p["name"], "licence_url": url, "licence_tier": p["tier"],
                   "author": None, "author_url": None,
                   "original_url": im["flickr_url"], "landing_url": None, "download_url": im["coco_url"],
                   "annotation_source": "COCO 2017 instances (CC BY 4.0, COCO Consortium)",
                   "decision": "rejected" if reasons else "selected", "reasons": reasons}
            prov.append(row)
            if reasons:
                continue
            boxes = [{"class_id": a["category_id"] - 1, "bbox": [float(v) for v in a["bbox"]],
                      "iscrowd": int(a["iscrowd"]), "source_label": "coco:%d" % a["coco_category_id"],
                      "occluded": None, "truncated": None}
                     for a in sorted(anns_by.get(im["id"], []), key=lambda a: a["id"])]
            records.append(dict(row, raw_path="coco/%s/%s" % (src, im["file_name"]),
                                file_name="coco/" + im["file_name"],
                                expect_size=[im["width"], im["height"]], boxes=boxes,
                                flickr_id=flickr_photo_id(im["flickr_url"])))
    return records, prov, inputs


def select_openimages(meta_dir, limit):
    oi.check_class_ids(meta_dir)
    names = class_sets.names(CLASS_SET)
    records, prov, inputs, outside = [], [], {}, {}
    for f in sorted(Path(meta_dir).iterdir()):
        if f.is_file():
            inputs["openimages/" + f.name] = pct.sha256_file(f)
    for subset, split in (("train", "train"), ("validation", "val"), ("test", "test")):
        boxes = oi.read_boxes(meta_dir, subset)
        cand = oi.candidates(boxes)
        cs = set(cand)
        outside[subset] = sum(1 for i, bs in boxes.items()
                              if i not in cs and any(b["label"] in oi.OI_FOLD for b in bs))
        rows = oi.read_images(meta_dir, subset, cs)
        human = oi.read_human_labels(meta_dir, subset, cs)
        n_sel = 0
        for iid in cand:
            r = rows.get(iid)
            reasons = oi.decide(r, boxes[iid])
            if not reasons and not oi.person_verified(human.get(iid, {})):
                reasons.append("person_unverified")
            if not reasons and limit and n_sel >= limit:
                reasons.append("over_limit")
            r = r or {}
            row = {"uid": "oi:%s:%s" % (subset, iid), "source": "openimages_v7",
                   "source_id": "%s/%s" % (subset, iid), "split": split, "kind": "positive",
                   "licence": "CC BY 2.0" if r.get("License") == oi.CC_BY_20 else r.get("License"),
                   "licence_url": r.get("License"), "licence_tier": None,
                   "author": r.get("Author"), "author_url": r.get("AuthorProfileURL"),
                   "original_url": r.get("OriginalURL"), "landing_url": r.get("OriginalLandingURL"),
                   "title": r.get("Title"), "download_url": oi.image_url(subset, iid),
                   "annotation_source": "Open Images V7 boxes (CC BY 4.0, Google LLC)",
                   "decision": "rejected" if reasons else "selected", "reasons": reasons}
            prov.append(row)
            if reasons:
                continue
            n_sel += 1
            bxs = []
            for b in boxes[iid]:
                c = oi.OI_FOLD.get(b["label"])
                if c is None:
                    continue
                bxs.append({"class_id": names.index(c), "norm": [b["x0"], b["y0"], b["x1"], b["y1"]],
                            "iscrowd": b["group"], "source_label": "oi:" + oi.OI_NAME[b["label"]],
                            "occluded": b["occluded"], "truncated": b["truncated"]})
            records.append(dict(row, raw_path="openimages/%s/%s.jpg" % (subset, iid),
                                file_name="oi/%s.jpg" % iid, expect_size=None, boxes=bxs,
                                flickr_id=flickr_photo_id(r.get("OriginalLandingURL"), r.get("OriginalURL")),
                                human_labels={oi.OI_NAME[k]: v for k, v in sorted(human.get(iid, {}).items())}))
    return records, prov, inputs, outside


# Flickr licences acceptable today under the policy (CC BY, CC0, PDM, no
# known restrictions, US Government). Everything else is contrary evidence.
FLICKR_OK = {4, 7, 8, 9, 10, 11}


def apply_spotcheck(path, records, prov):
    """Reject the sampled images whose current Flickr licence is outside the policy.

    A photo that is gone or unresolved is left as it is: the dataset record is
    then the only evidence, as for every image that was not sampled."""
    js = json.loads(Path(path).read_text())
    bad = {r["uid"]: r["flickr_licence"] for r in js["results"]
           if r["status"] == "ok" and r["flickr_licence_id"] not in FLICKR_OK}
    for p in prov:
        if p["uid"] in bad and p["decision"] == "selected":
            p["decision"] = "rejected"
            p["reasons"] = ["flickr_licence_now_restrictive"]
            p["flickr_licence_now"] = bad[p["uid"]]
    records[:] = [r for r in records if r["uid"] not in bad]
    return {"file": Path(path).name, "sha256_lf": sha256_text_lf(path), "rejected": len(bad)}


# ---------------------------------------------------------------- files

def fetch(records, raw, workers):
    def one(rec):
        p = raw / rec["raw_path"]
        if p.exists() and p.stat().st_size > 0:
            return rec["uid"], None
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix(".part")
        try:
            with urllib.request.urlopen(rec["download_url"], timeout=60) as r, open(tmp, "wb") as f:
                shutil.copyfileobj(r, f)
            os.replace(tmp, p)
            return rec["uid"], None
        except Exception as e:  # noqa: BLE001 - every failure is recorded, none is fatal
            if tmp.exists():
                tmp.unlink()
            return rec["uid"], "%s: %s" % (type(e).__name__, e)

    with ThreadPoolExecutor(workers) as ex:
        return {u: err for u, err in ex.map(one, records) if err}


def inspect_file(path):
    """sha256, decoded size, EXIF orientation, 64-bit dHash, luminance stats."""
    from PIL import Image
    try:
        data = Path(path).read_bytes()
    except OSError as e:
        return {"error": "missing_file: %s" % e}
    out = {"sha256": sha256_bytes(data), "bytes": len(data)}
    try:
        with Image.open(path) as im:
            out["format"] = im.format
            out["size"] = list(im.size)
            try:
                out["exif_orientation"] = im.getexif().get(0x0112)
            except Exception:  # noqa: BLE001
                out["exif_orientation"] = None
            g = im.convert("L")  # full decode: a truncated file fails here
    except Exception as e:  # noqa: BLE001
        return dict(out, error="decode_failed: %s: %s" % (type(e).__name__, e))
    px = g.resize((9, 8), Image.BILINEAR).tobytes()  # 8 rows of 9 grey bytes
    bits = 0
    for y in range(8):
        for x in range(8):
            bits = (bits << 1) | (1 if px[y * 9 + x] > px[y * 9 + x + 1] else 0)
    out["dhash"] = "%016x" % bits
    hist = g.histogram()
    n = sum(hist)
    mean = sum(i * c for i, c in enumerate(hist)) / n
    var = sum(c * (i - mean) ** 2 for i, c in enumerate(hist)) / n
    out["luma_mean"] = round(mean, 2)
    out["luma_std"] = round(var ** 0.5, 2)
    return out


def inspect_all(records, raw, workers):
    paths = [str(raw / r["raw_path"]) for r in records]
    if workers <= 1:
        return dict(zip((r["uid"] for r in records), map(inspect_file, paths)))
    with ProcessPoolExecutor(workers) as ex:
        return dict(zip((r["uid"] for r in records), ex.map(inspect_file, paths, chunksize=64)))


# ---------------------------------------------------------------- annotations

def iou(a, b):
    ax1, ay1, bx1, by1 = a[0] + a[2], a[1] + a[3], b[0] + b[2], b[1] + b[3]
    iw = max(0.0, min(ax1, bx1) - max(a[0], b[0]))
    ih = max(0.0, min(ay1, by1) - max(a[1], b[1]))
    inter = iw * ih
    u = a[2] * a[3] + b[2] * b[3] - inter
    return inter / u if u > 0 else 0.0


DUP_IOU = 0.95


def finalize_boxes(rec, w, h):
    """Pixel boxes, validated; returns (boxes, problems). Problems are per box."""
    out, problems = [], Counter()
    for b in rec["boxes"]:
        if not (isinstance(b["class_id"], int) and 0 <= b["class_id"] < len(class_sets.names(CLASS_SET))):
            problems["invalid_class_id"] += 1
            continue
        if "norm" in b:
            x0, y0, x1, y1 = b["norm"]
            x0, x1, y0, y1 = x0 * w, x1 * w, y0 * h, y1 * h
        else:
            x0, y0 = b["bbox"][0], b["bbox"][1]
            x1, y1 = x0 + b["bbox"][2], y0 + b["bbox"][3]
        if x0 < -1 or y0 < -1 or x1 > w + 1 or y1 > h + 1:
            problems["box_outside_image"] += 1
            continue
        x0, y0, x1, y1 = max(0.0, x0), max(0.0, y0), min(float(w), x1), min(float(h), y1)
        if x1 - x0 < 1.0 or y1 - y0 < 1.0:
            problems["box_under_1px"] += 1
            continue
        nb = {k: v for k, v in b.items() if k not in ("norm", "bbox")}
        nb["bbox"] = [round(x0, 2), round(y0, 2), round(x1 - x0, 2), round(y1 - y0, 2)]
        if any(o["class_id"] == nb["class_id"] and o["iscrowd"] == nb["iscrowd"]
               and iou(o["bbox"], nb["bbox"]) >= DUP_IOU for o in out):
            problems["duplicate_annotation"] += 1
            continue
        out.append(nb)
    return out, problems


# ---------------------------------------------------------------- duplicates

def popcount(x):
    return bin(x).count("1")


def near_duplicate_pairs(items, max_bits):
    """Pairs (uid_a, uid_b, distance) of 64-bit dHashes within max_bits.

    Banding: split the 64 bits into max_bits+1 bands; two hashes within
    max_bits bits agree exactly on at least one band (pigeonhole), so only
    hashes sharing a band value are compared. Exact, not approximate."""
    nb = max_bits + 1
    widths = [64 // nb + (1 if i < 64 % nb else 0) for i in range(nb)]
    hv = [(u, int(h, 16)) for u, h in items]
    pairs = set()
    shift = 64
    for wdt in widths:
        shift -= wdt
        mask = (1 << wdt) - 1
        buckets = defaultdict(list)
        for idx, (_, v) in enumerate(hv):
            buckets[(v >> shift) & mask].append(idx)
        for members in buckets.values():
            if len(members) < 2:
                continue
            for i in range(len(members)):
                for j in range(i + 1, len(members)):
                    a, b = members[i], members[j]
                    d = popcount(hv[a][1] ^ hv[b][1])
                    if d <= max_bits:
                        pairs.add((min(a, b), max(a, b), d))
    return sorted((hv[a][0], hv[b][0], d) for a, b, d in pairs)


def keep_order(rec):
    return (SPLIT_PRIORITY[rec["split"]], 0 if rec["source"] == "coco2017" else 1, rec["uid"])


def resolve_groups(groups, by_uid, reason, quarantine):
    """Each group shares a key (hash, Flickr id); keep the best member."""
    for key, uids in sorted(groups.items()):
        live = sorted((by_uid[u] for u in uids if u in by_uid), key=keep_order)
        if len(live) < 2:
            continue
        keeper = live[0]
        for r in live[1:]:
            quarantine.append({"uid": r["uid"], "split": r["split"], "reason": reason,
                               "key": key, "kept": keeper["uid"], "kept_split": keeper["split"]})
            del by_uid[r["uid"]]


# ---------------------------------------------------------------- output

def link(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        dst.unlink()
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def write_jsonl(path, rows):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for r in rows:
            f.write(json.dumps(r, sort_keys=True, ensure_ascii=False) + "\n")


def manifest_row(rec):
    keys = ("uid", "source", "source_id", "split", "kind", "path", "file_name", "width", "height",
            "sha256", "bytes", "dhash", "exif_orientation", "luma_mean", "luma_std", "licence",
            "licence_url", "author", "author_url", "original_url", "landing_url", "title",
            "flickr_id", "annotation_source", "human_labels", "image_id", "annotations")
    return {k: rec.get(k) for k in keys}


def write_outputs(out, raw, by_uid, prov, quarantine, meta):
    names = class_sets.names(CLASS_SET)
    cats = [{"id": i + 1, "name": n, "supercategory": "person" if n == "person" else "vehicle"}
            for i, n in enumerate(names)]
    final = sorted(by_uid.values(), key=lambda r: r["uid"])
    for i, r in enumerate(final):
        r["image_id"] = i + 1
    ann_id = 0
    split_files = {}
    for split in ("train", "val", "test"):
        recs = [r for r in final if r["split"] == split]
        images, anns = [], []
        for r in recs:
            r["path"] = "%s/%s" % (SPLIT_DIR[split], r["file_name"])
            link(raw / r["raw_path"], out / r["path"])
            images.append({"id": r["image_id"], "file_name": r["file_name"], "width": r["width"],
                           "height": r["height"], "doors_uid": r["uid"]})
            r["annotations"] = []
            for b in r["boxes_px"]:
                ann_id += 1
                a = {"id": ann_id, "image_id": r["image_id"], "category_id": b["class_id"] + 1,
                     "bbox": b["bbox"], "area": round(b["bbox"][2] * b["bbox"][3], 2),
                     "iscrowd": b["iscrowd"]}
                anns.append(a)
                r["annotations"].append({"class_id": b["class_id"], "class_name": names[b["class_id"]],
                                         "bbox": b["bbox"], "iscrowd": b["iscrowd"],
                                         "source_label": b["source_label"], "occluded": b["occluded"],
                                         "truncated": b["truncated"]})
        af = out / "annotations" / ("traffic_%s.json" % split)
        af.parent.mkdir(parents=True, exist_ok=True)
        write_text_lf(af, json.dumps({"info": {"description": "DOORS traffic6 %s split, see manifest.json" % split},
                                      "images": images, "annotations": anns, "categories": cats}, sort_keys=True))
        mf = out / ("manifest_%s.jsonl" % split)
        write_jsonl(mf, [manifest_row(r) for r in recs])
        split_files[split] = {"annotation_file": "annotations/" + af.name,
                              "annotation_sha256": pct.sha256_file(af),
                              "manifest_file": mf.name, "manifest_sha256": pct.sha256_file(mf),
                              "images": len(recs), "boxes": len(anns),
                              "boxes_non_crowd": sum(1 for a in anns if not a["iscrowd"]),
                              "image_list_sha256": sha256_bytes("\n".join(sorted(r["sha256"] for r in recs)).encode())}
    final_uids = set(by_uid)
    for p in prov:
        if p["decision"] == "selected":
            p["decision"] = "included" if p["uid"] in final_uids else "quarantined"
    prov.sort(key=lambda p: p["uid"])
    write_jsonl(out / "provenance.jsonl", prov)
    quarantine.sort(key=lambda q: (q["uid"], q["reason"]))
    write_jsonl(out / "quarantine.jsonl", quarantine)
    with open(out / "ATTRIBUTION.tsv", "w", encoding="utf-8", newline="\n") as f:
        f.write("# Training images of the DOORS Traffic detector. Annotations: COCO Consortium (CC BY 4.0,\n"
                "# https://cocodataset.org/#termsofuse) and Google LLC / Open Images V7 (CC BY 4.0), filtered\n"
                "# and relabelled. Images: Flickr photographers under the licence named per line.\n")
        f.write("uid\tlicence\tlicence_url\tauthor\tauthor_url\tsource_url\n")
        for r in final:
            if r["split"] == "train":
                f.write("\t".join(str(r.get(k) or "-").replace("\t", " ") for k in
                                  ("uid", "licence", "licence_url", "author", "author_url",
                                   "landing_url" if r.get("landing_url") else "original_url")) + "\n")
    with open(out / "test_set.sha256", "w", encoding="utf-8", newline="\n") as f:
        for r in sorted((r for r in final if r["split"] == "test"), key=lambda r: r["sha256"]):
            f.write("%s  %s\n" % (r["sha256"], r["path"]))
    meta["splits"] = split_files
    for k in ("provenance.jsonl", "quarantine.jsonl", "ATTRIBUTION.tsv", "test_set.sha256"):
        meta.setdefault("files", {})[k] = pct.sha256_file(out / k)
    write_text_lf(out / "manifest.json", json.dumps(meta, indent=1, sort_keys=True) + "\n")


# ---------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--coco-ann", required=True)
    ap.add_argument("--oi-meta")
    ap.add_argument("--raw", required=True, help="download cache: RAW/coco/..., RAW/openimages/...")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=20261004)
    ap.add_argument("--neg-ratio", type=float, default=0.15)
    ap.add_argument("--exclude-sha256")
    ap.add_argument("--near-dup-bits", type=int, default=4)
    ap.add_argument("--workers", type=int, default=16)
    ap.add_argument("--limit", type=int, default=0, help="per source split, for smoke tests only")
    ap.add_argument("--no-download", action="store_true", help="select and report candidates only")
    ap.add_argument("--no-openimages", action="store_true")
    ap.add_argument("--licence-spotcheck", help="provenance/flickr_spotcheck_*.json: reject images whose "
                    "licence on Flickr today is outside the policy")
    args = ap.parse_args(argv)
    if not args.no_openimages and not args.oi_meta:
        ap.error("--oi-meta is required unless --no-openimages")

    out, raw = Path(args.out), Path(args.raw)
    names = class_sets.names(CLASS_SET)
    print("selecting COCO ...", flush=True)
    records, prov, inputs = select_coco(args.coco_ann, args.seed, args.neg_ratio, args.limit)
    oi_outside = {}
    if not args.no_openimages:
        print("selecting Open Images ...", flush=True)
        r2, p2, i2, oi_outside = select_openimages(args.oi_meta, args.limit)
        records += r2
        prov += p2
        inputs.update(i2)
    spot = apply_spotcheck(args.licence_spotcheck, records, prov) if args.licence_spotcheck else None
    sel = Counter((r["source"], r["split"]) for r in records)
    print("selected:", dict(sorted(("%s/%s" % k, v) for k, v in sel.items())), flush=True)
    if args.no_download:
        out.mkdir(parents=True, exist_ok=True)
        write_jsonl(out / "provenance_candidates.jsonl", sorted(prov, key=lambda p: p["uid"]))
        write_text_lf(out / "selection.json", json.dumps({
            "selected": {"%s/%s" % k: v for k, v in sorted(sel.items())},
            "rejected_reasons": dict(Counter(x for p in prov for x in p["reasons"])),
            "openimages_outside_rule": oi_outside}, indent=1) + "\n")
        return 0

    quarantine = []
    print("fetching %d images ..." % len(records), flush=True)
    failed = fetch(records, raw, max(4, args.workers * 2))
    print("inspecting files ...", flush=True)
    info = inspect_all([r for r in records if r["uid"] not in failed], raw, args.workers)
    excluded = set()
    if args.exclude_sha256:
        for line in Path(args.exclude_sha256).read_text().splitlines():
            if line.strip() and not line.startswith("#"):
                excluded.add(line.split()[0].lower())

    by_uid = {}
    box_problems = Counter()
    for r in records:
        u = r["uid"]
        if u in failed:
            quarantine.append({"uid": u, "split": r["split"], "reason": "download_failed", "detail": failed[u]})
            continue
        fi = info[u]
        if "error" in fi:
            quarantine.append({"uid": u, "split": r["split"], "reason": fi["error"].split(":")[0],
                               "detail": fi["error"]})
            continue
        if fi["sha256"] in excluded:
            quarantine.append({"uid": u, "split": r["split"], "reason": "on_exclusion_list"})
            continue
        if fi["exif_orientation"] not in (None, 1):
            # cv2.imread (YOLOX) applies EXIF orientation, the boxes do not
            quarantine.append({"uid": u, "split": r["split"], "reason": "exif_orientation",
                               "detail": fi["exif_orientation"]})
            continue
        w, h = fi["size"]
        if r["expect_size"] and r["expect_size"] != [w, h]:
            quarantine.append({"uid": u, "split": r["split"], "reason": "size_mismatch",
                               "detail": [r["expect_size"], [w, h]]})
            continue
        boxes, probs = finalize_boxes(r, w, h)
        box_problems.update(probs)
        if probs:
            quarantine.append({"uid": u, "split": r["split"], "reason": "annotation_fixed",
                               "detail": dict(probs), "image_kept": True})
        if r["kind"] == "positive" and not any(not b["iscrowd"] for b in boxes):
            quarantine.append({"uid": u, "split": r["split"], "reason": "no_valid_target_box"})
            continue
        r.update(width=w, height=h, sha256=fi["sha256"], bytes=fi["bytes"], dhash=fi["dhash"],
                 exif_orientation=fi["exif_orientation"], luma_mean=fi["luma_mean"],
                 luma_std=fi["luma_std"], boxes_px=boxes)
        by_uid[u] = r

    # exact duplicates and the same Flickr photo across datasets
    by_hash, by_flickr = defaultdict(list), defaultdict(list)
    for r in by_uid.values():
        by_hash[r["sha256"]].append(r["uid"])
        if r.get("flickr_id"):
            by_flickr[r["flickr_id"]].append(r["uid"])
    resolve_groups(by_hash, by_uid, "exact_duplicate", quarantine)
    resolve_groups(by_flickr, by_uid, "same_flickr_photo", quarantine)

    # near duplicates: across splits they are removed (test > val > train keeps);
    # inside a split they are only reported
    pairs = near_duplicate_pairs(sorted((u, r["dhash"]) for u, r in by_uid.items()), args.near_dup_bits)
    within = []
    for a, b, d in pairs:
        if a not in by_uid or b not in by_uid:
            continue
        ra, rb = by_uid[a], by_uid[b]
        if ra["split"] == rb["split"]:
            within.append({"a": a, "b": b, "bits": d, "split": ra["split"]})
            continue
        keeper, loser = sorted((ra, rb), key=keep_order)
        quarantine.append({"uid": loser["uid"], "split": loser["split"], "reason": "near_duplicate_cross_split",
                           "key": "dhash<=%d (%d)" % (args.near_dup_bits, d), "kept": keeper["uid"],
                           "kept_split": keeper["split"]})
        del by_uid[loser["uid"]]

    from PIL import __version__ as pil_version
    meta = {
        "format": FORMAT,
        "class_set": CLASS_SET,
        "classes": names,
        "doors_index": class_sets.doors_indices(CLASS_SET),
        "class_mapping": [{"class_id": i, "category_id": i + 1, "name": n,
                           "doors_index": class_sets.DOORS_INDEX[n],
                           "coco_category_id": class_sets.COCO_CATEGORY_ID[n],
                           "openimages_labels": sorted(oi.OI_NAME[k] for k, v in oi.OI_FOLD.items() if v == n)}
                          for i, n in enumerate(names)],
        "seed": args.seed,
        "rules": {
            "coco_licence_policy": {"file": "provenance/coco_licence_policy.json",
                                    "sha256_lf": sha256_text_lf(pct.POLICY), "tiers": ["allow"]},
            "coco_negatives": "street-context images without a target, ratio %.2f of positives, seeded" % args.neg_ratio,
            "coco_split": "h = first 8 hex of sha256('doors-split:SEED:coco-SRC:ID'); val2017: h mod 2 = 0 val, "
                          "1 test; train2017: h mod 20 = 0-1 test, 2 val, else train",
            "openimages": {"licence": oi.CC_BY_20, "selection": "a box of %s" % sorted(oi.SELECT_CLASSES),
                           "reject_reasons": oi.REASONS,
                           "person_unverified": "Person (or Man/Woman/Boy/Girl) never human-verified in the image",
                           "group_of": "IsGroupOf boxes kept as iscrowd=1 (YOLOX ignores crowd boxes)",
                           "split": "official subsets: train->train, validation->val, test->test"},
            "file_checks": "full decode (Pillow), EXIF orientation must be absent or 1, COCO size must match",
            "box_checks": "class id 0..5, inside the image (+-1 px, then clipped), >= 1 px, same-class "
                          "IoU >= %.2f duplicate dropped" % DUP_IOU,
            "exact_duplicates": "same sha256 or same Flickr photo id: one kept, by split test>val>train, then COCO>OI, then uid",
            "near_duplicates": "64-bit dHash (9x8 grey, Pillow BILINEAR), Hamming <= %d; across splits the "
                               "lower-priority copy is removed, inside a split reported only" % args.near_dup_bits,
            "exclusion_list": {"file": args.exclude_sha256, "entries": len(excluded),
                               "sha256": pct.sha256_file(args.exclude_sha256) if args.exclude_sha256 else None},
            "limit": args.limit,
            "licence_spotcheck": spot,
        },
        "inputs": inputs,
        "tools": {"python": platform.python_version(), "pillow": pil_version},
        "command": "build_traffic_dataset.py " + " ".join(
            a if not os.path.isabs(a) else "<%s>" % Path(a).name for a in (argv or sys.argv[1:])),
        "counts": {
            "selected": {"%s/%s" % k: v for k, v in sorted(sel.items())},
            "quarantined": dict(Counter(q["reason"] for q in quarantine if not q.get("image_kept"))),
            "box_problems": dict(box_problems),
            "near_duplicates_within_split": len(within),
            "openimages_outside_rule": oi_outside,
        },
    }
    out.mkdir(parents=True, exist_ok=True)
    write_jsonl(out / "near_duplicates_within_split.jsonl", within)
    meta["files"] = {"near_duplicates_within_split.jsonl": pct.sha256_file(out / "near_duplicates_within_split.jsonl")}
    write_outputs(out, raw, by_uid, prov, quarantine, meta)
    errors = dataset_checks.check_dataset(out)
    print(json.dumps({s: {k: v for k, v in d.items() if k in ("images", "boxes")}
                      for s, d in meta["splits"].items()}))
    if errors:
        print("DATASET CHECK FAILED:\n  " + "\n  ".join(errors[:50]))
        return 1
    print("dataset checks: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
