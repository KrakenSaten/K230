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
**Branch `feat/deskbuddy-camera-presence` (2026-10-21) lets BUDDY notice a
person through the camera, with the shipped R0 detector ("Camera presence"
under "The Vision provider"). Host-tested on the fake camera only.**

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
  a welcome. Only a **recognised** owner's return is logged (already
  acknowledged); DISARM adds no line, because pressing it says nothing about
  who pressed it (verified on unit B, 2026-10-22).
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
the host: 20 s idle with motion, 26-43 repaints and 43-62 timer runs over
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
| `$POCKETOS_STATE_DIR/deskbuddy/prefs.v1` | `companion`, `camera_presence`, `guard`, `night`, `owner_recognition`, `greeting`, `idle_animation` (0/1, default 1; a file without `camera_presence`, written before it, loads it as 1), `guard_sound` (0/1, default **0**; a file without it loads it as 0), `guard_armed` (0/1), `mode` |
| `$POCKETOS_RUNTIME_DIR/deskbuddy/visit.wav` | the visit chime (tmpfs), written from code when first needed ("Visit sound", below) |
| `$POCKETOS_STATE_DIR/deskbuddy/guard.v1` | `next=<id>` and one `e <id> <wall_s> <subject> <conf> <ack> <snapshot>` line per event, oldest first |

The directory is 0700 and the files 0600. Writes are atomic (temp file,
fsync, rename). A line or value this build could not have written is skipped
and the rest loads; a damaged file is left in place until there is something
to save. Changes are written from the shell's once-a-second tick and on
close. The eight switches are on the screen's SET panel, under them one
line: "Guard notices people, not who they are. Without Vision's face models
and an enrolled owner, every visit is SOMEONE - you too."

| Switch | Off means |
| --- | --- |
| Companion mode | BUDDY is not offered; with every mode off, a resting face that does not react |
| Camera presence · Buddy | BUDDY plays by touch alone and never starts the camera. On (the default), BUDDY runs the camera to notice somebody arrive and leave ("Camera presence", below). GUARD and NIGHT run the camera whatever it says: the switch cannot turn a guard blind |
| Desk guard | GUARD is not offered; an armed guard is disarmed |
| Visit sound · Guard | **Off by default.** Off: visits are shown and logged, never heard. On: an armed guard plays one short chime for each new visit it logs ("Visit sound", below) |
| Night mode | NIGHT is not offered |
| Owner recognition | the owner and strangers are both "a person": no greeting, guard logs `person` |
| Greeting | the owner still gets a happy face, without HELLO / WELCOME BACK |
| Idle animation | no blinks, glances or dozes (reduced motion, DS §12, also stops them) |

## Visit sound (Guard)

**Branch `feat/deskbuddy-guard-sound` (2026-10-22).** With "Visit sound ·
Guard" on, an armed guard plays one short chime (three falling notes, 0.36 s)
when it logs a **new** visit. What counts is the guard log, nothing new:

- One chime per new visit line (`db_brain_take_visit_alert`). A visitor
  who stays, a return within 15 s (the same visit), a visit upgraded from
  "someone" to "unknown" and the owner's acknowledged return are not new
  lines and make no sound. The log loaded at open is never played.
- Only while armed: a line written as the guard stands down (DISARM with
  somebody still unidentified) is not announced. BUDDY and NIGHT log no
  visits and never sound.
- No bursts or queue: every step takes what is new, heard or not. Several
  visits between two steps are one chime; a visit while a chime still plays,
  while muted or with no sound on the unit is logged and not played later.
- Reopening an armed guard is unchanged: somebody already at the desk is a
  visit (see the GUARD record below), and with the switch on it sounds.
- With the switch on, an armed guard says why a chime would not be heard:
  `VISIT SOUND MUTED` (the system volume is muted or 0) or `NO SOUND ON
  THIS UNIT` (no `pos-record`). The note never hides `NO VISION YET`.

**How it sounds (ADR-010 Amendment 2, accepted 2026-10-22).** The way RIFT plays its
message sounds: DeskBuddy writes `visit.wav` (48 kHz mono 16-bit, peak
under pos-record's 8192 ceiling) into its runtime directory and starts
`/usr/bin/pos-record play --volume-percent <effective system volume>
<file>` (`apps/deskbuddy/db_sound_helper.c`). The helper takes the one
audio lock, route, amplifier and ceiling; a sound while Wave, the Recorder
or RIFT holds the card is simply not heard. Nothing waits for it on the
LVGL thread; one at a time, never stacked; it is stopped when DeskBuddy
closes. A sound that will not start is a log line (`deskbuddy: the visit
sound did not start`), never a change in the guard. `tests/deskbuddy_lint.sh`
(section 9) holds the boundary.

**Presence is still not identity.** A chime means "a new visit was logged",
which with only R0 is "somebody was seen" - never "a stranger".

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
| `tests/db_brain_test.c` | every transition above; owner greeting and its repeat rule; strangers in guard mode, visits merged and upgraded; bounded log under 500 visits; the owner coming back with and without visitors, and by DISARM; the visit alert once per new visit line (not for merges, upgrades, history, stand-down, BUDDY or NIGHT; a burst is one); night presence and sleep, blind too; 10 000 mixed and 2 000 flickering events; malformed events; vision lost mid-visit; preferences and modes; idle behaviour and when a tick is (not) needed; every face shape fits its box |
| `tests/db_personality_test.c` | tap, stroke, flick, long press and a lost press told apart, a stroke reported once; a poke looks toward the touch, its variants and varied lengths; hey, the huff, no restart, back to calm, the cool-down; petting and its extension; the snack followed, near, eaten with 3-4 chews, put away, timed out, no hunger after hours; drowsy, asleep, stir, REST, WAKE, WAKE mid-rest; Guard and Night unchanged; vision wins over a reaction; GREET from any source invents no sighting; reduced motion; 20 000 random stimuli never leave a broken face or a stuck reaction; every expression and mouth fits; tweens |
| `tests/db_vision_test.c` | event validity, the bounded queue, the none provider, script parsing (every malformed form), playback, looping, a clock jump |
| `tests/db_guard_test.c` | the ring, text round trip, damaged and oversize files, preferences (the visit sound off by default, its key, a file from before it), the store's files, modes and atomic writes |
| `tests/db_sound_test.c` | the visit sound's seam (silent backend, volume 0 and above 100), the chime (short, silent lead-in, under pos-record's ceiling, ends at rest), its WAV, and the pos-record backend against a stand-in helper: started at once, refused while sounding, stopped, nothing left running |
| `tests/deskbuddy_lint.sh` | the vision boundary, the pure core, only the store does I/O, identity is declarations only, no colour or font, registration, timer deleted in destroy, dev path behind the variable; the visit sound only through pos-record from one file, asked for in one place, at the system volume, stopped in destroy, off by default |
| `tests/deskbuddy_app_test.c` | the screen follows the brain; ARM / DISARM / SEEN IT and the SET panel through a real pointer; the visit sound against a counting backend: off by default and saved, once per new visit at the system volume, none for a stay, a merge, a busy player (not replayed), muted (VISIT SOUND MUTED) or no backend (NO SOUND ON THIS UNIT), disarming, BUDDY or NIGHT, the log at reopen; a person at the desk on reopening sounds; stopped on close; what is saved and when; 25 open/close rounds mid-animation leave no timer, object or focus; a sleeping face does not repaint and wakes about once a second; the simulation path writes nothing; portrait and landscape fit; through the real pointer: a tap on an eye pokes toward it, taps on controls never poke, a stroke pets once however long, rapid taps huff and calm; FEED, drag to the mouth, a drag let go elsewhere goes home, closing mid-drag, a tap on the snack, GIVE, F / Enter / Esc / R, Back, timeout, mode change; drowsy and asleep by inactivity, a stroke wakes; `$DESKBUDDY_VISION=none` plays the same; a snack out across a rotation; no play starts a provider |
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

**Where it runs** (`sync_provider()` and `camera_wanted()`): Guard and
Night always start it; Companion only with **Camera presence** on (the
default) and Companion mode on. Opening DeskBuddy in BUDDY then starts the
helper at once; turning the switch off, or going to BUDDY with it off,
stops it (within Vision's grace, as closing the app does) and tells the
brain it cannot see, so Companion plays by touch alone, as it did before
the switch, and says nothing about the camera. While a freshly started
helper has not reported, ARM waits for "nobody" (`GUARD_ARMING`) rather
than taking the silence for an empty desk. The scripted mock and `none`
open no camera and run in every mode.

### Camera presence (BUDDY)

With the switch on, BUDDY reacts through its existing personality and the
brain's existing rules (`companion_vision()`), nothing new:

- **Somebody arrives** (nobody, or no vision, before): a sleeping, idle or
  drowsy face wakes (`WAKE`, then `RECOGNIZING` "..." while an identity
  might still come, then `IDLE`). A person seen does not wake it again
  while they stay; the judge says a conclusion once, after two reports in
  a row, so a flicker is no arrival.
- **Nobody sits there alone with it**: while somebody is in view it does
  not get drowsy or fall asleep; the quiet starts when they leave.
- **They leave**: the judge says "nobody" after 1.5 s without anybody. That
  is the presence filter, not a face change: a face still in `WAKE` or
  `RECOGNIZING` goes back to `IDLE` then, but a face already in `IDLE`
  (the usual case, `WAKE` + `RECOGNIZING` last 4.8 s) shows nothing new.
  What the departure starts is the quiet: `DROWSY` 60 s after "nobody"
  (`DB_DROWSY_MS`), asleep 2 min after that, unless touched.
- **Touch** plays as before alongside: poke, pet, feed and REST / WAKE are
  stimuli, the camera does not change them; a vision state (a greeting,
  `WAKE`) is not interrupted by one, as before.

**Presence is not identity.** A person found by the object detector is
`PERSON_DETECTED` - "somebody is there" - and only that: no greeting, no
stranger's look, no guard entry naming anyone. `OWNER_RECOGNIZED` and
`UNKNOWN_PERSON` come only from Vision's RECOGNIZE, with the face models on
the unit **and** an owner enrolled in Vision; without them there is no
identity evidence, and none is claimed. Owner recognition off turns even
those into "somebody", as before.

**Models.** Person presence needs only the object detector the image
carries: R0 (`/usr/share/doors/vision/det-r0-traffic6-yolox-tiny-416.kmodel`,
docs/apps/VISION.md "The model"). The provider names it to the helper,
picked as Vision picks it (`vision_store_detector_files()`,
`vision_detector_pick()`: Vision's last confirmed detector when its file is
there, else the other, so a bench unit with UPSTREAM chosen in Vision gets
UPSTREAM). A person is the detector class whose name is `person`
(`vision_label()`, the names TRAFFIC maps by; COCO row 0 in both R0 and
UPSTREAM), never an index assumed; a car or a bicycle is nobody. The face
models (`face_det.kmodel`, `face_embed.kmodel`) are optional: present, the
helper runs FACE or RECOGNIZE instead, as before.

**When it cannot see.** No helper, no camera (or Camera holding it), no
detector and no face model, or a helper that died: the provider says
`UNAVAILABLE` once and BUDDY shows **CAMERA UNAVAILABLE** under the face
(only once the source has answered, not while it is starting) and plays by
touch. That is never taken for "nobody here": the brain's `seen` is
UNAVAILABLE, not NOBODY. GUARD keeps its own NO VISION YET.

**The hold.** The helper is told how the unit is held - the shell's
`hold_rotation` (app.h), as Camera and Vision use it: the display's
rotation on the panel, the mode's and keyboard's on an external display -
and the app passes a new hold on while DeskBuddy is open (on HDMI the
shell does not restart when it changes). Before this the provider took the
display's rotation, which on an external display is always 0.

**Lifetime.** Inference is in the helper process (ADR-006); the UI thread
only polls, every 100 ms. Closing DeskBuddy stops the helper (Vision's
grace: it gives back the camera and the KPU), so Camera and Vision open
normally afterwards. Nothing watches after DeskBuddy closes.

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
stop twice, no helper left behind; also under ASan/UBSan; and the person
class by name, the detector pick, a person from R0 with no
`yolov8n.kmodel` on the unit, a car that is nobody, a turn while running).
`tests/deskbuddy_app_test.c` covers Camera presence on and off, the
switch's scope over GUARD and NIGHT, a file from before it, CAMERA
UNAVAILABLE, the hold followed on an external display and - with the
built helper on the fake camera and fake detector - a person arriving and
leaving in BUDDY with touch alongside and no identity claimed. All of that
is host evidence on fake frames, not a real camera.

**Unit B, 2026-09-30** (recorded pictures through the real shell, KPU): the
owner enrolled in Vision from the vendor's selfie, then DeskBuddy on the ID
photo of the same man said HELLO, on a stranger HM. WHO'S THIS?, and on the
live camera in a dark room nothing (vision available, nobody there - not
NO VISION YET); after closing no helper was left and Camera opened. The
helper took 5-18 % CPU (live camera, RECOGNIZE); DeskBuddy's shell RSS was
unchanged. Not tested: a live person at the desk, and Guard mode's log with
real vision.

**Doors 0.3.6 to 0.3.7 and the R0 detector** (checked in the code, not on
hardware): the provider started `pos-vision` without `--model`, so its
DETECT fallback looked for the helper's old default, `yolov8n.kmodel`,
which no image ships since 0.3.5, and a card without face models reported
NO VISION YET in GUARD and NIGHT. **Fixed on
`feat/deskbuddy-camera-presence`:** the provider names the detector Vision
would open (R0 on every image from 0.3.6), as described under "Camera
presence" above. Run on unit B on 2026-10-22, below.

**Unit B, 2026-10-22: Camera presence with the real camera** (temporary
test shell; nothing installed, restored afterwards).

*Setup (device evidence).* `pocketos-shell` built from `1d2df9b`
(`Doors 0.3.7 build 1d2df9bb`, SHA-256 `99cfb7c2…ea910`) with the pinned
toolchain (BSP `bb831ab`, SDK `22d02c6`, Xuantie GCC 14.1.1) against the
sysroot that built the unit's installed shell (its `liblvgl` `4f761c44…`).
The unit: Doors 0.3.6 image (`9ad64a1`) with the owner-installed Neon shell
and QR `liblvgl.so.9.5.0` `2c4fd81b…`; `pos-vision` `b741bffc…` (0.3.6);
`det-r0-traffic6-yolox-tiny-416.kmodel`; no face models. All the shell's
undefined symbols resolve against the unit's own libraries. The test shell
ran on the installed `liblvgl`, with the installed shell stopped through its
init script; settings, DeskBuddy's `prefs.v1`, Vision's `settings.v1` and the
absence of `guard.v1` were backed up by hash and restored byte-identical.

*What ran (device evidence).* DeskBuddy in BUDDY, Camera presence on (the
unit's `prefs.v1` predates the switch and loaded it as on). DeskBuddy
started `pos-vision session --backend v4l2 --model
/usr/share/doors/vision/det-r0-traffic6-yolox-tiny-416.kmodel`, which held
`/dev/video2`. With no face model on the unit this is DETECT: R0 person
detection, **not** face recognition; no identity was claimed. The helper's
reports to the shell (`det` lines, traced read-only on its socket) carried
COCO class 0 (`person`) tracks at 0.65-0.93. The panel was read with
`kmsgrab` about every 2 s. No `CAMERA UNAVAILABLE` note at any time: the
camera was available, and the app did not claim otherwise.

*Timeline (device evidence, UTC; helper reports to 0.1 s, panel to ~2 s).*

| Time | Helper reports | Panel |
| --- | --- | --- |
| 07:09:57-07:11:13 | one person (the owner at the desk) | idle with glances; never drowsy although untouched for over 60 s: somebody present holds the quiet off |
| 07:11:13.85 | 0 people | |
| 07:11:27.44 | a person; confirmed track 11 at 07:11:27.5 | 07:11:28 `WAKE` (eyes wide); 07:11:30-07:11:31 `RECOGNIZING` ("..."); 07:11:33 `IDLE` - the brain's 0.8 s + 4 s |
| 07:11:44.58 | last person report, then 0 people | `IDLE`, no change (as designed, above) |
| 07:12:46 | 0 people | first `DROWSY` frame (heavy lids); last report + 1.5 s filter + 60 s = 07:12:46.1 |
| 07:13:08.92 | a person (the owner coming back to reply) | 07:13:09 `WAKE`, 07:13:11-07:13:13 "..." |

Both directions reached the brain through the filter: the arrival woke a
face left alone (an arrival only counts after "nobody", so the first
departure at 07:11:13 was registered too), and the departure started the
quiet at the filtered moment, within the capture interval.

*Closing (device evidence).* `shell.home` at 07:14:13; the helper was gone
at the first check (07:14:14). Camera then opened normally: `pos-camera
session --backend v4l2` held `/dev/video2` and showed the live picture;
after leaving Camera no camera helper remained. `/var/lib/pocketos/camera`
unchanged. meshcored untouched (same process throughout); no radio use.

*Owner observations.* The owner left the view, came back for about 15 s,
then left and stayed out about 90 s without touching the unit, as asked.
The owner reported the actions; the reactions above are read from the
panel captures and the helper reports, not from the owner's view of the
panel.

*Host simulation (earlier, not repeated).* The judge, provider and app tests
on the fake camera and fake detector listed under "Tests" above.

*Not tested on the unit:* NIGHT with the real camera, `CAMERA UNAVAILABLE`
with the camera actually missing (host only), a turn while running, more
than one person. GUARD: the next record. The "..." after an arrival with
only the detector is confirmed on the unit: 4 s, as the brain says.

Captures: `deskbuddy/unitb-presence-wake.png`,
`deskbuddy/unitb-presence-recognizing.png` and
`deskbuddy/unitb-presence-drowsy.png` (the panel through `kmsgrab` at
07:11:28, 07:11:30 and 07:12:46). No camera picture is kept.

**Unit B, 2026-10-22: GUARD with the real camera** (the same temporary shell,
`build=1d2df9bb`, SHA-256 `99cfb7c2…`, and the same kit; restored afterwards).

*What GUARD does without face models (code, unchanged by this branch).*
The detector only says "somebody". With Owner recognition on (the unit's
setting), an armed guard that sees a person shows `GUARD_PERSON_DETECTED`
("SOMEONE'S HERE") and waits 4 s (`DB_GUARD_IDENTIFY_MS`) for an identity
that cannot come, then logs the visit as `person` ("SOMEONE" on screen,
never "UNKNOWN" or a stranger) and shows `GUARD_UNKNOWN` ("VISITOR
NOTED"). Leaving goes back to `GUARD_ARMED` ("WATCHING THE DESK"). One
visit is one line; a return within 15 s is the same visit. The owner
cannot stand the guard down by being seen: only DISARM, which shows "N
VISITS WHILE YOU WERE AWAY" until SEEN IT. DeskBuddy had **no sound** then
(the optional visit sound came later: "Visit sound", above), and no
cooldown beyond the visit merge.

*How it was driven.* `shell.tap` exists only in shells built with
`POCKETOS_SHELL_TEST_HOOKS`, so the device shell cannot press ARM over
IPC. DeskBuddy was closed, its own `prefs.v1` (backed up) was set to
`mode=guard`, `guard_armed=1`, and DeskBuddy was opened again: the normal
"re-armed on reopen" path. Nobody touched the unit.

*Device evidence (UTC; helper reports to 0.1 s, panel about every 2 s).*
The helper announced `caps detect track …` (no face or recognize mode)
and reported COCO class 0 tracks.

| Time | Helper reports | Panel / `guard.v1` |
| --- | --- | --- |
| 07:43:07 | the owner at the desk | reopened armed: `GUARD_ARMED` at once, then "SOMEONE'S HERE" |
| 07:43:11 | | "VISITOR NOTED", `07:43 SOMEONE NEW`; `guard.v1` written (`e 1 … person -1 0 0`) |
| 07:43:13-07:44:10 | the owner stays | no further write (file hash unchanged) |
| 07:44:10.54 | 0 people | 07:44:13 "WATCHING THE DESK" |
| 07:44:38.00 | a person (the owner back after 27.5 s) | 07:44:40-42 "SOMEONE'S HERE"; 07:44:44 "VISITOR NOTED", second line `07:44 SOMEONE NEW`, one write |
| 07:44:44-07:44:58 | the owner stays | no write |
| 07:44:58.34-07:45:08.10 | 0 people for 9.8 s, then a person | "WATCHING THE DESK", then "SOMEONE'S HERE" and "VISITOR NOTED" again, merged into the same visit: no new line, no write |
| 07:45:59 | DeskBuddy closed | helper gone at the first check (07:46:00) |

The log holds `person` with confidence -1 (none): the detector path
reports presence, not a score. No identity was claimed anywhere.

*Owner observations.* The owner left the view for about 30 s, came back
and stayed about 20 s, without touching the unit; the reactions above are
read from the captures, the helper reports and `guard.v1`.

*Host tests (earlier, not repeated).* `tests/db_brain_test.c` covers every
Guard transition, merges and the bounded log; `tests/db_guard_test.c` the
file.

*Restore.* `RESTORE: OK`: installed shell supervised; settings, `prefs.v1`
and Vision `settings.v1` byte-identical; the test's `guard.v1` removed (it
was absent before; a copy is kept in the kit's `used/`); no camera helper;
meshcored the same process throughout.

*Limitations found (existing behaviour, not changed by this branch):*

- **Reopening an armed guard logs whoever opened it.** `enter_mode()`
  re-arms straight to `GUARD_ARMED`; only the ARM button waits for
  "nobody" (`GUARD_ARMING`, also while the camera is still starting). The
  person at the desk is logged as a visit at once (07:43:11 above). On a
  unit with an enrolled owner it would stand down instead; without one it
  is a false visit.
- **DISARM writes no return line.** `db_brain_disarm()` stands down with
  `owner_seen = false`, so only a recognised owner adds the acknowledged
  return line. (The rules above said otherwise; corrected on
  `feat/deskbuddy-guard-sound`.)
- **A short return re-shows "SOMEONE'S HERE"** for 4 s even when it merges
  into the visit already logged; the log is right, the status repeats.

Captures: `deskbuddy/unitb-guard-watching.png` (07:44:13) and
`deskbuddy/unitb-guard-visitor-noted.png` (07:44:44, two visits).

**Unit B, 2026-10-22: the visit sound** (`feat/deskbuddy-guard-sound`
`319ec18b`; temporary shell, restored afterwards).

*Setup (device evidence).* `pocketos-shell` `Doors 0.3.7 build 319ec18b`,
SHA-256 `5469c1f2…0fccbe4`, the same pinned toolchain and sysroot as the
camera checks above; every undefined symbol resolves against unit B's own
libraries (`liblvgl` `2c4fd81b…`). `pos-record info`: playback validated,
peak ceiling 8192. Backed up by hash and restored byte-identical:
settings.conf (it holds the volume and mute; no volume key, so the default
100 %), DeskBuddy `prefs.v1`, Vision `settings.v1`; `guard.v1` and
`/run/pocketos/deskbuddy` were absent and were removed again. DeskBuddy's
`prefs.v1` was set to `mode=guard`, `guard_armed=1`, `guard_sound=1` and
DeskBuddy opened (no tap hooks on a device build).

*Device evidence (UTC).* The shell's process calls and the helper's reports
were traced read-only for the whole run (08:32:27-08:37:24); the panel and
`pidof pos-record` were sampled about every 2 s.

| Time | Evidence |
| --- | --- |
| 08:32:28.74 | DeskBuddy opened armed; `pos-vision` started on R0 |
| 08:32:29.6 onwards | the helper reported 1-3 person tracks in **every** report to the end (never 0) |
| 08:32:33.73 | `pos-record` started by the shell (4 s identify wait after the first sighting); `visit.wav` written in `/run/pocketos/deskbuddy` |
| 08:32:34.46 | `pos-record` reaped, exit status 0: the chime played to the end |
| 08:32:35-08:37:24 | one `guard.v1` line (`person -1 0 0`), "VISITOR NOTED" throughout; no second `pos-record` start in the trace, none in 154 process samples |
| 08:38:20 | DeskBuddy closed: no `pos-vision` or `pos-record` left at the first check |

*Owner observations.* The owner heard one short chime when DeskBuddy
opened. The owner then left the view for about 30 s, came back and stayed
about 20 s, and reported exactly one more chime after coming back and
nothing while staying.

*What the evidence supports.* The chime at open (a person already at the
desk, logged as a visit) was played by DeskBuddy through `pos-record` and
heard. Continued presence for five minutes made no second sound and no
second line. The departure was **not** seen: at least one other person
track stayed in view the whole time, so the visit never ended and no new
visit was logged. The second chime the owner heard was not started by this
shell (no process, no log line); its source is not established. A new
visit after a real departure is therefore covered by the host tests only
(`db_brain_test`, `deskbuddy_app_test`), not on the unit.
