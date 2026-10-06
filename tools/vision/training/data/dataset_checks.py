"""Consistency, provenance and leakage checks of a built traffic6 dataset.

check_dataset(OUT) returns a list of error strings; empty means the dataset
passes. build_traffic_dataset.py runs it at the end and fails on any error;
audit_traffic_dataset.py runs it again (with --verify-files, re-hashing
every image) before it reports.
"""
import hashlib
import json
from collections import defaultdict
from pathlib import Path

import class_sets
import openimages_traffic as oi

SPLITS = ("train", "val", "test")
REQUIRED = ("uid", "source", "source_id", "split", "path", "file_name", "width", "height", "sha256",
            "licence", "licence_url", "original_url", "annotation_source", "image_id", "annotations")
DUP_IOU = 0.95
POLICY = Path(__file__).resolve().parent.parent / "provenance" / "coco_licence_policy.json"


def _sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _iou(a, b):
    iw = max(0.0, min(a[0] + a[2], b[0] + b[2]) - max(a[0], b[0]))
    ih = max(0.0, min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1]))
    u = a[2] * a[3] + b[2] * b[3] - iw * ih
    return iw * ih / u if u > 0 else 0.0


def read_jsonl(p):
    rows = []
    with open(p, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            if line.strip():
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError as e:
                    raise ValueError("%s line %d: %s" % (p, n, e))
    return rows


def check_dataset(out, verify_files=False):
    out = Path(out)
    err = []
    names = class_sets.names("traffic6")
    try:
        meta = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    except Exception as e:  # noqa: BLE001
        return ["manifest.json unreadable: %s" % e]

    # class mapping: one explicit table, used everywhere
    if meta.get("class_set") != "traffic6" or meta.get("classes") != names:
        err.append("class set/classes %s %s, expected traffic6 %s" % (meta.get("class_set"), meta.get("classes"), names))
    for i, m in enumerate(meta.get("class_mapping", [])):
        exp = {"class_id": i, "category_id": i + 1, "name": names[i], "doors_index": class_sets.DOORS_INDEX[names[i]],
               "coco_category_id": class_sets.COCO_CATEGORY_ID[names[i]]}
        if any(m.get(k) != v for k, v in exp.items()):
            err.append("class_mapping[%d] = %s, expected %s" % (i, m, exp))
    if meta.get("doors_index") != class_sets.doors_indices("traffic6"):
        err.append("doors_index %s" % meta.get("doors_index"))

    policy = json.loads(POLICY.read_text())
    coco_allowed = {l["url"] for l in policy["licences"] if l["tier"] == "allow"}

    try:
        prov = {p["uid"]: p for p in read_jsonl(out / "provenance.jsonl")}
    except Exception as e:  # noqa: BLE001
        prov = {}
        err.append("provenance.jsonl: %s" % e)

    seen = {"uid": {}, "source_id": {}, "sha256": {}, "flickr_id": {}}
    for split in SPLITS:
        sp = meta.get("splits", {}).get(split)
        if not sp:
            err.append("split %s missing in manifest.json" % split)
            continue
        mf = out / sp["manifest_file"]
        af = out / sp["annotation_file"]
        for f, key in ((mf, "manifest_sha256"), (af, "annotation_sha256")):
            if not f.exists():
                err.append("%s missing" % f.name)
            elif _sha(f) != sp[key]:
                err.append("%s sha256 differs from manifest.json" % f.name)
        try:
            rows = read_jsonl(mf)
            coco = json.loads(af.read_text(encoding="utf-8"))
        except Exception as e:  # noqa: BLE001
            err.append("%s: unreadable: %s" % (split, e))
            continue
        if len(rows) != sp["images"]:
            err.append("%s: %d manifest rows, manifest.json says %d" % (split, len(rows), sp["images"]))
        cats = [(c["id"], c["name"]) for c in coco["categories"]]
        if cats != [(i + 1, n) for i, n in enumerate(names)]:
            err.append("%s: categories %s" % (split, cats))
        imgs = {i["id"]: i for i in coco["images"]}
        anns = defaultdict(list)
        for a in coco["annotations"]:
            anns[a["image_id"]].append(a)
            if a["image_id"] not in imgs:
                err.append("%s: annotation %d on unknown image %d" % (split, a["id"], a["image_id"]))
        if len(imgs) != len(rows):
            err.append("%s: %d images in json, %d in manifest" % (split, len(imgs), len(rows)))

        for r in rows:
            uid = r.get("uid", "?")
            for k in REQUIRED:
                if r.get(k) in (None, ""):
                    err.append("%s %s: field %s missing" % (split, uid, k))
            if r.get("split") != split:
                err.append("%s %s: row says split %s" % (split, uid, r.get("split")))
            # provenance and licence
            if r.get("source") == "coco2017":
                if r.get("licence_url") not in coco_allowed:
                    err.append("%s %s: COCO licence %s not in the allow tier" % (split, uid, r.get("licence_url")))
            elif r.get("source") == "openimages_v7":
                if r.get("licence_url") != oi.CC_BY_20:
                    err.append("%s %s: Open Images licence %s" % (split, uid, r.get("licence_url")))
                if not r.get("author") or not r.get("landing_url"):
                    err.append("%s %s: Open Images attribution missing" % (split, uid))
            else:
                err.append("%s %s: unknown source %s" % (split, uid, r.get("source")))
            p = prov.get(uid)
            if p is None:
                err.append("%s %s: no provenance record" % (split, uid))
            elif p.get("decision") != "included":
                err.append("%s %s: provenance decision is %s" % (split, uid, p.get("decision")))
            # leakage keys
            for k in seen:
                v = r.get(k)
                if v is None:
                    continue
                if v in seen[k]:
                    err.append("%s %s: %s %s also in %s" % (split, uid, k, v, seen[k][v]))
                else:
                    seen[k][v] = "%s %s" % (split, uid)
            # file
            fp = out / (r.get("path") or "")
            if not fp.is_file():
                err.append("%s %s: file missing %s" % (split, uid, r.get("path")))
            elif verify_files and _sha(fp) != r.get("sha256"):
                err.append("%s %s: file sha256 differs" % (split, uid))
            # annotations
            im = imgs.get(r.get("image_id"))
            if im is None:
                err.append("%s %s: image_id %s not in json" % (split, uid, r.get("image_id")))
                continue
            if (im["file_name"], im["width"], im["height"]) != (r.get("file_name"), r.get("width"), r.get("height")):
                err.append("%s %s: json image record differs" % (split, uid))
            ja = anns.get(im["id"], [])
            if len(ja) != len(r.get("annotations") or []):
                err.append("%s %s: %d json boxes, %d manifest boxes" % (split, uid, len(ja), len(r.get("annotations") or [])))
            for a, m in zip(ja, r.get("annotations") or []):
                if m.get("class_id") != a["category_id"] - 1 or m.get("class_name") != names[a["category_id"] - 1]:
                    err.append("%s %s: class mapping differs (json %s, manifest %s/%s)"
                               % (split, uid, a["category_id"], m.get("class_id"), m.get("class_name")))
            for a in ja:
                x, y, w, h = a["bbox"]
                if not (1 <= a["category_id"] <= len(names)):
                    err.append("%s %s: invalid category_id %s" % (split, uid, a["category_id"]))
                if w < 1 or h < 1 or x < 0 or y < 0 or x + w > im["width"] + 0.01 or y + h > im["height"] + 0.01:
                    err.append("%s %s: bad box %s" % (split, uid, a["bbox"]))
            for i in range(len(ja)):
                for j in range(i + 1, len(ja)):
                    a, b = ja[i], ja[j]
                    if (a["category_id"] == b["category_id"] and a["iscrowd"] == b["iscrowd"]
                            and _iou(a["bbox"], b["bbox"]) >= DUP_IOU):
                        err.append("%s %s: duplicate annotation %d/%d" % (split, uid, a["id"], b["id"]))
            if r.get("kind") == "positive" and not any(not a["iscrowd"] for a in ja):
                err.append("%s %s: positive image without a trainable box" % (split, uid))

    included = {u for u, p in prov.items() if p.get("decision") == "included"}
    missing = included - set(seen["uid"])
    if missing:
        err.append("%d provenance records say included but are in no manifest, e.g. %s"
                   % (len(missing), sorted(missing)[:3]))
    return err
