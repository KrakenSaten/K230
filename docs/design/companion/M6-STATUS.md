# Companion M6: where it stands

Status: working notes of the M6 development session (2026-11-22). Not a contract; the contracts are `docs/api/gateway.md` and, for Vision, the proposal `VISION_PREVIEW.md`.

| Item | State |
| --- | --- |
| PacketShark room in the Android app | Done against the capture contract and a simulated source. Draft PR #173 (`feat/companion-m6-packetshark`) |
| Vision room in the Android app | Done against the proposed contract and a simulated source. `feat/companion-m6-vision`, stacked on #173 |
| Vision preview architecture | Proposed: `VISION_PREVIEW.md` |
| ADR-016 amendment 1 (capture for a companion) | Proposed since 2026-11-21; untouched |
| ADR-006 amendment 1, ADR-019 amendment 1 (Vision) | Proposed in this branch; revised 2026-11-23 to the trusted-device model and **accepted by the owner the same day** |
| gatewayd, shell, PacketShark app, Vision app, `pos-vision` | Not touched. Nothing new is served by any unit |

## Next, in order

1. Owner: accept, change or reject the three amendments.
2. The app-method hook on `shell.sock` (needed by capture and by Vision; `CONTRACTS.md` section 3). Its design is the first unit-side task of either feature.
3. Capture on the unit: the four `capture.*` methods in PacketShark, the request sheet, the header mark, gatewayd rows, the lint extension. Then `GatewayCapture` against the WSL harness and a unit.
4. Vision on the unit, only if its amendments are accepted: the measurements of `VISION_PREVIEW.md` section 8 first (items 1 and 2 decide the default rate), then helper, app and gateway.

## Run in this session

- The Android unit tests: 71, none failing (22 new: `CaptureTest`, `VisionTest`).
- The three gatewayd harness tests of the Android suite (`GatewayIntegrationTest`, `MeshIntegrationTest`, `SendIntegrationTest`) against the reference gatewayd built from this branch in WSL, after `GatewayConnection.subscribe` was introduced: all pass.
- The screens on an emulator, simulated sources, portrait and landscape.

## Integration readiness pass (2026-11-22, later the same day)

- Vision API: the phone's side and the unit's side are now two named surfaces (`VISION_PREVIEW.md` 5.1 and 5.2). A phone sees the event `vision.frame` and calls `vision.ack`; gatewayd calls `vision.pull` on the shell. The amendments stay Proposed.
- All 72 Android tests, the three gatewayd harness tests among them, pass after the review fixes (none skipped).
- Emulator against the reference gatewayd (not Unit A, not a physical phone): pairing, status, RIFT live, a draft kept through two rotations, a channel message sent and accepted; PacketShark and Vision both "Not served". No crash.
- The phone's screenshot behaviour for Vision: reviewed, recommendation in `VISION_PREVIEW.md` 7a, `FLAG_SECURE` set while a unit's picture is shown (withdrawn on 2026-11-23, see below).
- The shell hook both features wait for: `SHELL_APP_METHODS.md`. Step H1 is next.
- **Not done: the Flip8.** No device was visible to ADB, so the APK was not installed on it and nothing was checked against Unit A from a phone.

## Not run in this session

- Anything on a physical phone.
- JPEG decoding of a preview picture at run time (the simulation gives raw pixels).
- Anything on a unit beyond read-only inspection.

## Trusted-device revision (2026-11-23)

The owner asked for a simpler policy for a personally owned unit and its own phone. Changed in this branch, and accepted by the owner on 2026-11-23 (both amendments, as written):

- Both amendments and `VISION_PREVIEW.md` rewritten: the phone is authorized once (one scope, `vision`); the preview starts by itself; no sheet, no tap per preview, no ten-minute limit, no 30 s request limit; screenshots and recording allowed on the phone. Each amendment ends with a table of the product choices for the owner to accept or change.
- Kept: one camera owner, one viewer, TLS and pinned keys, the scope checked on every request, revocation, the mark and STOP on the unit (STOP now holds against the phone until resumed there), no FACE, RECOGNIZE or READ, the picture and rate bounds, acknowledgement and the pull pacing.
- `VISION_PREVIEW.md` 5.2 no longer uses `shell.subscribe` (ADR-019 rule 6 forbids it, as the capture work found): the gateway makes its events from its own `vision.status`.
- Android: `GatewayVision` asks for the picture by itself and holds back on `stopped`, `busy`, a refusal and a pause on the phone; `PreviewState` is `OFF, LIVE, STALLED, STOPPED, BUSY`; `canControl` and the two scopes are gone; `FLAG_SECURE` removed; the simulation starts by itself. 78 unit tests, none failing, 4 skipped (the gatewayd harness tests were not run this time); `:app:assembleDebug` builds.
- master (through #184) merged into the branch; the capture stack this was stacked on is in master.
- Not done: nothing was looked at on an emulator or a phone after this change; no gatewayd, shell or helper code exists for Vision; nothing was deployed.
- Next on the unit side, each its own reviewable step: the measurements of `VISION_PREVIEW.md` section 8 (items 1 and 2 first), the helper's `share` commands, the Vision app's `vision.*` handlers with the mark and STOP, then gatewayd. The larger answer size `vision.pull` needs, and a way to grant `vision` to a paired phone (#172), are open.
