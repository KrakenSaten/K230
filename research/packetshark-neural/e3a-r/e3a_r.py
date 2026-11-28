#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""E3a-R: E3a redone with the corrected methodology adopted on 27 November 2026
(docs/apps/packetshark/neural/INDEPENDENT-REVIEW.md F7, §8; HANDOVER §5 step 4).

THIS IS A GENERATOR, NOT DATA. It measures how decision rules behave on traffic
this script invents. Nothing here is accuracy on a real network.

What changed from E3a (research branch, synth_telemetry.py / exp_window.py):

  * The coverage signal is the helper's own, noisy and lagged: a cumulative
    lost-frame counter read every 250 ms, whose readings fail in spells. A
    reading reports what was lost since the last good one, not when; a spell
    not closed by a good reading leaves the loss unknown. No method is handed
    the true per-second loss (except the "oracle gate", kept only to show how
    E3a's figure was constructed).
  * Loss ranges from 0.1 % to 60 % (log-uniform), not 15-60 %.
  * One-way visibility is inferred from packet counts (this device's frames
    in and out), never given.
  * Faults and visibility artifacts are drawn independently, so a quarter of
    the windows hold both: faults overlap gaps, HOLD and loss.
  * Ten seeds; every figure is reported as mean, standard deviation and range
    over the seeds.
  * Deterministic methods only. The owner's brief for N1.1 forbids model
    training, so E3a's learned rows (N1-N3) are not repeated.
  * The rules are the N1.1 engine's (core/pocketpcap/ppc_intel.c), mirrored
    here line by line: the same windows (60 s against 60 s), the same tests
    (Mann-Whitney for answer time, binomial for answered and failed shares,
    Poisson for DHCP and ARP rates), the same minimum counts, alpha 0.01, the
    same 1.5x / 20 ms practical bound, the same material changes "no change"
    must rule out, the same one-way rules. The "gate" methods get the
    window-level one-way rule (nothing came back), not the silent-spell rule,
    which is part of the N1.1 design.

Methods, on the same simulated windows:
  naive        the rules with no visibility handling: lost and held seconds read
               as evidence, a question with no answer seen in 5 s is unanswered
  gate         E3a's binary gate on the estimated coverage: seconds whose loss,
               as the lagged readings place it, exceeds 2 %, and HOLD seconds,
               are dropped; the rest is judged as if nothing were lost
  oracle-gate  the same gate on the TRUE per-second loss (E3a's construction)
  loss-aware   the N1.1 engine: loss bounds [lo, hi] from the readings, each test
               at both ends, abstain (CAN'T TELL) when only the end that favours
               a change rejects; questions whose wait was not watched left out

Measured, per seed: false claims (WORSE on a metric no fault touched), split by
the visibility artifact in the window; recall and missed faults, split into
faults alone, faults overlapping an artifact and faults overlapping loss; how
a miss happened (abstained, or said "no change": false reassurance).

Usage (WSL, numpy only): python3 e3a_r.py OUT_DIR [--seeds 10] [--sites 40] [--windows 60]
"""
import json
import math
import sys
import time
from pathlib import Path

import os

import numpy as np

T_BASE, T_WIN, WAIT = 60, 60, 5
T = T_BASE + T_WIN + WAIT  # seconds simulated: the base, the window, the last question's wait
SLOTS = 4                  # readings a second
ALPHA_E6 = 10000
Z = 1.96
MIN_PAIRS = MIN_Q = MIN_ANS = 5
MIN_W = 10
SLOWER_PCT, SLOWER_US = 150, 20000
ONE_WAY_MIN_OUT = 8
ONE_WAY_LOG_P = 6.907755
GATE_COV = 0.98
OLD_TIME = os.environ.get("E3AR_OLD_TIME") == "1"    # the answer-time rule before the selective-loss correction
OLD_SPELL = os.environ.get("E3AR_OLD_SPELL") == "1"  # the silent-spell rate over both windows, as first written

METRICS = ["dns.time", "dns.answered", "dns.failed", "dhcp.discover", "arp.request"]
FAULTS = {"dns_down": "dns.answered", "dns_slow": "dns.time", "servfail": "dns.failed",
          "arp_storm": "arp.request", "dhcp_trouble": "dhcp.discover", "subtle_latency": "dns.time"}
# Metrics a fault moves as a true consequence, never counted as false claims:
# a resolver slower than the 5 s wait leaves questions unanswered within it.
TOUCHES = {"dns_down": {"dns.answered", "dns.time"}, "dns_slow": {"dns.time", "dns.answered"},
           "servfail": {"dns.failed"}, "arp_storm": {"arp.request"}, "dhcp_trouble": {"dhcp.discover"},
           "subtle_latency": {"dns.time", "dns.answered"}}
ARTIFACTS = ["loss", "hold", "oneway"]
METHODS = ["naive", "gate", "oracle-gate", "loss-aware"]
NO_CHANGE, WORSE, BETTER, CANT, SMALL = "no-change", "worse", "better", "cant-tell", "small"
MATERIAL_SHARE, MATERIAL_FAIL, MATERIAL_RATE, MATERIAL_RATE_MIN = 0.50, 0.25, 3.0, 6.0

LG = np.array([math.lgamma(i + 1.0) for i in range(300001)])


# ---- statistics: as ppc_intel.c ----------------------------------------------------------

def to_e6(p):
    if not p > 0.0:
        return 0
    if p >= 1.0:
        return 1000000
    return int(p * 1e6 + 0.5)


def binom_le(k, n, p):
    if k >= n or p <= 0.0:
        return 1000000
    if p >= 1.0:
        return 0
    i = np.arange(0, k + 1)
    return to_e6(min(1.0, float(np.exp(LG[n] - LG[i] - LG[n - i] + i * math.log(p) + (n - i) * math.log1p(-p)).sum())))


def binom_ge(k, n, p):
    if k == 0 or p >= 1.0:
        return 1000000
    if p <= 0.0 or k > n:
        return 0
    i = np.arange(k, n + 1)
    return to_e6(min(1.0, float(np.exp(LG[n] - LG[i] - LG[n - i] + i * math.log(p) + (n - i) * math.log1p(-p)).sum())))


def pois_sum(i0, i1, mu):
    sd = math.sqrt(mu)
    lo, hi = mu - 40.0 * sd - 40.0, mu + 40.0 * sd + 40.0
    if lo > i0:
        i0 = int(lo)
    if hi < i1:
        i1 = int(hi)
    if i0 > i1:
        return 0.0
    i = np.arange(i0, i1 + 1)
    return min(1.0, float(np.exp(-mu + i * math.log(mu) - LG[i]).sum()))


def pois_ge(c, mu):
    if c == 0:
        return 1000000
    if not mu > 0.0:
        return 0
    if math.isinf(mu):
        return 1000000
    return to_e6(pois_sum(c, 0xffffffff, mu))


def pois_le(c, mu):
    if not mu > 0.0 or math.isinf(mu):
        return 0 if mu > 0.0 else 1000000
    return to_e6(pois_sum(0, c, mu))


def wilson(k, n):
    ph = k / n
    d = 1.0 + Z * Z / n
    c = ph + Z * Z / (2.0 * n)
    w = Z * math.sqrt(ph * (1.0 - ph) / n + Z * Z / (4.0 * n * n))
    lo, hi = (c - w) / d, (c + w) / d
    if lo < 0.0 or k == 0:
        lo = 0.0
    if hi > 1.0 or k >= n:
        hi = 1.0
    return lo, hi


def median_us(v):
    v = sorted(v)
    n = len(v)
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) // 2


def mann_whitney(a, b):
    a, b = sorted(a), sorted(b)
    na, nb = len(a), len(b)
    n = na + nb
    i = j = 0
    pos = 1
    rank_b = ties = 0.0
    while i < na or j < nb:
        v = a[i] if (j >= nb or (i < na and a[i] <= b[j])) else b[j]
        ca = cb = 0
        while i < na and a[i] == v:
            i += 1
            ca += 1
        while j < nb and b[j] == v:
            j += 1
            cb += 1
        r = pos + (ca + cb - 1) / 2.0
        rank_b += r * cb
        ties += float(ca + cb) ** 3 - (ca + cb)
        pos += ca + cb
    u = rank_b - nb * (nb + 1.0) / 2.0
    mean = na * nb / 2.0
    var = na * nb / 12.0 * ((n + 1.0) - ties / (n * (n - 1.0)))
    if not var > 0.0:
        return 1000000, 1000000
    z = (u - mean - 0.5) / math.sqrt(var)
    gt = to_e6(0.5 * math.erfc(z / math.sqrt(2.0)))
    z = (u - mean + 0.5) / math.sqrt(var)
    lt = to_e6(0.5 * math.erfc(-z / math.sqrt(2.0)))
    return gt, lt


def scaled(x0, f0, f1):
    return math.inf if f0 >= 1.0 else x0 * (1.0 - f1) / (1.0 - f0)


def cap1(p):
    return 1.0 if p > 1.0 else p


def settle(cw, cb, aw, ab, known, rule_out):
    if cw < ALPHA_E6:
        return WORSE, "-"
    if cb < ALPHA_E6:
        return BETTER, "-"
    if aw < ALPHA_E6 or ab < ALPHA_E6:
        return CANT, "loss-explains" if known else "loss-unknown"
    if rule_out >= ALPHA_E6:
        return CANT, "weak"
    return NO_CHANGE, "-"


def decide_answered(k0, n0, k1, n1, f, known):
    lo0, hi0 = wilson(k0, n0)
    t_cw = cap1(scaled(lo0, f[0][0], 0.0))
    t_aw = cap1(scaled(lo0, f[0][1], 0.0))
    t_cb = cap1(scaled(hi0, f[0][1], 0.0))
    t_ab = cap1(scaled(hi0, f[0][0], 0.0))
    alt = t_ab * MATERIAL_SHARE * (1.0 - f[1][0])
    return settle(binom_le(k1, n1, t_cw * (1.0 - f[1][1])),
                  binom_ge(k1, n1, t_cb * (1.0 - f[1][0])) if t_cb < 1.0 else 1000000,
                  binom_le(k1, n1, t_aw * (1.0 - f[1][0])),
                  binom_ge(k1, n1, t_ab * (1.0 - f[1][1])) if t_ab < 1.0 else 1000000, known,
                  binom_ge(k1, n1, alt))


def decide_failed(k0, n0, k1, n1):
    lo0, hi0 = wilson(k0, n0)
    alt = cap1(hi0 + MATERIAL_FAIL)
    return settle(binom_ge(k1, n1, hi0), binom_le(k1, n1, lo0), binom_ge(k1, n1, hi0), binom_le(k1, n1, lo0), True,
                  1000000 if alt >= 1.0 else binom_le(k1, n1, alt))


def decide_rate(c0, w0, c1, w1, f, known):
    up = (c0 + Z * math.sqrt(c0 + 1.0) + 1.0) / w0 * w1
    dn = c0 - Z * math.sqrt(c0 + 1.0)
    dn = dn / w0 * w1 if dn > 0.0 else 0.0
    r0 = c0 / w0
    alt = max(r0 * MATERIAL_RATE, r0 + MATERIAL_RATE_MIN / 60.0)
    return settle(pois_ge(c1, scaled(up, f[0][1], f[1][0])),
                  pois_le(c1, scaled(dn, f[0][0], f[1][1])) if dn > 0.0 else 1000000,
                  0 if f[1][1] >= 1.0 else pois_ge(c1, scaled(up, f[0][0], f[1][1])),
                  pois_le(c1, scaled(dn, f[0][1], f[1][0])) if dn > 0.0 else 1000000, known,
                  pois_le(c1, scaled(alt * w1, f[0][0], f[1][1])))


def slower(p, m0, m1):
    return p < ALPHA_E6 and m1 * 100 >= m0 * SLOWER_PCT and m1 >= m0 + SLOWER_US


def decide_time(r0, r1, miss0=0, miss1=0):
    """As ppc_intel.c decide_time(): the answers not seen (a decided question
    with no answer: lost, or slower than the wait) count as the slowest, so
    a verdict survives selective loss. E3AR_OLD_TIME=1: the rule before that
    correction, for comparison on the same windows."""
    m0, m1 = median_us(r0), median_us(r1)
    gt, lt = mann_whitney(r0, r1)
    bound = max(m0 * SLOWER_PCT // 100, m0 + SLOWER_US)
    above = sum(1 for v in r1 if v > bound)
    if OLD_TIME:
        miss0 = miss1 = 0
    a0 = sorted(r0) + [0xffffffff] * miss0
    a1 = sorted(r1) + [0xffffffff] * miss1
    cm0, cm1 = median_us(a0), median_us(a1)
    cgt, _ = mann_whitney(a0, r1)
    _, clt = mann_whitney(r0, a1)
    if slower(cgt, cm0, m1):
        return WORSE, "-"
    if cm1 != 0xffffffff and slower(clt, cm1, m0):
        return BETTER, "-"
    if slower(gt, m0, m1) or slower(lt, m1, m0):
        return CANT, "unseen"
    if cgt < ALPHA_E6 or clt < ALPHA_E6:
        return SMALL, "-"
    if binom_le(above + miss1, len(r1) + miss1, 0.5) >= ALPHA_E6:
        return CANT, "weak"
    return NO_CHANGE, "-"


# ---- the generator ------------------------------------------------------------------------

def loguni(r, a, b):
    return float(math.exp(r.uniform(math.log(a), math.log(b))))


def site_params(r):
    return dict(
        q=loguni(r, 0.03, 2.0),            # this device's DNS questions a second: 2 to 120 a minute
        p_ans=float(r.uniform(0.995, 1.0)),
        rtt_mu=math.log(r.uniform(8, 80)), rtt_sig=float(r.uniform(0.2, 0.6)),  # ms
        fail=float(math.exp(r.normal(math.log(0.005), 1.0))),
        arp=loguni(r, 0.1, 4.0),           # Unit A saw about 1.5-2.7 ARP requests a second (HANDOVER §4)
        dhcp=loguni(r, 0.001, 0.05),
        kin=loguni(r, 0.2, 20.0), kout=loguni(r, 0.2, 20.0),
        bc=loguni(r, 1.0, 30.0),
    )


def simulate(r, p, fault, art_win, art_base):
    """One base minute, one window minute and five seconds after it. Returns
    the per-second and per-question record every method reads."""
    t = np.arange(T)
    q = np.full(T, p["q"])
    arp = np.full(T, p["arp"])
    dhcp = np.full(T, p["dhcp"])
    rtt_mult = np.ones(T)
    p_ans = np.full(T, p["p_ans"])
    fail = np.full(T, min(p["fail"], 0.2))
    # ordinary changes in the window, which no rule should call a fault
    if r.random() < 0.5:
        q[int(r.integers(60, 100)):] *= r.uniform(0.3, 3.0)
    rose = set()  # metrics whose true rate rose, in the harmful direction, without a fault
    if r.random() < 0.3:
        k = r.uniform(0.6, 1.6)
        arp[60:] *= k
        if k > 1.0:
            rose.add("arp.request")
    if r.random() < 0.3:
        k = r.uniform(0.8, 1.25)
        rtt_mult[60:] *= k
        if k > 1.0:
            rose.add("dns.time")
    if fault:
        on = int(r.integers(60, 100))
        if fault == "dns_down":
            p_ans[on:] = r.uniform(0.0, 0.3)
        elif fault == "dns_slow":
            rtt_mult[on:] *= r.uniform(3, 20)
        elif fault == "servfail":
            fail[on:] = r.uniform(0.3, 0.9)
        elif fault == "arp_storm":
            arp[on:] *= r.uniform(5, 40)
        elif fault == "dhcp_trouble":
            dhcp[on:] += r.uniform(0.3, 2.0)
        elif fault == "subtle_latency":
            rtt_mult[on:] *= r.uniform(1.3, 1.8)
    f = np.zeros(T)
    watched = np.ones(T, bool)
    silent = np.zeros(T, bool)  # one-way: nothing reaches this device; an artifact stays in its own minute
    selective = np.zeros(T, bool)  # selective loss: burst seconds, busy and slow, frames dropped
    bc_rate = np.full(T, p["bc"])
    for kind, lo, hi in ((art_base, 0, T_BASE), (art_win, T_BASE, T)):
        if not kind:
            continue
        a = int(r.integers(lo, hi - 5))
        if kind == "loss":
            ln = int(r.integers(8, 61))
            f[a:min(a + ln, T)] = loguni(r, 0.001, 0.6)
            if r.random() < 0.5:
                # Selective (time-correlated) loss: the path is busy in bursts.
                # In a burst second traffic triples, the kernel drops frames of
                # every kind at a high rate, and answers are slow. Answers that
                # are slow are therefore lost more often than the window's loss
                # share says - by time, not by kind, as a ring overflow drops.
                fb = r.uniform(0.3, 0.8)
                k = r.uniform(2.0, 5.0)
                for s_ in range(a, min(a + ln, T)):
                    if r.random() < 0.3:
                        selective[s_] = True
                        f[s_] = fb
                        bc_rate[s_] *= 3.0
                        rtt_mult[s_] *= k
                if selective[max(a, T_BASE):min(a + ln, T)].any():
                    rose.add("dns.time")  # a slowdown in the window: a claim of it is never false
        elif kind == "hold":
            ln = int(r.integers(5, 31))
            watched[a:min(a + ln, T)] = False
        else:
            silent[a:hi] = True
    # frames
    lost_slot = np.zeros(T * SLOTS, np.int64)
    frames = np.zeros(T, np.int64)
    own_in = np.zeros(T, np.int64)
    own_in_dns = np.zeros(T, np.int64)
    own_out = np.zeros(T, np.int64)
    arp_seen = np.zeros(T, np.int64)
    dhcp_seen = np.zeros(T, np.int64)

    def arrive(n, s):
        """n frames in second s: how many were seen; the lost ones go to their slot."""
        if n == 0:
            return 0
        lost = int(r.binomial(n, f[s])) if f[s] > 0 else 0
        if lost:
            np.add.at(lost_slot, s * SLOTS + r.integers(0, SLOTS, lost), 1)
        seen = n - lost
        if watched[s]:
            frames[s] += seen
            return seen
        return 0

    def arrive_all(n):
        """The same for a stream: n[s] frames in each second s."""
        lost = r.binomial(n, f)
        tot = int(lost.sum())
        if tot:
            np.add.at(lost_slot, np.repeat(np.arange(T), lost) * SLOTS + r.integers(0, SLOTS, tot), 1)
        seen = (n - lost) * watched
        frames[:] += seen
        return seen

    ki = r.poisson(p["kin"], T)
    ki[silent] = 0
    own_in[:] += arrive_all(ki)
    own_out[:] += arrive_all(r.poisson(p["kout"], T))
    arrive_all(r.poisson(bc_rate))
    arp_seen[:] = arrive_all(r.poisson(arp))
    dhcp_seen[:] = arrive_all(r.poisson(dhcp))
    # questions
    qs = []
    for s in range(T - WAIT):
        for _ in range(int(r.poisson(q[s]))):
            tq = s + float(r.random())
            if not arrive(1, s):
                continue  # lost, or on HOLD: never seen
            own_out[s] += 1
            answered = r.random() < p_ans[s]
            rtt_ms = float(math.exp(r.normal(p["rtt_mu"], p["rtt_sig"]))) * rtt_mult[s]
            ta = tq + rtt_ms / 1000.0
            sa = int(ta)
            seen_ans = False
            failed = False
            if answered and sa < T and ta < tq + WAIT and not silent[sa]:
                if arrive(1, sa):
                    seen_ans = True
                    own_in[sa] += 1
                    own_in_dns[sa] += 1
                    failed = r.random() < fail[s]
            wait_ok = all(watched[int(tq):int(tq + WAIT) + 1])
            qs.append((tq, seen_ans, failed, int(rtt_ms * 1000), wait_ok, answered))
    # the truth for answer time: medians over every answer the network sent, seen or not
    tb = sorted(x[3] for x in qs if x[0] < T_BASE and x[5])
    tw = sorted(x[3] for x in qs if T_BASE <= x[0] < T_BASE + T_WIN and x[5])
    material = bool(tb and tw and median_us(tw) * 100 >= median_us(tb) * SLOWER_PCT and
                    median_us(tw) >= median_us(tb) + SLOWER_US)
    return dict(f=f, watched=watched, frames=frames, own_in=own_in, own_in_dns=own_in_dns, own_out=own_out, arp=arp_seen,
                dhcp=dhcp_seen, lost_slot=lost_slot, qs=qs, rose=rose, selective=bool(selective[T_BASE:].any()),
                material_slow=material)


def readings(r, lost_slot):
    """Good readings as intervals (from, to, lost), seconds; failed spells merge
    their slots into the next good reading. Returns the intervals and the time
    of the last good reading."""
    n = len(lost_slot)
    fail = np.zeros(n, bool)
    if r.random() < 0.35:
        for _ in range(int(r.integers(1, 3))):
            a = int(r.integers(0, n))
            fail[a:a + int(r.integers(4, 161))] = True  # 1 to 40 s
    out = []
    last = 0.0
    acc = 0
    for j in range(n):
        acc += int(lost_slot[j])
        tr = (j + 1) / SLOTS
        if not fail[j]:
            if acc:
                out.append((last, tr, acc))
            last = tr
            acc = 0
    return out, last, bool(fail[T_BASE * SLOTS:(T_BASE + T_WIN + WAIT) * SLOTS].any())


def bounds(intervals, last_good, a, b, frames):
    lo = hi = 0
    for fr, to, n in intervals:
        if to <= a or fr >= b:
            continue
        hi += n
        if fr >= a and to <= b:
            lo += n
    known = last_good >= b
    flo = lo / (lo + frames) if lo else 0.0
    fhi = 1.0 if not known else (hi / (hi + frames) if hi else 0.0)
    return flo, fhi, known


def est_cov(intervals, last_good, frames):
    """E3a's per-second coverage, as the lagged readings place the loss: each
    reading's loss spread evenly over its interval; after the last good
    reading, nothing is known (coverage 0)."""
    est = np.zeros(T)
    for fr, to, n in intervals:
        s0, s1 = int(fr), min(int(math.ceil(to)), T)
        span = max(to - fr, 1e-9)
        for s in range(s0, s1):
            ov = min(to, s + 1) - max(fr, s)
            if ov > 0:
                est[s] += n * ov / span
    cov = np.where(frames + est > 0, frames / np.maximum(frames + est, 1e-9), 1.0)
    cov[int(math.floor(last_good)):] = 0.0  # no reading since: nothing known
    return cov


# ---- the methods --------------------------------------------------------------------------

def one_way(oi, oo):
    return oo >= ONE_WAY_MIN_OUT and oi == 0


def one_way_spell(own_in, own_in_dns, own_out, watched, a, m, b, f_hi):
    """As ppc_intel.c one_way_spell(): a silent run of watched seconds, with
    at least 8 frames sent, that the busier window's inbound rate ([a, m) or
    [m, b)), thinned by the window's loss bound, makes less likely than
    1/1000."""
    other_in = own_in - own_in_dns
    w = watched[a:b]
    tother = int(other_in[a:b][w].sum())
    src = other_in if tother > 0 else own_in  # as ppc_intel.c: inbound other than DNS answers, where there is any
    if OLD_SPELL:
        ws = int(w.sum())
        lam = src[a:b][w].sum() / ws if ws else 0.0
    else:
        rates = []
        for x, y in ((a, m), (m, b)):
            ww = watched[x:y]
            rates.append(src[x:y][ww].sum() / ww.sum() if ww.sum() else 0.0)
        lam = max(rates)
    if not lam > 0.0 or f_hi >= 1.0:
        return False
    lam *= 1.0 - f_hi
    run = out = 0
    for s in range(a, b):
        if not watched[s]:
            continue
        if src[s]:
            run = out = 0
            continue
        run += 1
        out += int(own_out[s])
        if out >= ONE_WAY_MIN_OUT and lam * run >= ONE_WAY_LOG_P:
            return True
    return False


def judge(sim, intervals, last_good, method):
    W = [(0, T_BASE), (T_BASE, T_BASE + T_WIN)]
    watched, frames = sim["watched"], sim["frames"]
    if method == "naive":
        keep = np.ones(T, bool)
    elif method == "gate":
        keep = watched & (est_cov(intervals, last_good, frames) >= GATE_COV)
    elif method == "oracle-gate":
        keep = watched & ((1.0 - sim["f"]) >= GATE_COV)
    else:
        keep = watched.copy()
    out = {}
    side = []
    for a, b in W:
        qa = [x for x in sim["qs"] if a <= x[0] < b]
        if method == "naive":
            dec = [x for x in qa if x[0] + WAIT <= T]
        elif method == "loss-aware":
            # as ppc_intel.c wait_watched(): watched whole, and the way back
            # witnessed by a frame other than a DNS answer during the wait
            back = sim["own_in"] - sim["own_in_dns"]
            dec = [x for x in qa if x[1] or (x[4] and back[int(x[0]):int(x[0] + WAIT) + 1].sum() > 0)]
        else:
            dec = [x for x in qa if all(keep[int(x[0]):int(x[0] + WAIT) + 1])]
        und = len(qa) - len(dec)
        kept_s = int(keep[a:b].sum())
        f_ans = bounds(intervals, last_good, a, b + WAIT, int(frames[a:b].sum()))
        f_win = bounds(intervals, last_good, a, b, int(frames[a:b].sum()))
        side.append(dict(
            n=len(dec), k=sum(1 for x in dec if x[1]), rtt=[x[3] for x in dec if x[1]],
            ans=sum(1 for x in dec if x[1]), failed=sum(1 for x in dec if x[1] and x[2]), und=und,
            w=kept_s, span=b - a, arp=int(sim["arp"][a:b][keep[a:b]].sum()),
            dhcp=int(sim["dhcp"][a:b][keep[a:b]].sum()),
            oi=int(sim["own_in"][a:b].sum()), oo=int(sim["own_out"][a:b].sum()), f_ans=f_ans, f_win=f_win))
    s0, s1 = side
    ow = method != "naive" and (one_way(s0["oi"], s0["oo"]) or one_way(s1["oi"], s1["oo"]))
    if method == "loss-aware" and not ow:
        ow = one_way_spell(sim["own_in"], sim["own_in_dns"], sim["own_out"], watched, W[0][0], W[1][0], W[1][1],
                           s1["f_win"][1])
    unwatched = method != "naive" and (s0["w"] * 2 < s0["span"] or s1["w"] * 2 < s1["span"])
    zero = ((0.0, 0.0), (0.0, 0.0))
    for m in METRICS:
        dns = m.startswith("dns")
        if unwatched:
            out[m] = (CANT, "not-watched")
            continue
        if dns and ow:
            out[m] = (CANT, "one-way")
            continue
        if m == "dns.time":
            if len(s0["rtt"]) < MIN_PAIRS or len(s1["rtt"]) < MIN_PAIRS:
                out[m] = (CANT, "too-few")
            else:
                out[m] = decide_time(s0["rtt"], s1["rtt"], s0["n"] - s0["k"], s1["n"] - s1["k"])
        elif m == "dns.answered":
            if s0["n"] < MIN_Q or s1["n"] < MIN_Q:
                out[m] = (CANT, "too-few")
            elif method == "loss-aware":
                f = ((s0["f_ans"][0], s0["f_ans"][1]), (s1["f_ans"][0], s1["f_ans"][1]))
                out[m] = decide_answered(s0["k"], s0["n"], s1["k"], s1["n"], f, s0["f_ans"][2] and s1["f_ans"][2])
            else:
                out[m] = decide_answered(s0["k"], s0["n"], s1["k"], s1["n"], zero, True)
        elif m == "dns.failed":
            if s0["ans"] < MIN_ANS or s1["ans"] < MIN_ANS:
                out[m] = (CANT, "too-few")
            else:
                out[m] = decide_failed(s0["failed"], s0["ans"], s1["failed"], s1["ans"])
        else:
            key = "arp" if m == "arp.request" else "dhcp"
            w0 = s0["span"] if method == "naive" else s0["w"]
            w1 = s1["span"] if method == "naive" else s1["w"]
            if w0 < MIN_W or w1 < MIN_W:
                out[m] = (CANT, "too-few")
            elif method == "loss-aware":
                f = ((s0["f_win"][0], s0["f_win"][1]), (s1["f_win"][0], s1["f_win"][1]))
                out[m] = decide_rate(s0[key], w0, s1[key], w1, f, s0["f_win"][2] and s1["f_win"][2])
            else:
                out[m] = decide_rate(s0[key], w0, s1[key], w1, zero, True)
    return out


# ---- one seed -----------------------------------------------------------------------------

def run_seed(seed, n_sites, per_site):
    r = np.random.default_rng(1000 + seed)
    rows = []
    for _ in range(n_sites):
        p = site_params(r)
        for _ in range(per_site):
            fault = list(FAULTS)[int(r.integers(0, len(FAULTS)))] if r.random() < 0.5 else None
            art_win = ARTIFACTS[int(r.integers(0, 3))] if r.random() < 0.5 else None
            art_base = ARTIFACTS[int(r.integers(0, 3))] if r.random() < 0.2 else None
            sim = simulate(r, p, fault, art_win, art_base)
            intervals, last_good, read_fail = readings(r, sim["lost_slot"])
            v = {m: judge(sim, intervals, last_good, m) for m in METHODS}
            rows.append(dict(fault=fault, art=art_win, art_base=art_base, q=p["q"], v=v, rose=sim["rose"],
                             selective=sim["selective"], material_slow=sim["material_slow"], read_fail=read_fail))
    return rows


def rate(num, den):
    return num / den if den else float("nan")


def score(rows, method):
    """Per method: false claims and missed faults, by condition."""
    fc = {k: [0, 0] for k in ("all", "none", "loss", "hold", "oneway")}
    fa = {k: [0, 0] for k in ("all", "none", "loss", "hold", "oneway")}   # windows with any false claim, no fault
    ab = {k: [0, 0] for k in ("all", "none", "loss", "hold", "oneway")}   # abstentions on clean metrics
    rec = {k: [0, 0] for k in ("all", "alone", "overlap", "overlap_loss", "overlap_hold", "overlap_oneway")}
    miss_how = {"cant-tell": 0, "no-change": 0, "better": 0, "small": 0}
    per_fault = {k: [0, 0, 0, 0, 0] for k in FAULTS}   # hit, n, cant-tell, no-change, small
    dens = {k: [0, 0] for k in ("sparse", "medium", "busy")}  # DNS faults by this device's question rate
    reasons = {}
    # answer time in windows with selective (burst) loss and no fault, split by whether the
    # true median (every answer the network sent) slowed materially (1.5x and 20 ms)
    sel = {k: {"n": 0, "worse": 0, "no-change": 0, "cant-tell": 0, "small": 0, "better": 0}
           for k in ("material", "not_material")}
    for x in rows:
        if x.get("selective") and not x["fault"]:
            d = sel["material" if x["material_slow"] else "not_material"]
            d["n"] += 1
            vt = x["v"][method]["dns.time"][0]
            d[vt if vt in d else "no-change"] += 1
        v = x["v"][method]
        art = x["art"] or "none"
        target = FAULTS.get(x["fault"]) if x["fault"] else None
        touched = (TOUCHES.get(x["fault"], set()) if x["fault"] else set()) | x["rose"]
        any_false = False
        for m in METRICS:
            if m in touched:
                continue
            for key in ("all", art):
                fc[key][1] += 1
                fc[key][0] += v[m][0] == WORSE
                ab[key][1] += 1
                ab[key][0] += v[m][0] == CANT
            any_false |= v[m][0] == WORSE
        if not x["fault"]:
            for key in ("all", art):
                fa[key][1] += 1
                fa[key][0] += any_false
        else:
            hit = v[target][0] == WORSE
            keys = ["all", "alone" if not x["art"] else "overlap"]
            if x["art"]:
                keys.append("overlap_" + x["art"])
            for key in keys:
                rec[key][1] += 1
                rec[key][0] += hit
            if not hit:
                miss_how[v[target][0] if v[target][0] in miss_how else "no-change"] += 1
                if v[target][0] == CANT:
                    reasons[v[target][1]] = reasons.get(v[target][1], 0) + 1
            pf = per_fault[x["fault"]]
            pf[0] += hit
            pf[1] += 1
            pf[2] += v[target][0] == CANT
            pf[3] += v[target][0] == NO_CHANGE
            pf[4] += v[target][0] == SMALL
            if target.startswith("dns"):
                d = "sparse" if x["q"] < 0.15 else "medium" if x["q"] < 0.6 else "busy"
                dens[d][0] += hit
                dens[d][1] += 1
    return dict(
        false_claim_rate={k: rate(*fc[k]) for k in fc},
        false_alert_windows={k: rate(*fa[k]) for k in fa},
        abstain_rate={k: rate(*ab[k]) for k in ab},
        recall={k: rate(*rec[k]) for k in rec},
        n_fault={k: rec[k][1] for k in rec},
        misses=miss_how,
        per_fault={k: dict(recall=rate(v[0], v[1]), cant_tell=rate(v[2], v[1]), no_change=rate(v[3], v[1]),
                           small=rate(v[4], v[1]))
                   for k, v in per_fault.items()},
        dns_recall_by_density={k: rate(*v) for k, v in dens.items()},
        miss_reasons=reasons,
        selective_time=sel,
    )


# ---- G0 (docs/apps/packetshark/neural/G0.md, frozen 27 November 2026) -------------------

G0_FALSE_MAX = 0.01      # false problem claims in each healthy visibility-artifact category, at most
G0_RECALL_MIN = 0.50     # recall for material faults overlapping capture loss, at least
G0_CATEGORIES = ("clean", "loss-uniform", "loss-burst", "hold", "one-way", "counters-failed")
MATERIAL_FAULTS = ("dns_down", "dns_slow", "servfail", "arp_storm", "dhcp_trouble")  # subtle_latency is not material


def wilson_bounds(k, n, z=1.645):
    """One-sided 95 % Wilson bounds."""
    if n == 0:
        return float("nan"), float("nan")
    ph = k / n
    d = 1 + z * z / n
    c = ph + z * z / (2 * n)
    w = z * math.sqrt(ph * (1 - ph) / n + z * z / (4 * n * n))
    return max(0.0, (c - w) / d), min(1.0, (c + w) / d)


def g0_categories(x):
    """The healthy visibility-artifact categories a window belongs to (it may be in two)."""
    cats = []
    if x["art"] == "loss":
        cats.append("loss-burst" if x.get("selective") else "loss-uniform")
    elif x["art"] == "hold":
        cats.append("hold")
    elif x["art"] == "oneway":
        cats.append("one-way")
    if x.get("read_fail"):
        cats.append("counters-failed")
    if not cats and not x["art_base"]:
        cats.append("clean")
    return cats


def g0(rows, method):
    """Counts for G0 over the given windows (pool seeds by summing)."""
    fc = {c: [0, 0] for c in G0_CATEGORIES}
    rec = [0, 0]
    reassure = [0, 0]
    abstain = [0, 0]
    pf = {f: [0, 0, 0, 0] for f in FAULTS}  # hit, n, abstained, no-change
    for x in rows:
        v = x["v"][method]
        if not x["fault"]:
            false = any(v[m][0] == WORSE for m in METRICS if m not in x["rose"])
            for c in g0_categories(x):
                fc[c][0] += false
                fc[c][1] += 1
            continue
        t = FAULTS[x["fault"]]
        d = pf[x["fault"]]
        d[0] += v[t][0] == WORSE
        d[1] += 1
        d[2] += v[t][0] == CANT
        d[3] += v[t][0] == NO_CHANGE
        if x["fault"] in MATERIAL_FAULTS:
            reassure[0] += v[t][0] == NO_CHANGE
            reassure[1] += 1
            abstain[0] += v[t][0] == CANT
            abstain[1] += 1
            if x["art"] == "loss":
                rec[0] += v[t][0] == WORSE
                rec[1] += 1
    return dict(false_claims=fc, recall_material_under_loss=rec, false_reassurance_material=reassure,
                abstention_material=abstain, per_fault=pf)


def g0_verdict(counts):
    out = {"false_claims": {}, "pass": True}
    for c, (k, n) in counts["false_claims"].items():
        lo, hi = wilson_bounds(k, n)
        ok = n > 0 and hi <= G0_FALSE_MAX
        out["false_claims"][c] = dict(k=k, n=n, rate=round(k / n, 5) if n else None, upper95=round(hi, 5), pass_=ok)
        out["pass"] &= ok
    k, n = counts["recall_material_under_loss"]
    lo, hi = wilson_bounds(k, n)
    out["recall_material_under_loss"] = dict(k=k, n=n, rate=round(k / n, 4) if n else None, lower95=round(lo, 4),
                                             pass_=n > 0 and lo >= G0_RECALL_MIN)
    out["pass"] &= out["recall_material_under_loss"]["pass_"]
    for key in ("false_reassurance_material", "abstention_material"):
        k, n = counts[key]
        out[key] = dict(k=k, n=n, rate=round(k / n, 4) if n else None)
    out["per_fault"] = {f: dict(n=v[1], recall=round(v[0] / v[1], 3) if v[1] else None,
                                abstained=round(v[2] / v[1], 3) if v[1] else None,
                                no_change=round(v[3] / v[1], 3) if v[1] else None)
                        for f, v in counts["per_fault"].items()}
    return out


def summarize(per_seed):
    """mean, sd, min, max over seeds of every number in the score dicts."""
    out = {}
    for method in METHODS:
        out[method] = {}
        for group in ("false_claim_rate", "false_alert_windows", "abstain_rate", "recall"):
            out[method][group] = {}
            for k in per_seed[0][method][group]:
                xs = np.array([s[method][group][k] for s in per_seed], float)
                xs = xs[~np.isnan(xs)]
                out[method][group][k] = dict(mean=round(float(xs.mean()), 4), sd=round(float(xs.std(ddof=1)), 4),
                                             min=round(float(xs.min()), 4), max=round(float(xs.max()), 4))
        out[method]["misses"] = {k: int(sum(s[method]["misses"][k] for s in per_seed)) for k in ("cant-tell",
                                                                                                    "no-change",
                                                                                                    "better",
                                                                                                    "small")}
        out[method]["per_fault"] = {f: {k: round(float(np.nanmean([s[method]["per_fault"][f][k] for s in per_seed])), 3)
                                        for k in ("recall", "cant_tell", "no_change", "small")} for f in FAULTS}
        out[method]["dns_recall_by_density"] = {k: round(float(np.nanmean([s[method]["dns_recall_by_density"][k]
                                                                           for s in per_seed])), 3)
                                                for k in ("sparse", "medium", "busy")}
        rs = {}
        for sd in per_seed:
            for k, v in sd[method]["miss_reasons"].items():
                rs[k] = rs.get(k, 0) + v
        out[method]["miss_reasons"] = rs
        out[method]["selective_time"] = {g: {k: int(sum(sd[method]["selective_time"][g][k] for sd in per_seed))
                                             for k in per_seed[0][method]["selective_time"][g]}
                                         for g in ("material", "not_material")}
    return out


def self_test():
    """The statistics agree with the values tests/ppc_intel_test.c checks in C."""
    assert abs(binom_le(0, 10, 0.5) - 977) <= 1
    assert abs(binom_le(5, 10, 0.5) - 623047) <= 1
    assert abs(binom_ge(9, 10, 0.5) - 10742) <= 1
    assert abs(binom_le(30, 40, 0.912) - 1995) <= 1
    assert abs(pois_ge(3, 1.5) - 191153) <= 2
    assert abs(pois_le(2, 6.0) - 61969) <= 2
    assert pois_ge(1000, 500.0) == 0


def main():
    out = Path(sys.argv[1])
    args = sys.argv[2:]
    seeds = int(args[args.index("--seeds") + 1]) if "--seeds" in args else 10
    sites = int(args[args.index("--sites") + 1]) if "--sites" in args else 40
    per = int(args[args.index("--windows") + 1]) if "--windows" in args else 60
    self_test()
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    per_seed = []
    g0_pool = {m: None for m in METHODS}
    for s in range(seeds):
        rows = run_seed(s, sites, per)
        sc = {m: score(rows, m) for m in METHODS}
        for m in METHODS:
            c = g0(rows, m)
            if g0_pool[m] is None:
                g0_pool[m] = c
            else:
                for c_ in G0_CATEGORIES:
                    g0_pool[m]["false_claims"][c_] = [a + b for a, b in zip(g0_pool[m]["false_claims"][c_],
                                                                            c["false_claims"][c_])]
                for key in ("recall_material_under_loss", "false_reassurance_material", "abstention_material"):
                    g0_pool[m][key] = [a + b for a, b in zip(g0_pool[m][key], c[key])]
                for f in FAULTS:
                    g0_pool[m]["per_fault"][f] = [a + b for a, b in zip(g0_pool[m]["per_fault"][f],
                                                                        c["per_fault"][f])]
        sc["n_windows"] = len(rows)
        per_seed.append(sc)
        print("seed %d: %d windows, %.0f s" % (s, len(rows), time.time() - t0), flush=True)
    res = dict(
        what="E3a-R, synthetic only; see the module docstring",
        rule=dict(old_time=OLD_TIME, old_spell=OLD_SPELL),
        config=dict(seeds=seeds, sites=sites, windows_per_site=per, alpha=ALPHA_E6 / 1e6, gate_coverage=GATE_COV,
                    fault_share=0.5, artifact_share_window=0.5, artifact_share_base=0.2,
                    loss_range=[0.001, 0.6], readings_per_s=SLOTS, read_fail_window_share=0.35),
        summary=summarize(per_seed),
        g0={m: g0_verdict(g0_pool[m]) for m in METHODS},
        seeds_used=[1000 + s_ for s_ in range(seeds)],
        per_seed=per_seed,
        seconds=round(time.time() - t0, 1),
    )
    (out / "e3a_r_results.json").write_text(json.dumps(res, indent=1))
    s = res["summary"]
    print("\nmethod       false-claim/metric  false-alert windows (none/loss/hold/oneway)   recall all/alone/overlap/loss")
    for m in METHODS:
        fc = s[m]["false_claim_rate"]["all"]
        fa = s[m]["false_alert_windows"]
        rc = s[m]["recall"]
        print("%-12s %.4f±%.4f   %.3f %.3f %.3f %.3f   %.3f %.3f %.3f %.3f  misses %s" % (
            m, fc["mean"], fc["sd"], fa["none"]["mean"], fa["loss"]["mean"], fa["hold"]["mean"],
            fa["oneway"]["mean"], rc["all"]["mean"], rc["alone"]["mean"], rc["overlap"]["mean"],
            rc["overlap_loss"]["mean"], s[m]["misses"]))
    print("\nG0 (frozen criteria, development set):")
    for m in METHODS:
        g = res["g0"][m]
        print("  %-12s %s  false claims upper95: %s  recall material under loss %.3f (lower95 %.3f)  "
              "false reassurance %.3f  abstention %.3f" % (
                  m, "PASS" if g["pass"] else "FAIL",
                  {c: v["upper95"] for c, v in g["false_claims"].items()},
                  g["recall_material_under_loss"]["rate"], g["recall_material_under_loss"]["lower95"],
                  g["false_reassurance_material"]["rate"], g["abstention_material"]["rate"]))
    for m in METHODS:
        print(m, "per fault", {f: (v["recall"], v["cant_tell"], v["no_change"], v["small"])
                               for f, v in s[m]["per_fault"].items()})
        print(m, "DNS recall by density", s[m]["dns_recall_by_density"], "abstain reasons", s[m]["miss_reasons"])
        print(m, "answer time under selective loss (true slowdown, no fault)", s[m]["selective_time"])


if __name__ == "__main__":
    main()
