#!/usr/bin/env python3
"""Build the DOORS Traffic training set from COCO 2017, licence-filtered.

Reads COCO's own instances_train2017.json / instances_val2017.json, keeps only
images whose per-image licence is allowed by provenance/coco_licence_policy.json,
keeps only the class set's categories (folded and relabelled), adds a share of
street-context negatives (no target vehicle, but street furniture or road
users), fetches exactly those images from COCO's image host, hashes every
file, drops any file listed in an exclusion hash list (the DOORS evaluation
and test sets), and writes a YOLOX-ready tree:

  OUT/annotations/traffic_train.json   COCO format, categories 1..N in label order
  OUT/annotations/traffic_val.json
  OUT/train2017/*.jpg                  from COCO train2017
  OUT/val2017/*.jpg                    from COCO val2017 (validation, never trained on)
  OUT/manifest.json                    inputs + their sha256, policy, class set, seed, counts
  OUT/images.tsv                       one line per image: split, file, sha256, licence, source URLs
  OUT/ATTRIBUTION.tsv                  the attribution record for the CC BY images
  OUT/stats.json                       class counts, box sizes at 416, negatives, licences

Splits: COCO train2017 -> train, COCO val2017 -> val. The test set is not
COCO: it is the DOORS camera set (docs/vision/DATASET_PROVENANCE.md), and
--exclude-sha256 makes sure none of its files is in train or val.

Usage:
  prepare_coco_traffic.py --coco-ann DIR --out DIR [--class-set traffic4]
      [--allow-review] [--neg-ratio 0.15] [--seed 20261004]
      [--limit-train N] [--limit-val N] [--exclude-sha256 FILE] [--no-download]
"""
import argparse
import hashlib
import json
import os
import random
import sys
import urllib.request
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import class_sets  # noqa: E402

POLICY = HERE.parent / "provenance" / "coco_licence_policy.json"

# Present in a picture without a target vehicle, these make it a street-like
# negative: the model must learn that a road without cars is empty.
STREET_CONTEXT = {"traffic light", "stop sign", "parking meter", "fire hydrant", "bench",
                  "bicycle", "person"}


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_policy(allow_review):
    pol = json.loads(POLICY.read_text())
    ok = {"allow"} | ({"review"} if allow_review else set())
    return pol, {l["url"]: l for l in pol["licences"]}, ok


def select(coco, class_set, lic_by_url, ok_tiers, neg_ratio, rng, limit):
    """Return (images, annotations, report) for one COCO split."""
    cat_by_name = {c["name"]: c for c in coco["categories"]}
    for name, cid in class_sets.COCO_CATEGORY_ID.items():
        if cat_by_name[name]["id"] != cid:
            raise SystemExit("COCO category id of %s is %d, expected %d"
                             % (name, cat_by_name[name]["id"], cid))
    fold = class_sets.coco_fold(class_set)  # coco name -> label
    label_of_cat = {cat_by_name[n]["id"]: l for n, l in fold.items()}
    ctx_cats = {cat_by_name[n]["id"] for n in STREET_CONTEXT}

    lic_tier = {}
    unknown = []
    for l in coco["licenses"]:
        p = lic_by_url.get(l["url"])
        if p is None:
            unknown.append(l["url"])
            lic_tier[l["id"]] = ("exclude", l["url"], l["name"])
        else:
            lic_tier[l["id"]] = (p["tier"], l["url"], p["name"])

    anns_by_img = {}
    for a in coco["annotations"]:
        anns_by_img.setdefault(a["image_id"], []).append(a)

    lic_count = Counter()
    pos, neg = [], []
    for im in sorted(coco["images"], key=lambda i: i["id"]):
        tier = lic_tier[im["license"]][0]
        lic_count[(lic_tier[im["license"]][2], tier)] += 1
        if tier not in ok_tiers:
            continue
        anns = anns_by_img.get(im["id"], [])
        if any(a["category_id"] in label_of_cat for a in anns):
            pos.append(im)
        elif any(a["category_id"] in ctx_cats for a in anns):
            neg.append(im)

    rng.shuffle(pos)
    rng.shuffle(neg)
    eligible = len(pos)
    if limit:
        pos = pos[:limit]
    n_neg = min(len(neg), int(round(len(pos) * neg_ratio)))
    chosen = sorted(pos + neg[:n_neg], key=lambda i: i["id"])

    images, annotations = [], []
    for im in chosen:
        images.append({k: im[k] for k in ("id", "file_name", "width", "height", "license",
                                          "coco_url", "flickr_url")})
        for a in anns_by_img.get(im["id"], []):
            if a["category_id"] in label_of_cat:
                annotations.append({"id": a["id"], "image_id": a["image_id"], "bbox": a["bbox"],
                                    "area": a["area"], "iscrowd": a["iscrowd"],
                                    "category_id": label_of_cat[a["category_id"]] + 1,
                                    "coco_category_id": a["category_id"]})
    report = {
        "licence_table": {"%s [%s]" % k: v for k, v in sorted(lic_count.items())},
        "unknown_licence_urls": unknown,
        "eligible_positive": eligible,
        "eligible_negative": len(neg),
        "positive": len(pos),
        "negative": n_neg,
        "licences_in_table": {str(k): v[1] for k, v in sorted(lic_tier.items())},
    }
    return images, annotations, report


def fetch(images, dest, workers=8):
    dest.mkdir(parents=True, exist_ok=True)

    def one(im):
        p = dest / im["file_name"]
        if not p.exists() or p.stat().st_size == 0:
            tmp = p.with_suffix(".part")
            with urllib.request.urlopen(im["coco_url"], timeout=60) as r, open(tmp, "wb") as f:
                f.write(r.read())
            tmp.rename(p)
        return im["id"], sha256_file(p)

    with ThreadPoolExecutor(workers) as ex:
        return dict(ex.map(one, images))


def box_stats(images, annotations, n):
    """Box width in model pixels after the 416 letterbox, per class."""
    size = {i["id"]: (i["width"], i["height"]) for i in images}
    edges = [8, 16, 32, 64, 128]
    out = {}
    for a in annotations:
        if a["iscrowd"]:
            continue
        w, h = size[a["image_id"]]
        bw = a["bbox"][2] * 416.0 / max(w, h)
        b = next((("<%d" % e) for e in edges if bw < e), ">=128")
        out.setdefault(a["category_id"] - 1, Counter())[b] += 1
    return {class_sets.names(n)[k]: dict(v) for k, v in sorted(out.items())}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--coco-ann", required=True, help="dir with instances_{train,val}2017.json")
    ap.add_argument("--out", required=True)
    ap.add_argument("--class-set", default="traffic4", choices=sorted(class_sets.CLASS_SETS))
    ap.add_argument("--allow-review", action="store_true", help="also use 'review' licences (owner decision)")
    ap.add_argument("--neg-ratio", type=float, default=0.15)
    ap.add_argument("--seed", type=int, default=20261004)
    ap.add_argument("--limit-train", type=int, default=0, help="positives cap, for smoke runs")
    ap.add_argument("--limit-val", type=int, default=0)
    ap.add_argument("--exclude-sha256", help="file of sha256 hashes (first field per line) never to use")
    ap.add_argument("--no-download", action="store_true", help="select and report only")
    args = ap.parse_args()

    out = Path(args.out)
    ann_dir = Path(args.coco_ann)
    pol, lic_by_url, ok_tiers = load_policy(args.allow_review)
    excluded = set()
    if args.exclude_sha256:
        for line in Path(args.exclude_sha256).read_text().splitlines():
            if line.strip() and not line.startswith("#"):
                excluded.add(line.split()[0].lower())

    manifest = {
        "format": "doors-traffic-dataset-v1",
        "class_set": args.class_set,
        "classes": class_sets.names(args.class_set),
        "doors_index": class_sets.doors_indices(args.class_set),
        "coco_fold": {c[0]: c[1] for c in class_sets.CLASS_SETS[args.class_set]},
        "seed": args.seed,
        "neg_ratio": args.neg_ratio,
        "allowed_tiers": sorted(ok_tiers),
        "licence_policy": {"file": "provenance/coco_licence_policy.json",
                           "sha256": sha256_file(POLICY), "version": pol["policy_version"]},
        "limits": {"train": args.limit_train, "val": args.limit_val},
        "exclusion_list": {"file": args.exclude_sha256,
                           "sha256": sha256_file(args.exclude_sha256) if args.exclude_sha256 else None,
                           "entries": len(excluded)},
        "inputs": {},
        "splits": {},
    }
    rng = random.Random(args.seed)
    stats = {}
    rows = []
    for split, src, limit in (("train", "train2017", args.limit_train), ("val", "val2017", args.limit_val)):
        ann_file = ann_dir / ("instances_%s.json" % src)
        manifest["inputs"][ann_file.name] = sha256_file(ann_file)
        coco = json.loads(ann_file.read_text())
        images, annotations, report = select(coco, args.class_set, lic_by_url, ok_tiers,
                                             args.neg_ratio, rng, limit)
        print("%s: %d images (%d positive, %d negative), %d boxes"
              % (split, len(images), report["positive"], report["negative"], len(annotations)))
        if report["unknown_licence_urls"]:
            print("  WARNING: licence URLs not in the policy (excluded):", report["unknown_licence_urls"])
        dropped = []
        if not args.no_download:
            hashes = fetch(images, out / src)
            keep = []
            for im in images:
                h = hashes[im["id"]]
                if h in excluded:
                    dropped.append(im["file_name"])
                    (out / src / im["file_name"]).unlink()
                    continue
                im["sha256"] = h
                keep.append(im)
            if dropped:
                print("  dropped %d image(s) on the exclusion list" % len(dropped))
            ids = {i["id"] for i in keep}
            images = keep
            annotations = [a for a in annotations if a["image_id"] in ids]
        lic_name = {l["id"]: (l["url"], lic_by_url.get(l["url"], {}).get("name", l["name"]))
                    for l in coco["licenses"]}
        for im in images:
            url, name = lic_name[im["license"]]
            rows.append((split, src + "/" + im["file_name"], im.get("sha256", "-"), name, url,
                         im["flickr_url"], im["coco_url"]))
        cats = [{"id": i + 1, "name": n, "supercategory": "vehicle"}
                for i, n in enumerate(class_sets.names(args.class_set))]
        (out / "annotations").mkdir(parents=True, exist_ok=True)
        js = {"info": {"description": "DOORS Traffic subset of COCO 2017 (%s), see manifest.json" % src},
              "licenses": coco["licenses"], "images": images, "annotations": annotations,
              "categories": cats}
        af = out / "annotations" / ("traffic_%s.json" % split)
        af.write_text(json.dumps(js))
        manifest["splits"][split] = {"annotation_file": "annotations/" + af.name,
                                     "annotation_sha256": sha256_file(af),
                                     "images": len(images), "boxes": len(annotations),
                                     "positive": report["positive"], "negative": report["negative"],
                                     "eligible_positive": report["eligible_positive"],
                                     "eligible_negative": report["eligible_negative"],
                                     "excluded_by_hash": dropped,
                                     "image_list_sha256": hashlib.sha256(
                                         "\n".join(sorted(i.get("sha256", i["file_name"]) for i in images))
                                         .encode()).hexdigest()}
        stats[split] = {"licences_in_coco_split": report["licence_table"],
                        "box_width_at_416": box_stats(images, annotations, args.class_set),
                        "boxes_per_class": dict(Counter(class_sets.names(args.class_set)[a["category_id"] - 1]
                                                        for a in annotations)),
                        "images_by_licence": dict(Counter(lic_name[i["license"]][1] for i in images))}

    with open(out / "images.tsv", "w") as f:
        f.write("split\tfile\tsha256\tlicence\tlicence_url\tflickr_url\tcoco_url\n")
        for r in rows:
            f.write("\t".join(r) + "\n")
    with open(out / "ATTRIBUTION.tsv", "w") as f:
        f.write("# Images used to train the DOORS Traffic detector. Annotations: COCO Consortium, CC BY 4.0\n"
                "# (https://cocodataset.org/#termsofuse). Images: Flickr photographers under the licence\n"
                "# named per line; COCO does not record author names, the Flickr URL identifies the photo.\n")
        f.write("file\tlicence\tlicence_url\tflickr_url\n")
        for r in rows:
            if r[0] == "train":
                f.write("\t".join((r[1], r[3], r[4], r[5])) + "\n")
    manifest["images_tsv_sha256"] = sha256_file(out / "images.tsv")
    (out / "stats.json").write_text(json.dumps(stats, indent=1))
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1))
    print(json.dumps(stats, indent=1))


if __name__ == "__main__":
    main()
