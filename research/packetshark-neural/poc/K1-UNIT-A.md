# K1 on Unit A: measured report (25 November 2026)

First run of the K1 proof of concept on hardware (`docs/apps/packetshark/neural/ROADMAP.md` §1). The owner authorized non-destructive K1 validation on **Unit A** on 25 November 2026: P1–P6 and P8–P10, **P7 (SIGKILL) skipped**, no reboot, no flashing, nothing installed, no setting changed. Unit B was not connected. The G4 verdicts are in ROADMAP §1. This page holds the measurements, the method and the limits.

Evidence classes: every number below is VERIFIED (measured on Unit A or computed on the host from the pulled outputs) unless it is marked otherwise.

**Revised on 25 November 2026 after an independent review.** Every measurement, table value and raw result is unchanged. Interpretations the review corrected are marked **Post-review correction**, and §9 lists each one beside its original wording. The original text is at commit [`9b790069`](https://github.com/KrakenSaten/K230/blob/9b790069/research/packetshark-neural/poc/K1-UNIT-A.md). The review is [`INDEPENDENT-REVIEW.md`](https://github.com/KrakenSaten/K230/blob/463ad7d8/docs/apps/packetshark/neural/INDEPENDENT-REVIEW.md) in draft PR #186.

## Status after the independent review

| Criterion | Status | Basis |
| --- | --- | --- |
| P2/P4 end to end | **PASS on latency** | 132.4 µs p95 back to back, 190.6 µs p95 at one call a second. The ratio to the C reference supports no claim of need (§2). |
| P8 under load | **PASS** | 221.4 µs p95 |
| P5 agreement | **PASS** | byte-identical to the simulator in both modes |
| P6 memory after a normal exit | **INCONCLUSIVE** | RSS passes (5.1 MiB peak). Whether a process returns its CMA is unanswered (§4). |
| P7 SIGKILL | **NOT TESTED** | skipped by the owner's decision |
| P9 duty cycle | **PASS for RSS only** | CMA is not assessable with the meter used (§4) |
| P10 exclusivity | **PASS for the bench only** | `flock` between two bench instances. Sharing with Vision is untested (§5). |

**G4 has not passed.** Five of seven criteria pass, two of them in a narrowed sense. P6 is inconclusive and P7 was not tested. The measurements show that the KPU is fast. They do not show that this model needs the KPU.

## 1. Setup

| Item | Value |
| --- | --- |
| Unit | Unit A. wlan0 MAC matches Unit A's recorded address. DOORS 0.4.0 (`BUILD_ID=21baeef0`), Linux 6.6.36, `nproc` 1 (one Linux hart). |
| Unit state found | Up 1 day. `doors-shell` (sha256 `50786142…`) idle under `pos-supervise`; sysd, netd, radiod, meshcored, `isp_media_server` running. No Vision helper, no capture. No process held `/dev/k230-gnne`, `/dev/k230-ai2d` or `/dev/mmz`. Load average 0.00. |
| Kit | `/tmp/k1` (tmpfs), 6.2 MB. Transferred over the serial console (gzip + base64), every file checked by SHA-256 on the unit. Removed at the end. |
| `infer-bench` | Built from `poc/infer_bench.cpp` at this commit with `poc/build.sh`: Xuantie GCC 14.1.1 (SDK), `-O2`, nncase runtime 2.11.0 linked statically. Stripped copy run on the unit: sha256 `d063fb59…`; unstripped `165c0758…`. |
| Models | `dna_b64` u8 (`2ae08486…`, 27,288 B) and a16w8 (`7775b595…`, 29,216 B), the E2 kmodels. |
| Inputs | 32 batches × 64 conversations from E3b (generated), the float outputs and the simulator outputs of both kmodels. |
| Driver | `poc/k1/k1.sh`, one step per call, run in the background. It never signals a process; the P8 CPU hog stops when a stop file appears. Three revisions during the run: P1 ran without the CMA allocation counters, P2 added them, and P3 onwards dropped the a16w8 profiling run to save one KPU run. |
| Change to the bench before the run | `infer_bench.cpp` gained the nearest-neighbour agreement in the `agree` lines and `--dump F`, so that device outputs could be pulled and compared byte for byte. The host check still gives cosine 1.000000 and nearest neighbour 100 % against the float model. |

## 2. Latency (µs; p50 / p95 / p99 / max)

One call = one batch of 64 conversations through the whole host-tensor path. 1,000 timed calls unless noted, after 33 untimed warm-up calls.

| Run | copy in | sync | run | copy out | **call end to end** | C reference (same batch, CPU) |
| --- | --- | --- | --- | --- | --- | --- |
| P2 u8 | 8.7 / 9.5 / 14.8 / 85 | 1.9 / 2.2 / 2.5 / 8 | 109.2 / 116.3 / 154.6 / 269 | 3.6 / 4.5 / 9.0 / 81 | **123.6 / 132.4 / 174.1 / 285** | 256,943 / 269,864 / 273,688 / 299,824 |
| P2 a16w8 | 8.7 / 14.1 / 14.7 / 27 | 1.9 / 2.3 / 7.1 / 15 | 140.8 / 157.0 / 198.6 / 2,053 | 3.6 / 4.7 / 8.7 / 17 | **155.3 / 173.4 / 214.0 / 2,071** | 290,348 / 299,860 / 304,905 / 308,584 |
| P8 u8 under a nice-0 CPU hog (300 calls) | 9.0 / 11.0 / 16.2 / 17 | 2.1 / 2.5 / 2.7 / 9 | 116.8 / 196.5 / 2,156 / 3,335 | 4.3 / 5.5 / 6.9 / 102 | **132.1 / 221.4 / 2,174 / 3,354** | 915,432 / 924,967 / 927,484 / 931,146 |
| P9 u8, 1 call per second for 600 s | 15.0 / 20.3 / 21.5 / 31 | 2.7 / 3.2 / 8.9 / 36 | 145.8 / 165.0 / 188.0 / 3,497 | 4.5 / 5.3 / 10.5 / 39 | **169.1 / 190.6 / 211.3 / 3,520** | — |
| P3 u8 with `--profile` (200 calls) | 9.4 / 16.4 | 2.3 / 2.7 | 278.8 / 327.2 | 6.0 / 10.6 | 297.4 / 346.6 | — |

- **P1 load and first call.** `load_model` took 1,442 µs on the first run after dropping caches and 1,248 µs on the next. Across all idle runs it took 1.2–1.6 ms. The first `run()` took 616–920 µs. Under the P8 hog these were 3.8 ms and 3.2 ms.
- **P3 profile.** The interpreter program is one `EXTCALL` (the KPU section) plus `LDARG`, two `LDC_I4` and `RET`, each under 1 µs. No operator runs on the CPU. `EXTCALL` over 233 calls: min 97.2, p50 106.0, p95 126.2, p99 184.3, max 486.3 µs. The compiler's roofline estimate was 101 µs (E2).
- **Overhead.** Outside the KPU section, one call costs about 18 µs at p50: copy in 8.7, sync 1.9, copy out 3.6, and interpreter and output-tensor handling about 3. Inside `EXTCALL` the measured median exceeds the compiler's ideal estimate by about 5 µs.
- **Profiling is not free.** `--profile` raised the run step from 109 to 279 µs at p50, because the runtime prints a timeline on every call. Timed runs (P2, P8, P9) were made without it.
- **The C reference** is a direct convolution compiled with `-O2`. GCC 14 targets RVV by default here, but the convolution loop holds only 9 vector instructions. It is a simple baseline, not an optimized CPU kernel. It took 256.9 ms at p50 for 13,303,808 multiply-accumulates: 19.3 ns each, about 31 cycles at the documented 1.6 GHz.
- **Post-review correction: the CPU comparison.** The measured ratio of the C reference to the KPU path is 2,038 at p95. It must not be read as the KPU's advantage over the CPU.
  - **Why the baseline is unfair.** Channels are the innermost loop over planar data, so successive loads sit 8 KiB apart and alias in the L1 cache, and the accumulator is serial.
  - **A fair kernel (ASSUMED, not measured).** 15–50 ms per batch for a cache-friendly scalar kernel, and 2–10 ms for int8 RVV. That puts the ratio at roughly 16–400 times.
  - **Per conversation.** The CPU estimate is 0.03–0.8 ms, which is comparable to one KPU call.
  - **Conclusion.** K1 shows that the KPU is fast. It does not show that this model needs the KPU.
- **Post-review correction: which latency matters.** P9, at one call a second, is the product's rate: 169.1 / 190.6 µs at p50 / p95, which is 37 % above the back-to-back figure. Every segment of the call is slower, which points to cold caches after a second of idleness (ASSUMED).
- **Spread between runs.** The C reference's p50 was 257 ms in the u8 run, 290 ms in the a16w8 run and 261 ms in the reference-only run. The cause was not investigated. **Post-review correction:** frequency scaling is not the cause, because it is not built into this kernel (`CONFIG_CPU_FREQ` is not set). The 13 % offset covered a whole run, and the CPU-side segments of the KPU call in that process were unchanged. It is unexplained. Physical page placement interacting with the reference's cache aliasing is a candidate (ASSUMED).
- **Tail under load.** The P8 p99 and max (2.2 ms and 3.4 ms) and the single P9 max (3.5 ms) match one scheduler slice lost to another task. The KPU's own time did not change: the p50 run step was 117 µs under load against 109 µs idle. **Post-review correction:** under the hog the C reference slowed 3.56 times at p50, where one extra equal-weight task predicts 2.0. That is unexplained, and the hog was never characterised.

## 3. Agreement (P5, P4)

| Comparison (2,048 conversations) | Result |
| --- | --- |
| Device u8 vs simulator u8 | **byte-identical** (`cmp`; both files sha256 `426c74e8…`). Nearest neighbour 100 %. |
| Device a16w8 vs simulator a16w8 | **byte-identical** (`a6a24ff0…`). Nearest neighbour 100 %. |
| C reference on the C908 vs the float model | cosine 1.000000 (min), nearest neighbour 100 % |
| Device u8 vs C reference (float) | cosine min 0.99474, mean 0.99976; same nearest neighbour 93.2 % |

Host analysis of the pulled outputs (`poc/k1_analyze.py`, labels from E3b, 10 classes):

| Float model vs | Cosine min / p1 | Same nearest neighbour | Neighbour in top 3 | Neighbour's class unchanged | Precision at 1 (float 0.8765) |
| --- | --- | --- | --- | --- | --- |
| u8 (device = simulator) | 0.9947 / 0.9985 | 93.4 % | 99.95 % | **99.0 %** | 0.8755 |
| a16w8 (device = simulator) | 0.9992 / 0.9997 | 96.9 % | 100 % | **99.6 %** | 0.8765 |

**Post-review correction: quantization and nearest neighbours.** The tables above are unchanged. Two conclusions drawn from them were wrong (§9):

- **The flips are not class-preserving.** u8 changes the nearest neighbour of 136 of 2,048 conversations (6.6 %). 21 of those land in a different class: 15.4 %, against 12.4 % expected by chance, which is the float model's own rate of wrong-class neighbours.
- **Retrieval quality is unchanged, but individual neighbours are not stable.** Net precision at 1 changes by 2 of 2,048 (0.8755 against 0.8765), which is inside noise.
- **a16w8** changes 64 neighbours, 9 of them to another class.
- **The a16w8 latency difference is not cleanly attributable to the mode.** 155.3 against 123.6 µs p50 comes from one run in a fixed order, in the process whose C reference also ran 13 % slow.
- **What byte identity shows.** The device computes what the simulator computes on these 2,048 inputs. It says nothing about quantization quality. u8 must be re-judged on real data.

## 4. Memory (P6, P9)

**RSS.** The highest `VmHWM` of any run was 5,248 kB. In P9, RSS read 4,480 kB at every minute and 4,608 kB at exit. **Post-review correction:** P9 passes for RSS only. Its `CmaFree` readings were flat from minute 2 to 10, but that meter cannot carry a verdict (below).

**CMA.** `CmaFree` after `sync; echo 1 > /proc/sys/vm/drop_caches`, with the kernel's explicit CMA allocation counters (`cma_alloc_success` in `/proc/vmstat`, `/sys/kernel/mm/cma/reserved/alloc_pages_success`):

| Point | CmaFree (kB) | Explicit CMA allocations during the step |
| --- | --- | --- |
| As found, three readings | 476,096 · 476,284 · 476,284 | — |
| Kit in tmpfs, three readings 20 s apart | 472,252 · 472,252 · 472,252 | — |
| After P1 (two KPU runs) | 471,596 | not counted (driver v1) |
| After P2 u8 | 471,520 | 4 allocations, 19 pages |
| After P2 a16w8 | 471,260 | 5 allocations, 21 pages |
| Reference-only run (no KPU), in process | 471,260 → 470,504 | none |
| After P3 | 471,188 | 4, 19 pages |
| After P8 | 470,740 | 4, 19 pages |
| During P9 at minutes 2–10 (in process, not dropped) | 470,592, flat | 4, 19 pages |
| After P9 | 470,592 | — |
| **After cleanup** (`/tmp/k1` removed), three readings | **479,148 · 479,148 · 479,148** | total over the run: 21 allocations, 97 pages |

Reading it:

- Each KPU process made 4–5 explicit CMA allocations of 19–21 pages (76–84 kB) in total: the model's buffers and tensors.
- The in-process readings at `before_release` and `released` were equal in every run. On Linux 6.6, pages freed by `cma_release` go to the per-CPU page lists first and are not counted in `CmaFree` until those lists drain. So this reading cannot show whether `shrink_memory_pool()` and `kd_mpi_mmz_deinit()` returned the pages.
- The per-CPU lists held 705 pages (2.8 MB) during the run and 261 pages (1.0 MB) after cleanup (`/proc/zoneinfo`). Readings moved by −756 kB in a run without the KPU and by +684 kB in a KPU run. So `CmaFree` resolves about 1–3 MB here, not tens of kilobytes.
- **After cleanup, `CmaFree` was 2.86 MB above the as-found value.**
- **Post-review correction: P6 is INCONCLUSIVE.** The readings above do not answer whether a process returns its CMA.
  - **Signal against noise.** The signal sought is 76–84 kB per process. Per-process deltas of the dropped readings average −129 kB with a standard deviation of 379 kB, and the run without the KPU moved the reading by −756 kB.
  - **The counters count allocations only,** never frees.
  - **The +2.86 MB does not show recovery.** The per-CPU lists held 705 pages mid-run and 261 after cleanup, and their size as found was not recorded. Their limit computes to about 1,240 pages (4.9 MB) on this unit.
  - **What is excluded.** A leak of a page per call (3,351 calls would have cost about 13 MB) and a loss of megabytes per process.
  - **An exact meter exists and was not used.** The kernel is built with `CONFIG_CMA_DEBUGFS=y`, so `/sys/kernel/debug/cma/reserved/used` counts held CMA pages once debugfs is mounted. Mounting it changes the system until the next reboot, so it needs the owner's OK.
- **P7 was NOT TESTED. Post-review correction:** no bound on what a SIGKILL strands can be given from these data. The review verified in the kernel source that buffers of one page or less bypass CMA. They are invisible to `CmaFree` and to both counters, and a kill strands them like any other buffer. With `CmaFree` alone, three kills would also be lost in the noise. P7 should follow the amended protocol in the review's §7.

## 5. Exclusivity (P10)

While P9 held `/tmp/k1/kpu.lock` and ran its duty cycle, a second instance with the same lock printed `{"what":"lock","ok":false,"errno":11}` and exited with code 3. It made no CMA allocation. P9 continued and completed its 600 calls with exit code 0. This tests `flock` between two bench instances only. Vision's helper does not take this lock today.

**Post-review correction: what P10 does and does not validate.**

- **P9's output was not byte-compared.** P9 ran without `--dump`. Its `agree` line gives cosine 1.000000 and nearest neighbour 100 % against the simulator for its last 32 batches.
- **The check is limited to the bench.** It shows that a second instance of the same program, using the same advisory lock file, is refused. It does not show that the KPU is shared safely with Vision, which does not take the lock.
- **The lock is needed for correctness, not only for sharing.** The review found in the driver source that the KPU driver's completion flag is global and zeroed by any open or close, and that the runtime waits without a timeout. Every KPU client must therefore take the lock before it opens the device.

## 6. Unit A afterwards

| Check | Before | After |
| --- | --- | --- |
| Processes | 101 lines from `ps`, services as listed in §1 | the same services with the same PIDs; no `infer-bench` or `k1.sh` left |
| KPU and MMZ device holders | none | none |
| `/tmp` | `dbus`, `isp.err.log`, `kod`, `messages` (60,954 B), `resolv.conf` | the same; `messages` 62,528 B before the final readings |
| Binaries | `doors-shell` `50786142…`, `pos-vision` `60a4b7d4…`, `doors` `4d552efd…` | unchanged |
| `MemAvailable` | 916,336 kB | 917,772 kB |
| `CmaFree` after dropping caches | 476,284 kB | 479,148 kB |
| Kernel log | 494 lines | +22 lines, all `drop_caches: 1` |

The serial console was root-logged-in when found and was left that way. Nothing was installed, no service was touched, no setting changed, no reboot. The only lasting traces are syslog and kernel-log lines about `drop_caches`, which emptied the page cache 22 times.

## 7. Not tested and limits

- **P7 (SIGKILL)** was not run, by the owner's decision. It is NOT TESTED, and G4 cannot pass without it.
- **P6 is inconclusive** (post-review, §4). It needs the exact meter or an amplified run.
- **An optimized CPU kernel** was not measured (post-review, §2). Without it, the KPU's real advantage is unknown.
- **Sharing the KPU with Vision** was not tested (post-review, §5).
- **Unit A, not Unit B.** Unit A runs the full stack (radio, mesh, Wi-Fi, idle shell). Those services shared the hart with the bench; the load average was near 0 before every idle step.
- **CPU time of a KPU call** was not measured (wall time only). Whether the hart spins during the 109 µs run step matters for G1 with a scorer. A later session should record `getrusage` around the calls. Post-review: the runtime's disassembly shows it waiting in `poll()`, so the hart should be asleep, but that is still unmeasured.
- **The P8 load** was a shell loop that calls `stat`, not a capture. A real capture load was not tested.
- **Temperature** was not recorded. Post-review: frequency scaling is not built into this kernel, so the 13 % spread of the C reference between runs is unexplained (§2).
- **One model, one batch size, generated inputs.** Latency is data-independent for a fixed graph. Accuracy figures on generated data are not accuracy on real traffic.
- **Energy** was not measured.

## 8. Reproduce

Host side (Git Bash and WSL; paths as used here):

```bash
bash poc/build.sh                                   # WSL: infer-bench + infer-bench-host
bash poc/k1/mkkit.sh                                # WSL: kit, SHA256SUMS, sendkit.ps1 under C:\K230-artifacts\packetshark-neural\k1-unit-a
```

Then from Windows PowerShell, with the unit's console on COM5 and the serial helpers of `C:\K230-artifacts\vision-stream\scripts` (`ser.ps1`, `sendfile.ps1`, `getfile.ps1`):

```
ser.ps1 -CmdFile poc\k1\cmd-01-before.txt -Until BF2DONE     # read-only snapshot
sendkit.ps1                                                   # kit to /tmp/k1, SHA-256 checked
step.ps1 -Step idle ; p1 ; p2u8 ; p2a16 ; reff ; p3 ; p8      # one at a time
step.ps1 -Step p9  (start) ; after ~90 s: step.ps1 -Step p10 ; then step.ps1 -Step p9 -NoStart
ser.ps1 -CmdFile poc\k1\cmd-09-after.txt -Until AF2DONE      # removes /tmp/k1, final state
python poc/k1_analyze.py <dir with float/sim/kpu .f32> results/k1_unit_a results/k1_unit_a.json
```

Outputs here: `results/k1_unit_a/*.jsonl` (every line the unit printed, including the P3 operator timelines) and `results/k1_unit_a.json` (host analysis and all steps). Console logs, the device dumps and the kit are in `C:\K230-artifacts\packetshark-neural\k1-unit-a\` with `SHA256SUMS.txt`. The console logs stay out of Git because they contain the unit's MAC address.

Post-review: this recipe reproduces the run as it was made. P7, and a repeat of P6, should follow the amended protocol in the review's §7 instead.

## 9. Revision record (25 November 2026)

This revision changes interpretations only. Not changed: every number in the tables of §1–§6, the raw results under `results/k1_unit_a/`, `results/k1_unit_a.json`, the scripts and the bench.

| Section | Original wording (commit `9b790069`) | Correction |
| --- | --- | --- |
| §2 | "The ratio to the KPU path is about 2,000× at p95. A CPU kernel 100× faster than this reference would still take about 2.7 ms per batch." | The baseline is cache-hostile. A fair CPU kernel is estimated at 2–50 ms per batch, a ratio of roughly 16–400 times. No claim that the model needs the KPU is supported. |
| §2 | "The cause was not investigated: CPU frequency and temperature were not recorded." | Frequency scaling is not built into the kernel. The spread is unexplained. |
| §2 | (not stated) | P9's latency, 169.1 / 190.6 µs, is the product-relevant figure. The hog's 3.56× slowdown of the reference is unexplained. |
| §3 | "… but nearly always to another conversation of the same class." | 21 of 136 flips change class: 15.4 %, against 12.4 % by chance. Not class-preserving. |
| §3 | "On generated data, a16w8 buys fewer flips for 26 % more latency." | The latency difference comes from one run in a fixed order and is not attributable to the mode. |
| §4 | "No CMA loss is detectable." | P6 is INCONCLUSIVE. A per-process loss of 76–84 kB is neither shown nor excluded, and an exact meter was available but not used. |
| §4 | "… a SIGKILL could strand at most about 84 kB, far under the 4 MiB criterion." | The bound is withdrawn. Single-page buffers bypass CMA and are invisible to the meters. P7 is NOT TESTED. |
| §4 | (P9's memory result not qualified) | P9 passes for RSS only. |
| §5 | "… completed its 600 calls with exit code 0 and byte-identical output." | P9's output was checked by cosine, not byte-compared. P10 validates the lock between two bench instances only. |
| §7 | "K2 should record `getrusage` around the calls." | "A later session should …": the review rates K2 as specified NO-GO, pending the owner. |
| top | (verdicts deferred to ROADMAP §1) | A status table was added. G4 has not passed. |
