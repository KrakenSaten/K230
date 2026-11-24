#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Synthetic 120-second telemetry windows shaped like what pos-capture already
computes per one-second bin (ppc_stats bins, the M11.1 DNS/TCP diagnosis, the
M12.1 ARP/DHCP events), with coverage masks for HOLD/shed/drops.

THIS IS A GENERATOR, NOT DATA. It exists to exercise model shapes, masks,
quantization and the evaluation protocol, and to check that a method does not
alarm on a visibility gap. Any accuracy number measured on it says how well a
method fits this generator, not how it behaves on a real network.

Feature channels (per bin), all log-scaled:
  0 pkts_in  1 pkts_out  2 kB_in  3 kB_out  4 bcast_mcast
  5 dns_q    6 dns_ans   7 dns_rtt_ms (median of the bin's pairs; NaN if none)
  8 dns_fail 9 dns_unans 10 tcp_syn  11 hs_rtt_ms (median; NaN if none)
  12 tcp_rst 13 retrans  14 arp_evt  15 dhcp_msg
Plus coverage[t] in {0,1}: 0 = frames passed unseen (HOLD, shed, kernel drops).

Labels: kind in BENIGN | FAULT_* | GAP (benign traffic + a visibility gap in
the recent part). The "recent" part is the last 30 bins; onset of a fault is
inside it.
"""
import numpy as np

T, F, RECENT = 120, 16, 30
NAMES = ["pkts_in", "pkts_out", "kB_in", "kB_out", "bcast", "dns_q", "dns_ans", "dns_rtt", "dns_fail",
         "dns_unans", "tcp_syn", "hs_rtt", "tcp_rst", "retrans", "arp_evt", "dhcp_msg"]
FAULTS = ["dns_slow", "dns_down", "dns_servfail", "path_loss", "upstream_latency", "arp_storm", "dhcp_trouble",
          "subtle_multi"]
BENIGN_EVENTS = ["none", "download", "idle", "surge", "update_burst", "video"]
GAPS = ["GAP_HOLD", "GAP_DROP", "GAP_ONEWAY"]


def site_params(r):
    return dict(
        act=float(np.exp(r.normal(np.log(8), 0.7))),  # new connections per minute at activity 1
        pkt_per_conn=float(np.exp(r.normal(np.log(40), 0.5))),
        dns_mu=float(np.log(r.uniform(8, 70))), dns_sig=float(r.uniform(0.25, 0.8)),
        hs_mu=float(np.log(r.uniform(6, 90))), hs_sig=float(r.uniform(0.2, 0.6)),
        bcast=float(np.exp(r.normal(np.log(2.5), 0.8))),
        ssdp_period=float(r.uniform(20, 90)), ssdp_burst=float(r.uniform(2, 12)),
        retrans_frac=float(np.exp(r.normal(np.log(0.004), 0.7))),
        rst_rate=float(np.exp(r.normal(np.log(0.03), 0.8))),
        fail_frac=float(np.exp(r.normal(np.log(0.005), 1.0))),
        unans_frac=float(np.exp(r.normal(np.log(0.004), 1.0))),
        arp_rate=float(np.exp(r.normal(np.log(0.15), 0.7))),
        dhcp_rate=float(np.exp(r.normal(np.log(0.004), 0.8))),
        qpc=float(r.uniform(0.6, 2.5)),  # DNS queries per new connection
    )


def window(p, r, kind="BENIGN", benign_event=None):
    """One 120 s window -> (x[F,T] float32 with NaN where undefined, cov[T])."""
    t = np.arange(T)
    # user activity: a slowly varying intensity with on/off bursts
    act = np.exp(np.cumsum(r.normal(0, 0.08, T)))
    act *= (r.random(T) < 0.85) * 1.0 + 0.15
    act *= r.uniform(0.3, 2.0)
    onset = int(r.integers(T - RECENT, T - 5))
    ramp = np.clip((t - onset) / r.uniform(1, 6), 0, 1)
    ev = benign_event or "none"
    if kind == "BENIGN" and ev == "none":
        ev = BENIGN_EVENTS[int(r.integers(0, len(BENIGN_EVENTS)))]
    bytes_boost = np.ones(T)
    if kind == "BENIGN" or kind in GAPS:
        if ev == "download":
            bytes_boost += ramp * r.uniform(5, 40) * (t < onset + r.integers(10, 60))
        elif ev == "idle":
            act *= 1 - ramp * r.uniform(0.7, 0.98)
        elif ev == "surge":
            act *= 1 + ramp * r.uniform(1, 4)
        elif ev == "update_burst":
            b = (t >= onset) & (t < onset + r.integers(3, 12))
            act = act + b * r.uniform(2, 6)
        elif ev == "video":
            bytes_boost += ramp * r.uniform(3, 15) * (np.sin(t / r.uniform(2, 5)) > 0)
    conns = r.poisson(p["act"] / 60 * act * 6)  # per second
    dns_rtt_mult = np.ones(T)
    hs_mult = np.ones(T)
    retrans_mult = np.ones(T)
    fail_frac = np.full(T, p["fail_frac"])
    unans_frac = np.full(T, p["unans_frac"])
    arp_rate = np.full(T, p["arp_rate"])
    dhcp_rate = np.full(T, p["dhcp_rate"])
    q_mult = np.ones(T)
    conn_mult = np.ones(T)
    if kind == "dns_slow":
        dns_rtt_mult += ramp * r.uniform(3, 20)
        q_mult += ramp * r.uniform(0.2, 1.0)  # retries
    elif kind == "dns_down":
        unans_frac = np.maximum(unans_frac, ramp * r.uniform(0.7, 1.0))
        q_mult += ramp * r.uniform(0.5, 2.0)
        conn_mult -= ramp * r.uniform(0.3, 0.8)
    elif kind == "dns_servfail":
        fail_frac = np.maximum(fail_frac, ramp * r.uniform(0.3, 0.9))
    elif kind == "path_loss":
        retrans_mult += ramp * r.uniform(5, 40)
        hs_mult += ramp * r.uniform(0.5, 3)
    elif kind == "upstream_latency":
        hs_mult += ramp * r.uniform(2, 10)
        dns_rtt_mult += ramp * r.uniform(1.5, 6)
    elif kind == "arp_storm":
        arp_rate = arp_rate + ramp * r.uniform(5, 40)
    elif kind == "dhcp_trouble":
        dhcp_rate = dhcp_rate + ramp * r.uniform(0.3, 2)
        conn_mult -= ramp * r.uniform(0.3, 0.9)
    elif kind == "subtle_multi":  # each part alone is within normal variation
        dns_rtt_mult += ramp * r.uniform(0.5, 1.0)
        hs_mult += ramp * r.uniform(0.4, 0.9)
        retrans_mult += ramp * r.uniform(1.0, 2.5)
    conns = r.poisson(np.maximum(conns * conn_mult, 0))
    syn = conns + r.poisson(0.05, T)
    q = r.poisson(syn * p["qpc"] * q_mult + 0.02)
    unans = r.binomial(q, np.clip(unans_frac, 0, 1))
    fail = r.binomial(q - unans, np.clip(fail_frac, 0, 1))
    ans = q - unans
    pk = r.poisson(np.maximum(syn * p["pkt_per_conn"] * (0.5 + r.random(T)), 0) + 0.3)
    pin = r.binomial(pk, 0.62)
    pout = pk - pin
    kin = pin * r.uniform(0.6, 1.4, T) * bytes_boost * 1.1
    kout = pout * r.uniform(0.08, 0.2, T)
    ssdp = ((t % p["ssdp_period"]) < 1) * p["ssdp_burst"]
    bc = r.poisson(p["bcast"] + ssdp)
    retrans = r.binomial(pout + pin, np.clip(p["retrans_frac"] * retrans_mult, 0, 0.5))
    rst = r.poisson(p["rst_rate"] * (1 + syn * 0.2))
    arp = r.poisson(arp_rate)
    dhcp = r.poisson(dhcp_rate)
    dns_rtt = np.full(T, np.nan)
    hs = np.full(T, np.nan)
    for i in range(T):
        if ans[i] > 0:
            dns_rtt[i] = np.median(np.exp(r.normal(p["dns_mu"], p["dns_sig"], ans[i]))) * dns_rtt_mult[i]
        if syn[i] > 0 and r.random() < 0.9:
            hs[i] = np.median(np.exp(r.normal(p["hs_mu"], p["hs_sig"], max(1, syn[i])))) * hs_mult[i]
    cov = np.ones(T)
    if kind in GAPS:
        g0 = int(r.integers(T - RECENT, T - 5))
        g1 = min(T, g0 + int(r.integers(8, 30)))
        sl = slice(g0, g1)
        if kind == "GAP_DROP":
            # capture-side loss (kernel drops / shed): a share d of frames never
            # reaches the helper. Answers lost -> their queries look unanswered;
            # handshakes lose their SYN-ACK -> untimed. The helper knows the
            # drop count per second, so coverage = 1 - d.
            d = r.uniform(0.15, 0.6)
            lost = r.binomial(ans[sl], d)
            ans[sl] -= lost
            unans[sl] += lost
            fail[sl] = r.binomial(fail[sl], 1 - d)
            for a in (pin, pout, bc, retrans, rst, arp, dhcp, syn):
                a[sl] = r.binomial(a[sl], 1 - d)
            kin[sl] *= 1 - d
            kout[sl] *= 1 - d
            for i in range(g0, g1):
                if r.random() < d:
                    hs[i] = np.nan
            cov[sl] = 1 - d
        elif kind == "GAP_ONEWAY":
            # replies arrive on another path/interface: queries and SYNs are
            # seen, answers and SYN-ACKs are not, while frames keep flowing.
            # The helper flags one-way conversations (tcp_oneway), so
            # directional coverage of these bins is 0.
            unans[sl] = q[sl]
            ans[sl] = 0
            fail[sl] = 0
            pin[sl] = r.binomial(pin[sl], 0.05)
            kin[sl] *= 0.05
            hs[sl] = np.nan
            cov[sl] = 0.0
        else:  # GAP_HOLD: HOLD / shed period, frames unseen, every counter zero
            cov[sl] = 0
            hs[sl] = np.nan
            for a in (pin, pout, bc, q, ans, fail, unans, syn, rst, retrans, arp, dhcp):
                a[sl] = 0
            kin[sl] = 0
            kout[sl] = 0
    x = np.stack([pin, pout, kin, kout, bc, q, ans, dns_rtt, fail, unans, syn, hs, rst, retrans, arp, dhcp]
                 ).astype(np.float64)
    x[7, cov < 0.5] = np.nan
    # sporadic short HOLD-like blanks anywhere, also in benign windows
    if r.random() < 0.15:
        g0 = int(r.integers(0, T - 3))
        g1 = g0 + int(r.integers(1, 3))
        cov[g0:g1] = 0
        x[:, g0:g1] = 0
        x[7, g0:g1] = np.nan
        x[11, g0:g1] = np.nan
    x = np.log1p(x)
    return x.astype(np.float32), cov.astype(np.float32)


def dataset(n_sites, per_site, seed, kinds, gap_share=0.0):
    """-> X[N,F,T] (NaN undefined), C[N,T], kind labels, site ids."""
    r = np.random.default_rng(seed)
    X, C, K, S = [], [], [], []
    for s in range(n_sites):
        p = site_params(r)
        for _ in range(per_site):
            u = r.random()
            if u < gap_share:
                k = GAPS[int(r.integers(0, len(GAPS)))]
            else:
                k = kinds[int(r.integers(0, len(kinds)))]
            x, c = window(p, r, k)
            X.append(x); C.append(c); K.append(k); S.append(s)
    return np.stack(X), np.stack(C), np.array(K), np.array(S)


def model_input(X, C, ref_end=T - RECENT):
    """Reference-normalised model input: per window and feature, subtract the
    median and divide by the MAD of the *reference* part (bins before the
    recent part, covered and defined). Undefined -> 0, plus three mask
    channels (coverage, dns_rtt defined, hs_rtt defined). -> [N, F+3, 1, T]
    This is the cheap CPU half of the engine: the adaptive baseline."""
    N = X.shape[0]
    out = np.zeros((N, F + 3, 1, T), np.float32)
    for n in range(N):
        for f in range(F):
            v = X[n, f]
            ok = ~np.isnan(v) & (C[n] > 0)
            ref = v[:ref_end][ok[:ref_end]]
            if ref.size >= 5:
                med = np.median(ref)
                mad = np.median(np.abs(ref - med)) * 1.4826
            else:
                med, mad = 0.0, 1.0
            scale = max(mad, 0.1)
            z = np.where(ok, (np.nan_to_num(v) - med) / scale, 0.0)
            out[n, f, 0] = np.clip(z, -8, 8)
        out[n, F, 0] = C[n]
        out[n, F + 1, 0] = (~np.isnan(X[n, 7])).astype(np.float32)
        out[n, F + 2, 0] = (~np.isnan(X[n, 11])).astype(np.float32)
    return out
