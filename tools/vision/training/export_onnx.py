#!/usr/bin/env python3
"""Export a DOORS-trained YOLOX checkpoint to ONNX in the layout DOORS reads.

Output: one float tensor [1, 84, rows] - per row a box centre and size in
input pixels, then 80 scores in the COCO class order of
core/pocketvision/vision_labels.c. The class convs are widened to 80
outputs: the N trained classes sit at their COCO indices
(data/class_sets.py), every other class scores about 1e-13
(widen_cls_preds). That is the layout the
2026-10-04 evaluation fed to the unchanged DOORS decoder, so vision_decode,
vision_labels and vision_traffic need no change.

Input: float NCHW BGR 0..255 (YOLOX's own, legacy=False). The kmodel's
preprocessing turns DOORS' u8 RGB into that (compile_kmodel.py: swapRB).

The decode (grid + stride, exp on size, obj * cls) is in the graph; the
check at the end compares the ONNX against PyTorch on random input.

Usage: export_onnx.py --exp EXP.py --ckpt CKPT.pth --out OUT.onnx [--size 416]
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
import onnx
import torch
import torch.nn as nn

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "data"))
import class_sets  # noqa: E402


# The ONNX op set of upstream YOLOX-Tiny 416 exported the same way on
# 2026-10-04; that graph compiled to an a16 kmodel that ran at 53 ms on
# unit B's KPU. A new op here is a new KPU-placement risk, so it is refused.
UPSTREAM_OPS = {"Add", "Concat", "Conv", "Exp", "MaxPool", "Mul", "Reshape", "Resize", "Sigmoid", "Slice",
                "Transpose"}


def grid_points(size, strides=(8, 16, 32)):
    pts = []
    for s in strides:
        h = w = size // s
        ys, xs = torch.meshgrid(torch.arange(h), torch.arange(w), indexing="ij")
        pts.append(torch.stack([xs.reshape(-1), ys.reshape(-1), torch.full((h * w,), float(s))], 1))
    return torch.cat(pts, 0).float()  # [rows, 3]: cell x, cell y, stride


# Bias of the class rows a DOORS model was not trained for: sigmoid(-30) is
# 9.4e-14, far below any threshold, and the logit range stays small enough
# for int16 activations.
UNTRAINED_BIAS = -30.0


def widen_cls_preds(head, doors_index):
    """Give each level's class conv 80 outputs in COCO order: trained row i
    goes to row doors_index[i]; every other row has zero weights and bias
    UNTRAINED_BIAS. The graph then has upstream's exact shape and ops (the
    2026-10-04 kmodel that ran on the KPU) - no scatter tail at all.
    Three tails were tried and dropped on 2026-10-04: a one-hot MatMul (an op
    upstream never had), a stored [1, rows, 80] zero constant (+1.1 MB in
    the kmodel) and x*0 columns joined by Concat (nncase still compiling
    after 20 minutes, against 4)."""
    new = nn.ModuleList()
    for conv in head.cls_preds:
        w = torch.zeros(80, *conv.weight.shape[1:])
        b = torch.full((80,), UNTRAINED_BIAS)
        for label, idx in enumerate(doors_index):
            w[idx] = conv.weight.data[label]
            b[idx] = conv.bias.data[label]
        c = nn.Conv2d(conv.in_channels, 80, conv.kernel_size, conv.stride, conv.padding)
        c.weight.data.copy_(w)
        c.bias.data.copy_(b)
        new.append(c)
    head.cls_preds = new
    head.num_classes = 80


class DoorsYolox(nn.Module):
    def __init__(self, model, size, doors_index):
        super().__init__()
        model.eval()
        model.head.decode_in_inference = False  # raw head; decoded below
        widen_cls_preds(model.head, doors_index)
        self.m = model
        self.register_buffer("pts", grid_points(size))

    def forward(self, x):
        o = self.m(x)  # [1, rows, 85]: reg(4) obj(1) cls(80); obj/cls already sigmoid'd
        stride = self.pts[:, 2:3]
        xy = (o[..., 0:2] + self.pts[:, 0:2]) * stride
        wh = torch.exp(o[..., 2:4]) * stride
        sc = o[..., 4:5] * o[..., 5:]
        return torch.cat([xy, wh, sc], -1).permute(0, 2, 1)


def sha256(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exp", required=True)
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--size", type=int, default=416)
    args = ap.parse_args()

    from yolox.exp import get_exp

    exp = get_exp(args.exp, None)
    model = exp.get_model()
    ck = torch.load(args.ckpt, map_location="cpu")
    model.load_state_dict(ck["model"])
    doors_index = class_sets.doors_indices(exp.class_set)
    if len(doors_index) != exp.num_classes:
        raise SystemExit("class set %s has %d classes, the exp %d" % (exp.class_set, len(doors_index),
                                                                       exp.num_classes))
    torch.manual_seed(0)
    x = torch.rand(1, 3, args.size, args.size) * 255
    # The trained N-class head's own scores, before widening.
    model.eval()
    model.head.decode_in_inference = False
    with torch.no_grad():
        o = model(x)
        native = (o[..., 4:5] * o[..., 5:]).permute(0, 2, 1).numpy()[0]  # [N, rows]
    mod = DoorsYolox(model, args.size, doors_index).eval()
    with torch.no_grad():
        ref = mod(x).numpy()
    torch.onnx.export(mod, x, args.out, opset_version=11, input_names=["images"],
                      output_names=["output0"], do_constant_folding=True)
    import onnxsim

    m, ok = onnxsim.simplify(onnx.load(args.out))
    if not ok:
        raise SystemExit("onnxsim could not validate the simplified graph")
    onnx.save(m, args.out)

    import onnxruntime as ort

    s = ort.InferenceSession(args.out, providers=["CPUExecutionProvider"])
    got = s.run(None, {"images": x.numpy()})[0]
    rows = sum((args.size // st) ** 2 for st in (8, 16, 32))
    assert got.shape == (1, 84, rows), got.shape
    others = np.delete(got[0, 4:], doors_index, axis=0)
    rec = {
        "onnx": args.out,
        "onnx_sha256": sha256(args.out),
        "ckpt_sha256": sha256(args.ckpt),
        "exp": args.exp,
        "class_set": exp.class_set,
        "doors_index": doors_index,
        "shape": list(got.shape),
        "max_abs_diff_vs_torch": float(np.abs(got - ref).max()),
        "max_abs_diff_trained_vs_native_head": float(np.abs(got[0, 4:][doors_index] - native).max()),
        "untrained_scores_max": float(others.max()),
        "ops": sorted({n.op_type for n in m.graph.node}),
        "opset": 11,
        "torch": torch.__version__, "onnx_version": onnx.__version__, "onnxruntime": ort.__version__,
        "onnxsim": onnxsim.__version__,
    }
    rec["ops_beyond_upstream"] = sorted(set(rec["ops"]) - UPSTREAM_OPS)
    if (rec["max_abs_diff_vs_torch"] > 1e-3 or rec["max_abs_diff_trained_vs_native_head"] > 1e-3
            or rec["untrained_scores_max"] > 1e-6):
        print(json.dumps(rec, indent=1))
        raise SystemExit("ONNX does not match PyTorch")
    if rec["ops_beyond_upstream"]:
        print(json.dumps(rec, indent=1))
        raise SystemExit("the graph has ops the proven upstream kmodel did not: %s" % rec["ops_beyond_upstream"])
    Path(args.out + ".json").write_text(json.dumps(rec, indent=1))
    print(json.dumps(rec, indent=1))


if __name__ == "__main__":
    main()
