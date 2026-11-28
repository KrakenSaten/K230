"""indep_gen2 -- independent synthetic comparison windows for the G0 held-out validation.

Written from G0-DEFINITIONS.md and INTERFACE.md only.  Public API:

    generate(seed: int, n_windows: int) -> list[dict]

Each window covers T = 125 seconds: the base minute (0-59), the window minute
(60-119) and a 5 s tail (120-124).  Arrays are numpy arrays (bool / int64 /
float64); `qs` is a list of plain-Python tuples; `rose` is a set of str.
numpy only, no I/O, deterministic per (seed, window index).
See GENERATOR.md for the model.
"""

import numpy as np

T = 125
NSLOT = 4 * T
BASE_R = (0, 60)       # base minute
WIN_R = (60, 125)      # window minute plus tail: faults and window artifacts live here
WIN_Q = (60, 120)      # window-minute questions

FAULTS = ("dns_down", "dns_slow", "servfail", "arp_storm", "dhcp_trouble", "subtle_latency")
FAULT_P = (0.09, 0.09, 0.09, 0.09, 0.09, 0.05)          # 50 % of windows faulted
FAULT_METRIC = {"dns_down": "dns.answered", "dns_slow": "dns.time", "servfail": "dns.failed",
                "arp_storm": "arp.request", "dhcp_trouble": "dhcp.discover",
                "subtle_latency": "dns.time"}
METRICS = ("dns.time", "dns.answered", "dns.failed", "dhcp.discover", "arp.request")

ART_KINDS = ("uniform", "burst", "hold", "oneway", "counters")
ART_P = (0.26, 0.22, 0.20, 0.16, 0.16)
P_ART_WIN = 0.50       # window minute has an artifact
P_ART_BASE = 0.35      # base minute has an artifact (independent)
P_READFAIL_OVERLAY = 0.15
P_BENIGN = 0.20        # per minute: a benign (non-fault) level change in one metric


# ----------------------------------------------------------------------------- helpers

def _lu(rng, a, b):
    """Log-uniform draw in [a, b]."""
    return float(np.exp(rng.uniform(np.log(a), np.log(b))))


def _comb(p, extra):
    """Combine two independent failure probabilities."""
    return 1.0 - (1.0 - p) * (1.0 - extra)


def _slot_counts(rng, rate_sec, shape):
    """Poisson counts per 250 ms slot with a per-second gamma(shape) rate modulation
    (mean preserved: burstiness without changing the true rate)."""
    lam = rate_sec * rng.gamma(shape, 1.0 / shape, T)
    return rng.poisson(np.repeat(lam / 4.0, 4))


def _cluster_counts(rng, rate_sec, mean_size, shape):
    """Compound Poisson per slot: clusters of geometric size (mean mean_size), so the
    true frame rate is rate_sec while counts come in sweeps."""
    lam = rate_sec * rng.gamma(shape, 1.0 / shape, T) / mean_size
    n = rng.poisson(np.repeat(lam / 4.0, 4))
    out = n.copy()
    m = n > 0
    if mean_size > 1.0 and m.any():
        out[m] += rng.negative_binomial(n[m], 1.0 / mean_size)
    return out


def _spans(rng, lo, hi):
    """Episode placement within [lo, hi) seconds: whole, edge, block or several short ones."""
    L = hi - lo
    r = rng.random()
    if r < 0.25:
        return [(lo, hi)]
    if r < 0.50:
        ln = int(rng.integers(3, L))
        return [(lo, lo + ln)] if rng.random() < 0.5 else [(hi - ln, hi)]
    if r < 0.82:
        ln = int(rng.integers(1, L))
        s = int(rng.integers(lo, hi - ln + 1))
        return [(s, s + ln)]
    out = []
    for _ in range(int(rng.integers(2, 6))):
        ln = int(rng.integers(1, max(2, L // 5) + 1))
        s = int(rng.integers(lo, hi - ln + 1))
        out.append((s, s + ln))
    return out


def _busy_seconds(rng, lo, hi):
    """Seconds of a few traffic bursts (downloads, sync, page loads) inside [lo, hi)."""
    busy = []
    for _ in range(int(rng.integers(1, 7))):
        ln = int(rng.integers(1, 9))
        s = int(rng.integers(lo, hi - ln + 1))
        busy.extend(range(s, s + ln))
    return sorted(set(busy))


def _read_fail_spell(rng, ok, lo, hi):
    """Mark failed counter readings in slots [lo, hi): whole span, one block,
    intermittent flapping, or several short spells.  At least one failure."""
    L = hi - lo
    r = rng.random()
    if r < 0.15:
        ok[lo:hi] = False
    elif r < 0.55:
        ln = int(rng.integers(1, L))
        s = int(rng.integers(lo, hi - ln + 1))
        ok[s:s + ln] = False
    elif r < 0.80:
        ln = int(rng.integers(8, L + 1))
        s = int(rng.integers(lo, hi - ln + 1))
        bad = rng.random(ln) < rng.uniform(0.2, 0.8)
        bad[int(rng.integers(0, ln))] = True
        ok[s:s + ln] &= ~bad
    else:
        for _ in range(int(rng.integers(2, 6))):
            ln = int(rng.integers(1, 13))
            s = int(rng.integers(lo, hi - ln + 1))
            ok[s:s + ln] = False


# ----------------------------------------------------------------------------- site

def _site(rng):
    kind = int(rng.choice(3, p=[0.35, 0.40, 0.25]))
    if kind == 0:     # home / small office: few devices
        bg, arp, dhcp, flood = _lu(rng, 0.2, 4), _lu(rng, 0.02, 0.4), _lu(rng, 0.0005, 0.01), 0.0
    elif kind == 1:   # office floor
        bg, arp, dhcp, flood = (_lu(rng, 3, 30), _lu(rng, 0.3, 3), _lu(rng, 0.004, 0.04),
                                _lu(rng, 0.01, 0.5))
    else:             # campus / venue Wi-Fi
        bg, arp, dhcp, flood = (_lu(rng, 20, 250), _lu(rng, 2, 25), _lu(rng, 0.02, 0.2),
                                _lu(rng, 0.2, 5))
    qc = int(rng.choice(3, p=[0.4, 0.4, 0.2]))
    q = _lu(rng, (0.01, 0.1, 0.8)[qc], (0.1, 0.8, 3.0)[qc])
    rc = int(rng.choice(3, p=[0.3, 0.5, 0.2]))      # LAN, ISP, distant resolver
    med = _lu(rng, (1.5, 10, 40)[rc], (15, 50, 150)[rc]) / 1000.0
    return dict(
        kind=("sparse", "medium", "busy")[kind], bg=bg, bg_shape=rng.uniform(1, 8),
        arp=arp, arp_m=rng.uniform(1.0, 3.0), dhcp=dhcp, flood=flood,
        own_in=_lu(rng, 0.2, 10), out_ratio=rng.uniform(0.5, 1.1), own_arp=_lu(rng, 0.003, 0.05),
        q=q, q_m=rng.uniform(1.2, 3.5),
        rtt_med=med, rtt_sig=rng.uniform(0.15, 0.6), miss=rng.uniform(0.0, 0.35),
        miss_mult=_lu(rng, 2, 12),
        p0=0.0 if rng.random() < 0.7 else _lu(rng, 0.001, 0.03),
        pfail=0.0 if rng.random() < 0.4 else _lu(rng, 0.001, 0.08),
        timeout=float(rng.choice([5.0, 5.0, 3.0, 2.0])), retries=int(rng.choice([0, 1, 1, 2])),
        cap=_lu(rng, 600, 3000),
    )


# ----------------------------------------------------------------------------- one window

def _window(rng, debug=False):
    S = _site(rng)
    sec = np.arange(T)
    in_win = sec >= 60

    # ---- per-second true parameters (base values, then minute changes)
    qmul = np.where(in_win, np.exp(rng.normal(0, 0.35)), np.exp(rng.normal(0, 0.2)))
    if rng.random() < 0.15:                       # the user started / stopped using the device
        qmul = np.where(in_win, qmul * _lu(rng, 0.2, 5), qmul)
    q_rate = S["q"] * qmul
    own_mul = np.where(in_win, np.exp(rng.normal(0, 0.5)), np.exp(rng.normal(0, 0.3)))
    own_in_rate = S["own_in"] * own_mul
    own_out_rate = own_in_rate * S["out_ratio"]
    bg_rate = np.full(T, S["bg"])
    arp_rate = np.full(T, S["arp"])
    dhcp_rate = np.full(T, S["dhcp"])
    own_arp_rate = np.full(T, S["own_arp"])
    p_unans = np.full(T, S["p0"])
    p_fail = np.full(T, S["pfail"])
    rtt_mult = np.ones(T)
    busy_delay = np.zeros(T)                      # congestion delay (s) in burst-loss seconds
    arp_shape = np.full(T, 2.0)
    dhcp_shape = np.full(T, 4.0)

    # ---- fault (window minute + tail)
    fault = None
    if rng.random() < sum(FAULT_P):
        fault = FAULTS[int(rng.choice(len(FAULTS), p=np.array(FAULT_P) / sum(FAULT_P)))]
    fmag = None
    w = in_win
    if fault == "dns_down":
        fmag = rng.uniform(0.75, 1.0)
        p_unans[w] = _comb(p_unans[w], fmag)
    elif fault == "dns_slow":
        fmag = _lu(rng, 3.2, 15.0)
        rtt_mult[w] *= fmag
    elif fault == "servfail":
        fmag = rng.uniform(0.32, 1.0)
        p_fail[w] = _comb(p_fail[w], fmag)
    elif fault == "arp_storm":
        fmag = _lu(rng, 5.5, 40.0)
        arp_rate[w] *= fmag
        arp_shape[w] = 1.5
    elif fault == "dhcp_trouble":
        fmag = _lu(rng, 0.32, 3.0)
        dhcp_rate[w] += fmag
        dhcp_shape[w] = 2.0
    elif fault == "subtle_latency":
        fmag = rng.uniform(1.3, 1.8)
        rtt_mult[w] *= fmag

    # ---- benign level changes (either minute; only window-minute ones are rises)
    rose = set()
    benign = {}
    fm = FAULT_METRIC.get(fault)
    for name, (lo, hi) in (("window", WIN_Q), ("base", BASE_R)):
        if rng.random() >= P_BENIGN:
            continue
        met = METRICS[int(rng.integers(0, len(METRICS)))]
        if met == fm or (name == "base" and met in benign.get("window", ())):
            continue
        if rng.random() < 0.5:
            s, e = lo, hi
        else:
            ln = int(rng.integers(15, 61))
            s = int(rng.integers(lo, hi - ln + 1))
            e = s + ln
        if met == "arp.request":
            arp_rate[s:e] *= _lu(rng, 1.5, 8.0)
        elif met == "dhcp.discover":
            dhcp_rate[s:e] += _lu(rng, 0.03, 0.4)
        elif met == "dns.time":
            rtt_mult[s:e] *= _lu(rng, 1.3, 2.5)
        elif met == "dns.failed":
            p_fail[s:e] = _comb(p_fail[s:e], rng.uniform(0.05, 0.3))
        else:
            p_unans[s:e] = _comb(p_unans[s:e], rng.uniform(0.05, 0.4))
        benign[name] = (met, s, e)
        if name == "window":
            rose.add(met)

    # ---- visibility artifacts
    def pick(p):
        return None if rng.random() >= p else ART_KINDS[int(rng.choice(5, p=ART_P))]

    kb, kw = pick(P_ART_BASE), pick(P_ART_WIN)
    watched = np.ones(T, bool)
    oneway = np.zeros(T, bool)
    dev_oneway = np.zeros(T, bool)      # rx truly broken: the device itself hears nothing
    p_slot = np.zeros(NSLOT)            # uniform-loss drop probability per slot
    burst_secs = {"base": [], "window": []}
    read_ok = np.ones(NSLOT, bool)
    probe_n = {"base": 0.0, "window": 0.0}

    straddle = None
    if kb is not None and kb == kw and kb in ("uniform", "hold", "oneway") and rng.random() < 0.3:
        straddle = (int(rng.integers(20, 60)), int(rng.integers(62, 126)))

    for name, kind, (lo, hi) in (("base", kb, BASE_R), ("window", kw, WIN_R)):
        if kind is None:
            continue
        spans = [straddle] if straddle else _spans(rng, lo, hi)
        if name == "window" and straddle:
            continue                                   # already applied with the base minute
        if kind == "hold":
            for s, e in spans:
                watched[s:e] = False
        elif kind == "oneway":
            dev = rng.random() < 0.4
            probe = rng.uniform(0.3, 1.0)
            delay = int(rng.integers(2, 21))
            for s, e in spans:
                oneway[s:e] = True
                if dev:
                    dev_oneway[s:e] = True
                    own_out_rate[s:e] *= rng.uniform(1.0, 2.0)   # TCP retransmits
                    own_arp_rate[s + delay:e] += probe         # neighbour re-probing
                    for t in range(s + delay, e):
                        probe_n["base" if t < 60 else "window"] += probe
        elif kind == "uniform":
            level = _lu(rng, 0.001, 0.6)
            for s, e in spans:
                n = 4 * (e - s)
                p_slot[4 * s:4 * e] = np.clip(level * np.exp(rng.normal(0, 0.25, n)), 0.001, 0.6)
        elif kind == "burst":
            burst_secs[name] = _busy_seconds(rng, lo, min(hi, 120))
        elif kind == "counters":
            sl = (0, 240) if name == "base" else (240, NSLOT)
            _read_fail_spell(rng, read_ok, *sl)
        if kind not in ("counters", None) and rng.random() < P_READFAIL_OVERLAY:
            sl = (0, 240) if name == "base" else (240, NSLOT)
            _read_fail_spell(rng, read_ok, *sl)

    # ---- own traffic bursts: saturating ones (burst loss) and harmless ones
    dl_in = np.zeros(NSLOT)
    dl_out = np.zeros(NSLOT)
    busy_mask = np.zeros(T, bool)
    for name, (lo, hi) in (("base", BASE_R), ("window", WIN_Q)):
        if burst_secs[name]:
            for t in burst_secs[name]:
                d = S["cap"] * rng.uniform(1.3, 6.0)
                wts = rng.dirichlet(np.full(4, 2.0))
                dl_in[4 * t:4 * t + 4] += 0.7 * d * wts
                dl_out[4 * t:4 * t + 4] += 0.3 * d * wts
                busy_mask[t] = True
                busy_delay[t] = _lu(rng, 0.02, 0.25) * np.sqrt(d / S["cap"])
        elif rng.random() < 0.35:
            for t in _busy_seconds(rng, lo, hi):
                d = S["cap"] * rng.uniform(0.05, 0.6)
                dl_in[4 * t:4 * t + 4] += 0.7 * d / 4
                dl_out[4 * t:4 * t + 4] += 0.3 * d / 4

    # ---- frame arrivals per slot (true, before capture)
    inc = {
        "bg": _slot_counts(rng, bg_rate, S["bg_shape"]),
        "flood": _slot_counts(rng, np.full(T, S["flood"]), 0.5),
        "arp": _cluster_counts(rng, arp_rate, S["arp_m"], arp_shape),
        "dhcp": _slot_counts(rng, dhcp_rate, dhcp_shape),
        "own_in": _slot_counts(rng, own_in_rate, 0.7) + rng.poisson(dl_in),
    }
    out = {
        "own_out": _slot_counts(rng, own_out_rate, 0.7) + rng.poisson(dl_out),
        "own_arp": _slot_counts(rng, own_arp_rate, 3.0),
    }

    # ---- DNS questions (device behaviour, incl. retries) and true answers
    lam = q_rate / S["q_m"]
    nc = rng.poisson(lam)
    ct = np.repeat(sec, nc) + rng.random(int(nc.sum()))
    sizes = rng.geometric(1.0 / S["q_m"], ct.size)
    starts = np.repeat(np.cumsum(sizes) - sizes, sizes)
    idx = np.arange(int(sizes.sum())) - starts
    tq = np.repeat(ct, sizes) + idx * rng.exponential(0.04, idx.size)
    tq = tq[tq < T]

    cols = {k: [] for k in ("tq", "ans", "fail", "rtt", "slowed")}
    pending = tq
    for attempt in range(S["retries"] + 1):
        n = pending.size
        if n == 0:
            break
        s = pending.astype(int)
        ans = rng.random(n) >= p_unans[s]
        fail = ans & (rng.random(n) < p_fail[s])
        r0 = S["rtt_med"] * np.exp(S["rtt_sig"] * rng.normal(0, 1, n))
        r0 = np.where(rng.random(n) < S["miss"], r0 * S["miss_mult"], r0)
        r1 = r0 * rtt_mult[s]
        s2 = np.minimum((pending + r1).astype(int), T - 1)
        dly = np.maximum(busy_delay[s], busy_delay[s2])
        rtt = r1 + dly
        ta = pending + rtt
        sa = np.minimum(ta.astype(int), T - 1)
        got = ans & (rtt <= S["timeout"]) & ~(dev_oneway[sa] & (ta < T))
        for k, v in (("tq", pending), ("ans", ans), ("fail", fail), ("rtt", rtt),
                     ("slowed", dly > 0)):
            cols[k].append(v)
        if attempt < S["retries"]:
            rt = pending[~got] + S["timeout"] + rng.uniform(0.0, 0.05, int((~got).sum()))
            pending = rt[rt < T]
    if cols["tq"]:
        Q = {k: np.concatenate(v) for k, v in cols.items()}
    else:
        Q = {k: np.zeros(0, dt) for k, dt in (("tq", float), ("ans", bool), ("fail", bool),
                                                ("rtt", float), ("slowed", bool))}
    order = np.argsort(Q["tq"], kind="stable")
    Q = {k: v[order] for k, v in Q.items()}
    nq = Q["tq"].size
    q_slot = (Q["tq"] * 4).astype(int)
    ta = Q["tq"] + Q["rtt"]
    a_has = Q["ans"] & (ta < T)
    a_slot = np.minimum((ta * 4).astype(int), NSLOT - 1)
    a_sec = a_slot // 4

    # ---- drop probability per slot: uniform loss, plus capture saturation in busy seconds
    ow_slot = np.repeat(oneway, 4)
    q_cnt = np.bincount(q_slot, minlength=NSLOT)
    a_arr = a_has & ~oneway[a_sec]
    a_cnt = np.bincount(a_slot[a_arr], minlength=NSLOT)
    arrive_in = sum(inc.values()) * ~ow_slot + a_cnt
    arrive_all = arrive_in + sum(out.values()) + q_cnt
    bs = np.repeat(busy_mask, 4)
    if bs.any():
        cap4 = S["cap"] / 4.0
        sat = np.where(arrive_all > cap4, 1.0 - cap4 / np.maximum(arrive_all, 1), 0.0)
        p_slot = np.where(bs, sat, p_slot)

    # ---- capture: kernel drops by arrival time (any kind), HOLD, one-way
    w_slot = np.repeat(watched, 4)
    lost = np.zeros(NSLOT, np.int64)
    seen = {}
    for k, c in inc.items():
        c = np.where(ow_slot, 0, c)
        l = rng.binomial(c, p_slot)
        lost += l
        seen[k] = (c - l) * w_slot
    for k, c in out.items():
        l = rng.binomial(c, p_slot)
        lost += l
        seen[k] = (c - l) * w_slot
    q_lost = rng.random(nq) < p_slot[q_slot]
    q_seen = ~q_lost & watched[q_slot // 4]
    a_lost = a_arr & (rng.random(nq) < p_slot[a_slot])
    a_seen = a_arr & ~a_lost & watched[a_sec]
    lost += np.bincount(q_slot[q_lost], minlength=NSLOT)
    lost += np.bincount(a_slot[a_lost], minlength=NSLOT)
    seen["dns_q"] = np.bincount(q_slot[q_seen], minlength=NSLOT)
    seen["dns_a"] = np.bincount(a_slot[a_seen], minlength=NSLOT)

    def per_sec(x):
        return np.asarray(x, np.int64).reshape(T, 4).sum(1)

    own_in = per_sec(seen["own_in"] + seen["dns_a"])
    own_in_dns = per_sec(seen["dns_a"])
    own_out = per_sec(seen["own_out"] + seen["own_arp"] + seen["dns_q"])
    arp = per_sec(seen["arp"] + seen["own_arp"])
    dhcp = per_sec(seen["dhcp"])
    frames = own_in + own_out + per_sec(seen["bg"] + seen["flood"] + seen["arp"] + seen["dhcp"])
    arr_sec = per_sec(arrive_all)
    # f: the true drop share of that second, i.e. the drop probability weighted by
    # arrivals per slot (the plain slot mean when nothing arrived)
    pw = (p_slot * arrive_all).reshape(T, 4).sum(1)
    f = np.where(arr_sec > 0, pw / np.maximum(arr_sec, 1), p_slot.reshape(T, 4).mean(1))

    # ---- qs: questions the capture saw, asked before 120
    qs = []
    tq_us = Q["tq"]
    for i in np.nonzero(q_seen & (tq_us < 120.0))[0]:
        t = float(tq_us[i])
        ww = bool(watched[int(t):int(t + 5.0) + 1].all())
        ans = bool(Q["ans"][i])
        sa = bool(a_seen[i] and Q["rtt"][i] <= 5.0)
        qs.append((t, sa, bool(ans and Q["fail"][i]),
                   int(round(Q["rtt"][i] * 1e6)) if ans else 0, ww, ans))

    # ---- ground truth summaries
    def answered(lo, hi):
        m = Q["ans"] & (Q["tq"] >= lo) & (Q["tq"] < hi)
        return Q["rtt"][m], Q["slowed"][m]

    rb, slb = answered(0, 60)
    rw, slw = answered(60, 120)
    material_slow = False
    if rb.size and rw.size:
        mb, mw = float(np.median(rb)), float(np.median(rw))
        material_slow = bool(mw >= 1.5 * mb and mw - mb >= 0.020)
    if slw.size and slw.any() and slw.mean() > (slb.mean() if slb.size else 0.0):
        rose.add("dns.time")                       # congestion really slowed answers
    if probe_n["window"] > probe_n["base"]:
        rose.add("arp.request")                    # the device really re-probed its neighbours
    if fm is not None:
        rose.discard(fm)

    art_name = {"uniform": "loss", "burst": "loss", "hold": "hold", "oneway": "oneway",
                "counters": None, None: None}
    win = {
        "watched": watched, "frames": frames, "own_in": own_in, "own_in_dns": own_in_dns,
        "own_out": own_out, "arp": arp, "dhcp": dhcp, "lost_slot": lost,
        "read_ok": read_ok, "f": f.astype(np.float64), "qs": qs, "rose": rose,
        "fault": fault, "art": art_name[kw], "art_base": art_name[kb],
        "selective": kw == "burst", "material_slow": material_slow,
        "read_fail": bool(not read_ok[240:].all()), "q": float(S["q"]),
    }
    if not debug:
        return win
    dbg = dict(site=S, kind_base=kb, kind_win=kw, fmag=fmag, benign=benign, oneway=oneway,
               dev_oneway=dev_oneway, p_slot=p_slot, arrive_all=arrive_all, Q=Q,
               q_seen=q_seen, a_seen=a_seen, arp_rate=arp_rate, dhcp_rate=dhcp_rate,
               p_unans=p_unans, p_fail=p_fail, rtt_mult=rtt_mult, busy_mask=busy_mask,
               probe_n=probe_n, seen=seen)
    return win, dbg


def _rng(seed, i):
    return np.random.default_rng(np.random.SeedSequence([int(seed) % (1 << 63), int(i), 0x47302]))


def generate(seed: int, n_windows: int) -> list:
    """Deterministic for a given seed."""
    return [_window(_rng(seed, i)) for i in range(int(n_windows))]


def _generate_debug(seed, n_windows):
    """Same windows as generate(), plus internals for the self-check."""
    return [_window(_rng(seed, i), debug=True) for i in range(int(n_windows))]
