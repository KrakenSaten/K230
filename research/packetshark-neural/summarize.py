#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Print the experiment results as Markdown tables (stdlib only).
Usage: summarize.py RUNS_DIR"""
import json
import sys
from pathlib import Path


def window(p):
    r = json.loads(p.read_text())
    print("### E3a window detectors (synthetic)\n")
    print("| Method | AUROC fault vs benign | AUROC fault vs gap | TPR seen @1% | TPR unseen @1% | gap alerts @1% |"
          " TPR seen @5% | TPR unseen @5% | gap alerts @5% |")
    print("|---|---|---|---|---|---|---|---|---|")
    for m, v in r["methods"].items():
        a, b = v["at_fa_0.01"], v["at_fa_0.05"]
        print(f"| {m} | {v['auroc_fault_vs_benign']:.3f} | {v['auroc_fault_vs_gap']:.3f} | {a['tpr_seen_kinds']:.3f} | "
              f"{a['tpr_unseen_kinds']:.3f} | {a['gap_alert_rate']:.3f} | {b['tpr_seen_kinds']:.3f} | "
              f"{b['tpr_unseen_kinds']:.3f} | {b['gap_alert_rate']:.3f} |")
    kinds = [k[4:] for k in r["methods"]["B1_robust_z"]["at_fa_0.01"] if k.startswith("tpr_") and "kinds" not in k]
    print("\nTPR per fault kind at 1 % benign alert rate:\n")
    print("| Method | " + " | ".join(kinds) + " |")
    print("|---|" + "---|" * len(kinds))
    for m, v in r["methods"].items():
        print(f"| {m} | " + " | ".join(f"{v['at_fa_0.01']['tpr_' + k]:.2f}" for k in kinds) + " |")
    gaps = [k[len("alert_rate_"):] for k in r["methods"]["B1_robust_z"]["at_fa_0.01"] if k.startswith("alert_rate_")]
    if gaps:
        print("\nAlert rate on visibility artifacts at 1 % benign alert rate (should be ~0):\n")
        print("| Method | " + " | ".join(gaps) + " |")
        print("|---|" + "---|" * len(gaps))
        for m, v in r["methods"].items():
            print(f"| {m} | " + " | ".join(f"{v['at_fa_0.01']['alert_rate_' + g]:.3f}" for g in gaps) + " |")
    if "N3_weights" in r:
        print("\nN3 logistic weights over log1p(rule scores):", json.dumps(r["N3_weights"]))
    if "B4_top_rule_by_fault" in r:
        print("\nB4 rule with the largest score, per fault kind:", json.dumps(r["B4_top_rule_by_fault"]))
    print("\nparams:", r["params"], "| AE/B1 top-feature agreement on faults:",
          r["top_feature_agreement_ae_vs_b1_on_faults"], "| n:", r["n"], "\n")


def dna(p):
    r = json.loads(p.read_text())
    print("### E3b Traffic DNA (synthetic flows)\n")
    keys = ["proto_acc_known", "knn5_acc_known", "proto_acc_known_short_lt16", "proto_acc_known_full32",
            "open_set_auroc_unknown_vs_known", "unknown_rejected_at_95pct_known_accept"]
    print("| Representation | " + " | ".join(keys) + " |")
    print("|---|" + "---|" * len(keys))
    for m in ("baseline_handcrafted_24d", "dna_cnn_32d"):
        print(f"| {m} | " + " | ".join(str(r[m][k]) for k in keys) + " |")
    print("\nparams:", r["params"], "| train_s:", r["train_s"], "\n")


def rhythm(p):
    r = json.loads(p.read_text())
    print("### E3c rhythm: TPR at 1 % FPR by number of events n =", r["n"], "\n")
    print("| Condition | cv: min n for TPR 0.9 | harm: min n for TPR 0.9 | harm TPR by n |")
    print("|---|---|---|---|")
    for k, v in r["conditions"].items():
        print(f"| {k} | {v['cv']['min_events_for_tpr90']} | {v['harm']['min_events_for_tpr90']} | "
              f"{v['harm']['tpr_at_fpr1pct']} |")
    print()


def quant(p):
    r = json.loads(p.read_text())
    print("### E2 quantization at decision level (nncase simulator)\n")
    print(json.dumps(r, indent=1))


if __name__ == "__main__":
    d = Path(sys.argv[1])
    for f, fn in (("e3a/window_results.json", window), ("e3b/dna_results.json", dna),
                  ("e3c/rhythm_results.json", rhythm), ("e2/quant_results.json", quant)):
        if (d / f).exists():
            fn(d / f)
