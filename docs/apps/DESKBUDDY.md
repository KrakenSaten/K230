# DeskBuddy

A small companion for the desk: a pair of eyes that wake when somebody
arrives, are glad to see the owner and wary of a stranger; a desk guard that
notes who came by while the owner was away; a calm night clock that still
blinks. Cute, observant, a little mischievous - not a chatbot.

**Status: v0.1, branch `feat/deskbuddy` (from `origin/master` `b3fd2a6`,
2026-09-29). Not merged. Host-tested only; never run on a K230. No vision:
real person and owner recognition is deferred to the Vision work in progress
elsewhere, and DeskBuddy runs blind or on simulated vision until then.**
**Branch `feat/vision-next` (2026-09-30) adds the real provider on Vision's
helper and a first run on unit B: "The Vision provider" at the end.**

Launcher: WORKSPACE after Calculator, in the `ai` hue, a first-party icon (a
small screen with two eyes). Place and icon are for the owner to confirm.
Fullscreen (`POCKETOS_CHROME_NONE`, DS §36): no status cluster over the face
or the night clock. No DS amendment has been written for it yet.

## Modes and states

One state enum, `db_state` (`apps/deskbuddy/db_brain.h`). A state belongs to
exactly one mode; nothing is a combination of booleans. What vision last said
is one enum too, `db_seen`: UNAVAILABLE, NOBODY, PERSON, OWNER, UNKNOWN.

| Mode | State | Face | Leaves on |
| --- | --- | --- | --- |
| Companion | `SLEEP` | closed | a person, a touch |
| | `IDLE` | open, blinks and glances, dozes when alone | nobody for 2 min: `SLEEP` |
| | `WAKE` | wide | 0.8 s: `RECOGNIZING` (someone there, recognition on) or `IDLE` |
| | `RECOGNIZING` | curious, "..." | owner, stranger, 4 s, or they leave |
| | `OWNER_GREETING` | happy ^ ^, "HELLO" | 3.5 s |
| | `UNKNOWN_REACTION` | wary ಠ ಠ, "HM. WHO'S THIS?" | 4 s |
| Guard | `GUARD_DISARMED` | open | ARM; the owner seen: `GUARD_OWNER` |
| | `GUARD_ARMING` | open | armed, waiting for the owner to leave: nobody seen, `GUARD_ARMED` |
| | `GUARD_ARMED` | open, looks about | a person, a stranger, the owner |
| | `GUARD_PERSON_DETECTED` | wide | identified, 4 s (logged as a person), or they leave (logged) |
| | `GUARD_UNKNOWN` | wary, "VISITOR NOTED" | they leave: `GUARD_ARMED`; the owner: stand down |
| | `GUARD_OWNER` | happy, "WELCOME BACK" | 3.5 s: `GUARD_DISARMED` |
| | `GUARD_ALERT_PENDING` | curious, "N VISITS WHILE YOU WERE AWAY" | SEEN IT |
| Night | `NIGHT_IDLE` | half-lidded, dim, slow blinks; the time, large | somebody or a touch; 5 min quiet: `NIGHT_SLEEP` |
| | `NIGHT_PRESENCE` | open, dim | 5 s |
| | `NIGHT_SLEEP` | closed, dim | somebody or a touch |

`GUARD_ARMING` is the one state added to the suggested layout: arming with
the owner in front of the device would otherwise read their own face as
"the owner is back" and disarm at once.

Rules that keep repeated or rapid events cheap and calm:

- An arrival is a change from nobody to somebody; a stream of PERSON events
  does not restart anything.
- The owner is greeted again only after being unseen for 60 s
  (`DB_OWNER_AWAY_MS`); a stranger gets the wary look again only after 30 s.
- One guard visit is one log line, however many events it makes. A visitor
  who flickers out and back within 15 s (`DB_VISIT_MERGE_MS`) is the same
  visit; a person first logged as "someone" and identified later in the same
  visit is upgraded, not added. The log is written only when a line changes.
- The owner coming back while armed (recognised, or DISARM pressed) stands
  the guard down: `GUARD_ALERT_PENDING` if a visitor is unacknowledged, else
  a welcome. The return itself is logged, already acknowledged.
- Leaving Guard mode by hand disarms (whoever changes the mode is at the
  desk). There is no authentication in v0.1: anyone at the desk can disarm.
- Invalid events (kind out of range, confidence outside 0..1000) are
  ignored.

## The vision boundary

`apps/deskbuddy/db_vision.h`. DeskBuddy includes nothing of `apps/vision`,
`core/pocketvision` or `core/pocketcam`, and they include nothing of it
(`tests/deskbuddy_lint.sh`).

```c
enum db_vision_kind { DB_VISION_NO_PERSON, DB_VISION_PERSON_DETECTED, DB_VISION_OWNER_RECOGNIZED,
                      DB_VISION_UNKNOWN_PERSON, DB_VISION_UNAVAILABLE };

struct db_vision_event {
    enum db_vision_kind kind;
    bool face;              /* a face was found */
    int16_t confidence_pm;  /* 0..1000, or DB_CONF_NONE */
    int64_t mono_ms;        /* when it was seen */
    int64_t wall_s;         /* wall clock, 0 when not set */
};

struct db_vision_provider_ops {
    const char *name;
    int (*start)(void *ctx, int64_t now_ms, struct db_vision_queue *q);
    int64_t (*poll)(void *ctx, int64_t now_ms, struct db_vision_queue *q); /* next poll time, or -1 */
    void (*stop)(void *ctx);
};
```

- Events are **conclusions**, not detections: "somebody", "the owner",
  "somebody else", "nobody", "I cannot see". Boxes, faces, embeddings and
  thresholds stay in the provider.
- Providers report **changes**; the brain holds the last report.
- Every provider call runs on the LVGL thread and **returns at once**. A
  provider that does real work does it in its own process or thread (the
  Vision helper's shape, ADR-006) and hands over what is ready.
- The queue between them is **fixed** (16). A full queue drops the oldest
  event (the newest is the truth about the room) and counts it; an invalid
  event is refused and counted.
- `db_vision_none_ops` is the provider of v0.1: it pushes
  `DB_VISION_UNAVAILABLE` once and never asks to be polled.

### Owner recognition (interfaces only)

`apps/deskbuddy/db_identity.h` declares the two-stage pipeline a real
provider will run - person/face detection, then a face embedding compared by
cosine similarity with a locally enrolled owner profile - as
`db_face_embedding`, `db_owner_profile`, `db_identity_match` and
`db_recognizer_ops`. **Nothing implements it**, nothing calls it, and no
embedding reaches the brain, the store or the screen (lint). Privacy terms,
binding on the implementation: the profile is biometric data and stays on the
device (`$POCKETOS_STATE_DIR/deskbuddy/owner.v1`, 0600 in a 0700 directory),
never over IPC, the network, a log line or a crash report; one "forget me"
deletes it whole; no enrolment image is kept once embedded. The embedding
model, its input and dimension are open.

## Guard log

`apps/deskbuddy/db_guard.h`: a ring of 32 events; the newest always fits and
the oldest goes. An event is an id, a wall-clock time (0 when the board's
clock was not set - it is then shown as `--:--`, never as 1970), a subject
(`unknown`, `owner`, `person`), an optional confidence and an acknowledged
flag. **No picture.** `snapshot_id` is reserved (always 0) for a later,
opt-in snapshot store, so the file format need not change for it.

## Files and settings

App-owned storage, the pattern RIFT and the games use; `settings.conf` is the
shell's and DeskBuddy does not write it. There is no second settings
framework: one struct (`db_prefs`), one parser, one formatter.

| File | Holds |
| --- | --- |
| `$POCKETOS_STATE_DIR/deskbuddy/prefs.v1` | `companion`, `guard`, `night`, `owner_recognition`, `greeting`, `idle_animation` (0/1, default 1), `guard_armed` (0/1), `mode` |
| `$POCKETOS_STATE_DIR/deskbuddy/guard.v1` | `next=<id>` and one `e <id> <wall_s> <subject> <conf> <ack> <snapshot>` line per event, oldest first |

The directory is 0700 and the files 0600. Writes are atomic (temp file,
fsync, rename). A line or value this build could not have written is skipped
and the rest loads; a damaged file is left in place until there is something
to save. Changes are written from the shell's once-a-second tick and on
close. The six switches are on the screen's SET panel.

| Switch | Off means |
| --- | --- |
| Companion mode | BUDDY is not offered; with every mode off, a resting face that does not react |
| Desk guard | GUARD is not offered; an armed guard is disarmed |
| Night mode | NIGHT is not offered |
| Owner recognition | the owner and strangers are both "a person": no greeting, guard logs `person` |
| Greeting | the owner still gets a happy face, without HELLO / WELCOME BACK |
| Idle animation | no blinks, glances or dozes (reduced motion, DS §12, also stops them) |

## Screen, time and lifetime

- The eyes are filled objects in role styles (DS §4; `style_lint.sh`): an
  accent "white", a pupil and two cut-outs in the background colour - a lid
  (sleepy, wary) and a disc from below that leaves an arch (happy). Night
  dims them by opacity. Shapes come from `db_face.c` as numbers and are
  restyled only when they change; there is no tweening.
- One `lv_timer`, created in `create()`, deleted in `destroy()`. Its period
  is set after every run to when the brain or provider next needs it (a
  blink, a timeout, a scripted event), between 20 ms and 1 s. Asleep with no
  vision it wakes once a second and repaints nothing.
- `destroy()` stops the provider, saves, deletes the timer, then deletes the
  app's own root, so no callback can arrive after the app is freed.
- Layout is done from the timer, never inside LVGL's layout pass (a size
  change only sets a flag). Portrait stacks face, text and controls;
  landscape puts the controls in a 400 px column beside the face.
- The v0.1 lifecycle limitation applies (app.h): **the guard watches only
  while DeskBuddy is open**. It is re-armed on reopen from `guard_armed`.

## Test and demo controls

Nothing of this is on the production screen. With `$DESKBUDDY_SIM` set the
app uses the scripted provider (`db_vision_mock.h`), **writes nothing** to the
store, and shows `SIMULATED VISION`:

```
DESKBUDDY_SIM="100:person 900:owner@930 1500:none 1700:unknown@800 2200:none"
DESKBUDDY_SIM="0:person 1000:owner 4000:none loop"     # repeats
DESKBUDDY_SIM=keys                                     # nothing scripted, keys only
DESKBUDDY_MODE=companion|guard|night|armed             # start there (simulation only)
```

A step is `<ms>:<kind>[@<confidence>]`, kinds `none person owner unknown
unavailable`; a malformed script is refused whole and the app runs blind. In
a simulation the keys `0`-`4` inject none, person, owner, unknown and
unavailable. Scenarios:

| Scenario | Script |
| --- | --- |
| no person | `0:none` |
| person detected | `0:person` |
| owner recognised | `0:owner@950` |
| unknown person | `0:unknown@800` |
| owner leaves, returns | `0:owner 3000:none 65000:owner` (greets again after 60 s away) |
| guard event | `DESKBUDDY_MODE=armed`, `500:unknown@820 3000:none 6000:owner` |
| night | `DESKBUDDY_MODE=night`, `2000:person 9000:none` |

```
SDL_VIDEODRIVER=... DESKBUDDY_SIM="..." DESKBUDDY_MODE=armed pocketos-shell --open deskbuddy
```

## Tests

| Suite | Proves |
| --- | --- |
| `tests/db_brain_test.c` | every transition above; owner greeting and its repeat rule; strangers in guard mode, visits merged and upgraded; bounded log under 500 visits; the owner coming back with and without visitors, and by DISARM; night presence and sleep, blind too; 10 000 mixed and 2 000 flickering events; malformed events; vision lost mid-visit; preferences and modes; idle behaviour and when a tick is (not) needed; every face shape fits its box |
| `tests/db_vision_test.c` | event validity, the bounded queue, the none provider, script parsing (every malformed form), playback, looping, a clock jump |
| `tests/db_guard_test.c` | the ring, text round trip, damaged and oversize files, preferences, the store's files, modes and atomic writes |
| `tests/deskbuddy_lint.sh` | the vision boundary, the pure core, only the store does I/O, identity is declarations only, no colour or font, registration, timer deleted in destroy, dev path behind the variable |
| `tests/deskbuddy_app_test.c` | the screen follows the brain; ARM / DISARM / SEEN IT and the SET panel through a real pointer; what is saved and when; 25 open/close rounds mid-animation leave no timer, object or focus; a sleeping face does not repaint and wakes about once a second; the simulation path writes nothing; portrait and landscape fit |
| `tests/deskbuddy_shell_test.sh` | runs the app test; the launcher holds 24 apps; the real shell opens and closes DeskBuddy, every mode on simulated vision in both orientations, nothing written, damaged files open |

```
make CC=gcc CFLAGS="-O2 -Wall -Wextra -Werror" deskbuddy-test
SHELL_BIN=~/work/.../pocketos-shell bash tests/deskbuddy_shell_test.sh
```

## Deferred

- ~~**Real vision**: a provider on the Vision pipeline (below), face
  detection, the embedding model, enrolment UI and "forget me".~~ Done on
  `feat/vision-next`: "The Vision provider" below.
- Optional guard snapshots (opt-in, local, bounded, deleted with the log).
- A shell-level background service, so the guard watches with the app
  closed or the screen locked (needs the lifecycle ADR-002 defers).
- A DS amendment for the screen, and the owner's confirmation of the place
  and icon; hardware gate on a unit (touch feel, night dimness, CPU).
- Authentication for DISARM; notification of visitors elsewhere in Doors.

## The Vision provider (feat/vision-next)

The plan that stood here is built, as `db_vision_pipeline_ops` in
`apps/deskbuddy_vision/db_vision_pipeline.c`: a directory of its own, the one
place DeskBuddy's boundary (`db_vision.h`) and Vision's helper client
(`apps/vision/vision_session.h`) meet, so DeskBuddy still includes nothing of
Vision and Vision knows nothing of DeskBuddy (`tests/deskbuddy_lint.sh`,
section 7). `start_provider()` picks it unless `$DESKBUDDY_SIM` is set (the
mock) or `$DESKBUDDY_VISION` is `none` (blind, as v0.1 was).

- `start()` starts Vision's helper (`pos-vision`, ADR-006) with no picture
  on screen - a 64 x 64 preview it takes and drops - non-blocking; a helper
  that cannot start is `UNAVAILABLE`, once.
- The helper runs the best mode the unit offers: RECOGNIZE (faces, and
  whether one is the owner), else FACE, else DETECT (the object detector's
  people). A face model that fails on the way drops to the next one down
  instead of going blind.
- `poll()` never waits; a small judge (`db_judge_*`, pure, tested) turns
  the helper's lines into **changes**: a conclusion is said once it has held
  for two reports, "nobody" after 1.5 s without anybody, the owner over a
  stranger when both are there, and no identity without an enrolled owner.
  `OWNER_RECOGNIZED` and `UNKNOWN_PERSON` carry the similarity (per-mille)
  as `confidence_pm`. The helper gone, the camera lost or taken, or no
  model at all: `UNAVAILABLE`, once. Polled every 100 ms while running.
- `stop()` ends the helper within Vision's own grace (1 s) and is safe
  twice; nothing calls back into the screen.

**The owner is Vision's.** Enrolment and FORGET are in Vision's RECOGNIZE
(docs/apps/VISION.md); the helper keeps the one owner at
`$POCKETOS_STATE_DIR/vision/owner.v1` (0600 in 0700) - not
`deskbuddy/owner.v1` as planned above - and DeskBuddy only ever receives
the conclusion. The privacy terms above hold: the profile never leaves the
helper and the unit, no enrolment image is kept, one FORGET deletes it.
`db_identity.h` stays declarations only: the helper is the recognizer.

**Tests:** `tests/db_pipeline_test.c` (the judge, and the provider end to end
against the real helper on the fake camera and fake face models: owner,
stranger, people without face models, nobody, a killed helper, no helper,
stop twice, no helper left behind; also under ASan/UBSan).

**Unit B, 2026-09-30** (recorded pictures through the real shell, KPU): the
owner enrolled in Vision from the vendor's selfie, then DeskBuddy on the ID
photo of the same man said HELLO, on a stranger HM. WHO'S THIS?, and on the
live camera in a dark room nothing (vision available, nobody there - not
NO VISION YET); after closing no helper was left and Camera opened. The
helper took 5-18 % CPU (live camera, RECOGNIZE); DeskBuddy's shell RSS was
unchanged. Not tested: a live person at the desk, and Guard mode's log with
real vision.
