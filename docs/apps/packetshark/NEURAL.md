# PacketShark NEURAL

**Research and architecture record · revision 3 · 27 November 2026.** First written as a design study for owner review (revision 2, based on master `448327d`, 24 November 2026), with the K1 result and an independent review on 25 November 2026. One owner-authorized, non-destructive bench run on Unit A (K1, without P7); Unit B untouched.

**Status on 27 November 2026 (owner's decisions).**

- **N1.0 is implemented and hardware-validated.** It is the owner's reduced first slice of WHEN, not the engine described here: MARK on LIVE, the minute before and after a mark, what the capture saw of DHCP, ARP and DNS in each, how much of each window was watched, the capture filter named, and every observation linked to its packet in PACKETS and INSPECT. CPU only, no model, no ledger, no verdicts, no INSIGHT face ([PACKETSHARK.md](../PACKETSHARK.md) §4d). Merged by [pull request #188](https://github.com/KrakenSaten/K230/pull/188) (master `ff95ab99`, 25 November 2026) and validated on Unit A on 25 and 26 November 2026, the second time by the owner's own fingers (§9.16 there). The wide shape's back target was enlarged afterwards by [pull request #191](https://github.com/KrakenSaten/K230/pull/191) (master `883201e3`), not yet re-checked on the device.
- **The independent review's CPU-first architecture and its research priorities are adopted** ([INDEPENDENT-REVIEW.md](neural/INDEPENDENT-REVIEW.md) §5, §8 and §9): a scorer contract with a CPU backend first, in process; the KPU re-gated on need (G4-N); the review's order of work.
- **Decisions D1–D10 are accepted as recommended** ([HANDOVER.md](neural/HANDOVER.md) §2).
- **Nothing is authorized by this record.** No experiment, test on a unit, download, training or implementation follows from it; each needs its own authorization.
- **Verdicts unchanged:** K2 as specified is **NO-GO**; the memory criterion P6 is **INCONCLUSIVE**; the SIGKILL criterion P7 is **NOT TESTED**; G4 has not passed.
- One security finding (F1) is held privately and is not described in these documents.

**Recommendation: one witness, three lenses.** Build one intelligence engine inside the capture helper. It is an evidence ledger fed by cheap observers and guarded by a visibility gate. Present it through three lenses on the same evidence:

- **WHEN** marks a symptom and compares before with after.
- **WHO** shows who is present and what the unit's own conversations resemble.
- **WHY** names competing explanations and the observation that would settle them.

Ship WHEN first, on the CPU, with no model. Any scorer comes first as a CPU backend inside the engine, behind a two-key rule (no model output is ever the sole basis of a claim). The KPU is considered only for a model that has passed G3 and cannot meet its rate on the CPU (G4-N). (Revised 27 November 2026: this replaces "use the KPU for one thing first: a batched embedding of conversations", after the owner adopted the review's architecture.)

Host-side experiments for this revision gave three results that drive the design:

1. **The visibility gate comes first, on its logic.** A lost answer looks exactly like an unanswered query, so every number must carry how much of its interval was seen. The synthetic figures first quoted here (alerts on 76–90 % of affected windows without the gate, 2–4 % with it) are not evidence: the generator hands the gate the ground truth and never overlaps a fault with a gap (review F7). The gate's cost, faults missed under partial loss, is still unmeasured.
2. **The KPU compiles telemetry-shaped models,** but only from a narrow operator set. On Unit A, one batch of 64 conversations took 0.13 ms p95 end to end, and the output was byte-identical to the simulator's. That shows the KPU is fast, not that this model needs it. The C reference it was compared with (270 ms) is cache-hostile; a fair CPU kernel is estimated at 2–50 ms per batch, which is enough at product rates (review F4).
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
| [neural/K1-REVIEW.md](neural/K1-REVIEW.md) | Brief for an independent review of the K1 measurements on Unit A |
| [neural/INDEPENDENT-REVIEW.md](neural/INDEPENDENT-REVIEW.md) | Independent review of this package (25 November 2026): findings, disputed verdicts, GO/NO-GO decisions and an amended P7 protocol. Its corrections were applied to the other documents on 25 November 2026, except one held with a private finding. Its recommendations on architecture and order were adopted by the owner on 27 November 2026, as direction and priorities, not as authorization |

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
| A small temporal model can rank multivariate shifts better than rules | Synthetic E3a could not settle this (review F8): the rules were written knowing every generator fault, two baselines could not alert, and it is one seed. The supervised TCN beat the rules on two of three held-out faults and found none of the third, whose channel no training fault moved. A learned fusion gave that channel's rule a negative weight. The literature favours simple rules (R2: a boxplot rule matched Kitsune) | **Rules and statistics are v1,** because they can be explained and there are no real labels yet, not because E3a proved them better. A neural detector is admitted only by gate G3 on real, consented data, and may only reorder evidence, never suppress it. |
| A traffic shape can be called "unclassified" when short or ambiguous | E3b: the encoder beat untrained hand-crafted features (93 % vs 89 % on known classes; 93.2 vs 90.6 with kNN). One global similarity threshold rejected one flow of about 455 from two unseen classes. That does not show open-set rejection is impossible: the threshold was calibrated on test flows, and the margin and length rules R3 specified were not implemented (review F9) | **Show resemblance within the capture, not labels.** It is the prudent choice until rejection is tested properly on real data. Names come only from deterministic evidence: the DNS answer that resolved the address. |
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
- **"Do this now."** When the missing observation is something a person can cause, the Doctor asks for it: "Reconnect the phone's Wi-Fi now — I'll watch it join." The client's half of that exchange is broadcast: its DISCOVER, its REQUEST naming the server it chose, and the ARP probe and announcements that follow a successful join. The unit can see those without sending a byte. The server's OFFER and ACK are usually unicast to the client and are not visible from another port ([PACKETSHARK.md](../PACKETSHARK.md) §4c).
- **Integrity.** It checks the unit's own connections for signs of interference. Example: unrelated servers answering with identical TCP option sets and hop counts suggest a terminating proxy. Other signs are MSS clamping, NXDOMAIN answers rewritten into addresses, and a second DHCP server.

**An example.** A technician plugs DOORS into a wall jack in a meeting room where "the Wi-Fi is bad", and starts a capture. The `SEEING: OWN + BROADCAST` chip says what kind of evidence this will be.

1. **WHO lists 31 devices.** Their broadcast DHCP REQUESTs name two different servers.
2. **A laptop on the table fails to join.** The technician taps MARK.
3. **WHEN shows the laptop's DHCP DISCOVER repeated five times.** Its REQUESTs named one server, then the other, within the same 20 seconds. The servers' own OFFERs are usually unicast to the laptop and are not visible from this jack.
4. **WHY names two explanations.** The second server is either a misconfigured travel router or a planned failover pair. As a discriminating observation it asks for one more join, so the technician reconnects the laptop.
5. **The laptop's next REQUEST names the second server again,** and no ARP announcement follows. The Doctor marks the explanation PROBLEM, states the evidence, and leaves the judgement about intent to the person who knows the network.

No model was needed for any of it. The engine, the gate and the ledger did the work.

## 4. Feature portfolio

Tiers follow the roadmap phases ([ROADMAP.md](neural/ROADMAP.md)). Every feature writes to the same ledger and appears in an existing lens. None adds a tab.

| Tier | Feature | Lens | Engine parts | CPU / KPU | Needs |
| --- | --- | --- | --- | --- | --- |
| **1** | Visibility gate and `SEEING:` chip | all | gate | CPU | — |
| **1** | Retroactive MARK, before/after sheet | WHEN | series, before/after | CPU | DS §59 amendment (a MARK control). **N1.0 shipped a reduced form:** MARK and the DHCP, ARP and DNS evidence of the minute before and after, without statistics or verdicts ([PACKETSHARK.md](../PACKETSHARK.md) §4d; DS §59.11) |
| **1** | Episodes with lifecycle | WHEN | change finder, ledger | CPU | — |
| **1** | Laps (two-jack compare) | WHEN | series, ledger | CPU | — |
| **1** | Conversations named by DNS answers | WHO | name binding | CPU | — |
| **2** | Presence DNA (device kind, rhythm, rhythm breaks) | WHO | presence, rhythm | CPU | legacy fingerprint seed licence review |
| **2** | Doctor v1: competing explanations, CAN'T TELL, "To be sure" | WHY | Doctor | CPU | — |
| **2** | "Do this now" join watch | WHY | Doctor, presence | CPU | — |
| **2** | Integrity check | WHY | integrity | CPU | — |
| **2** | Exposure radar | WHO | presence | CPU | — |
| **3** | Conversation resemblance (DNA encoder) | WHO, WHEN | conversations, scorer | CPU backend first; KPU only under G4-N (revised 27 Nov) | G3; a KPU backend also G4 and G4-N |
| **3** | Name-shape salience | WHO, WHY | name binding, scorer | NO-GO (review §8) | — |
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

Observers in `pos-capture` keep a per-second series keyed by packet time, the unit's conversations with their 192-byte DNA sketch, name bindings from DNS answers, and device presence from plumbing. All of it lives in fixed tables of about 1 MiB in total. Analysts then write **observations** to a session-local **ledger**: numbers with numerator, denominator, comparison window, coverage and packet references. On top of these they write **episodes** and **claims**, the claims scored against competing explanations. A scorer turns numeric tensors into **salience**: resemblance clusters and shape scores. (Revised 27 November 2026: the adopted architecture runs it first as a CPU backend inside the engine. The separate `pos-infer` child of the original proposal is not adopted; a KPU backend, if one is ever admitted, would be a supervised single-owner service; INDEPENDENT-REVIEW §5.) Salience can only point. A claim always needs a rule or a statistic as its witness. Details, the scorer contract, KPU exclusivity, CMA handling, replay and budgets are in [ENGINE.md](neural/ENGINE.md).

## 7. First KPU proof of concept, in brief

**Bench the DNA encoder (`dna-b64`, 64 conversations per call, 13.3 M MACs, 27 KB kmodel) on Unit B.** Run it standalone from a non-persistent directory, with no app, no capture and no install. Compare it against the same weights in a C reference on the CPU. Measure load time, end-to-end p50/p95 per call (copy in, sync, run, copy out), the per-operator profile, RSS and CMA before and after normal exit, and CMA lost after SIGKILL. Then repeat under a synthetic CPU load that resembles a capture.

**GO** if every one of these holds:

- p95 ≤ 2 ms per batch, and ≤ 25 % of the CPU reference;
- device output agrees with the simulator to a cosine of at least 0.999;
- CMA returns to idle within measured noise after a normal exit;
- the loss after SIGKILL is measured and under 4 MiB.

Otherwise conversation resemblance runs on the CPU reference at a lower rate, and the KPU stays with Vision. Full protocol, criteria and the roadmap: [ROADMAP.md](neural/ROADMAP.md).

**Result, 25 November 2026 (Unit A, owner-authorized, P7 skipped; verdicts revised after the independent review).** These pass:

- p95 132 µs per batch back to back, and 191 µs at one call a second;
- 221 µs p95 under CPU load;
- output byte-identical to the simulator;
- no RSS growth over a 10-minute duty cycle;
- a second bench instance refused by the lock.

These are open:

- **Memory after a normal exit is inconclusive.** The meter used cannot resolve one process's 76–84 kB.
- **The SIGKILL measurement was not run.**
- **Sharing the KPU with Vision is untested.** Vision does not take the lock yet.
- **Need.** The CPU reference it was compared with (270 ms) is cache-hostile, so nothing here shows that this model needs the KPU.

G4 has not passed. Details: [ROADMAP §1](neural/ROADMAP.md#result-on-unit-a-25-november-2026). The review: [INDEPENDENT-REVIEW.md](neural/INDEPENDENT-REVIEW.md). K2 as specified stays **NO-GO** (adopted 27 November 2026); P6 stays inconclusive and P7 untested until the amended P7 session runs under its own authorization.

## 8. What stays exactly as it is

Receive-only, with no transmit path, now or as an option. HEADERS by default, no payload-derived features, memory-only until SAVE, nothing logged from a packet. Identifiers expire at the end of the session, and nothing is uploaded. Companion keeps its field allowlist. Every exception to these is listed as an owner decision in [HANDOVER.md](neural/HANDOVER.md) and none is assumed.
