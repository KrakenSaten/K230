#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Run every bench session (METHOD.md §2) on N parallel, isolated bench
instances (each its own set of namespaces, 10.77.<k>.0/24), as root.

Usage: run_all.py RUNDIR HELPER [N_INSTANCES]
"""
import json
import os
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    run, helper = sys.argv[1], sys.argv[2]
    ninst = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    specs = os.path.join(run, "specs")
    subprocess.run([sys.executable, os.path.join(HERE, "scenarios.py"), specs], check=True)
    ids = json.load(open(os.path.join(specs, "index.json")))
    todo = list(ids)
    lock = threading.Lock()

    def worker(k):
        while True:
            with lock:
                if not todo:
                    return
                sid = todo.pop(0)
            d = os.path.join(run, sid)
            if os.path.exists(os.path.join(d, "capture.pcap")):
                continue
            r = subprocess.run([sys.executable, os.path.join(HERE, "bench.py"), "session", d, str(k),
                                os.path.join(specs, sid + ".json"), helper], capture_output=True, text=True)
            with lock:
                print(sid, "instance", k, (r.stdout.strip() or r.stderr.strip()[-300:]), flush=True)

    ths = [threading.Thread(target=worker, args=(k + 1,)) for k in range(ninst)]
    for t in ths:
        t.start()
    for t in ths:
        t.join()


if __name__ == "__main__":
    main()
