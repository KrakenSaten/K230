#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Parity of the Python mirror (e3a_r.judge, loss-aware) and the C engine on
the same windows, and G0 counts for both. Development data only: E3a-R
seeds 1000-1009 and 2000-2009 (both seen), and the first independent
generator's seeds 5000-5009 (failed, now regression evidence).

Usage: python3 parity.py BINARY OUT_JSON [e3a-dev|e3a-2000|indep] [n_seeds]
"""
import json
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "g0-heldout"))
import cjudge as C  # noqa: E402

E = C.E


def main():
    binary, out, which = sys.argv[1], Path(sys.argv[2]), sys.argv[3]
    ns = int(sys.argv[4]) if len(sys.argv) > 4 else 10
    rows = []
    if which == "indep":
        import indep_gen as G
        for sd in range(5000, 5000 + ns):
            rows += C.judge_c(C.indep_windows(G, sd, 2400), binary)
    else:
        base = 0 if which == "e3a-dev" else 1000
        for sd in range(base, base + ns):
            rows += C.judge_c(C.e3a_windows(sd), binary)
    agree = Counter()
    total = Counter()
    diffs = Counter()
    for x in rows:
        for m in E.METRICS:
            a = x["v"]["loss-aware"][m]
            b = x["v"]["c"][m]
            total[m] += 1
            if a[0] == b[0]:
                agree[m] += 1
            else:
                diffs["%s %s/%s -> %s/%s" % (m, a[0], a[1], b[0], b[1])] += 1
    res = dict(which=which, windows=len(rows),
               verdict_agreement={m: round(agree[m] / total[m], 5) for m in E.METRICS},
               top_differences=diffs.most_common(25),
               g0_mirror=E.g0_verdict(E.g0(rows, "loss-aware")), g0_c=E.g0_verdict(E.g0(rows, "c")))
    out.write_text(json.dumps(res, indent=1))
    print(json.dumps({k: res[k] for k in ("which", "windows", "verdict_agreement")}))
    for d, n in res["top_differences"][:12]:
        print("  %6d  %s" % (n, d))
    for k in ("g0_mirror", "g0_c"):
        g = res[k]
        print(k, "PASS" if g["pass"] else "FAIL",
              {c: (v["rate"], v["upper95"]) for c, v in g["false_claims"].items()},
              "recall", g["recall_material_under_loss"]["rate"], g["recall_material_under_loss"]["lower95"])


if __name__ == "__main__":
    main()
