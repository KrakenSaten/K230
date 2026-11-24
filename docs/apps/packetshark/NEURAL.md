# PacketShark NEURAL

**Design study for owner review · revision 2 · based on master `448327d` · 24 November 2026 · nothing implemented, nothing deployed, Unit A and Unit B untouched**

**Recommendation: one witness, three lenses.** Build one intelligence engine inside the capture helper. It is an evidence ledger fed by cheap observers and guarded by a visibility gate. Present it through three lenses on the same evidence:

- **WHEN** marks a symptom and compares before with after.
- **WHO** shows who is present and what the unit's own conversations resemble.
- **WHY** names competing explanations and the observation that would settle them.

Ship WHEN first, on the CPU, with no model. Use the KPU for one thing first: a batched embedding of conversations. It runs behind a two-key rule (no model output is ever the sole basis of a claim) and only after a bench proof of concept with measurable GO/NO-GO criteria.

Host-side experiments for this revision gave three results that drive the design:

1. **The visibility gate is worth more than any model tried.** On a synthetic test set, rules without it raised alerts on 76 % of windows with capture loss and 90 % with one-way visibility. With it, 2–4 %.
2. **The KPU compiles telemetry-shaped models,** but only from a narrow operator set. It pays only for batched work.
3. **The default vantage sees network plumbing far better than conversations.** This changes what the second lens can be.

The first revision of this brief (`41a6f5d`) chose between three products. This one treats them as views of one engine and challenges seven of its assumptions (§2).

| Document | What it holds |
| --- | --- |
| This page | Vision, portfolio, navigation, the decisions in brief |
| [neural/ENGINE.md](neural/ENGINE.md) | Shared engine: observers, ledger, analysts, scorer contract, replay, budgets |
| [neural/MODELS.md](neural/MODELS.md) | Candidate models, datasets, licences, data strategy |
| [neural/EXPERIMENTS.md](neural/EXPERIMENTS.md) | What was run on the host, the results, how to reproduce them |
| [neural/ROADMAP.md](neural/ROADMAP.md) | First KPU proof of concept, GO/NO-GO criteria, phased roadmap |
| [neural/HANDOVER.md](neural/HANDOVER.md) | Where this stands, owner decisions, what warrants independent review |

## 1. Evidence at the starting point

| Status | Consequence for design |
| --- | --- |
| **Verified in source** | `pos-capture` owns parsing, rings, signals and network tables. The shell polls a bounded line protocol and caches 48 rows ([PACKETSHARK.md](../PACKETSHARK.md) §3). `on_frame()` sees the whole frame before the HEADERS rule cuts it (`tools/capture/pos_capture.c`). Per-second bins hold only per-class packet counts. DNS and handshake times exist as cumulative sums in `diag`. The file source replays a pcap but loses direction and has no recorded-time clock. Map: R6 on the research branch. |
| **Documented hardware observations** | Unit A: one Linux-visible hart, 1 GiB RAM with 512 MB CMA. Switched Ethernet and managed Wi-Fi give the unit its own traffic plus broadcast and multicast. PacketShark offers no promiscuous mode (ADR-016 rule 3). The line-rate gate is still open ([T-DISPLAY-K230.md](../../hardware/T-DISPLAY-K230.md), [PACKETSHARK.md](../PACKETSHARK.md) §2). |
| **Verified on this host (new)** | nncase 2.11.0 places temporal Conv2D stacks, pooling, constant Gemm and batched inputs entirely on the KPU. It refuses GRU, LayerNormalization and Erf under quantization. Softmax, Gather and ArgMax stay on the CPU. Without quantization nothing reaches the KPU ([EXPERIMENTS.md](neural/EXPERIMENTS.md) E1). |
| **Verified in driver source (new)** | `/dev/k230-gnne` does no arbitration between processes, so only one process may use the KPU at a time. The MMZ driver frees nothing when a process dies ([KPU_CMA_LEAK.md](../../vision/KPU_CMA_LEAK.md); R1). |
| **Accepted constraints** | Capture is started by a physical START, stays memory-only until SAVE, defaults to HEADERS and is receive-only ([ADR-016](../../decisions/ADR-016-packet-capture.md)). A paired Companion sees only allowlisted aggregates ([amendment 1](../../decisions/ADR-016-amendment-1-companion.md), [gateway contract](../../api/gateway.md)). |

## 2. Seven assumptions this revision challenges

| The first brief assumed | What we found | So |
| --- | --- | --- |
| Timeline, Atlas and Doctor are **alternatives**, to be chosen by field trials | They need the same parts: a per-second series keyed by packet time, an evidence ledger, a visibility gate | **One engine, three lenses.** Build the engine once. The order of the lenses is the order in which the engine can support them end to end. |
| Atlas maps **who spoke with whom** | At the default vantage other hosts' unicast is invisible. Conversation shapes describe mostly the unit's own flows. Plumbing (DHCP, ARP, mDNS, LLMNR, NBNS) is visible from every port (R3; [PACKETSHARK.md](../PACKETSHARK.md) §2) | **WHO = presence of everyone + conversations of the unit itself.** A full conversation map waits for mirror-port capture, which needs promiscuous mode verified on hardware first. |
| The KPU is where the intelligence runs | A per-window model is 0.07–0.7 M multiply-accumulates (MACs): trivial on the CPU. Only batched scorers reach 13–66 M MACs per call. The call overhead is undocumented (E1, E2, R1) | **The KPU accelerates one or two batched scorers,** measured first on a bench. The engine is complete without it. |
| A small temporal model can rank multivariate shifts better than rules | Synthetic E3a: domain rules AUROC 0.88; unsupervised TCN 0.65; supervised TCN 0.90 but 0 % on a fault type it never saw. A learned fusion even learned to suppress the SERVFAIL rule. The literature agrees (R2: a boxplot rule matched Kitsune) | **Rules and statistics are v1.** A neural detector is admitted only by gate G3 on real, consented data, and may only reorder evidence, never suppress it. |
| A traffic shape can be called "unclassified" when short or ambiguous | E3b: the encoder beat hand-crafted features (93 % vs 89 % on known classes) but rejected only **0.2 %** of two unseen classes at 95 % known-acceptance | **Show resemblance within the capture, not labels.** Names come only from deterministic evidence: the DNS answer that resolved the address. |
| "Never turn endpoint names or addresses into model inputs" | Right for learned weights (identifiers are the documented shortcut). Too broad as a rule: names are already stored and shown, and binding a DNS name to a conversation is the best header-only label (R3) | **Narrower rule (decision D1):** raw identifiers never enter training data or weights. They may be joined and shown session-locally. A name-shape scorer is a named exception. |
| The symptom's own traffic is there to compare ("DNS replies slowed · 8 / 6 pairs" in 20 s) | The person's symptom usually happens on another device, whose DNS and TCP are invisible here. The unit's own DNS and TCP come only from its own services, cannot be provoked by PacketShark, and may be sparse. Plumbing, by contrast, reaches every port | **WHEN leads with shared-infrastructure and plumbing evidence** (DHCP, ARP, the resolver and router the unit shares). On the unit's own DNS/TCP it abstains honestly ("this unit asked 2 questions in that minute"). How sparse the unit's own traffic really is gets measured first (N1); G2 includes quiet captures. |

Two smaller corrections:

- **Prediction:** evidence for lead time from passive metrics is scarce, while estimating the current state ("nowcasting") is well supported (R2). So the product says *trend*, not *forecast*, until a lead-time gate is met.
- **Periodicity:** a rhythm claim needs 5–6 repetitions at 1–5 % jitter, 10–15 at 15 % and up to 30 at 30 % (E3c). In the default 15-minute capture, only periods of about three minutes or less can be claimed. Periodic traffic is mostly benign (NTP, keepalives, announcements). So the product shows *rhythm* and never calls anything a *beacon*.

## 3. The vision: a witness, not an oracle

DOORS cannot act on the network, so its value is what an act-free instrument can offer. It **remembers** what happened before you noticed. It **compares** a moment with what came before. It is **honest about what it cannot see**. Each lens asks one question of the same evidence.

**WHEN did it change?** Tap MARK when the call stutters. The 60 seconds before the tap are already in the unit's memory, like an oscilloscope's pre-trigger. The sheet compares them with the reference before that, counts what changed and keeps filling as the seconds after the mark arrive. Episodes coalesce repeated changes ("since 14:02, 3 episodes") and have a lifecycle: OPEN, STILL SEEN, NOT SEEN FOR 4 MIN. Not seen is not called fixed. **Laps** use the same comparison across places: lap at jack A, walk, lap at jack B. The diff shows the DHCP server, router MAC, VLAN tags seen, broadcast rate, DNS answer time and ARP findings, each marked same, different or can't compare.

**WHO is here, and what are they doing?** Everyone who announces themselves is listed, because announcements reach every port. **Presence DNA** describes each device:

- what kind it is, from its DHCP options and mDNS services;
- its rhythm (mDNS every 120 s, DHCP renew at 50 % of its lease);
- whether that rhythm broke near a mark.

The unit's own conversations are **named by the DNS answers that resolved them** (`video.example.net · answer #212 · 3 names share this address`), drawn as a rhythm strip, and grouped by **resemblance**: "these four look alike; this one looks like nothing else in this capture". **Exposure** lists the hosts broadcasting LLMNR, NBNS or WPAD queries that anyone on the segment could answer.

**WHY — and how sure?** The Doctor holds competing explanations and shows the evidence and counter-evidence for each. When it cannot decide, it says what would settle it:

- **Verdicts.** Four states: OK, MARGINAL, PROBLEM and **CAN'T TELL**, always with a reason ("12 % of frames were lost in that minute").
- **"Do this now."** When the missing observation is something a person can cause, the Doctor asks for it: "Reconnect the phone's Wi-Fi now — I'll watch its DHCP exchange." That exchange is broadcast, so the unit can see it without sending a byte.
- **Integrity.** It checks the unit's own connections for signs of interference. Example: unrelated servers answering with identical TCP option sets and hop counts suggest a terminating proxy. Other signs are MSS clamping, NXDOMAIN answers rewritten into addresses, and a second DHCP server.

**An example.** A technician plugs DOORS into a wall jack in a meeting room where "the Wi-Fi is bad", and starts a capture. The `SEEING: OWN + BROADCAST` chip says what kind of evidence this will be.

1. **WHO lists 31 devices.** One of them answers DHCP with a different router address than the others.
2. **A laptop on the table fails to join.** The technician taps MARK.
3. **WHEN shows the laptop's DHCP DISCOVER repeated five times.** Two different servers offered addresses in the same 20 seconds.
4. **WHY names two explanations.** The second server is either a misconfigured travel router or a planned failover pair. As a discriminating observation it asks for one more join, so the technician reconnects the laptop.
5. **The second server answers first again.** The Doctor marks the explanation PROBLEM, states the evidence, and leaves the judgement about intent to the person who knows the network.

No model was needed for any of it. The engine, the gate and the ledger did the work.

## 4. Feature portfolio

Tiers follow the roadmap phases ([ROADMAP.md](neural/ROADMAP.md)). Every feature writes to the same ledger and appears in an existing lens. None adds a tab.

| Tier | Feature | Lens | Engine parts | CPU / KPU | Needs |
| --- | --- | --- | --- | --- | --- |
| **1** | Visibility gate and `SEEING:` chip | all | gate | CPU | — |
| **1** | Retroactive MARK, before/after sheet | WHEN | series, before/after | CPU | DS §59 amendment (a MARK control) |
| **1** | Episodes with lifecycle | WHEN | change finder, ledger | CPU | — |
| **1** | Laps (two-jack compare) | WHEN | series, ledger | CPU | — |
| **1** | Conversations named by DNS answers | WHO | name binding | CPU | — |
| **2** | Presence DNA (device kind, rhythm, rhythm breaks) | WHO | presence, rhythm | CPU | legacy fingerprint seed licence review |
| **2** | Doctor v1: competing explanations, CAN'T TELL, "To be sure" | WHY | Doctor | CPU | — |
| **2** | "Do this now" join watch | WHY | Doctor, presence | CPU | — |
| **2** | Integrity check | WHY | integrity | CPU | — |
| **2** | Exposure radar | WHO | presence | CPU | — |
| **3** | Conversation resemblance (DNA encoder) | WHO, WHEN | conversations, scorer | KPU, CPU reference | gate G4, decision D4 |
| **3** | Name-shape salience | WHO, WHY | name binding, scorer | KPU | D1, G3, G4 |
| **3** | Verdict replay scrubber (within the session) | WHEN | ledger | CPU | — |
| **4** | Trend / nowcast, host Wi-Fi station counters | WHEN, WHY | series | CPU | a read-only netd method; driver support unverified |
| **4** | Replay of a saved capture on the unit | all | virtual-clock replay | CPU | D3 |
| **4** | Mirror-port mode (full conversation map) | WHO | all | CPU + KPU | promiscuous reversion verified on hardware (ADR-016 rule 3) |
| **4** | Offline, dated threat-indicator match | WHY | name binding | CPU | licence of the bundle; D6 |
| **4** | Learned fusion of evidence | WHY | Doctor | CPU | G3 with non-negative weights |

## 5. Navigation

No new tab. The portrait app keeps LIVE, PACKETS and INSPECT ([PACKETSHARK.md](../PACKETSHARK.md) §7).

- **LIVE** gains one row under NETWORK: `INSIGHT · 1 OPEN · SEEING OWN + BROADCAST`. A tap opens LIVE's third face, **INSIGHT**, beside the network overview. The capture state, Companion readership and the capture bar stay on screen.
- **INSIGHT** has three lens chips at its top: WHEN, WHO and WHY. Each lens is one scrolling list of cards, newest problems first. In the wide shape the list is on the left and the selected card's sheet on the right.
- **Every card opens one sheet anatomy.** It reuses the M12.1 finding sheet (what was seen, what it may mean, the evidence, the packets around it) and adds two parts. **SEEING** says how much of the interval was visible. **TO BE SURE** names the observation that would settle the claim. Evidence taps go to INSPECT or a filtered PACKETS list, as findings do today. An expired packet keeps its number and reads "packet expired".
- **MARK** is one button on the capture bar, which every tab already shows. It is a change to the accepted DS §59 navigation and needs its own small amendment. In landscape a key does the same. A lap is a long press on MARK.
- **Colour and words.** DS §59's protocol hues mark protocol facts. Its warning glyph and word mark PROBLEM. CAN'T TELL is a word, never a colour. Certainty is text ("3 of 3 queries", "LR 14 against the next explanation"), never a percentage.

```text
LIVE · ETH0 · HEADERS                      14:02:31
SEEING  OWN + BROADCAST · 0 LOST           [MARK]

MARK 14:02:12          60 s before │ 19 s after
DNS ANSWERS SLOWED     median 31 → 412 ms
                       9 of 9 pairs │ 6 of 6 pairs
TCP HANDSHAKES         unchanged · 40 → 42 ms · 7 │ 4
FRAMES LOST            0 │ 0

WHY  resolver slow ··· LR 12 over "path to it"
TO BE SURE  one answer from another resolver
[EXAMINE DNS PAIRS]                 [OPEN WHY]
```

The figures above are fictional. They show a unit that is itself busy, for example while its own services fetch something. Denominators are the pairs matched in each window. With one pair the sheet shows the pair and withholds a trend. With none it says CAN'T TELL and leads with what was visible: the DHCP, ARP and broadcast evidence of the same seconds.

## 6. The engine in one paragraph

Observers in `pos-capture` keep a per-second series keyed by packet time, the unit's conversations with their 192-byte DNA sketch, name bindings from DNS answers, and device presence from plumbing. All of it lives in fixed tables of about 1 MiB in total. Analysts then write **observations** to a session-local **ledger**: numbers with numerator, denominator, comparison window, coverage and packet references. On top of these they write **episodes** and **claims**, the claims scored against competing explanations. An optional scorer helper, `pos-infer`, turns batches of numeric tensors into **salience**: resemblance clusters and shape scores. Salience can only point. A claim always needs a rule or a statistic as its witness. Details, the scorer contract, KPU exclusivity, CMA handling, replay and budgets are in [ENGINE.md](neural/ENGINE.md).

## 7. First KPU proof of concept, in brief

**Bench the DNA encoder (`dna-b64`, 64 conversations per call, 13.3 M MACs, 27 KB kmodel) on Unit B.** Run it standalone from a non-persistent directory, with no app, no capture and no install. Compare it against the same weights in a C reference on the CPU. Measure load time, end-to-end p50/p95 per call (copy in, sync, run, copy out), the per-operator profile, RSS and CMA before and after normal exit, and CMA lost after SIGKILL. Then repeat under a synthetic CPU load that resembles a capture.

**GO** if every one of these holds:

- p95 ≤ 2 ms per batch, and ≤ 25 % of the CPU reference;
- device output agrees with the simulator to a cosine of at least 0.999;
- CMA returns to idle within measured noise after a normal exit;
- the loss after SIGKILL is measured and under 4 MiB.

Otherwise conversation resemblance runs on the CPU reference at a lower rate, and the KPU stays with Vision. Full protocol, criteria and the roadmap: [ROADMAP.md](neural/ROADMAP.md).

## 8. What stays exactly as it is

Receive-only, with no transmit path, now or as an option. HEADERS by default, no payload-derived features, memory-only until SAVE, nothing logged from a packet. Identifiers expire at the end of the session, and nothing is uploaded. Companion keeps its field allowlist. Every exception to these is listed as an owner decision in [HANDOVER.md](neural/HANDOVER.md) and none is assumed.
