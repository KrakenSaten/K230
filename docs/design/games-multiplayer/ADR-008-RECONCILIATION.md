<!-- Copyright (c) 2026 Doors authors. SPDX-License-Identifier: Apache-2.0 -->
# ADR-008: reconciliation with the hardware evidence (prepared, not applied)

Prepared 2026-11-20. **ADR-008's status is not changed by this package and
neither file below is edited.** This is the evidence and the exact text for
its transition to Accepted, ready to apply on the owner's word.

## Why

ADR-008 reads "Proposed (2026-09-25) ... awaiting the product owner", and
`docs/apps/FLEET_MULTIPLAYER.md` says "Nothing here has been on a radio ...
(P7), not yet run". Both statements predate three hardware gates between
units A and B. The feature is merged and shipped, and ADR-017 and ADR-018
build on its decisions.

## Evidence since 2026-09-25

| Gate | Date | Result | Document |
| --- | --- | --- | --- |
| P7, minimum scope | 2026-09-26 | PASS | `docs/hardware/FLEET_MULTIPLAYER_GATE.md` |
| Chat and status | 2026-09-30 | PASS, twelve steps | `docs/hardware/FLEET_CHAT_STATUS_GATE.md` |
| AUTO deployment, two matches | 2026-09-30 | PASS | `docs/hardware/FLEET_MP_AUTO_GATE.md` |

ADR-008's claims against it:

| ADR-008 | Was | Now | Basis (P7 unless said) |
| --- | --- | --- | --- |
| Decision 1: app datagrams on `PAYLOAD_TYPE_REQ`; flood, then a receipt teaches a direct route | VERIFIED host | **VERIFIED hardware** | INVITE answered in 3.6 s, `app_receipts` 1, then `DIRECT`, hops 0 both ways |
| Decision 2: Fleet owns its protocol | VERIFIED host | **VERIFIED hardware** | a 46-ply match, identical logs on both units |
| Decision 3: commitment and reveal | VERIFIED host | **VERIFIED hardware** | both "Their fleet: Verified"; reveal bits 0/0 until DONE |
| Decision 5: opening Fleet transmits nothing | VERIFIED host | **VERIFIED hardware** | lobby open 60 s, `tx_packets` 0, every `app_*` 0 |
| Decision 6: paused, never forfeited | VERIFIED host | **VERIFIED hardware** | radio off 179 s: no forfeit, resolved on the next retry |
| Decision 7: airtime governor | computed | **measured, one match** | unit B: 63 packets, 21,044 ms, duty cycle 0.58 % for the gate |
| Decision 10 and persistence: close/reopen, meshcored restart, reboot after FIRE | VERIFIED host | **VERIFIED hardware** | recovery cases A-D, each PASS, no shot counted twice |
| No Fleet packet in `mesh.messages`; RIFT text unaffected | VERIFIED host | **VERIFIED hardware** | count 0 on both; a direct text acked in 0.87 s |
| In-match chat (added 2026-09-30) | VERIFIED host | **VERIFIED hardware** | the chat gate |
| The 22-byte frame on air | VERIFIED hardware (ACK only) | VERIFIED hardware for app datagrams in use | 60 `app_tx` / 64 `app_rx` on unit B |

**Still open, and to stay stated in the ADR:**

| Claim | Class | Why |
| --- | --- | --- |
| A T-Deck (RIFT firmware) ignores a Doors REQ | **ASSUMED** | P7 step 10 was out of scope and has not been run |
| Behaviour at range, through a repeater, on a busy channel | UNRESOLVED | both units were on a bench, direct, at 2 dBm |
| Airtime over a full-length match | computed | the measured match was 46 plies, not the simulator's 104-141 |
| An unattended match at real pace (P7 step 11) | not run | out of the minimum scope |
| Decisions marked *(Default)* - 1, 3, 4, 6, 7, 8, 9 | owner has not ruled | accepting the ADR accepts these defaults as they shipped |

## What the owner would be accepting

ADR-008 as written, including its *(Default)* decisions as shipped, with
the four open items above carried as stated risks. Nothing in it needs to
change for M2: ADR-018 keeps decisions 4-10 and holds decision 7's figures
as the mesh profile.

## Text to apply on approval

`docs/decisions/ADR-008-fleet-multiplayer.md`, status line (lines 3-4):

```text
Status: Accepted (product owner, <date>), with its default decisions as
shipped. Proposed 2026-09-25; hardware evidence: FLEET_MULTIPLAYER_GATE.md
(P7, 2026-09-26), FLEET_CHAT_STATUS_GATE.md and FLEET_MP_AUTO_GATE.md
(2026-09-30). Open: the T-Deck check (ASSUMED) and behaviour at range.
```

Its Evidence table, three rows replaced and one added:

```text
| 22-byte frame on air | VERIFIED hardware | MESHCORED_HARDWARE_GATE.md (ACK); FLEET_MULTIPLAYER_GATE.md (app datagrams) |
| Everything built in P1-P6 | VERIFIED host; the match, recovery, fairness and coexistence VERIFIED hardware | the suites named in docs/apps/FLEET_MULTIPLAYER.md; FLEET_MULTIPLAYER_GATE.md "Results" |
| T-Deck ignores an unknown REQ type | ASSUMED | not tested: P7 step 10 was out of scope |
| In-match chat on air | VERIFIED hardware | FLEET_CHAT_STATUS_GATE.md |
```

`docs/decisions/README.md`, the index row:

```text
| [008](ADR-008-fleet-multiplayer.md) | Two-player Fleet over the mesh | Accepted, <date> |
```

`docs/apps/FLEET_MULTIPLAYER.md`, lines 3-9 (a factual correction that does
not depend on the status; it can be applied on its own):

```text
Two players, one match, turn based, over the MeshCore mesh. The decision and
its alternatives are docs/decisions/ADR-008-fleet-multiplayer.md.
Single-player Fleet is unchanged (docs/apps/POCKETFLEET.md).

Status: host-tested, and gated on hardware between units A and B over LoRa:
docs/hardware/FLEET_MULTIPLAYER_GATE.md (P7 minimum scope, PASS 2026-09-26),
FLEET_CHAT_STATUS_GATE.md and FLEET_MP_AUTO_GATE.md (PASS 2026-09-30). Not
checked: a non-Doors MeshCore node, range, a repeater, a busy channel.
```

ADRs are "never rewritten once Accepted" (docs/decisions/README.md); the
edits above are the acceptance itself and touch only the status line and
the evidence table.
