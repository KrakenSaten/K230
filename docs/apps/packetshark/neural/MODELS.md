# PacketShark NEURAL — candidate models, datasets and licences

**Research for owner review · 24 November 2026 · no model is shipped, no dataset was downloaded.** This lists what each detector would be built from, what it must beat, and what its training data would allow us to ship. The underlying desk research, with every source, is on the research branch ([R1–R6 notes](#sources)). Licence findings are research, not legal advice; the "ship weights" column uses a conservative allowlist (CC0, CC BY, MIT, MIT-0, explicit redistribution grants) because whether trained weights derive from their data is legally unsettled.

## 1. Candidates by capability

Ordered by the roadmap. "Baseline" is the cheapest method a candidate must beat on held-out sites at the same false-alert budget before it is admitted (gate G3). "KPU" says whether an accelerator could matter at all, from E1/E2 and R1.

| Capability (lens) | v1: deterministic | Baseline it must beat | Neural candidate | KPU | Inputs at this vantage |
|---|---|---|---|---|---|
| Visibility gate (all) | coverage from drops, shed, HOLD, one-way share; vantage | — (it is the baseline's precondition) | none | no | helper counters |
| Before/after at a mark (WHEN) | two-window robust tests, volume-normalised rates, BH correction | itself | none planned | no | series |
| Continuous change (WHEN) | CUSUM / Page–Hinkley on invariant ratios; robust T²/SPE with contributions | boxplot rule (Arp et al. 2022) | small TCN over [19 × 120] — **NO-GO on current evidence** (E3a: AUROC 0.65 unsupervised; supervised 0.90 but 0 % on an unseen fault) | not needed (0.7 M MAC) | series |
| Rhythm (WHO, WHEN) | median IAT, MAD, multiple-of-period share (E3c) | IAT coefficient of variation | none | no | own conversations, announcements |
| Doctor (WHY) | hypothesis graph, noisy-OR LR, LR ≥ 10 to accept | single-feature thresholds | none; LLMs excluded (R2: ~11 % on OpenRCA) | no | ledger |
| Presence DNA (WHO) | plumbing fingerprint strings (DHCP 55/60/12/61, DHCPv6 ORO/DUID, mDNS service types, LLMNR/NBNS) + announcement rhythm | OUI + option-55 lookup | gradient-boosted trees on the same fields, CPU | no | broadcast plumbing |
| Conversation DNA (WHO) | deterministic projection z = norm(W·(x − μ)), W 32 × 112 (PCA → LDA) learned offline, 3.6 k MAC per conversation (R3) | standardised hand-crafted features + cosine | 1-D CNN encoder, ~21–36 k parameters, contrastive with RTT/jitter/loss/truncation augmentation | **yes, batched** (13.3 M MAC per 64 conversations; est. 0.10 ms) | **the unit's own flows only** unless a mirror port and promiscuous mode are allowed |
| Name shape (WHO, WHY) | entropy + bigram score against a benign list; allowlists for Chromium probes and CDN labels | FANCI-style random forest | character CNN, 32 names × 64 characters | **yes, batched** (65.6 M MAC per 32 names; est. 0.23 ms) | names already stored under HEADERS — **needs decision D1** |
| Integrity (WHY) | header-only checks (identical SYN-ACK option sets, MSS clamping, NXDOMAIN rewriting, second DHCP server) | — | none | no | own connections, plumbing |
| Exposure (WHO) | LLMNR / NBNS / WPAD broadcasters | — | none | no | broadcast |
| Predictive health (WHEN) | nowcast: Theil–Sen trend with ≥ 10 points; host Wi-Fi station counters if netd can read them | persistence and a current-value threshold | GBT or logistic on windowed features; TCN only after GBT shows lift | no | series; nl80211 station info (ASSUMED available) |

Rejected outright: payload-byte traffic classifiers (ET-BERT, YaTC, NetMamba, TrafficFormer). They need payload that HEADERS does not keep, and the 2025 SoK found their accuracy falls from 0.91 to 0.59 (ET-BERT) and 0.93 to 0.68 (YaTC) once identifier and timing shortcuts are randomised (R3). Also rejected: JA4+ fingerprints other than JA4 itself (FoxIO License 1.1 is non-commercial and patent-pending, incompatible with Apache-2.0's downstream grant) and the Fingerbank cloud API (it would send device data off the unit, against ADR-016 rule 11).

**Fields that never enter a learned model or its training data:** IP addresses, MACs, raw ports, IP ID, checksums, sequence and acknowledgement numbers, window, TCP timestamps, TTL. They are the documented shortcuts (R3). Low-cardinality derived classes may be inputs: address scope, same subnet, port service class, initial-TTL class, the locally administered bit, OUI vendor.

## 2. The first two neural models, concretely

Both use only operators E1 placed entirely on the KPU, so one set of weights runs on the KPU, in the C reference backend and in the simulator.

**DNA encoder (`dna-b64`)**

```
input  [1, 3, 64, 32]  float32   channel 0: sign(direction) × L4 length / 1500
                                 channel 1: log1p(IAT ms) / log1p(1e5)
                                 channel 2: presence mask (0 = padding)
Conv(1×3, 3→32) ReLU · Conv(1×3, 32→64, stride 2) ReLU · Conv(1×3, 64→64, stride 2) ReLU
AveragePool(1×8) · Conv(1×1, 64→32)                     → [1, 32, 64, 1]
L2 normalisation on the CPU (E1 placed ReduceL2/Div on the CPU)
```

20,960 parameters, 27–30 KB as a kmodel (E1, E2). Eligibility before a conversation is embedded: at least 8 kept packets and at least 2 each way. Otherwise it is "unclassified: short". GRO-merged records (`[GRO]`) are excluded until the line-rate gate says whether they occur. The encoder answers "which of this capture's conversations look alike", never "this is Netflix". E3b shows why: neither the encoder nor hand-crafted features rejected an unseen traffic class at a similarity threshold (0 % rejected at 95 % known acceptance). A label would therefore need distance **and** margin rejection calibrated on real data, which is gate G3 work.

**Name-shape scorer (`names-b32`, only with D1)**

```
input  [1, 40, 32, 64]  one-hot characters (a–z, 0–9, '-', '.', '_', other), 32 names × 64
Conv(1×1, 40→32) · Conv(1×5, 32→64) ReLU · Conv(1×5, 64→64, dilation 2) ReLU
MaxPool(1×64) · Conv(1×1, 64→32) ReLU · Conv(1×1, 32→1) Sigmoid  → [1, 1, 32, 1]
```

The embedding is a 1 × 1 convolution over one-hot input because `Gather` runs on the CPU (E1, R1). The output is a shape score shown as salience ("unusual-looking name") beside the existing DNS evidence, never as a verdict. Published classifiers produced about 69 false positives an hour at enterprise scale (R3, Drichel et al. 2020). That is why the score is salience, not a claim.

## 3. Datasets

Vantage: **HOST** = one end host (matches the unit). **LAN** = all devices at a gateway, which can be re-sliced to one host plus broadcast. **BB** = ISP backbone, with no broadcast domain. Re-slicing a LAN capture keeps the host's own frames plus L2 broadcast and multicast. Managed Wi-Fi APs filter and proxy multicast, so wired sets overstate what the unit hears.

| Dataset | Licence | Ship derived weights? | Vantage | Best use |
|---|---|---|---|---|
| CESNET-TLS22 / QUIC22 / TLS-Year22 | CC BY 4.0 | **yes**, with attribution | BB, but per-flow first-packet sizes, directions, IATs | Conversation DNA (time-split evaluation; drift within weeks) |
| CESNET-TimeSeries24 | CC BY 4.0 | yes | per-IP aggregates | change and forecast priors |
| Deakin IoT Traffic 2024 | CC BY 4.0 | yes | pre-NAT LAN, 60 devices, ~120 days | Presence DNA, rhythms, benign baseline |
| UNSW IoT traces | MIT-0 | yes | LAN | Presence DNA |
| UNSW-IoTraffic 2025 | likely CC0 (unverified) | confirm first | LAN, 27 devices, 203 days | Presence DNA, rhythms |
| Kitsune | CC BY 4.0 | yes | IoT LAN | broadcast-borne anomalies (ARP MitM, SSDP flood) |
| CTU-13, IoT-23 | CC BY | yes | router / per device | beacon and C2 periodicity |
| LANL 2017 | rights waived | yes | enterprise NetFlow | periodicity |
| M-Lab NDT | CC0 | yes | client↔server tests | degradation curves, timing priors |
| CSE-CIC-IDS2018 | redistribution allowed with citation | yes, with attribution | per-machine pcaps | attack shifts (known CICFlowMeter flaws) |
| UMUDGA | MIT | yes | domain lists | name-shape positives |
| Majestic Million | CC BY 3.0 | yes | domain list | name-shape negatives |
| Legacy Fingerbank `dhcp_fingerprints.conf` (2012) | ODbL + DbCL | yes, with notice | fingerprint list | Presence DNA seed rules |
| CIC-IDS2017, ISCX VPN-nonVPN, CICIoT2023 | "for researchers" wording | ask in writing | MIR / HOST / testbed | evaluation only |
| IEEE OUI listing | no explicit licence | legal review | registry | vendor names |
| UNSW-NB15, MAWI, CAIDA, Ookla, Mon(IoT)r, DGArchive, Bambenek feeds, Tranco default list (includes BY-NC sources), current Fingerbank DB | research-only, non-commercial or no grant | **no** | — | — |

CIC-IDS2017 needs particular care. Engelen et al. (WTMC 2021) found errors in traffic generation, flow construction and labels, and rebuilt or relabelled more than 20 % of its traces. Treat it as an evaluation curiosity, not a benchmark.

## 4. Data strategy

1. **Now: generators and impairment emulation, on the host.** These are the E3 generators in [EXPERIMENTS.md](EXPERIMENTS.md), plus a scripted scenario source for `pos-capture`. The existing synthetic source has no scenario script (R6). Impairments come from `tc netem` (delay, jitter, loss, reordering), a resolver with injected latency and SERVFAIL, DHCP delays and capture-side loss (CPU load forcing kernel drops). Every run writes a ground-truth event log. Risk: a model learns the generator instead of the network. Mitigations: randomise everything, hold out whole generator families, and report accuracy on real captures only.
2. **Next: a consented bench corpus.** It contains only devices the project owns, on an isolated bench SSID or VLAN. Third-party broadcast is scrubbed: a MAC allowlist, salted MAC hashes that keep the OUI, pseudonymised host names. Each session records its provenance (scenario and seed, impairment log, firmware hash, site pseudonym, medium, UTC and NTP state, consent version, release licence CC BY or CC0). Splits go by site, never inside a session, forward in time within a site, with a frozen golden set of real incidents.
3. **Then: public sets for specific detectors.** See the table above, each with a NOTICE entry and a model card that lists every dataset and its terms.

Training fits the build host (Intel i7-1265U, 16 GB, no CUDA): E3a and E3b trained in 6–30 s each, and a 100–300 k-parameter DNA encoder on 1–5 M CESNET flows is a few hours (R4, ASSUMED). Export is PyTorch 2.1.2 → ONNX opset 13 → nncase 2.11.0 PTQ with calibration from the bench corpus, never from test data. The pinned environment already exists (`C:\K230-artifacts\ai-env\SETUP.md`), and nothing here needs a toolchain change.

## 5. Model card minimum

Before any weight file is proposed for the image: its purpose and the lens it serves; the exact input layout and the fields excluded; every training and calibration dataset with licence and hash; the split protocol; results against its baseline at the declared false-alert budget, per site; the quantization mode and the simulator-vs-float and device-vs-simulator agreement; the operators and their placement; its known failure modes at this vantage; its version, as cited in every ledger record it produces.

## Sources

Research notes on branch `research/packetshark-neural-lab` at [`29e1e8c`](https://github.com/KrakenSaten/K230/tree/29e1e8cc/research/packetshark-neural/notes), `research/packetshark-neural/notes/`: R1 KPU and nncase, R2 anomaly detection and diagnosis, R3 traffic analysis and fingerprinting, R4 datasets and licences, R5 handheld UX landscape, R6 codebase map. Each note lists its primary sources and what could not be verified.
