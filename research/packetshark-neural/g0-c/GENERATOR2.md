# indep_gen2: independent G0 window generator

Written from `G0-DEFINITIONS.md` and `INTERFACE.md` only. No engine, generator,
fixture or result was read. `generate(seed, n_windows)` returns windows in the
INTERFACE.md format. Each window draws from its own `SeedSequence([seed, i])`,
so a window depends only on the seed and its index. Requires numpy and nothing
else. Arrays are numpy (`bool`, `int64`, `float64`), `qs` holds plain-Python
tuples and `rose` is a `set`.

## What the capture sees

The device sits on a switch port or is a Wi-Fi station without monitor mode, so it sees:

- **its own unicast traffic**: app keep-alives, pushes and sync, plus occasional
  downloads that show up as 1-8 s bursts;
- **its own DNS questions and the answers to them**;
- **others' broadcast and multicast**: mDNS, SSDP, ND/RA and the like, ARP
  requests and DHCP DISCOVERs;
- **a little flooded unknown unicast** at busier sites.

It does not see other hosts' unicast.

Every stream keeps its **true mean rate** and gets its burstiness from a
mean-preserving modulation (a per-second gamma factor on a Poisson rate). ARP
comes in sweeps: a compound Poisson process with geometric cluster sizes. DNS
questions come in clusters, such as A+AAAA pairs and page loads, with a
geometric size of mean 1.2-3.5. Ordinary minute-to-minute burstiness therefore
never changes a true rate, and `rose` stays exact.

## Site mix (per window)

| Parameter | Sparse (35 %) | Medium (40 %) | Busy (25 %) |
| --- | --- | --- | --- |
| others' bcast/mcast /s | 0.2-4 | 3-30 | 20-250 |
| others' ARP requests /s | 0.02-0.4 | 0.3-3 | 2-25 |
| DHCP DISCOVER /s | 0.0005-0.01 | 0.004-0.04 | 0.02-0.2 |
| flooded unicast /s | 0 | 0.01-0.5 | 0.2-5 |

Device parameters are drawn independently of the site class:

- **DNS question rate `q`**: light 0.01-0.1 /s (40 %), moderate 0.1-0.8 /s
  (40 %), heavy 0.8-3 /s (20 %). The window minute varies by a log-normal
  factor (sigma 0.35), and in 15 % of windows also by an activity change of
  0.2-5x. Neither is a metric change.
- **Own traffic**: own unicast in 0.2-10 /s, out 0.5-1.1x that.
- **Own ARP requests**: 0.003-0.05 /s.
- **Resolver**: LAN 1.5-15 ms, ISP 10-50 ms or distant 40-150 ms median. The
  answer time is log-normal (sigma 0.15-0.6), and 0-35 % of answers are cache
  misses at 2-12x.
- **Base health**: 0-3 % of questions unanswered and 0-8 % error answers
  (NXDOMAIN and the like).
- **Device resolver**: a 2, 3 or 5 s timeout with 0-2 retries. The device
  retries only when it did not receive an answer in time.
- **Capture capacity**: 600-3000 frames/s, used only in burst-loss seconds.

## Faults (50 %, window minute plus tail, seconds 60-124)

| Fault | Share | Magnitude | Notes |
| --- | --- | --- | --- |
| `dns_down` | 9 % | 75-100 % of questions get no answer | Retries follow. |
| `dns_slow` | 9 % | 3.2-15x answer time | Answers past 5 s are not "seen within 5 s". |
| `servfail` | 9 % | 32-100 % of answers SERVFAIL | Combined with the base error rate. |
| `arp_storm` | 9 % | 5.5-40x others' ARP rate | Burstier. |
| `dhcp_trouble` | 9 % | +0.32-3 DISCOVER/s | Clients retrying. |
| `subtle_latency` | 5 % | 1.3-1.8x answer time | Not material. |

The lower bounds sit just above the G0 §1 thresholds, so every material fault
is material by its parameter.

## Benign level changes and `rose`

Each minute independently has a 20 % chance of one benign change, applied over
the whole minute or over a 15-60 s block. The change and its magnitude:

| Metric | Benign change |
| --- | --- |
| ARP | 1.5-8x |
| DHCP DISCOVERs | +0.03-0.4 /s |
| DNS answer time | 1.3-2.5x |
| DNS errors | +5-30 % |
| Unanswered questions | +5-40 % |

A window-minute change is a harmful rise and is listed in `rose`. A base-minute
change makes the window look better and is not listed.

Two artifact side effects are real and are also listed:

- `dns.time`, when burst-loss congestion slowed a larger share of the window's
  answers than of the base's;
- `arp.request`, when the device itself really lost its receive path (see
  one-way) and re-probed its neighbours.

The fault's own metric is never listed in `rose`.

## Visibility artifacts

The window minute has an artifact in 50 % of windows and the base minute in
35 %, drawn independently. The artifact kind, given that a minute has one:

| Kind | Share |
| --- | --- |
| uniform loss | 26 % |
| burst loss | 22 % |
| HOLD | 20 % |
| one-way | 16 % |
| counters only | 16 % |

A further 15 % of the other artifacts also get a counter-failure spell.

- **Placement**: the episode covers the whole region, runs from or to its
  edge, is one block of any length, or is 2-5 short pieces. The window region
  is 60-124, so the tail can be affected. When both minutes have the same
  HOLD, one-way or uniform kind, 30 % share one episode straddling second 60.
- **Uniform loss**: one level of 0.1-60 % (log-uniform) with ±25 % slot
  jitter, clipped to 0.1-60 %. Every frame is dropped with that probability
  whatever its kind.
- **Burst loss** (`selective`): 1-6 bursts of 1-8 busy seconds inside 0-59 or
  60-119. Each busy second carries a download of 1.3-6x the capture capacity,
  split unevenly over its four slots. Frames of every kind in a slot drop with
  probability 1 - capacity/arrivals. Questions asked, or answered, in a busy
  second gain 20-250 ms x sqrt(load) of queueing delay. Busy seconds that stay
  under capacity, with no loss and no delay, also appear in 35 % of the other
  minutes.
- **HOLD**: frames pass unseen, uncounted and undropped.
- **One-way**: no frame addressed or delivered to the device reaches the
  capture: unicast, broadcast, ARP, DHCP or DNS answers. Its own sends are
  still seen. In 60 % of cases the loss is on the capture side only and the
  device is unaffected. In 40 % the device's receive path really failed. Then
  the device retries DNS, retransmits (1-2x own out) and, after 2-20 s, sends
  0.3-1 ARP probes/s, which `rose` then lists. The network still answered DNS,
  so `answered_by_network` stays true.
- **Counters failed**: spells of failed readings: whole window, one block,
  intermittent flapping (20-80 % of slots) or several short spells.

## Consistency

- **Drop decision**: kernel drops are decided per frame from the drop
  probability of its arrival slot, whether watched or not.
- **What a second counts**: `frames` = `own_in` + `own_out` + others'
  broadcast and multicast + flooded unicast. `arp` includes the device's own
  ARP, which is part of `own_out`.
- **qs**: holds only questions that were watched and not dropped, with
  `tq` < 120.
- **seen_answer**: requires an answer that the network sent within 5 s and
  that was not dropped, not on HOLD and not one-way.
- **rtt_us**: the true time, including retries and unseen questions; 0 when
  the network never answered.
- **f**: the true drop share of the second, that is, the slot drop
  probability weighted by arrivals.
- **material_slow**: computed from every question the network answered, seen
  or not: base 0-59 against window 60-119.

## Assumptions where the definitions leave room

1. A clean minute has no kernel drops at all, and loss never overlaps HOLD.
2. Faults hold constant from second 60 to 124 and do not ramp.
3. `rose` uses true parameters, not realised counts. Any harmful parameter
   change in the window minute is listed, however small.
4. `art` labels any artifact touching seconds 60-124, the tail included. A
   counters-only minute has `art` None and shows in `read_fail`.
5. `subtle_latency` may make `material_slow` true at fast resolvers, because
   1.8x and +20 ms is reachable. The flag is computed, not forced.
