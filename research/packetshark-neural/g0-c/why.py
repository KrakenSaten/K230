#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Development diagnosis on the C engine: for material faults under loss,
the verdict and reason on the faulted metric, per fault, for one or two
replay binaries. Development data only.

Usage: python3 why.py e3a-dev|indep N_SEEDS BINARY [BINARY2]
"""
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "g0-heldout"))
import cjudge as C  # noqa: E402

E = C.E


def main():
    which, ns = sys.argv[1], int(sys.argv[2])
    bins = sys.argv[3:]
    rows = []
    if which == "indep":
        import indep_gen as G
        for sd in range(5000, 5000 + ns):
            rows += C.indep_windows(G, sd, 2400, mirror=False)
    else:
        for sd in range(0, ns):
            rows += C.e3a_windows(sd, mirror=False)
    for i, b in enumerate(bins):
        C.judge_c(rows, b, key="c%d" % i)
    for f in E.MATERIAL_FAULTS:
        t = E.FAULTS[f]
        sel = [x for x in rows if x["fault"] == f and x["art"] == "loss"]
        print("== %s under loss (%d windows), metric %s" % (f, len(sel), t))
        for i in range(len(bins)):
            cnt = Counter("%s/%s" % x["v"]["c%d" % i][t] for x in sel)
            print("   bin%d: %s" % (i, ", ".join("%s %d" % kv for kv in cnt.most_common(6))))


if __name__ == "__main__":
    main()
