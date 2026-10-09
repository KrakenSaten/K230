"""Focused tests of neg_obj_mask.py (experiment R1-A), on the CPU, no dataset.

Loss: only negative objectness terms of ignored anchors drop out; box, class,
L1 and positive objectness terms are bit-identical; normalisation (num_fg)
is unchanged; no ignore reproduces YOLOX's loss exactly.
Data: the masked dataset gives byte-identical images and labels to YOLOX's
MosaicDetection under the same seeds, and its mask matches the pixels'
source through mosaic, affine, HSV, flip, letterbox, the no-mosaic path and
multiscale resizing."""
import random
import sys
import unittest
from pathlib import Path

import cv2
import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import neg_obj_mask as nom  # noqa: E402
from yolox.data import TrainTransform  # noqa: E402
from yolox.data.datasets.datasets_wrapper import Dataset  # noqa: E402
from yolox.data.datasets.mosaicdetection import MosaicDetection  # noqa: E402
from yolox.models import YOLOPAFPN, YOLOX, YOLOXHead  # noqa: E402

RED, BLUE = (0, 0, 200), (200, 0, 0)  # BGR: flagged images red, others blue


# ---------------------------------------------------------------- loss

def tiny_model(use_l1):
    torch.manual_seed(0)
    backbone = YOLOPAFPN(0.33, 0.25, in_channels=[256, 512, 1024])
    head = YOLOXHead(6, 0.25, in_channels=[256, 512, 1024])
    m = YOLOX(backbone, head)
    m.train()
    m.head.use_l1 = use_l1
    return m


def fixed_batch():
    g = torch.Generator().manual_seed(1)
    x = torch.rand(2, 3, 128, 128, generator=g) * 255
    t = torch.zeros(2, 10, 5)  # cls, cx, cy, w, h
    t[0, :3] = torch.tensor([[0, 30, 30, 20, 16], [5, 80, 60, 24, 40], [1, 100, 100, 30, 20]], dtype=torch.float)
    t[1, :2] = torch.tensor([[2, 64, 64, 50, 40], [5, 20, 100, 12, 30]], dtype=torch.float)
    return x, t


class Recorder(torch.nn.Module):
    """Passes through and keeps the objectness call's inputs."""

    def __init__(self, inner):
        super().__init__()
        self.inner, self.obj = inner, None

    def forward(self, inp, target):
        if inp.shape[-1] == 1:
            self.obj = (inp.detach().clone(), target.detach().clone())
        return self.inner(inp, target)


def losses(model, x, t):
    with torch.no_grad():
        o = model(x, t)
    return {k: float(o[k]) for k in ("total_loss", "iou_loss", "conf_loss", "cls_loss", "l1_loss")}


class Loss(unittest.TestCase):
    def setUp(self):
        self.x, self.t = fixed_batch()

    def reference(self, use_l1):
        m = tiny_model(use_l1)
        rec = Recorder(m.head.bcewithlog_loss)
        m.head.bcewithlog_loss = rec
        ref = losses(m, self.x, self.t)
        return m, ref, rec.obj

    def masked(self, m, ignore):
        bce = nom.enable_head(m.head)
        bce.ignore = ignore
        return losses(m, self.x, self.t), bce

    def test_no_ignore_reproduces_yolox(self):
        for use_l1 in (False, True):
            m, ref, (inp, _) = self.reference(use_l1)
            m.head.bcewithlog_loss = m.head.bcewithlog_loss.inner
            got, _ = self.masked(m, None)
            self.assertEqual(got, ref)
            zero = torch.zeros(2, inp.shape[0] // 2, dtype=torch.bool)
            m.head.bcewithlog_loss.ignore = zero
            self.assertEqual(losses(m, self.x, self.t), ref)

    def test_only_ignored_negative_objectness_terms_drop(self):
        for use_l1 in (False, True):
            m, ref, (inp, tgt) = self.reference(use_l1)
            m.head.bcewithlog_loss = m.head.bcewithlog_loss.inner
            n = inp.shape[0] // 2
            ign = torch.zeros(2, n, dtype=torch.bool)
            ign[0, : n // 2] = True  # half of image 0's anchors: includes positives
            ign[1, ::3] = True
            got, bce = self.masked(m, ign)
            per = torch.nn.functional.binary_cross_entropy_with_logits(inp, tgt, reduction="none")
            drop = ign.reshape(-1, 1) & (tgt == 0)
            num_fg = float(tgt.sum())  # obj target = fg mask; YOLOX divides by num_fg
            self.assertGreater(int((ign.reshape(-1, 1) & (tgt == 1)).sum()), 0, "test must cover ignored positives")
            self.assertAlmostEqual(got["conf_loss"], ref["conf_loss"] - float(per[drop].sum()) / num_fg, places=4)
            for k in ("iou_loss", "cls_loss", "l1_loss"):
                self.assertEqual(got[k], ref[k], k)  # bit-identical
            self.assertEqual(bce.last["ignored"], int(drop.sum()))

    def test_ignored_positives_keep_their_objectness(self):
        m, ref, (inp, tgt) = self.reference(False)
        m.head.bcewithlog_loss = m.head.bcewithlog_loss.inner
        got, _ = self.masked(m, torch.ones(2, inp.shape[0] // 2, dtype=torch.bool))
        per = torch.nn.functional.binary_cross_entropy_with_logits(inp, tgt, reduction="none")
        self.assertAlmostEqual(got["conf_loss"], float(per[tgt == 1].sum()) / float(tgt.sum()), places=5)

    def test_anchor_order_matches_head(self):
        m = tiny_model(False)
        seen = {}
        orig = m.head.get_losses

        def spy(imgs, xs, ys, strides, *a, **k):
            seen.update(x=torch.cat(xs, 1)[0], y=torch.cat(ys, 1)[0], s=torch.cat(strides, 1)[0])
            return orig(imgs, xs, ys, strides, *a, **k)

        m.head.get_losses = spy
        losses(m, self.x, self.t)
        x, y = nom.anchor_centres(m.head, (128, 128), "cpu")
        self.assertTrue(torch.equal(x, ((seen["x"] + 0.5) * seen["s"]).long()))
        self.assertTrue(torch.equal(y, ((seen["y"] + 0.5) * seen["s"]).long()))


# ---------------------------------------------------------------- data

class FakeSet(Dataset):
    """Solid-colour pictures of random sizes with a few boxes; even ids flagged (red)."""

    def __init__(self, n=12, dim=(128, 128)):
        super().__init__(dim)
        rng = np.random.RandomState(7)
        self.items = []
        for i in range(n):
            h, w = int(rng.randint(60, 200)), int(rng.randint(60, 200))
            img = np.full((h, w, 3), RED if i % 2 == 0 else BLUE, np.uint8)
            k = 0 if i == 3 else int(rng.randint(1, 4))  # one picture without boxes
            boxes = []
            for _ in range(k):
                x1, y1 = rng.randint(0, w - 20), rng.randint(0, h - 20)
                boxes.append([x1, y1, x1 + rng.randint(8, 20), y1 + rng.randint(8, 20), rng.randint(0, 6)])
            self.items.append((img, np.array(boxes, np.float64).reshape(-1, 5), (h, w), np.array([i])))

    def __len__(self):
        return len(self.items)

    def pull_item(self, index):
        img, lab, info, iid = self.items[index]
        return img.copy(), lab.copy(), info, iid


def make(cls, mosaic=True):
    return cls(FakeSet(), img_size=(128, 128), mosaic=mosaic,
               preproc=TrainTransform(max_labels=120, flip_prob=0.5, hsv_prob=1.0),
               degrees=10.0, translate=0.1, mosaic_scale=(0.5, 1.5), mixup_scale=(0.5, 1.5), shear=2.0,
               enable_mixup=False, mosaic_prob=1.0, mixup_prob=1.0)


def source_of(img_chw):
    """Per pixel: 1 red (flagged), 0 blue, -1 grey padding / mixed edge."""
    b, r = img_chw[0].astype(int), img_chw[2].astype(int)
    out = np.full(b.shape, -1)
    out[r > b + 60] = 1
    out[b > r + 60] = 0
    return out


def sample(ds, i, seed):
    random.seed(seed)
    np.random.seed(seed)
    return ds[i]


class Data(unittest.TestCase):
    def check_identical_and_mask(self, mosaic):
        up, ours = make(MosaicDetection, mosaic), make(MosaicDetection, mosaic)
        nom.enable_dataset(ours, {i for i in range(12) if i % 2 == 0})
        interior = mismatched = 0
        for seed in range(40):
            i = seed % 12
            a = sample(up, i, seed)
            b = sample(ours, i, seed)
            self.assertTrue(np.array_equal(a[0], b[0]), "image differs (seed %d)" % seed)
            self.assertTrue(np.array_equal(a[1], b[1]), "labels differ (seed %d)" % seed)
            self.assertEqual(a[2], b[2])
            mask, src = b[4], source_of(b[0])
            self.assertEqual(mask.shape, (128, 128))
            # compare away from colour edges (interpolation mixes two sources there)
            edge = cv2.dilate((cv2.Laplacian(src.astype(np.float32), cv2.CV_32F) != 0).astype(np.uint8),
                              np.ones((3, 3), np.uint8)) > 0
            ok = ~edge
            interior += int(ok.sum())
            mismatched += int(((mask == 1) != (src == 1))[ok].sum())
        self.assertGreater(interior, 40 * 128 * 128 * 0.8)
        self.assertEqual(mismatched, 0)

    def test_mosaic_affine_hsv_flip_letterbox(self):
        self.check_identical_and_mask(True)

    def test_no_mosaic_phase(self):
        self.check_identical_and_mask(False)

    def test_close_mosaic_through_the_sampler_flag(self):
        ds = make(MosaicDetection, True)
        nom.enable_dataset(ds, {0})
        random.seed(0)
        np.random.seed(0)
        img, _, info, _, mask = ds[(False, 0)]  # what YoloBatchSampler sends after close_mosaic
        self.assertFalse(ds.enable_mosaic)
        self.assertEqual(info, ds._dataset.items[0][2])  # original (h, w): not a mosaic
        self.assertTrue((mask[source_of(img) == 1] == 1).all())

    def test_mixup_refused(self):
        ds = make(MosaicDetection, True)
        nom.enable_dataset(ds, set())
        ds.enable_mixup = True
        with self.assertRaises(RuntimeError):
            sample(ds, 0, 0)

    def test_multiscale_sampling(self):
        head = YOLOXHead(6, 0.25)
        mask = torch.zeros(1, 416, 416, dtype=torch.uint8)
        mask[:, :, :208] = 1  # left half from a flagged picture
        for size in ((320, 320), (416, 416), (640, 640)):
            ign = nom.anchor_ignore(mask, size, head, (416, 416))
            x, _ = nom.anchor_centres(head, size, "cpu")
            self.assertTrue(torch.equal(ign[0], x < size[1] / 2), size)


if __name__ == "__main__":
    unittest.main()
