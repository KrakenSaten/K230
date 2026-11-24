#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""E2 (second half): judge PTQ by the decisions the outputs feed.

Traffic DNA: per-flow cosine of simulator vs float embeddings, agreement of
the nearest prototype, accuracy with prototypes from float training
embeddings, open-set AUROC from simulator embeddings.
Window models: the same evaluate() as exp_window on simulator outputs, with
thresholds re-derived from simulator outputs of the validation set.

Usage (venv-train): eval_quant.py RUNS_DIR   (reads e3a/, e3b/, e2/)
"""
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import exp_dna as dna  # noqa: E402
import exp_window as win  # noqa: E402


def cos_rows(a, b):
    a = a / (np.linalg.norm(a, axis=1, keepdims=True) + 1e-9)
    b = b / (np.linalg.norm(b, axis=1, keepdims=True) + 1e-9)
    return (a * b).sum(1)


def main(runs):
    e2 = runs / "e2"
    res = {}
    m = np.load(runs / "e3b" / "dna_meta.npz")
    etr, ytr, ete, yte, nte, seen = m["etr"], m["ytr"], m["ete"], m["yte"], m["nte"], list(m["seen_ids"])
    protos = np.stack([etr[ytr == c].mean(0) for c in seen])
    for name, n_flows in (("dna_b64", None), ("dna_h64", None), ("dna_h1", 512)):
        for mode in ("u8", "a16w8"):
            f = e2 / f"{name}__{mode}__set0.sim.npy"
            if not f.exists():
                continue
            if name == "dna_b64":
                # [B,1,32,64,1] raw: the bench's expected output, then transpose + normalise as the host would
                raw = np.load(f).reshape(-1, 32, 64)
                raw[:32].astype("<f4").tofile(e2 / f"dna_b64__{mode}.sim_out.f32")
                sim = raw.transpose(0, 2, 1).reshape(-1, 32)
                sim = sim / (np.linalg.norm(sim, axis=1, keepdims=True) + 1e-6)
            else:
                sim = np.load(f).reshape(-1, 32)
            n = len(sim)
            ref = ete[:n]
            c = cos_rows(sim, ref)
            ps = np.array(seen)[(sim @ protos.T).argmax(1)]
            pf = np.array(seen)[(ref @ protos.T).argmax(1)]
            r, _, _ = dna.proto_eval(etr, ytr, sim, yte[:n], seen, nte[:n])
            rf, _, _ = dna.proto_eval(etr, ytr, ref, yte[:n], seen, nte[:n])
            res[f"{name}__{mode}"] = {
                "flows": n, "cosine_min": round(float(c.min()), 4), "cosine_p01": round(float(np.quantile(c, .01)), 4),
                "cosine_median": round(float(np.median(c)), 5),
                "nearest_prototype_agreement": round(float((ps == pf).mean()), 4),
                "proto_acc_known_sim": r["proto_acc_known"], "proto_acc_known_float_same_flows": rf["proto_acc_known"],
                "open_set_auroc_sim": r["open_set_auroc_unknown_vs_known"],
                "open_set_auroc_float_same_flows": rf["open_set_auroc_unknown_vs_known"]}
    meta = np.load(runs / "e3a" / "test_meta.npz", allow_pickle=True)
    kinds = meta["kinds"]
    xva = np.load(runs / "e3a" / "val_x.npy")
    xte = np.load(runs / "e3a" / "test_x.npy")
    for name in ("ae", "sup"):
        for mode in ("u8", "a16w8"):
            fv, ft = e2 / f"{name}__{mode}__set0.sim.npy", e2 / f"{name}__{mode}__set1.sim.npy"
            if not (fv.exists() and ft.exists()):
                continue
            sv, stt = np.load(fv), np.load(ft)
            if name == "ae":
                sv = win.ae_score(sv.reshape(len(sv), -1), xva)[0]
                stt = win.ae_score(stt.reshape(len(stt), -1), xte)[0]
                flt = meta["float_ae_test"]
            else:
                sv, stt = sv.reshape(-1), stt.reshape(-1)
                flt = meta["float_sup_test"]
            ev = win.evaluate(name, sv, stt, kinds)
            res[f"window_{name}__{mode}"] = {
                "auroc_fault_vs_benign": ev["auroc_fault_vs_benign"], "auroc_fault_vs_gap": ev["auroc_fault_vs_gap"],
                "tpr_seen_at_fa1": ev["at_fa_0.01"]["tpr_seen_kinds"],
                "tpr_unseen_at_fa1": ev["at_fa_0.01"]["tpr_unseen_kinds"],
                "gap_alert_rate_at_fa1": ev["at_fa_0.01"]["gap_alert_rate"],
                "score_rank_corr_vs_float": round(float(np.corrcoef(stt.argsort().argsort(),
                                                                    flt.argsort().argsort())[0, 1]), 4)}
    (e2 / "quant_results.json").write_text(json.dumps(res, indent=1))
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
