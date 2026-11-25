# PacketShark NEURAL — the shared intelligence engine

**Architecture proposal for owner review · 24 November 2026 · nothing here is implemented.** It serves the three lenses of [NEURAL.md](../NEURAL.md) (WHEN, WHO, WHY) from one engine. Evidence classes follow `AGENTS.md`. Measurements are in [EXPERIMENTS.md](EXPERIMENTS.md); budgets and gates are in [ROADMAP.md](ROADMAP.md).

The engine has one rule above the others: **the engine records what was seen, how much of it was seen, and what it rests on, before it says what it means.** Every later layer reads that record. Nothing writes around it.

## 1. Where it lives

```
 doors-shell (LVGL)                          pos-capture (one per START, no capabilities)
  apps/packetshark                socketpair   on_frame() ── ppc_decode ── ppc_store (HEADERS rule)
   psk_model  ◄── ledger.* lines ◄─────────►     │
   lens sheets (WHEN/WHO/WHY)                     ├─ ppc_signals, ppc_net            (exist: M7, M11.1, M12.1)
                                                  ├─ L1 observers  (new, per frame, allocation-free)
                                                  ├─ L2 ledger     (new, fixed rings, session-local)
                                                  ├─ L3 analysts   (new, CPU, 2 Hz / on mark / on demand)
                                                  └─ L4 scorer client ── socketpair ──► pos-infer (optional)
                                                                                         numeric tensors only
                                                                                         backends: nncase | ref | fake
```

- **L1 to L3 run inside `pos-capture`**, in C, under the same rules as `ppc_signals` and `ppc_net`: one allocation at START, bounded probes, no text from packets in logs, fuzzed, no LVGL (`tests/packetshark_lint.sh`). The hook is `on_frame()` in `tools/capture/pos_capture.c`, beside `ppc_net_frame()`, which already receives the whole frame before the HEADERS rule cuts it (VERIFIED in source, lines 590–607 at `448327d`).
- **L4 runs outside `pos-capture`** in an optional helper, `pos-infer`. `pos-capture` links only `$(PPC_LIBS)`, and `tests/vision_lint.sh` keeps every nncase header in one file of Vision. Putting the runtime into the capture helper would put the closed KPU runtime next to the hostile-packet decoder. The scorer receives only bounded numeric tensors. It has no capture descriptor, no packet bytes, no names, no files beyond its model and no network.
- **The shell stays a viewport.** It asks for ledger pages the way it asks for `net` lists today and renders them. It never computes a finding.

## 2. L1 — observers

Each observer reads the decoded record (and, where it already does today, the plumbing message) and updates one fixed table. The table sizes are proposals sized to fit the memory budget in §8.

| Observer | Reads | Keeps | Size (proposal) | Serves |
|---|---|---|---|---|
| **Series** | per-second deltas of the existing counters and `ppc_diag` sums, plus its own per-second sketches of DNS and handshake times | 48 features × u16 per second, plus coverage per second (drops, shed, HOLD, one-way share) | 3,600 s ring ≈ 360 KiB | WHEN, WHY |
| **Conversations** | IP pair + protocol + ports of the unit's own conversations | counters, first/last time, handshake RTT, the 192-byte DNA sketch of the first 32 packets (R3 layout) | 1,024 × 256 B = 256 KiB | WHO (own flows), WHY |
| **Name binding** | DNS answers already stored under HEADERS | address → name, the answer's packet sequence number, TTL, how many names share the address | 1,024 × 96 B = 96 KiB | WHO, WHY |
| **Presence** | DHCP options (55, 60, 12, 61), DHCPv6 ORO/DUID, mDNS service types, LLMNR/NBNS names, ARP | per device: a plumbing fingerprint string, the announcement times (last 32), service words | 256 × 384 B = 96 KiB | WHO |
| **Rhythm** | event times per conversation and per announcing device | last 32 event times (u32 ms) | shared with the two tables above | WHO, WHEN |

What the observers do not do:

- They do not keep any byte the HEADERS rule would drop. A conversation's DNA is lengths, directions, flags and times. TLS ClientHello fields, JA4 strings and other payload-derived data stay out unless the owner changes ADR-016 rule 7 (decision D2 in [HANDOVER.md](HANDOVER.md)).
- They do not observe while frames pass unseen. HOLD, shedding and a full table make them count what they skipped, as `ppc_signals` already does.
- Their clock is the packet's relative time (`rel_ns`), not the arrival time of the helper's loop. Today's bins follow the monotonic loop clock (`ppc_stats_roll` in `pos_capture.c`), which a replay cannot reproduce. The engine's series must be keyed by packet time, so that a live session and a replay of its saved file give the same ledger (gate G5).

## 3. L2 — the evidence ledger

A ledger is a set of fixed rings in the helper's memory. It exists for one session and is gone at DISCARD, NEW CAPTURE, leaving the app or a crash (ADR-016 rule 8).

| Record | Fields | Ring |
|---|---|---|
| **Observation** | id; kind; interval [t0, t1]; value; numerator and denominator; comparison window; coverage of the interval (0–1000 ‰); up to 4 packet sequence numbers; source (rule or model id and version) | 512 |
| **Episode** | id; kind; first and last time; count; strongest observation; state OPEN / STILL SEEN / NOT SEEN FOR N MIN | 64 |
| **Mark** | id; time; kind USER / LAP / LINK (carrier change) / AUTO (a suggestion, never a claim); the pre-trigger window it opened | 16 |
| **Claim** | id; hypothesis; verdict OK / MARGINAL / PROBLEM / CAN'T TELL; supporting and contrary observation ids; likelihood ratio to the runner-up; abstention reason; the observation that would settle it ("To be sure") | 32 |
| **Salience** | id; what a scorer said (an embedding cluster, a novelty rank, a name-shape score); the model id and version; never a verdict | 128 |

About 90 KiB in all. The rules that make it trustworthy:

1. **A claim needs a deterministic witness.** A claim may cite salience, but it is accepted only when at least one observation from a rule or statistic supports the same wording. A model can say "look here" or "these look alike". It cannot say "the resolver is slow". This is the **two-key rule**: the sheet shows which key turned.
2. **Coverage travels with every number.** An observation over an interval with known capture loss, HOLD or one-way visibility carries that coverage. The analysts treat a bin below 98 % coverage as no evidence. In E3a, rules without this gate raised alerts on 76 % of capture-drop windows and 90 % of one-way windows; with it, on 2–4 % ([EXPERIMENTS.md](EXPERIMENTS.md), E3a).
3. **Several numbers from the same pairs count once.** A median, a p95 and a model score computed from the same DNS pairs are one source of evidence, not three.
4. **Expiry is visible.** An observation whose packets have left the ring keeps its sequence numbers and says "packet expired". A tap never lands on a newer packet instead.
5. **Nothing in the ledger is logged or written.** SAVE writes the pcap as today. Saving an evidence card beside it is a separate retention decision (D5).

## 4. L3 — analysts (CPU)

The analysts run in the helper's loop, twice a second while capturing, at a mark, and when a lens asks. They write observations and claims. Everything here is a few thousand operations per second (R2, ASSUMED from operation counts; to be measured in the line-rate gate).

| Analyst | Method | Writes |
|---|---|---|
| **Visibility gate** | per interval: coverage from kernel drops, shed and HOLD; directional coverage from one-way conversations; the vantage (switch port, Wi-Fi, mirror) | the `CAN'T TELL` reasons every other analyst must honour |
| **Before/after** (WHEN) | at a mark: the 60 s before against the reference before that, and the following seconds as they arrive. Robust medians (median/MAD), exact Poisson and binomial tests on rates normalised by their volume, Benjamini–Hochberg across features. One-sided where the harmful direction is known (latency up, unanswered share up) | observations with numerator, denominator and both windows |
| **Change finder** (WHEN) | CUSUM / Page–Hinkley on invariant ratios (answers/queries, SYN-ACK/SYN, retransmissions/segments, ARP replies/requests); PELT around a mark for "when did it start"; thresholds from a false-alert budget per observed hour | episodes |
| **Rhythm** (WHO, WHEN) | per source: median inter-arrival, MAD, the share of intervals near a multiple of the period (tolerates missed events); claims a period only above the event count E3c requires | observations "every 30 s ± 2 s, 14 times" |
| **Doctor** (WHY) | a small hypothesis graph: local link, address (DHCP), resolver, upstream path, a destination, no network evidence. Each hypothesis declares its signature, its counter-evidence and its minimum counts. Noisy-OR log-likelihood ratios; accept the leader only at LR ≥ 10 over the runner-up, otherwise show both and the discriminating observation | claims |
| **Integrity** (WHY) | header-only checks over the unit's own connections: identical SYN-ACK option sets and hop counts across unrelated servers (a terminating proxy), MSS clamping, a second DHCP server, the router's address at a new MAC, NXDOMAIN answered with an address | observations and claims |
| **Exposure** (WHO) | broadcast name queries others could answer: LLMNR, NBNS, WPAD | observations |

Learned fusion is not an analyst in v1. E3a shows why: a logistic regression trained on five fault types learned a **negative** weight for the SERVFAIL rule and then missed every SERVFAIL. A model trained on a fault catalogue can silently suppress evidence for a fault outside it. If learned weighting is admitted later (gate G3), its weights must be non-negative on every rule, and it may only reorder accepted claims.

## 5. L4 — accelerated scorers (optional)

A scorer turns a batch of fixed-size numeric tensors into embeddings or scores. The engine works without one. Two candidates have a workload large enough to matter (E1, E2):

| Scorer | Input | Output | MACs per call | Compiler's KPU estimate | Status |
|---|---|---|---|---|---|
| **DNA encoder** | 64 conversations × [3, 32] (signed length, IAT code, mask), float32 | 64 × 32-d unit vectors | 13.3 M | 0.10 ms (AveragePool variant) | candidate for the first KPU proof of concept |
| **Name-shape scorer** | 32 names × 64 characters, one-hot over 40 symbols | 32 scores | 65.6 M | 0.23 ms | needs decision D1 (names as local model inputs) |

The compiler's estimate is its own roofline model at 800 MHz and excludes the runtime's call, copy and cache-sync overhead, which no source documents (R1). It is not a measurement.

**The contract.**

- `pos-capture` starts `pos-infer` only when the lens that needs it is open, the KPU lock is free and the owner has enabled the feature. It uses the same spawn shape it is spawned with: socketpair, `PR_SET_PDEATHSIG`, every inherited descriptor closed, capabilities dropped and `no_new_privs` after the model file and the device are open, nice 10. This makes the capture helper the parent of a process for the first time. That is a change to ADR-016's shape and is listed as decision D4.
- Frames on the socketpair are length-prefixed binary, at most 32 KiB, with a session id, a request sequence and a model id. The 1,024-byte text protocol is not stretched for tensors. One request is pending at most. A newer request replaces a pending one, and a late or mismatched answer is dropped.
- Inputs are numbers the observers computed: lengths, codes, masks. No address, port, MAC or name enters a tensor, except a name for the name-shape scorer if D1 allows it, and then only as one-hot characters with no other field.
- **The KPU has one user at a time.** The `/dev/k230-gnne` driver has no arbitration between processes: its mutex is declared and never used, and user space programs the registers (R1, VERIFIED in the driver source). `pos-infer` takes an exclusive `flock` on a runtime-directory lock that Vision's helper must take too. If the lock is held, the engine uses the reference backend or skips the scorer and says so. Vision's helper runs only while Vision is open (DOCUMENTED, `docs/apps/VISION.md`), so the two rarely compete, but a hung helper must not be able to share the device.
- **CMA.** The MMZ driver frees nothing at process exit (`docs/vision/KPU_CMA_LEAK.md`). `pos-infer` releases with `shrink_memory_pool()` and `kd_mpi_mmz_deinit()` on every exit path it controls, as `pos-vision` does after its fix. A SIGKILL still strands its pool. The proof of concept must measure how large that pool is for a 30 KB model with 25 KB tensors. On Unit A (K1, 25 November 2026) each bench process made 4–5 explicit CMA allocations of 19–21 pages (76–84 kB) in total, against Vision's 45 MB. What a SIGKILL strands is still unmeasured (P7).
- **The same weights run three ways.** Every scorer is built only from operators E1 placed entirely on the KPU: Conv2D with height 1, ReLU, MaxPool/AveragePool, Add, Concat, constant Gemm, Sigmoid. That small operator set lets `pos-infer` carry a portable C reference backend (`ref`) with the same weights, a `fake` backend for host tests (Vision's pattern), and the `nncase` backend. Golden vectors check that nncase agrees with the simulator and with `ref` on every build.
- **Generic names.** The public words are `infer`, `scorer`, `embedding`. No KPU or K230 name appears in the protocol or the app (`AGENTS.md`).

## 6. Presentation: the line protocol

Protocol 6 would add, in the style of M12.1's `net`:

- `ledger <req> <what> <from> <count>`: a page of episodes, claims or marks.
- `mark <kind>`: a user mark or a lap. It starts nothing, captures nothing new and sends nothing.
- Events `ledger.begin`/`ledger.*`/`ledger.end` (queued whole, under 8 KiB, like `net`), and `intel` (a summary line of at most 40 fields, once a second with `diag`: open episodes, claims by verdict, coverage, vantage).

Packet-derived text keeps today's sanitising rules. The shell adds one LIVE row and an overview face (see [NEURAL.md](../NEURAL.md#5-navigation)).

## 7. Replay

The file source already exists (`pos-capture session --source file:PATH[:PPS]`). Three gaps keep it from driving the engine (R6):

1. It replays as fast as the loop runs or at a fixed packet rate. There is no recorded-time mode.
2. The expiry clock is real time measured from the file's first stamp, not the file's own time.
3. Direction is lost: a classic pcap carries no inbound/outbound flag, and the file source reports every frame as UNKNOWN.

Proposal: a **virtual clock** mode, in which the engine's time is the packet's time and late decisions fire on packet time. Replays then run as fast as the CPU allows, give the same ledger every time, and can serve as regression fixtures on the host. Direction comes from the capturing interface's MAC. PacketShark's SAVE could record it in a pcapng interface block, or the replay could infer it from the dominant unicast MAC (ASSUMED reliable for a single-host capture). Opening a saved capture on the unit is a separate decision (D3), because today the app lists and deletes saved files and never opens them (ADR-016 rule 10, `tests/packetshark_lint.sh`).

## 8. Budgets

| Resource | Today (measured) | Engine budget (proposal) | Gate |
|---|---|---|---|
| Helper heap | ≈ 22.5 MiB allocated at START; peak RSS 23.2 MB on Unit A (`PACKETSHARK.md` §9.2) | + ≤ 1.5 MiB (tables in §2–3 ≈ 1 MiB) | total under ADR-016's 32 MiB bound with the libpcap ring |
| Helper CPU | 0.9 % of the hart at 2,000 synthetic pps on Unit B (§9.8) | + ≤ 1 % at 2,000 pps; ≤ 150 ns per frame in `pos-capture bench` | no new kernel drops at the line-rate gate |
| Shell | 4.0 % at 2,000 pps (§9.8) | + ≤ 0.5 %; no frame over the existing worst | unchanged frame budget |
| `pos-infer` | K1 bench on Unit A: peak RSS 5.1 MiB; 76–84 kB of explicit CMA per process | RSS ≤ 12 MiB; CMA ≤ 4 MiB held; ≤ 1 call per second while a lens is open | PoC (ROADMAP G4) |
| KPU call | K1 on Unit A: 132.4 µs p95 idle, 221.4 µs p95 under a CPU hog, 190.6 µs p95 at 1 call per second | p95 ≤ 2 ms end to end for one 64-conversation batch, including copies and sync | PoC: passed |
| Energy | — | ≤ 3 % above capture alone with the scorer active | PoC, if a power meter is available |

## 9. What does not change

- Receive-only. No observer, analyst or scorer sends, probes, scans, resolves or injects anything (ADR-016 rule 2). The "do this now" prompts ask the person to act. The unit never acts.
- HEADERS stays the default. FULL stays a confirmed per-session choice. Nothing is computed from payload without decision D2.
- Session-local. No baseline, profile, embedding or identifier survives the session without decision D5.
- Nothing leaves the unit. Companion keeps its allowlist. A new aggregate (for example "1 open episode") needs review under ADR-016 amendment 1 before it is proposed (D7).
- No log line carries an address, a name or a byte of a packet.
