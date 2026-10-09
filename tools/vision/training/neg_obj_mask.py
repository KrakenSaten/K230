"""Opt-in experiment R1-A: no background-objectness loss where a class is unknown.

An Open Images training image where Car was never human-verified can show
cars without boxes. YOLOX trains every anchor that SimOTA does not assign to a
box as background through the objectness loss, so such a car is taught as
"no object" (R0 finding, r0miss/FINDINGS.md). This module removes exactly those
terms and nothing else:

  ignored = negative objectness terms (target 0) of anchors whose centre lies
            in pixels that came from a flagged image

Positive anchors keep every loss (box, objectness, class, L1). Box, class and
L1 losses are never touched. YOLOX divides each loss by num_fg, which counts
positives only, so the remaining terms keep their weight.

YOLOX stays unpatched. When the option is off, train_loop.py uses YOLOX's own
classes and nothing here runs. When it is on:
  - MaskedMosaicDetection repeats MosaicDetection.__getitem__ and
    TrainTransform.__call__ (YOLOX 6ddff48, Apache-2.0) call for call, so the
    random draws, image and labels are identical, and carries a uint8 mask
    through the same tile placement, affine matrix, flip and letterbox.
  - IgnoringBCE replaces the head's BCE module (no parameters, so state_dict
    and checkpoints are unchanged) and zeroes the ignored objectness terms.
  - train_loop.py resizes the mask with the batch (multiscale, nearest) and
    samples it at each anchor centre.
Mixup is not supported (the exp disables it); the dataset refuses it.
"""
import hashlib
import json
import random
from pathlib import Path

import cv2
import numpy as np
import torch
import torch.nn as nn

from yolox.data.data_augment import augment_hsv, get_affine_matrix, apply_affine_to_bboxes, preproc
from yolox.data.datasets.datasets_wrapper import Dataset
from yolox.data.datasets.mosaicdetection import MosaicDetection, get_mosaic_coordinate
from yolox.utils import xyxy2cxcywh

RULES = {
    "oi-car-unverified": "Open Images training images whose human-verified labels do not hold Car "
                         "(present or absent): unboxed cars can be in them",
}


def flagged_ids(data_dir, rule):
    """Training image ids (json ids) the rule flags, read from manifest_train.jsonl."""
    if rule != "oi-car-unverified":
        raise ValueError("unknown rule %s" % rule)
    ids = []
    with open(Path(data_dir) / "manifest_train.jsonl", encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            if r["source"] == "openimages_v7" and "Car" not in (r.get("human_labels") or {}):
                ids.append(int(r["image_id"]))
    ids.sort()
    digest = hashlib.sha256(",".join(map(str, ids)).encode()).hexdigest()
    return set(ids), {"rule": rule, "meaning": RULES[rule], "images": len(ids), "ids_sha256": digest}


# ---- data: the mask follows every geometric step of the image

def _letterbox_mask(mask, input_dim):
    """preproc() for the mask: same r and top-left placement, nearest, pad 0."""
    out = np.zeros(input_dim, dtype=np.uint8)
    r = min(input_dim[0] / mask.shape[0], input_dim[1] / mask.shape[1])
    m = cv2.resize(mask, (int(mask.shape[1] * r), int(mask.shape[0] * r)), interpolation=cv2.INTER_NEAREST)
    out[: int(mask.shape[0] * r), : int(mask.shape[1] * r)] = m
    return out


def train_transform(tt, image, targets, input_dim, mask):
    """TrainTransform.__call__ of YOLOX 6ddff48 (tt holds its settings), with the
    mask. Same random draws in the same order (hsv: np.random, flip: random)."""
    boxes = targets[:, :4].copy()
    labels = targets[:, 4].copy()
    if len(boxes) == 0:
        targets = np.zeros((tt.max_labels, 5), dtype=np.float32)
        image, r_o = preproc(image, input_dim)
        return image, targets, _letterbox_mask(mask, input_dim)

    image_o = image.copy()
    mask_o = mask
    targets_o = targets.copy()
    boxes_o = targets_o[:, :4]
    labels_o = targets_o[:, 4]
    boxes_o = xyxy2cxcywh(boxes_o)

    if random.random() < tt.hsv_prob:
        augment_hsv(image)  # colour only, the mask is unaffected
    # _mirror(image, boxes, prob), with the mask flipped on the same draw
    _, width, _ = image.shape
    if random.random() < tt.flip_prob:
        image = image[:, ::-1]
        mask = mask[:, ::-1]
        boxes[:, 0::2] = width - boxes[:, 2::-2]
    image_t, r_ = preproc(image, input_dim)
    mask_t = _letterbox_mask(np.ascontiguousarray(mask), input_dim)
    boxes = xyxy2cxcywh(boxes)
    boxes *= r_

    mask_b = np.minimum(boxes[:, 2], boxes[:, 3]) > 1
    boxes_t = boxes[mask_b]
    labels_t = labels[mask_b]

    if len(boxes_t) == 0:
        image_t, r_o = preproc(image_o, input_dim)
        mask_t = _letterbox_mask(mask_o, input_dim)
        boxes_o *= r_o
        boxes_t = boxes_o
        labels_t = labels_o

    labels_t = np.expand_dims(labels_t, 1)
    targets_t = np.hstack((labels_t, boxes_t))
    padded_labels = np.zeros((tt.max_labels, 5))
    padded_labels[range(len(targets_t))[: tt.max_labels]] = targets_t[: tt.max_labels]
    padded_labels = np.ascontiguousarray(padded_labels, dtype=np.float32)
    return image_t, padded_labels, mask_t


class MaskedMosaicDetection(MosaicDetection):
    """MosaicDetection that also returns a uint8 [H, W] mask: 1 where the pixel
    came from a flagged image. Set `flagged` (a set of image ids) after the
    class swap. Without a flag set every mask is 0."""

    flagged = frozenset()

    def _flag(self, img_id):
        return 1 if int(np.asarray(img_id).reshape(-1)[0]) in self.flagged else 0

    @Dataset.mosaic_getitem
    def __getitem__(self, idx):
        if self.enable_mixup:
            raise RuntimeError("neg_obj_mask does not carry the mask through mixup")
        if self.enable_mosaic and random.random() < self.mosaic_prob:
            mosaic_labels = []
            input_dim = self._dataset.input_dim
            input_h, input_w = input_dim[0], input_dim[1]
            yc = int(random.uniform(0.5 * input_h, 1.5 * input_h))
            xc = int(random.uniform(0.5 * input_w, 1.5 * input_w))
            indices = [idx] + [random.randint(0, len(self._dataset) - 1) for _ in range(3)]
            mosaic_mask = np.zeros((input_h * 2, input_w * 2), dtype=np.uint8)
            for i_mosaic, index in enumerate(indices):
                img, _labels, _, img_id = self._dataset.pull_item(index)
                h0, w0 = img.shape[:2]
                scale = min(1. * input_h / h0, 1. * input_w / w0)
                img = cv2.resize(img, (int(w0 * scale), int(h0 * scale)), interpolation=cv2.INTER_LINEAR)
                (h, w, c) = img.shape[:3]
                if i_mosaic == 0:
                    mosaic_img = np.full((input_h * 2, input_w * 2, c), 114, dtype=np.uint8)
                (l_x1, l_y1, l_x2, l_y2), (s_x1, s_y1, s_x2, s_y2) = get_mosaic_coordinate(
                    mosaic_img, i_mosaic, xc, yc, w, h, input_h, input_w)
                mosaic_img[l_y1:l_y2, l_x1:l_x2] = img[s_y1:s_y2, s_x1:s_x2]
                mosaic_mask[l_y1:l_y2, l_x1:l_x2] = self._flag(img_id)
                padw, padh = l_x1 - s_x1, l_y1 - s_y1
                labels = _labels.copy()
                if _labels.size > 0:
                    labels[:, 0] = scale * _labels[:, 0] + padw
                    labels[:, 1] = scale * _labels[:, 1] + padh
                    labels[:, 2] = scale * _labels[:, 2] + padw
                    labels[:, 3] = scale * _labels[:, 3] + padh
                mosaic_labels.append(labels)
            if len(mosaic_labels):
                mosaic_labels = np.concatenate(mosaic_labels, 0)
                np.clip(mosaic_labels[:, 0], 0, 2 * input_w, out=mosaic_labels[:, 0])
                np.clip(mosaic_labels[:, 1], 0, 2 * input_h, out=mosaic_labels[:, 1])
                np.clip(mosaic_labels[:, 2], 0, 2 * input_w, out=mosaic_labels[:, 2])
                np.clip(mosaic_labels[:, 3], 0, 2 * input_h, out=mosaic_labels[:, 3])
            # random_affine(), with the mask warped by the same matrix
            target_size = (input_w, input_h)
            M, scale = get_affine_matrix(target_size, self.degrees, self.translate, self.scale, self.shear)
            mosaic_img = cv2.warpAffine(mosaic_img, M, dsize=target_size, borderValue=(114, 114, 114))
            mosaic_mask = cv2.warpAffine(mosaic_mask, M, dsize=target_size, flags=cv2.INTER_NEAREST, borderValue=0)
            if len(mosaic_labels) > 0:
                mosaic_labels = apply_affine_to_bboxes(mosaic_labels, target_size, M, scale)
            mix_img, padded_labels, mask = train_transform(self.preproc, mosaic_img, mosaic_labels,
                                                           self.input_dim, mosaic_mask)
            img_info = (mix_img.shape[1], mix_img.shape[0])
            return mix_img, padded_labels, img_info, img_id, mask
        else:
            self._dataset._input_dim = self.input_dim
            img, label, img_info, img_id = self._dataset.pull_item(idx)
            mask = np.full(img.shape[:2], self._flag(img_id), dtype=np.uint8)
            img, label, mask = train_transform(self.preproc, img, label, self.input_dim, mask)
            return img, label, img_info, img_id, mask


def enable_dataset(dataset, flagged):
    """Swap a MosaicDetection instance to MaskedMosaicDetection in place, so the
    loader built by the exp (sampler, workers, collate) stays as it is."""
    if type(dataset) is not MosaicDetection:
        raise TypeError("expected YOLOX MosaicDetection, got %s" % type(dataset).__name__)
    dataset.__class__ = MaskedMosaicDetection
    dataset.flagged = frozenset(flagged)


# ---- loss: zero the ignored negative objectness terms

class IgnoringBCE(nn.Module):
    """Stands in for YOLOXHead.bcewithlog_loss (BCEWithLogitsLoss, reduction
    none). The objectness call is the one with a single column
    ([batch * anchors, 1]); the class call has num_classes columns and passes
    through unchanged. `ignore` is a bool [batch, anchors] set per step."""

    def __init__(self, inner):
        super().__init__()
        self.inner = inner
        self.ignore = None
        self.last = None  # {"ignored": n, "negatives": n, "anchors": n} of the last objectness call

    def forward(self, inp, target):
        loss = self.inner(inp, target)
        if self.ignore is None or inp.shape[-1] != 1:
            return loss
        ign = self.ignore.reshape(-1, 1)
        if ign.shape[0] != inp.shape[0]:
            raise RuntimeError("ignore mask has %d anchors, objectness %d" % (ign.shape[0], inp.shape[0]))
        drop = ign & (target == 0)
        self.last = {"ignored": int(drop.sum()), "negatives": int((target == 0).sum()), "anchors": int(inp.shape[0])}
        return loss.masked_fill(drop, 0.0)


def enable_head(head):
    if head.num_classes == 1:
        raise RuntimeError("one class: the objectness and class BCE calls cannot be told apart")
    if isinstance(head.bcewithlog_loss, IgnoringBCE):
        return head.bcewithlog_loss
    head.bcewithlog_loss = IgnoringBCE(head.bcewithlog_loss)
    return head.bcewithlog_loss


def anchor_centres(head, tsize, device):
    """Anchor centres in input pixels, in the head's order: levels by stride,
    each row-major (YOLOXHead.get_output_and_grid); centre = (cell + 0.5) * stride."""
    xs, ys = [], []
    for s in head.strides:
        h, w = tsize[0] // s, tsize[1] // s
        yv, xv = torch.meshgrid(torch.arange(h, device=device), torch.arange(w, device=device), indexing="ij")
        xs.append(((xv.reshape(-1) + 0.5) * s).long())
        ys.append(((yv.reshape(-1) + 0.5) * s).long())
    return torch.cat(xs), torch.cat(ys)


def anchor_ignore(mask, tsize, head, base_size):
    """mask: uint8 [B, H, W] at base_size (the dataset's input_dim). Resize it like
    exp.preprocess resizes the batch (to tsize; nearest for a mask), sample at
    each anchor centre. Returns bool [B, anchors]."""
    m = mask.float().unsqueeze(1)
    if tuple(tsize) != tuple(base_size):
        m = nn.functional.interpolate(m, size=tuple(tsize), mode="nearest")
    m = m[:, 0] > 0.5
    x, y = anchor_centres(head, tsize, m.device)
    return m[:, y.clamp(max=tsize[0] - 1), x.clamp(max=tsize[1] - 1)]
