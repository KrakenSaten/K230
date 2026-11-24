# PacketShark NEURAL: handover

**24 November 2026.** Concise state of the research, the decisions that belong to the owner, and the decisions that warrant an independent Astra or Fabel review before anyone builds on them.

## 1. Where this stands

| Item | State |
| --- | --- |
| Design documents | This PR (#186, draft, branch `docs/packetshark-neural-concepts`): [NEURAL.md](../NEURAL.md) (revision 2), [ENGINE.md](ENGINE.md), [MODELS.md](MODELS.md), [EXPERIMENTS.md](EXPERIMENTS.md), [ROADMAP.md](ROADMAP.md), this file. Documentation only. |
| Experiments | Branch `research/packetshark-neural-lab` at [`29e1e8c`](https://github.com/KrakenSaten/K230/tree/29e1e8cc/research/packetshark-neural) (pushed, not for merging), directory `research/packetshark-neural/`: scripts, JSON results, the K1 kit (`poc/`), desk research notes R1–R6 with sources. Larger run outputs (kmodels, simulator outputs, dumps) are in WSL `~/work/psk-neural/runs` and copied to `C:\K230-artifacts\packetshark-neural\` with SHA-256 sums. |
| Code in DOORS | None. Nothing under `core/`, `tools/`, `apps/` or `tests/` changed on either branch. |
| Units | Neither Unit A nor Unit B was touched. Nothing was deployed or merged. |
| Validation repeated | None. M11.1/M12.1 and Vision results were taken from their documents, not re-run. |
| Built | `infer-bench` for riscv64 against the pinned SDK's nncase 2.11 runtime, and for the host. All E1–E3 runs on the build host. No DOORS build was needed, because the PR changes only Markdown. |
| Not tested | Anything on a K230: KPU latency, call overhead, CMA behaviour of a tiny model, device vs simulator agreement. Any real traffic. Any person using the proposed screens. The C reference's speed on the C908. |

## 2. Owner decisions

Each has a recommendation. None is assumed by the documents.

| # | Decision | Recommendation |
| --- | --- | --- |
| **D1** | Replace the brief's "never turn endpoint names or addresses into model inputs" with a narrower rule. Raw identifiers never enter training data, learned weights or embeddings. They may be joined and shown session-locally (DNS name binding). A name-shape scorer may read a name string and emit only a score. | Accept the narrower rule. Defer the name-shape scorer itself until G3 and G4. |
| **D2** | Features computed from payload the HEADERS rule does not keep (TLS ClientHello fields, JA4) | **No.** At this vantage it would fingerprint mostly the unit itself. JA4+ (all but JA4) is under a non-commercial, patent-pending licence that conflicts with Apache-2.0 (R3). ADR-016 rule 7 stays as it is. |
| **D3** | Opening a saved capture on the unit for replay through the helper's file source | Not yet. Host-only virtual-clock replay for fixtures needs no decision (N1). Revisit after G5. It changes ADR-016 rule 10 and `tests/packetshark_lint.sh`. |
| **D4** | Let `pos-capture` start one scorer child (`pos-infer`: no capabilities, no files but its model, numeric tensors only), and a KPU lock that Vision's helper takes too | Defer until K1 passes G4. Then write an ADR-016 amendment and a Vision ADR note. If G4 fails, never. |
| **D5** | Retention beyond the session (per-network baselines, saved evidence cards) | No for v1. Everything is session-local. Revisit after G2, with a separate retention decision. |
| **D6** | Policy for shipping model weights: a licence allowlist (CC0, CC BY, MIT, MIT-0, explicit grants), a NOTICE entry and a model card per weight file | Adopt the policy now, ship nothing before G3/G4. Legal review for CIC "for researchers" wording, IEEE OUI, legacy Fingerbank ODbL. |
| **D7** | A new Companion aggregate (for example, the number of open episodes) | Defer. Propose only after N1, through the ADR-016 amendment 1 allowlist review. |
| **D8** | DS §59 amendment: MARK on the capture bar, the INSIGHT face of LIVE, four verdict states including CAN'T TELL | Review mockups during N1 before code. |
| **D9** | Schedule the promiscuous-mode reversion test on hardware (ADR-016 rule 3) | Yes, as its own small authorized test. It is the single biggest unlock for WHO (mirror-port conversations) and is independent of this work. |
| **D10** | Opt-in, session-local linking of randomised MACs in Presence DNA | No for v1. It undoes a privacy feature of the devices around the unit. |

## 3. Decisions that warrant independent Astra or Fabel review

Give the reviewer the code and the documents, not this page's conclusions.

| Review | Question for the reviewer | Material |
| --- | --- | --- |
| **A. Product premise (highest)** | The person's symptom is usually on another device, and its unicast is invisible here. Is a WHEN lens that leads with plumbing and the unit's own, possibly sparse, traffic useful enough to come first? Or should mirror-port mode (D9) come before it? | NEURAL §2 last row, §3; PACKETSHARK.md §2; R3 §0 |
| **B. Experiment validity** | E3a and E3b use generators written by the same author as the baselines. Are the conclusions limited enough? The conclusions are: gated rules beat the tested neural models; the gate cuts false alerts on visibility artifacts from 76–90 % to 2–4 %; threshold-based open-set rejection fails. Which would survive real data? | EXPERIMENTS.md; `synth_telemetry.py`, `exp_window.py`, `exp_dna.py` |
| **C. Gates and statistics** | Are G0, G3, G6 and G7 the right bars? Is "≥ 15 benign hours for ≤ 0.2 false alerts per hour" achievable with a consented bench corpus? Is LR ≥ 10 the right acceptance margin for the Doctor? | ROADMAP §2; R2 |
| **D. Scorer security (D4)** | Attack surface of a child of the capture helper that loads a closed runtime and opens `/dev/mmz` and `/dev/k230-gnne` as uid 0 without capabilities. Is the lock sufficient given the driver's missing arbitration? Is the CMA loss on kill a denial-of-service vector? | ENGINE §5; R1; docs/vision/KPU_CMA_LEAK.md |
| **E. K1 protocol** | Measurement validity (CMA noise after drop_caches, profiling overhead), and safety of the SIGKILL test on Unit B | ROADMAP §1; `poc/README.md` |
| **F. Privacy rules (D1, D10)** | The narrowed identifier rule against ADR-016. mDNS and DHCP host names are personal data (R3: IMC 2023). Is display without learning enough? | NEURAL §2; MODELS §1; ENGINE §9 |
| **G. Licences (D6)** | The dataset and weights allowlist, JA4/JA4+, the legacy Fingerbank file, the IEEE OUI list | MODELS §3; R4 |

## 4. Biggest risks

1. **Sparse evidence.** If the unit's own traffic is thin, WHEN will mostly say CAN'T TELL on DNS and TCP. Measure the unit's own DNS pairs and handshakes per minute during an ordinary capture before N1 design closes.
2. **Call overhead.** Unknown overhead per KPU call could erase the 100× ideal advantage the compiler estimates. K1 settles this.
3. **Generator circularity.** A detector tuned on generated data may learn the generator (R4). Accuracy claims wait for the consented corpus (G3).
4. **The gate leaks rates.** The gated rules still raised alerts on 2–4 % of visibility-artifact windows, short of G0's 99 % abstention. The likely cause is rates whose denominator spans a lossy bin. It must be fixed and re-measured in N1.
5. **KPU sharing.** Exclusivity with Vision relies on a lock that both helpers honour. The driver will not enforce it.

## 5. Resume here

1. Read NEURAL.md §2 and HANDOVER §2. Collect the owner's answers to D1–D10.
2. Commission reviews A and B first. They can change the order of the roadmap.
3. If K1 is authorized, follow `research/packetshark-neural/poc/README.md` on Unit B, with Vision closed and no capture, from `/tmp`. Record the results against ROADMAP §1.
4. Otherwise start N1 with fixtures before code. First come the four replay cases of the first brief as scripted scenarios with expected ledgers, then the measurement of the unit's own background traffic.
