# R5 — Handheld instrument UX and AI-diagnosis landscape (desk research, 2026-11-24)

Desk research by a specialist agent; no downloads, no device. NetAlly user-guide pages returned 403, so NetAlly details come from search snippets of NetAlly's own docs. Reviews are anecdotal; AI vendors' explainability claims are their own.

## Competitor patterns (condensed)

| Product | Pattern | Strength | Pitfall |
|---|---|---|---|
| NetAlly LinkRunner/EtherScope/CyberScope AutoTest | One button runs a profile; each step (link, PoE, DHCP, DNS, gateway, targets) is a green/yellow/red card against user-set thresholds | ~10 s answer; explicit thresholds | spread over several windows; small screen; cloud upload inconsistent |
| NetAlly Discovery | Per-device Problems card: Warning/Failure/Info/**Resolved**; duplicate IP names switch ports | findings attached to a device with a lifecycle | manual clearing can hide recurrence |
| AirCheck G3 | red vertical line marks each roam on live graphs | event annotated where the symptom is drawn | one event type only |
| Fluke DSX | **PASS\*/FAIL\*** inside the tester's accuracy band (mandatory asterisk); FAULT INFO tab | admits uncertainty inside pass/fail vocabulary | customers may reject PASS\* |
| Wireshark Expert Info | Chat/Note/Warn/Error grouped by category; each links to its packet, can become a filter; docs say a finding proves nothing and absence proves no health | every finding ends at packets | noisy; dissector-dependent coverage |
| ntopng | numeric host score + behavioural checks | broad coverage | opaque score; own training slides admit many alerts |
| Zeek notices | per-identifier suppression (1 h default) | dedup built in | tuning in scripts |
| Fing / Fingbox | device inventory, outage/presence | simple list | "alerts constantly but no useful info"; paywall nags |
| Firewalla | learned per-device baseline alarms, sensitivity, mutes | tunable | dozens of alarms/day from cameras, not learned after weeks |
| Apple networkQuality | High/Medium/Low + raw RPM + what it means | category + number + consequence | no evidence trail |
| Google Nest mesh test | weak/good per point + plain fixes | actionable | flips at borderline (no hysteresis) |
| Juniper Mist Marvis + SLEs | action cards; SLE scores decompose into classifiers with timelines; validation time before auto-resolve; thumbs feedback | impact in plain terms ("DHCP failures 100 % of the time"); decomposable | confidence hidden; accuracy questioned in community |
| Kentik AI Advisor | collapsible reasoning pane + list of data used | auditable | vendor claims only |
| ThousandEyes | drill through layers HTTP → path → BGP | causal layering visible | cloud-only |
| Aruba Central AI Insights | prioritised insights with counts/percent per reason, likely cause, fix | quantified | silent coverage gaps (not implemented for some areas, not flagged) |
| Meraki smart thresholds | baseline anomaly alerts at a fixed 3 h cadence | limits volume | partial client coverage |
| Honeycomb BubbleUp | selection vs baseline, field by field | before/after comparison is the core | weak when selection is inside the main cluster |
| Datadog Watchdog / Bits AI | root cause → failure → impact; hypotheses validated / invalidated / **inconclusive** | abstention is first-class; missing root cause blamed on missing instrumentation | needs complete tracing |
| Grafana annotations | point/region marks with tags | familiar | made by hand after the fact |

## Ten design principles for a 4.1-inch portrait screen

1. A finding = claim + counted evidence + coverage ("3 of 3 DNS queries to 192.168.1.1 unanswered, last 60 s"); no naked score.
2. Four verdict states: OK, Marginal (PASS\*-style band), Problem, **Can't tell** — always with a reason.
3. Visibility before conclusions: a permanent "Seeing:" chip (own + broadcast vs mirror). Never green while blind.
4. Summaries decompose into named causes (stacked distribution; tap a segment to filter evidence).
5. Compare instead of alarm: change against a stated baseline window and count.
6. Hysteresis and dedup: "since 14:02, 3 episodes" instead of re-alerting.
7. Lifecycle: Open → Still seen → Not seen for N min ("not seen" ≠ "fixed").
8. Every finding says what would settle it ("To be sure:" — Heuer's Analysis of Competing Hypotheses); on a passive device that becomes a user action.
9. Three depths, at most two taps: glance → card (claim + two numbers) → packet list filtered to exactly the evidence frames. State never by colour alone.
10. One deterministic engine; ML only ranks and suggests; NPU output is labelled and points to rule-derived evidence. "Nothing abnormal in what I could see" is a designed result.

## Five interaction ideas competitors lack

1. **Retroactive mark ("it just stalled")** — oscilloscope pre-trigger / dashcam: the mark captures the 30 s *before* it from the ring and opens a before/after split. Risk: reaction lag; blaming the network for a non-network stall; buffer not yet full.
2. **"Do this now" provocation windows** — the device cannot transmit, so it asks the person to create traffic ("Reload the page now — watching DNS and TCP for 10 s"), then a verdict or an explicit "not seen" with reasons. Risk: pointless if coverage cannot see the host; caches give false negatives.
3. **Conversation rhythm strip** — a cadence glyph per conversation (periodic every N s, bursty, one-shot); a finding when a rhythm breaks near a mark. Risk: benign rhythm changes; density on 568 px.
4. **Two-jack compare (laps)** — lap at jack A, lap at jack B, diff card: DHCP server, gateway MAC, VLAN tags, broadcast rate, ARP findings, DNS latency; link down/up proposes a new lap. Risk: time-of-day confound, small samples.
5. **Verdict replay scrubber** — scrubbing re-renders every tile, coverage included, as it was; snaps to marks, link events, verdict changes. Risk: past vs live confusion (needs a strong PAST frame); RAM.

## Anti-patterns

Opaque 0–100 health score; green while blind; always naming a culprit or treating the mark as ground truth; alert firehose (USENIX Security '22 "99 % false positives"); flip-flopping at thresholds; fluent prose without a packet trail; uncalibrated "87 % confident"; severity by colour alone; a tab per feature; silent auto-resolve; upsell inside alerts.

## Key sources

Wireshark Expert Info docs (wireshark.org/docs/wsug_html_chunked/ChAdvExpert.html); Fluke marginal bands KB; Juniper Mist SLE and Marvis docs (juniper.net, networkingdocs.hpe.com); Honeycomb BubbleUp docs; Datadog Watchdog RCA and Bits AI docs; Apple support 101942 (responsiveness); Google Nest mesh test help; ntopng host checks; Zeek notice framework; Firewalla help community; USENIX Security 2022 Alahmadi et al. "99% False Positives"; FaulT-Bench (arXiv 2608.27021) on agents inventing root causes on healthy systems; Heuer, Psychology of Intelligence Analysis (CIA); NN/g progressive disclosure; Google PAIR Explainability + Trust.

Gaps: no documented numeric confidence UI found in Marvis/Watchdog/networkQuality; Link-Live compare not confirmed; K230 physical button/haptics availability not verified here (check docs/hardware).
