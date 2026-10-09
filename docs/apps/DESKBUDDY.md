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

Launcher: the Apps folder, first (DS §47; it was in WORKSPACE after Calculator), in the `ai` hue, a first-party icon (a
small screen with two eyes). Place and icon are for the owner to confirm.
Fullscreen (`POCKETOS_CHROME_NONE`, DS §36): no status cluster over the face
or the night clock. No DS amendment has been written for it yet.

## Modes and states

One state enum, `db_state` (`apps/deskbuddy/db_brain.h`). A state belongs to
exactly one mode; nothing is a combination of booleans. What vision last said
is one enum too, `db_seen`: UNAVAILABLE, NOBODY, PERSON, OWNER, UNKNOWN.

| Mode | State | Face | Leaves on |
| --- | --- | --- | --- |
| Companion | `SLEEP` | closed, a slow breath | a person, a touch, WAKE |
| | `IDLE` | open, blinks and glances, dozes when alone; plays (Personality, below) | nobody and no touch for 1 min: `DROWSY` |
| | `DROWSY` | heavy lids, slow blinks, a nod, now and then a yawn | a person or a touch: awake; 2 min of quiet in all: `SLEEP` |
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

## Personality (Companion)

**Branch `feat/deskbuddy-personality-opus` (from `origin/master` `8c5235b`,
2026-10-09). Host-tested only; not run on a K230.** Captures, the manual
check sequence and what is real: [deskbuddy/README.md](deskbuddy/README.md).
What could come next for Guard, Night, sound and focus:
[deskbuddy/PERSONALITY_NEXT.md](deskbuddy/PERSONALITY_NEXT.md).

DeskBuddy plays by touch alone: calm, curious, a little mischievous. Nothing
here needs a camera or a model, and nothing here starts, polls or names a
vision provider (`tests/deskbuddy_lint.sh`, section 8).

**One door in.** Whatever happens to it reaches the brain as a
`db_stimulus` (`db_brain_stimulus`, `db_brain.h`): `POKE`, `PET`, `SNACK`,
`SNACK_GONE`, `FEED`, `GREET`, `REST`, `WAKE`, with a source (touch, key,
vision) and, for the ones with a place, where it happened from the middle
of the eye line in per-mille of an eye box. The kind is what it means, not
how it was sensed: a provider that one day sees a wave would send `GREET`
and get the same reaction a tap gets today. Nothing sends `GREET` yet, and
a stimulus never changes what vision last said (`seen`).

**Reactions** play over Companion's calm states (`IDLE`, `DROWSY`) as a few
beats - an expression, a mouth, where the eyes look, a lean, a shake or a
hop - each held a few hundred ms, every length varied by a few per cent:

| Reaction | Beats | Variants |
| --- | --- | --- |
| poke | startled "o" leaning away, then a curious look at where it was touched | a blink-and-look; rarely a wink and a smirk |
| hey | poked again within 3 s: a squint and a flat mouth | |
| annoyed | 4 pokes within 3 s: a glare, a quick shake, then a squint and a slow blink back to calm (~2.4 s) | for 6 s after, pokes are only a blink, never another huff |
| pet | ^ ^ and a smile, a small hop toward the stroke, then soft lids | a happy wiggle |
| eat | three or four chews with ^ ^, a smile and a hop, soft lids | a wink to finish |
| stir | woken: lids lift, a blink, eyes open with a hop toward the touch | |
| yawn / rest | a big "o" under heavy lids; REST then closes the eyes into `SLEEP` | |
| greet | ^ ^ and a smile with a hop (for a future seen wave) | |

A vision state (a greeting, a stranger, `WAKE`) ends a reaction and is not
interrupted by one: vision's faces win. The face is eyes alone when nothing
is happening, as before; the mouth appears only in a reaction or when a
snack is at its mouth.

**Gestures** on the face area (`db_gesture.h`, pure): a tap is down and up
within 350 ms and 16 px; a stroke is 110 px of path, 50 px from the start,
over at least 220 ms and no faster than 1.5 px/ms on average. A stroke is
reported once, while the finger still moves, and never again in the same
gesture; a flick, a long press or a lost press is nothing. The buttons are
siblings of the face, so a tap on one never pokes.

**The snack.** FEED puts a biscuit in a lower corner of the face, clear of
the character, and the button becomes GIVE. The eyes follow it; near the
mouth they open wide with the mouth. Let go there and it is eaten; let go
anywhere else, or lose the press, and it floats back to its place. A tap on
it, GIVE or Enter float it to the mouth. Esc, Back, another mode, REST or
20 s untouched put it away quietly. There is no hunger, no counter that
matters, no reminder: it is optional fun.

**Rest and wake.** A minute alone: `DROWSY`; two: `SLEEP` (both counted from
the last presence or touch, as before). Any touch wakes it with a stir. REST
yawns it to sleep at once, WAKE (the same button, relabelled) wakes it. The
shell's screen-off, lock and alarms are not touched.

**Keys** (Companion): F is FEED / GIVE, Enter gives a snack that is out, R is
REST / WAKE, Esc closes the settings or puts the snack away.

**Motion.** In Companion a new face is tweened from the one drawn (160 ms,
a blink 70 ms; Guard and Night step from face to face as before) on a
33 ms timer that runs only while something moves; a breath (the
character rises 1.4 % of an eye box every 2 s, every 3.2 s asleep), a hop
and a shake are single steps. Reduced motion (DS §12) keeps the reactions as
end states: no tween, lean, hop, shake, breath or chewing. The idle
animation switch also stops breathing and the spontaneous yawn. Measured on
the host: 20 s idle with motion, 26-34 repaints and 43-53 timer runs over
several runs.

**Saved.** Nothing new: a reaction, the snack and drowsiness are moments,
not preferences. Closing mid-drag or mid-reaction leaves nothing behind;
reopening starts calm, with no snack out.

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
| `tests/db_personality_test.c` | tap, stroke, flick, long press and a lost press told apart, a stroke reported once; a poke looks toward the touch, its variants and varied lengths; hey, the huff, no restart, back to calm, the cool-down; petting and its extension; the snack followed, near, eaten with 3-4 chews, put away, timed out, no hunger after hours; drowsy, asleep, stir, REST, WAKE, WAKE mid-rest; Guard and Night unchanged; vision wins over a reaction; GREET from any source invents no sighting; reduced motion; 20 000 random stimuli never leave a broken face or a stuck reaction; every expression and mouth fits; tweens |
| `tests/db_vision_test.c` | event validity, the bounded queue, the none provider, script parsing (every malformed form), playback, looping, a clock jump |
| `tests/db_guard_test.c` | the ring, text round trip, damaged and oversize files, preferences, the store's files, modes and atomic writes |
| `tests/deskbuddy_lint.sh` | the vision boundary, the pure core, only the store does I/O, identity is declarations only, no colour or font, registration, timer deleted in destroy, dev path behind the variable |
| `tests/deskbuddy_app_test.c` | the screen follows the brain; ARM / DISARM / SEEN IT and the SET panel through a real pointer; what is saved and when; 25 open/close rounds mid-animation leave no timer, object or focus; a sleeping face does not repaint and wakes about once a second; the simulation path writes nothing; portrait and landscape fit; through the real pointer: a tap on an eye pokes toward it, taps on controls never poke, a stroke pets once however long, rapid taps huff and calm; FEED, drag to the mouth, a drag let go elsewhere goes home, closing mid-drag, a tap on the snack, GIVE, F / Enter / Esc / R, Back, timeout, mode change; drowsy and asleep by inactivity, a stroke wakes; `$DESKBUDDY_VISION=none` plays the same; a snack out across a rotation; no play starts a provider |
| `tests/deskbuddy_shell_test.sh` | runs the app test; the launcher holds 24 apps; the real shell opens and closes DeskBuddy, every mode on simulated vision in both orientations, nothing written, damaged files open |

```
make CC=gcc CFLAGS="-O2 -Wall -Wextra -Werror" deskbuddy-test
SHELL_BIN=~/work/.../pocketos-shell bash tests/deskbuddy_shell_test.sh
```

The app test doubles as the demonstration capture: with
`$DESKBUDDY_DEMO_DIR` set it runs no checks, plays one scripted session
through the real app and pointer and writes a frame every 70 ms;
`tools/design/deskbuddy_demo.py` makes animated PNGs of them
([deskbuddy/README.md](deskbuddy/README.md)).

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
section 7). `choose_provider()` picks it unless `$DESKBUDDY_SIM` is set (the
mock) or `$DESKBUDDY_VISION` is `none` (blind, as v0.1 was).

**Only Guard and Night start it** (`sync_provider()`, since the
personality slice). Companion plays by touch and never starts the helper:
opening DeskBuddy in Companion opens no camera, switching to GUARD or NIGHT
starts the helper, and switching back to BUDDY stops it (within Vision's
grace, as closing the app does) and tells the brain it cannot see, so
Companion runs blind and its NO VISION YET note is not shown. While a
freshly started helper has not reported, ARM waits for "nobody"
(`GUARD_ARMING`) rather than taking the silence for an empty desk. The
scripted mock and `none` open no camera and run in every mode. So the
owner greeting and the stranger's wary look in Companion need a source
other than the camera from now on; an explicit opt-in for them is left to
a later slice.

- `start()` starts Vision's helper (`pos-vision`, ADR-006) with no picture
  on screen - a 64 x 64 preview it takes and drops - non-blocking; a helper
  that cannot start is `UNAVAILABLE`, once. The preview is the whole
  camera frame letterboxed (`view ... contain`), not cut to the square: a
  person at the edge of the 640 x 360 frame is in the tracks like one in
  the middle.
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
- `stop()` ends the helper within Vision's own grace
  (`VISION_LEAVE_GRACE_MS`, 3 s, longer while it is still opening a model)
  and is safe twice; a helper that said it cannot go on gets the same time
  to close; nothing calls back into the screen.

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

**Doors 0.3.6 and the R0 detector** (checked in the code, not on hardware):
the provider starts `pos-vision` without `--model`, so its DETECT fallback
opens the helper's default, `yolov8n.kmodel`, which no image ships since
0.3.5. The R0 detector that 0.3.6 carries (docs/apps/VISION.md, "The model")
is opened only by the Vision app, by name, so it changes nothing here: a
fresh 0.3.6 card without face models reports NO VISION YET, as 0.3.5 does.
A unit given `yolov8n.kmodel` or the face models by hand behaves as before.
