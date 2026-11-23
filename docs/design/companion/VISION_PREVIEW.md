# Companion: Vision preview (design)

Status: **Design accepted in principle; nothing here is implemented on a unit.** Its two decisions, `docs/decisions/ADR-006-amendment-1-companion-preview.md` and `docs/decisions/ADR-019-amendment-1-vision.md`, were accepted by the owner on 2026-11-23. The measurements of section 8 still come before anything ships, and the wire format of section 5 becomes a contract only when it is written into `docs/api/gateway.md` with the gateway code. The Android side exists against a simulated source (`android/doors-companion`, README "Vision"); its gateway client is written to section 5 and has never met a gateway.
Date: 2026-11-22, revised 2026-11-23 (trusted-device model: one authorization of the phone, automatic preview, no time limit, one scope, screenshots allowed)

Evidence classes as in the hardware baseline: VERIFIED (seen on a unit), DOCUMENTED (in this repository's code or documents), ASSUMED (reasoned, not measured).

## 1. What is wanted

A paired phone shows what the unit's Vision app is looking at: the picture, the boxes with class, confidence and track id, the mode, the detector's figures. It can choose a mode and the settings Vision's own sheets offer. It does this without a second opener of the camera and without a camera service. The phone is authorized once, on the unit; after that the preview appears by itself whenever Vision is in front on the unit and open on the phone, with a mark and a STOP on the unit, and no picture goes to a phone that was not authorized.

## 2. What exists today

| Fact | Class |
| --- | --- |
| `pos-vision` (`tools/vision/pos_vision.c`) is the only process that opens the camera while Vision is in front; it is a child of the shell's Vision session (socketpair, `PR_SET_PDEATHSIG`), and exits with the screen | DOCUMENTED (`apps/vision/vision_session.c`, `docs/apps/VISION.md` "Architecture") |
| Capture is `/dev/video2`, 640 x 360, planar `BG3P`, four MMAP buffers; the driver refuses a second opener (EBUSY at REQBUFS). Doors has no lock of its own | DOCUMENTED (`core/pocketcam/pocketcam_v4l2.c`, ADR-006) |
| No frame goes to a display plane. The helper turns and scales each frame to RGB565 at the picture's size (for example 528 x 938) into a sealed memfd, at most every 100 ms; the shell copies the newest slot into an LVGL image | DOCUMENTED (`pos_vision.c` preview path, `pocketcam_proto.h`: four slots of 2 MiB, `POCKETCAM_PREVIEW_MIN_INTERVAL_MS 100`) |
| Slot 3 of that memory is not used by Vision | DOCUMENTED (`pocketcam_proto.h`) |
| In the pixel and segmentation modes the helper paints the result into that RGB565 picture; in the box modes the app draws boxes from text | DOCUMENTED (`pos_vision.c`, `vision_app.c`) |
| Boxes travel as `det seq n id:cls:conf:x:y:w:h:dir:kmh10`, in the picture's pixels, confidence per-mille, at most 24; figures as `stats fps_x10 infer_ms pre_ms post_ms cpu_pct rss_kb bad dropped` once a second | DOCUMENTED (`core/pocketvision/pocketvision_proto.h`) |
| `pos-vision` already links IJG libjpeg (the Camera's still codec, `core/pocketcam/pocketcam_codec.c`, `POCKETCAM_JPEG=1` on the device). There is no hardware encoder in use anywhere in first-party code: no VENC, no MPP call | DOCUMENTED (Makefile, `platforms/k230/package/pocketos/pocketos.mk`) |
| Linux has one hart. Unit A tonight: `nproc` 1, 990 MB RAM, 916 MB available, CMA 450 of 512 MB free with Vision closed, gatewayd 7.8 MB RSS in one thread | VERIFIED (Unit A, 2026-11-22, read-only) |
| The detector runs at 11 to 19 frames a second depending on model and mode; `pos-vision` takes 38 to 50 % of the hart, the shell 8 to 18 % | DOCUMENTED (`docs/apps/VISION.md`, `docs/hardware/VISION_GATE.md`) |
| The gateway carries UTF-8 JSON frames only, 256 KiB at most outward, and skips snapshot events for a client with more than 128 KiB unread | DOCUMENTED (`docs/api/gateway.md` sections 1 and 7) |
| An app inside the shell cannot serve a method on `shell.sock` yet; ADR-016 amendment 1 needs the same hook | DOCUMENTED (`docs/design/companion/CONTRACTS.md` section 3) |
| The camera LED is lit by the shell while a camera node is open | DOCUMENTED (`ui/shell/hw_activity.h`) |

## 3. The choices

**Where the picture comes from.**

| Option | Verdict |
| --- | --- |
| A second opener of the camera (gatewayd or a new helper) | Rejected. The driver refuses it, ADR-006 forbids it, and it would be a camera that runs without the Vision screen |
| `camerad`, a camera service (ADR-006 option B) | Rejected for this. It is the larger change ADR-006 keeps for when it is needed; a preview of the screen that is already open does not need it |
| The sensor frame (640 x 360 `BG3P`) | Rejected. It is sideways or upside down until turned, is not what the person at the unit sees, and lacks what the pixel and segmentation modes paint |
| **The RGB565 picture the helper already makes for the shell** | **Proposed.** Already turned, mirrored and fitted; its boxes are already in its pixels; the phone sees what the unit's screen shows and nothing else, so the mark on the unit means exactly what it says |

**How it is encoded.** No hardware encoder is assumed. A video codec in software is beyond one hart that is already half used. Raw RGB565 at half size (264 x 469) is 248 KB a picture: too much for the frame limit and for Wi-Fi at any useful rate. Baseline JPEG with the libjpeg the helper already links is the one choice that needs no new dependency: half size, quality about 50, an expected 8 to 20 KB a picture (ASSUMED).

**Where it is encoded.** In the helper, not in the shell (ADR-002, ADR-007: the shell's loop does no such work) and not in gatewayd (it would need the pixels, and a way to get them). The helper writes the JPEG into slot 3 with a length, and says `share slot seq w h len`.

**How it reaches the gateway.**

| Option | Verdict |
| --- | --- |
| Pass the memfd to gatewayd (SCM_RIGHTS) | Not in v1. Fewer copies, but gatewayd would hold a descriptor of the camera's memory for as long as it liked, and the gateway, a plain pocketipc client today, would have to take descriptors |
| A second socket from the helper to gatewayd | Rejected. The helper would have a peer besides its parent |
| **The Vision app answers `vision.pull` on `shell.sock` from memory** | **Proposed.** One bounded copy and a base64 of at most 64 KiB in a handler that does nothing else; it uses the same app-method hook ADR-016 amendment 1 needs. To be measured: the handler's time in the LVGL loop (budget 2 ms) |

**How it is paced.** Pulled, not pushed. The gateway asks the Vision app for a picture only when its one viewer has acknowledged the previous one and the interval has passed; the app asks the helper to encode only then. So a slow phone or a slow network lowers the frame rate at the source: nothing is encoded that will not be sent, and there is no queue anywhere. At most one picture is held at each stage (helper slot, gateway, phone), the newest.

**Boxes and pixels.** A picture (the answer to `vision.pull`, passed on as the event `vision.frame`) carries the boxes of the helper's newest `det` line, scaled into the shared picture's pixels by the Vision app, in the same message as the picture. They cannot drift apart, and the phone needs no geometry of the unit's.

## 4. The path

```
GC2093 -> ISP -> /dev/video2 -> pos-vision  (the only opener; unchanged)
                                  |- detector, tracker            -> det, stats lines   (unchanged)
                                  |- RGB565 picture, slots 0..2   -> shell -> panel     (unchanged)
                                  '- while sharing, on request:
                                     that picture, halved, JPEG   -> slot 3 + `share` line
shell, Vision app: answers vision.status / vision.pull from memory; draws the sharing mark and STOP
gatewayd:          one viewer; asks vision.status about once a second for its events; calls vision.pull when the viewer sent vision.ack; passes the answer on as the event vision.frame
phone:             one picture held; decodes off the main thread; draws picture and boxes with one transform
```

New helper commands: `share on <max_side> <quality>`, `share next`, `share off`. New event: `share slot seq w h len`. Protocol version 4; the app and the helper ship together.

## 5. Gateway API (proposed addition to `docs/api/gateway.md`)

There are two surfaces, and they do not have the same names for a picture. **5.1 is what a phone sees**; it is what `GatewayVision` in the Android app is written to. **5.2 is what gatewayd calls on the shell's socket**; a phone never sees it. A picture is *pulled* upstream (`vision.pull`, a request with an answer) and *pushed* to the phone (`vision.frame`, an event), and the phone's `vision.ack` is what allows the next pull. `vision.ack` is answered by gatewayd itself and never reaches the shell; `vision.pull` is not a method a phone can call (code 1, like any method outside the allowlist).

### 5.1 The phone's side

Capability `vision`. Topic `vision`. One scope, `vision`, for every method below.

| Method | Params | Result |
| --- | --- | --- |
| `vision.status` | - | `available` (false and nothing else unless Vision is the app in front), `owner` (`vision`, `camera`, `none`), `mode`, `modes[]` (what the helper can run), `settings{}`, `model`, `labels[]` (the detector's class names), `pic{w,h}`, `stats{fps_x10, infer_ms, pre_ms, post_ms, cpu_pct, rss_kb, dropped, count_ab, count_ba}`, `share` (`off`; `live`: this phone is the viewer; `stopped`: STOP was tapped on the unit; `busy`: another phone is the viewer) |
| `vision.share` `{on}` | | `{state}`. `on: true` starts the preview at once and answers `live`; the app sends it by itself when its Vision screen opens. No sheet and no tap on the unit. Error 3: Vision is not in front, the mode is one that is never shared, the unit is locked, or STOP was tapped on the unit. Error 5: another phone is the viewer. `on: false` always succeeds |
| `vision.ack` `{seq}` | the `seq` of the newest `vision.frame` received | `{}`. Answered by the gateway alone: it marks the viewer ready, and the gateway then pulls the next picture (5.2) no sooner than the interval allows. An unknown or old `seq` is ignored, not an error. The first picture of a preview needs no acknowledgement |
| `vision.mode` `{mode}` | one of `detect track traffic segment instances color edge trace` that is in `modes[]` | `{}` |
| `vision.set` `{key, value}` | `track.line`, `traffic.range`, `traffic.line`, `traffic.speed`, `color.tol`, `edge.hard`, `trace.dark`, `seg.view`, each with the values of `docs/apps/VISION.md` "Stored settings" | `{}` |
| `vision.reset` | - | `{}`; TRACK's and TRAFFIC's counters |

Events (topic `vision`): `vision.state {}` on any change of mode, owner, sharing or settings (the client asks `vision.status`); `vision.stats {…}` once a second while Vision runs; `vision.frame {seq, w, h, age_ms, jpeg, boxes[[id, cls, conf, x, y, w, h, dir, kmh10], …]}` only while sharing is `live`, to the viewer only, `jpeg` being base64 of a baseline JPEG.

A preview has no time limit. It follows the unit: when the unit leaves a mode that is never shared, or Vision comes to the front again, the phone sees `share: off` and asks again by itself. It does not ask while `share` is `stopped` or `busy`.

### 5.2 The unit's side (gatewayd to the Vision app, on `shell.sock`)

Served by the Vision app while it is in front, through the app-method hook (`SHELL_APP_METHODS.md`, merged for `capture`); "not served" when another app is in front, which the gateway passes on as `available: false`.

| Upstream method | Called when | Params | Answer |
| --- | --- | --- | --- |
| `vision.status` | a phone calls `vision.status`; and by the gateway itself about once a second while a client holds the topic (its events are made from the answers, as for `capture`) | - | as 5.1, with `share` as `off`, `live` or `stopped` and, when live, the viewer's `client`; the gateway turns `live` into `busy` for every phone but the viewer |
| `vision.share` | a phone calls `vision.share`; and with `on: false` whenever the gateway ends a preview itself (the viewer disconnected, gave the topic back, was revoked, lost the scope, stopped acknowledging) | `on`, and with `on: true` the viewer's `client` (fingerprint prefix) and `name` for the mark | `{state}`; error 3 after STOP until a person at the unit resumes or Vision is opened again |
| `vision.pull` | the viewer acknowledged the last picture (or none was sent yet) and the interval passed | `after_seq` | `{seq, w, h, age_ms, jpeg, boxes[]}` when a picture newer than `after_seq` is ready; `{seq}` alone when none is yet (the gateway asks again after the interval); error 3 when no preview is live |
| `vision.mode`, `vision.set`, `vision.reset` | a phone calls the method of the same name | as 5.1 | `{}` |

No `shell.*` method is called (ADR-019 rule 6): there is no upstream subscription. The gateway makes `vision.state` and `vision.stats` from its own `vision.status` answers and `vision.frame` from each `vision.pull` answer that carries a picture, for the one viewer only. The first draft of this section used `shell.subscribe`; the capture work showed that rule 6 does not allow it.

| On the phone's side | On the unit's side |
| --- | --- |
| method `vision.status` | method `vision.status` |
| method `vision.share` | method `vision.share` (plus who the viewer is) |
| method `vision.ack` | nothing: gateway only |
| event `vision.frame` | answer to method `vision.pull` |
| methods `vision.mode`, `vision.set`, `vision.reset` | the same |
| events `vision.state`, `vision.stats` | made by the gateway from `vision.status` |

Not offered, in any version without a new decision: FACE, RECOGNIZE and READ (neither chosen nor shared; `vision.share` is refused and a live preview ends when the unit enters one), `enrol` and `forget`, anything of a face or an owner, the detector's choice, the camera's configuration, a still at full size, saving on the unit, lines by coordinates, opening Vision or the camera from the phone.

## 6. Bounds

These protect the unit's one hart and the link. None of them is a limit on how long or how often the owner may look.

| What | Bound |
| --- | --- |
| Viewers | 1 on the unit: the first authorized phone that asks; a second is told `busy` |
| Picture | longest side 640 at most (half the unit's picture by default), baseline JPEG, 64 KiB at most on the unit; the phone refuses more than 96 KiB or 640 px |
| Rate | 3 pictures a second by default, 5 at most; never faster than the phone acknowledges |
| Held | one picture in the helper's slot, one in the gateway, one on the phone |
| Duration | none: while Vision is in front and the viewer is connected and acknowledging |
| Requests | `vision.share`, `vision.mode`, `vision.set`, `vision.reset` together 20 a minute per client |
| Acknowledgement | no acknowledgement for 5 s ends the preview (a phone that went away without saying so) |
| Helper | encoding may take at most 10 % of the hart and lower the detector's rate by at most 10 %, measured (section 8); if it cannot, the default rate goes down, not the bound up |
| Memory | none added on the unit: slot 3 exists; libjpeg's working memory aside (ASSUMED under 200 KB) |

## 7. Security

The model is a trusted device: the unit and the phone belong to the same person, the phone was paired at the unit and given `vision` there. Nothing is asked again after that.

- **Encrypted and authenticated.** The session is the gateway's: TLS 1.3, both keys pinned (ADR-019). There is no other endpoint, port or protocol for pictures.
- **Authorized once, checked always.** The scope `vision` is given on the unit and can be taken back there, as can the pairing. It is checked on every request and before every picture; taking it away ends a preview at once.
- **The unit says so while it shares.** Vision's header carries a sharing mark with the phone's name on every one of its screens, and a STOP that ends the preview at once. STOP holds until a person at the unit resumes or Vision is opened again; the phone cannot start it again. The camera LED is lit as whenever the camera is open.
- **A preview ends** when: STOP is tapped; Vision is left (the helper, and with it the camera, is gone); the mode becomes FACE, RECOGNIZE or READ; the phone leaves its Vision screen, disconnects or is revoked; the scope is taken away; acknowledgements stop for 5 s.
- **Nothing is kept by Doors.** The unit writes no picture for a preview. The gateway holds one in memory and logs nothing of it. The app holds one in memory and writes none.
- **No biometrics.** No face picture, embedding, enrolment or recognition result is a field of any message.
- **No camera without the screen.** A phone cannot open Vision or keep it open; the preview lives inside the Vision screen's own life.

Accepted with the amendments: an authorized phone sees what the unit's camera sees whenever Vision is in front, with no act at the unit; whoever holds that phone unlocked, with the app, sees it too until the phone is revoked on the unit.

## 7a. The phone's screen (revised 2026-11-23)

| Path by which a picture could outlive the preview | What the app does |
| --- | --- |
| App storage, cache, logs, saved instance state, backup | Nothing: a picture exists as one `VisionFrame` and two bitmaps in memory, is not in any `rememberSaveable` or database, and the app has no backup (`allowBackup=false`) |
| Screenshot, screen recording, casting | **Allowed**, as for any other screen of the app. `FLAG_SECURE` is not set. The first draft set it while a unit's picture was shown; the owner's direction is that a picture of one's own camera on one's own phone may be captured like anything else |
| The recents thumbnail | May show the last picture, like any app. The picture itself is dropped when the screen stops |
| Lock screen | The activity is not shown over the lock screen |
| Notifications, widgets, share sheet, clipboard | None exist for Vision |

A "save" button in the app is not proposed: Android's own screenshot does that job without the app storing anything.

## 8. To be measured before any of it ships

Nothing below has been done.

1. `pos-vision bench` with a JPEG of the half-size picture: milliseconds per encode on the C908 at quality 40, 50, 60; bytes per picture for a real scene.
2. The detector's frame rate and the helper's processor share with a share at 3 and 5 pictures a second, in DETECT, TRAFFIC and SEGMENT.
3. The Vision app's handler time for `vision.pull` in the LVGL loop, and the shell's worst frame meanwhile.
4. gatewayd's processor share and resident memory while one viewer is served.
5. End to end on Wi-Fi: age of a picture on the phone, pictures a second, behaviour when the phone is slow (the rate falls, nothing queues).
6. Every way a preview ends, in section 7, each seen on a unit; STOP holding against a phone that asks again.
7. Closing Vision during a share: the helper still leaves cleanly and gives the CMA pool back (`docs/vision/KPU_CMA_LEAK.md`).

## 9. What it takes

- The app-method hook on `shell.sock`: exists (merged with the capture work, PRs #175 to #178). Vision answers larger than its 8192-byte limit (`vision.pull`) need the owner's separate approval, as decided for H1.
- `pos-vision`: the `share` commands, a memory destination for the codec, the half-size reduction.
- Vision app: `vision.*` handlers, the mark with STOP and resume, settings applied as if chosen on its own sheets. No sheet.
- gatewayd: the scope, topic, rows and filters (`gw_policy.[ch]`), the shell uplink that exists for `capture`, the status poll, the one-viewer pull loop, limits (`gw.h`), `tests/gatewayd_lint.sh` (which today forbids every `shell.` name), unit and end-to-end tests.
- Settings: one Vision switch per phone on the Companion page (granting a scope to a paired phone is not possible on a unit today: issue #172).
- `docs/api/gateway.md`, `docs/apps/VISION.md`, `docs/design/companion/THREAT_MODEL.md`.

No gatewayd code was written for this: a new scope, topic or method is exactly what ADR-019 says needs the owner first.
