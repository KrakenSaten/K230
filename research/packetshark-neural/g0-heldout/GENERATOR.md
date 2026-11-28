# G0 held-out: the independent generator

`indep_gen.py` is the second generator of [G0.md](../../../docs/apps/packetshark/neural/G0.md) §4, step 2. It was written from G0.md §1, §2 and §4 and [INTERFACE.md](INTERFACE.md) only. The author did not read the engine, E3a-R, the N1.1 record, G0.md §3 or §5, the engine's tests, or `heldout.py`.

`generate(seed, n_windows)` returns the windows INTERFACE.md defines. Window `i` depends only on `(seed, i)`. `check(windows)` asserts the consistency rules, and `python3 indep_gen.py` runs `check` on `generate(2026, 500)`, confirms that a second run gives the same output, and prints the counts.

## Model

The capture runs on the device itself, so it sees the device's own unicast frames in both directions plus the broadcast and multicast every port receives. Each window draws its own site.

| Part | Choice |
| --- | --- |
| DNS questions | Lookup groups arrive as a Poisson process; each group holds a geometric number of questions (mean 1–4, closely spaced, like A and AAAA lookups for a page load). The mean rate `q` is log-uniform from 1 to 120 questions a minute. |
| Stub retries | When no answer arrives within the site's timeout (1, 2, 3 or 5 s), the device sends 1–2 retries. Retries are questions too. Capture loss never causes a retry, because the device still receives the answer. |
| Resolver | The median answer time is log-uniform from 2 to 60 ms, with a lognormal spread (σ 0.15–0.6) and a share of answers that need recursion (0–40 %, 2–8 times slower). Baseline error answers (NXDOMAIN and similar) 0.2–5 %. Baseline answers never sent 0.05–2 %. |
| Background | Other unicast traffic to the device at 0.2–80 frames a second, with the device's replies at 0.3–1 times that rate. Both share a per-second gamma intensity. The device sends its own ARP requests at 0.005–0.05 a second. Other hosts send ARP at 0.03–3 a second, and 70 % of sites have DHCP DISCOVERs at 0.0005–0.03 a second. Other broadcast and multicast runs at 0.2–30 a second, and 30 % of sites have flooded unknown unicast at 0.1–5 a second. |

### Faults (50 % of windows, uniform over six kinds, seconds 60–124)

| Fault | Magnitude |
| --- | --- |
| `dns_down` | Each answer goes missing with p 0.7–1.0, on top of the baseline. |
| `dns_slow` | Every answer time × 3–8 (log-uniform). |
| `servfail` | Error answers on top of the baseline with p 0.3–1.0. |
| `arp_storm` | Other hosts' ARP rate × 5–40. |
| `dhcp_trouble` | DISCOVER rate + 0.3 to 2 a second. |
| `subtle_latency` | Every answer time × 1.3–1.8. This fault is not material. |

Each fault changes only its own metric. Retries under `dns_down` also raise the number of questions.

### Benign changes, labelled in `rose`

These are drawn independently of the fault. A label is dropped when the fault's own metric covers it.

| Change | Probability | Label |
| --- | --- | --- |
| The user goes idle: question rate × 0.3–0.7 | 5 % | `dns.answered` (the answered rate falls) |
| The user gets busy: question rate × 1.5–3 | 5 % | `dns.failed` (the failure rate rises with it) |
| A real ARP rise × 1.5–4 | 8 % | `arp.request` |
| A device joins: + 0.01–0.15 DISCOVER a second | 6 % | `dhcp.discover` |
| More NXDOMAIN: + 5–20 % error answers | 6 % | `dns.failed` |
| A burst-loss slowdown touching at least one answered question in seconds 60–119 | when it occurs | `dns.time` |

### Artifacts (window minute 50 %, base minute 30 %, independently)

The window artifact covers seconds 60–124. Of the artifacts drawn, half are loss, a quarter HOLD and a quarter one-way.

| Artifact | Shape |
| --- | --- |
| Uniform loss | Constant drop probability, log-uniform 0.1–60 %. It covers the whole minute in 70 % of cases, otherwise a span of 10–60 s. |
| Burst loss (45 % of loss, `selective`) | 1–4 busy spells of 1–6 s. Each carries 200–3000 extra frames a second, either a download to the device or a multicast flood, and drops 15–90 % of every frame in those seconds. DNS questions sent in a busy second wait longer: answer time × 2–6 plus 20–300 ms. |
| HOLD | One span of 3–60 s, or the whole minute (15 %). Nothing is seen or counted. The capture takes no frames, so it drops none. |
| One-way | From an onset, which is the minute's start (40 %) or a second inside it, to the minute's end, no frame addressed to the device reaches the capture: unicast, broadcast, ARP and DHCP alike. Only the device's own sends are seen. A base one-way that is followed by a window one-way continues from second 60. |
| Counter failures | Independent of the artifacts. A spell of 1–60 s of failed readings in seconds 60–124 (15 %), a spell in the base (8 %), and isolated failures (p 10⁻⁴ per slot). |

Kernel drops are decided per frame with the drop probability of its arrival second, whatever its kind. Frames with exact times (DNS questions and answers) land in their own 250 ms slot. Counted frames are spread evenly over the second's four slots. `f` is the realised dropped share of the frames that reached the capture in that second, or the drop probability when no frame arrived. `material_slow` compares the medians of every answer the network sent to questions in 0–59 s and in 60–119 s, including late answers and answers that were not seen.

## Where it goes beyond or interprets INTERFACE.md

- `rtt_us` is filled for unanswered questions too, with the latent draw, and is meaningless there. `failed` is False when the network sent no answer.
- Answers that arrive later than 5 s are counted in `own_in_dns` but have `seen_answer` False.
- Arrays are numpy arrays (`bool`, `int64` and `float64`), not lists. `qs` holds tuples of Python scalars.
- A minute has at most one artifact. Loss and HOLD never share a minute, and a counter-failure spell is not an `art`.
- The `rose` labels are generous. `dns.time` is labelled when at least one answer was slowed, even if the median did not move.
- `subtle_latency` is a fault, so its `dns.time` change is not in `rose`.

## Limits

- Faults are constant from second 60 and never ramp. The base minute never holds a fault.
- With a fast local resolver (2–7 ms), a ≥ 3× `dns_slow` can stay below the 20 ms floor of `material_slow`. On sparse sites, a minute with no answers gives `material_slow` False.
- Sparse sites (1–3 questions a minute) often have no DNS evidence in one of the minutes, which makes DNS faults invisible there.
- Background traffic is stationary apart from per-second gamma noise. There are no diurnal changes, TCP dynamics or retransmissions.
- The model has no IPv6, no ND, no other DNS transports (DoH, DoT, mDNS answers) and no Wi-Fi retries or power-save effects.
- Burst-loss slowdowns are tied to the second a question is sent, not to the second its answer arrives.
- `check` verifies one-way onsets, true arrivals and `material_slow` only on windows from the latest `generate()` call. It keeps that ground truth in a module-level table and not in the windows.
