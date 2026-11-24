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
- On a unit: **PASS** on Unit A, 2026-11-23, from a tmpfs kit of abc9abbd, restored afterwards (state identical but for meshcored's own file). A real camera picture with its boxes and labels was shown live in the Companion app on an emulator on the bench PC: 4.1 to 4.2 pictures a second, 392 x 221, 6.5 KB each, age 18 to 45 ms. Granting the scope from the unit's console started the preview by itself; removing it ended the preview at once. Measured with the GENERAL 365 detector: the detector's rate falls from 18.3-19.1 to 16.7-16.9 a second with a viewer (about 10 %, the bound of `VISION_PREVIEW.md` section 6, not under it); `pos-vision` 44 % to 45 % of the hart, shell 14 % to 15 %, gatewayd under 1 %; encode 16 to 26 ms; `pos-vision` +0.2 MB; `vision.pull` worst 2.8 ms in the shell (2 of about 1280 over the 2 ms budget). Report and evidence: `C:\K230-artifacts\vision-stream\RESULTS.md`. Not done on the unit: STOP and ALLOW by a finger and the look of the mark, a physical phone, the private modes, a second phone, portrait.
- Still to measure on a unit (`VISION_PREVIEW.md` section 8): the same figures with the R0 detector and in TRAFFIC and SEGMENT, on a physical phone over Wi-Fi, in portrait, and over a long run.
- Rules without a test (from the independent review): STOP, hold and ALLOW in the real app except through the Android integration test's simulator taps; the app ending a share on a tap to a private mode; the helper's `sharefail mode`; the session's intake of a picture from shared memory on the default host build (no libjpeg); helper death during a share; a method named like `vision.pull` but longer.
- Known and left (all three settled on 2026-11-24, see "Finalization" below): a Vision app the shell restarts by itself forgets STOP (it lives in the app's memory); on the unit `vision.status` took up to 3.4 ms with 365 class names in it and `vision.mode` 4.6 ms, against the 2 ms handler budget; the app's "Not granted" text points to a Settings page that does not exist yet (the grant is a console command today).
- Encoder moved off the detector's loop (182c3556, `core/pocketvision/vision_share_worker.[ch]`): on Unit A, in a dark scene, the detector loses about 4 to 5 % with a viewer (19.1-20.1 without, 18.2-19.0 with), against about 10 % before in a bright scene. Not yet compared in one and the same scene.

## Finalization (2026-11-24)

The owner's brief: finish the existing feature, no new milestone, no redesign. What was found and changed in PR #174, each a defect seen or a gap the notes above already named:

| Found | Changed |
| --- | --- |
| No way on the unit's screen to give or take `vision` (the grant was a console command; the app's text named a Settings page that did not exist) | Settings > Companion: the gateway's state and one switch per paired phone, which sends `gateway.scope` (`apps/settings/settings_companion.c`, `docs/apps/SETTINGS.md` "Companion", DS section 52.8, proposed). Pairing and revoking are still not on that page (issue #172) |
| STOP was forgotten when the shell created the Vision app again by itself (a text size changed over IPC, an open request naming the app already open) | The shell tells an app when it is created again that way (`pocketos_shell_app_reopening`, `ui/shell/app.h`); Vision keeps STOP across it. Vision opened by a person still ends STOP, as accepted |
| The mark and STOP lay over the first row of the mode sheet in both orientations, and over the side column's text when wide. Never seen before: nobody had looked at the mark | The bar is placed on the picture, as wide as the picture; while a sheet is open it sits in the sheet's title band, above the first row (`share_place`, `apps/vision/vision_app.c`). STOP is a 64 px target on the picture and 52 px in the band |
| `vision.mode` took 4.6 ms on Unit A against the 2 ms handler budget: the handler wrote the settings file (with an fsync) and laid the screen out | The model changes in the answer; the helper's word, the layout and the file wait for the app's next poll, at most 33 ms |
| `vision.status` made 365 class names anew on every call (once a second while a phone looks) | Made once per detector and referred to. On this host 7.9 us became 1.4 us; the 3.4 ms once seen on Unit A is about 400 times the host's figure, so most of it was the hart being taken by the helper, not this code. To be measured again on a unit |
| `vision.pull` over 2 ms in 2 of about 1280 calls on Unit A (worst 2.8 ms) | Not changed. The handler is a copy and a base64 of about 6.5 KB; on this host its worst was 0.6 ms in the slowest run. Read as scheduling on one hart, not as a defect, and to be measured again on a unit |
| The app told a phone without the scope "Not served by this unit", because a gateway names `vision` only to a phone that holds it; the instruction to switch it on was never shown | The app asks `vision.status` once: error 7 is "Not granted" with the instruction, anything else is "Not served" (`GatewayVision.kt`) |
| The app's texts sent people to Settings pages that do not exist (pairing, capture) | Corrected to what the unit has; Vision's names the new switch |
| `tests/vision_lint.sh` failed since the encoder thread of 2026-11-23 ("no threads anywhere in Vision"), so `make vision-test` failed | The rule is now: one thread, the shared picture's encoder in the helper, started by the helper alone |
| `android/doors-companion/README.md` still said real streaming had never run | Brought up to date |

**Sharing ends at once: host evidence.** `tests/companion_vision_integration_test.py` (new; 96 checks, all passing): the real gatewayd, the real simulator shell with the Vision app and Settings, the real helper with libjpeg, TLS clients, and the simulator's finger. Measured from the start of each act to the unit no longer sharing, on this host: STOP 124 ms and FACE, READ, RECOGNIZE 169 to 186 ms (each includes the 80 ms the simulated finger is held down and a 50 ms sampling step); the viewer disconnecting 1 ms; the scope taken away 11 ms; the viewer revoked 14 ms; Vision left 27 ms. After each, no picture reached the phone. STOP held through two text size changes and an open request for the app already open; ALLOW let the phone back.

**The app against the bench** (the Companion app on an emulator; the real gatewayd; the simulator shell on the fake camera; not a unit, not a phone): paired with the default scopes it showed "Not granted to this phone" with the instruction; one tap on the unit's Settings switch and the picture appeared by itself on the app's Vision screen, upright, with its box, label and figures, 4.2 to 4.5 a second, in portrait and in landscape; STOP on the unit showed "Stopped on the unit"; ALLOW brought the picture back; FACE on the unit showed "This mode's picture stays on the unit"; the switch turned off returned the app to "Not granted". Screens: `C:\K230-artifacts\vision-stream\finalization\screens`.

**Run on the host:** `vision_remote_test` 88 checks (93 with libjpeg), also under ASan and UBSan; `vision_model_test` 908; `vision_session_test` 250; `settings_view_test` 210; `settings_app_test` 1660 (every page at three text sizes, both ways up); `settings_shell_test`, `settings_lint`, `vision_lint`, `shell_app_methods_test`, `capture_status_shell_test`, `vision_shell_test`, `vision_remote_shell_test` (with and without libjpeg): all passing. `companion_capture_integration_test.py` 57 and `gatewayd_vision_e2e_test.py` 135: passing. Android: 80 unit tests, none failing; `VisionIntegrationTest` against the real gatewayd, shell and helper: passing (`VisionTest.withoutTheScopeOrOffScreen...` timed out once while the host was loaded and passed twice alone; untouched code). Cross-built for the K230 (production shell, `pos-vision` with KPU and libjpeg).

**The shell change against every shell suite.** All 56 `tests/*_shell_test.sh` were run against this build and, for each suite with a failing line, against a build of the head before this work (7b1cd87d): 31 suites pass; 25 have failing lines, and in every one of them the failing lines are the same on both builds (helpers that the mirror did not build, and expectations that are stale on master: chrome, launcher groups, text size layout, system brand and others). One line more in `back_corner_shell_test` in the first run, made while the host was loaded, was gone when the suite was run again alone. Nothing fails that did not fail before.

**Independent review** of the finalization changes: no blocking defect. Its findings that were acted on: one missed poll of gatewayd no longer reads "Not running" (three in a row do); a tap on a row whose list changed under it is now tested to send nothing; the mark's place is asserted, not only pictured; STOP in a sheet is said to be 52 px, as it is; a mode a phone chose a moment ago is told to the helper before a share starts; a name of spaces alone is "Unnamed phone". Left as they are: the over-limit label set is measured on every status (cost only); no test of a setting changed by a phone and Vision left within 33 ms (the code stores it on the way out).

**Hardware: validated later the same day, see "Hardware validation (2026-11-24, evening)" below. What this paragraph said in the afternoon:** not validated in this session. On 2026-11-24 the bench PC had no serial port to a unit (COM5 absent), no phone on ADB, and was on a network from which Unit A (192.168.10.171) did not answer; Unit A's root password is also not known to this session since the owner's reboot. Nothing was deployed, started or stopped on any unit, so there was nothing to restore. What stands from 2026-11-23 is the PASS above, for streaming to an emulator.

Still to be done on a unit and a phone, none of it claimed: the picture on a physical phone over Wi-Fi; STOP, ALLOW and the Settings switch under a real finger and how they look on the panel; FACE, RECOGNIZE and READ ending a share on hardware; disconnect and reconnect on a real network; portrait on the unit; GENERAL, SEGMENT and TRAFFIC figures; a long run; the detector's rate with and without a viewer in one and the same scene (the 2026-11-23 figures are from two scenes); the handler times after these changes.

**Owner's calls, not made here:** DS section 52.8 (a new Settings category) is proposed, not accepted. A phone with `vision` can choose DETECT while the unit is in FACE and is then shown the picture; the decisions neither allow nor forbid it (found by the review; behaviour unchanged).

## Hardware validation (2026-11-24, evening)

Unit A (device 53fc1db8, installed shell f8939dbb) and the owner's Flip8 (SM-F776B), over the owner's Wi-Fi. A tmpfs kit of 03a5d025 in `/tmp/vsk` (production shell without test hooks, `pos-vision` with KPU and libjpeg, gatewayd without test hooks, on a tmpfs copy of the gateway's state); the test build of the app on the phone, its data kept. The owner ended the session early (the bench laptop's battery); what was finished stands, what was not is NOT TESTED, not failed. Evidence: `C:\K230-artifacts\vision-stream\finalization\evidence` and `...\phone`.

| Check | Result | Seen |
| --- | --- | --- |
| Real K230 camera to a physical phone | **PASS** | Live picture on the Flip8, 392 x 221, about 5.4 to 6.6 KB each, 4.2 to 5.0 a second, age 6 to 31 ms on the phone |
| A phone without `vision` | **PASS** | "Not granted to this phone" with the instruction; "Gateway serves it: Yes" |
| Granting by the switch on Settings > Companion, by a finger | **PASS** | The owner's tap; gatewayd logged the scope change; the page was not reported wrong |
| Automatic preview when Vision opens | **PASS** | No touch on the phone: the preview began 2 s after Vision was opened (16 s after the grant), and 3 s after Vision was opened again later |
| Orientation, boxes, labels, mask, live figures | **PASS** | Upright; boxes with "cup", "telephone", "desk", "cell-phone" and their confidences; INSTANCES masks in place; the detector's figures live (18 to 19 a second, 29 to 31 ms, 44 to 45 %) |
| Portrait and landscape on the phone | **PASS** | Both, picture and boxes in place |
| Modes from the phone | **PASS** | detect, track, instances, traffic, segment, color, edge and trace chosen; the picture stayed live in each |
| STOP by a finger | **PASS** | The unit ended the share at the tap; gatewayd saw it 132 ms later; the phone read "Stopped on the unit" |
| ALLOW by a finger | **PASS** | The picture was back 0.45 s later with no touch on the phone |
| Leaving Vision | **PASS** | The preview ended with the screen; no helper left |
| Disconnect and reconnect | **PASS** | The app killed on the phone: the unit stopped sharing about 0.17 s later ("its client went away"). The app opened again: the preview resumed when its Vision screen opened |
| Taking the scope away and giving it back | **PASS**, from the console | Ended in the same millisecond at the gateway and 3 ms later on the unit; given back, the picture resumed 0.13 s later with no touch. The switch's OFF was not tapped on hardware |
| FACE, RECOGNIZE, READ | **NOT TESTED** | Unit A has no face or text models, so the modes are not offered there. Nothing was installed |
| GENERAL (DETECT, 365 classes) with and without a viewer, one scene | **PASS** | 18.1 to 18.2 a second with the phone viewing, 19.1 to 19.4 without, 17.9 to 18.1 with it again: about 6 % lower, inside the 10 % bound. `pos-vision` 46 % against 43 % of the hart; shell 16 %; gatewayd 0 %. Encode 24 ms, 31 ms at most |
| SEGMENT with a viewer | Measured | 12.7 to 12.9 a second, `pos-vision` 44 to 45 %, 21.9 MB. No figure without a viewer |
| TRAFFIC figures | **NOT TESTED** | The run was interrupted before its figures were read |
| Sustained streaming | **NOT TESTED** as a run | Interrupted about two minutes in. What exists: the phone was shown the picture for most of 16 minutes with no error in either log and no failed picture (`share_failed` 0 of about 1450) |
| Handler budget (2 ms) | Measured | 9 of 6504 calls over, worst 4.9 ms (`vision.pull`); `vision.status` once at 2.4 ms. Still scheduling on one hart by every sign (commands on the console at the same moment halve the detector's rate too), and still over the budget now and then |
| The mark's look, the switch's look, STOP's size under a finger | Owner's eyes | Nothing was reported wrong; no picture of the panel exists (a production shell takes none) |

**Unit A afterwards.** Restored with the kit's own script and checked: the installed shell f8939dbb is the only shell and the only DRM holder; no gatewayd runs, as before; the kit is gone from `/tmp`; every file under `/etc/pocketos` and `/var/lib/pocketos`, `/etc/shadow` and the installed binaries have the checksums taken before, except meshcored's own `state.v1`. `eth0`, taken down for the test because it had lost its carrier and still held the route to the phone's network, is up again as it was found (no carrier). The root password is the one the owner set that evening. Unit B was not touched.

**The phone afterwards** keeps the test build (03a5d025); the build it had is saved as `C:\K230-artifacts\vision-stream\finalization\phone\installed-before.apk`. Its pairing holds `status`, `mesh_read`, `mesh_send` on the unit, as before: the grant of `vision` was made on the kit's tmpfs copy and went with it.

**Found on hardware, not fixed:** a box that touches the picture's top edge has its label under the app's LIVE and mode chips.
