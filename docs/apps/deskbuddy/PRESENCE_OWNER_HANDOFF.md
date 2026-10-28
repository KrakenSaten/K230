<!-- Copyright (c) 2026 PocketOS authors. SPDX-License-Identifier: Apache-2.0 -->
# DeskBuddy: presence freshness and the owner-aware Guard boundary

Investigation: 2026-10-26, base `origin/master` = `5011fac9`.
This is a bounded cloud investigation, not a K230 hardware diagnosis.
Related open issue: [#109](https://github.com/KrakenSaten/K230/issues/109).
No other open issue or open PR was returned by the repository queries.

## Delivered

- Absence requires a new empty detection report after the existing 1.5 s
  absence filter. Merely polling without results can no longer say NO_PERSON.
- A camera `stall` becomes UNAVAILABLE. Silence in detection reports also
  becomes UNAVAILABLE at the existing Vision 4 s liveness interval; this is
  a data-availability deadline, **not** a false-positive suppression threshold.
  Model loading retains the session's separate bounded loading deadline.
- Fresh reports recover from unavailable; the two-report confirmation starts
  again. A stopped helper is still abandoned/reaped using Vision's existing
  grace period. Missing models/camera/helper still report unavailable.
- The session queues events but retains just the newest DET/WHO snapshot.
  The bridge now consumes only its matching sequence, once: two queued event
  notifications cannot count the same latest snapshot as two observations.
- Losing vision preserves an open Guard visit. A fresh empty report ends it;
  recovery after a long outage does not create a second log/chime merely
  because the 15 s visit-merge interval elapsed. Actual departure and later
  return still create a new visit. Visits that occur wholly during an outage
  cannot be distinguished and are not invented.

No model, confidence threshold, motion rule, ignore zone, calibration, UI
layout, sound backend or persistence format is added. Stationary people remain
people. These fixes do **not** resolve the cause of #109.

## Current implementation and evidence

Source-level findings below are VERIFIED in the named code at the base.
Hardware results are historical DOCUMENTED evidence, not repeated here.

| Layer | What it actually provides |
| --- | --- |
| Camera (`core/pocketcam/pocketcam_v4l2.c`, `node_next`) | `f.seq = b.sequence`; `f.mono_ms` is host monotonic time at dequeue, not the driver's capture timestamp. The buffer is returned after processing. No check proves that new sequence numbers contain new sensor pixels. |
| Inference (`tools/vision/pos_vision.c`, `stream_once`, `detect`) | Runs on each successfully acquired frame. Timeout returns without inference and emits `stall`; malformed outputs return without a DET report and eventually fail. No reuse of an inference output on this timeout path was found. |
| Tracker (`core/pocketvision/vision_track.c`) | Tracks have IDs, `seen`, misses and confirmation. Confirmed tracks coast for at most 15 missed updates in NORMAL mode; they expire after that. Their boxes are predictions/smoothed boxes, not raw detections. |
| Report (`say_tracks`) | `det SEQ N ID:CLASS:CONF:BOX:DIR:SPEED...` includes confirmed coasting tracks. It does **not** carry `seen`, misses, capture time or per-track observation age. Thus a fresh DET report is not proof every listed track was detected in that frame. |
| Session (`apps/vision/vision_session.c`) | Copies the latest tracks/WHO and separately queues events. Any parsed activity refreshes its transport silence watchdog, including stalls/stats. That watchdog alone cannot distinguish live detections from a live but stalled helper. |
| Bridge (`apps/deskbuddy_vision/db_vision_pipeline.c`) | Whole-frame 64x64 preview, 100 ms polls; selects RECOGNIZE, else FACE, else DETECT. R0 person class is identified by its name. Detection-only has no identity evidence. Sequence filtering and stale handling are changed here. |
| Brain (`apps/deskbuddy/db_brain.c`) | Receives change events, not frames. Guard waits up to 4 s for identity, logs a person if unresolved, merges visits within 15 s. Its old unavailable transition ended visits; this delivery preserves them until observed absence. |
| Audio/persistence | Existing `guard_sound=0` default, one consumed alert per new visit, `pos-record play`, effective system volume/mute, one player without replay/queue, stopped on close. `db_store.c` writes prefs/Guard log through temp-file/fsync/rename. No biometric data is stored by DeskBuddy. |

### #109: what remains unresolved

The 510 reports over 39 seconds with an almost fixed box cannot be explained
by the NORMAL track's 15-missed-update retention **alone**, if those were new
successful updates with no detections. Continuous fresh false detections,
frozen/repeated camera pixels with advancing sequences, or other upstream
behaviour remain possible. A fixed box and score cannot distinguish these.
The two software defects above are independently reproducible, but neither
establishes what caused those Unit B reports. Do not close #109 on this PR.

PR #103's actual-person arrival/departure evidence and PR #106's speaker and
no-repeat-while-present evidence in [../DESKBUDDY.md](../DESKBUDDY.md) remain
accepted. The later sound runs did not demonstrate a second visit. No new
speaker test or broad hardware sweep is requested here.

## Owner recognition: exact stop boundary

Real implementation exists; `db_identity.h` is only an older unused interface.
The actual recognizer is `recognise()` in `pos-vision`, using RetinaFace's five
points, AI2D affine alignment to 112x112 and a normalized 512-value embedding.
Vision's ENROL averages five views of one face. `vision_owner_*` persists the
profile at `$POCKETOS_STATE_DIR/vision/owner.v1`, 0600 inside 0700; Vision's
confirmed FORGET deletes it. Reuse this, not a second DeskBuddy store.

Required external models (absent from this checkout; not shipped from 0.3.5):

| Runtime path | Documented origin and compatibility |
| --- | --- |
| `/usr/share/doors/vision/face_det.kmodel` | Vendor `face_detection_320.kmodel`, 584,576 B, RetinaFace/MobileNet 0.25, u8 `[1,3,320,320]`, nine f32 outputs listed in VISION.md. CanMV / SDK face_detect demo; historical unit B KPU test. Override `POCKETOS_VISION_FACE_DET`. |
| `/usr/share/doors/vision/face_embed.kmodel` | CanMV SDK `ai_poc/kmodel/face_recognition.kmodel`, 46,333,280 B, recorded abbreviated SHA-256 `2409a30f…78218472`, u8 `[1,3,112,112]` -> f32 `[1,512]`; historical unit B nncase/KPU test. Override `POCKETOS_VISION_FACE_EMBED`. |

The full artifact hashes must be recorded from the actual supplied files;
these abbreviated historical hashes are not verification pins. Source weights
and redistribution rights are unestablished (`MODEL_LICENSES.md`, EXTERNAL
ONLY; `docs/LICENSING.md` item 11). No model was fetched, converted or trained.
The compatible runtime is the pinned SDK nncase/AI2D stack already used by
`vision_kpu_nncase.cpp`, not a generic desktop inference runtime. The existing
code offers modes based on file readability and checks tensors when loading.
A live, locally enrolled owner and physical camera validation are still absent
from the documented recognition gate. Picture replay worked historically;
that is not live-face validation or secure authentication. Photographs can
match; there is no photograph/spoof resistance claim.

The current pipeline is also insufficient for the requested semantics:

- RECOGNIZE replaces person detection with face detection. A person whose
  face is hidden can disappear from its presence input.
- `who SEQ FACES N ID:SCORE:OWNER...` gives face count, track IDs and cached
  similarity, **not** the sequence/time of each successful embedding or its
  quality. `say_who()` retains scores while the tracker retains the ID, even
  when a current face is too small or an embedding attempt fails. The outer
  WHO sequence alone cannot prove two independent recognition results.
- The bridge prioritizes any owner over all other faces and otherwise any
  scored face over unscored faces. It cannot faithfully describe mixed groups.
- Legacy Guard stands down on OWNER_RECOGNIZED with `owner_recognition` on;
  there is no separate default-off auto-disarm preference. This pre-existing
  path is **not** completed or certified by this delivery. Do not enable it
  as the proposed owner-aware Guard feature. The brain also retains its
  previous identity across plain PERSON_DETECTED events.

Accordingly this PR stops before new identity/automatic-disarm implementation.
It does not add a dummy preference, pretend identity mode or alternate store.
R0 alone cannot identify owner, stranger or a waving hand.

### Concrete next implementation, once prerequisites are available

1. Keep current person observations alongside face detection/recognition in
   the same camera-owning helper. Associate faces with person tracks; report
   unassociated/obscured people as identity unknown. Export current observation
   sequence/time and `seen` separately from coasting display boxes.
2. Extend identity reports with per-track successful embedding sequence/time,
   observation association and quality validity. Cached scores, failed/too-small
   faces and unmatched person-face association cannot count as fresh matches.
   Distinguish owner, positively compared non-owner, and identity unknown.
   A low-quality/low-similarity face alone must not be called a stranger.
3. Require consecutive independent quality-valid owner results for the same
   current track over a measured stable interval; reset on stale data, unknown,
   failed inference, profile deletion/change or track change. Select the quality
   and stability parameters from the supplied models' live results, not R0
   confidence. Mock tests must explicitly exercise repeated cached scores.
4. Add `guard_auto_disarm=0`, persisted and default-off for old prefs too,
   exposed as a Guard-only switch with the recognition limitation stated.
   No system-lock API is called. Manual disarm remains available.
5. Multi-person rule: auto-disarm only with a stably recognized owner and no
   other current person/face whose identity is unknown or positively unmatched.
   Owner plus visitor stays armed and records one visitor visit, with existing
   optional sound; owner recognition never marks another person as known.
6. Preserve visit deduplication when identity refines from unknown to non-owner;
   no extra sound on refinement, owner greeting, history reload or recovery.
   Reuse local ENROL/FORGET, surface missing models/helper/camera truthfully,
   and never export/commit face images or embeddings. Test uncertain identity,
   mixed groups, cached/reordered reports, deletion and close/reopen. Render
   changed settings/status UI in the real simulator at all supported text sizes
   and both orientations, before the K230 check.

## Waving (later, separate)

R0 has no hand/pose keypoints and the face models have no wave output. A real
wave needs a compatible hand/upper-body landmark or temporal gesture model,
tracked hand/person association and a timestamped trajectory across multiple
frames (alternating lateral motion with a raised hand), with negatives such as
walking, head motion and camera movement. No suitable model is supplied here.
Use a bounded temporal window and cooldown, not a generic movement trigger.
Schedule it in the same helper: face detection, up to three embeddings per
250 ms round and hand/pose inference compete for KPU, AI2D, CMA and CPU. The
historical 37-39 ms per embedding is a K230 replay measurement, not a budget
for concurrent live recognition plus gestures. Measure that combination before
choosing a sampling rate. No wave implementation belongs in this delivery.

## Validation in this cloud checkout

Built the changed C code and real host `pos-vision` helper with the existing
native compiler; no SDK, toolchain, dependency or model installation.

| Focused check | Result |
| --- | --- |
| `db_pipeline_test tools/vision/pos-vision` | 73 checks, 0 failures: empty reports vs silence, stalls/recovery, missing model/helper, helper SIGKILL, stop twice/reap, queued DET/WHO snapshots and duplicate sequences. Camera/KPU/identities are fake, not real face recognition. |
| `db_brain_test` | 125 checks, 0 failures, including outage/recovery deduplication and observed departure/re-entry. |
| Existing `db_vision_test`, `db_guard_test`, `db_personality_test`, `db_sound_test` | 34 / 40 / 176 / 25 checks, 0 failures. |
| `tests/deskbuddy_lint.sh`, `git diff --check` | Pass. |
| Focused judge/queued-protocol harness plus brain, `-Wall -Wextra -Werror`, ASan + UBSan | 30 + 125 checks pass, no address/undefined-behaviour report. LeakSanitizer cannot run under this host's ptrace environment; rerun with `ASAN_OPTIONS=detect_leaks=0`. No leak-sanitizer claim. |
| Regression sensitivity | Rebuilding the focused harness against the original provider produces 10 failures; the original brain produces 3 failures with the updated brain tests. |

The existing helper-death test needed its `/proc` PID translated through
`NSpid` on this nested-PID-namespace host; this is test-only. One intermediate
full pipeline run also timed out waiting for the existing absence case; the
final full run passed. No detector threshold was adjusted for this.

No UI code, layout, label or setting changed. No new simulator render is
claimed: this checkout has no LVGL vendor tree, CMake or SDL/pkg-config setup.
Existing rendered layouts in `docs/apps/deskbuddy/` and PR #103/#106 are reused
as layout evidence, not as a render of these new transitions. The K230 shell
cross-build and physical transitions remain untested here: the pinned SDK and
sysroot are absent. Host fake-camera timings are not K230 performance.

## Consolidated local handoff (requires separate device authorization)

On the existing local pinned build setup, build this branch's shell; no SDK
upgrade. For #109, make **one short** capture of an occupied desk, a clearly
visible scene change, then an empty desk, with matching input-frame sequences,
local preview/input checksums and raw person detections versus track `seen`/
misses. Keep any images local and ephemeral. If sequences stop, follow the
capture path; if sequences advance but pixels do not change despite the scene
change, follow capture/ISP buffers; if pixels change and raw boxes persist,
inspect the detector/scene; if raw boxes disappear but tracks remain, compare
the misses and expiry. This distinguishes the remaining explanations without
repeating speaker tests or requiring prolonged leave/return sessions. A small
diagnostic-only helper build may be needed to correlate raw input with reports;
the current DET protocol does not expose all these fields.

Owner work is a subsequent task: supply the exact compatible external model
files with provenance/hashes and permitted local use, then implement the report
and Guard contract above before live enrollment/recognition checks. Nothing in
this cloud task accessed a unit, enrolled a face, transmitted radio, deployed,
merged or released anything.
