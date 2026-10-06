"""Focused tests of eval/eval_mixed.py: the Open Images scoring rule and the
size buckets, on a synthetic split (no model, no dataset, seconds)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "eval"))

import eval_mixed as em  # noqa: E402

CLASSES = ["car", "truck", "bus", "motorcycle", "bicycle", "person"]
CATS = [{"id": i + 1, "name": c} for i, c in enumerate(CLASSES)]


def split(images):
    """images: list of (id, source, human_labels, [(class, bbox)]) -> gt, rows."""
    gt = {"images": [], "annotations": [], "categories": CATS}
    rows, aid = {}, 1
    for iid, src, labels, boxes in images:
        gt["images"].append({"id": iid, "width": 416, "height": 416, "file_name": "%d.jpg" % iid})
        rows[iid] = {"image_id": iid, "source": src, "human_labels": labels, "uid": "u%d" % iid}
        for c, b in boxes:
            gt["annotations"].append({"id": aid, "image_id": iid, "category_id": CLASSES.index(c) + 1,
                                      "bbox": b, "area": b[2] * b[3], "iscrowd": 0})
            aid += 1
    return gt, rows


def det(iid, c, bbox, s=0.9):
    return {"image_id": iid, "category_id": CLASSES.index(c) + 1, "bbox": bbox, "score": s}


OI = "openimages_v7"


class Rules(unittest.TestCase):
    def test_mapping_is_the_datasets(self):
        """eval_mixed reads the build's own folding (DATASET_TRAFFIC6_R0.md section 1)."""
        self.assertEqual(em.FOLD_BY_NAME, {
            "Car": "car", "Limousine": "car", "Van": "car", "Taxi": "car", "Truck": "truck", "Bus": "bus",
            "Motorcycle": "motorcycle", "Bicycle": "bicycle",
            "Person": "person", "Man": "person", "Woman": "person", "Boy": "person", "Girl": "person"})
        self.assertEqual(em.PRIMARY_NAME, {"car": "Car", "truck": "Truck", "bus": "Bus",
                                           "motorcycle": "Motorcycle", "bicycle": "Bicycle", "person": "Person"})

    def test_coco_scores_every_class(self):
        self.assertEqual(em.evaluable_classes("coco2017", None, CLASSES), set(CLASSES))

    def test_openimages_primary_present_or_absent(self):
        got = em.evaluable_classes(OI, {"Car": 0, "Person": 1}, CLASSES)
        self.assertEqual(got, {"car", "person"})

    def test_verified_car_subclass_does_not_score_car(self):
        """Van/Taxi/Limousine present or absent says nothing about other cars."""
        for name in ("Van", "Taxi", "Limousine"):
            for conf in (0, 1):
                self.assertEqual(em.evaluable_classes(OI, {name: conf, "Person": 1}, CLASSES), {"person"},
                                 (name, conf))
        self.assertEqual(em.evaluable_classes(OI, {"Van": 1, "Taxi": 1, "Limousine": 1}, CLASSES), set())

    def test_verified_person_subclass_does_not_score_person(self):
        """Man/Woman/Boy/Girl present: other people may be unboxed."""
        for name in ("Man", "Woman", "Boy", "Girl"):
            for conf in (0, 1):
                self.assertEqual(em.evaluable_classes(OI, {name: conf, "Car": 1}, CLASSES), {"car"}, (name, conf))
        self.assertEqual(em.evaluable_classes(OI, {"Man": 1, "Woman": 1, "Boy": 1, "Girl": 1}, CLASSES), set())

    def test_buckets(self):
        self.assertEqual([em.bucket(s) for s in (0, 7.99, 8, 15.99, 16, 63.99, 64, 300)],
                         ["very_small", "very_small", "small", "small", "medium", "medium", "large", "large"])
        # side416 is normalised by the longer image side
        self.assertAlmostEqual(em.side416(10, 40, 832, 416), 10.0)

    def test_wilson(self):
        lo, hi = em.wilson(5, 10)
        self.assertTrue(0.2 < lo < 0.5 < hi < 0.8)
        self.assertEqual(em.wilson(0, 0), [None, None])


class Scoring(unittest.TestCase):
    def setUp(self):
        car = ("car", [100, 100, 80, 60])
        self.gt, self.rows = split([
            (1, "coco2017", None, [car]),
            (2, "openimages_v7", {"Car": 1, "Person": 1}, [car]),     # truck never verified
            (3, "openimages_v7", {"Car": 1, "Truck": 0, "Person": 1}, [car]),  # truck verified absent
            (4, "openimages_v7", {"Person": 1, "Van": 0}, [("person", [10, 10, 40, 120])]),  # car unknown
        ])

    def score(self, dets):
        return em.score(self.gt, self.rows, dets, 0.35)

    def test_unverified_class_detection_is_not_a_false_positive(self):
        r = self.score([det(i, "car", [100, 100, 80, 60]) for i in (1, 2, 3)]
                       + [det(2, "truck", [250, 200, 90, 90])])
        self.assertEqual(r["per_class"]["truck"]["fp"], 0)
        self.assertEqual(r["per_class"]["truck"]["images_scored"], 2)  # COCO 1 and verified-absent 3
        self.assertEqual(r["errors"]["openimages_left_out_detections"], {"truck": 1})

    def test_verified_absent_and_coco_count_as_false_positives(self):
        r = self.score([det(1, "truck", [250, 200, 90, 90]), det(3, "truck", [250, 200, 90, 90])])
        self.assertEqual(r["per_class"]["truck"]["fp"], 2)
        self.assertEqual(r["errors"]["false_positives"]["truck"]["background"], 2)

    def test_car_on_image_with_only_van_absent_is_left_out(self):
        r = self.score([det(4, "car", [200, 200, 50, 50])] + [det(i, "car", [100, 100, 80, 60]) for i in (1, 2, 3)])
        self.assertEqual(r["per_class"]["car"]["fp"], 0)
        self.assertEqual(r["per_class"]["car"]["tp"], 3)
        self.assertAlmostEqual(r["per_class"]["car"]["ap50"], 1.0)

    def test_existing_box_from_subclass_does_not_score_the_class(self):
        """A Van box (car) and a Man box (person) on an image where neither Car
        nor Person was verified: the boxes are dropped, not misses, and a
        detection of another, unboxed car or person is not a false positive."""
        gt, rows = split([
            (1, "coco2017", None, [("car", [100, 100, 80, 60])]),
            (2, OI, {"Van": 1, "Man": 1, "Bus": 1}, [("car", [10, 10, 60, 40]), ("person", [200, 50, 40, 120]),
                                                     ("bus", [250, 250, 120, 90])]),
        ])
        dets = [det(1, "car", [100, 100, 80, 60]),
                det(2, "car", [300, 20, 60, 40]),      # another car, maybe unboxed
                det(2, "person", [100, 200, 40, 120]),  # another person, maybe unboxed
                det(2, "bus", [250, 250, 120, 90])]
        r = em.score(gt, rows, dets, 0.35)
        self.assertEqual((r["per_class"]["car"]["fp"], r["per_class"]["car"]["fn"], r["per_class"]["car"]["gt"]),
                         (0, 0, 1))
        self.assertEqual((r["per_class"]["person"]["fp"], r["per_class"]["person"]["gt"]), (0, 0))
        self.assertEqual(r["per_class"]["bus"]["tp"], 1)
        self.assertEqual(r["evaluability"]["gt_boxes_dropped_class_not_verified"],
                         {"car": 1, "truck": 0, "bus": 0, "motorcycle": 0, "bicycle": 0, "person": 1})
        self.assertEqual(r["errors"]["openimages_left_out_detections"], {"car": 1, "person": 1})

    def test_car_absent_taxi_unverified_is_counted(self):
        gt, rows = split([(1, OI, {"Car": 0, "Person": 1}, [("person", [10, 10, 40, 120])]),
                          (2, OI, {"Car": 0, "Taxi": 0, "Person": 1}, [("person", [10, 10, 40, 120])])])
        r = em.score(gt, rows, [det(1, "car", [200, 200, 60, 40]), det(2, "car", [200, 200, 60, 40])], 0.35)
        self.assertEqual(r["per_class"]["car"]["fp"], 2)
        self.assertEqual(r["errors"]["car_fp_on_car_absent_taxi_unverified"]["count"], 1)

    def test_confusion_and_misses(self):
        # a truck detection on the COCO car, and the car itself missed
        r = self.score([det(1, "truck", [100, 100, 80, 60])] + [det(i, "car", [100, 100, 80, 60]) for i in (2, 3)])
        self.assertEqual(r["errors"]["confusion_pred_to_gt"], {"truck->car": 1})
        self.assertEqual(r["per_class"]["car"]["fn"], 1)
        self.assertEqual(r["errors"]["missed_by_size"]["car"]["large"], 1)  # side sqrt(80*60) = 69 px

    def test_size_bucket_metrics(self):
        gt, rows = split([(1, "coco2017", None, [("car", [10, 10, 6, 6]), ("car", [100, 100, 12, 12]),
                                                 ("car", [200, 200, 30, 30])])])
        r = em.score(gt, rows, [det(1, "car", [100, 100, 12, 12]), det(1, "car", [200, 200, 30, 30])], 0.35)
        s = r["sizes"]["car"]
        self.assertEqual((s["very_small"]["gt"], s["very_small"]["recall"]), (1, 0.0))
        self.assertEqual((s["small"]["gt"], s["small"]["recall"]), (1, 1.0))
        self.assertEqual((s["medium"]["gt"], s["medium"]["recall"]), (1, 1.0))
        self.assertEqual(s["large"]["gt"], 0)
        self.assertEqual(r["errors"]["missed_by_size"]["car"]["very_small"], 1)

    def test_no_detections(self):
        r = self.score([])
        self.assertEqual(r["per_class"]["car"]["ap50_95"], 0.0)


if __name__ == "__main__":
    unittest.main()
