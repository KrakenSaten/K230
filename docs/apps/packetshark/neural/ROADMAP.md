# PacketShark NEURAL: proof of concept, gates and roadmap

**Proposal for owner review, 24 November 2026; K1 result added 25 November 2026.** Nothing on this page has been authorized except the K1 bench, which ran on Unit A without P7 (§1). Each phase that touches a unit needs its own approval. Budgets are in [ENGINE.md](ENGINE.md#8-budgets); evidence for the numbers is in [EXPERIMENTS.md](EXPERIMENTS.md).

## 1. The first KPU proof of concept

### Why this model first

The DNA encoder (`dna-b64`) is the only candidate that meets all four conditions:

- **Large enough for the KPU to matter.** One call is 13.3 M MACs. A per-window anomaly model is 0.7 M, which the CPU handles easily.
- **Built only from KPU operators.** E1 placed the AveragePool variant entirely on the KPU, with no CPU fallback.
- **Needs no owner decision about inputs.** Its inputs are lengths, directions and times, not names.
- **Useful without labels.** Resemblance within one capture needs no class names.

The name-shape scorer is larger (65.6 M MACs) but waits on decision D1.

### What it does not try to prove

- Whether conversation resemblance is useful to people. That is gate G2 work.
- Accuracy on real traffic. That is gate G3 work, on a consented corpus.

The proof of concept answers one question: **does a tiny batched model on this KPU cost less than the same model on the C908, end to end, without leaving memory behind?**

### Setup

Unit B is preferred, because Unit A is the owner's daily unit. The bench needs separate authorization. (On 25 November 2026 the owner authorized it on Unit A without P7; see the result below.)

- **Kit.** A non-persistent directory in `/tmp` holds the bench binary, the model and the inputs:
  - the bench binary `infer-bench`;
  - `dna_b64_u8.kmodel` and `dna_b64_a16w8.kmodel` from the research branch (each about 27–30 KB);
  - 8 calibration batches and 32 test batches as raw float32 files;
  - the simulator's expected outputs.
- **Nothing on the unit changes.** Nothing is installed or started at boot, and no service is touched. No app runs, so no capture happens and Vision is closed. The kit is removed afterwards.
- **Bench program.** It drives the model through the host-tensor API, with no AI2D. The pattern follows the vendor's keyword-spotting demo (R1):
  1. `interpreter::load_model`;
  2. `hrt::create(dt_float32, {1,3,64,32}, hrt::pool_shared)`;
  3. map, copy, `hrt::sync(sync_write_back)`;
  4. `run()`;
  5. read the output through map;
  6. release with `shrink_memory_pool()` and `kd_mpi_mmz_deinit()`.

  The same file carries a C reference of the same network, so both paths read the same inputs.

### Measurements

| # | Measurement | How |
| --- | --- | --- |
| P1 | Load time, cold and warm | wall clock around `load_model` and the first `run` |
| P2 | Per-call latency, p50/p95/p99 over 1,000 calls, split into copy in, sync, run, copy out | `CLOCK_MONOTONIC` around each step |
| P3 | Per-operator time on the CPU side | `interpreter::set_profiling(1)` and `op_profile::print()` |
| P4 | The same batch through the C reference on the CPU | same inputs, same clock |
| P5 | Agreement: device vs simulator vs C reference | cosine per conversation, and agreement on the nearest neighbour |
| P6 | RSS and CMA before, during and after a normal exit | `/proc/self/status`; `CmaFree` after `sync; echo 1 > /proc/sys/vm/drop_caches`, as in [KPU_CMA_LEAK.md](../../../vision/KPU_CMA_LEAK.md) |
| P7 | CMA lost after SIGKILL mid-run, three times | same reading; idle readings between runs give the noise |
| P8 | Under load: P2 and P4 again while a CPU hog at nice 0 imitates the shell and a capture | `taskset` is not needed on one hart; record `/proc/loadavg` |
| P9 | Duty cycle: 1 call a second for 10 minutes; RSS and CMA drift | readings every minute |
| P10 | Exclusivity: a second instance must refuse while the first holds the lock | exit code and message |

### GO / NO-GO

| Criterion | GO when |
| --- | --- |
| End-to-end p95 per 64-conversation batch (P2) | ≤ 2 ms, and ≤ 25 % of the C reference's p95 (P4) |
| Under load (P8) | p95 ≤ 4 ms; the C reference slows at least as much |
| Agreement (P5) | device vs simulator cosine ≥ 0.999 per conversation; nearest neighbour identical for ≥ 99 % |
| Memory after a normal exit (P6) | `CmaFree` back to idle within the measured idle noise; RSS ≤ 12 MiB during the run |
| SIGKILL (P7) | loss measured and ≤ 4 MiB per kill. A larger loss rules out the KPU for anything that runs during a capture, until the driver is fixed |
| Duty cycle (P9) | no growth of RSS or CMA beyond noise over 10 minutes |
| Exclusivity (P10) | the second instance refuses and leaves the first unaffected |

**If GO:** build `pos-infer` (phase K2) with the nncase, reference and fake backends.

**If NO-GO on latency or memory:** conversation resemblance runs on the C reference. It then works at a lower rate, for example every 5 s, and only while the WHO lens is open. The KPU stays with Vision. This is an acceptable outcome. The engine was designed to be complete without a scorer.

### Result on Unit A, 25 November 2026

The owner authorized a non-destructive run on Unit A, because Unit B was not connected. **P7 was skipped by decision.** Nothing was installed, rebooted or reconfigured, and Unit A was left as found. The measurements, method and limits are in the research report [`poc/K1-UNIT-A.md`](https://github.com/KrakenSaten/K230/blob/9b790069/research/packetshark-neural/poc/K1-UNIT-A.md). The raw output is beside it. The independent review brief is [K1-REVIEW.md](K1-REVIEW.md). All values below are VERIFIED measurements on Unit A for `dna_b64` u8 unless stated.

| Criterion | GO when | Measured | Verdict |
| --- | --- | --- | --- |
| End to end (P2, P4) | p95 ≤ 2 ms and ≤ 25 % of the C reference's p95 | **132.4 µs** p95 (123.6 µs p50, 174 µs p99, 1,000 calls). The C reference: 269.9 ms p95, so the KPU path takes 0.05 % of it. a16w8: 173.4 µs p95. | **PASS** |
| Under load (P8) | p95 ≤ 4 ms; the C reference slows at least as much | KPU **221.4 µs** p95 (×1.7). C reference 925.0 ms p95 (×3.4). The p99 of 2.2 ms is consistent with one scheduler slice lost to the hog. The run step's p50 moved only from 109 to 117 µs. | **PASS** |
| Agreement (P5) | device vs simulator cosine ≥ 0.999; nearest neighbour identical ≥ 99 % | Device output **byte-identical** to the simulator for u8 and a16w8, 2,048 conversations each; nearest neighbour 100 %. | **PASS** |
| Memory after normal exit (P6) | `CmaFree` back to idle within measured noise; RSS ≤ 12 MiB | RSS peak 5.1 MiB. After cleanup `CmaFree` was 2.86 MB *above* the as-found value. The meter's resolution is about 1–3 MB, set by the kernel's per-CPU page lists, so a loss under about 1 MB per run is neither shown nor excluded. | **PASS** at the available resolution |
| SIGKILL (P7) | loss measured and ≤ 4 MiB per kill | not run | **PENDING** |
| Duty cycle (P9) | no RSS or CMA growth beyond noise in 10 minutes | 600 calls at 1 per second. RSS flat at 4,480 kB; `CmaFree` flat from minute 2 to 10. Latency 190.6 µs p95. | **PASS** |
| Exclusivity (P10) | the second instance refuses; the first is unaffected | The second instance got `EAGAIN` and exited with code 3, with no CMA allocation. The holder completed all 600 calls with byte-identical output. | **PASS** |

**G4: not passed yet.** Six of seven criteria pass. A gate that has not been run counts as not passed (§2), and P7 is still open. On this evidence the latency and agreement questions are settled. The open questions are what a killed process strands, and how a scorer shares the KPU with Vision.

What the run adds beyond the criteria:

- **Call overhead is small.** The KPU section (`EXTCALL` in the runtime's profile) took 106 µs at p50 against the compiler's ideal estimate of 101 µs. Everything else (copy in, cache sync, interpreter, copy out) added about 18 µs. The risk that overhead erases the accelerator's advantage is retired for batched calls. Unbatched calls were not measured.
- **The fallback is expensive.** The C reference took 257 ms per 64-conversation batch on Unit A's only Linux hart. At one batch every 5 s, the NO-GO path would cost about 5 % of the hart. The KPU path would cost at most about 0.003 %, even if the hart were busy for the whole call. The reference is a simple direct convolution. An optimized RVV kernel was not written and would be faster by an unmeasured factor.
- **The simulator is an exact oracle.** Device and simulator outputs are byte-identical for both quantization modes. K2 can test the `nncase` backend against simulator golden vectors on the host and keep the device check for hardware gates.
- **u8 is good enough for resemblance.** On generated data, u8 changes the exact nearest neighbour of 6.6 % of conversations, but the neighbour's class changes for only 1.0 %, and precision at 1 stays at 0.876 (float) against 0.875. a16w8 halves the flips for 26 % more latency.
- **P7 can be cheap.** Each bench process made 4–5 explicit CMA allocations of 19–21 pages, 76–84 kB in total. If that is the whole device footprint (ASSUMED: single-page DMA allocations bypass CMA and are not counted), a SIGKILL strands at most about 84 kB, far under 4 MiB. P7 measures it. Any CMA it strands stays lost until the unit reboots.
- **Not measured.** CPU time spent during a KPU call (only wall time was recorded; G1 needs it), a real capture as the load, CPU frequency and temperature, and energy.

## 2. Gates

Every gate is measured. A gate that has not been run counts as not passed.

| Gate | What it protects | Pass when |
| --- | --- | --- |
| **G0 Visibility honesty** | the brief's first question: slow replies vs missing visibility | On replayed fixtures with injected HOLD, capture loss and one-way visibility: **≥ 99 % of affected intervals produce no PROBLEM claim**. Each such interval shows CAN'T TELL with its reason. Every cited packet resolves or reads "packet expired". (Synthetic E3a: the gated rules reached 96–98 %, so the analysts need one more refinement: also abstain on rates whose denominator falls inside a lossy bin.) |
| **G1 Cost** | the capture itself | `pos-capture bench`: ≤ +150 ns per frame. Helper ≤ +1 % of the hart at 2,000 pps on Unit B, shell ≤ +0.5 %. RSS within ADR-016's 32 MiB. No new kernel drops at the line-rate gate's rate. |
| **G2 Usefulness to people** | that a mark helps someone diagnose | Same shuffled cases, current SIGNALS workflow against the INSIGHT sheets (simulator or mockups, portrait and Large text). Time to the correct evidence improves. Confident wrong answers do not increase. ≥ 90 % of participants notice a visibility gap. Quiet captures are included. |
| **G3 Model admission** | against models that only fit their training data | Consented corpus, leave-site-out, thresholds frozen before one test run: **≥ 10 points more precision than the best deterministic baseline at equal false alerts per observed hour**. The interval across sites must exclude 0, and no held-out fault family may lose. Also: ≤ 0.2 false alerts per observed hour per detector family (upper Poisson bound, which needs ≥ 15 benign hours), and ≤ 5 % errors among accepted findings (Wilson upper bound). Learned weights over rules must be non-negative. |
| **G4 KPU admission** | the capture, memory, Vision | §1 above |
| **G5 Replay parity** | determinism; fixtures that mean something | The ledger of a live session equals the ledger of a virtual-clock replay of its saved file, for every observation the HEADERS file can support. On the host first, then on a unit. |
| **G6 Field precision** | trust in accepted claims | On a frozen golden set of real incidents from the bench corpus: errors among accepted claims ≤ 5 % (Wilson upper bound) at coverage ≥ 60 %. The resolver/path/local confusion matrix is published. |
| **G7 Lead time** | the word "early" | Before any "early warning" wording: median lead ≥ 15 s at precision ≥ 0.7, beating persistence and a current-value threshold in a paired bootstrap by site. Until then the product says "trend". |

## 3. Phases

Phases N1–N4 are the product line. K1–K3 are the accelerator line, and K1 can run in parallel with N1 because it shares no code. R is continuous research. Each phase ends with a build, its gates and a note of what could not be tested (`AGENTS.md`).

| Phase | Scope | Exit gates | Needs |
| --- | --- | --- | --- |
| **N0 Decide** (now) | This package; owner decisions D1–D8; independent review | owner's answers | — |
| **N1 Engine and WHEN on the CPU** | Series keyed by packet time, ledger, visibility gate, before/after, MARK and laps, the `intel` line and `ledger` pages (protocol 6), the INSIGHT row and face, the `SEEING:` chip. For the host: a scripted scenario source (faults and visibility artifacts) and virtual-clock file replay. Lint rules for the new tables | G0, G1 (host bench, then Unit B), G5 (host) | DS §59 amendment for MARK; Unit B time for G1 |
| **N2 Study** | The brief's experiment 2 on the N1 simulator build | G2 | participants; consent |
| **N3 WHO** | Name binding, Presence DNA (rules seeded from a licence-clean fingerprint list), rhythm, exposure | G0 and G1 again; rhythm claims respect the E3c minimum counts | licence review of the seed list and OUI names |
| **N4 WHY** | Doctor v1 (hypothesis graph, LR ≥ 10, CAN'T TELL, TO BE SURE), join watch, integrity check | G0, G6 on the golden set | bench corpus |
| **K1 KPU bench** | §1, research kit only. **Done on Unit A on 25 Nov 2026 except P7**: six of seven G4 criteria pass | G4 (P7 open) | authorization for P7: Unit B, or Unit A accepting stranded CMA until its next reboot |
| **K2 Scorer helper** | `pos-infer` with nncase, ref and fake backends; KPU lock shared with Vision; DNA encoder; resemblance in WHO | G1 with the scorer active; G4 again from the helper | D4 (ADR-016 amendment: the capture helper may start one scorer child) and a Vision ADR note for the shared lock |
| **K3 Name shape** | the character CNN as salience | G3, G4 | D1 |
| **R Research** | consented bench corpus; G3 evaluations; CESNET-trained DNA projection; trend/nowcast with netd station counters; promiscuous-mode reversion test for mirror-port mode | G3, G7 | corpus consent; separate ADR-016 decision for promiscuous mode |

**Order of value.** N1 alone answers the brief's first question on the unit, with no model and no new process. N1 and N3 together give a field technician presence, named conversations and laps, which no handheld in the landscape survey (R5) offers passively. K1 is cheap and independent, and it settles the accelerator question with measurements instead of opinions.

## 4. Smallest next steps

1. Owner answers D1–D8 ([HANDOVER.md](HANDOVER.md)). The answers decide the scope of N1 and of K2.
2. Independent review of the items listed in HANDOVER §3.
3. Close G4 with P7: three SIGKILLs of the bench mid-run, each followed by `CmaFree` after dropping caches and by the CMA allocation counters. It needs its own authorization (about 20 minutes with the existing kit). Unit B is preferred. On Unit A, any stranded CMA stays lost until its next reboot.
4. Write the N1 fixtures before the N1 code: the four replay cases of the first brief (slow paired replies, one-sided replies, HOLD across recovery, loss during a healthy exchange) as scripted scenarios, each with its expected ledger.
