# R4 — Datasets, pretrained models, licences, training pipeline (desk research, 2026-11-24)

Desk research by a specialist agent; **nothing was downloaded**. Tags: [V] read on the maintainer's page, [S] secondary/snippet, [U] unverified. Not legal advice: whether trained weights are a derivative of training data is unsettled, so use a conservative allowlist (CC0, CC BY, MIT/MIT-0, the LANL waiver, ODbL "Produced Work" with notice, explicit CIC redistribution statements) and ship a NOTICE + model card listing every training dataset.

Vantage: HOST (one end host = matches the handheld), LAN (gateway/LAN side incl. broadcast, re-sliceable per device), MIR (mirror port, re-sliceable), BB (backbone, no broadcast domain). Re-slicing recipe: keep frames where the host's MAC/IP is source or destination plus L2 broadcast/multicast, drop other unicast. Caveat: managed Wi-Fi APs filter/convert multicast and proxy ARP, so wired sets overstate broadcast.

## Datasets (condensed)

| Dataset | Licence | Ship weights? | Vantage | Best use |
|---|---|---|---|---|
| CESNET-TLS22 / QUIC22 / TLS-Year22 (DataZoo) | CC BY 4.0 [V] | **Yes** | BB, but per-flow sequences of first-packet sizes/directions/IATs (header-derived) | **Traffic DNA** (labels from SNI; backbone mix ≠ home) |
| CESNET-TimeSeries24 | CC BY 4.0 [V] | Yes | per-IP aggregates 10 min/1 h/1 day | baseline/forecast pre-training |
| UNSW IoT traces (Sivanathan) | MIT-0 [V] | Yes | LAN | device type (c), periodicity |
| Deakin IoT Traffic 2024 | CC BY 4.0 [S] | Yes | pre-NAT LAN, 24 IoT + 36 non-IoT, ~120 days | device type, rhythms, benign baseline |
| UNSW-IoTraffic 2025 (Dryad) | likely CC0 [U] | confirm | LAN pre-NAT, 27 devices, 203 days | c, d, baseline |
| Kitsune (UCI) | CC BY 4.0 [S] | Yes | surveillance/IoT LAN, ARP MitM, SSDP flood | broadcast-borne anomalies |
| CTU-13 | CC BY [V] | Yes | university router, botnet pcaps | C2 periodicity |
| IoT-23 | CC BY [S] | Yes | per-device, HeartBeat/C&C labels | periodicity, threat |
| LANL 2017 | rights waived [S] | Yes | enterprise NetFlow + host logs | periodicity, shifts |
| M-Lab NDT | CC0 [S] | Yes | client↔server tests with TCP_INFO | degradation curves, timing priors |
| CSE-CIC-IDS2018 | redistribution allowed with citation [V] | Yes (attribution) | per-machine pcaps (AWS testbed) | attack shifts — CICFlowMeter flaws persist |
| CIC-IDS2017 / ISCX VPN-nonVPN / CICIoT2023 | "publicly available for researchers" [V]; CIC index allows redistribution with citation [S] | ask in writing | MIR / HOST / testbed | evaluation only; Engelen et al. WTMC 2021 found >20 % of traces needing rebuild/relabel; Flood et al. EuroS&P 2024 "design smells" |
| UNSW-NB15 | academic; commercial needs authors' agreement [S] | **No** | IXIA synthetic | avoid |
| MAWI, CAIDA, Ookla, Mon(IoT)r, IoT Sentinel (TU Darmstadt copy), RIPE Atlas (commercial needs permission) | research-only / NC | **No** / ask | | — |

DGA and lists: **UMUDGA (MIT)** positives; 360netlab/DGA repo (MIT, feed terms unclear); **Majestic Million (CC BY 3.0)** benign negatives; Tranco default list **blocked** (includes Cloudflare Radar BY-NC; a Majestic-only custom Tranco list is OK); DGArchive (vetted, no public ToS) and Bambenek feeds (commercial derivative needs licence) **blocked**; Umbrella top-1M grey. Fingerprints: legacy Fingerbank 2012 `dhcp_fingerprints.conf` ODbL+DbCL (OK with notice); current Fingerbank DB/API **blocked**; IEEE OUI listing grey/low risk (no explicit licence; Wireshark `manuf` is GPL-2.0+).

## Pretrained models

None is small, cleanly licensed and trustworthy for header-only use — **train our own**. ET-BERT (payload, ~110M; SIGCOMM'25 Zhao et al.: random-init ≈ pretrained; SoK arXiv 2503.20093: 0.59 accuracy once leakage features randomised, YaTC 0.68) — reject. YaTC (no licence) — reject. netFound (MIT, 644M) — laptop teacher at most. nPrint (Apache-2.0, encoder, no weights) — usable encoder. Kitsune-py (MIT) — baseline. DGA HF models trained on Bambenek data — reject; retrain. Chronos-Bolt-tiny (Apache-2.0, 9M) — offline zero-shot baseline/teacher for forecasting; Moirai (BY-NC) — reject.

Agent's claim to check: "Kendryte's nncase docs list LSTM and GRU as supported ONNX ops". **E1 contradicts this for GRU on nncase 2.11.0** (importer: `Not Supported Onnx op: GRU`, VERIFIED on this host); LSTM imports and lands on the KPU with three CPU `GetItem` ops.

## Data strategy

1. **Now — synthetic + impairment emulation:** scripted clients (dig, curl, iperf3, ping, NTP, avahi/SSDP, DHCP renew loops), tc/netem delay/jitter/loss/reorder/rate, an unbound resolver with injected latency/SERVFAIL, DHCP delays, AP attenuation; the generator writes a ground-truth event log. Risk: the model learns the generator (constant timing, one OS stack, identical TTL/options, square onsets) → randomise, mix real bench background, hold out whole generator families, report on real captures only, audit shortcuts (Trustee, Jacobs et al. CCS'22).
2. **Next — consented bench corpus:** only project-owned devices on an isolated bench SSID/VLAN, written consent; scrub third-party broadcast (MAC allowlist, salted MAC hash keeping OUI, pseudonymised host names); per-session provenance (scenario + seed, impairment log, firmware hash, site pseudonym, medium, UTC/NTP state, consent version, release licence CC BY/CC0; "Datasheets for Datasets"); group splits by site, whole sessions, forward-chaining time, unseen device models held out, frozen golden test set of real incidents.
3. **Then — public data per detector:** DNA → CESNET; device type → Deakin, UNSW IoT, UNSW-IoTraffic, legacy Fingerbank, OUI; periodicity → CTU-13, IoT-23, LANL; names → UMUDGA + Majestic; shifts/forecast → CESNET-TimeSeries24, M-Lab NDT, CSE-CIC-IDS2018 re-sliced per host; CIC-IDS2017 and the DistriNet corrected sets for evaluation only until terms are confirmed.

## CPU-only training (i7, 16 GB, no CUDA; ASSUMED times)

Window baselines minutes; < 50k-param 1-D CNN/AE < 1 h; 100–300k-param DNA embedder on 1–5M CESNET flows a few hours; LightGBM device fingerprints minutes; periodicity needs no NN; char-CNN on ~2M domains < 1 h. Export PyTorch → ONNX (opset the pinned nncase accepts) → nncase PTQ with bench calibration data. Tree models run on the CPU in C. SHA-256 manifests, fixed seeds, model cards with datasets and licences.
