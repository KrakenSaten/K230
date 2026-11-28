#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""G0 on the C engine: the windows of a generator, replayed into ppc_intel.c.

The held-out runs of 27-28 November judged windows with e3a_r.judge(), a
Python mirror of the engine's rules. This module judges them with the
engine itself (intel_replay, built from core/pocketpcap/ppc_intel.c), so
that G0 speaks about the code a helper would run, and measures where the
mirror and the engine part (parity).

  e3a_windows(seed)   E3a-R's windows for a seed (as run_seed() makes them,
                      the same random stream), with the inputs the replay
                      needs and the mirror's verdicts
  indep_windows(mod, seed, n)   a held-out generator's windows (INTERFACE.md)
  judge_c(windows, binary)      the engine's verdicts, one dict a window
"""
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "e3a-r"))
import e3a_r as E  # noqa: E402

KEYS = ("watched", "frames", "own_in", "own_in_dns", "own_out", "arp", "dhcp")
TAGS = ("S", "F", "I", "D", "O", "A", "H")


def readings_mask(r, lost_slot):
    """e3a_r.readings(), drawing the same numbers, and also the mask of
    failed slots the replay needs."""
    n = len(lost_slot)
    fail = np.zeros(n, bool)
    if r.random() < 0.35:
        for _ in range(int(r.integers(1, 3))):
            a = int(r.integers(0, n))
            fail[a:a + int(r.integers(4, 161))] = True
    out = []
    last = 0.0
    acc = 0
    for j in range(n):
        acc += int(lost_slot[j])
        tr = (j + 1) / E.SLOTS
        if not fail[j]:
            if acc:
                out.append((last, tr, acc))
            last = tr
            acc = 0
    return out, last, bool(fail[E.T_BASE * E.SLOTS:(E.T_BASE + E.T_WIN + E.WAIT) * E.SLOTS].any()), ~fail


def e3a_windows(seed, n_sites=40, per_site=60, mirror=True):
    """As e3a_r.run_seed(seed): rows for E.g0(), each with "win" (the
    replay's input) and, if mirror, the mirror's loss-aware verdicts."""
    r = np.random.default_rng(1000 + seed)
    rows = []
    for _ in range(n_sites):
        p = E.site_params(r)
        for _ in range(per_site):
            fault = list(E.FAULTS)[int(r.integers(0, len(E.FAULTS)))] if r.random() < 0.5 else None
            art_win = E.ARTIFACTS[int(r.integers(0, 3))] if r.random() < 0.5 else None
            art_base = E.ARTIFACTS[int(r.integers(0, 3))] if r.random() < 0.2 else None
            sim = E.simulate(r, p, fault, art_win, art_base)
            intervals, last_good, read_fail, ok = readings_mask(r, sim["lost_slot"])
            v = {"loss-aware": E.judge(sim, intervals, last_good, "loss-aware")} if mirror else {}
            win = {k: sim[k] for k in KEYS}
            win["lost_slot"] = sim["lost_slot"]
            win["read_ok"] = ok
            win["qs"] = sim["qs"]
            rows.append(dict(fault=fault, art=art_win, art_base=art_base, q=p["q"], v=v, rose=sim["rose"],
                             selective=sim["selective"], material_slow=sim["material_slow"], read_fail=read_fail,
                             win=win))
    return rows


def indep_windows(gen, seed, n, mirror=True):
    """A generator of the held-out interface: rows for E.g0()."""
    rows = []
    for w in gen.generate(seed, n):
        win = {k: np.asarray(w[k]) for k in KEYS}
        win["lost_slot"] = np.asarray(w["lost_slot"])
        win["read_ok"] = np.asarray(w["read_ok"], bool)
        win["qs"] = w["qs"]
        v = {}
        if mirror:
            sim = dict(win)
            sim["f"] = np.asarray(w["f"])
            iv, last = readings_of(win["lost_slot"], win["read_ok"])
            v["loss-aware"] = E.judge(sim, iv, last, "loss-aware")
        rows.append(dict(fault=w["fault"], art=w["art"], art_base=w["art_base"], q=w["q"], v=v, rose=set(w["rose"]),
                         selective=w["selective"], material_slow=w["material_slow"], read_fail=w["read_fail"],
                         win=win))
    return rows


def readings_of(lost_slot, read_ok):
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


def encode(win):
    parts = ["W"]
    for k, t in zip(KEYS, TAGS):
        parts.append(t + " " + " ".join(str(int(x)) for x in np.asarray(win[k]).astype(int)))
    parts.append("L " + " ".join(str(int(x)) for x in np.asarray(win["lost_slot"]).astype(int)))
    parts.append("R " + " ".join("1" if x else "0" for x in np.asarray(win["read_ok"])))
    qs = [q for q in win["qs"] if q[0] < E.T_BASE + E.T_WIN + E.WAIT]
    parts.append("Q %d" % len(qs))
    for q in qs:
        parts.append("%d %d %d %d" % (int(round(q[0] * 1e6)), 1 if q[1] else 0, 1 if q[2] else 0, int(q[3])))
    parts.append("E")
    return "\n".join(parts)


def judge_c(rows, binary, key="c"):
    """Fill row["v"][key] with the engine's verdicts: {metric: (verdict, reason)}."""
    text = "\n".join(encode(x["win"]) for x in rows) + "\n"
    out = subprocess.run([str(binary)], input=text.encode(), capture_output=True, check=True).stdout.decode()
    lines = out.strip().split("\n")
    if len(lines) != len(rows):
        raise RuntimeError("replay gave %d verdict lines for %d windows" % (len(lines), len(rows)))
    for x, ln in zip(rows, lines):
        x["v"][key] = {m: tuple(w.split(":", 1)) for m, w in zip(E.METRICS, ln.split())}
    return rows
