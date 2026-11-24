# ADR-019 amendment 1: Vision through the gateway

Status: **Accepted** by the product owner on 2026-11-23, as revised that day to the trusted-device model. Nothing is implemented yet: no gatewayd serves `vision`, and gateway code for it is still to be written, reviewed and approved for deployment as its own work.
Date: 2026-11-22, revised 2026-11-23
Deciders: product owner (final), AI engineering partner (author)
Depends on: ADR-006 amendment 1 (accepted the same day). Neither has an effect without the other.
Design and wire format: `docs/design/companion/VISION_PREVIEW.md` sections 5 to 7.

## Why

ADR-019 fixes what the gateway may pass on. Three of its rules do not allow a Vision preview as written:

- Rule 6: no `shell.*` method is ever an allowlist entry; the only methods the gateway calls on the shell's socket are the `capture.*` methods named there.
- Rule 24: the scopes of v1 are `status`, `mesh_read`, `mesh_send`, `capture_view`, `capture_control`.
- The limits: frames are JSON; nothing in v1 is a picture.

## The changes proposed

1. **One scope, `vision`.** It covers Vision's state and figures, the preview, and what a tap on Vision's own sheets could do (mode, the listed settings, the counters). It is granted to a phone once, on the unit, and holds until it or the pairing is revoked there (rule 24: a client cannot raise its own). The first draft had two scopes, `vision_view` and `vision_control`; they are merged because every method here is refused unless Vision is already in front, the biometric modes cannot be chosen, and one switch per phone is what a person can keep track of.
2. **Rule 6, extended**: the gateway may also call, on the shell's socket, the methods `vision.status`, `vision.share`, `vision.pull`, `vision.mode`, `vision.set` and `vision.reset`, served by the Vision app while it is in front, and no other (`VISION_PREVIEW.md` section 5.2). A phone calls `vision.status`, `vision.share`, `vision.ack`, `vision.mode`, `vision.set` and `vision.reset` (section 5.1): `vision.ack` is answered by the gateway and never passed on, `vision.pull` is the gateway's own and not a method a phone can call, and a picture reaches the phone as the event `vision.frame`. Every one is an allowlist row with its scope, its parameters and the fields passed on. No `shell.*` method is used: like `capture`, the gateway makes its events from its own `vision.status`, asked about once a second while a client holds the topic. The lint keeps holding that no other `shell.` or app method name appears.
3. **A capability** `vision` and a topic `vision`, named only by a gateway that serves them.
4. **Pictures.** A reduced picture may travel as base64 inside a JSON event, under these rules: one viewer on the unit; the gateway pulls a picture only when the viewer acknowledged the last one; it holds at most one; it is a snapshot event (skipped, never queued, for a client with unread output); 64 KiB of JPEG at most; 5 a second at most; nothing of it is logged or stored.
5. **No approval per preview and no time limit.** A phone with `vision` gets the preview when it asks while Vision is in front; the app asks by itself when its Vision screen opens. The unit shows the viewer's name and a STOP for as long as it lasts. After STOP on the unit, `vision.share` is refused until a person there resumes or Vision is opened again. The authorization is the scope; it is checked on every request and every picture, and taking it away ends a preview at once.
6. **Safe ends.** The gateway ends a preview itself (and tells the Vision app) when the viewer disconnects, gives the topic back, is revoked, loses the scope, or has not acknowledged for 5 s.
7. **Rate limit**: `vision.share`, `vision.mode`, `vision.set` and `vision.reset` together 20 a minute per client. It protects the unit's one hart, not the person at the unit.

## What does not change

Rules 1 to 5, 7 to 23, 25 and 26: disabled by default; pocketipc client only; TLS 1.3 with both keys pinned; pairing at the unit; per-client revocation; nothing that changes the node, the radio, Wi-Fi, storage or power; no secrets. `vision.mode` and `vision.set` change what the Vision app on the screen is doing and what its settings file remembers, exactly as a tap there would; they change nothing else of the unit. The owner's acceptance of this amendment settles that this is within rule 25.

## Not allowed

A stream endpoint outside the session; a picture for a client without the scope; the modes FACE, RECOGNIZE and READ, their results, `enrol` and `forget`; opening an app from the phone; the Camera app; a second viewer; recording by the unit or the gateway.

## Risks

- The gateway's surface grows by six methods for a phone (five of which reach an app inside the shell) and one, `vision.pull`, that only the gateway calls. Each answers from memory, takes enumerated values only, and is refused unless Vision is in front.
- A picture is the largest thing the gateway sends. It is bounded per message and per second, and paced by the receiver.
- A phone with `vision` sees the picture whenever Vision is in front, and can change what the person at the unit sees on the Vision screen. One grant now carries both; the mark, STOP and revocation on the unit are what remain.

## Product choices in this revision (all accepted as proposed)

| # | Choice proposed | Alternative |
| --- | --- | --- |
| 1 | One scope `vision` | Two scopes, `vision_view` and `vision_control`, as `capture` has |
| 2 | `vision` is granted by one switch per phone on the unit (at pairing or later in Settings), off unless switched on | Part of the default set every new pairing gets, with the pairing sheet saying so |
| 3 | No physical approval per preview; the scope is the authorization | The first draft: a sheet on the unit for every preview |
| 4 | No time limit; ends only by the causes in change 6, STOP, or Vision closing | A limit (the first draft had ten minutes) |

Choice 2 needs a way to grant a scope to an already paired phone on the unit, which did not exist when this was accepted (issue #172). Note of 2026-11-24, not a change to the decision: PR #174 adds it as `gateway.scope` on the local socket and as the switch on Settings > Companion (`docs/apps/SETTINGS.md`).

## Decision record

| Date | Decision | By |
| --- | --- | --- |
| 2026-11-23 | Direction given: trusted-device model (one authorization, automatic preview, no time limit, simpler permissions); draft revised accordingly. | Product owner |
| 2026-11-23 | **Amendment 1 accepted as written**, with the proposed choice in every row of the product-choices table above | Product owner |
