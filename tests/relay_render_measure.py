#!/usr/bin/env python3
"""Alternate already-built replay binaries; retain every run and peak host RSS.
Copyright (c) 2026 PocketOS authors. SPDX-License-Identifier: Apache-2.0
"""
import argparse
import json
import pathlib
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=pathlib.Path)
parser.add_argument("binaries", nargs="+", help="label=/absolute/path/to/replay")
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
variants = [entry.split("=", 1) for entry in args.binaries]
records = []
for run in range(3):
    for label, executable in variants[::1 if run % 2 == 0 else -1]:
        capture = args.output / label
        capture.mkdir(exist_ok=True)
        with (capture / f"run-{run + 1}.txt").open("w") as out:
            child = subprocess.Popen([executable, str(capture)], stdout=out)
            peak_kib = 0
            while child.poll() is None:
                try:
                    status = pathlib.Path(f"/proc/{child.pid}/status").read_text()
                    for line in status.splitlines():
                        if line.startswith("VmHWM:"):
                            peak_kib = max(peak_kib, int(line.split()[1]))
                except FileNotFoundError:
                    pass
                time.sleep(0.005)
            if child.returncode:
                raise SystemExit(f"{label} failed: {child.returncode}")
        records.append(dict(run=run + 1, label=label, peak_rss_kib=peak_kib,
                            output=(capture / f"run-{run + 1}.txt").read_text()))
(args.output / "runs.json").write_text(json.dumps(records, indent=2) + "\n")
