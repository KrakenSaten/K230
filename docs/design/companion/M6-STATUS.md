# Companion M6: where it stands

Status: working notes of the M6 development session (2026-11-22). Not a contract; the contracts are `docs/api/gateway.md` and, for Vision, the proposal `VISION_PREVIEW.md`.

| Item | State |
| --- | --- |
| PacketShark room in the Android app | Done against the capture contract and a simulated source. Draft PR #173 (`feat/companion-m6-packetshark`) |
| Vision room in the Android app | Done against the proposed contract and a simulated source. `feat/companion-m6-vision`, stacked on #173 |
| Vision preview architecture | Proposed: `VISION_PREVIEW.md` |
| ADR-016 amendment 1 (capture for a companion) | Proposed since 2026-11-21; untouched |
| ADR-006 amendment 1, ADR-019 amendment 1 (Vision) | Proposed in this branch |
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
- The phone's screenshot behaviour for Vision: reviewed, recommendation in `VISION_PREVIEW.md` 7a, `FLAG_SECURE` set while a unit's picture is shown.
- The shell hook both features wait for: `SHELL_APP_METHODS.md`. Step H1 is next.
- **Not done: the Flip8.** No device was visible to ADB, so the APK was not installed on it and nothing was checked against Unit A from a phone.

## Not run in this session

- Anything on a physical phone.
- JPEG decoding of a preview picture at run time (the simulation gives raw pixels).
- Anything on a unit beyond read-only inspection.
