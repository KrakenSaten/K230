#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""N1.2 bench evaluation (METHOD.md §3-4), frozen before any scored run.

For every session: the ground truth from the endpoints' own logs, the
engine's verdicts from bench_replay, and the counts METHOD.md reports.

Usage: evaluate.py RUNDIR BENCH_REPLAY > results.json
"""
import json
import math
import os
import statistics
import subprocess
import sys

METRICS = ["dns.time", "dns.answered", "dns.failed", "dhcp.discover", "arp.request"]
MATERIAL = dict(arp_storm=5.0, dhcp_trouble=0.3, dns_down=0.70, dns_slow=3.0, servfail=0.30)
BASE = (0, 60000)
WIN = (60000, 120000)


def wilson(k, n, z=1.645):
    if n == 0:
        return (0.0, 1.0)
    p = k / n
    d = 1 + z * z / n
    c = p + z * z / (2 * n)
    w = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))
    return (max(0.0, (c - w) / d), min(1.0, (c + w) / d))


def lines(path):
    try:
        return [l.split() for l in open(path)]
    except FileNotFoundError:
        return []


def truth(d, start):
    """What the endpoints did, by window: never read from the capture."""
    rel = lambda t: int(t) - start
    inw = lambda t, w: w[0] <= rel(t) < w[1]
    t = {}
    arp = [l[0] for i in range(4) for l in lines(os.path.join(d, "truth-arp%d.log" % i))]
    t["arp"] = [sum(inw(x, w) for x in arp) for w in (BASE, WIN)]
    disc = [l[0] for n in ("dhcpc", "dhcpc2") for l in lines(os.path.join(d, "truth-%s.log" % n)) if l[1] == "discover"]
    t["discover_per_s"] = [sum(inw(x, w) for x in disc) / 60.0 for w in (BASE, WIN)]
    dns = lines(os.path.join(d, "truth-dns.log"))
    for k, w in (("base", BASE), ("win", WIN)):
        q = [l for l in dns if inw(l[0], w)]
        ans = [l for l in q if l[2] in ("answer", "servfail")]
        t["dns_" + k] = dict(questions=len(q), dropped=sum(l[2] == "drop" for l in q),
                             servfail=sum(l[2] == "servfail" for l in q),
                             delay_median=statistics.median([int(l[3]) for l in ans]) if ans else None)
    return t


def worsened(t):
    """The metrics whose true level worsened materially (METHOD.md §3)."""
    out = set()
    b, w = t["dns_base"], t["dns_win"]
    if w["questions"] and w["dropped"] / w["questions"] >= MATERIAL["dns_down"]:
        out.add("dns.answered")
    if b["delay_median"] and w["delay_median"] and w["delay_median"] >= MATERIAL["dns_slow"] * b["delay_median"]:
        out.add("dns.time")
    answered = w["questions"] - w["dropped"]
    if answered and w["servfail"] / answered >= MATERIAL["servfail"]:
        out.add("dns.failed")
    if t["arp"][0] and t["arp"][1] >= MATERIAL["arp_storm"] * t["arp"][0]:
        out.add("arp.request")
    if t["discover_per_s"][1] - t["discover_per_s"][0] >= MATERIAL["dhcp_trouble"]:
        out.add("dhcp.discover")
    return out


def changed_at_all(t):
    """Metrics whose true level moved in the harmful direction at all (a
    claim of those is never false, METHOD.md §3)."""
    out = set()
    b, w = t["dns_base"], t["dns_win"]
    rate = lambda x, n: x / n if n else 0.0
    if rate(w["dropped"], w["questions"]) > rate(b["dropped"], b["questions"]) + 0.05:
        out.add("dns.answered")
    if b["delay_median"] and w["delay_median"] and w["delay_median"] > 1.5 * b["delay_median"]:
        out.add("dns.time")
    if rate(w["servfail"], w["questions"] - w["dropped"]) > rate(b["servfail"], b["questions"] - b["dropped"]) + 0.05:
        out.add("dns.failed")
    if t["arp"][0] and t["arp"][1] > 1.5 * t["arp"][0]:
        out.add("arp.request")
    if t["discover_per_s"][1] > t["discover_per_s"][0] + 0.1:
        out.add("dhcp.discover")
    return out


def category(spec):
    if spec["fault"]:
        return None
    if spec["art"] == "loss":
        return "loss-burst"
    if spec["art"] == "hold":
        return "hold"
    if spec["art"] == "asym":
        return "asymmetric-return"
    if spec["art_base"]:
        return "base-artifact"
    return "clean"


def main():
    run, binary = sys.argv[1], sys.argv[2]
    sessions = json.load(open(os.path.join(run, "specs", "index.json")))
    rows = []
    for sid in sessions:
        d = os.path.join(run, sid)
        spec = json.load(open(os.path.join(run, "specs", sid + ".json")))
        cap, side = os.path.join(d, "capture.pcap"), os.path.join(d, "sidecar.txt")
        if not (os.path.exists(cap) and os.path.exists(side)):
            rows.append(dict(id=sid, error="no capture"))
            continue
        start = int([l for l in lines(side) if l[0] == "start"][0][1])
        out = subprocess.run([binary, cap, side], capture_output=True, text=True, check=True).stdout.split("\n")
        v = {}
        for ln in out:
            f = ln.split()
            if len(f) == 8:
                v[f[0]] = dict(verdict=f[1], reason=f[2], quality=f[3], n=[int(f[4]), int(f[5])], k=[int(f[6]), int(f[7])])
        t = truth(d, start)
        rows.append(dict(id=sid, spec=spec, truth=t, worsened=sorted(worsened(t)), changed=sorted(changed_at_all(t)),
                         verdicts=v))
    res = dict(sessions=len(rows), errors=[r["id"] for r in rows if "error" in r], rows=rows)
    good = [r for r in rows if "error" not in r]
    # false problem claims in healthy windows, by category
    fc = {}
    for r in good:
        c = category(r["spec"])
        if c is None:
            continue
        false = [m for m in METRICS if r["verdicts"][m]["verdict"] == "worse" and m not in r["changed"]]
        fc.setdefault(c, [0, 0, []])
        fc[c][0] += bool(false)
        fc[c][1] += 1
        if false:
            fc[c][2].append((r["id"], false))
    # false claims on the other metrics of faulted windows
    oth = [0, 0, []]
    for r in good:
        if r["spec"]["fault"]:
            false = [m for m in METRICS if r["verdicts"][m]["verdict"] == "worse" and m not in r["changed"]]
            oth[0] += bool(false)
            oth[1] += 1
            if false:
                oth[2].append((r["id"], false))
    # faults: on the faulted metric, material by the endpoints' logs
    per = {}
    for r in good:
        f = r["spec"]["fault"]
        if not f:
            continue
        m = r["spec"]["metric"]
        mat = m in r["worsened"]
        key = (f, "mid" if r["spec"]["onset"] > 60 else "mark", "loss" if r["spec"]["art"] == "loss" else "clear")
        x = per.setdefault("|".join(key), dict(n=0, material=0, worse=0, cant=0, no_change=0, small=0, reasons={}))
        x["n"] += 1
        x["material"] += mat
        vv = r["verdicts"][m]
        if mat:
            x["worse"] += vv["verdict"] == "worse"
            x["cant"] += vv["verdict"] == "cant-tell"
            x["no_change"] += vv["verdict"] == "no-change"
            x["small"] += vv["verdict"] == "small"
            if vv["verdict"] == "cant-tell":
                x["reasons"][vv["reason"]] = x["reasons"].get(vv["reason"], 0) + 1
    mats = [r for r in good if r["spec"]["fault"] and r["spec"]["metric"] in r["worsened"]]
    under_loss = [r for r in mats if r["spec"]["art"] == "loss"]
    hit = lambda rs: sum(r["verdicts"][r["spec"]["metric"]]["verdict"] == "worse" for r in rs)
    count = lambda rs, v: sum(r["verdicts"][r["spec"]["metric"]]["verdict"] == v for r in rs)
    res["false_claims"] = {c: dict(k=k, n=n, rate=k / n if n else None, upper95=wilson(k, n)[1], which=w)
                           for c, (k, n, w) in fc.items()}
    res["false_claims_other_metrics_in_fault_windows"] = dict(k=oth[0], n=oth[1], upper95=wilson(oth[0], oth[1])[1],
                                                              which=oth[2])
    res["material_faults"] = dict(n=len(mats), recall=hit(mats), recall_lower95=wilson(hit(mats), len(mats))[0],
                                  abstained=count(mats, "cant-tell"), no_change=count(mats, "no-change"),
                                  small=count(mats, "small"))
    res["material_faults_under_loss"] = dict(n=len(under_loss), recall=hit(under_loss),
                                             recall_lower95=wilson(hit(under_loss), len(under_loss))[0])
    res["per_fault"] = per
    json.dump(res, sys.stdout, indent=1, default=list)


if __name__ == "__main__":
    main()
