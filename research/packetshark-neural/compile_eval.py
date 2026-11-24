#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""E2: compile a trained ONNX model for k230 (nncase 2.11, PTQ) and run the
simulator over a whole input set, so that quantization can be judged on the
decisions the model feeds (eval_quant.py), not only on tensor cosine.

For every mode it records the compiler's own placement report and roofline
estimate (probe_ops.roofline) and writes OUT/<name>__<mode>.sim.npy with one
output row per input sample.

Usage (venv-convert): compile_eval.py NAME MODEL.onnx CALIB.npy OUT_DIR SET.npy [SET2.npy ...]
  CALIB.npy / SET.npy: arrays whose first axis indexes samples, each sample
  already shaped as the model input (batch axis 1 included or not).
"""
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from probe_ops import roofline  # noqa: E402

MODES = {"u8": ("uint8", "uint8"), "a16w8": ("int16", "uint8")}


def as_input(a):
    a = np.ascontiguousarray(a.astype(np.float32))
    return a if a.ndim == 4 else a[None]


def main():
    name, onnx_path, calib, out = sys.argv[1], Path(sys.argv[2]), np.load(sys.argv[3]), Path(sys.argv[4])
    sets = sys.argv[5:]
    import nncase
    out.mkdir(parents=True, exist_ok=True)
    model = onnx_path.read_bytes()
    rep = {}
    for mode, (qa, qw) in MODES.items():
        t0 = time.time()
        d = out / f"{name}__{mode}"
        co = nncase.CompileOptions()
        co.target = "k230"
        co.preprocess = False
        co.dump_ir = True
        co.dump_asm = False
        co.dump_dir = str(d / "dump")
        c = nncase.Compiler(co)
        c.import_onnx(model, nncase.ImportOptions())
        po = nncase.PTQTensorOptions()
        po.quant_type, po.w_quant_type = qa, qw
        po.calibrate_method = "NoClip"
        po.finetune_weights_method = "NoFineTuneWeights"
        cal = [as_input(x) for x in calib]
        po.samples_count = len(cal)
        po.set_tensor_data([cal])
        c.use_ptq(po)
        c.compile()
        km = c.gencode_tobytes()
        d.mkdir(parents=True, exist_ok=True)
        (d / "model.kmodel").write_bytes(km)
        r = {"kmodel_bytes": len(km), "compile_s": round(time.time() - t0, 1)}
        r.update(roofline(d / "dump"))
        sim = nncase.Simulator()
        sim.load_model(km)
        for si, sp in enumerate(sets):
            X = np.load(sp)
            outs = []
            t1 = time.time()
            for x in X:
                sim.set_input_tensor(0, nncase.RuntimeTensor.from_numpy(as_input(x)))
                sim.run()
                outs.append(sim.get_output_tensor(0).to_numpy().copy())
            np.save(out / f"{name}__{mode}__set{si}.sim.npy", np.stack(outs))
            r[f"set{si}"] = {"file": Path(sp).name, "n": len(X), "sim_s": round(time.time() - t1, 1)}
        rep[mode] = r
        print(mode, json.dumps({k: v for k, v in r.items() if k not in ("kpu_groups",)}), flush=True)
    (out / f"{name}__compile.json").write_text(json.dumps(rep, indent=1))


if __name__ == "__main__":
    main()
