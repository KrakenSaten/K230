# R2 — Anomaly detection, adaptive baselines, predictive health, root cause (desk research, 2026-11-24)

Desk research by a specialist agent; some figures come from abstracts or snippets (marked in "Limits"). Device claims are ASSUMED.

## Bottom line

- Within one short session, **robust per-feature statistics** (median/MAD, CUSUM, Page-Hinkley), offline change-point search (PELT), **explicit cross-feature invariants** (answers/queries, SYN-ACK/SYN, retrans/segments, ARP replies/requests) and a cheap multivariate statistic (robust Hotelling T² + SPE with contribution plots) are what to beat. Evidence that deep models add lift is weak: Audibert et al. 2022 (no family dominates across 16 methods), Sarfraz et al. ICML 2024 (SOTA deep reduced to linear, on par), TSB-AD NeurIPS 2024 (simpler often better), and network-specific: Arp et al. USENIX Sec 2022 — a boxplot rule on packet frequency matched Kitsune (AUC 0.998 vs 0.968) and beat it at low FPR.
- The binding constraints are a clean reference, calibrated false-alarm control and labels — not compute. The NPU helps none of the per-session detectors at this scale (ASSUMED: dispatch overhead dominates; int8 shifts the score distributions calibration relies on).
- **Nowcasting** QoE from passive metrics is well supported (Prometheus HotMobile 2014; Bronzino et al. 2019; Sundaresan IMC 2013: above ~16 Mbps DNS and TCP setup dominate page load; Padmanabhan CoNEXT 2006: 34–42 % of web failures DNS-related). **Lead-time prediction** evidence is sparse: Höchst et al. LCN 2019 predicted Wi-Fi loss 15 s ahead (with phone motion sensors). Host-side nl80211 station counters (signal, bitrate, retries, beacon loss, CQM events) are the most credible lead signals; availability on rtl8189fs is unverified.
- **LLMs must not be the detector or diagnostician:** OpenRCA (ICLR 2025) best ≈ 11 % of cases; NIKA/NetPress ≈ 24 % correctness; failure taxonomies: fabricated evidence, anchoring.

## Methods and costs (F ≈ 32 features at 1 Hz; all ≪ 1 % of one hart, ASSUMED from op counts)

| Task | Rank | Method | Cost | NPU | Must beat |
|---|---|---|---|---|---|
| (a) change around a mark | 1 | two-window test (recent vs reference, masked bins out), robust z + exact Poisson/Fisher tests, BH across features | F·W ops, KB | no | itself |
| | 2 | PELT / binary segmentation on the pre-mark series (change time + effect size) | O(n) | no | rank 1 on change-time error |
| | 3 | truncated BOCPD (Poisson-Gamma counts, Normal-Gamma log RTT) → P(change in [m−Δ, m]) | ~300 ops/feature | no | ranks 1–2 on calibration |
| (b) continuous multivariate shift | 1 | CUSUM/Page-Hinkley on robust residuals + invariant ratios; ARL0 from the alert budget | ~10 ops/feature | no | boxplot rule |
| | 2 | robust T²/SPE + contribution plots | ~1k MAC, 8 KB | no | (b)1 |
| | 3 | LODA (tolerates missing variables, attributes features) or HS-Trees | ~1k ops, 50–400 KB | no | (b)2 |
| | 4 | KitNET / small TCN-AE | 1e4–1e6 MAC | no (too small) | (b)2–3 at equal FA/h — **default NO-GO** |
| (c) rhythm change | 1 | per-source IAT median/MAD, autocorrelation or Lomb-Scargle, missed-event tests (BAYWATCH DSN 2016) | O(events) | no | "gap > k × median IAT" |
| | 2 | streaming left Matrix Profile | ~3.6k ops/feature | no | (c)1 |
| (d) predictive health | 1 | Theil-Sen trend + persistence on host Wi-Fi counters and handshake RTT | tiny | no | threshold + persistence |
| | 2 | GBT / logistic on windowed features | µs | no | (d)1 |
| | 3 | LSTM/TCN forecaster | small | not needed | (d)2 — NO-GO until (d)2 lifts |

## Adaptive baselines and false-alarm control

- Cold start: no finding until each metric has a minimum count (e.g. ≥ 120 valid bins, ≥ 30 DNS transactions); early findings say "relative to session start" (Kentik "learning mode" precedent).
- Freeze the reference while any detector alerts or HOLD is on; winsorise reference updates (PCA/AE references get contaminated or poisoned: Ringberg SIGMETRICS 2007, ANTIDOTE IMC 2009); drift via ADWIN/Page-Hinkley.
- Across sessions (only if persistence is allowed): mergeable quantile sketches (DDSketch, KB-sized), BOCPD priors, calibration offsets — never identifiers; only as a prior the session re-checks.
- Missing data: validity mask per bin; CUSUM/BOCPD skip invalid bins; LODA marginalises; neural needs mask inputs (GRU-D).
- Small samples: Wilson/Jeffreys intervals for rates; p95 only with n ≥ 59; median CI needs n ≥ 6; trend only when the Theil-Sen interval excludes 0 with ≥ ~10 points.
- Calibration: conformal p-values with BH (Bates et al. 2023) — min p = 1/(n+1), exchangeability fails under autocorrelation → calibrate on blocks; anytime control via conformal test martingales or e-detectors (Ville's inequality); adaptive conformal holds long-run rate under drift; selective prediction tuned on the risk-coverage curve.

## Diagnosis from one passive vantage

Transferable: Sherlock (SIGCOMM 2007, up/troubled/down states, noisy-OR), NetMedic (SIGCOMM 2009, 30–60 min of history suffices), NetPoirot (SIGCOMM 2016, single-endpoint TCP stats classify the failed component), 007 (NSDI 2018, retransmissions across many unrelated destinations → shared segment; concentrated → that destination). Commercial confidence = corroboration + persistence (Mist classifiers sum to 100 % of failures; ThousandEyes needs several vantage points); the handheld's corroboration must come from **cross-layer consistency**.

Proposed: (1) visibility gate first (drops/shed/HOLD/one-way → no hypothesis); (2) hypotheses with signatures — local link, address (DHCP), resolver r, upstream path, destination d, **no network evidence**; (3) log-likelihood ratios per evidence item (noisy-OR + leak); (4) accept the top hypothesis only with minimum n and LR ≥ 10 over the runner-up, else show both and the discriminating observation.

## GO/NO-GO metrics (event-level; never point-adjusted F1)

- False alerts per observed hour on held-out benign sessions ≤ 0.2 per detector family, ≤ 0.5 total, judged on the upper Poisson bound (0 alarms → needs ≥ 15 benign hours for 0.2/h).
- Precision ≥ 0.8 at recall 0.5; errors among accepted findings ≤ 5 % (Wilson upper) at coverage ≥ 60 %; ≥ 99 % of injected visibility-fault segments produce no affirmative finding; change-time error median ≤ 10 s; diagnosis top-1 ≥ 90 % among accepted; lead time median ≥ 15 s at precision ≥ 0.7 beating persistence (paired bootstrap by site).
- **Neural GO:** pre-registered lift ≥ 10 precision points at equal FA/h over the cheapest baseline, interval across sites excluding 0.
- Protocol: leave-site-out, never split a session, time-ordered within site, calibration sites ≠ test sites, no normalisation across test data, fault injection upstream (netem loss/delay, resolver delay/failure, DHCP outage, AP attenuation, CPU load to force drops), independent ground truth, freeze thresholds before the single test run, boxplot/persistence/random baselines in every table.

## Assumptions to challenge

1. Deep beats statistics. 2. Published F1 transfers (point adjustment). 3. Unsupervised needs no clean data. 4. 99 % specificity is good (3,600 bins/h × 1e-3 = 3.6 FA/h — base-rate fallacy, Axelsson 2000). 5. Anomalous means broken (Sommer & Paxson 2010). 6. Conformal guarantees hold on time series. 7. Prediction is the next easy step. 8. An LLM can do RCA. 9. The NPU makes AI cheap.

## Key references

Mirsky et al. Kitsune NDSS 2018; Arp et al. USENIX Sec 2022; Wu & Keogh TKDE 2021; Kim et al. AAAI 2022; Audibert et al. Pattern Recognition 2022; Sarfraz et al. ICML 2024; Liu & Paparrizos TSB-AD NeurIPS 2024; Adams & MacKay BOCPD 2007; Killick et al. PELT JASA 2012; Yeh et al. Matrix Profile ICDM 2016; Pevný LODA 2016; MacGregor & Kourti 1995; Bates et al. AoS 2023; Shin/Ramdas/Rinaldo e-detectors 2024; Gibbs & Candès NeurIPS 2021; Geifman & El-Yaniv NeurIPS 2017; Masson et al. DDSketch PVLDB 2019; Che et al. GRU-D 2018; Höchst et al. LCN 2019; Salfner et al. CSUR 2010; Tatbul et al. NeurIPS 2018; Bahl et al. Sherlock SIGCOMM 2007; Kandula et al. NetMedic SIGCOMM 2009; Arzani et al. NetPoirot SIGCOMM 2016 and 007 NSDI 2018; Xu et al. OpenRCA ICLR 2025; Hu et al. BAYWATCH DSN 2016; Linux nl80211 UAPI.

Limits: Kitsune-vs-boxplot figures, commercial docs and NIKA/NetPress numbers from abstracts/snippets; no numeric confidence UI documented in Marvis/ThousandEyes/Kentik; nothing about the unit's Wi-Fi counters or NPU verified.
