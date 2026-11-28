# indep_gen3: model, parameters and assumptions

Written from `G0-DEFINITIONS.md` and `INTERFACE.md` only. `generate(seed, n)` spawns one
`numpy.random.SeedSequence` child per window, so window *i* depends only on `(seed, i)`
(a run of 20 windows is a prefix of a run of 2,400). numpy only. About 2 s per 2,400 windows.

## 1. What the capture sees

One device at a switch port (40 %) or as a Wi-Fi station (60 %). It sees its own unicast traffic
in and out, every broadcast and multicast frame, and at a switch port a little flooded unknown
unicast (1-20 % of the broadcast rate). Rates are drawn log-uniform per window from a site class
(25 % each):

| Site | Others' bcast/mcast /s | ARP req (others) /s | DHCP DISCOVER /s | Own unicast in /s | DNS questions /s (`q`) |
| --- | --- | --- | --- | --- | --- |
| sparse | 0.2-4 | 0.01-0.25 | 0.0003-0.01 | 0.1-15 | 0.01-0.5 |
| home | 2-30 | 0.05-1.5 | 0.001-0.03 | 1-80 | 0.03-1.5 |
| office | 15-150 | 0.5-6 | 0.005-0.1 | 3-200 | 0.08-2.5 |
| busy | 60-600 | 2-30 | 0.02-0.4 | 5-400 | 0.15-4 |

Timing structure (all stationary, so the base and window minutes share the same true rates):

- Broadcast and own traffic are Poisson, modulated by a slow log-normal AR(1) "busy-ness"
  (sigma 0.15-0.7, rho 0.6-0.95, mean 1). 0-3 periodic announcers (SSDP/mDNS style) add bursts of
  3-15 frames every 20-120 s.
- ARP: Poisson; 20 % of sites have a periodic sweep (4-20 requests every 30-90 s). The device
  itself sends ARP requests at 0.005-0.05/s (counted in `arp` and `own_out`; replies in `own_in`).
- DHCP DISCOVERs: Poisson joins; 30 % are repeated once 1-4 s later.
- DNS: clusters (page loads) of a geometric number of names (mean 1-4), names 10-300 ms apart, an
  AAAA follows the A in 30-90 % of names (0.1-3 ms later); 40 % of devices also poll a name every
  15-120 s. 70 % of clusters whose first name resolves trigger a traffic burst of 3-200 frames over
  0.5-4 s (outbound ACKs at 30-80 % of inbound).
- Resolver: cache hit (50-95 %) median 1.5-40 ms, miss 3-15 times slower, log-normal sigma
  0.15-0.5, plus link jitter (Wi-Fi 0.3-4 ms, wired 0.05-0.4 ms). Unanswered base 0.05-1 %, error
  answers (NXDOMAIN) 0.2-6 %. Answer time clipped at 4.8 s. The client retries an unanswered
  question after 1, 2, 2.5, 3 or 5 s, 2 or 3 attempts in all. Capture loss never causes retries.

## 2. Faults (window minute only; 50 % of windows)

Each fault covers a fraction c of the window minute (0.90-1.0 for DNS faults, 0.85-1.0 for
ARP/DHCP), starting anywhere that fits, and continues through the tail if it reaches second 119.
Magnitudes are scaled by c so that the window-minute effect meets G0 §1:

| Fault | Share | Magnitude in its span |
| --- | --- | --- |
| dns_down | 8.5 % | answers withheld with p = U(max(0.90, 0.80/c), 1) |
| dns_slow | 8.5 % | answer time x 4-25 (log-uniform) |
| servfail | 8.5 % | SERVFAIL on p = U(max(0.40, 0.36/c), 1) of answers, which also take 1-5 times longer |
| arp_storm | 8.5 % | ARP rate x k, k log-uniform in [1 + 4.4/c, 60] of the mean (sweeps and own ARP included) |
| dhcp_trouble | 8.5 % | +0.35-3.0 DISCOVERs/s divided by c |
| subtle_latency | 7.5 % | answer time x 1.3-1.8 (not material) |

## 3. Benign changes and `rose`

- 15 % of windows get one real benign rise in the window minute (start 60-100 s, 15-60 s long),
  never on the fault's metric: ARP +0.4-6 times the mean (a device joins or scans), DISCOVERs
  +0.02-0.4/s (people arrive), answer time x 1.3-3 (cold cache), error share +5-35 % (half the time
  with an extra stream of NXDOMAIN questions), unanswered share +5-45 %. Listed in `rose`.
- 8 % of the others get a benign fall (ARP, DHCP or answer time x 0.3-0.85); not listed.
- Burst loss comes with congestion that slows answers (section 4). When the window minute has a
  larger share of slowed seconds than the base, `dns.time` is in `rose` (unless the fault is a DNS
  timing fault).

## 4. Visibility artifacts

Window-minute type (seconds 60-124): none 56 %, uniform loss 15 %, burst loss 11 %, HOLD 10 %,
one-way 8 %. Base minute: none 70 %, 10 / 7 / 7 / 6 %. In 35 % of artifact windows the same
episode crosses the boundary (starts at 5-58 s, ends at 62-125 s). One type per minute.

- **HOLD** (app in the background, screen off): one hold of 1-60 s, 2-5 short holds of 1-8 s,
  or the whole minute. Nothing is counted while held.
- **Uniform loss** (CPU contention, small socket buffer): whole minute, one span of 3-55 s or 2-4
  spans of 2-15 s; level log-uniform 0.1-60 %, flat, ramping up or down, or wavering. Each frame
  of every kind is dropped with that probability.
- **Burst loss** (`selective`): 1-7 busy episodes of 1-8 s with traffic x 3-15 (our own bulk
  transfer, or someone's flood/multicast storm). The capture handles 1.3-2.5 times the calm-second
  median, so f = 1 - capacity/arrivals (at least 10-30 % in busy seconds, at most 95 %). Questions
  asked in busy seconds are answered 1.5-6 times slower, so the lost answers are the slow ones.
  Drops are spread unevenly over the four 250 ms slots.
- **One-way** (half: only unicast to the device disappears, a receive-path quirk; half: every
  inbound frame, a mirror of the device's transmit side only): whole minute, from a moment to the
  end, or a 5-49 s span. The device still gets its answers in reality (no retries).
- **Counters failed** (independent of `art`): 15 % of windows (30 % with burst loss in the window)
  have failed readings in seconds 60-124: 1-3 spells of 1-260 slots, sporadic failures (1-10 % of
  slots), the whole window, or the whole run; 6 % have base-only spells.

## 5. Assumptions where the definitions leave room

1. Material thresholds are applied to the window minute as a whole (scaled by span coverage), at
   the level of true rates; realised counts in sparse windows can fall short by chance.
2. "SERVFAIL on ≥ 30 % of answers" is the share of window-minute answers.
3. `rose` holds only changes without a fault and never the fault's own metric; a subtle_latency
   window's slowdown is a fault, not a rise. Stationary Poisson noise and periodic sweeps are not
   true rate changes.
4. `f` is the per-second drop probability (model truth), not the realised share.
5. `rtt_us` is -1 when the resolver sent no answer; `failed` is False then.
6. `material_slow` uses every answered question (seen or not, retries included) asked in 0-59 vs
   60-119.
7. Answers to questions asked at 120-124 are counted in `own_in_dns` but those questions are not in
   `qs`. Answers to questions the capture missed still count in `own_in_dns`.
8. `art`/`art_base` classify seconds 60-124 / 0-59; a HOLD only in the tail is `hold`. `selective`
   refers to the window minute.
9. Kernel drops are counted in `lost_slot` whether watched or not; HOLD never overlaps loss in this
   model, so lost frames during HOLD are zero.
10. Answers never take longer than 4.8 s, so no late answer arrives after the 5 s wait.
