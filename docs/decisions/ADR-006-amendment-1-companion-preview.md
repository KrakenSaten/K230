# ADR-006 amendment 1: Vision's picture and a trusted companion

Status: **Proposed** (draft for the owner; revised 2026-11-23 to the trusted-device model the owner asked for). Until it is accepted ADR-006 stands as written and no picture leaves a unit.
Date: 2026-11-22, revised 2026-11-23
Deciders: product owner (final), AI engineering partner (author)
Depends on: ADR-019 (accepted) and its amendment 1 (proposed). Without both this amendment has no effect.
Design: `docs/design/companion/VISION_PREVIEW.md`.

## Why

A paired phone should be able to show what the unit's Vision app is looking at. ADR-006 decided that one helper per screen is the only thing that opens the camera, and that pictures travel from it to the app over sealed shared memory because "pictures cannot travel in pocketipc's JSON". It names three reasons to revisit the decision; one is "anything outside the shell needs the camera". A companion's preview is close enough to that to need the owner, even though, as proposed, nothing outside the shell touches the camera.

The first draft treated every preview as a new act of consent: a tap on the unit per share, ten minutes at most, no screenshots on the phone. The owner's direction of 2026-11-23 is that this is more than a personally owned unit and its own trusted phone need. This revision keeps the camera architecture and the technical bounds, and replaces the repeated consent with one authorization of the phone.

## What does not change

- The helper is the only opener of the camera, for exactly as long as its screen is open. No camera service, no second opener, no camera without the screen.
- Camera's helper and Vision's are never alive together. The sensor has one owner at a time.
- Full-size pictures stay in the sealed memory between a helper and its app.
- The Camera app is not part of this: nothing of it is shared.

## What is added

1. **Authorized once.** A paired phone that was given the `vision` permission on the unit (ADR-019 amendment 1) is a trusted viewer until that permission or the pairing is revoked. No further confirmation is asked at the unit for a preview.
2. **Automatic while both are open.** While the Vision screen is in front on the unit and a trusted phone has its own Vision screen open, Vision's helper may write a **reduced** copy of the picture it already makes for the screen (half size by default, the longest side 640 at most, baseline JPEG, 64 KiB at most) into the unused fourth slot of the same sealed memory, and the Vision app may hand that copy, and only that, to the gateway of ADR-019 for one phone at a time. This is the one place a picture travels in JSON; the rule that full pictures do not is kept.
3. **No time limit.** A preview lasts while Vision stays in front and the viewer stays connected and keeps acknowledging.
4. **Paced by the viewer.** The copy is made only on request, at most five times a second, and never faster than the viewer acknowledges.
5. **Visible, with a STOP.** The unit shows who is being shown the picture, on every Vision screen, with a STOP that ends the preview at once. After STOP the picture stays on the unit until a person there resumes it or Vision is opened again; a phone cannot undo it.
6. **A preview ends** with the Vision screen, with STOP, with the modes FACE, RECOGNIZE and READ, when the viewer leaves its Vision screen, disconnects or stops acknowledging, and when its permission or pairing is revoked.
7. **The unit and the app store nothing** for a preview. What the phone's owner does with their own screen (a screenshot, a screen recording) is theirs and is not prevented.

## Not allowed, now or as an option of this amendment

A preview that outlives the Vision screen; a preview for a phone without the permission; more than one viewer; a full-size picture or a still; recording by the unit or by the app; anything of a face, an enrolment or an owner; the Camera app's pictures; opening Vision or the camera from a phone; any process but the helper opening the camera.

## Relation to `camerad`

Not introduced and not needed. If a later feature needs the camera while no screen is open, that is ADR-006's own trigger and a new decision.

## Risks

- A preview costs processor time in the helper on a unit with one hart. Bounded and to be measured before shipping (`VISION_PREVIEW.md` sections 6 and 8).
- Stopping a stream has frozen the unit before (`docs/KNOWN_ISSUES.md`). A preview starts and stops nothing of the stream: it only adds or stops an encode of pictures that are made anyway.
- A trusted phone sees the room whenever Vision is in front on the unit, with no further act at the unit. What stands against a lost or misused phone is the mark and STOP on the unit, revocation there, and that a phone can never open Vision itself. Accepted as the price of not asking every time.
- Pictures can leave the phone as screenshots or recordings its owner makes. Accepted: it is the owner's phone.

## Product choices in this revision, for the owner

| # | Choice proposed | Alternative |
| --- | --- | --- |
| 1 | One authorization of the phone; no tap per preview | The first draft: a tap on the unit for every preview |
| 2 | Preview starts by itself when both Vision screens are open | The phone asks with a button each time |
| 3 | No time limit | A limit (the first draft had ten minutes) |
| 4 | STOP on the unit holds until resumed there or Vision is reopened | STOP only interrupts; the phone may start again at once |
| 5 | Screenshots and screen recording allowed on the phone | `FLAG_SECURE` while a unit's picture is shown |

## Decision record

| Date | Decision | By |
| --- | --- | --- |
| 2026-11-23 | Direction given: trusted-device model (one authorization, automatic preview, no time limit, screenshots allowed); draft revised accordingly. The amendment itself is still pending | Product owner |
| | Amendment 1 accepted / changed / rejected (pending) | Product owner |
