#!/usr/bin/env python3
"""Focused tests of train_loop.py: schedule switch, best rule, metrics,
sampler position and, on real data, that stop + resume equals a straight
run bit for bit (CPU, 0 loader workers).

  cd tools/vision/training && python -m unittest discover -s tests -v

The integration tests need YOLOX importable (PYTHONPATH) and a prepared
dataset in DOORS_TRAIN_DATA (the smoke set is enough); they skip otherwise.
"""
import itertools
import json
import os
import random
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import torch

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
import train_loop as tl  # noqa: E402

try:
    import yolox  # noqa: F401
    HAVE_YOLOX = True
except ImportError:
    HAVE_YOLOX = False
DATA = os.environ.get("DOORS_TRAIN_DATA")
HAVE_DATA = bool(DATA) and (Path(DATA or ".") / "manifest.json").exists()


def upstream_no_aug(start_epoch, max_epoch, no_aug_epochs):
    """yolox/core/trainer.py, literally: which 0-based epochs run without
    mosaic and with L1 in a run that starts (or resumes) at start_epoch."""
    no_aug = start_epoch >= max_epoch - no_aug_epochs  # before_train
    closed, out = False, {}
    for epoch in range(start_epoch, max_epoch):
        if epoch + 1 == max_epoch - no_aug_epochs or no_aug:  # before_epoch
            closed = True
        out[epoch] = closed or no_aug
    return out


class Schedule(unittest.TestCase):
    def test_switch_matches_upstream_fresh_and_resumed(self):
        for m, k in ((300, 15), (5, 1), (4, 1), (10, 0), (3, 2)):
            fresh = upstream_no_aug(0, m, k)
            for e in range(m):
                self.assertEqual(tl.no_aug_epoch(e, m, k), fresh[e], (m, k, e))
            for s in range(1, m):  # every resume point agrees with the fresh run
                for e, v in upstream_no_aug(s, m, k).items():
                    self.assertEqual(v, fresh[e], (m, k, s, e))

    def test_real_recipe_switch_epoch(self):
        off = [e + 1 for e in range(300) if tl.no_aug_epoch(e, 300, 15)]
        self.assertEqual(off[0], 285)  # upstream's off-by-one: 16 epochs without mosaic
        self.assertEqual(len(off), 16)

    def test_schedule_of_matches_lr_function(self):
        if not HAVE_YOLOX:
            self.skipTest("yolox not importable")
        from yolox.utils import LRScheduler

        exp = SimpleNamespace(basic_lr_per_img=0.01 / 64, max_epoch=5, warmup_epochs=1, no_aug_epochs=1,
                              warmup_lr=0, min_lr_ratio=0.05, scheduler="yoloxwarmcos", eval_interval=1)
        ipe, batch = 12, 16
        s = tl.schedule_of(exp, ipe, batch)
        f = LRScheduler(exp.scheduler, s["lr"], ipe, exp.max_epoch, warmup_epochs=1, warmup_lr_start=0,
                        no_aug_epochs=1, min_lr_ratio=0.05).update_lr
        lrs = [f(i) for i in range(s["total_iters"] + 1)]
        warm = lrs[:s["warmup_iters"] + 1]
        self.assertTrue(all(b > a for a, b in zip(warm, warm[1:])))  # quadratic warmup rises
        self.assertAlmostEqual(lrs[s["warmup_iters"]], s["lr"])
        cos = lrs[s["warmup_iters"]:s["min_lr_from_iter"]]
        self.assertTrue(all(b < a for a, b in zip(cos, cos[1:])))  # cosine falls
        self.assertGreater(cos[-1], s["min_lr"])
        self.assertTrue(all(abs(x - s["min_lr"]) < 1e-12 for x in lrs[s["min_lr_from_iter"]:]))
        self.assertEqual(s["mosaic_epochs"], "1..3")
        self.assertEqual(s["no_mosaic_l1_epochs"], "4..5")


class Best(unittest.TestCase):
    def test_rule(self):
        self.assertTrue(tl.is_better(0.0, None))  # first validation, even at AP 0
        self.assertFalse(tl.is_better(0.0, 0.0))  # ties keep the earlier checkpoint
        self.assertFalse(tl.is_better(0.1, 0.2))
        self.assertTrue(tl.is_better(0.2, 0.1))

    def test_sequence(self):
        best, picks = None, []
        for epoch, ap in enumerate([0.0, 0.0, 0.05, 0.03, 0.05, 0.06], 1):
            if tl.is_better(ap, best):
                best, _ = ap, picks.append(epoch)
        self.assertEqual(picks, [1, 3, 6])


class Data(unittest.TestCase):
    def test_skip_sampler_continues_the_stream(self):
        if not HAVE_YOLOX:
            self.skipTest("yolox not importable")
        from yolox.data import InfiniteSampler

        full = list(itertools.islice(iter(InfiniteSampler(10, seed=7)), 40))
        for skip in (0, 3, 10, 17):
            got = list(itertools.islice(iter(tl.SkipSampler(InfiniteSampler(10, seed=7), skip)), 40 - skip))
            self.assertEqual(got, full[skip:])

    def test_is_mosaic(self):
        info = [torch.tensor([416, 416, 480]), torch.tensor([3, 640, 3])]
        self.assertEqual(tl.is_mosaic(info, (416, 416)).tolist(), [True, False, False])

    def test_rng_roundtrip(self):
        dev = torch.device("cpu")
        s = tl.rng_state(dev)
        a = (random.random(), np.random.rand(), torch.rand(1).item())
        tl.set_rng_state(s, dev)
        self.assertEqual(a, (random.random(), np.random.rand(), torch.rand(1).item()))


class Metrics(unittest.TestCase):
    def coco(self):
        from pycocotools.coco import COCO

        gt = COCO()
        gt.dataset = {"images": [{"id": 1, "width": 100, "height": 100}, {"id": 2, "width": 100, "height": 100}],
                      "categories": [{"id": 1, "name": "car"}, {"id": 2, "name": "bus"}],
                      "annotations": [
                          {"id": 1, "image_id": 1, "category_id": 1, "bbox": [10, 10, 20, 20], "area": 400, "iscrowd": 0},
                          {"id": 2, "image_id": 2, "category_id": 2, "bbox": [50, 50, 30, 30], "area": 900, "iscrowd": 0}]}
        gt.createIndex()
        return gt

    def test_precision_recall(self):
        dets = [{"image_id": 1, "category_id": 1, "bbox": [10, 10, 20, 20], "score": 0.9},   # TP
                {"image_id": 1, "category_id": 1, "bbox": [70, 70, 10, 10], "score": 0.8},   # FP
                {"image_id": 2, "category_id": 2, "bbox": [50, 50, 30, 30], "score": 0.2}]   # TP, low score
        m = tl.coco_metrics(self.coco(), dets, 0.5)
        self.assertEqual((m["tp"], m["fp"], m["gt"]), (1, 1, 2))
        self.assertAlmostEqual(m["precision"], 0.5)
        self.assertAlmostEqual(m["recall"], 0.5)
        m0 = tl.coco_metrics(self.coco(), dets, 0.0)
        self.assertAlmostEqual(m0["precision"], 2 / 3)
        self.assertAlmostEqual(m0["recall"], 1.0)
        self.assertAlmostEqual(m0["map50"], 1.0)
        self.assertEqual(set(m0["per_class"]), {"car", "bus"})
        self.assertAlmostEqual(m0["per_class"]["bus"]["ap50"], 1.0)

    def test_no_detections(self):
        m = tl.coco_metrics(self.coco(), [], 0.35)
        self.assertEqual((m["map50_95"], m["precision"], m["recall"]), (0.0, 0.0, 0.0))


@unittest.skipUnless(HAVE_YOLOX and HAVE_DATA, "needs YOLOX on PYTHONPATH and DOORS_TRAIN_DATA")
class Resume(unittest.TestCase):
    """4 epochs x 5 iterations on the CPU: warmup in epoch 1, a multiscale
    resize at step 10 (end of epoch 2), mosaic off and L1 on from epoch 3."""
    ARGS = ["--device", "cpu", "--batch", "2", "--workers", "0", "--threads", "4", "--max-epoch", "4",
            "--warmup-epochs", "1", "--no-aug-epochs", "1", "--eval-interval", "2", "--iters-per-epoch", "5"]

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.straight = cls.make_run("straight")
        cls.at_switch = cls.make_run("at_switch", stop=2)   # resumes into the switch epoch
        cls.in_no_aug = cls.make_run("in_no_aug", stop=3)   # resumes inside the no-mosaic phase

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @classmethod
    def train(cls, out, *extra):
        cmd = [sys.executable, str(HERE / "train_loop.py"), "--exp", str(HERE / "exps/yolox_tiny_traffic_416.py"),
               "--out", str(out)] + cls.ARGS + list(extra)
        r = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError("train_loop failed:\n" + r.stdout[-3000:] + r.stderr[-3000:])

    @classmethod
    def make_run(cls, name, stop=None):
        out = cls.root / name
        if stop:
            cls.train(out, "--stop-after-epoch", str(stop))
            assert not (out / "final_ckpt.pth").exists()
            cls.train(out, "--resume", "latest")
        else:
            cls.train(out)
        return out

    @staticmethod
    def epochs(out):
        return [json.loads(x) for x in (out / "epochs.jsonl").read_text().splitlines()]

    @staticmethod
    def ckpt(out, name="final_ckpt.pth"):
        return torch.load(out / name, map_location="cpu", weights_only=False)

    def assert_same_state(self, a, b):
        for key in ("train_model", "model"):
            self.assertEqual(a[key].keys(), b[key].keys())
            for k in a[key]:
                self.assertTrue(torch.equal(a[key][k], b[key][k]), "%s.%s differs" % (key, k))
        sa, sb = a["optimizer"]["state"], b["optimizer"]["state"]
        self.assertEqual(sa.keys(), sb.keys())
        for i in sa:
            self.assertTrue(torch.equal(sa[i]["momentum_buffer"], sb[i]["momentum_buffer"]), "momentum %d" % i)
        self.assertEqual([g["lr"] for g in a["optimizer"]["param_groups"]],
                         [g["lr"] for g in b["optimizer"]["param_groups"]])
        for k in ("start_epoch", "ema_updates", "samples_seen", "input_size", "use_l1", "best_ap", "best_epoch"):
            self.assertEqual(a[k], b[k], k)

    def test_resume_equals_straight_run(self):
        ref = self.ckpt(self.straight)
        for out in (self.at_switch, self.in_no_aug):
            self.assert_same_state(ref, self.ckpt(out))
            drop = ("seconds", "images_per_s", "steady_images_per_s", "memory", "written")
            strip = lambda es: [{k: v for k, v in e.items() if k not in drop and k != "metrics"} for e in es]  # noqa
            self.assertEqual(strip(self.epochs(self.straight)), strip(self.epochs(out)))

    def test_resume_continues_progress(self):
        mid = self.epochs(self.at_switch)
        self.assertEqual([e["epoch"] for e in mid], [1, 2, 3, 4])  # next epoch, no restart
        self.assertEqual(mid[2]["ema_updates"], 15)
        self.assertEqual(mid[2]["lr_first_step"], mid[1]["lr_next"])  # lr continues, warmup not redone
        run = json.loads((self.at_switch / "run.json").read_text())
        self.assertEqual([s["start_epoch"] for s in run["sessions"]], [1, 3])
        self.assertEqual(run["sessions"][1]["resumed_from"]["start_epoch"], 2)
        self.assertEqual(self.ckpt(self.at_switch, "last_mosaic_epoch_ckpt.pth")["start_epoch"], 2)

    def test_phases(self):
        es = self.epochs(self.straight)
        self.assertEqual([e["mosaic_samples"] for e in es[2:]], [0, 0])
        self.assertTrue(all(e["mosaic_samples"] == e["samples"] for e in es[:2]))
        self.assertEqual([e["use_l1"] for e in es], [False, False, True, True])
        self.assertEqual([e["l1_loss_mean"] > 0 for e in es], [False, False, True, True])
        self.assertEqual(es[0]["lr_first_step"], 0)
        self.assertEqual([e["metrics"] is not None for e in es], [False, True, True, True])  # interval 2, then 1
        self.assertTrue(all(e["metrics"]["weights_unchanged"] for e in es if e["metrics"]))
        self.assertEqual(es[0]["sizes"], [416])
        self.assertGreater(len({s for e in es for s in e["sizes"]}), 1)  # multiscale resized once

    def test_checkpoints_and_run_json(self):
        for out in (self.straight, self.at_switch):
            for name in ("latest_ckpt.pth", "best_ckpt.pth", "final_ckpt.pth", "last_mosaic_epoch_ckpt.pth"):
                self.assertTrue((out / name).exists(), name)
            run = json.loads((out / "run.json").read_text())
            self.assertTrue(run["completed"])
            self.assertEqual(run["best_checkpoint"]["metric"], "map50_95")
            self.assertEqual(run["best_checkpoint"]["epoch"], self.ckpt(out, "best_ckpt.pth")["start_epoch"])
            self.assertEqual(set(run["checkpoints"]), {"latest_ckpt.pth", "best_ckpt.pth", "final_ckpt.pth",
                                                       "last_mosaic_epoch_ckpt.pth"})
            for k in ("seed", "image_size", "multiscale", "batch", "epochs", "optimizer", "lr", "amp", "schedule",
                      "class_mapping", "validation", "doors_tree", "yolox", "torch", "dataset"):
                self.assertIn(k, run)

    def test_eval_only_reproduces_checkpoint_metrics(self):
        ck = self.ckpt(self.straight)
        r = subprocess.run([sys.executable, str(HERE / "train_loop.py"), "--exp",
                            str(HERE / "exps/yolox_tiny_traffic_416.py"), "--eval-only",
                            str(self.straight / "final_ckpt.pth"), "--device", "cpu", "--batch", "2",
                            "--workers", "0"], cwd=HERE, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])
        m = json.loads(r.stdout[r.stdout.index("{\n"):])
        for k in ("map50_95", "map50", "precision", "recall"):
            self.assertAlmostEqual(m[k], ck["metrics"][k], places=6)


if __name__ == "__main__":
    unittest.main()
