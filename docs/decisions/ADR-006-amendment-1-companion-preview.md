# ADR-006 amendment 1: Vision's picture and a paired companion

Status: **Proposed** (draft for the owner). Until it is accepted ADR-006 stands as written and no picture leaves a unit.
Date: 2026-11-22
Deciders: product owner (final), AI engineering partner (author)
Depends on: ADR-019 (accepted) and its amendment 1 (proposed). Without both this amendment has no effect.
Design: `docs/design/companion/VISION_PREVIEW.md`.

## Why

A paired phone should be able to show what the unit's Vision app is looking at. ADR-006 decided that one helper per screen is the only thing that opens the camera, and that pictures travel from it to the app over sealed shared memory because "pictures cannot travel in pocketipc's JSON". It names three reasons to revisit the decision; one is "anything outside the shell needs the camera". A companion's preview is close enough to that to need the owner, even though, as proposed, nothing outside the shell touches the camera.

## What does not change

- The helper is the only opener of the camera, for exactly as long as its screen is open. No camera service, no second opener, no camera without the screen.
- Camera's helper and Vision's are never alive together. The sensor has one owner at a time.
- Full-size pictures stay in the sealed memory between a helper and its app.
- The Camera app is not part of this: nothing of it is shared.

## What is added

1. While the Vision screen is open and a person at the unit has allowed it for this session, Vision's helper may also write a **reduced** copy of the picture it already makes for the screen (half size by default, the longest side 640 at most, baseline JPEG, 64 KiB at most) into the unused fourth slot of the same sealed memory.
2. The Vision app may hand that reduced copy, and only that, to the gateway of ADR-019 as the answer to a request, for one paired client at a time. This is the one place a picture travels in JSON; the rule that full pictures do not is kept.
3. The copy is made only on request, at most five times a second, and never faster than the viewer acknowledges.
4. The unit shows who is being shown the picture, on every Vision screen, with a STOP; a share ends with the screen, with STOP, with the modes FACE, RECOGNIZE and READ, with the client, with its permission, and after ten minutes.
5. Nothing is written to storage for a share, on the unit or on the phone.

## Not allowed, now or as an option of this amendment

A share that outlives the Vision screen; a share nobody at the unit allowed; more than one viewer; a full-size picture or a still; recording; anything of a face, an enrolment or an owner; the Camera app's pictures; any process but the helper opening the camera.

## Relation to `camerad`

Not introduced and not needed. If a later feature needs the camera while no screen is open, that is ADR-006's own trigger and a new decision.

## Risks

- A share costs processor time in the helper on a unit with one hart. Bounded and to be measured before shipping (`VISION_PREVIEW.md` sections 6 and 8).
- Stopping a stream has frozen the unit before (`docs/KNOWN_ISSUES.md`). A share starts and stops nothing of the stream: it only adds or stops an encode of pictures that are made anyway.
- A person who taps ALLOW shows the room to a paired phone for up to ten minutes.

## Decision record

| Date | Decision | By |
| --- | --- | --- |
| | Amendment 1 accepted / changed / rejected (pending) | Product owner |
