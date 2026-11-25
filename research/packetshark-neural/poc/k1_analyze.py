"""K1 analysis on the host: device outputs against the simulator and the float
model, judged as the product would use them (nearest other conversation in
the same 64-conversation batch), plus a summary of the on-unit JSON lines.

usage: k1_analyze.py KIT_DIR [LOG_DIR] [OUT_JSON]
  KIT_DIR  holds float.f32, sim_u8.f32, sim_a16.f32 and optionally the device
           dumps kpu_u8.f32 / kpu_a16.f32 pulled from /tmp/k1/out
  LOG_DIR  holds the step-*.jsonl files copied from /tmp/k1/out
Labels come from runs/e3b/dna_meta.npz (yte; the 32 bench batches are the
first 2,048 test conversations in order, see exp_dna.py).
Research code; nothing here runs on a unit.
"""
import json
import sys
from pathlib import Path

import numpy as np

B, D, H = 32, 32, 64


def load(p):
    x = np.fromfile(p, dtype="<f4")
    assert x.size == B * D * H, (p, x.size)
    return x.reshape(B, D, H).transpose(0, 2, 1)  # [batch][conversation][dim]


def unit(x):
    return x / (np.linalg.norm(x, axis=-1, keepdims=True) + 1e-12)


def nn(x):
    s = np.einsum("bhd,bjd->bhj", unit(x), unit(x))
    idx = np.arange(H)
    s[:, idx, idx] = -np.inf
    return s.argmax(-1), s


def compare(a, b, y):
    ca = np.sum(unit(a) * unit(b), -1).ravel()
    na, _ = nn(a)
    nb, sb = nn(b)
    # rank of a's neighbour among b's similarities (0 = b's nearest too)
    picked = np.take_along_axis(sb, na[..., None], -1)[..., 0]
    nearer = (sb > picked[..., None]).sum(-1)
    yb = y.reshape(B, H)
    cls_a = np.take_along_axis(yb, na, -1)
    cls_b = np.take_along_axis(yb, nb, -1)
    return {
        "bit_identical_share": float(np.mean(a.ravel() == b.ravel())),
        "max_abs_diff": float(np.abs(a - b).max()),
        "cos_min": float(ca.min()), "cos_p1": float(np.percentile(ca, 1)), "cos_mean": float(ca.mean()),
        "nn_same": float(np.mean(nearer == 0)), "nn_top3": float(np.mean(nearer < 3)),
        "nn_class_same": float(np.mean(cls_a == cls_b)),
        "p_at_1_a": float(np.mean(cls_a == yb)), "p_at_1_b": float(np.mean(cls_b == yb)),
        "nn_flip_class_change": float(np.mean((na != nb) & (cls_a != cls_b))),
    }


def jsonl(p):
    out = []
    for line in Path(p).read_text(errors="replace").splitlines():
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return out


def main():
    kit = Path(sys.argv[1])
    logs = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    meta = np.load(Path.home() / "work/psk-neural/runs/e3b/dna_meta.npz")
    y = meta["yte"][:B * H]
    res = {"labels_classes_in_batches": int(len(np.unique(y)))}
    f = load(kit / "float.f32")
    pairs = {"float_vs_sim_u8": ("float.f32", "sim_u8.f32"), "float_vs_sim_a16": ("float.f32", "sim_a16.f32"),
             "sim_u8_vs_device_u8": ("sim_u8.f32", "kpu_u8.f32"),
             "sim_a16_vs_device_a16": ("sim_a16.f32", "kpu_a16.f32"),
             "float_vs_device_u8": ("float.f32", "kpu_u8.f32"), "float_vs_device_a16": ("float.f32", "kpu_a16.f32")}
    for k, (a, b) in pairs.items():
        if (kit / a).exists() and (kit / b).exists():
            res[k] = compare(load(kit / a), load(kit / b), y)
    res["float_p_at_1"] = float(np.mean(np.take_along_axis(y.reshape(B, H), nn(f)[0], -1) == y.reshape(B, H)))
    if logs:
        steps = {}
        for p in sorted(logs.glob("*.jsonl")):
            steps[p.stem] = jsonl(p)
        res["steps"] = steps
    out = Path(sys.argv[3]) if len(sys.argv) > 3 else None
    text = json.dumps(res, indent=1)
    if out:
        out.write_text(text)
    print(json.dumps({k: v for k, v in res.items() if k != "steps"}, indent=1))


if __name__ == "__main__":
    main()
