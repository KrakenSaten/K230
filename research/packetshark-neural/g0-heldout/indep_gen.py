"""Independent synthetic window generator for the G0 held-out validation.

Written from G0.md sections 1, 2 and 4 and INTERFACE.md only (G0.md section 4,
step 2). It models one small LAN as seen by the capture on the device itself
(a switch port or a Wi-Fi station): the device's own unicast traffic in both
directions, plus the broadcast and multicast every port receives.

Public API:
    generate(seed, n_windows) -> list[dict]   (INTERFACE.md)
    check(windows)                             (asserts the consistency rules)

numpy only, deterministic per seed, no I/O.
"""

import heapq

import numpy as np

T = 125            # seconds 0-59 base, 60-119 window, 120-124 tail
SLOTS = 4 * T      # 250 ms counter slots
WIN = 60           # first second of the window minute
TAIL_END = 120     # questions at or after this are not listed

FAULTS = ("dns_down", "dns_slow", "servfail", "arp_storm", "dhcp_trouble",
          "subtle_latency")
MATERIAL = FAULTS[:5]
ARTS = ("loss", "hold", "oneway")
ROSE_NAMES = frozenset(("dns.time", "dns.answered", "dns.failed",
                        "dhcp.discover", "arp.request"))
FAULT_METRIC = {
    "dns_down": "dns.answered",
    "dns_slow": "dns.time",
    "servfail": "dns.failed",
    "arp_storm": "arp.request",
    "dhcp_trouble": "dhcp.discover",
    "subtle_latency": "dns.time",
}

# Ground truth that is not part of the interface, kept for check() only.
# Keyed by id(window); the window object itself is stored to guard against
# id reuse. Cleared on every generate() call.
_TRUTH = {}


# --------------------------------------------------------------------------
# helpers

def _logu(rng, lo, hi):
    return float(np.exp(rng.uniform(np.log(lo), np.log(hi))))


def _site(rng):
    """One site: DNS habits, resolver, background traffic levels."""
    s = {}
    s["q"] = _logu(rng, 1.0 / 60.0, 2.0)          # questions a second (1-120/min)
    s["cluster"] = rng.uniform(1.0, 4.0)          # mean questions per lookup group
    s["gap"] = rng.uniform(0.002, 0.05)           # mean spacing inside a group (s)
    s["rtt_med"] = _logu(rng, 0.002, 0.060)       # resolver median answer time (s)
    s["rtt_sigma"] = rng.uniform(0.15, 0.6)       # lognormal spread
    s["miss_p"] = rng.uniform(0.0, 0.4)           # share of answers needing recursion
    s["miss_mult"] = rng.uniform(2.0, 8.0)        # recursion cost multiplier
    s["fail_p"] = _logu(rng, 0.002, 0.05)         # NXDOMAIN etc. at baseline
    s["noans_p"] = _logu(rng, 0.0005, 0.02)       # baseline answers never sent
    s["retry_t"] = float(rng.choice([1.0, 2.0, 3.0, 5.0]))  # stub retry timeout
    s["retries"] = int(rng.integers(1, 3))
    s["own_in"] = _logu(rng, 0.2, 80.0)           # other unicast to device (fps)
    s["out_ratio"] = rng.uniform(0.3, 1.0)        # device's sends per frame received
    s["burstiness"] = rng.uniform(0.5, 4.0)       # gamma shape, per-second intensity
    s["own_arp"] = _logu(rng, 0.005, 0.05)        # device's own ARP requests
    s["arp"] = _logu(rng, 0.03, 3.0)              # other hosts' ARP requests
    s["dhcp"] = _logu(rng, 0.0005, 0.03) if rng.random() < 0.7 else 0.0
    s["bcast"] = _logu(rng, 0.2, 30.0)            # mDNS, SSDP, NetBIOS, ...
    s["flood"] = _logu(rng, 0.1, 5.0) if rng.random() < 0.3 else 0.0
    return s


def _pick_art(rng, p):
    if rng.random() >= p:
        return None, None
    r = rng.random()
    if r < 0.5:
        return "loss", ("burst" if rng.random() < 0.45 else "uniform")
    if r < 0.75:
        return "hold", None
    return "oneway", None


def _span(rng, lo, hi, min_len, full_p):
    """A contiguous span inside the minute starting at lo; a span that reaches
    the minute's end runs to hi (the window artifact covers 120-124 too)."""
    if rng.random() < full_p:
        return lo, hi
    length = int(rng.integers(min_len, 61))
    start = lo + int(rng.integers(0, 60 - min_len + 1))
    end = start + length
    if end >= lo + 60:
        end = hi
    return start, end


# --------------------------------------------------------------------------
# one window

def _window(rng):
    site = _site(rng)

    # ---- fault (window minute and the 5 s tail) --------------------------
    fault = None
    fp = {}
    if rng.random() < 0.5:
        fault = FAULTS[int(rng.integers(len(FAULTS)))]
    if fault == "dns_down":
        fp["p_down"] = rng.uniform(0.7, 1.0)
    elif fault == "dns_slow":
        fp["k"] = _logu(rng, 3.0, 8.0)
    elif fault == "subtle_latency":
        fp["k"] = rng.uniform(1.3, 1.8)
    elif fault == "servfail":
        fp["p_sf"] = rng.uniform(0.3, 1.0)
    elif fault == "arp_storm":
        fp["arp_k"] = _logu(rng, 5.0, 40.0)
    elif fault == "dhcp_trouble":
        fp["dhcp_add"] = _logu(rng, 0.3, 2.0)

    # ---- benign harmful-direction changes (labelled in rose) -------------
    rose = set()
    act = 1.0
    r = rng.random()
    if r < 0.05:                       # user goes idle: answered rate falls
        act = rng.uniform(0.3, 0.7)
        rose.add("dns.answered")
    elif r < 0.10:                     # user gets busy: failure rate rises too
        act = rng.uniform(1.5, 3.0)
        rose.add("dns.failed")
    arp_mult = 1.0
    if rng.random() < 0.08:            # a host scans or a device joins
        arp_mult = rng.uniform(1.5, 4.0)
        rose.add("arp.request")
    dhcp_benign = 0.0
    if rng.random() < 0.06:            # a phone joins or roams, below material
        dhcp_benign = rng.uniform(0.01, 0.15)
        rose.add("dhcp.discover")
    nx_add = 0.0
    if rng.random() < 0.06:            # typos, blocked names: NXDOMAIN rises
        nx_add = rng.uniform(0.05, 0.2)
        rose.add("dns.failed")

    # ---- visibility artifacts per minute ----------------------------------
    art_w, kind_w = _pick_art(rng, 0.5)
    art_b, kind_b = _pick_art(rng, 0.3)

    watched = np.ones(T, dtype=bool)
    hidden = np.zeros(T, dtype=bool)      # frames to the device never reach capture
    p = np.zeros(T)                       # kernel drop probability per second
    busy = {}                             # second -> burst parameters
    oneway_onset = {}

    for art, kind, lo, hi in ((art_b, kind_b, 0, WIN), (art_w, kind_w, WIN, T)):
        if art == "hold":
            a, b = _span(rng, lo, hi, 3, 0.15)
            watched[a:b] = False
        elif art == "oneway":
            if lo == WIN and art_b == "oneway":
                a = WIN                     # the base's one-way view persists
            elif rng.random() < 0.4:
                a = lo
            else:
                a = lo + int(rng.integers(1, 56))   # begins inside the minute
            hidden[a:hi] = True
            oneway_onset[lo] = a
        elif art == "loss" and kind == "uniform":
            a, b = _span(rng, lo, hi, 10, 0.7)
            p[a:b] = _logu(rng, 0.001, 0.6)
        elif art == "loss" and kind == "burst":
            nb = int(rng.integers(1, 5))
            for _ in range(nb):
                length = int(rng.integers(1, 7))
                a = lo + int(rng.integers(0, hi - lo - length + 1))
                prm = {
                    "drop": rng.uniform(0.15, 0.9),
                    "rate": _logu(rng, 200.0, 3000.0),
                    "kind": "own_in" if rng.random() < 0.6 else "mcast",
                    "slow": rng.uniform(2.0, 6.0),
                    "add": rng.uniform(0.02, 0.3),
                }
                for s in range(a, a + length):
                    busy[s] = prm
                    p[s] = prm["drop"]
    p[~watched] = 0.0   # on HOLD the capture takes nothing, so drops nothing

    # ---- counter readings --------------------------------------------------
    read_ok = np.ones(SLOTS, dtype=bool)
    if rng.random() < 0.15:
        a = int(rng.integers(4 * WIN, SLOTS))
        read_ok[a:a + int(rng.integers(4, 241))] = False
    if rng.random() < 0.08:
        a = int(rng.integers(0, 4 * WIN))
        read_ok[a:min(4 * WIN, a + int(rng.integers(4, 241)))] = False
    read_ok &= rng.random(SLOTS) >= 1e-4      # rare isolated failures

    # ---- DNS questions and the network's answers ---------------------------
    starts = []
    for lo, hi, rate in ((0, WIN, site["q"]), (WIN, T, site["q"] * act)):
        n_groups = rng.poisson(rate / site["cluster"] * (hi - lo))
        for t0 in rng.uniform(lo, hi, n_groups):
            k = int(rng.geometric(1.0 / site["cluster"]))
            offs = np.concatenate(([0.0], np.cumsum(rng.exponential(site["gap"], k - 1))))
            for o in offs:
                if t0 + o < T:
                    starts.append((float(t0 + o), 0))
    heapq.heapify(starts)

    allq = []          # every question the device sent (seen or not)
    slowed_window = False
    while starts:
        tq, attempt = heapq.heappop(starts)
        win = tq >= WIN
        answered = rng.random() >= site["noans_p"]
        if win and fault == "dns_down":
            answered = answered and rng.random() >= fp["p_down"]
        rtt = site["rtt_med"] * float(np.exp(site["rtt_sigma"] * rng.standard_normal()))
        if rng.random() < site["miss_p"]:
            rtt *= site["miss_mult"]
        if win and fault in ("dns_slow", "subtle_latency"):
            rtt *= fp["k"]
        s = int(tq)
        if s in busy:                       # queueing on a saturated link
            rtt = rtt * busy[s]["slow"] + busy[s]["add"]
            if answered and WIN <= tq < TAIL_END:
                slowed_window = True
        rtt = min(rtt, 30.0)
        pf = site["fail_p"]
        if win:
            pf = 1.0 - (1.0 - pf) * (1.0 - nx_add)
            if fault == "servfail":
                pf = 1.0 - (1.0 - pf) * (1.0 - fp["p_sf"])
        failed = answered and rng.random() < pf
        allq.append({"tq": tq, "answered": answered, "rtt": rtt, "failed": failed})
        if attempt < site["retries"] and (not answered or rtt > site["retry_t"]):
            tr = tq + site["retry_t"]
            if tr < T:
                heapq.heappush(starts, (tr, attempt + 1))

    if slowed_window:
        rose.add("dns.time")
    if fault is not None:
        rose.discard(FAULT_METRIC[fault])

    lost_slot = np.zeros(SLOTS, dtype=np.int64)
    lost_sec = np.zeros(T, dtype=np.int64)
    arr_sec = np.zeros(T, dtype=np.int64)    # frames that reached the capture
    dns_q_seen = np.zeros(T, dtype=np.int64)
    dns_a_seen = np.zeros(T, dtype=np.int64)

    def timed_frame(t):
        """One frame with an exact arrival time at the capture (watched,
        not hidden). Returns True if seen, False if the kernel dropped it."""
        s = int(t)
        arr_sec[s] += 1
        if p[s] > 0.0 and rng.random() < p[s]:
            lost_sec[s] += 1
            lost_slot[min(SLOTS - 1, int(t * 4))] += 1
            return False
        return True

    qs = []
    for q in allq:
        tq = q["tq"]
        s = int(tq)
        q_seen = bool(watched[s]) and timed_frame(tq)
        q["seen"] = q_seen
        if q_seen:
            dns_q_seen[s] += 1
        a_seen = False
        if q["answered"]:
            ta = tq + q["rtt"]
            if ta < T:
                sa = int(ta)
                if watched[sa] and not hidden[sa]:
                    a_seen = timed_frame(ta)
                    if a_seen:
                        dns_a_seen[sa] += 1
        q["a_seen"] = a_seen
        seen_answer = a_seen and q["rtt"] <= 5.0
        if q_seen and tq < TAIL_END:
            ww = bool(watched[s:int(tq + 5) + 1].all())
            qs.append((float(tq), bool(seen_answer), bool(q["failed"]),
                       int(round(q["rtt"] * 1e6)), ww, bool(q["answered"])))
    qs.sort(key=lambda x: x[0])

    # ---- counted background categories -------------------------------------
    in_mask = watched & ~hidden
    sec = np.arange(T)
    winm = sec >= WIN
    mult = rng.gamma(site["burstiness"], 1.0 / site["burstiness"], T)
    bmult = rng.gamma(2.0, 0.5, T)

    arp_rate = np.full(T, site["arp"])
    arp_rate[winm] *= arp_mult
    if fault == "arp_storm":
        arp_rate[winm] *= fp["arp_k"]
    dhcp_rate = np.full(T, site["dhcp"])
    dhcp_rate[winm] += dhcp_benign
    if fault == "dhcp_trouble":
        dhcp_rate[winm] += fp["dhcp_add"]
    burst_own = np.zeros(T)
    burst_mc = np.zeros(T)
    for s, prm in busy.items():
        if prm["kind"] == "own_in":
            burst_own[s] = prm["rate"]
        else:
            burst_mc[s] = prm["rate"]

    rates = {
        "own_in": (site["own_in"] * mult + burst_own, True),
        "own_out": (site["own_in"] * site["out_ratio"] * mult, False),
        "own_arp": (np.full(T, site["own_arp"]), False),
        "arp": (arp_rate, True),
        "dhcp": (dhcp_rate, True),
        "bcast": (site["bcast"] * bmult + burst_mc, True),
        "flood": (np.full(T, site["flood"]), True),
    }
    seen = {}
    truth_counts = {}
    for name, (rate, incoming) in rates.items():
        n = rng.poisson(rate)
        mask = in_mask if incoming else watched
        n = np.where(mask, n, 0)
        d = rng.binomial(n, p)
        seen[name] = n - d
        arr_sec += n
        lost_sec += d
        truth_counts[name] = (n, d)
        for s in np.nonzero(d)[0]:
            lost_slot[4 * s:4 * s + 4] += rng.multinomial(int(d[s]), [0.25] * 4)

    own_in = seen["own_in"] + dns_a_seen
    own_out = seen["own_out"] + seen["own_arp"] + dns_q_seen
    arp = seen["arp"] + seen["own_arp"]
    dhcp = seen["dhcp"]
    frames = (own_in + own_out + seen["arp"] + dhcp + seen["bcast"]
              + seen["flood"])

    f = np.where(arr_sec > 0, lost_sec / np.maximum(arr_sec, 1), p).astype(float)

    # ---- truth flags --------------------------------------------------------
    rb = [q["rtt"] for q in allq if q["answered"] and q["tq"] < WIN]
    rw = [q["rtt"] for q in allq if q["answered"] and WIN <= q["tq"] < TAIL_END]
    material_slow = False
    if rb and rw:
        mb, mw = float(np.median(rb)), float(np.median(rw))
        material_slow = mw >= 1.5 * mb and mw - mb >= 0.020

    w = {
        "watched": watched,
        "frames": frames.astype(np.int64),
        "own_in": own_in.astype(np.int64),
        "own_in_dns": dns_a_seen.astype(np.int64),
        "own_out": own_out.astype(np.int64),
        "arp": arp.astype(np.int64),
        "dhcp": dhcp.astype(np.int64),
        "lost_slot": lost_slot,
        "read_ok": read_ok,
        "f": f,
        "qs": qs,
        "rose": rose,
        "fault": fault,
        "art": art_w,
        "art_base": art_b,
        "selective": art_w == "loss" and kind_w == "burst",
        "material_slow": bool(material_slow),
        "read_fail": bool((~read_ok[4 * WIN:]).any()),
        "q": float(site["q"]),
    }
    truth = {
        "hidden": hidden, "p": p, "lost_sec": lost_sec, "arr_sec": arr_sec,
        "allq": allq, "counts": truth_counts, "seen_own_arp": seen["own_arp"],
        "oneway_onset": oneway_onset, "kind": (kind_b, kind_w), "site": site,
        "fp": fp, "busy": busy,
    }
    return w, truth


def generate(seed: int, n_windows: int) -> list:
    """Deterministic for a given seed. Window i depends only on (seed, i)."""
    _TRUTH.clear()
    out = []
    for i in range(n_windows):
        rng = np.random.default_rng([int(seed), i])
        w, truth = _window(rng)
        _TRUTH[id(w)] = (w, truth)
        out.append(w)
    return out


# --------------------------------------------------------------------------
# self-check

def _minute_slices(lo):
    return (slice(0, WIN), slice(0, 4 * WIN)) if lo == 0 else \
        (slice(WIN, T), slice(4 * WIN, SLOTS))


def check(windows):
    """Assert INTERFACE.md's consistency rules. Rules that need ground truth
    beyond the interface (one-way onset, true arrivals, all answers) are
    checked when the windows came from the latest generate() call."""
    counts = ("frames", "own_in", "own_in_dns", "own_out", "arp", "dhcp")
    for i, w in enumerate(windows):
        tag = "window %d" % i
        watched = np.asarray(w["watched"])
        assert watched.shape == (T,) and watched.dtype == bool, tag
        for k in counts:
            a = np.asarray(w[k])
            assert a.shape == (T,) and np.issubdtype(a.dtype, np.integer), (tag, k)
            assert (a >= 0).all(), (tag, k)
            assert (a[~watched] == 0).all(), (tag, k, "HOLD seconds show nothing")
        fr, oi, oid, oo = w["frames"], w["own_in"], w["own_in_dns"], w["own_out"]
        assert (oid <= oi).all(), tag
        assert (fr >= oi + oo).all(), tag
        assert (fr >= oi + w["arp"] + w["dhcp"]).all(), tag
        ls = np.asarray(w["lost_slot"])
        ro = np.asarray(w["read_ok"])
        assert ls.shape == (SLOTS,) and (ls >= 0).all(), tag
        assert ro.shape == (SLOTS,) and ro.dtype == bool, tag
        f = np.asarray(w["f"], dtype=float)
        assert f.shape == (T,) and ((f >= 0) & (f <= 1)).all(), tag
        lsec = ls.reshape(T, 4).sum(axis=1)
        assert (f[lsec > 0] > 0).all(), (tag, "drops imply f > 0")
        assert w["read_fail"] == bool((~ro[4 * WIN:]).any()), tag
        assert w["fault"] is None or w["fault"] in FAULTS, tag
        assert w["art"] is None or w["art"] in ARTS, tag
        assert w["art_base"] is None or w["art_base"] in ARTS, tag
        assert isinstance(w["rose"], set) and w["rose"] <= ROSE_NAMES, tag
        if w["fault"] is not None:
            assert FAULT_METRIC[w["fault"]] not in w["rose"], tag
        assert not w["selective"] or w["art"] == "loss", tag
        assert isinstance(w["q"], float) and w["q"] > 0, tag
        # artifacts are the only source of HOLD and of kernel drops
        for lo, art in ((0, w["art_base"]), (WIN, w["art"])):
            ssl, ssl4 = _minute_slices(lo)
            if (~watched[ssl]).any():
                assert art == "hold", (tag, lo, "unwatched only on HOLD")
            if art == "hold":
                assert (~watched[ssl]).any(), (tag, lo)
            if ls[ssl4].sum() > 0:
                assert art == "loss", (tag, lo, "drops only under loss")
        # questions
        prev = -1.0
        per_sec_q = np.zeros(T, dtype=np.int64)
        per_sec_a = np.zeros(T, dtype=np.int64)
        for qt in w["qs"]:
            tq, sa, failed, rtt_us, ww, abn = qt
            assert len(qt) == 6, tag
            assert 0.0 <= tq < TAIL_END and tq >= prev, (tag, "qs order/range")
            prev = tq
            s = int(tq)
            assert watched[s], (tag, "question seen in a HOLD second")
            per_sec_q[s] += 1
            assert ww == bool(watched[s:int(tq + 5) + 1].all()), (tag, "wait_watched")
            assert isinstance(rtt_us, int) and rtt_us > 0, tag
            if sa:
                assert abn and rtt_us <= 5_000_000, (tag, "seen answer")
                sa_sec = int(tq + rtt_us / 1e6)
                if sa_sec < T:
                    # microsecond rounding may cross a second boundary
                    assert watched[max(0, sa_sec - 1):sa_sec + 2].any(), tag
                    per_sec_a[sa_sec] += 1
        assert (per_sec_q <= oo).all(), (tag, "questions within own_out")
        # rounding of rtt_us can move an answer across a second boundary;
        # compare cumulatively with one second of slack
        assert (np.cumsum(per_sec_a)[:-1] <= np.cumsum(oid)[1:]).all(), \
            (tag, "seen answers within own_in_dns")

        entry = _TRUTH.get(id(w))
        if entry is None or entry[0] is not w:
            continue
        t = entry[1]
        hidden = t["hidden"]
        # lost frames are never seen, and the counter totals match the truth
        assert (lsec == t["lost_sec"]).all(), tag
        for name, (n, d) in t["counts"].items():
            assert (d <= n).all(), (tag, name)
        # one-way stops every frame to the device
        if hidden.any():
            assert (oi[hidden] == 0).all() and (oid[hidden] == 0).all(), tag
            assert (w["dhcp"][hidden] == 0).all(), tag
            assert (w["arp"][hidden] == t["seen_own_arp"][hidden]).all(), tag
            assert (fr[hidden] == oo[hidden]).all(), tag
        for lo, art in ((0, w["art_base"]), (WIN, w["art"])):
            ssl, _ = _minute_slices(lo)
            assert hidden[ssl].any() == (art == "oneway"), (tag, lo)
        # qs is exactly the seen questions before 120 s
        want = [q for q in t["allq"] if q["seen"] and q["tq"] < TAIL_END]
        assert len(want) == len(w["qs"]), (tag, "qs complete")
        for q in t["allq"]:
            if q["seen"]:
                assert watched[int(q["tq"])], tag
            if q["a_seen"]:
                ta = q["tq"] + q["rtt"]
                assert q["answered"] and watched[int(ta)] and not hidden[int(ta)], tag
        # material_slow from every answer the network sent
        rb = [q["rtt"] for q in t["allq"] if q["answered"] and q["tq"] < WIN]
        rw = [q["rtt"] for q in t["allq"]
              if q["answered"] and WIN <= q["tq"] < TAIL_END]
        ms = False
        if rb and rw:
            mb, mw = float(np.median(rb)), float(np.median(rw))
            ms = mw >= 1.5 * mb and mw - mb >= 0.020
        assert ms == w["material_slow"], (tag, "material_slow")

    if len(windows) >= 200:
        n = len(windows)
        nf = sum(w["fault"] is not None for w in windows)
        na = sum(w["art"] is not None for w in windows)
        assert 0.35 * n <= nf <= 0.65 * n, ("fault share", nf, n)
        assert 0.35 * n <= na <= 0.65 * n, ("artifact share", na, n)
    return True


def summary(windows):
    """Counts by fault, artifact and flags (for GENERATOR.md and the report)."""
    from collections import Counter
    out = {
        "fault": Counter(str(w["fault"]) for w in windows),
        "art": Counter(str(w["art"]) + ("/burst" if w["selective"] else
                                        "/uniform" if w["art"] == "loss" else "")
                       for w in windows),
        "art_base": Counter(str(w["art_base"]) for w in windows),
        "fault_x_art": Counter((w["fault"] is not None, w["art"] is not None)
                               for w in windows),
        "read_fail": sum(w["read_fail"] for w in windows),
        "material_slow": sum(w["material_slow"] for w in windows),
        "rose": Counter(r for w in windows for r in w["rose"]),
        "material_with_loss": sum(w["fault"] in MATERIAL and w["art"] == "loss"
                                  for w in windows),
        "q_per_min": np.percentile([60 * w["q"] for w in windows],
                                   [0, 25, 50, 75, 100]).round(1).tolist(),
    }
    return out


if __name__ == "__main__":
    ws = generate(2026, 500)
    check(ws)
    again = generate(2026, 500)
    assert all(
        all(np.array_equal(a[k], b[k]) if isinstance(a[k], np.ndarray) else a[k] == b[k]
            for k in a)
        for a, b in zip(ws, again)), "not deterministic"
    for k, v in summary(again).items():
        print(k, dict(v) if hasattr(v, "items") else v)
