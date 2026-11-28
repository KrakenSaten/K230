"""indep_gen3: independent synthetic capture generator for the G0 held-out validation.

Written from G0-DEFINITIONS.md and INTERFACE.md only. See GENERATOR.md for the model.

API:  generate(seed: int, n_windows: int) -> list[dict]
Deterministic per seed; window i depends only on (seed, i). numpy only.
"""
import numpy as np

T = 125            # seconds: base 0-59, window 60-119, tail 120-124
NSLOT = 4 * T      # 250 ms lost-frame slots
SECS = np.arange(T, dtype=float)
WIN0, WIN1 = 60.0, 120.0

FAULTS = [None, "dns_down", "dns_slow", "servfail", "arp_storm",
          "dhcp_trouble", "subtle_latency"]
FAULT_P = [0.50, 0.085, 0.085, 0.085, 0.085, 0.085, 0.075]
METRIC_OF_FAULT = {
    "dns_down": "dns.answered", "dns_slow": "dns.time", "servfail": "dns.failed",
    "arp_storm": "arp.request", "dhcp_trouble": "dhcp.discover",
    "subtle_latency": "dns.time",
}
METRICS = ["dns.time", "dns.answered", "dns.failed", "dhcp.discover", "arp.request"]

# Artifact plan per minute. 'uniform' and 'burst' are both art == 'loss'.
ART_TYPES = [None, "uniform", "burst", "hold", "oneway"]
ART_W_P = [0.56, 0.15, 0.11, 0.10, 0.08]
ART_B_P = [0.70, 0.10, 0.07, 0.07, 0.06]

# Site classes: (weight, ranges). Rates are per second, log-uniform within range.
SITES = {
    #            weight  others' bcast/mcast  ARP req (others)  DHCP DISCOVER    own unicast in   DNS questions
    "sparse": (0.25, dict(bg=(0.2, 4.0), arp=(0.01, 0.25), dhcp=(0.0003, 0.01), own=(0.1, 15.0), q=(0.01, 0.5))),
    "home":   (0.25, dict(bg=(2.0, 30.0), arp=(0.05, 1.5), dhcp=(0.001, 0.03), own=(1.0, 80.0), q=(0.03, 1.5))),
    "office": (0.25, dict(bg=(15.0, 150.0), arp=(0.5, 6.0), dhcp=(0.005, 0.10), own=(3.0, 200.0), q=(0.08, 2.5))),
    "busy":   (0.25, dict(bg=(60.0, 600.0), arp=(2.0, 30.0), dhcp=(0.02, 0.40), own=(5.0, 400.0), q=(0.15, 4.0))),
}
SITE_NAMES = list(SITES)
SITE_W = [SITES[k][0] for k in SITE_NAMES]

RTT_CLIP_S = 4.8   # resolvers answer (or give up) before the client's 5 s wait ends


def _lu(rng, a, b, size=None):
    return np.exp(rng.uniform(np.log(a), np.log(b), size))


def _cov(start, end):
    """Fraction of each second [s, s+1) covered by [start, end)."""
    return np.clip(np.minimum(end, SECS + 1.0) - np.maximum(start, SECS), 0.0, 1.0)


def _inspan(t, start, end):
    return (t >= start) & (t < end)


def _ar1(rng, sigma, rho):
    """Stationary log-normal AR(1) multiplier with mean 1 (slow busy-ness)."""
    e = rng.standard_normal(T)
    x = np.empty(T)
    x[0] = e[0]
    a = np.sqrt(1.0 - rho * rho)
    for t in range(1, T):
        x[t] = rho * x[t - 1] + a * e[t]
    return np.exp(sigma * x - 0.5 * sigma * sigma)


def _poisson_times(rng, rate_t):
    """Event times for a piecewise-constant per-second rate."""
    n = rng.poisson(np.maximum(rate_t, 0.0))
    if n.sum() == 0:
        return np.empty(0)
    return np.repeat(SECS, n) + rng.random(n.sum())


def _hist(times, weights=None):
    times = times[(times >= 0) & (times < T)]
    return np.bincount(times.astype(np.int64), weights=weights, minlength=T)[:T]


# --------------------------------------------------------------------------- artifacts

def _plan_artifacts(rng):
    tw = ART_TYPES[rng.choice(5, p=ART_W_P)]
    cross = tw is not None and rng.random() < 0.35
    if cross:
        tb = tw
    else:
        tb = ART_TYPES[rng.choice(5, p=ART_B_P)]
    return tb, tw, cross


def _spans_in(rng, r0, r1, kind):
    """List of [a, b) integer-second spans for one artifact inside region [r0, r1)."""
    L = r1 - r0
    u = rng.random()
    spans = []
    if kind == "hold":
        if u < 0.10:
            spans.append((r0, r1))                          # whole minute on HOLD
        elif u < 0.65:
            ln = int(min(L, max(1, round(_lu(rng, 1, 60)))))  # one hold, 1-60 s
            a = rng.integers(r0, r1 - ln + 1)
            spans.append((a, a + ln))
        else:
            for _ in range(rng.integers(2, 6)):             # several short holds (app switching)
                ln = int(rng.integers(1, 9))
                a = rng.integers(r0, r1 - ln + 1)
                spans.append((a, a + ln))
    elif kind == "oneway":
        if u < 0.35:
            spans.append((r0, r1))
        elif u < 0.70:
            a = rng.integers(r0, r1 - 3)                    # from a moment to the end of the region
            spans.append((a, r1))
        else:
            ln = int(rng.integers(5, min(50, L - 1)))
            a = rng.integers(r0, r1 - ln + 1)
            spans.append((a, a + ln))
    else:  # uniform loss
        if u < 0.40:
            spans.append((r0, r1))
        elif u < 0.80:
            ln = int(rng.integers(3, min(56, L)))
            a = rng.integers(r0, r1 - ln + 1)
            spans.append((a, a + ln))
        else:
            for _ in range(rng.integers(2, 5)):
                ln = int(rng.integers(2, 16))
                a = rng.integers(r0, r1 - ln + 1)
                spans.append((a, a + ln))
    return spans


def _uniform_f(rng, a, b, f):
    f0 = _lu(rng, 0.001, 0.6)
    n = b - a
    shape = rng.integers(4)
    if shape == 0:
        prof = np.ones(n)
    elif shape == 1:
        prof = np.linspace(0.3, 1.0, n)
    elif shape == 2:
        prof = np.linspace(1.0, 0.3, n)
    else:
        prof = np.exp(0.35 * rng.standard_normal(n))
    f[a:b] = np.maximum(f[a:b], np.clip(f0 * prof, 0.001, 0.6))


def _busy_seconds(rng, r0, r1, busy, slow):
    """Burst episodes (time-correlated loss) in [r0, r1): traffic multiplier and DNS slowdown."""
    n_ep = 1 + min(6, rng.poisson(_lu(rng, 0.5, 3.0)))
    mean_len = _lu(rng, 1.5, 4.0)
    for _ in range(n_ep):
        ln = int(min(8, rng.geometric(1.0 / mean_len)))
        a = int(rng.integers(r0, max(r0 + 1, r1 - ln + 1)))
        b = min(r1, a + ln)
        busy[a:b] = np.maximum(busy[a:b], _lu(rng, 3.0, 15.0))
        slow[a:b] = np.maximum(slow[a:b], _lu(rng, 1.5, 6.0))


# --------------------------------------------------------------------------- one window

def _window(rng):
    # ---- site -------------------------------------------------------------
    site = SITE_NAMES[rng.choice(len(SITE_NAMES), p=SITE_W)]
    R = SITES[site][1]
    wifi = rng.random() < 0.6
    bg_rate = _lu(rng, *R["bg"])
    arp_rate = _lu(rng, *R["arp"])
    dhcp_rate = _lu(rng, *R["dhcp"])
    own_rate = _lu(rng, *R["own"])
    q = float(_lu(rng, *R["q"]))
    own_arp_rate = _lu(rng, 0.005, 0.05)
    ack_ratio = rng.uniform(0.3, 0.8)
    own_out_base = _lu(rng, 0.05, 2.0)
    flood_rate = 0.0 if wifi else bg_rate * _lu(rng, 0.01, 0.2)

    # slow busy-ness (stationary: same expected rate in both minutes)
    busy_bg = _ar1(rng, rng.uniform(0.15, 0.5), rng.uniform(0.7, 0.95))
    busy_own = _ar1(rng, rng.uniform(0.2, 0.7), rng.uniform(0.6, 0.95))

    # resolver
    rtt_hit_ms = _lu(rng, 1.5, 40.0)
    hit_p = rng.uniform(0.5, 0.95)
    miss_mult = _lu(rng, 3.0, 15.0)
    rtt_sigma = rng.uniform(0.15, 0.5)
    jitter_ms = _lu(rng, 0.3, 4.0) if wifi else _lu(rng, 0.05, 0.4)
    base_noans = _lu(rng, 0.0005, 0.01)
    base_fail = _lu(rng, 0.002, 0.06)        # NXDOMAIN etc. from search domains and typos
    timeout_s = float(rng.choice([1.0, 2.0, 2.5, 3.0, 5.0]))
    max_attempts = int(rng.choice([2, 3]))

    # ---- artifacts plan ---------------------------------------------------
    tb, tw, cross = _plan_artifacts(rng)
    watched = np.ones(T, dtype=bool)
    oneway = np.zeros(T, dtype=np.int64)    # 0 none, 1 unicast-to-me blocked, 2 all inbound blocked
    f = np.zeros(T)
    busy = np.ones(T)                       # burst traffic multiplier
    slow = np.ones(T)                       # congestion slowdown of DNS answers
    burst_region = np.zeros(T, dtype=bool)
    ow_mode = 1 if rng.random() < 0.5 else 2
    burst_target_own = rng.random() < 0.5

    def apply(kind, spans):
        for a, b in spans:
            if kind == "hold":
                watched[a:b] = False
            elif kind == "oneway":
                oneway[a:b] = ow_mode
            elif kind == "uniform":
                _uniform_f(rng, a, b, f)

    if cross:
        a = int(rng.integers(5, 59))
        b = int(rng.integers(62, 126))
        if tw == "burst":
            burst_region[0:60] = True
            burst_region[60:T] = True
            _busy_seconds(rng, 0, 60, busy, slow)
            _busy_seconds(rng, 60, T, busy, slow)
        else:
            apply(tw, [(a, b)])
    else:
        for kind, (r0, r1) in ((tb, (0, 60)), (tw, (60, T))):
            if kind is None:
                continue
            if kind == "burst":
                burst_region[r0:r1] = True
                _busy_seconds(rng, r0, r1, busy, slow)
            else:
                apply(kind, _spans_in(rng, r0, r1, kind))

    # ---- fault plan -------------------------------------------------------
    fault = FAULTS[rng.choice(len(FAULTS), p=FAULT_P)]
    fmetric = METRIC_OF_FAULT.get(fault)
    fs = fe = None
    fmag = None
    if fault is not None:
        c = rng.uniform(0.90, 1.0) if fault.startswith("dns") or fault == "servfail" else rng.uniform(0.85, 1.0)
        D = c * 60.0
        fs = WIN0 + rng.uniform(0.0, 60.0 - D)
        fe = fs + D
        if fe >= 119.0:
            fe = float(T)                   # still going at the end of the window
        c = (min(fe, WIN1) - fs) / 60.0
        if fault == "dns_down":
            fmag = rng.uniform(max(0.90, 0.80 / c), 1.0)       # share of answers withheld
        elif fault == "dns_slow":
            fmag = _lu(rng, 4.0, 25.0)                          # answer-time multiplier
        elif fault == "servfail":
            fmag = rng.uniform(max(0.40, 0.36 / c), 1.0)        # share of answers SERVFAIL
        elif fault == "arp_storm":
            fmag = _lu(rng, 1.0 + 4.4 / c, 60.0)                # ARP rate multiplier in span
        elif fault == "dhcp_trouble":
            fmag = _lu(rng, 0.35, 3.0) / c                      # added DISCOVERs per second
        elif fault == "subtle_latency":
            fmag = rng.uniform(1.3, 1.8)

    # ---- benign changes (true, no fault) ----------------------------------
    rose = set()
    benign = None          # (metric, start, end, magnitude, variant)
    fall = None            # (metric, factor) for the whole window minute
    avail = [m for m in METRICS if m != fmetric]
    if rng.random() < 0.15:
        w = np.array([{"arp.request": 0.38, "dhcp.discover": 0.28, "dns.time": 0.12,
                       "dns.failed": 0.12, "dns.answered": 0.10}[m] for m in avail])
        m = avail[rng.choice(len(avail), p=w / w.sum())]
        bs = WIN0 + rng.uniform(0.0, 40.0)
        be = bs + rng.uniform(15.0, 60.0)
        if be >= 119.0:
            be = float(T)
        if m == "arp.request":
            mag = _lu(rng, 0.4, 6.0)      # extra ARP as a multiple of the mean (device joins, scans)
        elif m == "dhcp.discover":
            mag = _lu(rng, 0.02, 0.4)     # extra DISCOVERs/s (people arriving)
        elif m == "dns.time":
            mag = _lu(rng, 1.3, 3.0)      # cold cache / upstream path change
        elif m == "dns.failed":
            mag = rng.uniform(0.05, 0.35)  # extra error share (app asking a missing name)
        else:
            mag = rng.uniform(0.05, 0.45)  # extra unanswered share (query type silently dropped)
        benign = (m, bs, be, mag)
        rose.add(m)
    else:
        avail2 = [m for m in avail if m in ("arp.request", "dhcp.discover", "dns.time")]
        if avail2 and rng.random() < 0.08:
            fall = (avail2[rng.integers(len(avail2))], rng.uniform(0.3, 0.85))

    def bspan(metric):
        return benign is not None and benign[0] == metric

    # congestion in burst seconds slows answers: a real change of the answer-time level
    slow_frac_b = np.mean(slow[0:60] > 1.0)
    slow_frac_w = np.mean(slow[60:120] > 1.0)
    if fmetric != "dns.time" and slow_frac_w > slow_frac_b:
        rose.add("dns.time")

    # ---- broadcast / multicast arrivals -----------------------------------
    # ARP from others: stationary Poisson + optional periodic sweep by a router/NAS
    sweep = np.zeros(T)
    sweep_mean = 0.0
    if rng.random() < 0.2:
        period = _lu(rng, 30.0, 90.0)
        size = int(rng.integers(4, 21))
        t0 = rng.uniform(0.0, period)
        while t0 < T:
            ts = t0 + rng.uniform(0.0, 2.0, size)
            sweep += _hist(ts)
            t0 += period
        sweep_mean = size / period
    arp_mean = arp_rate + sweep_mean + own_arp_rate
    arp_rt = np.full(T, arp_rate)
    if fault == "arp_storm":
        arp_rt += arp_mean * (fmag - 1.0) * _cov(fs, fe)
    if bspan("arp.request"):
        arp_rt += arp_mean * benign[3] * _cov(benign[1], benign[2])
    if fall is not None and fall[0] == "arp.request":
        arp_rt[60:] *= fall[1]
        sweep[60:] = np.round(sweep[60:] * fall[1])
    arp_others = rng.poisson(arp_rt) + sweep.astype(np.int64)

    # DHCP DISCOVER: joining clients, some retransmit once a few seconds later
    ev_rate = np.full(T, dhcp_rate / 1.3)
    if fall is not None and fall[0] == "dhcp.discover":
        ev_rate[60:] *= fall[1]
    ev = _poisson_times(rng, ev_rate)
    rep = ev[rng.random(len(ev)) < 0.3]
    rep = rep + rng.uniform(1.0, 4.0, len(rep))
    dhcp_times = [ev, rep]
    extra_dhcp = np.zeros(T)
    if fault == "dhcp_trouble":
        extra_dhcp += fmag * _cov(fs, fe)
    if bspan("dhcp.discover"):
        extra_dhcp += benign[3] * _cov(benign[1], benign[2])
    dhcp_times.append(_poisson_times(rng, extra_dhcp))
    dhcp_others = _hist(np.concatenate(dhcp_times)).astype(np.int64)

    # Other broadcast/multicast (mDNS, SSDP, NetBIOS, IPv6 ND...) plus periodic announcers
    bc = rng.poisson(bg_rate * busy_bg).astype(np.int64)
    for _ in range(rng.integers(0, 4)):
        period = _lu(rng, 20.0, 120.0)
        t0 = rng.uniform(0.0, period)
        size = int(rng.integers(3, 16))
        while t0 < T:
            bc[int(t0)] += size
            t0 += period
    flood = rng.poisson(flood_rate * busy_bg).astype(np.int64)

    own_arp = rng.poisson(np.full(T, own_arp_rate)).astype(np.int64)

    # ---- DNS questions ----------------------------------------------------
    p_aaaa = rng.uniform(0.3, 0.9)
    mean_names = _lu(rng, 1.0, 4.0)
    q_per_cluster = mean_names * (1.0 + p_aaaa)
    poll_rate = 0.0
    poll_times = np.empty(0)
    if rng.random() < 0.4:
        period = float(rng.choice([15.0, 30.0, 60.0, 120.0]))
        pr = (1.0 + p_aaaa) / period
        if pr <= 0.5 * q:
            poll_rate = pr
            t0 = rng.uniform(0.0, period)
            grid = np.arange(t0, T, period)
            poll_times = np.maximum(0.0, grid + rng.normal(0.0, 0.05, len(grid)))
    cl_rate = (q - poll_rate) / q_per_cluster
    n_cl = rng.poisson(cl_rate * T)
    cl_t = np.sort(rng.uniform(0.0, T, n_cl))
    names_per = rng.geometric(1.0 / mean_names, n_cl) if n_cl else np.empty(0, dtype=np.int64)
    gap = _lu(rng, 0.01, 0.3)
    n_names = int(names_per.sum())
    if n_names:
        g = rng.exponential(gap, n_names)
        starts = np.concatenate(([0], np.cumsum(names_per)[:-1]))
        g[starts] = 0.0
        cs = np.cumsum(g)
        cs -= np.repeat(cs[starts], names_per)
        name_t = np.repeat(cl_t, names_per) + cs
        name_cl = np.repeat(np.arange(n_cl), names_per)
        first_of_cl = np.zeros(n_names, dtype=bool)
        first_of_cl[starts] = True
    else:
        name_t = np.empty(0)
        name_cl = np.empty(0, dtype=np.int64)
        first_of_cl = np.empty(0, dtype=bool)
    # polls are clusters of their own (no follow-up traffic burst modelled)
    name_t = np.concatenate((name_t, poll_times))
    name_cl = np.concatenate((name_cl, np.full(len(poll_times), -1)))
    first_of_cl = np.concatenate((first_of_cl, np.zeros(len(poll_times), dtype=bool)))
    aaaa = rng.random(len(name_t)) < p_aaaa
    q_t = np.concatenate((name_t, name_t[aaaa] + rng.uniform(0.0001, 0.003, int(aaaa.sum()))))
    q_cl = np.concatenate((name_cl, np.full(int(aaaa.sum()), -1)))
    q_first = np.concatenate((first_of_cl, np.zeros(int(aaaa.sum()), dtype=bool)))
    q_forced_fail = np.zeros(len(q_t), dtype=bool)

    def outcome(t, forced_fail):
        n = len(t)
        p_no = np.full(n, base_noans)
        if bspan("dns.answered"):
            p_no = 1.0 - (1.0 - p_no) * (1.0 - benign[3] * _inspan(t, benign[1], benign[2]))
        if fault == "dns_down":
            p_no = 1.0 - (1.0 - p_no) * (1.0 - fmag * _inspan(t, fs, fe))
        answered = rng.random(n) >= p_no
        p_f = np.full(n, base_fail)
        if bspan("dns.failed"):
            p_f = 1.0 - (1.0 - p_f) * (1.0 - benign[3] * _inspan(t, benign[1], benign[2]))
        failed = (rng.random(n) < p_f) | forced_fail
        sf = np.zeros(n, dtype=bool)
        if fault == "servfail":
            sf = _inspan(t, fs, fe) & (rng.random(n) < fmag)
            failed |= sf
        hit = rng.random(n) < hit_p
        rtt = rtt_hit_ms * np.exp(rtt_sigma * rng.standard_normal(n)) * np.where(hit, 1.0, miss_mult)
        rtt = rtt + rng.exponential(jitter_ms, n)
        mult = np.ones(n)
        if fault in ("dns_slow", "subtle_latency"):
            mult = np.where(_inspan(t, fs, fe), fmag, 1.0)
        if bspan("dns.time"):
            mult = mult * np.where(_inspan(t, benign[1], benign[2]), benign[3], 1.0)
        if fall is not None and fall[0] == "dns.time":
            mult = mult * np.where(t >= WIN0, fall[1], 1.0)
        mult = mult * slow[np.clip(t.astype(np.int64), 0, T - 1)]
        rtt = rtt * mult
        rtt = np.where(sf, rtt * _lu(rng, 1.0, 5.0, n), rtt)
        rtt_s = np.clip(rtt / 1000.0, 0.0002, RTT_CLIP_S)
        return answered, failed & answered, rtt_s

    # benign stream of questions for a missing name (NXDOMAIN): part of a dns.failed rise
    if bspan("dns.failed") and rng.random() < 0.5:
        xt = _poisson_times(rng, _lu(rng, 0.03, 0.6) * _cov(benign[1], benign[2]))
        q_t = np.concatenate((q_t, xt))
        q_cl = np.concatenate((q_cl, np.full(len(xt), -1)))
        q_first = np.concatenate((q_first, np.zeros(len(xt), dtype=bool)))
        q_forced_fail = np.concatenate((q_forced_fail, rng.random(len(xt)) < 0.95))

    keep = q_t < T
    q_t, q_cl, q_first, q_forced_fail = q_t[keep], q_cl[keep], q_first[keep], q_forced_fail[keep]
    ans, fail, rtt_s = outcome(q_t, q_forced_fail)
    all_t, all_ans, all_fail, all_rtt, all_cl, all_first = [q_t], [ans], [fail], [rtt_s], [q_cl], [q_first]
    # client retries when the network gave no answer (capture loss never causes retries)
    cur_t, cur_ans = q_t, ans
    for _ in range(max_attempts - 1):
        rt = cur_t[~cur_ans] + timeout_s * rng.uniform(0.95, 1.05, int((~cur_ans).sum()))
        rt = rt[rt < T]
        if len(rt) == 0:
            break
        a2, f2, r2 = outcome(rt, np.zeros(len(rt), dtype=bool))
        all_t.append(rt); all_ans.append(a2); all_fail.append(f2); all_rtt.append(r2)
        all_cl.append(np.full(len(rt), -1)); all_first.append(np.zeros(len(rt), dtype=bool))
        cur_t, cur_ans = rt, a2
    q_t = np.concatenate(all_t)
    order = np.argsort(q_t, kind="stable")
    q_t = q_t[order]
    q_ans = np.concatenate(all_ans)[order]
    q_fail = np.concatenate(all_fail)[order]
    q_rtt = np.concatenate(all_rtt)[order]
    q_cl = np.concatenate(all_cl)[order]
    q_first = np.concatenate(all_first)[order]

    # ---- own unicast traffic ----------------------------------------------
    own_in_rate = own_rate * busy_own
    own_in_other = rng.poisson(own_in_rate).astype(np.int64)
    # page-load style bursts after a cluster's first name resolves
    sel = q_first & q_ans & (rng.random(len(q_t)) < 0.7)
    if sel.any():
        t_ans = q_t[sel] + q_rtt[sel]
        nb = np.round(_lu(rng, 3.0, 200.0, int(sel.sum()))).astype(np.int64)
        dur = rng.uniform(0.5, 4.0, len(nb))
        bt = np.repeat(t_ans, nb) + rng.random(int(nb.sum())) * np.repeat(dur, nb)
        own_in_other += _hist(bt).astype(np.int64)
    own_in_other += own_arp           # ARP replies to this device's requests
    own_out_other = rng.poisson(ack_ratio * own_in_other + own_out_base).astype(np.int64)

    # burst traffic in busy seconds (someone's bulk transfer / multicast storm, or our own)
    if (busy > 1.0).any():
        tot_rate = bg_rate + arp_rate + own_rate * (1.0 + ack_ratio) + flood_rate + own_out_base
        extra = rng.poisson((busy - 1.0) * tot_rate).astype(np.int64)
        if burst_target_own:
            own_in_other += extra
            own_out_other += rng.binomial(extra, ack_ratio)
        elif wifi:
            bc += extra
        else:
            flood += extra

    # ---- visibility -------------------------------------------------------
    # kinds: 0 own_in_other, 1 own_out_other, 2 own_arp(out), 3 arp_others, 4 dhcp_others,
    #        5 other bcast/mcast, 6 flooded unicast
    A = np.vstack([own_in_other, own_out_other, own_arp, arp_others, dhcp_others, bc, flood]).astype(np.int64)
    inbound_all = np.array([1, 0, 0, 1, 1, 1, 1], dtype=bool)
    inbound_uc = np.array([1, 0, 0, 0, 0, 0, 0], dtype=bool)
    block = (inbound_uc[:, None] & (oneway >= 1)[None, :]) | (inbound_all[:, None] & (oneway == 2)[None, :])
    A = np.where(block, 0, A)

    q_sec = np.minimum(q_t.astype(np.int64), T - 1)
    ta = q_t + q_rtt
    a_ok = q_ans & (ta < T)
    a_sec = np.minimum(ta.astype(np.int64), T - 1)
    a_arrive = a_ok & (oneway[a_sec] == 0)       # one-way visibility stops every frame to this device

    if burst_region.any():
        arr = A.sum(axis=0) + np.bincount(q_sec, minlength=T)[:T] + np.bincount(a_sec[a_arrive], minlength=T)[:T]
        for r0, r1 in ((0, 60), (60, T)):
            if not burst_region[r0:r1].any():
                continue
            seg = arr[r0:r1].astype(float)
            calm = seg[busy[r0:r1] <= 1.0]
            ref = max(1.0, np.median(calm) if len(calm) else np.median(seg))
            cap = rng.uniform(1.3, 2.5) * ref
            with np.errstate(divide="ignore", invalid="ignore"):
                fr = np.where(seg > 0, 1.0 - cap / np.maximum(seg, 1e-9), 0.0)
            fr = np.clip(fr, 0.0, 0.95)
            # every busy second drops something
            fr = np.where(busy[r0:r1] > 1.0, np.maximum(fr, rng.uniform(0.1, 0.3)), fr)
            f[r0:r1] = fr

    drop = rng.binomial(A, f[None, :])
    seen = (A - drop) * watched[None, :]

    q_drop = rng.random(len(q_t)) < f[q_sec]
    q_seen = (~q_drop) & watched[q_sec]
    a_drop = a_arrive & (rng.random(len(q_t)) < f[a_sec])
    a_seen = a_arrive & (~a_drop) & watched[a_sec]

    dns_q_seen = np.bincount(q_sec[q_seen], minlength=T)[:T]
    dns_a_seen = np.bincount(a_sec[a_seen], minlength=T)[:T]
    lost_sec = drop.sum(axis=0) + np.bincount(q_sec[q_drop], minlength=T)[:T] + np.bincount(a_sec[a_drop], minlength=T)[:T]

    own_in = seen[0] + dns_a_seen
    own_out = seen[1] + seen[2] + dns_q_seen
    arp = seen[2] + seen[3]
    dhcp = seen[4]
    frames = seen.sum(axis=0) + dns_q_seen + dns_a_seen

    # lost frames per 250 ms slot
    if burst_region.any():
        pv = rng.dirichlet(np.full(4, 0.6), T)
    else:
        pv = np.full((T, 4), 0.25)
    lost_slot = rng.multinomial(lost_sec.astype(np.int64), pv).reshape(NSLOT).astype(np.int64)

    # counter readings
    read_ok = np.ones(NSLOT, dtype=bool)
    p_rf = 0.30 if burst_region[60:].any() else 0.15
    if rng.random() < p_rf:
        u = rng.random()
        if u < 0.08:
            read_ok[240:] = False                        # counter unavailable for the whole window
        elif u < 0.12:
            read_ok[:] = False                           # unavailable for the whole run
        elif u < 0.30:
            read_ok[240:] &= rng.random(260) >= rng.uniform(0.01, 0.1)   # sporadic failures
            read_ok[240 + rng.integers(260)] = False
        else:
            for k in range(rng.integers(1, 4)):
                ln = int(min(260, max(1, round(_lu(rng, 1, 260)))))
                lo = 240 if k == 0 else 0
                a = int(rng.integers(lo, NSLOT - ln + 1))
                if k == 0 and a + ln <= 240:
                    a = 240
                read_ok[a:a + ln] = False
    if rng.random() < 0.06:                              # base-only failures
        ln = int(rng.integers(1, 120))
        a = int(rng.integers(0, 240 - ln + 1))
        read_ok[a:a + ln] = False

    # ---- qs ---------------------------------------------------------------
    qs = []
    idx = np.nonzero(q_seen & (q_t < WIN1))[0]
    for i in idx:
        t = float(q_t[i])
        lo, hi = int(t), int(t + 5.0)
        ww = bool(watched[lo:hi + 1].all())
        sa = bool(a_seen[i]) and q_rtt[i] <= 5.0
        rtt_us = int(round(q_rtt[i] * 1e6)) if q_ans[i] else -1
        qs.append((t, sa, bool(q_fail[i]) if q_ans[i] else False, rtt_us, ww, bool(q_ans[i])))

    # ---- ground truth -----------------------------------------------------
    bm = q_ans & (q_t < WIN0)
    wm = q_ans & (q_t >= WIN0) & (q_t < WIN1)
    material_slow = False
    if bm.any() and wm.any():
        mb = np.median(q_rtt[bm])
        mw = np.median(q_rtt[wm])
        material_slow = bool(mw >= 1.5 * mb and mw - mb >= 0.020)

    def art_of(kind):
        return {None: None, "uniform": "loss", "burst": "loss", "hold": "hold", "oneway": "oneway"}[kind]

    win = {
        "watched": watched,
        "frames": frames.astype(np.int64),
        "own_in": own_in.astype(np.int64),
        "own_in_dns": dns_a_seen.astype(np.int64),
        "own_out": own_out.astype(np.int64),
        "arp": arp.astype(np.int64),
        "dhcp": dhcp.astype(np.int64),
        "lost_slot": lost_slot,
        "read_ok": read_ok,
        "f": f.astype(np.float64),
        "qs": qs,
        "rose": rose,
        "fault": fault,
        "art": art_of(tw),
        "art_base": art_of(tb),
        "selective": bool(tw == "burst"),
        "material_slow": material_slow,
        "read_fail": bool((~read_ok[240:]).any()),
        "q": q,
    }
    truth = dict(site=site, wifi=wifi, fault_span=(fs, fe), fault_mag=fmag, benign=benign, fall=fall,
                 q_t=q_t, q_ans=q_ans, q_fail=q_fail, q_rtt=q_rtt, q_seen=q_seen, a_seen=a_seen,
                 arp_rt=arp_rt, arp_mean=arp_mean, sweep_mean=sweep_mean, own_arp_rate=own_arp_rate,
                 dhcp_ev_rate=ev_rate, extra_dhcp=extra_dhcp, oneway=oneway, ow_mode=ow_mode,
                 lost_sec=lost_sec, base_fail=base_fail)
    return win, truth


def _generate_with_truth(seed, n_windows):
    children = np.random.SeedSequence(int(seed)).spawn(int(n_windows))
    return [_window(np.random.default_rng(c)) for c in children]


def generate(seed: int, n_windows: int) -> list:
    """Deterministic for a given seed."""
    return [w for w, _ in _generate_with_truth(seed, n_windows)]
