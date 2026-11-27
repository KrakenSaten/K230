# DOORS Relay: resolution of the Codex review, re-review and status (2026-11-27)

Status: **record.** It shows how the Codex review was resolved, what the independent re-review found, the validation status, and the exact next steps (§15-§17). Development stopped here on 2026-11-27 at the owner's request. Both PRs are drafts. Nothing is merged or deployed, no unit was touched, and phase 2 has not started.

The review is kept verbatim in [`RELAY_REVIEW_CODEX_2026-11-27.md`](RELAY_REVIEW_CODEX_2026-11-27.md).

**The owner's decisions:**
- accept every finding and every disposition of Q1-Q27;
- Q9: **one relay data place per unit** in v1;
- Q22: **an explicit registration acknowledgement**;
- Q27: **pending invitations end when the pairing window closes or changes**.

Sections 1 to 14 are the contract both PRs were brought to:
- **PR #192** carries it in the ADR amendment, the proposal and the threat model.
- **PR #194** carries it in `docs/api/relay.md`, the Go relay and its tests.

**The committed ADR, proposal, threat model and `relay.md` are authoritative.** This file records how they were reached. Its §5 includes the correction made while the work was under way: the relay state file records grants both before and after each change.

**Out of scope:** Phase 2 has not started and needs its own go-ahead. Sections 5, 7, 8 and 9 are contracts Phase 2 must meet; nothing on a unit changes now.

## 1. Capacity (finding 3, Q9)

**One relay place per unit.** It holds one pending invitation or one splice, for a listed phone or a candidate.

**At the relay:**
- A `connect` from a phone that does not hold the place gets `unavailable` while the place is taken. This applies to a listed phone and to a candidate alike.
- **So relay pairing can be busy while a listed phone's splice runs, and the other way round.** This is the accepted trade-off of v1. It is said in the proposal and the threat model.
- A phone that holds the place replaces itself (Q10, §9). That is true whether it holds it as a listed phone or as a candidate, since a candidate re-pairing has the same key.

**At the helper:**
- 1 data connection at a time, including one waiting for its `join`.
- An invitation for the client that holds it replaces it: the old data connection and its socketpair end are closed first.
- An invitation for another client is declined.
- At most 1 invitation per second is acted on.

**At gatewayd:**
- At most **1 relay socket**, a session or a handshake, beside the LAN's own slots, which it never takes from.
- A relay session replaced by the same phone ends before the new handshake. That is the helper's order above.

**LAN and relay pairing** keep separate candidate state and separate wrong-token counts (rule 34, unchanged).

**The single place is a v1 decision, not a protocol limit.** Concurrent relay pairing beside a listed session needs:
- a revised helper and gatewayd budget;
- a measurement showing LAN service stays available.

## 2. The control connection (Q22, Q8)

**`registered`** is a new relay-to-unit frame with no members. It is sent once on a control connection, after `register` has taken effect:
- the connection is the unit's control at generation *g* + 1;
- the earlier control is closed;
- its pending invitations have ended, and their phones got `unavailable`;
- splices that do not survive the replacement have stopped forwarding on both legs (below);
- routing to the unit is available from that frame on.

**The unit** treats the relay as available only after `registered`. Its helper sends its first `pair_slot` only after it, so the first relay window is in force when the phone is shown the code.

**What survives a replacement:**
- a listed phone's running splice survives if, and only if, that phone is on the new `register`'s list;
- a candidate's running splice ends: the new control starts with no pairing slot, and a window is not carried across controls;
- pending invitations always end.

All of this happens under one critical section of the unit's state, together with the generation change. Socket shutdown happens before `registered` is sent.

**Control loss without replacement:**
- under the same lock, the unit goes offline, its pending invitations end, and its splices are marked stopping;
- both legs of each splice are closed at once.

## 3. Invitations (finding 1, Q27)

**Each invitation holds:**
- its cookie;
- the unit;
- the **control generation** it was issued on;
- for `via: "pair"`, the **slot generation**;
- a **monotonic deadline**, the invite time plus 5 s;
- the waiting client connection.

**`join` is decided in one critical section.** It is accepted only if all of these hold:
- the cookie exists and is unused;
- the monotonic now is before the deadline;
- the issuing generation is the unit's current control generation;
- for `pair`, the slot generation is current;
- the client is still waiting.

The cookie is consumed in that same section. Expiry never depends on which goroutine's timer fires first.

**These end every affected pending invitation, inside the same critical section as the change that causes it:**

| Change | Invitations it ends | Their phones get |
| --- | --- | --- |
| Control replacement | all | `unavailable` |
| Control loss | all | `unavailable` |
| `pair_slot` that changes or clears the slot | `pair` ones | `unavailable` |
| `admit` that drops the phone | that phone's `admit` ones | `unavailable` |

**Q27:**
- a running candidate splice is not ended by a slot clear or change;
- gatewayd owns its final response, its timeout, its expiry, its revocation and its teardown.

**`decline {cookie}`:**
- it counts only for a pending invitation issued on the **same control generation**;
- otherwise it is ignored without an answer, because a race with expiry or cancellation is normal.

## 4. Admission updates, revocation and acknowledgements (finding 2, Q19)

**`admit {admit, n}`:**
- `n` must be **greater than every earlier `n` on this control connection**; the first may be any value. A non-increasing `n` ends the connection (Q23).
- The list `register` carries is acknowledged by `registered`, not by `admitted`.

**Processing an `admit`:**
1. **Under the lock:** check that this control is current, replace the list, end the pending invitations of dropped phones, and mark their splices stopping.
2. **Close both legs** of every stopping splice, and wait until its copy goroutines have returned. The wait is bounded: closing the sockets unblocks any read or write in progress.
3. **Only then**, and only if the update was committed on the **current** control generation, send `admitted {n}`.

A superseded control's update is neither applied nor acknowledged.

**The ready transition:**

| State | Meaning |
| --- | --- |
| `invited` | the invitation is sent |
| `joined` | the cookie is consumed |
| `ready-sent` | `ready` written to the data leg, then the client leg |
| `forwarding` | entered only under the lock, after checking the splice is not stopping; only then do the copy goroutines start |

- **Stopping** at any state closes both sockets at once. It is never just a flag that a goroutine sees later.
- **Q4: `ready` is a byte-stream boundary on each leg.** Any bytes the relay has read ahead from a leg, past `ready`, are forwarded first and never lost.

**What an acknowledgement means:**
- `admitted {n}` or `registered` means only that this relay reports the update applied.
- A hostile relay can lie. gatewayd's own `remote` check, and its own close of a revoked phone's sessions, stay authoritative (rule 32).

## 5. Pairing commit and recovery on the unit (finding 4)

This is a Phase 2 contract. **Pairing and remote admission are separate states:**
- **pairing** is durable once `paired.v1` is written;
- the **remote grant** is durable once `relay.v1` is written;
- **confirmed relay admission** is never durable. It is what the current relay last acknowledged on the current control generation.

**The two-file rule:**
- Every change to `paired.v1` (a pairing, a known key pairing again, or a revocation) is preceded by a `relay.v1` write. That write records:
  - the grants **before and after** the change;
  - the SHA-256 of `paired.v1` before and after it.
- Then `paired.v1` is written.
- Changes to `remote` alone write only `relay.v1`.
- Both files are written temp, flush, rename, flush of the directory.

**At start, gatewayd compares the digest of `paired.v1`:**

| Digest of `paired.v1` | Grants used |
| --- | --- |
| equal to *after* | the grants after |
| equal to *before* (the change was interrupted, so it did not happen) | the grants before |
| neither, e.g. a rollback gatewayd or a manual edit changed the list | **every grant dropped**, logged, and said in Settings: "Remote access was turned off for every phone because the paired list changed" |

*Corrected during the work:* grants before **and** after are recorded, and the *before* case uses the grants before. The first version used "as recorded, less missing keys", which let an interrupted pairing of a known key change its grant.

In every case **a grant counts only for a key present in `paired.v1`**. So revocation followed by re-pairing never revives a grant: a re-pairing without "Through the relay" switched on records no grant.

**Approval with "Through the relay" on:**
1. Write `relay.v1` (the grant and the digests), then `paired.v1`. **Pairing is now committed.**
2. Send the **full admission snapshot**: the paired keys with a grant, at most 8. Send it as `admit` with `n` + 1 on the current control generation.
3. Wait at most 3 s for `admitted` with that `n` on the same generation. An acknowledgement for another generation or another `n` is ignored.
4. Answer the phone with `{paired: true, scopes: [.., "remote"], relay: "admitted" | "pending"}`.
   - `pending` means: no current control, a timeout, or a control replaced while waiting.
   - It is an outcome, not an error. **The pairing stands.**

**After every `registered`**, gatewayd sends the full snapshot before anything else, including before a `pair_slot`. The relay's list is always replaced, never merged.

**Retries:**
- the phone's retries after `pending` or a lost answer are ordinary authenticated sessions of a paired key;
- gatewayd answers from `paired.v1` and `relay.v1`;
- no second entry and no second approval is ever created for the same key;
- `gateway.hello` reports `relay: "admitted" | "pending" | "off"`.

**The app on `pending`:**
- it says "Paired. Remote access is waiting for the relay.";
- it retries through the relay twice within 10 s, then with PAIRING.md's back-off.

**Taking `remote` away:**
1. Write `relay.v1`.
2. gatewayd closes that phone's relay session itself.
3. Send the snapshot. Its `admitted` is information only.

**Revoking a phone:**
1. Write `relay.v1`, which drops the grant.
2. Write `paired.v1`.
3. Close every session of that phone, LAN and relay.
4. Send the snapshot.

**Tests in #194:** the fake unit implements this, and a test drives it through the real pairing flow against a relay that withholds `admitted`. That is §11, T9.

## 6. Bounded source tracking and logging (finding 5, Q16, Q17)

**The source table** (a source is an IPv4 /32 or an IPv6 /64):
- It has a hard cap, `max_sources`, default 4096 and configurable.
- An entry may be removed once it has no live connection and its rate window has passed.
- Removal work per accept is bounded and O(1) amortised: examine a fixed small number of entries per accept, or sweep a bounded batch on a timer. **Never a scan of the whole table per accept.**
- A new source when the table is full and nothing is removable is **refused at accept**, before TLS, and counted in the minute summary.

**Logging:**
- Nothing on a connection's path writes synchronously.
- Log lines go through a bounded queue, of at most 256 lines, to one writer goroutine.
- A full queue drops the line and counts it. The minute summary reports the count.
- Refusal lines are limited to at most 5 per second in all. The rest are counted.

**Shared NAT and TCP pass-through:**
- The per-source limits stay configurable.
- The measurement (§12) includes many legitimate clients behind one source.

**The 64-128 MB estimate** of proposal §12 stays ASSUMED until §12's measurement replaces it with a measured figure for the run's size.

## 7. Rotation and recovery (corrections to #192)

**Proposal §4.4:**
- When **`A` and `B` are both lost**, or the relay is set up again, the unit draws a new configuration id.
- A phone holding the old id **cannot adopt the new one provisionally**.
- Recovery needs an authenticated `gateway.hello` on a path the phone can still use, which is the LAN, or a physical re-pairing.
- The row "Phones follow by endorsement" is wrong there and is corrected.

**The rotation runbook says:**
- publishing a new backup hash at the relay does not install it on any unit;
- each unit takes a new `B` only at set-up, by a person at that unit.

**Phase 2 promotion:**
- `A := B`, removing the old `A`, and `seq + 1` are **one** `relay.v1` write;
- after it, the helper closes every connection presenting the old `A`;
- gatewayd signs nothing for the old `A`;
- a fault-injection test shows that a restart at any point leaves the old state or the new one, never a mix.

**Set-up** trusts the human-confirmed `A`. A relay that already controls `A` at set-up can choose `B`; that is in the threat model.

**Q7, the set-up dial:**
- The probe connection shows the presented key.
- After confirmation, a **fresh connection pinned to the full SPKI hash from the probe** fetches the backup and registers.
- Rule 28's wording ("over that confirmed connection") changes to "over a connection presenting the confirmed key".
- Rule 29 explicitly allows the person's set-up dial at the unit.

**Q7, the deadline:**
- The relay's 10 s from accept to `register` covers, on that connection, the backup check, or the proof check and its durable promotion write.
- If the deadline passes, the connection ends and the unit dials again with the durable state it has. Promotion is at most once per configuration, so the redial registers with the endorsement of the new `A`.

## 8. The helper's IPC authority (Phase 2 contract)

**Generations:**
- Every helper connection request from gatewayd carries a **generation** `gen`. Every operation carries an **operation id** `op`, for set-up, backup, promotion and promotion proof.
- gatewayd acts on a helper message only for the current `gen` and an outstanding `op`, with the expected pin. Anything else is discarded and counted.

**Messages:**

| From | Messages |
| --- | --- |
| The helper, each with `gen` | `presented`, `proof {op}`, `backup {op}` (missing from the current list), `sign_login`, `registered` (new), `admitted {n}`, `want_session`, `state` |
| gatewayd, each with `gen` | `connect {purpose: setup or serve, expected pin}`, `disconnect` (shutdown), `admit {n}`, `pair_slot`, `endorsement`, `signature`, `prove {op}`, `get_backup {op}`, `session {cookie}` with the descriptor, `decline {cookie}` |

**Peers are verified in both directions:**
- gatewayd checks the helper's `SO_PEERCRED` uid;
- the helper checks that the listener's uid is gatewayd's before sending anything, since an abstract name can be taken by any local process.

**Per-turn bounds that gatewayd enforces even against a compromised helper:**
- at most one helper frame of at most 256 bytes, and 20 a second;
- at most one relay TLS handshake step;
- reads within `GW_READ_BATCH`.

**Phase 2 tests:**
- a sandbox test lists which local sockets (abstract and path) and network destinations the helper can reach, and the result is reviewed;
- password removal, `identity_fault`, `state_fault` and switch-off, each with a live remote session, close it;
- an address-only change keeps it;
- LAN latency is measured while a hostile relay floods. A nice value and a reserved slot are not evidence.

## 9. Session replacement (rule 37, Q10)

**At the relay:** replacement happens when the phone's new `connect` is **admitted**, and the old splice of that phone ends then. If the new `join` then fails, the phone has no relay session. That is the accepted availability trade-off.

**At gatewayd:**
- A session arriving over the relay replaces nothing until the inner handshake has **authenticated** the phone's key and gatewayd has checked eligibility on the **actual ingress path**: a paired key with a valid grant, or the relay candidate of a window that allows the relay.
- **In particular, a relay attempt without `remote` can never displace that phone's working LAN session.**
- `via` and `client` are hints only (Q5).

## 10. Q1 to Q27

Every disposition is accepted. Q9, Q22 and Q27 are settled by the owner as revised.

| Q | Resolution | Where |
| --- | --- | --- |
| 1 | The phone hashes the canonical SPKI against its stored unit pin, or the QR `fp`, before it verifies anything. First-pairing state is provisional until pairing succeeds | spec §9.2; proposal §4.2 |
| 2 | After `which` matched unit U, `client.device_id` must be U, else `unavailable` (relay-enforced) | spec §7.3; code |
| 3 | Kept. gatewayd matches each answer to the outstanding `op`, nonce, expected pin and set-up or promotion generation | spec §9.1; proposal §8.1 |
| 4 | `ready` is a byte-stream boundary on each leg; read-ahead bytes are preserved. Stopping before `forwarding` aborts both legs | spec §7.4-7.5; code; tests T4, T12 |
| 5 | `via` is a hint without authority | spec §6 |
| 6 | 25 s and 75 s. Liveness is any valid frame. Each end must answer a `ping` with a `pong` of the same `n`; a `pong` is telemetry, not matched, and a stray one is ignored | spec §7.2 |
| 7 | §7 above; rules 28 and 29 amended | ADR; spec §9.1 |
| 8 | §2 above | spec §7.2; code |
| 9 | **One place** (§1) | spec §7.6, §10; proposal §8; code |
| 10 | §9 above | spec §7.6 |
| 11 | Raw 16-byte token, zero salt, info = the exact ASCII string **without** a NUL, L = 16. Signed messages are context, then 0x00. The independent vector is kept | spec §2, §8.7 |
| 12 | `n` is unsigned decimal: `0` or a non-zero digit followed by digits, no sign, fraction or exponent, at most 4294967295. Rejection vectors (duplicates, encodings, `null`s, overflow, malformed DER) are published as JSON test data that Go checks and C and Kotlin can reuse | spec §5; `relay/internal/wire/testdata`; code |
| 13 | Raw-line rules: printable ASCII; one space after the colon optional; no other whitespace; no obs-fold. The sender's wait-until-101 rule is stated separately. The relay detects only bytes already read ahead | spec §4 |
| 14 | Name: canonical dialable DNS. Labels are 1-63 characters of `a-z 0-9 -`, with no hyphen at either end; no empty label; no dot at either end; 1-64 in all. `seq` is 1 to 2^63-1, and 0 is refused. Within one configuration id, a different key needs a strictly higher `seq` | spec §8.2, §9.2; code (relay checks at `register`) |
| 15 | A missing ALPN, or a missing or wrong SNI, gets `404` after TLS. An offered but incompatible ALPN fails the handshake | spec §3, §14 |
| 16 | Kept; validated by the shared-NAT measurement | spec §10; §12 |
| 17 | Kept, with `max_sources` and the pre-authentication limits bounded separately | spec §10; code |
| 18 | Input validation only; the phone checks again | spec §7.2 |
| 19 | Kept. Tests: queue saturation, slow writes, teardown during writes, generation-safe acknowledgements | tests T6, T13 |
| 20 | Kept: bounded head reader, raw-line validation where cardinality and syntax are normative | spec §4; code |
| 21 | No relay byte rate for an owner-operated v1, **conditional on the slow-reader and load run**. Helper and gatewayd limits are Phase 2 | spec §10; §12 |
| 22 | **`registered`** (§2) | spec §6, §7.2; code |
| 23 | Kept. The spec says plainly that invitation failures and successful lookups differ in timing and result | spec §7.3, §11 |
| 24 | 10 s, measured over a slow link. No login or slot before the endorsement is verified | spec §7.3; §12 |
| 25 | P-256 key in the 91-byte SPKI form. The outer pin (leaf-only) and the inner gateway pin (exactly one certificate) stay distinct policies | spec §3 |
| 26 | Restart applies. The disconnect and re-register window is documented. `enroll` refuses to write if the file changed since it was read (lost-update check); one operator writer is documented | spec §13; README; code |
| 27 | Revised (§3) | spec §7.2; code |

## 11. Tests required in #194

All must be **deterministic**: injected clocks or hooks at the race points, not sleeps that hope.

| Id | Test |
| --- | --- |
| T1 | `join` after the deadline but before the phone's timer fires: refused |
| T2 | `join` with a cookie issued on a superseded control, including a join that arrives inside the replacement (hook between the steps): refused |
| T3 | A slot change and a slot clear end pending `pair` invitations. A running candidate splice survives a slot clear and ends on control replacement |
| T4 | Revocation while the splice is between `joined` and `forwarding`: no byte crosses after `admitted`, and both legs are closed before it |
| T5 | Revocation while forwarding is blocked (one end not reading, a copy goroutine stuck in Write): `admitted` comes only after both legs are closed, within a bound |
| T6 | Stale-control acknowledgements: an `admit` processed for a superseded control gets no `admitted`. `admitted` carries the `n` of its update. A non-increasing `n` closes the connection |
| T7 | A `decline` from a superseded control is ignored |
| T8 | `registered`: sent after replacement effects; a candidate splice ends at replacement; listed splices survive iff on the new list |
| T9 | **Through the real pairing flow** (fake unit + fake phone + a relay withholding `admitted`): pairing durable, outcome `pending`; reconnect then snapshot then `admitted`; a retry creates no second entry; revoke then re-pair without remote does not revive the grant; the two-file rule's three start cases |
| T10 | Control replacement while a splice runs and invitations wait (§2 survival rules) |
| T11 | Q2: `which` for U, then `client` naming another unit: refused |
| T12 | Q4: bytes read ahead on either leg at `ready` are forwarded, in order |
| T13 | The source table never exceeds `max_sources`; a full table refuses at accept; the sweep work per accept is bounded; a blocked log writer never stalls accept |
| T14 | `TestEnroll` passes under umask 077 and 022. The fixture sets its mode explicitly with `chmod`, not through `open` under the umask |
| T15 | One place: a listed splice makes a candidate's `connect` `unavailable`, and the reverse; same-phone replacement still works |
| T16 | Q12 rejection vectors (§10) |
| T17 | Q14: `seq` 0 and non-canonical names are refused at `register` |
| T18 | Q26: `enroll` detects a concurrent change and refuses |

**The full suite** runs `go vet` and `go test -race -count=1` under umask 022 **and** 077, on the Go in WSL (1.27.4). Go 1.24.0 is not installed. Say so rather than installing it.

## 12. Measurements required in #194 (focused, recorded with their machine)

| Id | Measurement |
| --- | --- |
| M1 | **Connection churn.** Many sources (127.0.0.0/8 addresses) opening and closing at a high rate for about 60 s. Record: RSS and Go heap, CPU, source-table size, refused counts, and a legitimate unit's and phone's reconnect latency (accept to `registered` or `ready`, p50/p99) during the churn |
| M2 | **Shared NAT.** Many legitimate clients behind one source up to its limits. Record: the latency and refusals of the clients beyond the limits |
| M3 | **Slow reader.** A splice whose one end stops reading, and a control connection that stops reading. Record: relay memory stays bounded; another splice's throughput and latency are unaffected; the control is disconnected at queue saturation |
| M4 | **Slow link for Q24.** Added delay (for example 300 ms RTT through a local delaying proxy) on the phone's provisional path. Record: the time from accept to `connect` against the 10 s deadline |

Report the figures in `docs/api/relay.md` §15 (Not validated / Measured). Mark what stays ASSUMED.

## 13. Who changes what

| PR | Changes |
| --- | --- |
| #192 (`claude/companion-relay-design`) | ADR amendment 3: rules 28, 29, 32, 33, 34, 37, 38 wording. RELAY_PROPOSAL: §4.1, §4.4, §5.2, §6, §7.2, §8.1-8.3, §12, §15 (a section answering this review), §16 phase gates. RELAY_THREAT_MODEL: new threats and checks for findings 1-5 and the IPC authority |
| #194 (`claude/relay-phase1`) | `docs/api/relay.md` (§§2-15, the Q list marked resolved). `relay/` server, fake unit and phone, tests T1-T18, measurements M1-M4, umask fix. Merge #192's new head |

Both PRs stay drafts. Nothing is merged or deployed, no unit is touched, and Phase 2 is not started.

## 14. Gates after this work

| Gate | Condition |
| --- | --- |
| #192 | The architecture GO stands. It becomes the Phase 2 contract once the independent re-review finds §1, §5 and §7-§9 settled |
| #194 | Phase 1 sign-off needs T1-T18 green, M1-M4 recorded, and the re-review's GO |
| Phase 2 | Still needs the owner's go-ahead, after both of the above |

## 15. The independent re-review (2026-11-27)

**What was reviewed:** PR #192 at `3d05c05c` and PR #194 at `ca450707`, against the Codex review's findings, dispositions and exit criteria.

**Who reviewed it:** a reviewer that had written neither PR.

**How:**
- it read both diffs, the whole of `relay.md`, and the server, the fake unit and the tests;
- it probed the code;
- it ran one batch of mutations.

### 15.1 Verdicts

| Gate | Verdict | Basis |
| --- | --- | --- |
| PR #192 at `3d05c05c` (phase 2 contract) | **GO, with one wording fix (F2)** | Capacity, pairing commit and recovery, rotation, IPC authority and replacement are settled. Proposal §8.3 conflicts with the relay's frame rate |
| PR #194 at `ca450707` (phase 1 sign-off) | **NO-GO, narrowly** | All five original findings are resolved in code and tests. F1 contradicts Q22 ("routing available from `registered` on"). After an F1 fix with a regression test: GO without another full review |
| Phase 2 | **NO-GO** until F1 is fixed and the owner decides F2 | F1 and F2 shape the helper's control state machine. The owner's go-ahead is still required |

**The five original findings:**
1. Invitations: **resolved**. T1, T2, T7 and T10.
2. Stopping before the acknowledgement: **resolved**. T4, T5 and T6.
3. One place: **resolved**. T15.
4. Pairing commit: **resolved as a contract**. T9 runs through the pairing flow, against the lying relay.
5. Bounded sources and logs: **resolved**. T13, with M1-M3.

**The architecture concerns:** rotation, IPC authority, and eligibility before displacement are all resolved, the IPC authority as a contract.

**Q1-Q27:** all reflected, except Q22 (partly, F1) and Q17 (partly, F3).

The reviewer agreed with the owner's Q9, Q22 and Q27 decisions.

### 15.2 Open findings

**F1 (medium): `invite` can come before `registered`.**
- **Cause.** `register` makes the new control the unit's at commit (`relay/internal/server/state.go`, `u.ctl = k` under `s.mu`). It sends `registered` only after the effects: `finish`, then the old control's close.
- **Reproduction.** In that window, a listed phone's `connect` is admitted (`admitSplice`), and its goroutine queues `invite` on the new control (`client.go`, `sp.ctl.send(&wire.Invite{...})`) ahead of `registered`.
- **It reproduces on the unmodified commit.** No source or existing test was changed. A new throwaway test (RR1) holds `register` at the existing `register-committed` hook and then connects a listed phone. It failed 3 of 3 times and in a full run. Without the hook the window is real but narrow, and was not reproduced. RR1 was not committed.
- **Consequence.** The fake unit rejects any frame but `ping` before `registered` (`relay/internal/fake/unit.go`, `ErrUnexpected`), and the phase 2 helper would inherit the ambiguity. This is from reading the code; that probe (RR3) was not run.

**F2 (low-medium): declines can close the control.**
- Proposal §8.3 says the helper acts on at most one invitation a second and *declines* the rest. The relay closes a control that sends more than 10 frames a second.
- Probe RR2, on unmodified code: 12 quick declines closed the control.
- A phone, or anyone holding the slot, connecting from two or more sources at more than about 11/s therefore makes the unit's own declines cut its control connection.
- This adds to M3b in `relay.md` §15.3, where a reconnect burst fills the 32-frame queue.
- **Needs the owner's decision** on limiting invitations per unit at the relay.

**F3 (medium-low, there before this work; Q17 partly met):**
- 16 sources × 64 unauthenticated connections reach the global cap of 1024, with about 100 TCP connects a second and no TLS. 16 IPv6 /64s fit in one /60.
- Every new connection is then refused, unit reconnects included.
- This is arithmetic from the defaults; the probe (RR4) was not run.

**F4 (low): two test gaps.**
- Mutation MD survived: removing the end of a pending `pair` invitation on a slot change is not caught, because T3 waits for the phone's 5 s timer and never tries a `join` between the change and the expiry.
- Mutation MP survived: removing `setAdmit`'s current-control check lets an `admit` already read from the old control overwrite the new control's list. Only the acknowledgement is suppressed.
- The 100 ms tolerance in `closedNow` cannot tell "closed before the acknowledgement" from "just after".

**F5 (low): a stale grant can return in one edge case.**
- A rollback gatewayd revokes a phone and pairs it again within one boot, without a wall clock. That can rewrite a byte-identical `paired.v1`, whose digest matches the recorded "after", so the old grant returns.
- The rule "a grant counts only for a key in `paired.v1`" has no test of its own: mutation FD was equivalent, because the fake filters before writing.

**F6 (low): an acknowledgement on a replaced control.**
- `setAdmit` reports true and the `admitted` is sent outside the lock. So an acknowledgement can still reach a control replaced in between.
- This is harmless because gatewayd matches generations, but `relay.md` §7.2 says "neither applied nor acknowledged".

**F7 (low): the limits of the measurements.**
- M4: above about 2.85 s RTT the provisional path misses the 10 s deadline. Acceptable; the app should say so.
- M1 and M3 ran on loopback on a fast machine. M1 cost 0.63 cores for about 600 TLS handshakes a second, so a 1-vCPU host may saturate near 1000 a second.
- The host-size estimate stays ASSUMED.

**Documentation nits:** both are fixed in #194's last commit.
- `relay.md` §15.4 named threat-model check 13 as left to later phases, where 14 was meant.
- `relay.md` pointed to this contract while it was outside the repository.

## 16. Validation status

| What | Status |
| --- | --- |
| #194 `ca450707`: `go vet ./...` and `go test -race -count=1 ./...` on Go 1.27.4 | **Pass** under umask 022 and 077: 75 tests pass, 5 skip by design (the M1-M4 measurement tests, run on request), no data race. Run by the #194 session, and again independently from a `git archive` of `ca450707` |
| T1-T18 | In `relay/internal/server/review_test.go`, green |
| M1-M4 | Recorded in `relay.md` §15.2, on loopback, one machine |
| Mutation batch 1 | 22 mutations: 17 killed (4 by a hang), FC did not compile, ME was not applied, 4 survived (MR and FD equivalent; MD and MP real gaps, F4). Batch 2 (MS, MT, ME2, MU, MV) was not run, at the owner's request |
| Fuzzers | Not rerun after these changes: the protocol code they cover did not change |
| Not run | Go 1.24.0 (not installed); a real network or relay host; the C helper, gatewayd and the app (phases 2 and 3); probes RR3 and RR4 |
| #192 | Documentation only; nothing to run |

## 17. Exact next steps

Each step needs the owner's go-ahead. Development stopped at the owner's request on 2026-11-27.

1. **F1, in #194.** Make a control routable only once `registered` is queued:
   - in `register`, after `k.send(&wire.Registered{})`, set a per-control `routable` flag under `s.mu`;
   - make `admitSplice` refuse a unit whose control is not yet routable;
   - add RR1 as regression test T19: hold `register` at `register-committed`, connect a listed phone, and expect `registered` before any `invite`.
2. **F2, the owner's decision, then #192 and #194.** Recommended:
   - a per-unit invitation limit at the relay (refuse with `unavailable` before inviting), set below the control's frame limit;
   - the helper drops invitations beyond its 1 a second **silently**, with no `decline`, so a flood cannot close the control;
   - fix proposal §8.3's wording, `relay.md` §7.2 and §10, and add a test.
3. **F4, in #194.** Add tests:
   - a `join` after a slot change and before expiry is refused (catches MD), with T3 bounded in time;
   - an `admit` read from a superseded control does not change the new control's list (catches MP).
4. **F6, in #194.** Either check the current generation under the same lock as the send, or reword `relay.md` §7.2: "not applied; a late acknowledgement can reach the old connection, which gatewayd ignores by generation".
5. **F5, in #192, plus the fake unit in #194.**
   - Record `paired.v1`'s inode with its digest. The same digest with another inode counts as "neither".
   - Add a direct test that a grant for a key not in `paired.v1` never counts.
6. **F3, the owner's decision.**
   - A global cap for unauthenticated connections that leaves room for authenticated ones;
   - IPv6 sources grouped at /56;
   - a shorter deadline before the ClientHello.
   - Then update Q17 in `relay.md` §14.
7. **Then:**
   - `go vet` and `go test -race -count=1` once under each umask;
   - a focused re-review of F1 and F2 only (the reviewer said no full review is needed);
   - update §15 here.
8. **Phase 2** only after #194's GO, and with the owner's go-ahead.
