# N1.2 controlled bench: method (frozen before the scored run)

The owner authorized a bounded, controlled validation of the frozen N1.1 engine on real protocol exchanges: an isolated local environment, with owned endpoints only. This page fixes the method **before** the scored sessions run. Changing it after any scored result would invalidate the run.

The engine and its decision values are the third freeze (`G0.md` §8.5, `ppc_intel.c` `d24c0753a8f6…`); nothing here changes them. The G0 criteria are unchanged. **These are bench results, reported apart from the synthetic G0 results. They are not field validation and not G3/G6 admission.**

## 1. The bench (`bench.py`)

- **Isolation.** Every endpoint is a Linux network namespace on the build host (WSL Ubuntu 22.04), joined by veth links to a bridge in a namespace of its own. No interface of the host's network namespace is used. Nothing reaches the PC's LAN, Wi-Fi or the internet.
- **Endpoints (all owned, all real protocol exchanges on the wire):**
  - the device (`dev0` on the LAN, captured; `dev1` a second link straight to the resolver);
  - a DNS resolver answering A questions, which can be slow, can drop questions or can answer SERVFAIL;
  - a DHCP server, which can stop offering;
  - six DHCP clients, which DISCOVER now and then and retry with back-off (4, 8, 8 … s) when no OFFER comes;
  - four peers sending ARP requests, on periodic cycles (6, 12, 15, 31 s) with short bursts, as Unit A's own traffic showed (`G0.md` §8.1), and storms;
  - bulk UDP to the device, in natural bursts ("busy") and in a blast.
- **The device's own traffic:**
  - DNS questions at 1 a second (Poisson), or 0.1 a second in the sparse sessions;
  - a ping a second to a peer, which gives unicast traffic back to the device.
- **The capture.** The real `pos-capture` (libpcap build from this branch, with the F1 identity switch, network namespace and seccomp filter) captures `dev0` with a physical-START equivalent: `start dev0 headers 0 all 3600` over its socketpair, as the shell sends it. HEADERS mode keeps the whole ARP, DHCP and DNS messages and only the headers of other frames.
- **Artifacts, made the way they happen:**
  - **Capture loss:** the helper is stopped (SIGSTOP) for 2.5–4 s while bulk traffic blasts the device. Its kernel ring overflows, and the drops are the kernel's own, read by the helper's counters.
  - **HOLD:** the helper's own `hold 1` / `hold 0`.
  - **Incomplete visibility:** the resolver's route to the device is switched to `dev1` for 20–35 s. The device still gets its answers, but the capture on `dev0` does not see them: an asymmetric return path.
- **What is kept:**
  - the helper's SAVE file (`capture.pcap`) and a sidecar of what the helper said (its state lines, its lost-frame readings once a second, shedding), with wall-clock times;
  - the endpoints' own logs (`truth-*.log`);
  - the event log.

  All of it stays outside Git, in `C:\K230-artifacts\n12-bench`. It contains only bench addresses (10.77.x, 10.78.x, locally administered MACs) and synthetic names (`hN.bench.test`).

## 2. The sessions (`scenarios.py`, fixed seed)

One comparison a session: base 0–60 s, mark at 60 s, window 60–120 s, STOP at 127 s. This is the held-out interface's framing and the engine's pair "after against before". 63 sessions (26 healthy, 37 with a fault):

| Group | Sessions |
| --- | --- |
| Healthy, periodic ARP | 4 |
| Healthy, busy (bulk bursts) | 4 |
| Healthy, sparse DNS (0.1/s) | 2 |
| Healthy, capture loss in the window | 4 |
| Healthy, HOLD in the window | 4 |
| Healthy, HOLD in the base | 2 |
| Healthy, capture loss in the base | 2 |
| Healthy, asymmetric return in the window | 4 |
| Each of 5 faults at the mark | 3 each (15) |
| Each fault starting inside the window (78–92 s) | 3 for ARP and DHCP, 2 for each DNS fault (12) |
| Each fault with capture loss in the window | 2 each (10) |

The faults, at onset:

| Fault | Change |
| --- | --- |
| ARP storm | +1.6 requests a second a peer, or +2.6 when it starts inside the window |
| DHCP trouble | The server stops offering |
| Resolver down | 90 % of questions dropped |
| Resolver slow | 120–240 ms instead of 10–30 ms |
| SERVFAIL | Half the answers |

## 3. Ground truth (`evaluate.py`), from the endpoints only

- **Materially worsened** (G0 §1's material changes, measured in the window against the base, from the endpoints' logs):
  - answered: at least 70 % of the window's questions dropped by the resolver;
  - answer time: the resolver's median delay at least 3 times the base's;
  - failures: SERVFAIL on at least 30 % of answers;
  - ARP: requests sent at least 5 times the base's;
  - DHCP: DISCOVERs sent at least 0.3 a second more.
- **Changed at all** (a claim of it is never false): answered or failure share up by more than 5 points; median delay over 1.5 times; ARP over 1.5 times; DISCOVERs over 0.1 a second more.
- **A false problem claim** is a WORSE verdict on a metric that did not change at all in the harmful direction. On the asymmetric-return sessions the resolver answered everything: a claim of fewer answers there is false.

## 4. What is reported, apart from the synthetic G0 results

- **Sample sizes:** windows per group and per category.
- **False problem claims per healthy category** (clean, loss-burst, HOLD, base artifact, asymmetric return), with the one-sided 95 % Wilson upper bound. Also counted: false claims on the other metrics of faulted windows.
- **Material faults:**
  - recall (WORSE on the faulted metric), with its lower bound, overall and under capture loss;
  - abstention (CAN'T TELL, with reasons);
  - false reassurance (NO CHANGE);
  - SMALL;
  - all of these per fault and by onset (at the mark, inside the window).
- **Not claimed:** field accuracy; G0 passing on bench data; G3/G6 admission. With 63 windows the bounds are wide: a category of 4 healthy windows with no false claim still has an upper bound near 50 %.
- **Kept as limitations:**
  - one host;
  - one link type (veth);
  - endpoints written for the bench;
  - no failed counter readings (the helper's counters cannot be made to fail from outside);
  - no Wi-Fi.

## 5. Frozen files

Frozen at the commit that adds this section, before any scored session ran. The trial session used to test the bench is not part of the scored set. Hashes are over LF line ends.

| File | SHA-256 |
| --- | --- |
| `bench.py` | `64ff00635be8…` |
| `bench_replay.c` | `d27085f0bc0a…` |
| `scenarios.py` | `ae330718e898…` |
| `evaluate.py` | `4eed1f0f26f9…` |
| `run_all.py` | `c9de53582c1c…` |

The engine under test is the third freeze (`G0.md` §8.5). The helper is `pos-capture` built with libpcap from the same commit.

## 6. The run (added after it; the method above is unchanged)

Run once on 28 November 2026, 15:59–16:37: 63 sessions, all saved, evaluated once with the frozen files (hashes checked before evaluating). Results: `results/bench_results.json`. Report and findings: `docs/apps/packetshark/neural/N1.2.md` §2–3. Replay parity, a separate check after the run that is not part of this method: `parity.py`, `results/parity.json`.
