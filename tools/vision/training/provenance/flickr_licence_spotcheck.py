#!/usr/bin/env python3
"""Spot-check the licence Flickr shows today for a fixed sample of selected images.

The dataset licence labels (COCO's per-image field, Open Images' License
column) are what the uploader set when the dataset was collected. This
script compares them, for a small deterministic sample, with the licence the
Flickr photo page shows now. It is evidence about label reliability, not a
per-image verification: CC licences cannot be revoked for copies already
obtained, so a later change on Flickr does not by itself void the grant.

Sample: the N selected candidates per source with the smallest
sha256("doors-spotcheck:SEED:" + uid). Flickr's public oEmbed endpoint is
queried one photo at a time with a pause; the photo id comes from the
recorded Flickr URL. (The photo page itself redirects anonymous clients to
Explore, whose HTML carries other photos' licences: not usable.)

Usage:
  flickr_licence_spotcheck.py PROVENANCE_JSONL OUT_JSON [--n 40] [--seed 20261004]
"""
import argparse
import hashlib
import json
import sys
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "data"))
from build_traffic_dataset import flickr_photo_id  # noqa: E402

# flickr.photos.licenses.getInfo (ids 0-10 are stable; 11+ are the CC 4.0 set)
FLICKR_LICENCE = {0: "All Rights Reserved", 1: "CC BY-NC-SA 2.0", 2: "CC BY-NC 2.0", 3: "CC BY-NC-ND 2.0",
                  4: "CC BY 2.0", 5: "CC BY-SA 2.0", 6: "CC BY-ND 2.0", 7: "No known copyright restrictions",
                  8: "United States Government Work", 9: "CC0 1.0", 10: "Public Domain Mark 1.0",
                  11: "CC BY 4.0", 12: "CC BY-SA 4.0", 13: "CC BY-ND 4.0", 14: "CC BY-NC 4.0",
                  15: "CC BY-NC-SA 4.0", 16: "CC BY-NC-ND 4.0"}


def flickr_licence(photo_id):
    """Flickr's public oEmbed endpoint: one photo, its current licence id.

    It resolves the photo by id; the owner part of the URL is not checked.
    404 means the photo is deleted or not public."""
    url = ("https://www.flickr.com/services/oembed/?format=json&url="
           "https://www.flickr.com/photos/flickr/%s/" % photo_id)
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (DOORS provenance spot check)"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            js = json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return {"status": "photo_unavailable" if e.code == 404 else "http_%d" % e.code}
    except Exception as e:  # noqa: BLE001
        return {"status": "error", "detail": "%s: %s" % (type(e).__name__, e)}
    if str(js.get("web_page", "")).rstrip("/").split("/")[-1] != str(photo_id) or "license_id" not in js:
        return {"status": "unexpected_response"}
    lid = int(js["license_id"])
    return {"status": "ok", "flickr_licence_id": lid, "flickr_licence": FLICKR_LICENCE.get(lid, "?"),
            "flickr_author": js.get("author_name")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("provenance")
    ap.add_argument("out")
    ap.add_argument("--n", type=int, default=40)
    ap.add_argument("--seed", type=int, default=20261004)
    ap.add_argument("--pause", type=float, default=1.5)
    a = ap.parse_args()
    rows = [json.loads(l) for l in open(a.provenance, encoding="utf-8")]
    sel = [r for r in rows if r["decision"] in ("selected", "included")]
    out = {"seed": a.seed, "n_per_source": a.n, "checked": time.strftime("%Y-%m-%d"), "results": []}
    for src in ("coco2017", "openimages_v7"):
        pool = sorted((r for r in sel if r["source"] == src),
                      key=lambda r: hashlib.sha256(("doors-spotcheck:%d:%s" % (a.seed, r["uid"])).encode()).hexdigest())
        for r in pool[:a.n]:
            pid = flickr_photo_id(r.get("landing_url"), r.get("original_url"))
            res = flickr_licence(pid) if pid else {"status": "no_photo_id"}
            res.update(uid=r["uid"], source=src, photo_id=pid, dataset_licence=r["licence"])
            if res["status"] == "ok":
                res["match"] = res["flickr_licence"] == r["licence"] or (
                    r["licence"].startswith("No known") and res["flickr_licence"].startswith("No known"))
            out["results"].append(res)
            print(json.dumps(res), flush=True)
            time.sleep(a.pause)
    summ = {}
    for src in ("coco2017", "openimages_v7"):
        rs = [r for r in out["results"] if r["source"] == src]
        summ[src] = {"sampled": len(rs),
                     "same_licence_today": sum(1 for r in rs if r.get("match") is True),
                     "different_licence_today": sum(1 for r in rs if r.get("match") is False),
                     "photo_unavailable": sum(1 for r in rs if r["status"] == "photo_unavailable"),
                     "other": sum(1 for r in rs if r["status"] not in ("ok", "photo_unavailable")),
                     "today_licences": sorted(set(r.get("flickr_licence") for r in rs if r.get("flickr_licence")))}
    out["summary"] = summ
    Path(a.out).write_text(json.dumps(out, indent=1))
    print(json.dumps(summ, indent=1))


if __name__ == "__main__":
    main()
