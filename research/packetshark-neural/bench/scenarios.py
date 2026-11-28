#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""The N1.2 bench's sessions (METHOD.md §2), deterministic: one comparison a
session, base 0-60 s, mark at 60 s, window 60-120 s, stop at 127 s.

Usage: scenarios.py OUTDIR   writes one SPEC.json a session and index.json
"""
import json
import os
import random
import sys

R = random.Random(20261128)


def mid():
    return round(R.uniform(78, 92), 1)  # a fault that begins inside the window


def loss_events(t=None, secs=None):
    return [[t or round(R.uniform(68, 108), 1), "pause", secs or round(R.uniform(2.5, 4.0), 1)]]


FAULTS = {
    # mode at onset, the metric it worsens
    "arp_storm": ({"arp_storm": 1.6}, "arp.request"),
    "dhcp_trouble": ({"dhcp_answer": 0}, "dhcp.discover"),
    "dns_down": ({"drop": 0.9}, "dns.answered"),
    "dns_slow": ({"delay_ms": [120, 240]}, "dns.time"),
    "servfail": ({"servfail": 0.5}, "dns.failed"),
}


def spec(sid, kind, fault=None, onset=60.0, art=None, art_base=None, dns_rate=1.0, busy=0, bursty=1):
    ev = []
    if fault:
        m = dict(FAULTS[fault][0])
        if fault == "arp_storm" and onset > 60:
            m["arp_storm"] = 2.6  # stronger, so the whole window still carries a material rise
        ev.append([onset, "mode", m])
    if art == "loss":
        ev += loss_events()
    elif art == "hold":
        t = round(R.uniform(68, 95), 1)
        ev += [[t, "hold", 1], [t + round(R.uniform(10, 20), 1), "hold", 0]]
    elif art == "asym":
        t = round(R.uniform(65, 85), 1)
        ev += [[t, "asym", 1], [t + round(R.uniform(20, 35), 1), "asym", 0]]
    if art_base == "loss":
        ev += loss_events(round(R.uniform(10, 45), 1))
    elif art_base == "hold":
        t = round(R.uniform(10, 35), 1)
        ev += [[t, "hold", 1], [t + round(R.uniform(10, 18), 1), "hold", 0]]
    return dict(id=sid, kind=kind, fault=fault, metric=FAULTS[fault][1] if fault else None, onset=onset, art=art,
                art_base=art_base, dns_rate=dns_rate, busy=busy, arp_bursty=bursty, events=ev)


def sessions():
    out = []
    n = 0

    def add(*a, **k):
        nonlocal n
        n += 1
        out.append(spec("s%02d" % n, *a, **k))

    # healthy: no fault
    for _ in range(4):
        add("clean-periodic", bursty=1)
    for _ in range(4):
        add("clean-busy", busy=1, bursty=1)
    for _ in range(2):
        add("clean-sparse-dns", dns_rate=0.1)
    for _ in range(4):
        add("loss", art="loss", busy=1)
    for _ in range(4):
        add("hold", art="hold")
    for _ in range(2):
        add("hold-base", art_base="hold")
    for _ in range(2):
        add("loss-base", art_base="loss", busy=1)
    for _ in range(4):
        add("asymmetric-return", art="asym")
    # faults at the mark and inside the window
    for f in FAULTS:
        for _ in range(3):
            add("fault", fault=f)
        for _ in range(2 if f.startswith("dns") or f == "servfail" else 3):
            add("fault-mid", fault=f, onset=mid())
    # faults under capture loss
    for f in FAULTS:
        for _ in range(2):
            add("fault-loss", fault=f, art="loss", busy=1)
    return out


def main():
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    ss = sessions()
    for s in ss:
        json.dump(s, open(os.path.join(d, s["id"] + ".json"), "w"), indent=1)
    json.dump([s["id"] for s in ss], open(os.path.join(d, "index.json"), "w"))
    print(len(ss), "sessions")


if __name__ == "__main__":
    main()
