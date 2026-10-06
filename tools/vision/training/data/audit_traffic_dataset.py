#!/usr/bin/env python3
"""Audit a dataset built by build_traffic_dataset.py.

Runs dataset_checks (optionally re-hashing every file), then measures what
the data covers and writes:

  OUT/stats.json          machine-readable summary
  OUT/DATASET_REPORT.md   the same numbers as tables

Size convention (documented in the report, not COCO's):
  side416 = sqrt(w * h) * 416 / max(W, H)
  the box's geometric-mean side in model pixels after YOLOX's 416 letterbox.
  very_small < 8 px   smaller than one cell of YOLOX's finest output (stride 8)
  small      8-16 px  one to two stride-8 cells
  medium    16-64 px
  large      >= 64 px two or more stride-32 cells
  Normalised area (w*h / (W*H), percent) is reported beside it.

Distance is not measured anywhere: no source has distance labels. Small
boxes are a proxy for distant vehicles, nothing more.

Usage: audit_traffic_dataset.py OUT [--verify-files] [--raw RAW] [--meta DIR]
"""
import argparse
import json
import math
import os
import sys
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import class_sets  # noqa: E402
import dataset_checks  # noqa: E402

SPLITS = ("train", "val", "test")
SIZE_EDGES = [(8, "very_small"), (16, "small"), (64, "medium")]
AREA_BINS = [0.01, 0.05, 0.1, 0.5, 1, 5, 10, 25]  # percent of image area
SIDE_BINS = [4, 8, 16, 32, 64, 128, 256]
VEHICLES = ("car", "truck", "bus", "motorcycle")
DARK_LUMA = 60.0       # mean grey level below which a picture is counted as dark
LOW_CONTRAST_STD = 30.0  # grey std below which a picture is counted as low-contrast (haze, fog, glare)


def size_class(side):
    for edge, name in SIZE_EDGES:
        if side < edge:
            return name
    return "large"


def bin_label(v, edges, unit=""):
    lo = None
    for e in edges:
        if v < e:
            return ("<%s%s" % (e, unit)) if lo is None else ("%s-%s%s" % (lo, e, unit))
        lo = e
    return ">=%s%s" % (edges[-1], unit)


def bin_order(edges, unit=""):
    out, lo = [], None
    for e in edges:
        out.append(("<%s%s" % (e, unit)) if lo is None else ("%s-%s%s" % (lo, e, unit)))
        lo = e
    return out + [">=%s%s" % (edges[-1], unit)]


def pct(vals, ps=(5, 25, 50, 75, 95)):
    if not vals:
        return {}
    s = sorted(vals)
    return {"p%d" % p: round(s[min(len(s) - 1, int(math.floor(p / 100.0 * len(s))))], 3) for p in ps}


def du(path):
    tot, n = 0, 0
    for root, _, files in os.walk(path):
        for f in files:
            tot += os.path.getsize(os.path.join(root, f))
            n += 1
    return tot, n


def audit(out, raw=None, meta_dir=None, verify_files=False):
    out = Path(out)
    names = class_sets.names("traffic6")
    errors = dataset_checks.check_dataset(out, verify_files=verify_files)
    man = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    rows = {s: dataset_checks.read_jsonl(out / ("manifest_%s.jsonl" % s)) for s in SPLITS}
    prov = dataset_checks.read_jsonl(out / "provenance.jsonl")
    quar = dataset_checks.read_jsonl(out / "quarantine.jsonl")
    within = dataset_checks.read_jsonl(out / "near_duplicates_within_split.jsonl")

    st = {"dataset": str(out.name), "format": man["format"], "checks": {"errors": len(errors),
          "first_errors": errors[:20], "files_rehashed": verify_files},
          "class_mapping": man["class_mapping"],
          "size_convention": {"side416": "sqrt(w*h)*416/max(W,H)", "very_small": "<8", "small": "8-16",
                              "medium": "16-64", "large": ">=64",
                              "area_pct": "100*w*h/(W*H)"}}

    # ---- totals
    tot = {}
    for s in SPLITS:
        rs = rows[s]
        tot[s] = {"images": len(rs),
                  "positive_images": sum(1 for r in rs if r["kind"] == "positive"),
                  "negative_images": sum(1 for r in rs if not r["annotations"]),
                  "boxes_trainable": sum(1 for r in rs for a in r["annotations"] if not a["iscrowd"]),
                  "boxes_crowd_ignored": sum(1 for r in rs for a in r["annotations"] if a["iscrowd"]),
                  "by_source": dict(Counter(r["source"] for r in rs))}
    st["totals"] = tot

    # ---- per class
    pc = {}
    all_boxes = sum(t["boxes_trainable"] for t in tot.values())
    for ci, n in enumerate(names):
        d = {"images": {}, "boxes": {}, "crowd_boxes": {}, "boxes_by_source": Counter()}
        for s in SPLITS:
            d["images"][s] = sum(1 for r in rows[s] if any(a["class_id"] == ci and not a["iscrowd"] for a in r["annotations"]))
            d["boxes"][s] = sum(1 for r in rows[s] for a in r["annotations"] if a["class_id"] == ci and not a["iscrowd"])
            d["crowd_boxes"][s] = sum(1 for r in rows[s] for a in r["annotations"] if a["class_id"] == ci and a["iscrowd"])
            for r in rows[s]:
                for a in r["annotations"]:
                    if a["class_id"] == ci and not a["iscrowd"]:
                        d["boxes_by_source"]["%s/%s" % (r["source"], s)] += 1
        d["images_total"] = sum(d["images"].values())
        d["boxes_total"] = sum(d["boxes"].values())
        d["pct_of_all_boxes"] = round(100.0 * d["boxes_total"] / all_boxes, 2) if all_boxes else 0
        d["split_share_pct"] = {s: round(100.0 * d["boxes"][s] / d["boxes_total"], 1) if d["boxes_total"] else 0
                                for s in SPLITS}
        d["boxes_by_source"] = dict(sorted(d["boxes_by_source"].items()))
        pc[n] = d
    st["per_class"] = pc
    tr = {n: pc[n]["boxes"]["train"] for n in names}
    st["balance"] = {"train_boxes": tr,
                     "max_over_min": round(max(tr.values()) / max(1, min(tr.values())), 1),
                     "vehicles_train_boxes": {n: tr[n] for n in VEHICLES},
                     "vehicle_max_over_min": round(max(tr[n] for n in VEHICLES) / max(1, min(tr[n] for n in VEHICLES)), 1)}

    # ---- object size
    size = {}
    for s in SPLITS:
        per = defaultdict(lambda: {"size_class": Counter(), "area_pct_bins": Counter(), "side416": [],
                                   "w416": [], "h416": [], "area_pct": [], "w416_bins": Counter(),
                                   "h416_bins": Counter(), "truncated_flag": Counter(),
                                   "occluded_flag": Counter(), "touches_border": 0})
        for r in rows[s]:
            W, H = r["width"], r["height"]
            k = 416.0 / max(W, H)
            for a in r["annotations"]:
                if a["iscrowd"]:
                    continue
                x, y, w, h = a["bbox"]
                side = math.sqrt(w * h) * k
                ap = 100.0 * w * h / (W * H)
                d = per[names[a["class_id"]]]
                d["size_class"][size_class(side)] += 1
                d["area_pct_bins"][bin_label(ap, AREA_BINS, "%")] += 1
                d["w416_bins"][bin_label(w * k, SIDE_BINS)] += 1
                d["h416_bins"][bin_label(h * k, SIDE_BINS)] += 1
                d["side416"].append(side)
                d["w416"].append(w * k)
                d["h416"].append(h * k)
                d["area_pct"].append(ap)
                d["truncated_flag"][{1: "yes", 0: "no", None: "not_annotated"}[a["truncated"]]] += 1
                d["occluded_flag"][{1: "yes", 0: "no", None: "not_annotated"}[a["occluded"]]] += 1
                if x <= 1 or y <= 1 or x + w >= W - 1 or y + h >= H - 1:
                    d["touches_border"] += 1
        size[s] = {n: {"size_class": dict(per[n]["size_class"]),
                       "area_pct_bins": {b: per[n]["area_pct_bins"].get(b, 0) for b in bin_order(AREA_BINS, "%")},
                       "w416_bins": {b: per[n]["w416_bins"].get(b, 0) for b in bin_order(SIDE_BINS)},
                       "h416_bins": {b: per[n]["h416_bins"].get(b, 0) for b in bin_order(SIDE_BINS)},
                       "side416_pct": pct(per[n]["side416"]), "w416_pct": pct(per[n]["w416"]),
                       "h416_pct": pct(per[n]["h416"]), "area_pct_pct": pct(per[n]["area_pct"]),
                       "truncated_flag": dict(per[n]["truncated_flag"]),
                       "occluded_flag": dict(per[n]["occluded_flag"]),
                       "touches_border": per[n]["touches_border"]} for n in names}
    st["object_size"] = size

    # ---- small-target scenes and position (proxy for distant traffic)
    scenes = {}
    for s in SPLITS:
        multi_small, any_small = 0, 0
        ybands = Counter()
        for r in rows[s]:
            k = 416.0 / max(r["width"], r["height"])
            small = [a for a in r["annotations"] if not a["iscrowd"] and names[a["class_id"]] in VEHICLES
                     and math.sqrt(a["bbox"][2] * a["bbox"][3]) * k < 16]
            if small:
                any_small += 1
            if len(small) >= 3:
                multi_small += 1
            for a in small:
                yc = (a["bbox"][1] + a["bbox"][3] / 2.0) / r["height"]
                ybands["%.1f-%.1f" % (min(int(yc * 5), 4) / 5.0, min(int(yc * 5), 4) / 5.0 + 0.2)] += 1
        scenes[s] = {"images_with_small_vehicle": any_small, "images_with_3plus_small_vehicles": multi_small,
                     "small_vehicle_centre_y_bands": dict(sorted(ybands.items()))}
    st["small_vehicle_scenes"] = scenes

    # ---- negatives and difficult-scene proxies
    neg = {}
    for s in SPLITS:
        rs = rows[s]
        dark = [r for r in rs if r["luma_mean"] < DARK_LUMA]
        lowc = [r for r in rs if r["luma_std"] < LOW_CONTRAST_STD]
        neg[s] = {"no_target_images": sum(1 for r in rs if not r["annotations"]),
                  "no_target_images_pct": round(100.0 * sum(1 for r in rs if not r["annotations"]) / max(1, len(rs)), 2),
                  "dark_images": len(dark),
                  "dark_vehicle_boxes": sum(1 for r in dark for a in r["annotations"]
                                            if not a["iscrowd"] and names[a["class_id"]] in VEHICLES),
                  "low_contrast_images": len(lowc),
                  "low_contrast_vehicle_boxes": sum(1 for r in lowc for a in r["annotations"]
                                                    if not a["iscrowd"] and names[a["class_id"]] in VEHICLES)}
    st["negatives_and_difficulty"] = {"proxy_thresholds": {"dark_luma_mean_below": DARK_LUMA,
                                                           "low_contrast_luma_std_below": LOW_CONTRAST_STD},
                                      "splits": neg}

    # ---- quality
    qr = Counter((q["reason"], q["split"]) for q in quar if not q.get("image_kept"))
    fixes = Counter()
    for q in quar:
        if q.get("image_kept"):
            fixes.update(q.get("detail", {}))
    st["quality"] = {
        "quarantined_by_reason_split": {"%s/%s" % k: v for k, v in sorted(qr.items())},
        "annotation_fixes": dict(fixes),
        "exact_duplicates_removed": sum(v for (r, _), v in qr.items() if r == "exact_duplicate"),
        "same_flickr_photo_removed": sum(v for (r, _), v in qr.items() if r == "same_flickr_photo"),
        "near_duplicates_cross_split_removed": sum(v for (r, _), v in qr.items() if r == "near_duplicate_cross_split"),
        "near_duplicate_pairs_within_split": dict(Counter(w["split"] for w in within)),
        "broken_or_missing_files": sum(v for (r, _), v in qr.items()
                                       if r in ("download_failed", "decode_failed", "missing_file")),
        "exif_orientation_removed": sum(v for (r, _), v in qr.items() if r == "exif_orientation"),
        "size_mismatch_removed": sum(v for (r, _), v in qr.items() if r == "size_mismatch"),
        "cross_split_leakage_after_build": sum(1 for e in errors if "also in" in e),
    }

    # ---- provenance
    pv = defaultdict(lambda: {"decision": Counter(), "reasons": Counter()})
    for p in prov:
        pv[p["source"]]["decision"][p["decision"]] += 1
        for x in p["reasons"]:
            pv[p["source"]]["reasons"][x] += 1
    st["provenance"] = {src: {"candidates": sum(d["decision"].values()), "decision": dict(d["decision"]),
                              "reasons": dict(d["reasons"])} for src, d in sorted(pv.items())}
    st["provenance"]["included_by_licence"] = dict(Counter(r["licence"] for s in SPLITS for r in rows[s]))
    st["provenance"]["openimages_outside_selection_rule"] = man["counts"].get("openimages_outside_rule")
    st["provenance"]["spotcheck"] = man["rules"].get("licence_spotcheck")

    # ---- disk
    disk = {}
    if raw:
        for sub in ("coco", "openimages"):
            p = Path(raw) / sub
            if p.exists():
                b, n = du(p)
                disk["raw_" + sub] = {"bytes": b, "files": n}
    if meta_dir:
        b, n = du(meta_dir)
        disk["openimages_metadata"] = {"bytes": b, "files": n}
    b, n = du(out)
    linked = sum(1 for s in SPLITS for r in rows[s] if os.stat(out / r["path"]).st_nlink > 1)
    img_bytes = sum(r["bytes"] for s in SPLITS for r in rows[s])
    disk["prepared"] = {"apparent_bytes": b, "files": n, "image_bytes": img_bytes,
                        "images_hard_linked": linked, "extra_bytes_on_disk_approx": b - (img_bytes if linked else 0)}
    st["disk"] = disk
    st["hashes"] = {"manifest.json": dataset_checks._sha(out / "manifest.json"),
                    **{s: man["splits"][s]["manifest_sha256"] for s in SPLITS}}
    return st


def md_report(st):
    names = [m["name"] for m in st["class_mapping"]]
    L = []
    a = L.append
    a("# DOORS traffic6 dataset audit: %s\n" % st["dataset"])
    a("Generated by `audit_traffic_dataset.py`. Checks: **%d errors** (files re-hashed: %s).\n"
      % (st["checks"]["errors"], st["checks"]["files_rehashed"]))
    a("## Class mapping\n")
    a("| class_id | category_id | name | DOORS index | COCO id | Open Images labels |")
    a("|---|---|---|---|---|---|")
    for m in st["class_mapping"]:
        a("| %d | %d | %s | %d | %d | %s |" % (m["class_id"], m["category_id"], m["name"], m["doors_index"],
                                             m["coco_category_id"], ", ".join(m["openimages_labels"])))
    a("\n## Totals\n")
    a("| split | images | positive | no-target | trainable boxes | crowd/group boxes (ignored) | COCO | Open Images |")
    a("|---|---|---|---|---|---|---|---|")
    for s in SPLITS:
        t = st["totals"][s]
        a("| %s | %d | %d | %d | %d | %d | %d | %d |" % (s, t["images"], t["positive_images"], t["negative_images"],
          t["boxes_trainable"], t["boxes_crowd_ignored"], t["by_source"].get("coco2017", 0),
          t["by_source"].get("openimages_v7", 0)))
    a("\n## Per class (trainable boxes)\n")
    a("| class | images train/val/test | boxes train/val/test | boxes total | % of boxes | crowd boxes |")
    a("|---|---|---|---|---|---|")
    for n in names:
        d = st["per_class"][n]
        a("| %s | %s | %s | %d | %.2f | %d |" % (n, "/".join(str(d["images"][s]) for s in SPLITS),
          "/".join(str(d["boxes"][s]) for s in SPLITS), d["boxes_total"], d["pct_of_all_boxes"],
          sum(d["crowd_boxes"].values())))
    b = st["balance"]
    a("\nTrain box ratio max/min over all six classes: **%.1f**; over the four vehicle classes: **%.1f**.\n"
      % (b["max_over_min"], b["vehicle_max_over_min"]))
    a("\n## Object size\n")
    a("Convention: side416 = sqrt(w*h) * 416 / max(W, H). very_small < 8 px, small 8-16, medium 16-64, large >= 64.\n")
    for s in SPLITS:
        a("\n### %s\n" % s)
        a("| class | very_small | small | medium | large | side416 p5/p50/p95 | area % p5/p50/p95 | truncated (OI flag) | touches border |")
        a("|---|---|---|---|---|---|---|---|---|")
        for n in names:
            d = st["object_size"][s][n]
            sc = d["size_class"]
            sp, ap = d["side416_pct"], d["area_pct_pct"]
            a("| %s | %d | %d | %d | %d | %s | %s | %d of %d | %d |" % (
                n, sc.get("very_small", 0), sc.get("small", 0), sc.get("medium", 0), sc.get("large", 0),
                "/".join(str(round(sp.get(k, 0), 1)) for k in ("p5", "p50", "p95")),
                "/".join(str(round(ap.get(k, 0), 3)) for k in ("p5", "p50", "p95")),
                d["truncated_flag"].get("yes", 0), d["truncated_flag"].get("yes", 0) + d["truncated_flag"].get("no", 0),
                d["touches_border"]))
    a("\nNormalised area distribution (train, % of image area):\n")
    bins = list(st["object_size"]["train"][names[0]]["area_pct_bins"])
    a("| class | " + " | ".join(bins) + " |")
    a("|---|" + "---|" * len(bins))
    for n in names:
        a("| %s | %s |" % (n, " | ".join(str(st["object_size"]["train"][n]["area_pct_bins"][x]) for x in bins)))
    a("\nBox width / height after the 416 letterbox (train, px):\n")
    bins = list(st["object_size"]["train"][names[0]]["w416_bins"])
    a("| class | dim | " + " | ".join(bins) + " |")
    a("|---|---|" + "---|" * len(bins))
    for n in names:
        for dim in ("w416_bins", "h416_bins"):
            a("| %s | %s | %s |" % (n, dim[0], " | ".join(str(st["object_size"]["train"][n][dim][x]) for x in bins)))
    a("\n## Small-vehicle scenes (proxy for distant traffic, not distance)\n")
    a("| split | images with a vehicle < 16 px | images with >= 3 | centre-y bands of small vehicles (0 = top) |")
    a("|---|---|---|---|")
    for s in SPLITS:
        d = st["small_vehicle_scenes"][s]
        a("| %s | %d | %d | %s |" % (s, d["images_with_small_vehicle"], d["images_with_3plus_small_vehicles"],
          ", ".join("%s: %d" % kv for kv in d["small_vehicle_centre_y_bands"].items())))
    a("\n## Negatives and difficult-scene proxies\n")
    th = st["negatives_and_difficulty"]["proxy_thresholds"]
    a("Dark = mean grey < %g; low contrast = grey std < %g. Pixel statistics, not labels.\n"
      % (th["dark_luma_mean_below"], th["low_contrast_luma_std_below"]))
    a("| split | no-target images | % | dark images | vehicle boxes in dark images | low-contrast images | vehicle boxes in them |")
    a("|---|---|---|---|---|---|---|")
    for s in SPLITS:
        d = st["negatives_and_difficulty"]["splits"][s]
        a("| %s | %d | %.2f | %d | %d | %d | %d |" % (s, d["no_target_images"], d["no_target_images_pct"],
          d["dark_images"], d["dark_vehicle_boxes"], d["low_contrast_images"], d["low_contrast_vehicle_boxes"]))
    a("\n## Quality\n")
    q = st["quality"]
    for k in ("exact_duplicates_removed", "same_flickr_photo_removed", "near_duplicates_cross_split_removed",
              "broken_or_missing_files", "exif_orientation_removed", "size_mismatch_removed",
              "cross_split_leakage_after_build"):
        a("- %s: **%d**" % (k.replace("_", " "), q[k]))
    a("- near-duplicate pairs inside a split (kept, reported): %s" % (q["near_duplicate_pairs_within_split"] or 0))
    a("- annotation fixes (box dropped, image kept): %s" % (q["annotation_fixes"] or "none"))
    a("- quarantined by reason/split: %s" % (q["quarantined_by_reason_split"] or "none"))
    a("\n## Provenance\n")
    a("| source | candidates | decisions | reasons |")
    a("|---|---|---|---|")
    for src in ("coco2017", "openimages_v7"):
        d = st["provenance"].get(src)
        if d:
            a("| %s | %d | %s | %s |" % (src, d["candidates"], ", ".join("%s %d" % kv for kv in sorted(d["decision"].items())),
                                        ", ".join("%s %d" % kv for kv in sorted(d["reasons"].items()))))
    a("\nIncluded images by licence: %s\n" % st["provenance"]["included_by_licence"])
    a("\n## Disk\n")
    for k, v in st["disk"].items():
        a("- %s: %s" % (k, ", ".join("%s %s" % (kk, ("%.2f GB" % (vv / 1e9)) if "bytes" in kk else vv)
                                     for kk, vv in v.items())))
    a("\n## Hashes\n")
    for k, v in st["hashes"].items():
        a("- %s: `%s`" % (k, v))
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--verify-files", action="store_true")
    ap.add_argument("--raw")
    ap.add_argument("--meta")
    a = ap.parse_args()
    st = audit(a.out, a.raw, a.meta, a.verify_files)
    for name, text in (("stats.json", json.dumps(st, indent=1, sort_keys=True) + "\n"),
                       ("DATASET_REPORT.md", md_report(st))):
        with open(Path(a.out, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    print("checks: %d errors" % st["checks"]["errors"])
    for e in st["checks"]["first_errors"]:
        print("  " + e)
    return 1 if st["checks"]["errors"] else 0


if __name__ == "__main__":
    sys.exit(main())
