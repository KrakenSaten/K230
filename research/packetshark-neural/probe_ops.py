#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""E1: which operators of small non-image models does nncase 2.11 place on the
K230 KPU, which fall back to the CPU, and which are refused?

Each case is a tiny ONNX graph shaped like network telemetry (features x
time bins, or a batch of flows x packets). For every case and quantization
mode a child process (the .NET importer aborts the process on an unsupported
op) compiles for target k230 with IR dumps, then reads what the compiler
itself reports:

  - import / compile failure and its message
  - the CPU ops list nncase writes into
    7_TargetDependentAfterQuantPass/13_DumpTraceInfoPass/estimate_fps.py
  - the KPU tile groups and their MACs/traffic (10_FusionToTirPass/mac.csv)
  - the compiler's own roofline time estimate (formula copied from the
    generated draw_roofline.py: 0.6 TFLOP/s, 3.2 GB/s off chip, 25.6 GB/s on
    chip, CPU 1.6 GHz) - an ideal lower bound, NOT a measurement
  - data types left in the final IL
  - simulator output vs float onnxruntime on held-out random inputs

Usage: probe_ops.py OUT_DIR [case ...]        (parent: all cases, all modes)
       probe_ops.py --one CASE MODE OUT_DIR   (child)
"""
import ast
import json
import re
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

MODES = ("none", "u8", "a16w8")
F, T = 16, 120  # telemetry window: 16 features x 120 one-second bins


def rng(s):
    return np.random.default_rng(s)


def W(shape, s, scale=None):
    fan = int(np.prod(shape[1:])) if len(shape) > 1 else shape[0]
    return (rng(s).standard_normal(shape) * (scale or 1.0 / np.sqrt(max(fan, 1)))).astype(np.float32)


def build(case):
    """-> (ModelProto, [(input name, shape, kind)]) ; kind: normal|mask|index|onehot"""
    from onnx import TensorProto, helper as h
    nodes, inits, ins, outs = [], [], [], []
    opset = 13

    def init(name, arr):
        dt = TensorProto.INT64 if arr.dtype == np.int64 else TensorProto.FLOAT
        inits.append(h.make_tensor(name, dt, arr.shape, arr.ravel()))
        return name

    def inp(name, shape, kind="normal", dt=TensorProto.FLOAT):
        ins.append((h.make_tensor_value_info(name, dt, shape), shape, kind))
        return name

    def node(op, i, o, **kw):
        nodes.append(h.make_node(op, i, [o] if isinstance(o, str) else o, **kw))
        return o

    def conv(x, cin, cout, k, s, name, dil=1, groups=1, rank=4):
        if rank == 4:
            w = init(name + "_w", W((cout, cin // groups, 1, k), s))
            b = init(name + "_b", W((cout,), s + 1, 0.1))
            p = dil * (k - 1) // 2
            return node("Conv", [x, w, b], name, pads=[0, p, 0, p], dilations=[1, dil], group=groups)
        w = init(name + "_w", W((cout, cin // groups, k), s))
        b = init(name + "_b", W((cout,), s + 1, 0.1))
        p = dil * (k - 1) // 2
        return node("Conv", [x, w, b], name, pads=[p, p], dilations=[dil], group=groups)

    if case == "conv1d_as_2d":
        x = inp("x", [1, F, 1, T]); y = node("Relu", [conv(x, F, 32, 3, 1, "c")], "y")
    elif case == "conv1d_rank3":
        x = inp("x", [1, F, T]); y = node("Relu", [conv(x, F, 32, 3, 1, "c", rank=3)], "y")
    elif case == "conv_dilated":
        x = inp("x", [1, F, 1, T]); y = node("Relu", [conv(x, F, 32, 3, 1, "c", dil=4)], "y")
    elif case == "conv_depthwise":
        x = inp("x", [1, 32, 1, T]); y = node("Relu", [conv(x, 32, 32, 5, 1, "c", groups=32)], "y")
    elif case == "conv_stride_pool":
        x = inp("x", [1, F, 1, T])
        c = node("Relu", [conv(x, F, 32, 3, 1, "c")], "r")
        y = node("MaxPool", [c], "y", kernel_shape=[1, 2], strides=[1, 2])
    elif case == "tcn_residual":
        x = inp("x", [1, 32, 1, T])
        a = node("Relu", [conv(x, 32, 32, 3, 1, "c1", dil=2)], "a")
        b = conv(a, 32, 32, 3, 3, "c2", dil=4)
        y = node("Relu", [node("Add", [x, b], "s")], "y")
    elif case == "masked_input":  # two inputs: values and a 0/1 coverage mask
        x = inp("x", [1, F, 1, T]); m = inp("m", [1, F, 1, T], "mask")
        xm = node("Mul", [x, m], "xm")
        y = node("Relu", [conv(node("Concat", [xm, m], "cat", axis=1), 2 * F, 32, 3, 1, "c")], "y")
    elif case == "gemm_mlp":
        x = inp("x", [1, F * T])
        a = node("Relu", [node("Gemm", [x, init("w1", W((64, F * T), 1)), init("b1", W((64,), 2, .1))], "g1",
                                transB=1)], "a")
        y = node("Gemm", [a, init("w2", W((16, 64), 3)), init("b2", W((16,), 4, .1))], "y", transB=1)
    elif case == "matmul_batched":  # 64 flows x 96 features -> 32-d embeddings
        x = inp("x", [1, 64, 96])
        y = node("Relu", [node("Add", [node("MatMul", [x, init("w", W((96, 32), 1))], "mm"),
                                       init("b", W((32,), 2, .1))], "ad")], "y")
    elif case == "matmul_prototypes":  # cosine retrieval against 16 stored prototypes
        x = inp("x", [1, 64, 32])
        y = node("MatMul", [x, init("p", W((32, 16), 1))], "y")
    elif case == "l2_normalize":
        x = inp("x", [1, 64, 32])
        n = node("ReduceL2", [x], "n", axes=[2], keepdims=1)
        y = node("Div", [x, node("Add", [n, init("eps", np.array([1e-6], np.float32))], "ne")], "y")
    elif case == "softmax":
        x = inp("x", [1, 4, 32, 32]); y = node("Softmax", [x], "y", axis=-1)
    elif case == "layernorm_op":
        opset = 17
        x = inp("x", [1, 32, 64])
        y = node("LayerNormalization", [x, init("g", W((64,), 1, 1.)), init("b", W((64,), 2, .1))], "y", axis=-1)
    elif case == "layernorm_decomposed":
        x = inp("x", [1, 32, 64])
        mu = node("ReduceMean", [x], "mu", axes=[2], keepdims=1)
        d = node("Sub", [x, mu], "d")
        v = node("ReduceMean", [node("Mul", [d, d], "d2")], "v", axes=[2], keepdims=1)
        sd = node("Sqrt", [node("Add", [v, init("eps", np.array([1e-5], np.float32))], "ve")], "sd")
        y = node("Add", [node("Mul", [node("Div", [d, sd], "z"), init("g", W((64,), 1, 1.))], "zg"),
                         init("b", W((64,), 2, .1))], "y")
    elif case == "attention_1head":  # 32 tokens (packets) x 32 dims
        x = inp("x", [1, 32, 32])
        q = node("MatMul", [x, init("wq", W((32, 32), 1))], "q")
        k = node("MatMul", [x, init("wk", W((32, 32), 2))], "k")
        v = node("MatMul", [x, init("wv", W((32, 32), 3))], "v")
        kt = node("Transpose", [k], "kt", perm=[0, 2, 1])
        s = node("Mul", [node("MatMul", [q, kt], "qk"), init("sc", np.array([1 / np.sqrt(32)], np.float32))], "s")
        y = node("MatMul", [node("Softmax", [s], "p", axis=-1), v], "y")
    elif case == "lstm":
        x = inp("x", [32, 1, 8])
        Hd = 16
        y = node("LSTM", [x, init("W", W((1, 4 * Hd, 8), 1)), init("R", W((1, 4 * Hd, Hd), 2))], ["Y", "y"],
                 hidden_size=Hd)
        y = "y"
    elif case == "gru":
        x = inp("x", [32, 1, 8])
        Hd = 16
        node("GRU", [x, init("W", W((1, 3 * Hd, 8), 1)), init("R", W((1, 3 * Hd, Hd), 2))], ["Y", "y"],
             hidden_size=Hd)
        y = "y"
    elif case == "gather_embedding":  # char ids of a 64-char name -> 16-d vectors
        x = inp("x", [1, 64], "index", TensorProto.INT64)
        y = node("Gather", [init("emb", W((40, 16), 1, 1.)), x], "y", axis=0)
    elif case == "onehot_charcnn":  # embedding as a 1x1 conv over a one-hot name
        x = inp("x", [1, 40, 1, 64], "onehot")
        e = conv(x, 40, 16, 1, 1, "emb")
        c = node("Relu", [conv(e, 16, 64, 5, 2, "c1")], "r")
        g = node("GlobalMaxPool", [c], "g")
        y = node("Sigmoid", [node("Gemm", [node("Flatten", [g], "fl"), init("w", W((1, 64), 3)),
                                           init("b", W((1,), 4, .1))], "lo", transB=1)], "y")
    elif case == "reductions":
        x = inp("x", [1, 32, 1, T])
        a = node("GlobalAveragePool", [x], "a")
        b = node("ReduceMax", [x], "b", axes=[3], keepdims=1)
        y = node("Concat", [a, b], "y", axis=1)
    elif case == "activations":
        x = inp("x", [1, 32, 1, T])
        a = node("Sigmoid", [x], "a"); b = node("Tanh", [x], "b"); c = node("LeakyRelu", [x], "c", alpha=0.1)
        e = node("Mul", [node("Mul", [x, init("half", np.array([0.5], np.float32))], "hx"),
                         node("Add", [node("Erf", [node("Mul", [x, init("rs2", np.array([0.70710677], np.float32))],
                                                         "xs")], "er"), init("one", np.array([1.], np.float32))],
                              "e1")], "gelu")
        y = node("Concat", [a, b, c, e], "y", axis=1)
    elif case == "argmax_topk":
        x = inp("x", [1, 64, 16])
        node("ArgMax", [x], "y", axis=2, keepdims=0)
        y = "y"
    elif case == "batch64_flows":  # a batch of 64 flows, 3 channels x 32 packets
        x = inp("x", [64, 3, 1, 32])
        c = node("Relu", [conv(x, 3, 32, 3, 1, "c1")], "r1")
        g = node("GlobalAveragePool", [node("Relu", [conv(c, 32, 32, 3, 3, "c2")], "r2")], "g")
        y = node("Flatten", [g], "y")
    elif case == "flows_as_height":  # the same 64 flows as the H axis of one tensor
        x = inp("x", [1, 3, 64, 32])
        c = node("Relu", [conv(x, 3, 32, 3, 1, "c1")], "r1")
        y = node("ReduceMean", [node("Relu", [conv(c, 32, 32, 3, 3, "c2")], "r2")], "y", axes=[3], keepdims=0)
    elif case == "charcnn_batch32_names":  # 32 DNS names x 64 chars, one-hot over 40 symbols
        x = inp("x", [1, 40, 32, 64], "onehot")
        e = conv(x, 40, 32, 1, 1, "emb")
        c1 = node("Relu", [conv(e, 32, 64, 5, 2, "c1")], "r1")
        c2 = node("Relu", [conv(c1, 64, 64, 5, 3, "c2", dil=2)], "r2")
        p = node("MaxPool", [c2], "p", kernel_shape=[1, 64], strides=[1, 64])
        h1 = node("Relu", [conv(p, 64, 32, 1, 4, "fc1")], "h1")
        y = node("Sigmoid", [conv(h1, 32, 1, 1, 5, "fc2")], "y")
    elif case == "dna_batch64_avgpool":  # the DNA encoder with KPU pooling instead of ReduceMean
        x = inp("x", [1, 3, 64, 32])
        a = node("Relu", [conv(x, 3, 32, 3, 1, "c1")], "r1")
        b = node("Relu", [node("Conv", [a, init("c2_w", W((64, 32, 1, 3), 2)), init("c2_b", W((64,), 3, .1))], "c2",
                                pads=[0, 1, 0, 1], strides=[1, 2])], "r2")
        c = node("Relu", [node("Conv", [b, init("c3_w", W((64, 64, 1, 3), 4)), init("c3_b", W((64,), 5, .1))], "c3",
                                pads=[0, 1, 0, 1], strides=[1, 2])], "r3")
        p = node("AveragePool", [c], "p", kernel_shape=[1, 8], strides=[1, 8])
        y = conv(p, 64, 32, 1, 6, "fc")
    else:
        raise SystemExit("unknown case " + case)

    g = h.make_graph(nodes, case, [i[0] for i in ins], [h.make_tensor_value_info(y, TensorProto.FLOAT, None)
                                                       if case not in ("argmax_topk",)
                                                       else h.make_tensor_value_info(y, TensorProto.INT64, None)],
                     inits)
    m = h.make_model(g, opset_imports=[h.make_opsetid("", opset)])
    m.ir_version = 8
    import onnx
    m = onnx.shape_inference.infer_shapes(m)
    onnx.checker.check_model(m)
    return m, [(i[0].name, i[1], i[2]) for i in ins]


def sample(shape, kind, s):
    r = rng(s)
    if kind == "mask":
        return (r.random(shape) > 0.2).astype(np.float32)
    if kind == "index":
        return r.integers(0, 40, shape).astype(np.int64)
    if kind == "onehot":
        idx = r.integers(0, 40, (shape[0], shape[3]))
        o = np.zeros(shape, np.float32)
        for b in range(shape[0]):
            o[b, idx[b], 0, np.arange(shape[3])] = 1.0
        return o
    return r.standard_normal(shape).astype(np.float32)


CASES = ["conv1d_as_2d", "conv1d_rank3", "conv_dilated", "conv_depthwise", "conv_stride_pool", "tcn_residual",
         "masked_input", "gemm_mlp", "matmul_batched", "matmul_prototypes", "l2_normalize", "softmax",
         "layernorm_op", "layernorm_decomposed", "attention_1head", "lstm", "gru", "gather_embedding",
         "onehot_charcnn", "reductions", "activations", "argmax_topk", "batch64_flows", "flows_as_height"]


def roofline(dump):
    """The compiler's own estimate, from the files it generated."""
    base = dump / "7_TargetDependentAfterQuantPass"
    res = {"kpu_groups": [], "cpu_ops": [], "est_ms_kpu": 0.0, "est_ms_cpu": 0.0}
    est = base / "13_DumpTraceInfoPass" / "estimate_fps.py"
    if est.exists():
        src = est.read_text()
        m = re.search(r"cpu_ops = (\[.*?\n\])", src, re.S)
        if m:
            ops = []
            for name, args in re.findall(r"\['(\w+)', dict\(([^)]*)\)\]", m.group(1)):
                d = {}
                for kv in filter(None, args.split(",")):
                    k, v = kv.split("=")
                    d[k.strip()] = ast.literal_eval(v.strip())
                ops.append((name, d))
            for name, d in ops:
                t = max(d.get("FLOPs", 0) / (1.6e9 * d.get("Parallel", 1)),
                        d.get("OffChipMemoryTraffic", 0) / 3.2e9) * 1e3
                res["cpu_ops"].append({"name": name, "flops": d.get("FLOPs", 0), "est_ms": t})
                res["est_ms_cpu"] += t
    mac = base / "10_FusionToTirPass" / "mac.csv"
    if mac.exists():
        lines = mac.read_text().strip().splitlines()
        hdr = [c.strip() for c in lines[0].split(",")]
        for ln in lines[1:]:
            v = dict(zip(hdr, [c.strip() for c in ln.split(",")]))
            fl, on, off, lw = (float(v["FLOPs"]), float(v["OnChipMemTraffic"]), float(v["OffChipMemTraffic"]),
                               float(v["OffChipMemLWTraffic"]))
            comp, ton, toff, tlw = fl / 0.6e12, on / 25.6e9, off / 3.2e9, lw / 3.2e9
            if int(float(v["OffChipLoadStoreCnt"])) == 2:
                t = toff + max(tlw, ton, comp)
            else:
                t = max(comp, toff + tlw, ton)
            res["kpu_groups"].append({"name": v["Name"], "macs": int(float(v["Mac"])), "est_ms": t * 1e3})
            res["est_ms_kpu"] += t * 1e3
    sched = base / "12_DDrBufferSchdeule" / "End" / "main.il"
    if sched.exists():
        txt = sched.read_text(errors="replace")
        res["kpu_datatypes"] = {k: txt.count("datatype: " + k) for k in ("fp32", "fp16", "bf16", "i8", "u8", "i16")
                                if txt.count("datatype: " + k)}
    # ops left in the main function outside any KPU tile group = CPU work
    for f in (base / "10_FusionToTirPass" / "End" / "main.il", base / "Epoch_0" / "After" / "main.il",
              dump / "6_QuantizePass" / "Epoch_0" / "After" / "main.il",
              dump / "5_TargetDependentPass" / "Epoch_0" / "After" / "main.il"):
        if f.exists():
            txt = f.read_text(errors="replace")
            ops = re.findall(r"^  %[\w.]+ = ([A-Z][A-Za-z0-9_]*)\(", txt, re.M)
            res["main_level_ops"] = sorted(set(ops))
            res["main_level_from"] = str(f.relative_to(dump))
            break
    if res["kpu_groups"] and not res["cpu_ops"]:
        res["placement"] = "KPU"
    elif res["kpu_groups"]:
        res["placement"] = "KPU+CPU"
    else:
        res["placement"] = "CPU"
    return res


def child(case, mode, out):
    import nncase
    import onnxruntime as ort
    d = out / f"{case}__{mode}"
    d.mkdir(parents=True, exist_ok=True)
    m, ins = build(case)
    (d / "model.onnx").write_bytes(m.SerializeToString())
    rec = {"case": case, "mode": mode, "inputs": [[n, s, k] for n, s, k in ins]}
    t0 = time.time()
    co = nncase.CompileOptions()
    co.target = "k230"
    co.preprocess = False
    co.dump_ir = True
    co.dump_asm = False
    co.dump_dir = str(d / "dump")
    c = nncase.Compiler(co)
    c.import_onnx(m.SerializeToString(), nncase.ImportOptions())
    rec["imported"] = True
    if mode != "none":
        po = nncase.PTQTensorOptions()
        po.quant_type = "uint8" if mode == "u8" else "int16"
        po.w_quant_type = "uint8"
        po.calibrate_method = "NoClip"
        po.finetune_weights_method = "NoFineTuneWeights"
        n = 8
        po.samples_count = n
        po.set_tensor_data([[sample(s, k, 100 + i) for i in range(n)] for _, s, k in ins])
        c.use_ptq(po)
    c.compile()
    km = c.gencode_tobytes()
    (d / "model.kmodel").write_bytes(km)
    rec["compiled"] = True
    rec["compile_s"] = round(time.time() - t0, 1)
    rec["kmodel_bytes"] = len(km)
    rec.update(roofline(d / "dump"))
    sess = ort.InferenceSession(m.SerializeToString(), providers=["CPUExecutionProvider"])
    cos, mae = [], []
    for j in range(4):
        feeds = {n: sample(s, k, 900 + j) for n, s, k in ins}
        ref = sess.run(None, feeds)[0]
        sim = nncase.Simulator()
        sim.load_model(km)
        for i, (n, s, k) in enumerate(ins):
            sim.set_input_tensor(i, nncase.RuntimeTensor.from_numpy(feeds[n]))
        sim.run()
        got = sim.get_output_tensor(0).to_numpy()
        rec["out_shape"] = list(got.shape)
        if got.shape != ref.shape:
            rec["shape_mismatch"] = [list(ref.shape), list(got.shape)]
            got = got.reshape(ref.shape) if got.size == ref.size else got
        if got.dtype.kind in "iu":
            cos.append(float((got.ravel() == ref.ravel()).mean()))
            mae.append(0.0)
            continue
        a, b = ref.ravel().astype(np.float64), got.ravel().astype(np.float64)
        cos.append(float(a @ b / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-12)))
        mae.append(float(np.abs(a - b).max() / (np.abs(a).max() + 1e-12)))
    rec["sim_vs_float_cosine_min"] = round(min(cos), 5)
    rec["sim_vs_float_rel_maxerr"] = round(max(mae), 5)
    (d / "result.json").write_text(json.dumps(rec, indent=1))
    print(json.dumps(rec))


def parent(out, cases):
    out.mkdir(parents=True, exist_ok=True)
    allr = []
    for case in cases:
        for mode in MODES:
            t0 = time.time()
            p = subprocess.run([sys.executable, __file__, "--one", case, mode, str(out)], capture_output=True,
                               text=True, timeout=1200)
            rj = out / f"{case}__{mode}" / "result.json"
            if p.returncode == 0 and rj.exists():
                r = json.loads(rj.read_text())
            else:
                err = (p.stdout + p.stderr).strip().splitlines()
                msg = next((ln for ln in err if "Exception" in ln or "Error" in ln or "error" in ln), err[-1] if err
                           else "no output")
                r = {"case": case, "mode": mode, "compiled": False, "error": msg.strip()[:300],
                     "returncode": p.returncode}
            r["wall_s"] = round(time.time() - t0, 1)
            # keep the dumps small: drop everything except the files we read
            allr.append(r)
            print(f"{case:22s} {mode:6s} " + ("OK " if r.get("compiled") else "FAIL ") +
                  (f"cpu={[o['name'] for o in r.get('cpu_ops', [])]} kpu_groups={len(r.get('kpu_groups', []))} "
                   f"cos={r.get('sim_vs_float_cosine_min')} est_kpu={r.get('est_ms_kpu', 0):.4f}ms"
                   if r.get("compiled") else r.get("error", "")), flush=True)
            (out / "probe_ops.json").write_text(json.dumps(allr, indent=1))


def reparse(out):
    """Re-read the compiler's files of an earlier run (no recompile)."""
    allr = json.loads((out / "probe_ops.json").read_text())
    for r in allr:
        d = out / f"{r['case']}__{r['mode']}" / "dump"
        if r.get("compiled") and d.exists():
            r.update(roofline(d))
    (out / "probe_ops.json").write_text(json.dumps(allr, indent=1))
    for r in allr:
        print(f"{r['case']:22s} {r['mode']:6s} " + (
            f"{r.get('placement')} cpu={[o['name'] for o in r.get('cpu_ops', [])]} groups={len(r.get('kpu_groups', []))}"
            f" macs={sum(g['macs'] for g in r.get('kpu_groups', []))} est_kpu={r.get('est_ms_kpu', 0):.3f}ms"
            f" est_cpu={r.get('est_ms_cpu', 0):.3f}ms cos={r.get('sim_vs_float_cosine_min')}"
            f" kmodel={r.get('kmodel_bytes')}" if r.get("compiled") else "FAIL " + r.get("error", "")[:110]))


if __name__ == "__main__":
    if sys.argv[1] == "--reparse":
        reparse(Path(sys.argv[2]))
    elif sys.argv[1] == "--one":
        child(sys.argv[2], sys.argv[3], Path(sys.argv[4]))
    else:
        parent(Path(sys.argv[1]), sys.argv[2:] or CASES)
