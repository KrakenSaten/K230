# Companion: Vision preview (proposal)

Status: **Proposed. Nothing here is implemented on a unit, and nothing here is approved.** It needs two decisions of the owner first: `docs/decisions/ADR-006-amendment-1-companion-preview.md` and `docs/decisions/ADR-019-amendment-1-vision.md`. The Android side exists against a simulated source (`android/doors-companion`, README "Vision"); its gateway client is written to section 5 and has never met a gateway.
Date: 2026-11-22

Evidence classes as in the hardware baseline: VERIFIED (seen on a unit), DOCUMENTED (in this repository's code or documents), ASSUMED (reasoned, not measured).

## 1. What is wanted

A paired phone shows what the unit's Vision app is looking at: the picture, the boxes with class, confidence and track id, the mode, the detector's figures. With a second permission it chooses a mode and the settings Vision's own sheets offer. It does this without a second opener of the camera, without a camera service, and without a picture leaving the unit unless a person at the unit allowed it for that session.

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
| **The RGB565 picture the helper already makes for the shell** | **Proposed.** Already turned, mirrored and fitted; its boxes are already in its pixels; the phone sees what the unit's screen shows and nothing else, which is also what a person can meaningfully consent to |

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
gatewayd:          one viewer; calls vision.pull when the viewer sent vision.ack; passes the answer on as the event vision.frame
phone:             one picture held; decodes off the main thread; draws picture and boxes with one transform
```

New helper commands: `share on <max_side> <quality>`, `share next`, `share off`. New event: `share slot seq w h len`. Protocol version 4; the app and the helper ship together.

## 5. Gateway API (proposed addition to `docs/api/gateway.md`)

There are two surfaces, and they do not have the same names for a picture. **5.1 is what a phone sees**; it is what `GatewayVision` in the Android app is written to. **5.2 is what gatewayd calls on the shell's socket**; a phone never sees it. A picture is *pulled* upstream (`vision.pull`, a request with an answer) and *pushed* to the phone (`vision.frame`, an event), and the phone's `vision.ack` is what allows the next pull. `vision.ack` is answered by gatewayd itself and never reaches the shell; `vision.pull` is not a method a phone can call (code 1, like any method outside the allowlist).

### 5.1 The phone's side

Capability `vision`. Topic `vision`. Scopes `vision_view` and `vision_control` (which requires `vision_view`).

| Method | Scope | Params | Result |
| --- | --- | --- | --- |
| `vision.status` | view | - | `available` (false and nothing else unless Vision is the app in front), `owner` (`vision`, `camera`, `none`), `mode`, `modes[]` (what the helper can run), `settings{}`, `model`, `labels[]` (the detector's class names), `pic{w,h}`, `stats{fps_x10, infer_ms, pre_ms, post_ms, cpu_pct, rss_kb, dropped, count_ab, count_ba}`, `share` (`off`, `asking`, `live`, `refused`, `ended`), `share_left_s` |
| `vision.share` `{on}` | view | | `{state}`. `on: true` raises the sheet on the unit and answers `asking`; the outcome arrives as `vision.state`. Error 3: Vision is not in front, the mode is one that is never shared, the unit is locked. Error 5: another phone is being shown the picture. Error 10: more than one request in 30 s |
| `vision.ack` `{seq}` | view | the `seq` of the newest `vision.frame` received | `{}`. Answered by the gateway alone: it marks the viewer ready, and the gateway then pulls the next picture (5.2) no sooner than the interval allows. An unknown or old `seq` is ignored, not an error. The first picture of a share needs no acknowledgement |
| `vision.mode` `{mode}` | control | one of `detect track traffic segment instances color edge trace` that is in `modes[]` | `{}` |
| `vision.set` `{key, value}` | control | `track.line`, `traffic.range`, `traffic.line`, `traffic.speed`, `color.tol`, `edge.hard`, `trace.dark`, `seg.view`, each with the values of `docs/apps/VISION.md` "Stored settings" | `{}` |
| `vision.reset` | control | - | `{}`; TRACK's and TRAFFIC's counters |

Events (topic `vision`): `vision.state {}` on any change of mode, owner, sharing or settings (the client asks `vision.status`); `vision.stats {…}` once a second while Vision runs; `vision.frame {seq, w, h, age_ms, jpeg, boxes[[id, cls, conf, x, y, w, h, dir, kmh10], …]}` only while sharing is `live`, `jpeg` being base64 of a baseline JPEG.

### 5.2 The unit's side (gatewayd to the Vision app, on `shell.sock`)

Served by the Vision app while it is in front, through the app-method hook (`SHELL_APP_METHODS.md`); "not served" when another app is in front, which the gateway passes on as `available: false`.

| Upstream method | Called when | Params | Answer |
| --- | --- | --- | --- |
| `vision.status` | a phone calls `vision.status`; after a `vision.state` event | - | as 5.1, without `share_left_s` (the gateway's own clock) |
| `vision.share` | a phone calls `vision.share`; and with `on: false` whenever the gateway ends a share itself (the viewer left, was revoked, lost the scope, stopped acknowledging, ten minutes passed) | `on`, and with `on: true` the viewer's `client` (fingerprint prefix) and `name` for the sheet | `{state}` |
| `vision.pull` | the viewer acknowledged the last picture (or none was sent yet) and the interval passed | `after_seq` | `{seq, w, h, age_ms, jpeg, boxes[]}` when a picture newer than `after_seq` is ready; `{seq}` alone when none is yet (the gateway asks again after the interval); error 3 when no share is live |
| `vision.mode`, `vision.set`, `vision.reset` | a phone calls the method of the same name | as 5.1 | `{}` |

Upstream events (the shell's existing `shell.subscribe`): `vision.state {}` and `vision.stats {…}`, passed on to the phones holding the topic. The gateway makes `vision.frame` itself from each `vision.pull` answer that carries a picture, for the one viewer only.

| On the phone's side | On the unit's side |
| --- | --- |
| method `vision.status` | method `vision.status` |
| method `vision.share` | method `vision.share` (plus who is asking) |
| method `vision.ack` | nothing: gateway only |
| event `vision.frame` | answer to method `vision.pull` |
| methods `vision.mode`, `vision.set`, `vision.reset` | the same |
| events `vision.state`, `vision.stats` | the same, as shell events |

Not offered, in any version without a new decision: FACE, RECOGNIZE and READ (neither chosen nor shared; `vision.share` is refused and a live share ends when the unit enters one), `enrol` and `forget`, anything of a face or an owner, the detector's choice, the camera's configuration, a still at full size, saving on the unit, lines by coordinates, opening Vision or the camera from the phone.

## 6. Bounds

| What | Bound |
| --- | --- |
| Viewers | 1 on the unit, whichever paired client was allowed |
| Picture | longest side 640 at most (half the unit's picture by default), baseline JPEG, 64 KiB at most on the unit; the phone refuses more than 96 KiB or 640 px |
| Rate | 3 pictures a second by default, 5 at most; never faster than the phone acknowledges |
| Held | one picture in the helper's slot, one in the gateway, one on the phone |
| A share | ends after 10 minutes and must be allowed again on the unit; no "always allow" |
| Requests | one sheet per 30 s per client; the sheet closes after 30 s as a refusal |
| Acknowledgement | no acknowledgement for 5 s ends the share |
| Helper | encoding may take at most 10 % of the hart and lower the detector's rate by at most 10 %, measured (section 8); if it cannot, the default rate goes down, not the bound up |
| Memory | none added on the unit: slot 3 exists; libjpeg's working memory aside (ASSUMED under 200 KB) |

## 7. Security

- The session is the gateway's: TLS 1.3, both keys pinned, scopes checked on every request (ADR-019). There is no other endpoint, port or protocol for pictures.
- **A person at the unit allows each share.** The sheet names the phone, says that the picture on this screen will be shown on it, and offers ALLOW and REFUSE; no answer is a refusal. `vision_view` alone shares nothing.
- **The unit says so while it shares.** Vision's header carries a sharing mark with the phone's name on every one of its screens, and a STOP that ends the share at once. The camera LED is lit as whenever the camera is open.
- **A share ends** when: STOP is tapped; Vision is left (the helper, and with it the camera, is gone); the mode becomes FACE, RECOGNIZE or READ; the phone leaves its Vision screen, disconnects or is revoked; `vision_view` is taken away; acknowledgements stop; ten minutes passed.
- **Nothing is kept.** The unit writes no picture for a share. The gateway holds one in memory and logs nothing of it. The phone holds one in memory, writes none, and lets it go when the share ends.
- **No biometrics.** No face picture, embedding, enrolment or recognition result is a field of any message.
- **No camera without the screen.** A phone cannot open Vision or keep it open; the share lives inside the Vision screen's own life.

Accepted if this is approved: a paired phone whose owner was allowed a share sees what the unit's camera sees for up to ten minutes; anyone who can tap ALLOW on an unlocked unit can grant that.

## 7a. The phone's screen (reviewed 2026-11-22)

What the Android app does with a picture today, and what is recommended.

| Path by which a picture could outlive the share | Today | Recommended |
| --- | --- | --- |
| App storage, cache, logs, saved instance state, backup | Nothing: a picture exists as one `VisionFrame` and two bitmaps in memory, is not in any `rememberSaveable` or database, and the app has no backup (`allowBackup=false`) | Keep. A lint test should hold that `ui/Vision*.kt` and `core/*Vision*.kt` write no file |
| Screenshot, screen recording, casting, the assistant's screen read | Allowed by Android unless the window says otherwise | **Blocked while a unit's picture is shown**: `FLAG_SECURE` on the window for exactly that time. Implemented in `ui/Vision.kt`; it has never run with a real picture, because none exists |
| The recents thumbnail | Would show the last picture | Covered by the same flag (the thumbnail is blank while it is set); and the picture is dropped when the screen stops, which is already so |
| The simulated viewfinder | Capturable | Keep capturable: it is drawn, and screenshots of it are how the screen is reviewed |
| Lock screen | The activity is not shown over the lock screen | Keep |
| Notifications, widgets, share sheet, clipboard | None exist for Vision | Add none |
| A second device photographing the phone | Cannot be prevented | Say nothing that suggests otherwise |

Recommendation for the owner: keep the flag unconditional for real pictures in v1, with no switch. A person who wants a still should take it on the unit, where the Camera app writes to the unit's own storage under the unit's own rules; a "save" on the phone would be a new decision against "nothing is kept" in section 7. Not recommended: setting the flag for the whole app (it would block screenshots of RIFT and of status pages, which are the owner's own data and useful in bug reports).

Open: whether the capture figures of PacketShark deserve the same flag. They name no host; the recommendation is no.

## 8. To be measured before any of it ships

Nothing below has been done.

1. `pos-vision bench` with a JPEG of the half-size picture: milliseconds per encode on the C908 at quality 40, 50, 60; bytes per picture for a real scene.
2. The detector's frame rate and the helper's processor share with a share at 3 and 5 pictures a second, in DETECT, TRAFFIC and SEGMENT.
3. The Vision app's handler time for `vision.pull` in the LVGL loop, and the shell's worst frame meanwhile.
4. gatewayd's processor share and resident memory while one viewer is served.
5. End to end on Wi-Fi: age of a picture on the phone, pictures a second, behaviour when the phone is slow (the rate falls, nothing queues).
6. Every way a share ends, in section 7, each seen on a unit.
7. Closing Vision during a share: the helper still leaves cleanly and gives the CMA pool back (`docs/vision/KPU_CMA_LEAK.md`).

## 9. What it takes

- The app-method hook on `shell.sock` (shared with ADR-016 amendment 1; not designed).
- `pos-vision`: the `share` commands, a memory destination for the codec, the half-size reduction.
- Vision app: `vision.*` handlers, the sheet, the mark and STOP, settings applied as if chosen on its own sheets.
- gatewayd: scopes, topic, rows and filters (`gw_policy.[ch]`), a shell uplink, the one-viewer pull loop, limits (`gw.h`), `tests/gatewayd_lint.sh` (which today forbids every `shell.` name), unit and end-to-end tests.
- Settings: the two scopes on the Companion page.
- `docs/api/gateway.md`, `docs/apps/VISION.md`, `docs/design/companion/THREAT_MODEL.md`.

No gatewayd code was written for this: a new scope, topic or method is exactly what ADR-019 says needs the owner first.
