#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""G0, second held-out validation (G0.md §7), run once, on the C engine.

  synthetic    E3a-R's generator unchanged, seeds 3000-3009 (never run;
               1000-1009 and 2000-2009 are development data now)
  independent  indep_gen2.py, a second generator written from G0.md §1-2
               and INTERFACE.md alone by an author who saw neither engine,
               E3a-R, the first independent generator nor any result;
               seeds 7000-7009

Every window is replayed into the frozen engine (intel_replay, built from
core/pocketpcap/ppc_intel.c) and scored by the frozen G0 evaluation
(e3a_r.g0, e3a_r.g0_verdict). The criteria are G0.md §2, unchanged. This
script changes nothing; it records the SHA-256 of what it judged with.

Usage (WSL): python3 heldout2.py BINARY OUT_DIR synthetic|independent
"""
import hashlib
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import cjudge as C  # noqa: E402

E = C.E
ROOT = HERE.parents[2]
FROZEN = [ROOT / "core" / "pocketpcap" / "ppc_intel.c", ROOT / "core" / "pocketpcap" / "ppc_intel.h",
          HERE / "intel_replay.c", HERE / "cjudge.py", HERE / "heldout2.py", HERE.parent / "e3a-r" / "e3a_r.py"]


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def pool(acc, c):
    if acc is None:
        return c
    for k in E.G0_CATEGORIES:
        acc["false_claims"][k] = [a + b for a, b in zip(acc["false_claims"][k], c["false_claims"][k])]
    for k in ("recall_material_under_loss", "false_reassurance_material", "abstention_material"):
        acc[k] = [a + b for a, b in zip(acc[k], c[k])]
    for f in E.FAULTS:
        acc["per_fault"][f] = [a + b for a, b in zip(acc["per_fault"][f], c["per_fault"][f])]
    return acc


def main():
    binary, out, which = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    acc = None
    n = 0
    if which == "synthetic":
        seeds = list(range(3000, 3010))
        for s in range(2000, 2010):  # e3a_windows(s) draws from seed 1000 + s
            rows = C.judge_c(C.e3a_windows(s, mirror=False), binary)
            n += len(rows)
            acc = pool(acc, E.g0(rows, "c"))
    else:
        import indep_gen2 as G
        seeds = list(range(7000, 7010))
        for sd in seeds:
            rows = C.judge_c(C.indep_windows(G, sd, 2400, mirror=False), binary)
            n += len(rows)
            acc = pool(acc, E.g0(rows, "c"))
    res = dict(run=which, windows=n, seeds=seeds, engine="C (intel_replay)",
               frozen_sha256={p.name: sha(p) for p in FROZEN}, binary_sha256=sha(binary),
               g0=E.g0_verdict(acc), counts=acc, seconds=round(time.time() - t0, 1))
    if which == "independent":
        res["generator_sha256"] = sha(HERE / "indep_gen2.py")
    (out / ("g0_heldout2_%s.json" % which)).write_text(json.dumps(res, indent=1, default=list))
    g = res["g0"]
    print("%s: %s  false claims (rate, upper95, n): %s  recall %s (lower95 %s)  reassurance %s  abstention %s" % (
        which, "PASS" if g["pass"] else "FAIL",
        {c: (v["rate"], v["upper95"], v["n"]) for c, v in g["false_claims"].items()},
        g["recall_material_under_loss"]["rate"], g["recall_material_under_loss"]["lower95"],
        g["false_reassurance_material"]["rate"], g["abstention_material"]["rate"]))


if __name__ == "__main__":
    main()
