#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""E3a: does a small temporal model add anything over robust statistics for
"something changed in the last 30 s" on the helper's per-second telemetry,
and does it stay quiet on a visibility gap?

Methods (all mask-aware except where stated):
  B1 robust-z      max over features of |median(recent z)|, z normalised by
                   the reference part's median/MAD; a feature abstains with
                   fewer than 8 defined recent bins
  B2 top3-z        sum of the three largest B1 terms (cheap multivariate)
  B3 cusum         two-sided CUSUM (k=0.5) on recent z, max over features
  N1 tcn-ae        bottleneck autoencoder trained on BENIGN and GAP windows
                   only (no fault labels); score = masked recent residual
  N2 tcn-sup       supervised classifier trained on five fault kinds; tested
                   also on three kinds it never saw
Train sites and test sites are disjoint; thresholds come from benign windows
of separate validation sites (false-alert budget), never from test data.

Writes OUT/window_results.json, OUT/{ae,sup}.onnx, OUT/{calib,test}_x.npy
(model inputs for the nncase step) and OUT/test_meta.npz.
Usage (venv-train): exp_window.py OUT
"""
import json
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

sys.path.insert(0, str(Path(__file__).parent))
import synth_telemetry as st  # noqa: E402

TRAIN_FAULTS = ["dns_slow", "dns_down", "path_loss", "arp_storm", "dhcp_trouble"]
UNSEEN_FAULTS = ["dns_servfail", "upstream_latency", "subtle_multi"]
R0 = st.T - st.RECENT
FULL_COV = 0.98  # a bin with any known capture loss or one-way visibility is not evidence
VALID_CH = {7: st.F + 1, 11: st.F + 2}


def recent_masks(x):
    """per feature, which recent bins are defined -> [N,F,RECENT] bool"""
    cov = x[:, st.F, 0, R0:] >= FULL_COV
    m = np.repeat(cov[:, None, :], st.F, axis=1)
    for f, c in VALID_CH.items():
        m[:, f] &= x[:, c, 0, R0:] > 0.5
    return m


def stat_scores(x):
    z = x[:, :st.F, 0, R0:]
    m = recent_masks(x)
    N = x.shape[0]
    terms = np.zeros((N, st.F))
    cus = np.zeros((N, st.F))
    for n in range(N):
        for f in range(st.F):
            v = z[n, f][m[n, f]]
            if v.size >= 8:
                terms[n, f] = abs(np.median(v))
                sp = sn = 0.0
                best = 0.0
                for e in v:
                    sp = max(0.0, sp + e - 0.5)
                    sn = max(0.0, sn - e - 0.5)
                    best = max(best, sp, sn)
                cus[n, f] = best
    return {"B1_robust_z": terms.max(1), "B2_top3_z": np.sort(terms, 1)[:, -3:].sum(1),
            "B3_cusum": cus.max(1)}, terms


RATE_RULES = [("retrans_per_pkt", 13, (0, 1)), ("unanswered_per_query", 9, (5,)), ("fail_per_answer", 8, (6,)),
              ("rst_per_syn", 12, (10,))]
LAT_RULES = [("dns_rtt_up", 7), ("handshake_rtt_up", 11)]
CNT_RULES = [("arp_events_up", 14), ("dhcp_msgs_up", 15)]
QUALITY_NAMES = [r[0] for r in RATE_RULES] + [r[0] for r in LAT_RULES] + [r[0] for r in CNT_RULES]


def quality_scores(X, C):
    """B4: one-sided, domain-informed rules on raw telemetry - the Network
    Doctor baseline. Rates are normalised by their volume (binomial z), RTT
    medians compared to the reference MAD, event counts by a Poisson z. A
    rule abstains (0) below a minimum count. Volume changes alone never score."""
    N = X.shape[0]
    out = np.zeros((N, len(QUALITY_NAMES)))
    for n in range(N):
        cov = C[n] >= FULL_COV
        ref = cov.copy(); ref[R0:] = False
        rec = cov.copy(); rec[:R0] = False
        raw = np.expm1(np.nan_to_num(X[n], nan=0.0))
        j = 0
        for _, num, dens in RATE_RULES:
            den = sum(raw[d] for d in dens)
            nr, nc = den[ref].sum(), den[rec].sum()
            if nr >= 20 and nc >= 20:
                pr = (raw[num][ref].sum() + 0.5) / (nr + 1)
                pc = raw[num][rec].sum() / nc
                out[n, j] = max(0.0, (pc - pr) / np.sqrt(pr * (1 - min(pr, 0.99)) / nc))
            j += 1
        for _, ch in LAT_RULES:
            v = X[n, ch]
            ok = ~np.isnan(v) & cov
            a, b = v[ok & ref], v[ok & rec]
            if a.size >= 10 and b.size >= 5:
                med = np.median(a)
                mad = max(1.4826 * np.median(np.abs(a - med)), 0.05)
                out[n, j] = max(0.0, (np.median(b) - med) / mad)
            j += 1
        for _, ch in CNT_RULES:
            lam = (raw[ch][ref].sum() + 0.5) / max(ref.sum(), 1)
            exp = lam * rec.sum()
            out[n, j] = max(0.0, (raw[ch][rec].sum() - exp) / np.sqrt(exp + 1.0))
            j += 1
    return out


class Enc(nn.Module):
    def __init__(s, cin=st.F + 3, w=32):
        super().__init__()
        s.net = nn.Sequential(
            nn.Conv2d(cin, w, (1, 5), padding=(0, 2)), nn.ReLU(),
            nn.Conv2d(w, w, (1, 3), stride=(1, 2), padding=(0, 1)), nn.ReLU(),
            nn.Conv2d(w, w, (1, 3), stride=(1, 2), padding=(0, 1)), nn.ReLU(),
            nn.Conv2d(w, 16, (1, 3), stride=(1, 2), padding=(0, 1)), nn.ReLU())

    def forward(s, x):
        return torch.flatten(s.net(x), 1)  # 16 x 15


class AE(nn.Module):
    def __init__(s):
        super().__init__()
        s.enc = Enc()
        s.dec = nn.Sequential(nn.Linear(240, 32), nn.ReLU(), nn.Linear(32, st.F * st.RECENT))

    def forward(s, x):
        return s.dec(s.enc(x))


class Sup(nn.Module):
    def __init__(s):
        super().__init__()
        s.enc = Enc()
        s.head = nn.Sequential(nn.Linear(240, 32), nn.ReLU(), nn.Linear(32, 1))

    def forward(s, x):
        return s.head(s.enc(x))


def ae_score(pred, x):
    z = x[:, :st.F, 0, R0:]
    m = recent_masks(x)
    p = pred.reshape(-1, st.F, st.RECENT)
    r = ((p - z) ** 2) * m
    per_f = r.sum(2) / np.maximum(m.sum(2), 1)
    per_f[m.sum(2) < 8] = 0.0  # abstain per feature like B1
    return per_f.mean(1), per_f


def train(model, x, y, loss_fn, epochs, lr=2e-3, bs=64, seed=0):
    torch.manual_seed(seed)
    opt = torch.optim.Adam(model.parameters(), lr=lr)
    X = torch.from_numpy(x)
    Y = torch.from_numpy(y)
    n = X.shape[0]
    for ep in range(epochs):
        perm = torch.randperm(n)
        tot = 0.0
        for i in range(0, n, bs):
            idx = perm[i:i + bs]
            opt.zero_grad()
            loss = loss_fn(model(X[idx]), Y[idx], X[idx])
            loss.backward()
            opt.step()
            tot += float(loss) * len(idx)
    return tot / n


def auc(pos, neg):
    s = np.concatenate([pos, neg])
    rk = s.argsort().argsort().astype(np.float64) + 1
    return float((rk[:len(pos)].sum() - len(pos) * (len(pos) + 1) / 2) / (len(pos) * len(neg)))


def evaluate(name, score_val_benign, score_test, kinds):
    out = {}
    for fa in (0.01, 0.05):
        thr = float(np.quantile(score_val_benign, 1 - fa))
        hit = score_test > thr
        d = {"threshold": round(thr, 4),
             "benign_alert_rate": round(float(hit[kinds == "BENIGN"].mean()), 4),
             "gap_alert_rate": round(float(hit[np.isin(kinds, st.GAPS)].mean()), 4)}
        for g in st.GAPS:
            d["alert_rate_" + g] = round(float(hit[kinds == g].mean()), 4)
        for k in TRAIN_FAULTS + UNSEEN_FAULTS:
            d["tpr_" + k] = round(float(hit[kinds == k].mean()), 3)
        d["tpr_seen_kinds"] = round(float(hit[np.isin(kinds, TRAIN_FAULTS)].mean()), 3)
        d["tpr_unseen_kinds"] = round(float(hit[np.isin(kinds, UNSEEN_FAULTS)].mean()), 3)
        out[f"at_fa_{fa}"] = d
    fault = np.isin(kinds, TRAIN_FAULTS + UNSEEN_FAULTS)
    out["auroc_fault_vs_benign"] = round(auc(score_test[fault], score_test[kinds == "BENIGN"]), 4)
    out["auroc_fault_vs_gap"] = round(auc(score_test[fault], score_test[np.isin(kinds, st.GAPS)]), 4)
    return out


def main(out):
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    # disjoint site populations (different seeds)
    Xtr, Ctr, Ktr, _ = st.dataset(40, 60, 1, ["BENIGN"] + TRAIN_FAULTS, gap_share=0.15)
    Xva, Cva, Kva, _ = st.dataset(10, 60, 2, ["BENIGN"], gap_share=0.0)
    Xte, Cte, Kte, Ste = st.dataset(15, 120, 3, ["BENIGN"] * 3 + TRAIN_FAULTS + UNSEEN_FAULTS, gap_share=0.15)
    xtr, xva, xte = st.model_input(Xtr, Ctr), st.model_input(Xva, Cva), st.model_input(Xte, Cte)
    gen_s = time.time() - t0
    res = {"n": {"train": len(Ktr), "val_benign": len(Kva), "test": len(Kte)},
           "test_kinds": {k: int((Kte == k).sum()) for k in np.unique(Kte)}, "gen_s": round(gen_s, 1)}

    sv, _ = stat_scores(xva)
    stt, terms_te = stat_scores(xte)
    res["methods"] = {k: evaluate(k, sv[k], stt[k], Kte) for k in sv}
    qv, qt = quality_scores(Xva, Cva), quality_scores(Xte, Cte)
    res["methods"]["B4_quality_rules"] = evaluate("q", qv.max(1), qt.max(1), Kte)
    fault = np.isin(Kte, TRAIN_FAULTS + UNSEEN_FAULTS)
    top = np.array(QUALITY_NAMES)[qt.argmax(1)]
    res["B4_top_rule_by_fault"] = {k: {r: int((top[Kte == k] == r).sum()) for r in np.unique(top[Kte == k])}
                                   for k in TRAIN_FAULTS + UNSEEN_FAULTS}
    # control: the same rules without coverage masks (a lossy or one-way bin read as evidence)
    res["methods"]["B4_quality_rules_NO_MASK"] = evaluate("nm", quality_scores(Xva, np.ones_like(Cva)).max(1),
                                                          quality_scores(Xte, np.ones_like(Cte)).max(1), Kte)
    # N3: a tiny learned fusion (logistic regression, 9 weights) over the rule evidence vector
    qtr = quality_scores(Xtr, Ctr)
    torch.manual_seed(0)
    lr_w = nn.Linear(len(QUALITY_NAMES), 1)
    opt = torch.optim.Adam(lr_w.parameters(), lr=0.05, weight_decay=1e-3)
    ftr = torch.from_numpy(np.log1p(qtr).astype(np.float32))
    ytr = torch.from_numpy(np.isin(Ktr, TRAIN_FAULTS).astype(np.float32)[:, None])
    for _ in range(400):
        opt.zero_grad()
        loss = nn.functional.binary_cross_entropy_with_logits(lr_w(ftr), ytr)
        loss.backward()
        opt.step()
    with torch.no_grad():
        f3v = lr_w(torch.from_numpy(np.log1p(qv).astype(np.float32))).numpy()[:, 0]
        f3t = lr_w(torch.from_numpy(np.log1p(qt).astype(np.float32))).numpy()[:, 0]
    res["methods"]["N3_logreg_over_rules"] = evaluate("lr", f3v, f3t, Kte)
    res["N3_weights"] = dict(zip(QUALITY_NAMES, [round(float(w), 3) for w in lr_w.weight[0]]))

    # N1: autoencoder on benign + gap windows only
    keep = np.isin(Ktr, ["BENIGN"] + st.GAPS)
    xa = xtr[keep]
    tgt = xa[:, :st.F, 0, R0:].reshape(len(xa), -1)
    mtr = recent_masks(xa).reshape(len(xa), -1).astype(np.float32)
    ae = AE()

    def ae_loss(p, y, xb):
        m = torch.from_numpy(recent_masks(xb.numpy()).reshape(len(xb), -1).astype(np.float32))
        return (((p - y) ** 2) * m).sum() / m.sum().clamp(min=1)
    t1 = time.time()
    res["ae_train_loss"] = round(train(ae, xa, tgt.astype(np.float32), ae_loss, 40), 4)
    res["ae_train_s"] = round(time.time() - t1, 1)
    ae.eval()
    with torch.no_grad():
        pv = ae(torch.from_numpy(xva)).numpy()
        pt = ae(torch.from_numpy(xte)).numpy()
    av, _ = ae_score(pv, xva)
    at, per_f = ae_score(pt, xte)
    res["methods"]["N1_tcn_ae"] = evaluate("ae", av, at, Kte)

    # N2: supervised on five fault kinds (GAP labelled 0)
    sup = Sup()
    ysup = np.isin(Ktr, TRAIN_FAULTS).astype(np.float32)[:, None]
    bce = nn.BCEWithLogitsLoss()
    t1 = time.time()
    res["sup_train_loss"] = round(train(sup, xtr, ysup, lambda p, y, xb: bce(p, y), 25), 4)
    res["sup_train_s"] = round(time.time() - t1, 1)
    sup.eval()
    with torch.no_grad():
        sv2 = sup(torch.from_numpy(xva)).numpy()[:, 0]
        st2 = sup(torch.from_numpy(xte)).numpy()[:, 0]
    res["methods"]["N2_tcn_supervised"] = evaluate("sup", sv2, st2, Kte)

    # explanation agreement: does the AE's top feature match B1's top feature on faults?
    fault = np.isin(Kte, TRAIN_FAULTS + UNSEEN_FAULTS)
    res["top_feature_agreement_ae_vs_b1_on_faults"] = round(
        float((per_f[fault].argmax(1) == terms_te[fault].argmax(1)).mean()), 3)
    res["params"] = {"ae": sum(p.numel() for p in ae.parameters()), "sup": sum(p.numel() for p in sup.parameters())}

    # exports for the nncase step (batch 1, fixed shapes, opset 13)
    dummy = torch.from_numpy(xte[:1])
    for name, m in (("ae", ae), ("sup", sup)):
        torch.onnx.export(m, dummy, str(out / f"{name}.onnx"), opset_version=13, input_names=["x"],
                          output_names=["y"], do_constant_folding=True)
    rs = np.random.default_rng(7)
    np.save(out / "calib_x.npy", xtr[rs.choice(len(xtr), 64, replace=False)])
    np.save(out / "test_x.npy", xte)
    np.save(out / "val_x.npy", xva)
    np.savez(out / "test_meta.npz", kinds=Kte, sites=Ste, float_ae_val=av, float_ae_test=at,
             float_sup_val=sv2, float_sup_test=st2)
    res["total_s"] = round(time.time() - t0, 1)
    (out / "window_results.json").write_text(json.dumps(res, indent=1))
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
