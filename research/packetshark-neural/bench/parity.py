#!/usr/bin/env python3
"""N1.2 replay parity: the engine as linked into pos-capture, live on the
bench, against bench_replay on the capture and sidecar of the same session.

Not part of the frozen evaluation (METHOD.md §5): bench.py is loaded
unchanged from disk and given two extra session actions in memory only —
"send" (a line to the helper: the mark, at 60 s) and, after STOP, the
protocol 7 questions for the mark's two pairs. The live answer (helper.out,
in.cmp lines) and the replay's line for each metric are then compared:
verdict, reason and quality must agree; counts are reported with their
differences, which come from the two clocks (the helper's own against the
pcap's timestamps less the sidecar's START) and from the lost-frame readings
being the helper's own live and the sidecar's in the replay.

Usage (root, on the bench host):
  parity.py OUTDIR HELPER BENCH_REPLAY K SCENARIO_ID...
"""
import json
import os
import subprocess
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scenarios  # noqa: E402

METRICS = ["dns.time", "dns.answered", "dns.failed", "dhcp.discover", "arp.request"]
VERDICTS = ["pending", "no-change", "worse", "better", "cant-tell", "small"]
QUALITY = ["good", "degraded", "insufficient"]
REASONS = ["-", "not-captured", "not-analyzed", "cut", "history", "not-watched", "one-way", "loss-unknown",
           "too-few", "loss-explains", "weak", "unseen"]


def load_bench():
    path = os.path.join(HERE, "bench.py")
    src = open(path).read()
    a = '        elif act == "asym":'
    b = '    a.sendall(b"save\\n")'
    assert src.count(a) == 1 and src.count(b) == 1
    src = src.replace(a, '        elif act == "send":\n            a.sendall(arg.encode())\n' + a, 1)
    src = src.replace(b, '    for q in spec.get("after_stop", []):\n        a.sendall(q.encode())\n'
                         '        time.sleep(0.4)\n' + b, 1)
    mod = types.ModuleType("bench_parity")
    mod.__file__ = path  # the agents are started from bench.py itself
    exec(compile(src, path, "exec"), mod.__dict__)
    return mod


def live_answer(out, req):
    rows = {}
    on = False
    for ln in open(os.path.join(out, "helper.out")):
        f = ln.rstrip("\n").split(" ", 1)[1].split("\t")
        if f[0] == "in.begin":
            on = f[1] == str(req)
        elif f[0] == "in.end":
            on = False
        elif on and f[0] == "in.cmp":
            v = [int(x) for x in f[1:10]]
            rows[METRICS[v[0]]] = dict(verdict=VERDICTS[v[1]], quality=QUALITY[v[2]], reason=REASONS[v[3]],
                                       n=v[5:7], k=v[7:9])
    return rows


def replay_answer(out, replay, mark_s):
    r = subprocess.run([replay, os.path.join(out, "capture.pcap"), os.path.join(out, "sidecar.txt"),
                        "%.3f" % mark_s], capture_output=True, text=True, check=True)
    rows = {}
    for ln in r.stdout.split("\n"):
        f = ln.split()
        if len(f) == 8:
            rows[f[0]] = dict(verdict=f[1], reason=f[2], quality=f[3], n=[int(f[4]), int(f[5])],
                              k=[int(f[6]), int(f[7])])
    return rows


def main():
    out, helper, replay, k = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    want = sys.argv[5:]
    bench = load_bench()
    specs = {s["id"]: s for s in scenarios.sessions()}
    report = []
    for sid in want:
        spec = json.loads(json.dumps(specs[sid]))
        spec["events"] = spec.get("events", []) + [[60.0, "send", "mark\n"]]
        spec["after_stop"] = ["intel 9001 0 1\n", "intel 9002 0 0\n"]
        d = os.path.join(out, sid)
        ok = bench.session(d, k, spec, helper)
        start = [int(x.split()[1]) for x in open(os.path.join(d, "sidecar.txt")) if x.startswith("start ")][0]
        sent = [int(x.split()[0]) for x in open(os.path.join(d, "truth-events.log")) if " send " in x][0]
        mark_s = (sent - start) / 1000.0
        live = live_answer(d, 9001)
        rep = replay_answer(d, replay, mark_s) if ok else {}
        rows = []
        for m in METRICS:
            a, b = live.get(m), rep.get(m)
            same = bool(a and b and all(a[x] == b[x] for x in ("verdict", "reason", "quality")))
            rows.append(dict(metric=m, live=a, replay=b, same=same,
                             dn=[a["n"][i] - b["n"][i] for i in (0, 1)] if a and b else None,
                             dk=[a["k"][i] - b["k"][i] for i in (0, 1)] if a and b else None))
        report.append(dict(id=sid, kind=spec["kind"], fault=spec.get("fault"), saved=ok, mark_s=mark_s,
                           pair0_answered=bool(live_answer(d, 9002)), rows=rows))
        print(sid, spec["kind"], spec.get("fault"), "agree %d/5" % sum(r["same"] for r in rows), flush=True)
    json.dump(report, open(os.path.join(out, "parity.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
