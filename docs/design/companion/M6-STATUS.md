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

## Real streaming implemented (2026-11-23, later)

Built in this branch: helper JPEG sharing, the Vision app's `vision.*` methods with the mark and STOP, the shell's one larger answer for `vision.pull`, gatewayd's vision group with `gateway.scope`, and the Android room on real frames. Details: `docs/apps/VISION.md` "Shown to a paired companion", `docs/api/gateway.md` 9a, `docs/api/shell.md`.

- Host, end to end: PASS. The Android client and the real app on an emulator were sent real JPEGs made by the real helper (fake camera) through the real Vision app and the real gatewayd.
- On a unit: **PASS** on Unit A, 2026-11-23, from a tmpfs kit of abc9abbd, restored afterwards (state identical but for meshcored's own file). A real camera picture with its boxes and labels was shown live in the Companion app on an emulator on the bench PC: 4.1 to 4.2 pictures a second, 392 x 221, 6.5 KB each, age 18 to 45 ms. Granting the scope from the unit's console started the preview by itself; removing it ended the preview at once. Measured with the GENERAL 365 detector: the detector's rate falls from 18.3-19.1 to 16.7-16.9 a second with a viewer (about 10 %, the bound of `VISION_PREVIEW.md` section 6, not under it); `pos-vision` 44 % to 45 % of the hart, shell 14 % to 15 %, gatewayd under 1 %; encode 16 to 26 ms; `pos-vision` +0.2 MB; `vision.pull` worst 2.8 ms in the shell (2 of about 1280 over the 2 ms budget). Report and evidence: `C:\K230-artifactsision-stream\RESULTS.md`. Not done on the unit: STOP and ALLOW by a finger and the look of the mark, a physical phone, the private modes, a second phone, portrait.
- Still to measure on a unit (`VISION_PREVIEW.md` section 8): the same figures with the R0 detector and in TRAFFIC and SEGMENT, on a physical phone over Wi-Fi, in portrait, and over a long run.
- Rules without a test (from the independent review): STOP, hold and ALLOW in the real app except through the Android integration test's simulator taps; the app ending a share on a tap to a private mode; the helper's `sharefail mode`; the session's intake of a picture from shared memory on the default host build (no libjpeg); helper death during a share; a method named like `vision.pull` but longer.
- Known and left: a Vision app the shell restarts by itself forgets STOP (it lives in the app's memory); on the unit `vision.status` took up to 3.4 ms with 365 class names in it and `vision.mode` 4.6 ms, against the 2 ms handler budget; the app's "Not granted" text points to a Settings page that does not exist yet (the grant is a console command today).

