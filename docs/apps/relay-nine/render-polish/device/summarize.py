#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Summarize the PR #107 unit B DOORS_RELAY_STATS files: per run and per scene/variant,
over windows that are at least 90% gameplay frames.  summarize.py <dir>"""
import glob, os, re, statistics as st, sys
root = sys.argv[1]
F = ['interval_ms', 'scene_ms', 'draw_ms', 'flush_ms', 'wait_ms']
rows = {}
for path in sorted(glob.glob(os.path.join(root, '**', '*stats-*.txt'), recursive=True)):
    label = os.path.basename(path).split('stats-', 1)[1][:-4]
    if 'cap' in label: continue
    v, s = label.split('-', 1)
    scene = s.rsplit('-', 1)[0]
    wins = []
    for line in open(path):
        if not line.startswith('t='): continue
        kv = dict(x.split('=', 1) for x in line.split() if '=' in x)
        fr, pl = int(kv['frames']), int(kv['playing'])
        if fr == 0 or pl < 0.9 * fr or kv.get('paused') != '0': continue
        w = {'frames': fr, 'stalls': int(kv['stalls_100ms'])}
        for f in F:
            mean, p50, p95, mx = map(float, kv[f].split('/'))
            w[f] = (p50, p95, mx)
        wins.append(w)
    rows.setdefault((scene, v), []).append((label, wins))
meas = {}
for path in glob.glob(os.path.join(root, '**', '*measure.txt'), recursive=True):
    for line in open(path):
        m = dict(x.split('=', 1) for x in line.split()[1:] if '=' in x)
        meas[m['label']] = m
for (scene, v), runs in sorted(rows.items()):
    print('== %s %s' % (scene, v))
    allw = [w for _, ws in runs for w in ws]
    for label, ws in runs:
        m = meas.get(label, {})
        print('  %-14s windows=%d fps=%.1f stalls=%d | interval p50 %.1f p95 %.1f | scene p50 %.2f p95 %.2f | draw p50 %.2f p95 %.2f | flush p50 %.2f p95 %.2f | cpu %s rss %s hwm %s starts %s' % (
            label, len(ws), st.mean(w['frames'] for w in ws) / 5, sum(w['stalls'] for w in ws),
            st.median(w['interval_ms'][0] for w in ws), st.median(w['interval_ms'][1] for w in ws),
            st.median(w['scene_ms'][0] for w in ws), st.median(w['scene_ms'][1] for w in ws),
            st.median(w['draw_ms'][0] for w in ws), st.median(w['draw_ms'][1] for w in ws),
            st.median(w['flush_ms'][0] for w in ws), st.median(w['flush_ms'][1] for w in ws),
            m.get('shell_cpu_avg'), m.get('rss_kb'), m.get('hwm_kb'), m.get('pcm_stream_starts')))
    print('  ALL windows=%d fps=%.2f' % (len(allw), st.mean(w['frames'] for w in allw) / 5))
    for f in F[:4]:
        p50 = sorted(w[f][0] for w in allw); p95 = sorted(w[f][1] for w in allw)
        print('    %-12s median(window p50) %.2f [%.2f-%.2f]  median(window p95) %.2f [%.2f-%.2f]  max %.1f' % (
            f, st.median(p50), p50[0], p50[-1], st.median(p95), p95[0], p95[-1], max(w[f][2] for w in allw)))
