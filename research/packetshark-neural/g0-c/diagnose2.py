#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Read-only diagnosis of the second held-out run (G0.md §7.5): which
claims were false, on which metric, and what the windows looked like.
Changes nothing and is not used to tune anything.

Usage: python3 diagnose2.py BINARY N_SEEDS
"""
import sys
from collections import Counter
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import cjudge as C  # noqa: E402
import indep_gen2 as G  # noqa: E402

E = C.E


def main():
    binary, ns = sys.argv[1], int(sys.argv[2])
    rows = []
    for sd in range(7000, 7000 + ns):
        rows += C.judge_c(C.indep_windows(G, sd, 2400, mirror=False), binary)
    healthy = [x for x in rows if not x["fault"]]
    by_metric = Counter()
    by_cat = Counter()
    detail = Counter()
    for x in healthy:
        v = x["v"]["c"]
        bad = [m for m in E.METRICS if m not in x["rose"] and v[m][0] == E.WORSE]
        for m in bad:
            by_metric[m] += 1
            for c in E.g0_categories(x) or ["(base artifact only)"]:
                by_cat[(c, m)] += 1
            if m.startswith("dns"):
                w = x["win"]
                qs = w["qs"]
                b = [q for q in qs if q[0] < 60]
                a = [q for q in qs if 60 <= q[0] < 120]
                med = lambda l: int(np.median([q[3] for q in l if q[1]])) if any(q[1] for q in l) else -1
                detail[(m, "base q=%d seen=%d med=%d / win q=%d seen=%d med=%d" % (
                    len(b), sum(1 for q in b if q[1]), med(b) // 1000, len(a), sum(1 for q in a if q[1]),
                    med(a) // 1000))] += 1
    print("healthy windows:", len(healthy))
    print("false claims by metric:", dict(by_metric))
    print("by category and metric:")
    for k, n in by_cat.most_common(20):
        print("  %5d  %s" % (n, k))
    print("examples (DNS):")
    for k, n in list(detail.items())[:15]:
        print("  ", k)
    # how often the generator lists each metric in rose for healthy windows
    rc = Counter(m for x in healthy for m in x["rose"])
    print("rose labels among healthy windows:", dict(rc))
    # material faults under loss: verdict on the faulted metric
    for f in E.MATERIAL_FAULTS:
        t = E.FAULTS[f]
        sel = [x for x in rows if x["fault"] == f and x["art"] == "loss"]
        cnt = Counter("%s/%s" % x["v"]["c"][t] for x in sel)
        print("%-13s under loss n=%4d: %s" % (f, len(sel), ", ".join("%s %d" % kv for kv in cnt.most_common(5))))


if __name__ == "__main__":
    main()
