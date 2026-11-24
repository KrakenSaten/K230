#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""E3c: how many repetitions does a short session need before a conversation's
rhythm can be called periodic at a fixed false-alert rate?

Positives: periodic events with relative jitter (uniform +-j of the period)
and missed events (visibility loss, drop probability q).
Negatives at the same event count: Poisson arrivals and human-like bursty
arrivals (log-normal inter-arrivals, sigma 1.0).
Scores (both O(n), CPU):
  cv    1 - min(1, 1.4826*MAD(IAT)/median(IAT))
  harm  share of IATs within 10 % of an integer multiple (1..4) of the
        median IAT - tolerant of missed events
Thresholds at 1 % FPR on the negatives with the same n. Output: TPR per n,
and the smallest n with TPR >= 0.9, which with the period gives the minimum
observation time. Synthetic by construction; it bounds what any detector can
claim from n events, it does not measure a real network.
"""
import json
import sys
from pathlib import Path

import numpy as np

NS = [3, 4, 5, 6, 8, 10, 12, 15, 20, 30]
TRIALS = 4000


def iats_periodic(r, n, j, q):
    t, out, last = 0.0, [], 0.0
    k = 0
    while len(out) < n - 1:
        k += 1
        t = k + r.uniform(-j, j)
        if r.random() < q and k > 1:
            continue
        if k > 1 or out:
            out.append(t - last)
        last = t
    return np.array(out[:n - 1])


def score_cv(d):
    med = np.median(d)
    return 1 - min(1.0, 1.4826 * np.median(np.abs(d - med)) / max(med, 1e-9))


def score_harm(d):
    p = np.median(d)
    k = np.clip(np.round(d / p), 1, 4)
    return float((np.abs(d - k * p) / p < 0.1).mean())


def main(out):
    r = np.random.default_rng(42)
    res = {"n": NS, "conditions": {}}
    negs = {}
    for n in NS:
        neg = {s: [] for s in ("cv", "harm")}
        for t in range(TRIALS):
            d = r.exponential(1.0, n - 1) if t % 2 == 0 else np.exp(r.normal(0, 1.0, n - 1))
            neg["cv"].append(score_cv(d)); neg["harm"].append(score_harm(d))
        negs[n] = {s: float(np.quantile(v, 0.99)) for s, v in neg.items()}
    for j in (0.01, 0.05, 0.15, 0.30):
        for q in (0.0, 0.2):
            key = f"jitter{j}_miss{q}"
            row = {}
            for s in ("cv", "harm"):
                tpr = []
                for n in NS:
                    hit = 0
                    for _ in range(TRIALS // 4):
                        d = iats_periodic(r, n, j, q)
                        sc = score_cv(d) if s == "cv" else score_harm(d)
                        hit += sc > negs[n][s]
                    tpr.append(round(hit / (TRIALS // 4), 3))
                nmin = next((n for n, v in zip(NS, tpr) if v >= 0.9), None)
                row[s] = {"tpr_at_fpr1pct": tpr, "min_events_for_tpr90": nmin}
            res["conditions"][key] = row
    out.mkdir(parents=True, exist_ok=True)
    (out / "rhythm_results.json").write_text(json.dumps(res, indent=1))
    for k, v in res["conditions"].items():
        print(f"{k:22s} cv n90={v['cv']['min_events_for_tpr90']}  harm n90={v['harm']['min_events_for_tpr90']}  "
              f"harm tpr={v['harm']['tpr_at_fpr1pct']}")


if __name__ == "__main__":
    main(Path(sys.argv[1]))
