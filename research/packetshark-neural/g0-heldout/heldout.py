#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""G0 held-out validation (docs/apps/packetshark/neural/G0.md §4), run once.

  synthetic    E3a-R's generator, rules and evaluation unchanged, seeds
               2000-2009 (never run before; the development set is 1000-1009)
  independent  the windows of indep_gen.py, a generator written from G0.md §1
               and INTERFACE.md alone by an author who did not see the rules

Both are judged by the frozen rules (e3a_r.judge, mirrored from ppc_intel.c)
and scored by the frozen G0 evaluation (e3a_r.g0, e3a_r.g0_verdict). This
script imports them and changes nothing; it records the SHA-256 of the files
it judged with, so the run can be tied to the freeze.

Usage (WSL, numpy only): python3 heldout.py OUT_DIR synthetic|independent
"""
import hashlib
import json
import sys
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "e3a-r"))
import e3a_r as E  # noqa: E402

FROZEN = [HERE.parent / "e3a-r" / "e3a_r.py", HERE.parents[2] / "core" / "pocketpcap" / "ppc_intel.c"]


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


def readings_of(lost_slot, read_ok):
    """The helper's counter: a good reading reports what was lost since the
    last good one; a failed one reports nothing (as E3a-R's readings())."""
    out = []
    last = 0.0
    acc = 0
    for j in range(len(lost_slot)):
        acc += int(lost_slot[j])
        tr = (j + 1) / E.SLOTS
        if read_ok[j]:
            if acc:
                out.append((last, tr, acc))
            last = tr
            acc = 0
    return out, last


def synthetic():
    acc = {m: None for m in E.METHODS}
    n = 0
    for s in range(1000, 1010):  # E3a-R seeds 1000 + s: 2000-2009
        rows = E.run_seed(s, 40, 60)
        n += len(rows)
        for m in E.METHODS:
            acc[m] = pool(acc[m], E.g0(rows, m))
    return acc, n, list(range(2000, 2010))


def independent():
    import indep_gen as G
    acc = {m: None for m in E.METHODS}
    n = 0
    seeds = list(range(5000, 5010))
    for sd in seeds:
        rows = []
        for w in G.generate(sd, 2400):
            sim = dict(w)
            for k in ("watched", "frames", "own_in", "own_in_dns", "own_out", "arp", "dhcp", "lost_slot", "f"):
                sim[k] = np.asarray(sim[k])
            iv, last = readings_of(sim["lost_slot"], np.asarray(w["read_ok"], bool))
            v = {m: E.judge(sim, iv, last, m) for m in E.METHODS}
            rows.append(dict(fault=w["fault"], art=w["art"], art_base=w["art_base"], q=w["q"], v=v, rose=set(w["rose"]),
                             selective=w["selective"], material_slow=w["material_slow"], read_fail=w["read_fail"]))
        n += len(rows)
        for m in E.METHODS:
            acc[m] = pool(acc[m], E.g0(rows, m))
    return acc, n, seeds


def main():
    out = Path(sys.argv[1])
    which = sys.argv[2]
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    acc, n, seeds = synthetic() if which == "synthetic" else independent()
    res = dict(run=which, windows=n, seeds=seeds, frozen_sha256={p.name: sha(p) for p in FROZEN},
               g0={m: E.g0_verdict(acc[m]) for m in E.METHODS}, seconds=round(time.time() - t0, 1))
    if which == "independent":
        res["generator_sha256"] = sha(HERE / "indep_gen.py")
    (out / ("g0_heldout_%s.json" % which)).write_text(json.dumps(res, indent=1))
    for m in E.METHODS:
        g = res["g0"][m]
        print("%-12s %s  false claims (rate, upper95): %s  recall %s (lower95 %s)  reassurance %s  abstention %s" % (
            m, "PASS" if g["pass"] else "FAIL",
            {c: (v["rate"], v["upper95"], v["n"]) for c, v in g["false_claims"].items()},
            g["recall_material_under_loss"]["rate"], g["recall_material_under_loss"]["lower95"],
            g["false_reassurance_material"]["rate"], g["abstention_material"]["rate"]))


if __name__ == "__main__":
    main()
