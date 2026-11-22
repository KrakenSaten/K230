# ADR-019 amendment 1: Vision through the gateway

Status: **Proposed** (draft for the owner). Until it is accepted ADR-019 stands as written: five scopes, and no method of the shell but the four `capture.*` entries.
Date: 2026-11-22
Deciders: product owner (final), AI engineering partner (author)
Depends on: ADR-006 amendment 1 (proposed). Neither has an effect without the other.
Design and wire format: `docs/design/companion/VISION_PREVIEW.md` sections 5 to 7.

## Why

ADR-019 fixes what the gateway may pass on. Three of its rules do not allow a Vision preview as written:

- Rule 6: no `shell.*` method is ever an allowlist entry; the only methods the gateway calls on the shell's socket are the four `capture.*`.
- Rule 24: the scopes of v1 are `status`, `mesh_read`, `mesh_send`, `capture_view`, `capture_control`.
- The limits: frames are JSON; nothing in v1 is a picture.

## The changes proposed

1. **Two scopes**, granted per client on the unit like the others: `vision_view` (Vision's state and figures; to ask for a share) and `vision_control` (mode, the listed settings, the counters; requires `vision_view`). Neither is in the default set a pairing grants.
2. **Rule 6, extended**: the gateway may also call, on the shell's socket, the methods `vision.status`, `vision.share`, `vision.pull`, `vision.mode`, `vision.set` and `vision.reset`, served by the Vision app while it is in front, and no other (`VISION_PREVIEW.md` section 5.2). A phone calls `vision.status`, `vision.share`, `vision.ack`, `vision.mode`, `vision.set` and `vision.reset` (section 5.1): `vision.ack` is answered by the gateway and never passed on, `vision.pull` is the gateway's own and not a method a phone can call, and a picture reaches the phone as the event `vision.frame`. Every one is an allowlist row with its scope, its parameters and the fields passed on. The lint keeps holding that no other `shell.` or app method name appears.
3. **A capability** `vision` and a topic `vision`, named only by a gateway that serves them.
4. **Pictures.** A reduced picture may travel as base64 inside a JSON event, under these rules: one viewer on the unit; the gateway pulls a picture only when the viewer acknowledged the last one; it holds at most one; it is a snapshot event (skipped, never queued, for a client with unread output); 64 KiB of JPEG at most; 5 a second at most; nothing of it is logged or stored.
5. **Physical approval, per share.** Having `vision_view` shares nothing. Each share is allowed by a tap on the unit, shown there while it lasts, and ends after ten minutes (ADR-006 amendment 1).
6. **Rate limits**: one share request per 30 s per client; `vision.mode` and `vision.set` together 20 a minute per client.

## What does not change

Rules 1 to 5, 7 to 23, 25 and 26: disabled by default; pocketipc client only; TLS 1.3 with both keys pinned; pairing at the unit; per-client revocation; nothing that changes the node, the radio, Wi-Fi, storage or power; no secrets. `vision.mode` and `vision.set` change what the Vision app on the screen is doing and what its settings file remembers, exactly as a tap there would; they change nothing else of the unit. Whether that is within rule 25 as the owner means it is part of this decision.

## Not allowed

A stream endpoint outside the session; a picture without the tap; the modes FACE, RECOGNIZE and READ, their results, `enrol` and `forget`; opening an app from the phone; the Camera app; a second viewer; recording on either side.

## Risks

- The gateway's surface grows by six methods for a phone (five of which reach an app inside the shell) and one, `vision.pull`, that only the gateway calls. Each answers from memory, takes enumerated values only, and is refused unless Vision is in front.
- A picture is the largest thing the gateway sends. It is bounded per message and per second, and paced by the receiver.
- `vision_control` lets a paired phone change what the person at the unit sees on the Vision screen. It is a separate grant for that reason.

## Decision record

| Date | Decision | By |
| --- | --- | --- |
| | Amendment 1 accepted / changed / rejected (pending) | Product owner |
