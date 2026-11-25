# K1 on Unit A: measured report (25 November 2026)

First run of the K1 proof of concept on hardware (`docs/apps/packetshark/neural/ROADMAP.md` §1). The owner authorized non-destructive K1 validation on **Unit A** on 25 November 2026: P1–P6 and P8–P10, **P7 (SIGKILL) skipped**, no reboot, no flashing, nothing installed, no setting changed. Unit B was not connected. The G4 verdicts are in ROADMAP §1. This page holds the measurements, the method and the limits.

Evidence classes: every number below is VERIFIED (measured on Unit A or computed on the host from the pulled outputs) unless it is marked otherwise.

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
- **The C reference** is a direct convolution compiled with `-O2`. GCC 14 targets RVV by default here, but the convolution loop holds only 9 vector instructions. It is a simple baseline, not an optimized CPU kernel. The ratio to the KPU path is about 2,000× at p95. A CPU kernel 100× faster than this reference would still take about 2.7 ms per batch.
- **Spread between runs.** The C reference's p50 was 257 ms in the u8 run, 290 ms in the a16w8 run and 261 ms in the reference-only run. The cause was not investigated: CPU frequency and temperature were not recorded.
- **Tail under load.** The P8 p99 and max (2.2 ms and 3.4 ms) and the single P9 max (3.5 ms) match one scheduler slice lost to another task. The KPU's own time did not change: the p50 run step was 117 µs under load against 109 µs idle.

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

So u8 quantization changes the exact nearest neighbour of 6.6 % of conversations, but nearly always to another conversation of the same class. Retrieval quality is unchanged within 0.1 point. On generated data, a16w8 buys fewer flips for 26 % more latency.

## 4. Memory (P6, P9)

**RSS.** The highest `VmHWM` of any run was 5,248 kB. In P9, RSS read 4,480 kB at every minute and 4,608 kB at exit.

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
- **After cleanup, `CmaFree` was 2.86 MB above the as-found value.** No CMA loss is detectable. A loss below about 1 MB per run is neither shown nor excluded by these meters. A decisive meter would be `cma/reserved/used` in debugfs, which is not mounted on the image; mounting it would have changed the system.
- **For P7.** If the explicit allocations are a process's whole device footprint, a SIGKILL could strand at most about 84 kB, far under the 4 MiB criterion. This is ASSUMED: single-page DMA allocations bypass CMA and are not counted. P7 must measure it.

## 5. Exclusivity (P10)

While P9 held `/tmp/k1/kpu.lock` and ran its duty cycle, a second instance with the same lock printed `{"what":"lock","ok":false,"errno":11}` and exited with code 3. It made no CMA allocation. P9 continued and completed its 600 calls with exit code 0 and byte-identical output. This tests `flock` between two bench instances only. Vision's helper does not take this lock today.

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

- **P7 (SIGKILL)** was not run, by the owner's decision. G4 cannot pass without it.
- **Unit A, not Unit B.** Unit A runs the full stack (radio, mesh, Wi-Fi, idle shell). Those services shared the hart with the bench; the load average was near 0 before every idle step.
- **CPU time of a KPU call** was not measured (wall time only). Whether the hart spins during the 109 µs run step matters for G1 with a scorer. K2 should record `getrusage` around the calls.
- **The P8 load** was a shell loop that calls `stat`, not a capture. A real capture load was not tested.
- **CPU frequency and temperature** were not recorded. They may explain the 13 % spread of the C reference between runs.
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
