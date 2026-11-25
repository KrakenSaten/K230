# K1 on Unit A: brief for independent review (Fabel 5.2)

**25 November 2026.** This is a handover for an independent technical review of the first KPU measurement for PacketShark NEURAL. It gives the question, the material and what to challenge. The author's verdicts are in [ROADMAP §1](ROADMAP.md#result-on-unit-a-25-november-2026). Please form your own reading of the raw data before you read them.

## 1. The question

Does the evidence support admitting a small batched model to the KPU (gate G4), apart from the SIGKILL criterion P7, which was not run? Specifically:

1. Are the latency figures valid measurements of what a scorer helper would pay per call?
2. Does the agreement evidence show that the device computes what the simulator computes?
3. Is the CMA conclusion ("no loss detectable at 1–3 MB resolution") the strongest claim the meters allow, and is it strong enough for G4's P6?
4. Is anything in the protocol, the code or the analysis biased toward the KPU?

The decisions it feeds: whether to proceed to K2 (`pos-infer`) after P7, the owner's decision D4, the choice between u8 and a16w8, and where to run P7.

## 2. What ran

- **Unit A**, DOORS 0.4.0 (`21baeef0`), Linux 6.6.36, one Linux hart, its normal services running and its shell idle. Unit B was not connected. The owner authorized P1–P6 and P8–P10 only: no SIGKILL, reboot, install or setting change.
- **Program.** `infer-bench`: nncase 2.11.0 host-tensor path (no AI2D) and a portable C reference of the same network and weights.
- **Model.** `dna_b64`: input [1,3,64,32], output [1,32,64,1], 20,960 parameters, compiled with nncase 2.11.0 PTQ as u8 and a16w8. The compiler placed every operator on the KPU (E1, E2).
- **Inputs.** 32 batches of 64 generated conversations (E3b).
- **Protocol.** ROADMAP §1, run step by step by `k1.sh`. There were three deviations, each with its reason:
  - P3 profiling ran separately.
  - The P8 hog was a shell loop stopped by a file, so that nothing was killed.
  - P10 ran against the P9 instance while it held the lock.
- **Changes made on the day of the run.** The bench gained `--dump` and a nearest-neighbour check before the run. The driver script was revised twice during the run: it added the CMA allocation counters after P1, and it dropped one profiling run.

## 3. Material

| What | Where |
| --- | --- |
| Bench source, build script, driver, host scripts, analysis | Branch `research/packetshark-neural-lab` at [`9b790069`](https://github.com/KrakenSaten/K230/tree/9b790069/research/packetshark-neural/poc): `poc/infer_bench.cpp`, `poc/build.sh`, `poc/k1/`, `poc/k1_analyze.py` |
| The diff made to the bench on the day | `git diff 29e1e8cc 9b790069 -- research/packetshark-neural/poc/infer_bench.cpp` |
| Every JSON line the unit printed, including 233 operator timelines | `research/packetshark-neural/results/k1_unit_a/*.jsonl` |
| Host analysis of the pulled device outputs | `research/packetshark-neural/results/k1_unit_a.json` |
| Measured report: setup, tables, CMA accounting, unit state before and after, limits | `research/packetshark-neural/poc/K1-UNIT-A.md` |
| Console logs, device output dumps, kit and kmodels, with SHA-256 | `C:\K230-artifacts\packetshark-neural\k1-unit-a\` on the build PC (not in Git: the logs contain the unit's MAC address) |
| Model and quantization background | [EXPERIMENTS.md](EXPERIMENTS.md) E1, E2, E3b; [MODELS.md](MODELS.md) §2 |
| CMA background | `docs/vision/KPU_CMA_LEAK.md` |

## 4. Measured values, without interpretation

`dna_b64` u8, one call = 64 conversations, µs:

| | p50 | p95 | p99 | max |
| --- | --- | --- | --- | --- |
| Call, idle (P2, 1,000 calls) | 123.6 | 132.4 | 174.1 | 285.0 |
| `EXTCALL` from the runtime profile (P3, 233 calls, profiling on) | 106.0 | 126.2 | 184.3 | 486.3 |
| Call under a nice-0 CPU hog (P8, 300 calls) | 132.1 | 221.4 | 2,174 | 3,354 |
| Call at 1 per second for 600 s (P9) | 169.1 | 190.6 | 211.3 | 3,520 |
| C reference, idle (P2, 1,000 calls) | 256,943 | 269,864 | 273,688 | 299,824 |
| C reference under the hog (P8, 300 calls) | 915,432 | 924,967 | 927,484 | 931,146 |

- **Compiler estimate.** The roofline estimate for the KPU section was 101 µs.
- **a16w8.** The call took 155.3 µs at p50 and 173.4 µs at p95.
- **Agreement.** Device outputs are byte-identical to the simulator's for both modes. The C reference on the device matches the float model at cosine 1.000000.
- **Memory.** RSS peaked at 5,248 kB.
- **CMA counters.** Each KPU process made 4–5 explicit CMA allocations of 19–21 pages.
- **CmaFree after dropping caches:**
  - as found: 476,284 kB;
  - with the kit in tmpfs: 472,252 kB;
  - lowest during the run: 470,592 kB;
  - after cleanup: 479,148 kB.
- **Per-CPU page lists:** 705 pages during the run, 261 after.
- **P10.** The second instance got `EAGAIN` and exited with code 3.

## 5. Points to challenge

1. **CMA meter.** The claim rests on two things: freed CMA pages pass through the per-CPU lists, and `CmaFree` therefore cannot resolve less than about 1 MB. Is the reasoning about Linux 6.6's `free_unref_page` path and `drop_caches` correct? Could a per-run leak of 76–84 kB hide in it across 7 KPU runs? Is there a read-only meter that was missed? The author considered `/proc/kpageflags` and `/proc/kpagecount` and rejected them, and considered debugfs `cma/*/used` but did not mount it.
2. **Release path.** `before_release` and `released` were equal in every run. Does that show only the per-CPU list effect, or could `shrink_memory_pool()` and `kd_mpi_mmz_deinit()` have freed nothing, leaving the buffers to process exit, which the MMZ driver does not reclaim?
3. **Baseline fairness.** The C reference is a direct convolution at `-O2` with only 9 vector instructions in its inner loop. G4 compares against it. Is the "≤ 25 % of the CPU" criterion still meaningful? An optimized RVV kernel was not written.
4. **What a call costs the hart.** Only wall time was measured. If the runtime spins while the KPU runs, the CPU cost equals the wall time, about 0.12 ms per call. Does that matter for G1?
5. **Load model.** The P8 hog was a shell loop calling `stat`, not a capture. The p99 and max under load may reflect scheduler slices rather than the KPU. Is the "≤ 4 ms p95" criterion the right one when p99 is 2.2 ms?
6. **Venue.** Unit A, not Unit B, with radio, mesh and Wi-Fi services running. CPU frequency and temperature were not recorded. The C reference's p50 differed by 13 % between runs.
7. **Agreement oracle.** The simulator's outputs were produced on the host by the same compile as the device's kmodel. Byte identity shows device = simulator, not that either is right. The quantization error against the float model is in K1-UNIT-A §3. Is the nearest-neighbour analysis on generated data with 10 classes a sound basis for choosing u8?
8. **Mid-run changes.** The bench changed on the day of the run, and the driver changed during it. Check that the reported numbers come from the versions stated, using the `rc` lines, the SHA-256 sums and the step files.
9. **P7 prediction.** The author predicts that a SIGKILL strands at most about 84 kB, ASSUMING single-page DMA allocations bypass CMA. Is that sound for `dma_alloc_coherent` on this kernel and the MMZ driver?

## 6. Out of scope

- Accuracy of the DNA encoder on real traffic (gate G3).
- Whether resemblance helps people (G2).
- The product design in [NEURAL.md](../NEURAL.md).
- P7 itself.
