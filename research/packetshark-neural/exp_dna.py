#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""E3b: Traffic DNA - a 32-d embedding of a conversation's first 32 packets
(signed size, inter-arrival time, presence mask), header-derived only.

THE FLOWS ARE GENERATED (stochastic class models below), NOT CAPTURED. The
numbers say whether the representation, the model shape, quantization and
open-set rejection work mechanically; they say nothing about real traffic.

Compared: a hand-crafted 24-d feature vector (per-direction size/IAT stats,
burst counts, first signed sizes) with the same prototype/kNN decision.
Two classes are never trained on and must come out "unclassified".
Sites (RTT, MTU/overhead, server behaviour) are disjoint between train/test.

Exports dna_h1.onnx ([1,3,1,32] -> [1,1,32]) and dna_h64.onnx (64 flows as
the H axis, [1,3,64,32] -> [1,64,32]); same weights.
Usage (venv-train): exp_dna.py OUT
"""
import json
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as Fn

L = 32
CLASSES = ["web_tls", "video", "bulk_down", "rtc_voice", "gaming", "api_small", "iot_telemetry",
           "ssh_interactive", "bulk_up", "quic_web"]
UNSEEN = ["gaming", "ssh_interactive"]


def flow(cls, site, r):
    """-> list of (dir +1 client->server / -1, size bytes, iat ms)"""
    rtt = site["rtt"] * float(np.exp(r.normal(0, 0.15)))
    mtu = site["mtu"]
    hdr = site["hdr"]
    pk = []

    def add(d, size, iat):
        pk.append((d, int(np.clip(size + hdr, 40 + hdr, mtu)), max(0.01, iat)))

    def tls_hs():
        add(1, 60, 0); add(-1, 60, rtt); add(1, 52, 0.1)               # TCP handshake
        add(1, r.integers(517, 1800), 0.2)                              # ClientHello
        for i in range(int(r.integers(2, 5))):
            add(-1, mtu - hdr if i < 2 else r.integers(200, 1400), rtt if i == 0 else 0.05)
        add(1, r.integers(60, 160), 0.3)

    def acks_for(n, d):
        for _ in range(n // 2):
            add(-d, 52, 0.05)

    if cls == "web_tls":
        tls_hs()
        while len(pk) < L:
            add(1, r.integers(200, 700), float(np.exp(r.normal(np.log(30), 1.0))))
            for i in range(int(r.integers(1, 8))):
                add(-1, mtu - hdr if r.random() < .7 else r.integers(100, 1400), rtt if i == 0 else 0.05)
    elif cls == "video":
        tls_hs()
        while len(pk) < L:
            for i in range(int(r.integers(6, 14))):
                add(-1, mtu - hdr, (rtt if i == 0 else 0.03) + (r.uniform(500, 3000) if i == 0 and len(pk) > 12
                                                                 else 0))
                if i % 2:
                    add(1, 52, 0.02)
    elif cls == "bulk_down":
        tls_hs()
        while len(pk) < L:
            add(-1, mtu - hdr, r.exponential(0.15)); add(-1, mtu - hdr, 0.02); add(1, 52, 0.05)
    elif cls == "bulk_up":
        tls_hs()
        while len(pk) < L:
            add(1, mtu - hdr, r.exponential(0.15)); add(1, mtu - hdr, 0.02); add(-1, 52, rtt / 4)
    elif cls == "rtc_voice":
        f = r.uniform(18, 22)
        for _ in range(L):
            add(1 if r.random() < 0.5 else -1, r.integers(90, 220), max(0.1, r.normal(f / 2, 3)))
    elif cls == "gaming":
        for _ in range(L):
            add(1 if r.random() < 0.6 else -1, r.integers(40, 300), max(0.1, r.normal(16, 5)))
    elif cls == "api_small":
        tls_hs()
        while len(pk) < L:
            add(1, r.integers(150, 600), float(np.exp(r.normal(np.log(800), 1.2))))
            add(-1, r.integers(100, 900), rtt)
            add(1, 52, 0.1)
    elif cls == "iot_telemetry":
        add(1, 60, 0); add(-1, 60, rtt); add(1, 52, 0.1)
        per = r.uniform(5000, 60000)
        while len(pk) < L:
            add(1, r.integers(60, 300), per * r.uniform(0.95, 1.05)); add(-1, 52, rtt)
            if r.random() < 0.3:
                add(-1, r.integers(60, 200), 1); add(1, 52, rtt)
    elif cls == "ssh_interactive":
        add(1, 60, 0); add(-1, 60, rtt); add(1, 52, 0.1)
        add(-1, r.integers(20, 60), rtt); add(1, r.integers(20, 60), 1)  # banners
        while len(pk) < L:
            add(1, r.integers(36, 100), float(np.exp(r.normal(np.log(250), 0.9))))
            add(-1, r.integers(36, 140), rtt)
            add(1, 52, 0.5)
    elif cls == "quic_web":
        add(1, 1200 + r.integers(0, 52) - hdr, 0)
        add(-1, 1200 + r.integers(0, 150) - hdr, rtt); add(-1, 1200 - hdr, 0.05)
        add(1, r.integers(40, 120), 0.3)
        while len(pk) < L:
            add(1, r.integers(60, 400), float(np.exp(r.normal(np.log(40), 1.0))))
            for i in range(int(r.integers(1, 10))):
                add(-1, mtu - hdr - 28, rtt if i == 0 else 0.04)
            add(1, r.integers(40, 70), 0.2)
    return pk[:L]


def site(r):
    return {"rtt": float(np.exp(r.normal(np.log(30), 0.8))), "mtu": int(r.choice([1500, 1500, 1492, 1420, 1400])),
            "hdr": int(r.choice([0, 0, 20]))}


def tensor(pk, n):
    """first n packets -> [3, L]: signed size/1500, log1p(iat ms)/log1p(1e5), mask"""
    x = np.zeros((3, L), np.float32)
    for i, (d, s, iat) in enumerate(pk[:n]):
        x[0, i] = d * s / 1500.0
        x[1, i] = np.log1p(iat if i else 0.0) / np.log1p(1e5)
        x[2, i] = 1.0
    return x


def handcrafted(x):
    """24-d stats from the same tensor (same information, no learning)"""
    m = x[2] > 0
    s, t = x[0][m], x[1][m]
    up, dn = s[s > 0], -s[s < 0]
    f = []
    for v in (up, dn):
        f += [len(v) / L, v.mean() if len(v) else 0, v.std() if len(v) else 0, (v > 0.9).mean() if len(v) else 0,
              (v < 0.06).mean() if len(v) else 0]
    f += [t.mean(), t.std(), np.median(t), (t > 0.4).mean(), m.sum() / L]
    dirs = np.sign(s)
    f += [float((np.diff(dirs) != 0).mean()) if len(s) > 1 else 0]
    first = np.zeros(8, np.float32)
    first[:min(8, len(s))] = s[:8]
    f += list(first)
    return np.array(f, np.float32)


def make(n_sites, per, seed, classes, short_share=0.3):
    r = np.random.default_rng(seed)
    X, Y, N, S = [], [], [], []
    for si in range(n_sites):
        st = site(r)
        for _ in range(per):
            c = int(r.integers(0, len(classes)))
            n = int(r.integers(4, 16)) if r.random() < short_share else L
            X.append(tensor(flow(classes[c], st, r), n)); Y.append(CLASSES.index(classes[c])); N.append(n)
            S.append(si)
    return np.stack(X), np.array(Y), np.array(N), np.array(S)


class DNA(nn.Module):
    """input [B,3,H,32]: H independent flows; output [B,H,32] unit vectors"""
    def __init__(s, d=32):
        super().__init__()
        s.c1 = nn.Conv2d(3, 32, (1, 3), padding=(0, 1))
        s.c2 = nn.Conv2d(32, 64, (1, 3), stride=(1, 2), padding=(0, 1))
        s.c3 = nn.Conv2d(64, 64, (1, 3), stride=(1, 2), padding=(0, 1))
        s.fc = nn.Conv2d(64, d, (1, 1))

    def forward(s, x, normalize=True):
        h = Fn.relu(s.c3(Fn.relu(s.c2(Fn.relu(s.c1(x))))))
        # pool over packets -> [B,64,H,1]; AveragePool stays on the KPU,
        # ReduceMean does not (E1), and the two are the same arithmetic
        h = Fn.avg_pool2d(h, (1, h.shape[3]))
        e = s.fc(h)[..., 0].transpose(1, 2)      # [B,H,d]
        if not normalize:                        # dna-b64: L2 norm on the host (ReduceL2/Div are CPU ops)
            return e
        return e / (e.norm(dim=2, keepdim=True) + 1e-6)


class Raw(nn.Module):
    """dna-b64 export: the encoder without its normalisation."""
    def __init__(s, m):
        super().__init__()
        s.m = m

    def forward(s, x):
        m = s.m
        h = Fn.relu(m.c3(Fn.relu(m.c2(Fn.relu(m.c1(x))))))
        h = Fn.avg_pool2d(h, (1, h.shape[3]))
        return m.fc(h)  # [1, 32, 64, 1]: transpose and L2 normalisation happen on the host


def auc(pos, neg):
    s = np.concatenate([pos, neg])
    rk = s.argsort().argsort().astype(np.float64) + 1
    return float((rk[:len(pos)].sum() - len(pos) * (len(pos) + 1) / 2) / (len(pos) * len(neg)))


def proto_eval(emb_tr, ytr, emb_te, yte, seen_ids, nte):
    protos = np.stack([emb_tr[ytr == c].mean(0) for c in seen_ids])
    protos /= np.linalg.norm(protos, axis=1, keepdims=True) + 1e-9
    e = emb_te / (np.linalg.norm(emb_te, axis=1, keepdims=True) + 1e-9)
    sim = e @ protos.T
    pred = np.array(seen_ids)[sim.argmax(1)]
    best = sim.max(1)
    known = np.isin(yte, seen_ids)
    # kNN (k=5) on training embeddings
    etr = emb_tr / (np.linalg.norm(emb_tr, axis=1, keepdims=True) + 1e-9)
    nn_idx = np.argsort(-(e @ etr.T), axis=1)[:, :5]
    knn = np.array([np.bincount(ytr[i], minlength=len(CLASSES)).argmax() for i in nn_idx])
    thr = float(np.quantile(best[known], 0.05))  # accept 95 % of known flows
    res = {"proto_acc_known": round(float((pred[known] == yte[known]).mean()), 4),
           "knn5_acc_known": round(float((knn[known] == yte[known]).mean()), 4),
           "proto_acc_known_short_lt16": round(float((pred[known & (nte < 16)] == yte[known & (nte < 16)]).mean()), 4),
           "proto_acc_known_full32": round(float((pred[known & (nte == L)] == yte[known & (nte == L)]).mean()), 4),
           "open_set_auroc_unknown_vs_known": round(auc(-best[~known], -best[known]), 4),
           "unknown_rejected_at_95pct_known_accept": round(float((best[~known] < thr).mean()), 4)}
    return res, pred, best


def main(out):
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    seen = [c for c in CLASSES if c not in UNSEEN]
    seen_ids = [CLASSES.index(c) for c in seen]
    Xtr, ytr, ntr, _ = make(60, 120, 11, seen)
    Xte, yte, nte, ste = make(20, 120, 12, CLASSES)
    res = {"n_train": len(ytr), "n_test": len(yte), "unseen": UNSEEN, "gen_s": round(time.time() - t0, 1)}

    # baseline: hand-crafted features, standardised on train
    Htr = np.stack([handcrafted(x) for x in Xtr]); Hte = np.stack([handcrafted(x) for x in Xte])
    mu, sd = Htr.mean(0), Htr.std(0) + 1e-6
    res["baseline_handcrafted_24d"], _, _ = proto_eval((Htr - mu) / sd, ytr, (Hte - mu) / sd, yte, seen_ids, nte)

    # model: cosine-softmax classifier over seen classes, embedding = normalised penultimate
    torch.manual_seed(0)
    m = DNA()
    head = nn.Parameter(torch.randn(len(seen_ids), 32) * 0.1)
    opt = torch.optim.Adam(list(m.parameters()) + [head], lr=2e-3)
    remap = {c: i for i, c in enumerate(seen_ids)}
    Xt = torch.from_numpy(Xtr[:, :, None, :])
    Yt = torch.tensor([remap[c] for c in ytr])
    t1 = time.time()
    for ep in range(30):
        perm = torch.randperm(len(Yt))
        for i in range(0, len(Yt), 128):
            idx = perm[i:i + 128]
            e = m(Xt[idx])[:, 0]
            w = head / head.norm(dim=1, keepdim=True)
            loss = Fn.cross_entropy(16.0 * e @ w.t(), Yt[idx])
            opt.zero_grad(); loss.backward(); opt.step()
    res["train_s"] = round(time.time() - t1, 1)
    m.eval()
    with torch.no_grad():
        etr = m(Xt)[:, 0].numpy()
        ete = m(torch.from_numpy(Xte[:, :, None, :]))[:, 0].numpy()
    res["dna_cnn_32d"], _, _ = proto_eval(etr, ytr, ete, yte, seen_ids, nte)
    res["params"] = sum(p.numel() for p in m.parameters())

    with torch.no_grad():
        torch.onnx.export(m, torch.from_numpy(Xte[:1, :, None, :]), str(out / "dna_h1.onnx"), opset_version=13,
                          input_names=["x"], output_names=["y"])
        xb = torch.from_numpy(np.ascontiguousarray(Xte[:64].transpose(1, 0, 2)[None]))  # [1,3,64,32]
        torch.onnx.export(m, xb, str(out / "dna_h64.onnx"), opset_version=13, input_names=["x"],
                          output_names=["y"])
        e64 = m(xb)[0].numpy()
        torch.onnx.export(Raw(m), xb, str(out / "dna_b64.onnx"), opset_version=13, input_names=["x"],
                          output_names=["y"])
    res["h64_matches_h1_max_abs"] = float(np.abs(e64 - ete[:64]).max())
    # weights for the C reference in infer-bench (poc/README.md): c1, c2, c3, fc; weight then bias
    np.concatenate([t.detach().numpy().ravel() for t in (m.c1.weight, m.c1.bias, m.c2.weight, m.c2.bias,
                                                         m.c3.weight, m.c3.bias, m.fc.weight, m.fc.bias)]
                   ).astype("<f4").tofile(out / "dna_b64.weights.f32")
    rs = np.random.default_rng(5)
    np.save(out / "dna_calib_h1.npy", Xtr[rs.choice(len(Xtr), 64, replace=False)][:, :, None, :])
    np.save(out / "dna_test_h1.npy", Xte[:, :, None, :])
    cal64 = Xtr[rs.choice(len(Xtr), 64 * 8, replace=False)].reshape(8, 64, 3, L).transpose(0, 2, 1, 3)
    np.save(out / "dna_calib_h64.npy", np.ascontiguousarray(cal64))
    te64 = Xte[:len(Xte) // 64 * 64].reshape(-1, 64, 3, L).transpose(0, 2, 1, 3)
    np.save(out / "dna_test_h64.npy", np.ascontiguousarray(te64))
    # raw files for the bench: inputs as float32 [B,1,3,64,32] and float reference outputs [B,1,64,32]
    np.ascontiguousarray(te64[:32]).astype("<f4").tofile(out / "dna_b64.test_in.f32")
    with torch.no_grad():
        ref = np.stack([Raw(m)(torch.from_numpy(te64[i:i + 1])).numpy() for i in range(32)])
    ref.astype("<f4").tofile(out / "dna_b64.float_out.f32")
    np.savez(out / "dna_meta.npz", etr=etr, ytr=ytr, ete=ete, yte=yte, nte=nte, ste=ste,
             seen_ids=np.array(seen_ids))
    res["total_s"] = round(time.time() - t0, 1)
    (out / "dna_results.json").write_text(json.dumps(res, indent=1))
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
