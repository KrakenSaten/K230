# ADR-016 amendment 1: PacketShark and a paired companion

Status: **Accepted** by the product owner, 2026-11-22, **subject to its documented security and privacy constraints** and with the clarifications under "Acceptance" below. ADR-016 is accepted subject to its security constraints and says that changing any of them "is a change to this ADR and needs the owner again". This is that change, kept in its own file so the accepted text stays as it was accepted. The rules below are conditions of the acceptance, not aims.
Date: 2026-11-21
Deciders: product owner (final), AI engineering partner (author)
Depends on: ADR-019 (the authenticated gateway, accepted 2026-11-21). Without ADR-019 this amendment has no effect.

## Why

The owner approved, on 2026-11-21, that DOORS Companion v1 may show PacketShark's aggregate statistics, may HOLD and STOP a capture, may ask for a START that a person confirms on the unit, and may not receive raw packets. Two accepted rules do not allow that as written:

- Rule 1: "Nothing starts a capture on boot, on a timer, **over IPC** or from another app."
- Rule 11: "**Nothing is uploaded or sent.** The app and its helper have no network client code."

## What does not change

Ownership and privilege are untouched.

- `pos-capture` remains a per-capture child of the shell's PacketShark session, spawned at START over a socketpair, with the exclusive `capture.lock`, `PR_SET_PDEATHSIG`, the watchdog and the abandon sequence as they are.
- Rules 2 to 10 and 12 to 18 stand word for word: receive only; promiscuous mode not offered; one capture; **a capture ends when the app is left**; the 15-minute default; HEADERS by default and FULL only by a confirmed choice on the panel; memory only until SAVE; SAVE to `/root/Captures` only; nothing logged; the state on every screen; the screen held awake; capabilities dropped before a frame is parsed; packet bytes parsed only in the helper.
- `gatewayd` never spawns, signals, or speaks to `pos-capture`, never takes `capture.lock`, and links nothing of `core/pocketpcap`.
- The PacketShark app and its helper still contain no network code. The lint keeps holding that.

## The amended rules

**Rule 1, replaced by:**

> 1. Nothing is captured without an explicit START **tapped on the app's own screen on the unit**. Nothing starts a capture on boot, on a timer, or from another app. Over IPC, a paired companion with the `capture_control` scope may **request** a capture (rule 1a); a request starts nothing.
>
> 1a. A request opens PacketShark's setup screen with a sheet that names the requesting client, the interface and the time limit. The capture starts only when a person taps START on that sheet. The sheet times out after 60 s as a refusal, and leaving the app refuses it. A request can ask only for what a tap could: an interface the setup screen offers, a limit of 5, 15 or 60 minutes, HEADERS. It can never ask for FULL, for promiscuous mode, or for no limit. A request while a capture is running, or while another request is shown, is refused. A request is never queued and never remembered.

**Rule 11, replaced by:**

> 11. No packet, no part of a packet, and nothing that identifies a host leaves the unit: no address, no name, no port, no payload, no row, no file. The app and its helper have no network client code.
>
> 11a. The capture's **state and aggregate figures** may be read by a paired companion with the `capture_view` scope, through the gateway of ADR-019 and no other way: the state and why it ended, the interface name, the stored-bytes mode, the elapsed time and the limit, counts and rates of frames and bytes, the per-class counts, the drop and shed counts, and the totals of the findings and the diagnosis. The exact fields are listed in `docs/api/gateway.md`; adding one that identifies a host is a change to this rule.

**New rules:**

> 19. A paired companion with `capture_control` may HOLD, release a HOLD, and STOP a running capture. It cannot DISCARD, SAVE, pin, change the view, inspect, or delete a saved capture.
>
> 20. While a companion is reading the capture's figures, the app's header says so on every one of its screens, beside the capture state (rule 13).
>
> 21. The methods of rules 1a, 11a and 19 are served by the PacketShark app from the model it already holds, inside the shell. They add no command to the helper's line protocol and no traffic to it. They are refused unless PacketShark is the app in front, except the request of rule 1a, which opens it.

## Not allowed, now or as an option of v1

Packet rows, the inspector, findings or diagnosis text for a packet, DNS or host names, the hex view, the pcap file, a list of saved captures, a capture that continues when the app is left, a capture with no person at the unit. Each would be a new decision against rules 5, 9 and 11.

## Relation to accepted ADRs

- **ADR-016**: rules 1 and 11 are reworded and rules 19 to 21 added; every other rule and every accepted risk stands. The option ADR-016 rejected (a supervised capture service) is not reintroduced: no process other than the per-capture helper can capture.
- **ADR-002 and ADR-007**: the new methods are served by an app inside the shell from memory; no parser of network input and no capture code enters the shell.
- **ADR-019** (accepted): the only path by which these methods are reachable from outside the unit.

## Consequences

Needed for implementation (a later step): a way for an app inside the shell to serve methods on the shell's socket (`docs/design/companion/CONTRACTS.md` §3); the `capture.*` methods; the request sheet; the header mark; an extension of `tests/packetshark_lint.sh` so that the `capture.status` result is built only from counters.

Risks:

- The figures say something about the network: how busy it is, and that DNS or TCP is failing. They name nobody. Accepted as the purpose of the feature.
- A request sheet can be raised by a paired client at will. It is rate-limited by the gateway (one per 30 s per client), it is refused unless the unit is unlocked and not showing an alert, and it can do nothing without a tap.
- Remote STOP ends a capture the person at the unit may have wanted. A stopped capture keeps its packets; nothing is lost.

## Acceptance (2026-11-22)

The owner accepted this amendment "for the PacketShark Companion architecture, subject to its documented security and privacy constraints", with these clarifications, recorded as given. They bind as the rules do.

1. Local shell IPC may HOLD/RESUME/STOP an active capture under the existing socket access controls.
2. Repeated `capture.hold` remains idempotent (`changing: false`).
3. `capture.stop` on an already stopped capture remains an error.
4. No packet contents, addresses, host identifiers, per-packet details or capture files may reach Companion.
5. Gateway must explicitly enforce `capture_view`/`capture_control`, topic subscriptions and event filtering.
6. Remote START always requires a physical tap on K230.
7. No background capture or changes to helper ownership.

## How the implementation reads the rules (for the owner to confirm or change)

These are the implementer's readings, not the owner's words. They are in the code and in `docs/api/shell.md`; none loosens a rule.

- **Rule 1a, how a request ends.** Besides Back, leaving the app and 60 s, the sheet is taken down at once when the unit is locked or an alarm comes over the app, and it can be withdrawn by whoever asked (`capture.request_cancel`). START on the sheet takes no tap in the sheet's first 1.5 s. A request for a unit that is locked or showing an alarm is refused without opening anything.
- **Rule 20, "while a companion is reading".** The app cannot see who listens to its events. It says a companion is reading for five seconds after any `capture.*` request on the shell socket, and raises its figures (`capture.stats`) only while it says so. Whoever forwards for a phone therefore has to keep asking while the phone reads, or it gets no figures; clarification 5 puts that on the gateway.
- **Clarification 1.** Rule 19 names a paired companion. The methods are served on `shell.sock`, where any process the socket admits can call them.
- **Open, the owner's call:** a request to a unit whose screen is dark but not locked wakes the screen and shows the sheet. Refusing it instead, as a locked unit is refused, would be a change to the shell's opening rule.

## Evidence

- The rules quoted: DOCUMENTED (`docs/decisions/ADR-016-packet-capture.md`).
- The app's model already holds every figure of rule 11a (`stats`, `bin`, `diag`, `state` lines; `apps/packetshark/psk_model.h`, `docs/apps/PACKETSHARK.md` §6): DOCUMENTED.
- An app serving methods on `shell.sock`: did not exist when this was drafted; built since as steps H1 to H4 (`docs/api/shell.md`, "Methods served by an app"), simulator-tested, not yet run on a unit.

## Decision record

| Date | Decision | By |
| --- | --- | --- |
| 2026-11-21 | Direction approved: aggregate statistics, HOLD/STOP, START only with physical confirmation, no raw packets | Product owner |
| 2026-11-22 | **Amendment 1 accepted** for the PacketShark Companion architecture, subject to its documented security and privacy constraints, with the seven clarifications under "Acceptance" | Product owner |
