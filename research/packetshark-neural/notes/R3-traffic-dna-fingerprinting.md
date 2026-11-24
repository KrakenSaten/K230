# R3 — Encrypted-traffic analysis, Traffic DNA, fingerprinting, periodicity, threats (desk research, 2026-11-24)

Desk research by a specialist agent. Key: V = primary source read this session; D = well-known source not re-read; A = estimate.

## The vantage point changes the question

- **Wi-Fi (managed):** own unicast + group-addressed frames only; with any station in power save, group frames wait for the DTIM beacon (timing rounded to ~100 ms × DTIM period); APs may proxy ARP/mDNS, convert multicast to unicast, or isolate clients (D).
- **Switch port:** own traffic, broadcast/multicast, occasional unknown-unicast floods (one-way fragments of others' traffic) (D).
- **Consequence:** other hosts' conversation rhythms are almost never visible. Conversation-level Traffic DNA mostly describes **the handheld's own flows** unless a SPAN port or tap is used — and a mirror port also needs promiscuous mode, which ADR-016 rule 3 keeps off until its reversion is verified. The universally visible, rich signal is **network plumbing** (ARP, DHCP/DHCPv6, mDNS, LLMNR, NBNS, SSDP).
- **Offload gotcha:** with GRO/LRO/TSO/GSO, AF_PACKET sees merged super-segments up to 64 KB; size sequences are meaningless unless offloads are off or such records are flagged (PacketShark labels GRO records `[GRO]`; whether they occur on the unit is a line-rate-gate measurement).

## Capability table (condensed)

| Capability | HEADERS? | Best candidate | Baseline | NPU | Licence notes | Main failure here |
|---|---|---|---|---|---|---|
| Conversation DNA | yes | deterministic projection (PCA→LDA, 32×112) first; optional ~36k-param 1-D CNN | standardised features + cosine; CUMUL, k-FP | no (< 1 ms/flow CPU, A) | CESNET CC BY 4.0 | only own flows visible; VPN aggregation; drift; offload super-segments |
| Payload-byte classifiers (ET-BERT, YaTC, NetMamba, TrafficFormer) | **no** | reject | — | — | — | shortcuts, not content (SoK S&P 2025: ET-BERT 0.91→0.59, YaTC 0.93→0.68 with identifiers randomised; SIGCOMM 2025 "Sweet Danger of Sugar"; CSCloud 2026: IP overlap adds 32–33 points) |
| Device inventory | yes (SSDP, WS-Discovery, IGMP are not on the plumbing list → policy extension) | rule-based evidence fusion with explanations | OUI + DHCP option 55 | no | Fingerbank unusable; Satori GPL-2.0; p0f LGPL-2.1; IEEE OUI no explicit licence | MAC randomisation (Android 10+/12+, iOS 18 rotating); AP multicast filtering |
| TCP stack fingerprint (p0f-style) | yes | own format | initial-TTL class | no | JA4T/JA4TS under FoxIO 1.1 | LAN devices' SYNs never seen; only remote servers' SYN-ACKs |
| TLS fingerprint (JA4) | **no** (transient parse = policy decision) | JA4 (BSD-3) if at all | — | no | JA3 defeated by Chrome extension permutation | fingerprints only the handheld itself; ECH (RFC 9849, 2026) |
| Beacon / periodicity | yes | RITA-style score verified by periodogram + autocorrelation (reimplement idea; RITA is GPL-3.0) | IAT CV with jitter tolerance | no | — | NTP, keepalives, push, telemetry, mDNS announce FPs; short captures; log-normal jitter evasion (ICNP 2026 "When Agents Look Like Beacons") |
| DGA / name risk | yes (names in stored DNS/mDNS/LLMNR/NBNS) | char CNN (2×128 filters) or B-ResNet | FANCI-style RF; entropy + bigram | **maybe** (batched fixed 64-char input) | UMUDGA MIT; DGArchive gated | CDN hash labels; Chromium's random 7–15-char probes; wordlist DGAs; ~69 FP/h at enterprise scale (Drichel et al. ARES 2020) |
| DNS tunnelling | yes | per-domain isolation forest | thresholds | no | — | only own DNS visible; DoH hides it |
| Port scan / ARP sweep | yes | TRW | distinct targets per window | no | — | only scans of the handheld itself; ARP sweeps visible from anywhere |
| ARP spoof / rogue DHCP / rogue RA | yes | binding table + conflict checks (exists in M12.1 for IPv4) | arpwatch | no | — | poisoning of other hosts uses invisible unicast; AP proxy ARP |
| Threat-intel match | yes | offline bundle with its date shown | exact match | no | Feodo Tracker and SSLBL CC0; URLhaus/ThreatFox conditional; Spamhaus DROP grants no licence | staleness; shared CDN/hosting IPs |

Fields to exclude from any learned model: IPs, MACs, raw ports, IP ID, checksum, seq/ack, window, TCP timestamps, TTL (shortcut evidence: SoK S&P 2025; netFound's constant TCP-timestamp shortcut).

## Recommended Traffic DNA v1 (192 bytes per conversation)

- **A (112 B):** s[0..31] int16 signed L4 payload length (+ initiator→responder, − reverse; pure ACKs and SYN/SYN-ACK skipped; 0 = padding) · t[0..31] uint8 IAT code `min(255, round(12·log2(1+Δt/64 µs)))` (~6 % steps, 64 µs–160 s) · f[0..31] 4-bit flags (PSH; FIN/RST; suspected retransmit; first after > 1 s idle).
- **B (48 B):** CESNET-style 8 log size bins × 2 directions; 8 log IAT bins × 2 directions; bursts (same direction, gap ≤ 10 ms); totals (packets/bytes out/in as 8·log2(1+x), duration, handshake RTT, pure-ACK fraction, class bits TCP/UDP, QUIC-likely, tunnel-likely, short).
- **C (32 B):** embedding, 32 × int8, L2-normalised. v1 = deterministic `z = norm(W·(x−μ))`, W 32×112 from PCA→LDA learned offline on CESNET-TLS-Year22/QUIC22 with a time split (3.6k MAC per conversation). v1.1 optional 1-D CNN (~36k params) trained contrastively with RTT-scaling, jitter, loss and truncation augmentation, shipped only if it beats v1 on time-split recall@10 and unknown-service rejection with identifiers masked.
- Eligibility ≥ 8 kept packets and ≥ 2 each way, else "unclassified: short". QUIC-likely = UDP whose initiator's first datagram ≥ 1200 B (RFC 9000 padding). Tunnels (WireGuard 148/92-byte handshakes, ESP, OpenVPN) get "encapsulated" and are never matched to app prototypes. Open-set: accept a prototype only with cosine ≥ τ (calibrated at 5 % FPR on held-out services) **and** a margin δ over the runner-up; in-capture clustering density-based, noise = "unclassified". CESNET PPI 1-D CNN: 97.04 % accuracy, 91.94 % unknown services detected at 5 % FPR (Luxemburk & Čejka 2023) — with full visibility at an ISP backbone.

## Fingerprinting and licences

- Visible plumbing: DHCP DISCOVER/REQUEST options (55/60/12/61), DHCPv6 Solicit (DUID-LLT embeds a MAC and a time), mDNS service types and TXT (model=, md=, fn=). IMC 2023 (Girish et al.): devices leak real names, UUIDs, geolocation over mDNS/UPnP → treat as personal data: display, never use as model inputs.
- **JA4+:** JA4 is BSD-3 with no patent claims; JA4S/H/L/LS/X/SSH/T/TS/TScan/D/D6 are under **FoxIO License 1.1 (non-commercial only, patent pending, non-commercial patent grant)** — incompatible with Apache-2.0's downstream rights; an independent reimplementation would not escape granted patents. **Avoid JA4+; define own TCP/DHCP fingerprint strings; legal review either way.**
- Fingerbank cloud API sends device data off the device (violates rule 11); offline DB is a licensed product → not usable. Satori GPL-2.0 (data licence unstated); p0f LGPL-2.1 (signatures from 2016); IEEE OUI CSV no explicit licence → legal review.

## Three genuinely novel handheld capabilities

1. **Presence DNA** — per-device plumbing fingerprint + announcement-rhythm spectrum (mDNS, DHCP renew, ARP, NBNS, MLD at ≥ 1 s resolution; DTIM rules out finer on Wi-Fi). AuDI (JSAC 2019) 98.2 % device-type accuracy from periodic traffic on 33 devices (full visibility). Optional, opt-in, session-local linking of randomised MACs (DHCP fingerprint + DUID + mDNS instance name) — opt-in because it undoes a privacy feature. No neural network needed.
2. **"Is this network tampering with me?"** — header-only path integrity over the handheld's own connections: many unrelated servers with an identical SYN-ACK fingerprint, hop count and RTT → terminating proxy / transparent firewall; MSS clamping → tunnel/PPPoE; two DHCP servers, rogue RAs, gateway ARP conflicts, DNS anomalies. Passive counterpart of tracebox/Netalyzr.
3. **Poisoning-exposure and name-risk radar** — hosts broadcasting LLMNR, NBT-NS or WPAD queries an attacker could answer (MITRE ATT&CK T1557.001); character-CNN name scoring with allowlists for Chromium probes and CDN labels — the one place the NPU plausibly helps (fixed 64-char input over thousands of names; ~6 MFLOP per name, A).

## Assumptions to challenge

- "Never turn endpoint names or addresses into model inputs" → keep for learned weights; drop as a blanket rule. Replacement: raw identifiers never enter training data, weights or the DNA embedding; they may be used for session-local joins, grouping and display; low-cardinality derived classes (address scope, same subnet, port service class, initial-TTL class, LA bit, OUI vendor) may be inputs; name scorers are a documented exception (string in, score out).
- "Other hosts' conversations are visible" → mostly not; make **Presence DNA** primary, not Conversation DNA.
- "IATs are a core feature" → vantage-dependent (DTIM, own CPU jitter, RTT): coarse log codes, normalise to handshake RTT.
- "Comparing only within a capture" → never yields a label; allow a small, shipped, non-identifying prototype library (expect LAN vs backbone domain shift).
- "A neural embedding is required" → reported gains are often leakage; ship the deterministic projection first.
- "Length-based unclassified is enough" → needs distance + margin rejection too; short flows are the majority and are described by their plumbing messages.
- "TCP fingerprinting of LAN devices" → not at this vantage; use DHCP and mDNS.

## Key references

Wickramasinghe et al. SoK S&P 2025 (arXiv 2503.20093); Zhao et al. SIGCOMM 2025 (arXiv 2507.16438); Yu et al. CSCloud 2026 (arXiv 2610.17961); FlowPic (INFOCOM WS 2019; IMC 2022; TNSM 2024; replication IMC 2023); nPrint CCS 2021; NetMamba ICNP 2024; TrafficFormer S&P 2025; netFound (arXiv 2310.17025); CESNET DataZoo and Luxemburk & Čejka Computer Networks 2023; FlowPrint NDSS 2020; CUMUL NDSS 2016; k-FP USENIX Sec 2016; RITA (github.com/activecm/rita); BAYWATCH DSN 2016; Drichel et al. ARES 2020; FANCI USENIX Sec 2018; Android MAC randomisation docs; Apple support 102509; Nadler et al. 2019 (DNS tunnelling); TRW S&P 2004; abuse.ch and Spamhaus terms; FoxIO-LLC/ja4 LICENSE + License FAQ; RFC 9849; Girish et al. IMC 2023; AuDI JSAC 2019; UMUDGA (Mendeley, MIT).

Not verified: TrafficFormer/FlowPic exact architectures; whether any FoxIO patent is granted; Satori/IEEE data licences; the K230 drivers' offload and timestamp behaviour.
