#!/usr/bin/env python3
"""Summarise score.py's result for the Traffic report: vehicle recall, FN,
FP, the smallest true positive, the synthetic small-car ladder and the
confidence distribution of true-positive vehicles.

Usage: conf_summary.py SCORE.json MODEL [MODEL...]
"""
import json
import sys


def hist(confs, edges=(0.35, 0.5, 0.65, 0.8, 0.9, 1.01)):
    out, lo = [], edges[0]
    for hi in edges[1:]:
        out.append(sum(1 for c in confs if lo <= c < hi))
        lo = hi
    return out


def main():
    score = json.load(open(sys.argv[1]))
    print("%-28s %5s %4s %4s %6s %8s %10s  %s" % ("model", "TP", "FN", "FP", "recall", "min TP", "ladder3/5",
                                                 "TP conf .35/.5/.65/.8/.9"))
    for m in sys.argv[2:]:
        r = score.get(m)
        if r is None:
            print("%-28s (not scored)" % m)
            continue
        t = r["traffic"]
        confs = []
        for k in ("pic_road", "pic_traffic", "pic_roi_road", "pic_roi_traffic"):
            confs += r.get(k, {}).get("conf", [])
        print("%-28s %5d %4d %4d %6.2f %6spx %8spx  %s (named pictures) mean %.2f" % (
            m, t["tp"], t["fn"], t["fp"], t["recall"], t["smallest_tp_w"], r["ladder_min_w_3of5"],
            hist(confs), t["mean_conf"]))
        if t["fp_list"]:
            print("    FP:", ", ".join(t["fp_list"][:12]))


if __name__ == "__main__":
    main()
