"""Focused tests of the traffic6 dataset build (build_traffic_dataset.py,
dataset_checks.py, openimages_traffic.py). A small synthetic COCO + Open
Images fixture is written to a temp dir; nothing is downloaded.

Each bad case (duplicate across splits, missing provenance, invalid class id,
missing file, rejected image in a manifest) is introduced on purpose and the
checker must name it.

  python -m unittest tests.test_dataset_pipeline -v
"""
import hashlib
import json
import random
import shutil
import sys
import tempfile
from collections import Counter
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "data"))
import build_traffic_dataset as btd  # noqa: E402
import dataset_checks  # noqa: E402
import openimages_traffic as oi  # noqa: E402

CC_BY_COCO = "http://creativecommons.org/licenses/by/2.0/"
NC_COCO = "http://creativecommons.org/licenses/by-nc/2.0/"
SA_COCO = "http://creativecommons.org/licenses/by-sa/2.0/"
COCO_CATS = {"person": 1, "bicycle": 2, "car": 3, "motorcycle": 4, "bus": 6, "truck": 8,
             "traffic light": 10, "fire hydrant": 11, "stop sign": 13, "parking meter": 14, "bench": 15}
NOWHERE = "file:///nonexistent/doors-test-image.jpg"


def smooth_image(seed, size=(64, 48), shift=0):
    """A smooth random picture: its dHash is stable under small changes."""
    rs = np.random.RandomState(seed)
    small = rs.randint(0, 200, (6, 8, 3)).astype(np.uint8)
    im = Image.fromarray(small).resize(size, Image.BILINEAR)
    if shift:
        im = Image.fromarray(np.clip(np.asarray(im).astype(int) + shift, 0, 255).astype(np.uint8))
    return im


def save(im, path, quality=95):
    path.parent.mkdir(parents=True, exist_ok=True)
    im.save(path, "JPEG", quality=quality)


class Fixture:
    """COCO train/val + Open Images train/validation/test, with known traps."""

    def __init__(self, root):
        self.root = Path(root)
        self.ann = self.root / "coco_ann"
        self.meta = self.root / "oi_meta"
        self.raw = self.root / "raw"
        self.ann.mkdir(parents=True)
        self.meta.mkdir(parents=True)
        self._coco()
        self._openimages()

    def _coco(self):
        licences = [{"id": 1, "url": CC_BY_COCO, "name": "Attribution License"},
                    {"id": 2, "url": NC_COCO, "name": "Attribution-NonCommercial License"},
                    {"id": 3, "url": SA_COCO, "name": "Attribution-ShareAlike License"}]
        cats = [{"id": i, "name": n, "supercategory": "x"} for n, i in COCO_CATS.items()]
        for src, base in (("train2017", 100), ("val2017", 900)):
            images, anns = [], []
            n = 6 if src == "train2017" else 8
            for k in range(n):
                iid = base + k
                lic = 1
                if src == "train2017" and k == 4:
                    lic = 2  # non-commercial: must be rejected
                if src == "train2017" and k == 5:
                    lic = 3  # share-alike: review tier, rejected by default
                fn = "%012d.jpg" % iid
                images.append({"id": iid, "file_name": fn, "width": 64, "height": 48, "license": lic,
                               "coco_url": NOWHERE, "flickr_url": "http://farm1.staticflickr.com/1/%d_ab_z.jpg" % (5000 + iid)})
                save(smooth_image(iid), self.raw / "coco" / src / fn)
                cat = [COCO_CATS["car"], COCO_CATS["truck"], COCO_CATS["person"]][k % 3]
                anns.append({"id": iid * 10, "image_id": iid, "category_id": cat, "bbox": [4, 4, 20, 16],
                             "area": 320.0, "iscrowd": 0})
                anns.append({"id": iid * 10 + 1, "image_id": iid, "category_id": COCO_CATS["bicycle"],
                             "bbox": [30, 20, 10, 10], "area": 100.0, "iscrowd": 0})
            # a street-context negative (traffic light, no target)
            iid = base + 50
            fn = "%012d.jpg" % iid
            images.append({"id": iid, "file_name": fn, "width": 64, "height": 48, "license": 1,
                           "coco_url": NOWHERE, "flickr_url": "http://farm1.staticflickr.com/1/%d_ab_z.jpg" % (5000 + iid)})
            save(smooth_image(iid), self.raw / "coco" / src / fn)
            anns.append({"id": iid * 10, "image_id": iid, "category_id": COCO_CATS["traffic light"],
                         "bbox": [1, 1, 5, 9], "area": 45.0, "iscrowd": 0})
            (self.ann / ("instances_%s.json" % src)).write_text(json.dumps(
                {"licenses": licences, "categories": cats, "images": images, "annotations": anns}))

    def _openimages(self):
        m = self.meta
        with open(m / oi.FILES["classes"], "w", encoding="utf-8") as f:
            for k, v in oi.OI_NAME.items():
                f.write("%s,%s\n" % (k, v))
        img_hdr = ("ImageID,Subset,OriginalURL,OriginalLandingURL,License,AuthorProfileURL,Author,Title,"
                   "OriginalSize,OriginalMD5,Thumbnail300KURL,Rotation\n")
        box_hdr = ("ImageID,Source,LabelName,Confidence,XMin,XMax,YMin,YMax,IsOccluded,IsTruncated,"
                   "IsGroupOf,IsDepiction,IsInside\n")
        self.oi_ids = {}
        for subset, seed0 in (("train", 300), ("validation", 400), ("test", 500)):
            rows, boxes, labels = [], [], []
            ids = []
            for k in range(5):
                iid = "%016x" % (seed0 * 1000 + k)
                ids.append(iid)
                lic = oi.CC_BY_20
                if subset == "train" and k == 3:
                    lic = "https://creativecommons.org/licenses/by-nc/2.0/"  # must be rejected
                rows.append("%s,%s,https://farm1.staticflickr.com/1/%d_x_o.jpg,https://www.flickr.com/photos/u/%d,"
                            "%s,https://www.flickr.com/people/u/,Author %d,t,1,md5,thumb,0.0\n"
                            % (iid, subset, 7000 + seed0 + k, 7000 + seed0 + k, lic, k))
                label = ["/m/0k4j", "/m/0h2r6", "/m/07r04", "/m/0k4j", "/m/0199g"][k]
                boxes.append("%s,xclick,%s,1,0.1,0.5,0.2,0.6,0,1,0,0,0\n" % (iid, label))
                boxes.append("%s,xclick,/m/04yx4,1,0.6,0.7,0.3,0.9,1,0,0,0,0\n" % iid)
                # the same car twice (Car and Taxi): a duplicate after folding
                if k == 0:
                    boxes.append("%s,xclick,/m/0pg52,1,0.1,0.5,0.2,0.6,0,1,0,0,0\n" % iid)
                if not (subset == "train" and k == 4):
                    labels.append("%s,verification,/m/01g317,1\n" % iid)  # person verified
                save(smooth_image(int(iid, 16) + 20000, size=(80, 60)),
                     self.raw / "openimages" / subset / ("%s.jpg" % iid))
            self.oi_ids[subset] = ids
            (m / oi.FILES["images"][subset]).write_text(img_hdr + "".join(rows), encoding="utf-8")
            (m / oi.FILES["boxes"][subset]).write_text(box_hdr + "".join(boxes), encoding="utf-8")
            (m / oi.FILES["labels"][subset]).write_text("ImageID,Source,LabelName,Confidence\n" + "".join(labels),
                                                       encoding="utf-8")


def build(fx, out, *extra):
    return btd.main(["--coco-ann", str(fx.ann), "--oi-meta", str(fx.meta), "--raw", str(fx.raw),
                     "--out", str(out), "--workers", "1", "--neg-ratio", "1.0"] + list(extra))


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def rows_of(out, split):
    return dataset_checks.read_jsonl(Path(out) / ("manifest_%s.jsonl" % split))


class DatasetPipelineTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="doors-ds-test-"))
        cls.fx = Fixture(cls.tmp / "fx")
        # trap 1: an Open Images test picture byte-identical to a COCO train picture
        coco_train = cls.fx.raw / "coco" / "train2017" / ("%012d.jpg" % 100)
        oi_test = cls.fx.raw / "openimages" / "test" / ("%s.jpg" % cls.fx.oi_ids["test"][1])
        shutil.copyfile(coco_train, oi_test)
        cls.dup_pair = ("coco:train2017:%012d" % 100, "oi:test:%s" % cls.fx.oi_ids["test"][1])
        # trap 2: an Open Images validation picture that is a re-encoded, brightened COCO train picture
        oi_val = cls.fx.raw / "openimages" / "validation" / ("%s.jpg" % cls.fx.oi_ids["validation"][2])
        save(smooth_image(101, shift=6).resize((80, 60), Image.BILINEAR), oi_val, quality=80)
        cls.near_pair = ("coco:train2017:%012d" % 101, "oi:validation:%s" % cls.fx.oi_ids["validation"][2])
        # trap 3: a missing Open Images file that cannot be fetched
        cls.missing_uid = "oi:validation:%s" % cls.fx.oi_ids["validation"][4]
        (cls.fx.raw / "openimages" / "validation" / ("%s.jpg" % cls.fx.oi_ids["validation"][4])).unlink()
        cls._orig_url = oi.image_url
        oi.image_url = lambda subset, iid: NOWHERE
        cls.out1 = cls.tmp / "out1"
        cls.out2 = cls.tmp / "out2"
        cls.rc1 = build(cls.fx, cls.out1)
        cls.rc2 = build(cls.fx, cls.out2)

    @classmethod
    def tearDownClass(cls):
        oi.image_url = cls._orig_url
        shutil.rmtree(cls.tmp, ignore_errors=True)

    # ---- the build itself
    def test_build_passes_checks(self):
        self.assertEqual(self.rc1, 0)
        self.assertEqual(dataset_checks.check_dataset(self.out1, verify_files=True), [])

    def test_deterministic_rebuild(self):
        m1 = json.loads((self.out1 / "manifest.json").read_text())
        m2 = json.loads((self.out2 / "manifest.json").read_text())
        self.assertEqual(m1["splits"], m2["splits"])
        for f in ("provenance.jsonl", "quarantine.jsonl", "test_set.sha256", "ATTRIBUTION.tsv"):
            self.assertEqual(sha(self.out1 / f), sha(self.out2 / f), f)

    def test_coco_split_rule(self):
        for src, lo, hi, share in (("val2017", 900, 1100, {"val", "test"}), ("train2017", 0, 0, None)):
            a = [btd.split_of_coco(20261004, src, i) for i in range(4000)]
            self.assertEqual(a, [btd.split_of_coco(20261004, src, i) for i in range(4000)])
            self.assertNotEqual(a, [btd.split_of_coco(1, src, i) for i in range(4000)])
            if src == "val2017":
                self.assertEqual(set(a), share)
                self.assertTrue(1800 < a.count("val") < 2200)
            else:
                self.assertTrue(300 < a.count("test") < 500 and 120 < a.count("val") < 280, Counter(a))
        for s in ("train", "val", "test"):
            for r in rows_of(self.out1, s):
                if r["source"] == "coco2017":
                    src, iid = r["source_id"].split("/")
                    self.assertEqual(r["split"], btd.split_of_coco(20261004, src, int(iid)))

    def test_class_mapping(self):
        m = json.loads((self.out1 / "manifest.json").read_text())
        self.assertEqual([(c["class_id"], c["name"], c["doors_index"]) for c in m["class_mapping"]],
                         [(0, "car", 2), (1, "truck", 7), (2, "bus", 5), (3, "motorcycle", 3),
                          (4, "bicycle", 1), (5, "person", 0)])
        js = json.loads((self.out1 / "annotations" / "traffic_train.json").read_text())
        self.assertEqual([(c["id"], c["name"]) for c in js["categories"]],
                         [(1, "car"), (2, "truck"), (3, "bus"), (4, "motorcycle"), (5, "bicycle"), (6, "person")])
        by_src = {}
        for s in ("train", "val", "test"):
            for r in rows_of(self.out1, s):
                for a in r["annotations"]:
                    by_src.setdefault(a["source_label"], set()).add((a["class_id"], a["class_name"]))
        self.assertEqual(by_src["coco:3"], {(0, "car")})
        self.assertEqual(by_src["coco:8"], {(1, "truck")})
        self.assertEqual(by_src["coco:2"], {(4, "bicycle")})
        self.assertEqual(by_src["oi:Van"], {(0, "car")})
        self.assertEqual(by_src["oi:Man"], {(5, "person")})
        self.assertEqual(by_src["oi:Truck"], {(1, "truck")})

    def test_rejected_never_in_manifests(self):
        prov = {p["uid"]: p for p in dataset_checks.read_jsonl(self.out1 / "provenance.jsonl")}
        in_manifest = {r["uid"] for s in ("train", "val", "test") for r in rows_of(self.out1, s)}
        expect = {"coco:train2017:%012d" % 104: "coco_licence_exclude",
                  "coco:train2017:%012d" % 105: "coco_licence_review",
                  "oi:train:%s" % self.fx.oi_ids["train"][3]: "licence_not_cc_by_2.0",
                  "oi:train:%s" % self.fx.oi_ids["train"][4]: "person_unverified"}
        for uid, reason in expect.items():
            self.assertEqual(prov[uid]["decision"], "rejected", uid)
            self.assertIn(reason, prov[uid]["reasons"])
            self.assertNotIn(uid, in_manifest)
        for uid in in_manifest:
            self.assertEqual(prov[uid]["decision"], "included")

    def test_provenance_fields_present(self):
        for s in ("train", "val", "test"):
            for r in rows_of(self.out1, s):
                for k in ("source", "source_id", "licence", "licence_url", "original_url", "annotation_source", "sha256"):
                    self.assertTrue(r.get(k), (r["uid"], k))
                if r["source"] == "openimages_v7":
                    self.assertTrue(r["author"] and r["landing_url"])

    def test_exact_duplicate_kept_in_test_only(self):
        q = dataset_checks.read_jsonl(self.out1 / "quarantine.jsonl")
        hit = [x for x in q if x["reason"] == "exact_duplicate"]
        self.assertEqual([(x["uid"], x["kept"]) for x in hit], [self.dup_pair])
        test_uids = {r["uid"] for r in rows_of(self.out1, "test")}
        self.assertIn(self.dup_pair[1], test_uids)

    def test_near_duplicate_across_splits_removed(self):
        q = dataset_checks.read_jsonl(self.out1 / "quarantine.jsonl")
        hit = [(x["uid"], x["kept"]) for x in q if x["reason"] == "near_duplicate_cross_split"]
        self.assertEqual(hit, [self.near_pair])  # train copy removed, val keeps it

    def test_missing_file_quarantined(self):
        q = dataset_checks.read_jsonl(self.out1 / "quarantine.jsonl")
        self.assertIn((self.missing_uid, "download_failed"), [(x["uid"], x["reason"]) for x in q])
        prov = {p["uid"]: p for p in dataset_checks.read_jsonl(self.out1 / "provenance.jsonl")}
        self.assertEqual(prov[self.missing_uid]["decision"], "quarantined")

    def test_duplicate_annotation_dropped(self):
        r = [r for r in rows_of(self.out1, "train") if r["uid"] == "oi:train:%s" % self.fx.oi_ids["train"][0]][0]
        cars = [a for a in r["annotations"] if a["class_name"] == "car"]
        self.assertEqual(len(cars), 1)

    # ---- the checker on deliberately broken copies
    def broken_copy(self, name):
        dst = self.tmp / name
        if dst.exists():
            shutil.rmtree(dst)
        shutil.copytree(self.out1, dst)
        return dst

    @staticmethod
    def rehash(out):
        """Refresh manifest.json's hashes so only the planted defect remains."""
        m = json.loads((out / "manifest.json").read_text())
        for s, d in m["splits"].items():
            d["manifest_sha256"] = sha(out / d["manifest_file"])
            d["annotation_sha256"] = sha(out / d["annotation_file"])
            d["images"] = len(rows_of(out, s))
        (out / "manifest.json").write_text(json.dumps(m))

    def test_checker_rejects_duplicate_across_splits(self):
        out = self.broken_copy("bad_dup")
        row = rows_of(out, "train")[0]
        js_tr = json.loads((out / "annotations" / "traffic_train.json").read_text())
        js_va = json.loads((out / "annotations" / "traffic_val.json").read_text())
        row["split"] = "val"
        row["path"] = row["path"].replace("train2017/", "val2017/")
        (out / row["path"]).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(out / ("train2017/" + row["file_name"]), out / row["path"])
        js_va["images"].append([i for i in js_tr["images"] if i["id"] == row["image_id"]][0])
        js_va["annotations"] += [a for a in js_tr["annotations"] if a["image_id"] == row["image_id"]]
        (out / "annotations" / "traffic_val.json").write_text(json.dumps(js_va))
        with open(out / "manifest_val.jsonl", "a") as f:
            f.write(json.dumps(row) + "\n")
        self.rehash(out)
        errs = dataset_checks.check_dataset(out)
        self.assertTrue(any("uid %s also in train" % row["uid"] in e for e in errs), errs)
        self.assertTrue(any("sha256 %s also in train" % row["sha256"] in e for e in errs), errs)

    def test_checker_rejects_missing_provenance(self):
        out = self.broken_copy("bad_prov")
        uid = rows_of(out, "train")[0]["uid"]
        prov = [p for p in dataset_checks.read_jsonl(out / "provenance.jsonl") if p["uid"] != uid]
        btd.write_jsonl(out / "provenance.jsonl", prov)
        errs = dataset_checks.check_dataset(out)
        self.assertIn("train %s: no provenance record" % uid, errs)

    def test_checker_rejects_rejected_image_in_manifest(self):
        out = self.broken_copy("bad_decision")
        uid = rows_of(out, "val")[0]["uid"]
        prov = dataset_checks.read_jsonl(out / "provenance.jsonl")
        for p in prov:
            if p["uid"] == uid:
                p["decision"] = "rejected"
        btd.write_jsonl(out / "provenance.jsonl", prov)
        errs = dataset_checks.check_dataset(out)
        self.assertIn("val %s: provenance decision is rejected" % uid, errs)

    def test_checker_rejects_invalid_class_id(self):
        out = self.broken_copy("bad_class")
        js = json.loads((out / "annotations" / "traffic_train.json").read_text())
        js["annotations"][0]["category_id"] = 7
        (out / "annotations" / "traffic_train.json").write_text(json.dumps(js))
        self.rehash(out)
        errs = dataset_checks.check_dataset(out)
        self.assertTrue(any("invalid category_id 7" in e for e in errs), errs)
        self.assertTrue(any("class mapping differs" in e for e in errs), errs)

    def test_checker_rejects_missing_file_and_bad_hash(self):
        out = self.broken_copy("bad_file")
        rows = rows_of(out, "test")
        (out / rows[0]["path"]).unlink()
        p1 = out / rows[1]["path"]
        p1.unlink()  # break the hard link before changing bytes
        p1.write_bytes(b"not the same picture")
        errs = dataset_checks.check_dataset(out, verify_files=True)
        self.assertIn("test %s: file missing %s" % (rows[0]["uid"], rows[0]["path"]), errs)
        self.assertIn("test %s: file sha256 differs" % rows[1]["uid"], errs)

    def test_checker_rejects_tampered_manifest(self):
        out = self.broken_copy("bad_manifest")
        with open(out / "manifest_train.jsonl", "a") as f:
            f.write("{not json\n")
        errs = dataset_checks.check_dataset(out)
        self.assertTrue(any("manifest_train.jsonl sha256 differs" in e for e in errs), errs)
        self.assertTrue(any("unreadable" in e for e in errs), errs)


class UnitTest(unittest.TestCase):
    def test_finalize_boxes(self):
        rec = {"boxes": [
            {"class_id": 0, "bbox": [10, 10, 20, 20], "iscrowd": 0},
            {"class_id": 0, "bbox": [10.2, 10, 20, 20], "iscrowd": 0},   # duplicate (IoU > 0.95)
            {"class_id": 9, "bbox": [1, 1, 5, 5], "iscrowd": 0},         # invalid class id
            {"class_id": 1, "bbox": [90, 90, 30, 30], "iscrowd": 0},     # outside a 100x100 image
            {"class_id": 2, "bbox": [5, 5, 0.5, 9], "iscrowd": 0},       # under 1 px wide
            {"class_id": 5, "norm": [0.5, 0.5, 0.75, 1.0], "iscrowd": 1},
        ]}
        boxes, probs = btd.finalize_boxes(rec, 100, 100)
        self.assertEqual([(b["class_id"], b["bbox"]) for b in boxes],
                         [(0, [10.0, 10.0, 20.0, 20.0]), (5, [50.0, 50.0, 25.0, 50.0])])
        self.assertEqual(dict(probs), {"duplicate_annotation": 1, "invalid_class_id": 1,
                                       "box_outside_image": 1, "box_under_1px": 1})

    def test_near_duplicate_banding_is_exact(self):
        rng = random.Random(7)
        hs = []
        for i in range(400):
            base = rng.getrandbits(64) if i % 4 == 0 else hs[-1][1] ^ (1 << rng.randrange(64)) ^ (1 << rng.randrange(64))
            hs.append(("u%03d" % i, base))
        items = [(u, "%016x" % h) for u, h in hs]
        got = btd.near_duplicate_pairs(items, 4)
        brute = sorted((items[i][0], items[j][0], bin(hs[i][1] ^ hs[j][1]).count("1"))
                       for i in range(len(hs)) for j in range(i + 1, len(hs))
                       if bin(hs[i][1] ^ hs[j][1]).count("1") <= 4)
        self.assertEqual(got, brute)
        self.assertGreater(len(got), 100)

    def test_openimages_decide(self):
        row = {"License": oi.CC_BY_20, "Author": "a", "AuthorProfileURL": "p", "OriginalURL": "o",
               "OriginalLandingURL": "l", "Rotation": "0.0"}
        box = {"label": "/m/0k4j", "x0": 0.1, "x1": 0.2, "y0": 0.1, "y1": 0.2, "depiction": 0, "inside": 0}
        self.assertEqual(oi.decide(row, [box]), [])
        self.assertEqual(oi.decide(dict(row, Rotation=""), [box]), ["rotation_not_zero"])
        self.assertEqual(oi.decide(dict(row, Author=""), [box]), ["missing_author"])
        self.assertEqual(oi.decide(row, [box, dict(box, label="/m/07yv9")]), ["ambiguous_vehicle_box"])
        self.assertEqual(oi.decide(row, [dict(box, depiction=1)]), ["depiction"])
        self.assertEqual(oi.decide(row, [dict(box, x1=1.2)]), ["invalid_box"])
        self.assertEqual(oi.decide(None, [box]), ["not_in_image_list"])
        self.assertTrue(oi.person_verified({"/m/03bt1vf": 0}))
        self.assertFalse(oi.person_verified({"/m/0k4j": 1}))


if __name__ == "__main__":
    unittest.main()
