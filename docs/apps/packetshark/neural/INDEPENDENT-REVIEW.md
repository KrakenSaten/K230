# PacketShark NEURAL: independent AI and architecture review

**25 November 2026 · Fable 5.2 · reviews draft PR #186 at `c9c8e64e` and `research/packetshark-neural-lab` at `9b790069`.** Nothing was run on a unit, no model was trained, no test was repeated and no production code was changed.

**Method.** Three reviewers with no access to the author's conclusions audited the kernel and driver sources, the synthetic-data experiments and the raw K1 data. I read the design package myself and checked their sharpest claims in the source.

**Evidence classes.** **V** = verified in this review (source, kernel config, raw data). **D** = documented in the repository. **A** = assumed or estimated, not measured.

## 1. Bottom line

1. **K1's numbers are real, but they answer a question the product does not have.** The KPU runs `dna_b64` in 124 µs. For this model at product rates, a competent CPU kernel is also fast enough (A). K1 shows the KPU is fast, not that it is needed.
2. **The "2,000 times" ratio is against a pathological baseline.** A fair CPU figure makes it roughly 16–400 times (A).
3. **The memory criterion is unanswered, not passed.** An exact meter exists and was not used.
4. **The main synthetic result is constructed.** The generator hands the visibility gate its ground truth and never lets a fault overlap a gap.
5. **Two platform defects surfaced that matter more than the latency.** One is a security finding whose detail is withheld from this public file (F1). The other is that the KPU driver's completion flag is shared between processes (F2).
6. **K2 as specified: NO-GO.** N1 (the CPU engine and WHEN) remains the right next build. The accelerator line should be re-gated on need.

## 2. Findings

### P0: act before building further

| # | Finding | Class | Consequence |
| --- | --- | --- | --- |
| F1 | A platform security finding, verified in vendor driver source. Its detail is withheld from this public file by the owner's decision and was reported to the owner privately. | V | It predates NEURAL and bears on a risk that ADR-016 accepted. It was derived from source and not exercised. |
| F2 | The KPU driver has one global "done" flag, set by the interrupt and cleared by any poller. Its mutex is unused, and open and release only zero the flag (`drivers/misc/canaan/gnne/k230_gnne.c:60–128`). The runtime waits with `poll(fd, POLLIN, -1)` and ignores the result (disassembly). | V | (a) The CPU sleeps during the KPU section, so a call costs about the 18 µs outside it (A until `getrusage` confirms). (b) There is no timeout, and any other process that opens or closes the device zeroes the flag, so a waiting caller can hang forever while it holds the lock. (c) A process killed mid-inference leaves the hardware running. Its late interrupt sets the flag with no owner, and a client that **already has the device open** would take it as its own completion and read an unfinished result. A client that opens afresh is protected, because open zeroes the flag. Effects (b) and (c) are derived from source, not observed (A). The lock is therefore mandatory for correctness, not only for sharing. |
| F3 | The roadmap's only KPU feature is resemblance among **the unit's own conversations**. At the default vantage those are a handful of the unit's own service flows (D). The lab's own desk note says conversation DNA needs no KPU ("< 1 ms per flow on the CPU") and should start as a deterministic projection (D, R3). The batch of 64 exists to give the KPU a workload. | D | The accelerator is being built for a feature with almost no input until mirror-port mode exists, and that mode waits on decision D9. |

### P1: validity of the evidence

| # | Finding | Class |
| --- | --- | --- |
| F4 | **CPU baseline.** 13,303,808 MACs in 256.9 ms is 19.3 ns per MAC, about 31 cycles at the documented 1.6 GHz. The loop puts channels innermost over planar data, so successive loads sit 8 KiB apart and alias in L1, with a serial accumulator. Estimates: 15–50 ms for a cache-friendly scalar kernel, 2–10 ms for int8 RVV. Per conversation (208 k MACs) that is roughly 0.03–0.8 ms on the CPU, against at least 0.12 ms per KPU call. | V for the measured part, A for the estimates |
| F5 | **K1 measured the KPU's floor, not its capacity.** The compiler's estimates are 0.07 ms for models up to 0.7 M MACs, 0.101 ms at 13.3 M and 0.229 ms at 65.6 M: about 0.07 ms fixed plus 2.4 µs per million MACs. K1 confirmed one point (106 µs measured against 101). If the slope holds, 2 ms buys about 0.8 G MACs, 60 times this model. | D for the estimates, V for one point, A for the slope |
| F6 | **CMA.** The author's mechanism is correct: freed CMA pages park on per-CPU lists, `drop_caches` does not drain them, and `pcp->high` computes to about 1,240 pages (4.9 MB). But the data cannot carry a verdict. Per-process deltas average −129 kB with SD 379 kB, the no-KPU control moved −756 kB, and the signal sought is 76–84 kB. A per-call or MB-scale leak is excluded. A per-process leak is **unanswered**. Buffers of one page or less bypass CMA entirely and are invisible to every CMA meter. The kernel has `CONFIG_CMA_DEBUGFS=y`: once debugfs is mounted, `/sys/kernel/debug/cma/reserved/used` is exact. | V |
| F7 | **Visibility gate.** Coverage is written by the same branch that injects the artifact (`synth_telemetry.py:158,170,172`), with loss between 15 and 60 %, and a window is either a gap or a fault, never both. So the benefit (65/86 → 2/86, 71/79 → 3/79) is constructed, and the cost is invisible: fault detection is identical with and without the gate. | V |
| F8 | **Rules against learned models are not like for like.** The rules map one-to-one onto the generator's fault channels, including the "unseen" ones. The supervised TCN beat the rules on two of three held-out faults (0.96 and 0.64 against 0.44 and 0.26). 0.904 against 0.879 is inside noise: one seed, 15 sites. Two baselines are dead (their threshold equals the clip value). The negative fusion weights come from a training catalogue that never moved those channels. | V |
| F9 | **Open-set.** "0.2 % rejected" is one flow of about 455. The threshold is calibrated on test flows and pooled over lengths. The embedding is trained with cross-entropy. The margin and length eligibility that R3 specified were not implemented. With kNN the encoder's lead is 93.2 against 90.6. | V |
| F10 | **Quantization.** u8 flips 136 of 2,048 nearest neighbours, and 21 of those change class: 15.4 %, against a 12.4 % chance rate. The flips are not class-preserving beyond chance. Net precision at 1 changes by 2 of 2,048. "26 % more latency for a16w8" comes from one run in a fixed order. | V |
| F11 | **Smaller K1 points.** The product-relevant latency is P9's (one call a second): 169 / 191 µs, not 124 / 132. P9's "byte-identical" claim has no dump behind it. The hog slowed the reference 3.56 times where one extra task predicts 2.0. The 13 % spread of the reference is unexplained, and it is not frequency scaling (`CONFIG_CPU_FREQ` is off). | V |
| F12 | **Backend mixing.** The design falls back to the float reference when Vision holds the lock. Float and KPU outputs differ (cosine min 0.9947, 6.6 % neighbour changes), so one session could mix two embedding spaces. | V for the difference, D for the design |
| F13 | **The meeting-room example overstates passive sight.** OFFER and ACK are unicast unless the client sets the broadcast flag. `PACKETSHARK.md` already encodes this for M12.1 (D). "Two servers offered addresses" and "that exchange is broadcast" are wrong for another device's join. | D |

## 3. K1, criterion by criterion

| Criterion | Author | This review |
| --- | --- | --- |
| P2/P4 end to end | PASS | **PASS** on absolute latency. The "≤ 25 % of the CPU" clause passes even against a fair kernel (A), but it never discriminated. Drop "2,000 times". |
| P8 under load | PASS | **PASS** on p95 (221 µs). The 3.56× reference slowdown is unexplained. |
| P5 agreement | PASS | **PASS**: byte-identical on 2,048 inputs in two modes. It shows device = simulator, not that quantization is harmless. |
| P6 memory | PASS at resolution | **INCONCLUSIVE.** RSS passes (5.1 MiB). The leak question was not answerable with the meter used. |
| P7 SIGKILL | pending | pending, with the protocol changes in §7. |
| P9 duty cycle | PASS | **PASS for RSS.** CMA not assessable. |
| P10 exclusivity | PASS | **PASS for `flock` between two bench instances only.** Vision does not take the lock, so KPU sharing is untested. |

G4 lacks the criterion that matters: **necessity**. Add G4-N: a KPU backend is admitted only for a model that has passed G3 and cannot meet its rate on the CPU backend within the helper's budget.

## 4. What the latency opens

The headroom is real (F5). These are the uses worth it, in order.

| Opportunity | KPU cost | Why it matters | Verdict |
| --- | --- | --- | --- |
| **Larger encoder, longer context** (residual TCN, 128–256 packets, 0.5–4 G MACs) | 1.3–10 ms (A) | The only case where the KPU is necessary, not just nice | Research on real data first (§6) |
| **Deep ensemble plus augmented views** (8–16 passes) | 1–2 ms | Gives the neural layer its own CAN'T TELL. Disagreement is the standard remedy for the open-set weakness in F9 | Host study now |
| **Occlusion attribution** (N + 1 passes) | under 2 ms | Every salience item names the evidence to check, which strengthens the two-key rule | With any admitted scorer |
| **Plumbing sequence model**: a causal TCN over broadcast and multicast message tokens, with surprisal as salience | 0.1 ms per window | The one neural idea that fits the default vantage. It learns the normal join grammar and flags sequences no rule names | Research, on licensed LAN captures |
| **Session-adaptive novelty**: frozen encoder, online covariance on the CPU | one pass | Adaptive baselines with no on-device training and no retention | After a real-data encoder exists |
| **Specialist model zoo** | 1.2–1.6 ms load, about 80 kB each (V) | Ten loaded models cost under 1 MB | Later |

Not worth more compute: a neural window detector on telemetry (latency was never its limit) and the name-shape scorer.

## 5. Architecture recommendations

1. **Scorer contract first, CPU backend first, in process.** Embed each conversation once when it becomes eligible and again when it closes, inside the analyst tick under a time budget, in fixed-point C. This needs no batching, no new process and no decision D4. It works while Vision is open and is identical in host tests. Measure an optimized kernel on the unit before committing (§7).
2. **If a KPU backend is ever admitted, make it a supervised single-owner service** with a generic `infer.*` interface, like `sysd` and `netd`, not a child of `pos-capture`. It stays loaded, so nothing is stranded per session. It holds the deadline and recovery logic in one place, and ADR-016's process shape is untouched. Extract the existing nncase wrapper from Vision instead of writing a second one.
3. **Deadline and recovery around every KPU call** (F2):
   - an external timeout;
   - a SIGTERM handler that releases, with SIGTERM (never SIGKILL) as the parent-death signal;
   - the lock taken by every KPU client, Vision included, before it opens the device.
4. **Pin one backend per session** (F12).
5. **Replace the binary gate with loss-aware estimation.** Test the observed unanswered rate against what the measured loss predicts, and abstain only when the corrected interval is uninformative. As written, G0 is passed by always abstaining. Add a recall-under-loss clause.
6. **Treat vantage as a capability matrix,** and add LLDP, CDP, STP, router advertisements and IGMP to the plumbing observers. They reach every wired port and tell a technician where the jack goes.
7. **Close F1** along the lines reported privately. It needs a proposal under `AGENTS.md`.

## 6. Model recommendations

- **Traffic DNA.**
  - **Baseline:** the deterministic projection R3 already specified.
  - **Challengers:** a small (21 k), medium (~300 k) and large (~2 M) residual TCN.
  - **Data:** CESNET-TLS22 / QUIC22 (CC BY), with a time split and held-out services.
  - **Input:** matched to the dataset's packet-sequence length (A: 30), plus a flow-statistics branch.
  - **Training:** supervised-contrastive, with RTT, jitter, loss, truncation and GRO augmentation.
  - **Open-set:** ensemble disagreement, kNN distance and margin, with per-length thresholds.
  - **Admission:** a challenger only if it beats the projection on time-split recall. The KPU only if the medium or large model wins.
- **Anomaly detection.** Loss-aware rules first. A supervised TCN stays in research, but trained on a randomized catalogue in which every rule channel is exercised, with leave-one-fault-out over 10 seeds and non-negative fusion.
- **Device fingerprinting.** No neural model and no KPU. Use gradient-boosted trees or a hashed n-gram linear model over DHCP option order, vendor class, DHCPv6, mDNS service sets, LLDP and CDP. Train on Deakin and UNSW, evaluate by held-out device, and report accuracy under MAC randomization.
- **Predictive.** Keep "trend". Use Theil–Sen with a conformal interval, and hazard-style statements on plumbing timers ("renewal due, not seen"). A neural forecaster is NO-GO until station counters exist and G7 is met.

## 7. Changes to P7 before it runs on Unit B

1. **Exact meter.** Mount debugfs for the session and read `cma/reserved/used`. Owner's OK is needed; it reverts on reboot.
2. **User-space ledger.** Wrap the MMZ ioctls in the bench and print bytes allocated, freed and outstanding. This also covers single-page buffers, which no CMA meter sees.
3. **Answer P6 first, amplified:** 300 short KPU processes against 300 reference-only processes as a control, and 200 load-run-release cycles in one process. Pass means `used` and the ledger return exactly to baseline.
4. **Kill at three phases** (idle after load, mid-run, mid-load), at least 10 each, recording `used` per kill.
5. **After every kill, require a byte-identical first inference** from a fresh process, under an external watchdog. F2 predicts a pass, because open zeroes the flag.
6. **Test F2's hang once, deliberately.** While one instance runs back-to-back calls, have a second process open and close the device without the lock. Record whether the first hangs or returns wrong output. A hung process on Unit B is acceptable; it shows why every client must take the lock.
7. **Vision check after the series.** Run one detector inference, then reboot and confirm the baseline.
8. **Record CPU time and voluntary context switches** around the calls.
9. **Add three timing points** to test the slope in F5: the already-compiled 65.6 M MAC name model, an unbatched KPU-only call, and one stack near 1 G MACs compiled with random weights (no training).
10. **Measure an optimized CPU kernel** in the same session (F4).
11. **Stop rule.** Abort if `used` grows by more than 4 MiB or the Vision check fails.

## 8. GO / NO-GO

| Item | Decision | Condition |
| --- | --- | --- |
| N1: engine, ledger, MARK, WHEN on the CPU | **GO** | Measure the unit's own traffic rate first. Use the loss-aware gate. Correct the example. |
| P7 on Unit B | **GO** | With §7's changes. |
| K2 (`pos-infer` on the KPU) as specified | **NO-GO** | Reopen only when G4-N holds and D9 has given WHO real input. |
| CPU scorer backend inside the engine | **GO, after N1** | The optimized kernel meets the tick budget on the unit. |
| Real-data DNA study on the host | **GO** | Owner approves the download (D6). |
| D9: promiscuous-mode reversion test | **GO, ahead of any accelerator work** | Its own authorization. |
| Redo E3a honestly (hours, host only) | **GO** | Before any document cites the gate figures again. |
| Neural window detector; name-shape scorer; a16w8 | **NO-GO** | Unchanged. u8 must be re-judged on real data. |
| F1 mitigation | **Owner decision** | It is outside this PR's scope. |

## 9. Next tasks, in order

1. Decide F1.
2. Measure own-traffic evidence per minute in an ordinary capture, and schedule D9.
3. Run the amended P7 session on Unit B.
4. Host: redo E3a with a noisy, lagged coverage signal, faults inside gaps and 10 seeds.
5. Host: the CESNET study for Traffic DNA, with the ensemble open-set protocol.
6. Build N1.
7. Add the CPU scorer backend and the contract.
8. Revisit the KPU only under G4-N.

## 10. Corrections the PR text needs

- "About 2,000 times" (NEURAL.md, EXPERIMENTS, ROADMAP): state the baseline's nature or give the fair range.
- "P6 PASS at the available resolution": make it INCONCLUSIVE.
- "u8 does not change any decision" and "the neighbour's class changes for only 1.0 %": see F10.
- "Visibility gate … 76–90 % to 2–4 %" as a headline result: see F7.
- "Rejected only 0.2 %": one flow.
- The DHCP example in NEURAL §3: see F13.
- One further correction depends on F1 and is held with it.

## 11. Limits of this review

No device was touched, so F1 and F2's hang and stale-completion effects are derived from source, not demonstrated. F1's detail is withheld from this public file by the owner's decision. The CPU estimates in F4 are estimates. The slope in F5 rests on the compiler's model. I share a session history with the reviewed work; the three audits were done from code and raw data alone to offset that.
