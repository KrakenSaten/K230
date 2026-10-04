#!/usr/bin/env python3
"""Compile the exported ONNX to a K230 kmodel with nncase 2.11.0, the proven
configuration of the 2026-10-04 evaluation: int16 activations, uint8 weights
(a16/w8). uint8 activations collapse YOLOX (0 vehicles found), int16 weights
fall back to the CPU (49.7 s per inference).

Input contract = what DOORS' AI2D gives today (vision_kpu_nncase.cpp): u8
NCHW RGB, letterboxed into the top-left corner, padded 114. nncase's
preprocessing swaps to BGR; YOLOX needs no mean/std (0..255 in).

Calibration: --calib-list, one image path per line, from the TRAINING split
(never val or test). The list's sha256 and each file's sha256 go into the
record, so the PTQ input is pinned.

Checks recorded in OUT.kmodel.json:
  - simulator vs float ONNX on --check-images (score/box cosine, max error)
  - determinism: with --twice the model is compiled again and the two
    kmodels' sha256 compared
  - CPU-fallback heuristic against --reference (the upstream YOLOX-Tiny 416
    a16 kmodel that ran at 53 ms on unit B): the size ratio (the a16w16
    build that fell back to the CPU was 3.4x the a16 one). The header word
    at 0x38 is recorded for information. The kmodel format is not
    documented, so this is ASSUMED, not proof; the proof is the unit B KPU
    timing gate.

Usage: compile_kmodel.py --onnx IN.onnx --out OUT.kmodel --calib-list FILE
           [--size 416] [--check-images FILE] [--twice] [--reference K]
"""
import argparse
import hashlib
import importlib.metadata
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

import cv2
import numpy as np


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def letterbox(rgb, size):
    """RGB HxWx3 -> u8 NCHW RGB [1,3,size,size], top-left, pad 114 (DOORS AI2D)."""
    h, w = rgb.shape[:2]
    r = min(size / w, size / h)
    nw, nh = int(round(w * r)), int(round(h * r))
    out = np.full((size, size, 3), 114, np.uint8)
    out[:nh, :nw] = cv2.resize(rgb, (nw, nh), interpolation=cv2.INTER_LINEAR)
    return out.transpose(2, 0, 1)[None].copy()


def read_rgb(p):
    bgr = cv2.imread(str(p))
    if bgr is None:
        raise SystemExit("cannot read %s" % p)
    return cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)


def compile_once(onnx_bytes, size, calib, dump_dir):
    import nncase

    co = nncase.CompileOptions()
    co.target = "k230"
    co.preprocess = True
    co.swapRB = True
    co.input_shape = [1, 3, size, size]
    co.input_type = "uint8"
    co.input_range = [0, 255]
    co.mean = [0, 0, 0]
    co.std = [1, 1, 1]
    co.input_layout = "NCHW"
    co.letterbox_value = 0.0
    co.dump_ir = False
    co.dump_asm = False
    co.dump_dir = str(dump_dir)
    comp = nncase.Compiler(co)
    comp.import_onnx(onnx_bytes, nncase.ImportOptions())
    po = nncase.PTQTensorOptions()
    po.quant_type = "int16"
    po.w_quant_type = "uint8"
    po.calibrate_method = "NoClip"
    po.finetune_weights_method = "NoFineTuneWeights"
    po.dump_quant_error = False
    po.dump_quant_error_symmetric_for_signed = False
    po.samples_count = len(calib)
    po.set_tensor_data([calib])
    comp.use_ptq(po)
    comp.compile()
    return comp.gencode_tobytes()


def sim_run(kbytes, x):
    import nncase

    sim = nncase.Simulator()
    sim.load_model(kbytes)
    sim.set_input_tensor(0, nncase.RuntimeTensor.from_numpy(x))
    sim.run()
    return sim.get_output_tensor(0).to_numpy()


def cosine(a, b):
    """Over the entries finite in both (an undertrained head can overflow
    exp() in float); the count of the others is reported apart."""
    a, b = a.ravel().astype(np.float64), b.ravel().astype(np.float64)
    ok = np.isfinite(a) & np.isfinite(b)
    a, b = a[ok], b[ok]
    return float(a @ b / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-12))


def nonfinite(a):
    return int(np.size(a) - np.isfinite(a).sum())


def header_word(b, off=0x38):
    return struct.unpack_from("<I", b, off)[0] if len(b) >= off + 4 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--calib-list", required=True)
    ap.add_argument("--size", type=int, default=416)
    ap.add_argument("--check-images", help="list of images (val/test) for the simulator check")
    ap.add_argument("--twice", action="store_true", help="compile twice and compare (determinism)")
    ap.add_argument("--reference", help="known-good kmodel for the CPU-fallback heuristic")
    args = ap.parse_args()

    import nncase
    import onnxruntime as ort

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    onnx_bytes = Path(args.onnx).read_bytes()
    calib_files = [l.strip() for l in Path(args.calib_list).read_text().splitlines() if l.strip()]
    calib = [letterbox(read_rgb(p), args.size) for p in calib_files]

    t0 = time.time()
    kb = compile_once(onnx_bytes, args.size, calib, out.parent / (out.stem + "_dump"))
    ct = time.time() - t0
    out.write_bytes(kb)

    try:
        # nncase needs only the .NET runtime, not an SDK: list runtimes.
        dotnet = subprocess.run(["dotnet", "--list-runtimes"], capture_output=True, text=True).stdout.strip()
    except OSError:
        dotnet = None
    rec = {
        "kmodel": str(out),
        "kmodel_sha256": sha256_bytes(kb),
        "kmodel_bytes": len(kb),
        "onnx_sha256": sha256_bytes(onnx_bytes),
        "nncase": importlib.metadata.version("nncase"),
        "nncase_kpu": importlib.metadata.version("nncase-kpu"),
        "dotnet": dotnet,
        "config": {"target": "k230", "input": "u8 NCHW RGB %dx%d, letterbox top-left pad 114" % (args.size, args.size),
                   "swapRB": True, "mean": [0, 0, 0], "std": [1, 1, 1],
                   "quant_type": "int16", "w_quant_type": "uint8", "calibrate_method": "NoClip",
                   "finetune_weights_method": "NoFineTuneWeights"},
        "calibration": {"list": args.calib_list,
                        "list_sha256": sha256_bytes(Path(args.calib_list).read_bytes()),
                        "count": len(calib_files),
                        "files_sha256": sha256_bytes("\n".join(
                            sha256_bytes(Path(p).read_bytes()) for p in calib_files).encode())},
        "compile_s": round(ct, 1),
        "header_word_0x38": header_word(kb),
    }

    if args.twice:
        kb2 = compile_once(onnx_bytes, args.size, calib, out.parent / (out.stem + "_dump2"))
        rec["deterministic"] = sha256_bytes(kb2) == rec["kmodel_sha256"]
        rec["second_compile_sha256"] = sha256_bytes(kb2)

    if args.reference:
        ref = Path(args.reference).read_bytes()
        rec["reference"] = {"file": args.reference, "bytes": len(ref), "header_word_0x38": header_word(ref),
                            "size_ratio": round(len(kb) / len(ref), 3)}
        # ASSUMED heuristic, see the module docstring: the CPU-fallback
        # build was 3.4x the size of the KPU one. The header word is
        # recorded only; it moved between 65 and 67 for graphs that differ
        # in the class-scatter tail alone, so it says nothing by itself.
        rec["cpu_fallback_suspect"] = bool(rec["reference"]["size_ratio"] > 1.5)

    if args.check_images:
        s = ort.InferenceSession(args.onnx, providers=["CPUExecutionProvider"])
        checks = {}
        for p in [l.strip() for l in Path(args.check_images).read_text().splitlines() if l.strip()]:
            rgb = read_rgb(p)
            x = letterbox(rgb, args.size)
            xf = np.ascontiguousarray(x[:, ::-1].astype(np.float32))  # what swapRB makes of it
            f = s.run(None, {"images": xf})[0]
            q = sim_run(kb, x)
            # Kept for decoder_check (convert.sh): simulator = KPU, bit-exact.
            q.astype("<f4").tofile(out.parent / (Path(p).stem + ".sim.f32"))
            checks[os.path.basename(p)] = {
                "frame_wh": [rgb.shape[1], rgb.shape[0]],
                "box_cosine": round(cosine(f[0, :4], q[0, :4]), 5),
                "nonfinite_float_box_values": nonfinite(f[0, :4]),
                "nonfinite_kmodel_values": nonfinite(q),
                "score_cosine": round(cosine(f[0, 4:], q[0, 4:]), 5),
                "max_score_err": round(float(np.abs(f[0, 4:] - q[0, 4:]).max()), 4),
                "float_max_score": round(float(f[0, 4:].max()), 4),
                "kmodel_max_score": round(float(q[0, 4:].max()), 4),
            }
        rec["sim_vs_float"] = checks

    Path(str(out) + ".json").write_text(json.dumps(rec, indent=1))
    print(json.dumps(rec, indent=1))


if __name__ == "__main__":
    main()
