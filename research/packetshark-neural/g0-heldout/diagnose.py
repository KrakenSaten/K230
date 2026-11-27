#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Read-only diagnosis of a G0 held-out run: which metric, with which reason,
the frozen loss-aware rule claimed falsely, per healthy category. It changes
no rule and no criterion (G0.md §4); it explains the frozen result.

Usage: python3 diagnose.py independent|synthetic [seed]
"""
import collections
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "e3a-r"))
sys.path.insert(0, str(HERE))
import e3a_r as E  # noqa: E402
import heldout as H  # noqa: E402


def rows_independent(seed):
    import indep_gen as G
    for w in G.generate(seed, 2400):
        sim = dict(w)
        for k in ("watched", "frames", "own_in", "own_in_dns", "own_out", "arp", "dhcp", "lost_slot", "f"):
            sim[k] = np.asarray(sim[k])
        iv, last = H.readings_of(sim["lost_slot"], np.asarray(w["read_ok"], bool))
        yield w, E.judge(sim, iv, last, "loss-aware"), sim, iv, last


def main():
    which = sys.argv[1]
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
    by = collections.defaultdict(collections.Counter)
    n = collections.Counter()
    ex = collections.defaultdict(list)
    for w, v, sim, iv, last in rows_independent(seed):
        if w["fault"]:
            continue
        x = dict(art=w["art"], art_base=w["art_base"], selective=w["selective"], read_fail=w["read_fail"])
        for c in E.g0_categories(x):
            n[c] += 1
            for m in E.METRICS:
                if v[m][0] == E.WORSE and m not in w["rose"]:
                    by[c][m] += 1
                    by[c + " by base artifact"][str(w["art_base"])] += 1
                    if len(ex[(c, m)]) < 2:
                        a, b = E.T_BASE, E.T_BASE + E.T_WIN
                        f_win = E.bounds(iv, last, a, b, int(sim["frames"][a:b].sum()))
                        f_base = E.bounds(iv, last, 0, a, int(sim["frames"][0:a].sum()))
                        ex[(c, m)].append(dict(
                            loss_base=(round(f_base[0], 3), round(f_base[1], 3)),
                            loss_win=(round(f_win[0], 3), round(f_win[1], 3)),
                            true_loss_win=round(float(np.mean(sim["f"][a:b])), 3),
                            arp=(int(sim["arp"][:a].sum()), int(sim["arp"][a:b].sum())),
                            dhcp=(int(sim["dhcp"][:a].sum()), int(sim["dhcp"][a:b].sum())),
                            watched=(int(sim["watched"][:a].sum()), int(sim["watched"][a:b].sum()))))
    for c in E.G0_CATEGORIES:
        print(c, n[c], dict(by[c]), "base artifact of the false claims:", dict(by[c + " by base artifact"]))
    for k, v in ex.items():
        print(k, v)


if __name__ == "__main__":
    main()
