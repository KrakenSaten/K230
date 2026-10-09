/*
 * DeskBuddy's state machine. See db_brain.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "db_brain.h"

#include <string.h>

static const char *const state_names[DB_STATE_COUNT] = {
    "SLEEP",          "IDLE",          "WAKE",           "RECOGNIZING",           "OWNER_GREETING",
    "UNKNOWN_REACTION", "DROWSY",      "GUARD_DISARMED", "GUARD_ARMING",       "GUARD_ARMED",
    "GUARD_PERSON_DETECTED", "GUARD_OWNER", "GUARD_UNKNOWN", "GUARD_ALERT_PENDING", "NIGHT_IDLE",
    "NIGHT_PRESENCE", "NIGHT_SLEEP",
};

/* ---- the reactions (db_brain.h, PERSONALITY) ------------------------------------ *
 *
 * Each is a few beats that grow out of each other: a startle settles into a
 * look, a huff into a squint and a slow blink, a chew into a smile and soft
 * lids. Where there are variants one is picked at random, and every beat's
 * length varies by a few per cent, so the same poke twice is not the same
 * film twice. */
#define BEAT(e, m, look, lean, shake, bob, ms) { DB_EXPR_##e, DB_MOUTH_##m, look, lean, shake, bob, ms }
#define CHEW BEAT(HAPPY, CHEW_OPEN, 0, 0, 0, 1, 170), BEAT(HAPPY, CHEW_SHUT, 0, 0, 0, 0, 190)

static const struct db_beat poke_startle[] = {
    BEAT(WIDE, O, 1, -1, 0, 0, 260),
    BEAT(CURIOUS, NONE, 1, 0, 0, 0, 950),
};
static const struct db_beat poke_blink[] = {
    BEAT(CLOSED, NONE, 1, -1, 0, 0, 150),
    BEAT(OPEN, NONE, 1, 0, 0, 0, 450),
    BEAT(CURIOUS, NONE, 1, 0, 0, 0, 550),
};
static const struct db_beat poke_wink[] = {
    BEAT(WIDE, O, 1, -1, 0, 0, 220),
    BEAT(OPEN, NONE, 1, 0, 0, 0, 420),
    BEAT(WINK, SMIRK, 1, 1, 0, 0, 750),
};
static const struct db_beat hey[] = {
    BEAT(WIDE, O, 1, -1, 0, 0, 160),
    BEAT(SQUINT, FLAT, 1, 0, 0, 0, 800),
};
static const struct db_beat annoyed[] = {
    BEAT(ANNOYED, FLAT, 0, 0, 1, 0, 110),
    BEAT(ANNOYED, FLAT, 0, 0, -1, 0, 110),
    BEAT(ANNOYED, FLAT, 0, 0, 1, 0, 110),
    BEAT(ANNOYED, FLAT, 0, 0, 0, 0, 1150),
    BEAT(SQUINT, NONE, 0, 0, 0, 0, 600), /* settling */
    BEAT(CLOSED, NONE, 0, 0, 0, 0, 240), /* a slow blink, and it is over */
};
static const struct db_beat pet[] = {
    BEAT(HAPPY, SMILE, 0, 1, 0, 1, 260),
    BEAT(HAPPY, SMILE, 0, 1, 0, 0, 1100),
    BEAT(CONTENT, SMILE, 0, 0, 0, 0, 900),
};
static const struct db_beat pet_wiggle[] = {
    BEAT(HAPPY, SMILE, 0, 0, 1, 1, 160),
    BEAT(HAPPY, SMILE, 0, 0, -1, 0, 160),
    BEAT(HAPPY, SMILE, 0, 0, 1, 0, 160),
    BEAT(HAPPY, SMILE, 0, 0, 0, 0, 900),
    BEAT(CONTENT, SMILE, 0, 0, 0, 0, 900),
};
/* Four chews; a shorter meal starts at the second. */
static const struct db_beat eat[] = {
    CHEW, CHEW, CHEW, CHEW,
    BEAT(HAPPY, SMILE, 0, 0, 0, 1, 900),
    BEAT(CONTENT, SMILE, 0, 0, 0, 0, 800),
};
static const struct db_beat eat_wink[] = {
    CHEW, CHEW, CHEW, CHEW,
    BEAT(HAPPY, SMILE, 0, 0, 0, 1, 700),
    BEAT(WINK, SMIRK, 0, 0, 0, 0, 800),
};
static const struct db_beat greet[] = {
    BEAT(HAPPY, SMILE, 0, 0, 0, 1, 300),
    BEAT(HAPPY, SMILE, 0, 0, 0, 0, 1200),
};
static const struct db_beat stir[] = {
    BEAT(SLEEPY, NONE, 0, 0, 0, 0, 500),
    BEAT(CLOSED, NONE, 0, 0, 0, 0, 170),
    BEAT(OPEN, NONE, 1, 0, 0, 1, 260),
    BEAT(OPEN, NONE, 1, 0, 0, 0, 500),
};
static const struct db_beat yawn[] = {
    BEAT(SLEEPY, O_BIG, 0, 0, 0, 1, 1100),
    BEAT(SLEEPY, NONE, 0, 0, 0, 0, 300),
};
static const struct db_beat rest[] = {
    BEAT(SLEEPY, O_BIG, 0, 0, 0, 1, 1100),
    BEAT(SLEEPY, NONE, 0, 0, 0, 0, 500),
    BEAT(CLOSED, NONE, 0, 0, 0, 0, 300),
};

#define N_BEATS(t) ((int)(sizeof(t) / sizeof((t)[0])))
#define CHEW_BEATS 2

/* How far a lean, a shake, a hop and a breath move the character, in
 * per-mille of the eye box: a few pixels. */
#define LEAN_PM 45
#define SHAKE_PM 30
#define BOB_PM 40
#define BREATH_PM 14
#define LONG_AGO (-((int64_t)1 << 40))

static const char *const react_names[DB_REACT_COUNT] = {
    "none", "poke", "hey", "annoyed", "pet", "eat", "greet", "stir", "yawn", "rest",
};

const char *db_react_name(enum db_react r)
{
    return ((int)r >= 0 && r < DB_REACT_COUNT) ? react_names[r] : "?";
}

const char *db_state_name(enum db_state s)
{
    return ((int)s >= 0 && s < DB_STATE_COUNT) ? state_names[s] : "?";
}

enum db_mode db_state_mode(enum db_state s)
{
    if (s >= DB_ST_NIGHT_IDLE) {
        return DB_MODE_NIGHT;
    }
    return s >= DB_ST_GUARD_DISARMED ? DB_MODE_GUARD : DB_MODE_COMPANION;
}

bool db_state_armed(enum db_state s)
{
    return s == DB_ST_GUARD_ARMING || s == DB_ST_GUARD_ARMED || s == DB_ST_GUARD_PERSON_DETECTED ||
           s == DB_ST_GUARD_UNKNOWN;
}

const char *db_seen_name(enum db_seen s)
{
    static const char *const names[] = { "unavailable", "nobody", "person", "owner", "unknown" };

    return ((int)s >= 0 && s <= DB_SEEN_UNKNOWN) ? names[s] : "?";
}

bool db_seen_present(enum db_seen s)
{
    return s == DB_SEEN_PERSON || s == DB_SEEN_OWNER || s == DB_SEEN_UNKNOWN;
}

/* ---- helpers ---------------------------------------------------------------- */

static uint32_t rnd(struct db_brain *b)
{
    /* xorshift32: plenty for when to blink. Never zero. */
    uint32_t x = b->rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    b->rng = x;
    return x;
}

static int64_t rnd_between(struct db_brain *b, int64_t lo, int64_t hi)
{
    return lo + (int64_t)(rnd(b) % (uint32_t)(hi - lo + 1));
}

static bool recognition(const struct db_brain *b)
{
    return b->prefs.on[DB_PREF_RECOGNITION];
}

/* Movement nobody asked for - blinks, glances, breathing, a yawn - and
 * never under reduced motion (DS §12: end states only) or with the
 * preference off. */
static bool motion_ok(const struct db_brain *b)
{
    return !b->reduced_motion && b->prefs.on[DB_PREF_IDLE_ANIMATION];
}

/* Companion's calm states, where the personality plays. */
static bool calm(const struct db_brain *b)
{
    return b->state == DB_ST_IDLE || b->state == DB_ST_DROWSY;
}

/* Idle behaviour runs in the calm states only, never over a reaction or a
 * snack (they have the eyes), and only when motion_ok. */
static bool acts_allowed(const struct db_brain *b)
{
    if (!motion_ok(b) || b->react != DB_REACT_NONE || b->snack) {
        return false;
    }
    switch (b->state) {
    case DB_ST_IDLE:
    case DB_ST_DROWSY:
    case DB_ST_GUARD_DISARMED:
    case DB_ST_GUARD_ARMING:
    case DB_ST_GUARD_ARMED:
    case DB_ST_NIGHT_IDLE:
        return true;
    default:
        return false;
    }
}

static void schedule_act(struct db_brain *b, int64_t now)
{
    b->act = DB_ACT_NONE;
    b->act_until_ms = DB_NEVER;
    if (!acts_allowed(b)) {
        b->next_act_ms = DB_NEVER;
    } else if (b->state == DB_ST_NIGHT_IDLE) {
        b->next_act_ms = now + rnd_between(b, DB_NIGHT_ACT_GAP_MIN_MS, DB_NIGHT_ACT_GAP_MAX_MS);
    } else if (b->state == DB_ST_DROWSY) {
        b->next_act_ms = now + rnd_between(b, DB_DROWSY_GAP_MIN_MS, DB_DROWSY_GAP_MAX_MS);
    } else {
        b->next_act_ms = now + rnd_between(b, DB_ACT_GAP_MIN_MS, DB_ACT_GAP_MAX_MS);
    }
}

static void start_act(struct db_brain *b, int64_t now)
{
    uint32_t pick = rnd(b) % 100;

    if (b->state == DB_ST_NIGHT_IDLE) {
        b->act = DB_ACT_BLINK;
        b->act_until_ms = now + DB_NIGHT_BLINK_MS;
    } else if (b->state == DB_ST_DROWSY) {
        /* Slow blinks, and now and then a nod: the lids drop and lift. */
        b->act = pick < 65 ? DB_ACT_BLINK : DB_ACT_DOZE;
        b->act_until_ms = now + (pick < 65 ? DB_DROWSY_BLINK_MS : DB_NOD_MS);
    } else if (pick < 60) {
        b->act = DB_ACT_BLINK;
        b->act_until_ms = now + DB_BLINK_MS;
    } else if (pick < 90 || db_seen_present(b->seen) || b->state != DB_ST_IDLE) {
        /* A doze only in Companion with nobody there; otherwise look about. */
        b->act = (pick & 1) ? DB_ACT_GLANCE_LEFT : DB_ACT_GLANCE_RIGHT;
        b->act_until_ms = now + DB_GLANCE_MS;
    } else {
        b->act = DB_ACT_DOZE;
        b->act_until_ms = now + DB_DOZE_MS;
    }
    b->next_act_ms = DB_NEVER;
}

static int64_t timeout_of(enum db_state s)
{
    switch (s) {
    case DB_ST_WAKE:
        return DB_WAKE_MS;
    case DB_ST_RECOGNIZING:
        return DB_RECOGNIZE_MS;
    case DB_ST_OWNER_GREETING:
        return DB_GREETING_MS;
    case DB_ST_UNKNOWN_REACTION:
        return DB_UNKNOWN_REACTION_MS;
    case DB_ST_GUARD_PERSON_DETECTED:
        return DB_GUARD_IDENTIFY_MS;
    case DB_ST_GUARD_OWNER:
        return DB_GUARD_OWNER_MS;
    case DB_ST_NIGHT_PRESENCE:
        return DB_NIGHT_PRESENCE_MS;
    default:
        return 0;
    }
}

static void set_armed(struct db_brain *b, bool armed)
{
    if (b->prefs.guard_armed != armed) {
        b->prefs.guard_armed = armed;
        b->prefs_rev++;
    }
}

/* Breathing: in Companion's quiet states, while nothing else moves it. */
static void schedule_breath(struct db_brain *b, int64_t now)
{
    bool quiet = calm(b) || b->state == DB_ST_SLEEP;

    if (!motion_ok(b) || !quiet || !b->prefs.on[DB_PREF_COMPANION]) {
        b->breath = false;
        b->breath_ms = DB_NEVER;
    } else if (b->breath_ms == DB_NEVER || b->breath_ms < now) {
        b->breath_ms = now + (b->state == DB_ST_SLEEP ? DB_SLEEP_BREATH_MS : DB_BREATH_MS);
    }
}

static void stop_react(struct db_brain *b)
{
    b->react = DB_REACT_NONE;
    b->beats = NULL;
    b->beat = 0;
    b->beat_count = 0;
    b->beat_until_ms = DB_NEVER;
}

static void enter(struct db_brain *b, enum db_state s, int64_t now)
{
    int64_t t = timeout_of(s);

    b->state = s;
    b->since_ms = now;
    b->deadline_ms = t ? now + t : DB_NEVER;
    if (db_state_mode(s) == DB_MODE_GUARD) {
        set_armed(b, db_state_armed(s));
    }
    /* A reaction belongs to the calm state it plays over: a greeting, a
     * stranger or sleep ends it. The snack stays while Companion does. */
    if (!calm(b)) {
        stop_react(b);
    }
    if (db_state_mode(s) != DB_MODE_COMPANION) {
        b->snack = false;
        b->snack_near = false;
    }
    schedule_act(b, now);
    schedule_breath(b, now);
}

/* ---- playing a reaction ------------------------------------------------------- */

static int64_t beat_len(struct db_brain *b, const struct db_beat *beat)
{
    /* A few per cent either way, so no two are the same length. */
    return (int64_t)beat->ms * (int64_t)rnd_between(b, 920, 1080) / 1000;
}

static void start_beats(struct db_brain *b, enum db_react r, const struct db_beat *beats, int first, int count,
                        int64_t now)
{
    b->react = r;
    b->beats = beats;
    b->beat = first;
    b->beat_count = count;
    b->beat_until_ms = now + beat_len(b, &beats[first]);
    b->act = DB_ACT_NONE;
    b->act_until_ms = DB_NEVER;
    b->next_act_ms = DB_NEVER;
}

/* Start reaction r, picking its variant. */
static void play(struct db_brain *b, enum db_react r, int64_t now)
{
    uint32_t pick = rnd(b) % 100;

    switch (r) {
    case DB_REACT_POKE:
        if (pick < 55) {
            start_beats(b, r, poke_startle, 0, N_BEATS(poke_startle), now);
        } else if (pick < 80) {
            start_beats(b, r, poke_blink, 0, N_BEATS(poke_blink), now);
        } else {
            start_beats(b, r, poke_wink, 0, N_BEATS(poke_wink), now);
        }
        break;
    case DB_REACT_HEY:
        start_beats(b, r, hey, 0, N_BEATS(hey), now);
        break;
    case DB_REACT_ANNOYED:
        start_beats(b, r, annoyed, 0, N_BEATS(annoyed), now);
        break;
    case DB_REACT_PET:
        if (pick < 70) {
            start_beats(b, r, pet, 0, N_BEATS(pet), now);
        } else {
            start_beats(b, r, pet_wiggle, 0, N_BEATS(pet_wiggle), now);
        }
        break;
    case DB_REACT_EAT:
        /* Three or four chews; now and then a wink to finish. */
        start_beats(b, r, pick < 25 ? eat_wink : eat, (pick & 1) ? CHEW_BEATS : 0, N_BEATS(eat), now);
        break;
    case DB_REACT_GREET:
        start_beats(b, r, greet, 0, N_BEATS(greet), now);
        break;
    case DB_REACT_STIR:
        start_beats(b, r, stir, 0, N_BEATS(stir), now);
        break;
    case DB_REACT_YAWN:
        start_beats(b, r, yawn, 0, N_BEATS(yawn), now);
        break;
    case DB_REACT_REST:
        start_beats(b, r, rest, 0, N_BEATS(rest), now);
        break;
    default:
        stop_react(b);
        break;
    }
}

/* The last beat is over. Rest ends in sleep; the rest back to calm. */
static void end_react(struct db_brain *b, int64_t now)
{
    enum db_react was = b->react;

    stop_react(b);
    if (was == DB_REACT_REST) {
        enter(b, DB_ST_SLEEP, now);
        return;
    }
    schedule_act(b, now);
}

static void advance_react(struct db_brain *b, int64_t now)
{
    if (b->beat + 1 < b->beat_count) {
        b->beat++;
        b->beat_until_ms = now + beat_len(b, &b->beats[b->beat]);
    } else {
        end_react(b, now);
    }
}

/* ---- the guard log -------------------------------------------------------------- */

static int64_t event_wall(const struct db_brain *b, const struct db_vision_event *ev)
{
    return (ev && ev->wall_s > 0) ? ev->wall_s : b->wall_s;
}

/* One line per visit. A visitor still here, or back within the merge
 * window, updates the line they already have: a stranger identified after
 * being logged as a person is upgraded, and the best confidence is kept. */
static void log_visitor(struct db_brain *b, enum db_subject subject, int16_t conf, int64_t wall, int64_t now)
{
    struct db_guard_event *e = db_guard_find(b->log, b->visit_id);

    bool same_visit = e && e->subject != DB_SUBJECT_OWNER &&
                      (b->visit_end_ms == DB_NEVER || now - b->visit_end_ms < DB_VISIT_MERGE_MS);

    b->visit_end_ms = DB_NEVER;
    if (same_visit) {
        bool changed = e->acknowledged;

        if (subject == DB_SUBJECT_UNKNOWN && e->subject == DB_SUBJECT_PERSON) {
            e->subject = DB_SUBJECT_UNKNOWN;
            changed = true;
        }
        if (conf > e->confidence_pm) {
            e->confidence_pm = conf;
            changed = true;
        }
        e->acknowledged = false;
        /* A stream of the same sighting changes nothing, and saves nothing. */
        if (changed) {
            b->log_rev++;
        }
        return;
    }
    e = db_guard_add(b->log, wall, subject, conf, false);
    b->visit_id = e ? e->id : 0;
    b->log_rev++;
}

static void visit_ended(struct db_brain *b, int64_t now)
{
    if (b->visit_end_ms == DB_NEVER) {
        b->visit_end_ms = now;
    }
}

/* Whoever is back, the guard stands down: a welcome, or the news. */
static void guard_stand_down(struct db_brain *b, int64_t now, bool owner_seen, int64_t wall)
{
    if (b->state == DB_ST_GUARD_PERSON_DETECTED) {
        /* Somebody who was never identified was still here. */
        log_visitor(b, DB_SUBJECT_PERSON, b->pending_conf, wall, now);
    }
    visit_ended(b, now);
    if (owner_seen && db_state_armed(b->state)) {
        db_guard_add(b->log, wall, DB_SUBJECT_OWNER, DB_CONF_NONE, true);
        b->log_rev++;
    }
    if (db_guard_unacknowledged(b->log) > 0) {
        enter(b, DB_ST_GUARD_ALERT_PENDING, now);
    } else if (owner_seen && recognition(b)) {
        enter(b, DB_ST_GUARD_OWNER, now);
    } else {
        enter(b, DB_ST_GUARD_DISARMED, now);
    }
}

/* ---- modes ----------------------------------------------------------------------- */

static void enter_mode(struct db_brain *b, enum db_mode mode, int64_t now)
{
    b->activity_ms = now;
    switch (mode) {
    case DB_MODE_GUARD:
        if (b->prefs.guard_armed) {
            enter(b, db_seen_present(b->seen) ? DB_ST_GUARD_ARMING : DB_ST_GUARD_ARMED, now);
        } else {
            enter(b, db_guard_unacknowledged(b->log) ? DB_ST_GUARD_ALERT_PENDING : DB_ST_GUARD_DISARMED, now);
        }
        break;
    case DB_MODE_NIGHT:
        enter(b, DB_ST_NIGHT_IDLE, now);
        break;
    case DB_MODE_COMPANION:
    default:
        enter(b, DB_ST_IDLE, now);
        break;
    }
    if (b->prefs.mode != mode) {
        b->prefs.mode = mode;
        b->prefs_rev++;
    }
}

void db_brain_init(struct db_brain *b, const struct db_prefs *prefs, struct db_guard_log *log, uint32_t seed,
                   int64_t now_ms)
{
    int k;

    memset(b, 0, sizeof(*b));
    if (prefs) {
        b->prefs = *prefs;
    } else {
        db_prefs_defaults(&b->prefs);
    }
    b->log = log;
    b->rng = seed ? seed : 0x9E3779B9u;
    b->seen = DB_SEEN_UNAVAILABLE; /* until a provider says otherwise */
    b->owner_seen_ms = -1;
    b->unknown_seen_ms = -1;
    b->visit_end_ms = 0;
    stop_react(b);
    for (k = 0; k < DB_POKES_TO_ANNOY; k++) {
        b->pokes[k] = LONG_AGO;
    }
    b->calm_until_ms = LONG_AGO;
    b->snack_until_ms = DB_NEVER;
    b->breath_ms = DB_NEVER;
    if (!b->prefs.on[DB_PREF_GUARD]) {
        b->prefs.guard_armed = false;
    }
    enter_mode(b, db_prefs_resolve_mode(&b->prefs, b->prefs.mode), now_ms);
    b->prefs_rev = 0; /* what was loaded is what is stored */
}

void db_brain_set_prefs(struct db_brain *b, const struct db_prefs *prefs, int64_t now_ms)
{
    enum db_mode was = db_state_mode(b->state);
    enum db_mode want;
    bool armed = b->prefs.guard_armed;

    b->prefs = *prefs;
    b->prefs.guard_armed = armed && prefs->on[DB_PREF_GUARD];
    b->prefs.mode = was;
    b->prefs_rev++;
    want = db_prefs_resolve_mode(&b->prefs, was);
    if (want != was || (was == DB_MODE_GUARD && armed && !b->prefs.guard_armed)) {
        if (was == DB_MODE_GUARD && db_state_armed(b->state)) {
            visit_ended(b, now_ms);
        }
        enter_mode(b, want, now_ms);
    } else {
        schedule_act(b, now_ms); /* the idle-animation switch may have changed */
        schedule_breath(b, now_ms);
    }
}

bool db_brain_set_mode(struct db_brain *b, enum db_mode mode, int64_t now_ms)
{
    if (!db_prefs_mode_allowed(&b->prefs, mode)) {
        return false;
    }
    if (db_state_mode(b->state) == mode) {
        return true;
    }
    if (db_state_armed(b->state)) {
        /* Whoever changes the mode is at the desk. */
        visit_ended(b, now_ms);
        set_armed(b, false);
    }
    enter_mode(b, mode, now_ms);
    return true;
}

bool db_brain_arm(struct db_brain *b, int64_t now_ms)
{
    if (db_state_mode(b->state) != DB_MODE_GUARD || !b->prefs.on[DB_PREF_GUARD] || db_state_armed(b->state)) {
        return false;
    }
    b->visit_id = 0;
    b->visit_end_ms = 0;
    /* The owner is usually still in front of it: wait until they have gone,
     * or they would be "back" at once. Blind, there is nobody to wait for. */
    enter(b, db_seen_present(b->seen) ? DB_ST_GUARD_ARMING : DB_ST_GUARD_ARMED, now_ms);
    return true;
}

bool db_brain_disarm(struct db_brain *b, int64_t now_ms)
{
    if (!db_state_armed(b->state)) {
        return false;
    }
    guard_stand_down(b, now_ms, false, b->wall_s);
    return true;
}

bool db_brain_acknowledge(struct db_brain *b, int64_t now_ms)
{
    if (b->state != DB_ST_GUARD_ALERT_PENDING) {
        return false;
    }
    if (db_guard_acknowledge_all(b->log)) {
        b->log_rev++;
    }
    enter(b, DB_ST_GUARD_DISARMED, now_ms);
    return true;
}

/* ---- vision ------------------------------------------------------------------------ */

static void companion_vision(struct db_brain *b, enum db_seen before, enum db_vision_kind kind,
                             bool greet_ok, bool react_ok, int64_t now)
{
    bool arrived = !db_seen_present(before) && db_seen_present(b->seen);

    if (!b->prefs.on[DB_PREF_COMPANION]) {
        return; /* resting: it sees, it does not react */
    }
    switch (kind) {
    case DB_VISION_OWNER_RECOGNIZED:
        if (greet_ok) {
            enter(b, DB_ST_OWNER_GREETING, now);
        } else if (b->state == DB_ST_SLEEP) {
            enter(b, DB_ST_WAKE, now);
        } else if (b->state == DB_ST_RECOGNIZING || b->state == DB_ST_DROWSY) {
            enter(b, DB_ST_IDLE, now);
        }
        break;
    case DB_VISION_UNKNOWN_PERSON:
        if (react_ok) {
            enter(b, DB_ST_UNKNOWN_REACTION, now);
        } else if (b->state == DB_ST_SLEEP) {
            enter(b, DB_ST_WAKE, now);
        } else if (b->state == DB_ST_RECOGNIZING || b->state == DB_ST_DROWSY) {
            enter(b, DB_ST_IDLE, now);
        }
        break;
    case DB_VISION_PERSON_DETECTED:
        if (arrived && (b->state == DB_ST_SLEEP || b->state == DB_ST_IDLE || b->state == DB_ST_DROWSY)) {
            enter(b, DB_ST_WAKE, now);
        }
        break;
    case DB_VISION_NO_PERSON:
    case DB_VISION_UNAVAILABLE:
    default:
        if (b->state == DB_ST_WAKE || b->state == DB_ST_RECOGNIZING) {
            enter(b, DB_ST_IDLE, now);
        }
        break;
    }
}

static void guard_vision(struct db_brain *b, enum db_vision_kind kind, const struct db_vision_event *ev,
                         int64_t now)
{
    int16_t conf = ev->confidence_pm;
    int64_t wall = event_wall(b, ev);

    switch (b->state) {
    case DB_ST_GUARD_ARMING:
        if (kind == DB_VISION_NO_PERSON || kind == DB_VISION_UNAVAILABLE) {
            enter(b, DB_ST_GUARD_ARMED, now);
        }
        break;
    case DB_ST_GUARD_ARMED:
        if (kind == DB_VISION_OWNER_RECOGNIZED) {
            guard_stand_down(b, now, true, wall);
        } else if (kind == DB_VISION_UNKNOWN_PERSON) {
            log_visitor(b, DB_SUBJECT_UNKNOWN, conf, wall, now);
            enter(b, DB_ST_GUARD_UNKNOWN, now);
        } else if (kind == DB_VISION_PERSON_DETECTED) {
            if (recognition(b)) {
                b->pending_conf = conf;
                enter(b, DB_ST_GUARD_PERSON_DETECTED, now);
            } else {
                log_visitor(b, DB_SUBJECT_PERSON, conf, wall, now);
                enter(b, DB_ST_GUARD_UNKNOWN, now);
            }
        }
        break;
    case DB_ST_GUARD_PERSON_DETECTED:
        if (kind == DB_VISION_OWNER_RECOGNIZED) {
            b->state = DB_ST_GUARD_ARMED; /* not a visitor after all: nothing to log for them */
            guard_stand_down(b, now, true, wall);
        } else if (kind == DB_VISION_UNKNOWN_PERSON) {
            log_visitor(b, DB_SUBJECT_UNKNOWN, conf > b->pending_conf ? conf : b->pending_conf, wall, now);
            enter(b, DB_ST_GUARD_UNKNOWN, now);
        } else if (kind == DB_VISION_NO_PERSON || kind == DB_VISION_UNAVAILABLE) {
            /* Gone before anyone could say who: still a visit. */
            log_visitor(b, DB_SUBJECT_PERSON, b->pending_conf, wall, now);
            visit_ended(b, now);
            enter(b, DB_ST_GUARD_ARMED, now);
        }
        break;
    case DB_ST_GUARD_UNKNOWN:
        if (kind == DB_VISION_OWNER_RECOGNIZED) {
            guard_stand_down(b, now, true, wall);
        } else if (kind == DB_VISION_UNKNOWN_PERSON) {
            log_visitor(b, DB_SUBJECT_UNKNOWN, conf, wall, now);
        } else if (kind == DB_VISION_NO_PERSON || kind == DB_VISION_UNAVAILABLE) {
            visit_ended(b, now);
            enter(b, DB_ST_GUARD_ARMED, now);
        }
        break;
    case DB_ST_GUARD_DISARMED:
        if (kind == DB_VISION_OWNER_RECOGNIZED && recognition(b) &&
            (b->owner_seen_ms < 0 || now - b->owner_seen_ms >= DB_OWNER_AWAY_MS)) {
            enter(b, DB_ST_GUARD_OWNER, now);
        }
        break;
    default:
        /* GUARD_OWNER and ALERT_PENDING wait for time and the owner. */
        break;
    }
}

static void night_vision(struct db_brain *b, enum db_seen before, int64_t now)
{
    if (!db_seen_present(before) && db_seen_present(b->seen) && b->state != DB_ST_NIGHT_PRESENCE) {
        enter(b, DB_ST_NIGHT_PRESENCE, now);
    }
}

void db_brain_vision(struct db_brain *b, const struct db_vision_event *in, int64_t now_ms)
{
    struct db_vision_event ev;
    enum db_seen before = b->seen;
    bool greet_ok;
    bool react_ok;

    if (!db_vision_event_valid(in)) {
        return;
    }
    ev = *in;
    b->events++;
    /* Recognition off: an identity is only "somebody". */
    if (!recognition(b) && (ev.kind == DB_VISION_OWNER_RECOGNIZED || ev.kind == DB_VISION_UNKNOWN_PERSON)) {
        ev.kind = DB_VISION_PERSON_DETECTED;
    }
    greet_ok = ev.kind == DB_VISION_OWNER_RECOGNIZED &&
               (b->owner_seen_ms < 0 || now_ms - b->owner_seen_ms >= DB_OWNER_AWAY_MS);
    react_ok = ev.kind == DB_VISION_UNKNOWN_PERSON &&
               (b->unknown_seen_ms < 0 || now_ms - b->unknown_seen_ms >= DB_UNKNOWN_AWAY_MS);
    switch (ev.kind) {
    case DB_VISION_NO_PERSON:
        b->seen = DB_SEEN_NOBODY;
        break;
    case DB_VISION_PERSON_DETECTED:
        /* A plain detection does not forget who it was. */
        if (b->seen != DB_SEEN_OWNER && b->seen != DB_SEEN_UNKNOWN) {
            b->seen = DB_SEEN_PERSON;
        }
        break;
    case DB_VISION_OWNER_RECOGNIZED:
        b->seen = DB_SEEN_OWNER;
        break;
    case DB_VISION_UNKNOWN_PERSON:
        b->seen = DB_SEEN_UNKNOWN;
        break;
    case DB_VISION_UNAVAILABLE:
    default:
        b->seen = DB_SEEN_UNAVAILABLE;
        break;
    }
    if (db_seen_present(b->seen) || db_seen_present(before)) {
        b->activity_ms = now_ms; /* the quiet starts when they leave */
    }
    switch (db_state_mode(b->state)) {
    case DB_MODE_GUARD:
        guard_vision(b, ev.kind, &ev, now_ms);
        break;
    case DB_MODE_NIGHT:
        night_vision(b, before, now_ms);
        break;
    case DB_MODE_COMPANION:
    default:
        companion_vision(b, before, ev.kind, greet_ok, react_ok, now_ms);
        break;
    }
    if (ev.kind == DB_VISION_OWNER_RECOGNIZED) {
        b->owner_seen_ms = now_ms;
    } else if (ev.kind == DB_VISION_UNKNOWN_PERSON) {
        b->unknown_seen_ms = now_ms;
    }
}

void db_brain_poke(struct db_brain *b, int64_t now_ms)
{
    b->activity_ms = now_ms;
    switch (b->state) {
    case DB_ST_SLEEP:
        if (b->prefs.on[DB_PREF_COMPANION]) {
            enter(b, DB_ST_WAKE, now_ms);
        }
        break;
    case DB_ST_DROWSY:
        enter(b, DB_ST_IDLE, now_ms);
        break;
    case DB_ST_NIGHT_IDLE:
    case DB_ST_NIGHT_SLEEP:
        enter(b, DB_ST_NIGHT_PRESENCE, now_ms);
        break;
    default:
        if (acts_allowed(b) && b->act == DB_ACT_NONE) {
            b->act = DB_ACT_BLINK;
            b->act_until_ms = now_ms + DB_BLINK_MS;
            b->next_act_ms = DB_NEVER;
        }
        break;
    }
}

/* ---- personality -------------------------------------------------------------------- */

bool db_brain_playful(const struct db_brain *b)
{
    return db_state_mode(b->state) == DB_MODE_COMPANION && b->prefs.on[DB_PREF_COMPANION];
}

bool db_brain_asleep(const struct db_brain *b)
{
    return b->state == DB_ST_SLEEP;
}

static int clamp_pm(int v, int lim)
{
    return v < -lim ? -lim : (v > lim ? lim : v);
}

/* Out of sleep or a doze, gently: asleep it stirs first, drowsy it simply
 * opens its eyes. Returns true when it was asleep. */
static bool rouse(struct db_brain *b, int64_t now)
{
    if (b->state == DB_ST_SLEEP) {
        enter(b, DB_ST_IDLE, now);
        play(b, DB_REACT_STIR, now);
        return true;
    }
    if (b->state == DB_ST_DROWSY) {
        enter(b, DB_ST_IDLE, now);
    }
    return false;
}

/* A poke: a look at where it was touched; again soon, a squint; too often,
 * a short huff - and after that a while in which it will not be wound up
 * again, so it always comes back to calm. */
static bool poked(struct db_brain *b, int64_t now)
{
    int recent = 0;
    int k;

    b->poked++;
    if (rouse(b, now)) {
        return true;
    }
    if (!calm(b) || b->react == DB_REACT_ANNOYED) {
        return false; /* a greeting is not interrupted; a huff is not restarted */
    }
    if (now < b->calm_until_ms) {
        start_beats(b, DB_REACT_POKE, poke_blink, 0, N_BEATS(poke_blink), now);
        return true;
    }
    for (k = DB_POKES_TO_ANNOY - 1; k > 0; k--) {
        b->pokes[k] = b->pokes[k - 1];
    }
    b->pokes[0] = now;
    for (k = 0; k < DB_POKES_TO_ANNOY; k++) {
        recent += now - b->pokes[k] < DB_POKE_WINDOW_MS;
    }
    if (recent >= DB_POKES_TO_ANNOY) {
        int64_t len = 0;

        play(b, DB_REACT_ANNOYED, now);
        for (k = 0; k < b->beat_count; k++) {
            len += b->beats[k].ms;
        }
        b->calm_until_ms = now + len + DB_ANNOY_COOLDOWN_MS;
        for (k = 0; k < DB_POKES_TO_ANNOY; k++) {
            b->pokes[k] = LONG_AGO;
        }
    } else {
        play(b, recent >= 2 ? DB_REACT_HEY : DB_REACT_POKE, now);
    }
    return true;
}

static bool petted(struct db_brain *b, int64_t now)
{
    int k;

    b->petted++;
    if (rouse(b, now)) {
        return true; /* a stroke over a sleeper wakes it gently; the next one pleases it */
    }
    if (!calm(b)) {
        return false;
    }
    /* Soothed: a stroke forgives the pokes, and calms a huff. */
    for (k = 0; k < DB_POKES_TO_ANNOY; k++) {
        b->pokes[k] = LONG_AGO;
    }
    if (b->react == DB_REACT_PET && b->beat < b->beat_count - 1) {
        /* Still pleased from the last stroke: stay pleased. */
        b->beat_until_ms = now + beat_len(b, &b->beats[b->beat]);
        return true;
    }
    play(b, DB_REACT_PET, now);
    return true;
}

static bool snack_moved(struct db_brain *b, int x, int y, int64_t now)
{
    bool was = b->snack;
    int64_t dx = x;
    int64_t dy = y - DB_MOUTH_Y_PM;

    b->snack = true;
    b->snack_x = x;
    b->snack_y = y;
    b->snack_until_ms = now + DB_SNACK_IDLE_MS;
    b->snack_near = dx * dx + dy * dy <= (int64_t)DB_MOUTH_REACH_PM * DB_MOUTH_REACH_PM;
    if (!was) {
        rouse(b, now);
        b->act = DB_ACT_NONE;
        b->next_act_ms = DB_NEVER;
    }
    return !was;
}

bool db_brain_stimulus(struct db_brain *b, const struct db_stimulus *s, int64_t now_ms)
{
    int x;
    int y;

    if (!s || (int)s->kind < 0 || s->kind >= DB_STIM_COUNT) {
        return false;
    }
    if (!db_brain_playful(b)) {
        /* Guard, Night, or Companion switched off: a touch is the plain
         * touch it always was, and nothing else is offered there. */
        if (s->kind == DB_STIM_POKE || s->kind == DB_STIM_PET) {
            db_brain_poke(b, now_ms);
        }
        b->snack = false;
        b->snack_near = false;
        return false;
    }
    x = clamp_pm(s->x_pm, 3000);
    y = clamp_pm(s->y_pm, 3000);
    b->activity_ms = now_ms;
    switch (s->kind) {
    case DB_STIM_POKE:
        b->look_x = x;
        b->look_y = y;
        return poked(b, now_ms);
    case DB_STIM_PET:
        b->look_x = x;
        b->look_y = y;
        return petted(b, now_ms);
    case DB_STIM_SNACK:
        return snack_moved(b, x, y, now_ms);
    case DB_STIM_SNACK_GONE:
        b->snack = false;
        b->snack_near = false;
        b->snack_until_ms = DB_NEVER;
        schedule_act(b, now_ms);
        return false;
    case DB_STIM_FEED:
        if (!b->snack) {
            return false; /* only a snack it was offered can be eaten */
        }
        b->snack = false;
        b->snack_near = false;
        b->snack_until_ms = DB_NEVER;
        b->fed++;
        if (!calm(b)) {
            enter(b, DB_ST_IDLE, now_ms);
        }
        play(b, DB_REACT_EAT, now_ms);
        return true;
    case DB_STIM_GREET:
        if (rouse(b, now_ms) || !calm(b)) {
            return b->react != DB_REACT_NONE;
        }
        play(b, DB_REACT_GREET, now_ms);
        return true;
    case DB_STIM_REST:
        b->snack = false;
        b->snack_near = false;
        if (b->state == DB_ST_SLEEP) {
            return false;
        }
        if (!calm(b)) {
            enter(b, DB_ST_IDLE, now_ms);
        }
        play(b, DB_REACT_REST, now_ms);
        return true;
    case DB_STIM_WAKE:
        if (b->state != DB_ST_SLEEP && b->state != DB_ST_DROWSY &&
            !(b->react == DB_REACT_REST || b->react == DB_REACT_YAWN)) {
            return false;
        }
        b->look_x = 0;
        b->look_y = 0;
        if (b->state != DB_ST_SLEEP) {
            enter(b, DB_ST_SLEEP, now_ms); /* half-way into a rest: back out the same way */
        }
        return rouse(b, now_ms);
    default:
        return false;
    }
}

/* ---- time ------------------------------------------------------------------------- */

static int64_t sleep_at(const struct db_brain *b)
{
    if (db_seen_present(b->seen) || b->react != DB_REACT_NONE || b->snack) {
        return DB_NEVER;
    }
    if (b->state == DB_ST_IDLE) {
        return b->activity_ms + DB_DROWSY_MS;
    }
    if (b->state == DB_ST_DROWSY) {
        return b->activity_ms + DB_IDLE_SLEEP_MS;
    }
    if (b->state == DB_ST_NIGHT_IDLE) {
        return b->activity_ms + DB_NIGHT_SLEEP_MS;
    }
    return DB_NEVER;
}

static void state_timeout(struct db_brain *b, int64_t now)
{
    switch (b->state) {
    case DB_ST_WAKE:
        if (b->seen == DB_SEEN_PERSON && recognition(b)) {
            enter(b, DB_ST_RECOGNIZING, now);
        } else {
            enter(b, DB_ST_IDLE, now);
        }
        break;
    case DB_ST_RECOGNIZING:
    case DB_ST_OWNER_GREETING:
    case DB_ST_UNKNOWN_REACTION:
        enter(b, DB_ST_IDLE, now);
        break;
    case DB_ST_GUARD_PERSON_DETECTED:
        log_visitor(b, DB_SUBJECT_PERSON, b->pending_conf, b->wall_s, now);
        enter(b, DB_ST_GUARD_UNKNOWN, now);
        break;
    case DB_ST_GUARD_OWNER:
        enter(b, db_guard_unacknowledged(b->log) ? DB_ST_GUARD_ALERT_PENDING : DB_ST_GUARD_DISARMED, now);
        break;
    case DB_ST_NIGHT_PRESENCE:
        b->activity_ms = now;
        enter(b, DB_ST_NIGHT_IDLE, now);
        break;
    default:
        b->deadline_ms = DB_NEVER;
        break;
    }
}

void db_brain_tick(struct db_brain *b, int64_t now_ms)
{
    int rounds;

    /* A late tick may owe several steps (a timeout, then an act); each round
     * moves a deadline forward, and the bound keeps a bad clock from
     * spinning. */
    for (rounds = 0; rounds < 8; rounds++) {
        if (b->deadline_ms <= now_ms) {
            state_timeout(b, now_ms);
        } else if (b->react != DB_REACT_NONE && b->beat_until_ms <= now_ms) {
            advance_react(b, now_ms);
        } else if (b->snack && b->snack_until_ms <= now_ms) {
            /* Nobody wanted it: put away, no fuss. */
            b->snack = false;
            b->snack_near = false;
            b->snack_until_ms = DB_NEVER;
            schedule_act(b, now_ms);
        } else if (sleep_at(b) <= now_ms) {
            if (b->state == DB_ST_IDLE) {
                enter(b, DB_ST_DROWSY, now_ms);
                if (motion_ok(b) && (rnd(b) & 1)) {
                    play(b, DB_REACT_YAWN, now_ms);
                }
            } else {
                enter(b, b->state == DB_ST_DROWSY ? DB_ST_SLEEP : DB_ST_NIGHT_SLEEP, now_ms);
            }
        } else if (b->breath_ms <= now_ms) {
            b->breath = !b->breath;
            b->breath_ms = DB_NEVER;
            schedule_breath(b, now_ms);
        } else if (b->act != DB_ACT_NONE && b->act_until_ms <= now_ms) {
            schedule_act(b, now_ms);
        } else if (b->act == DB_ACT_NONE && b->next_act_ms <= now_ms) {
            start_act(b, now_ms);
        } else {
            break;
        }
    }
}

int64_t db_brain_next_ms(const struct db_brain *b)
{
    int64_t next = b->deadline_ms;
    int64_t s = sleep_at(b);

    if (s < next) {
        next = s;
    }
    if (b->act != DB_ACT_NONE && b->act_until_ms < next) {
        next = b->act_until_ms;
    }
    if (b->act == DB_ACT_NONE && b->next_act_ms < next) {
        next = b->next_act_ms;
    }
    if (b->react != DB_REACT_NONE && b->beat_until_ms < next) {
        next = b->beat_until_ms;
    }
    if (b->snack && b->snack_until_ms < next) {
        next = b->snack_until_ms;
    }
    if (b->breath_ms < next) {
        next = b->breath_ms;
    }
    return next;
}

void db_brain_set_reduced_motion(struct db_brain *b, bool on, int64_t now_ms)
{
    if (b->reduced_motion != on) {
        b->reduced_motion = on;
        schedule_act(b, now_ms);
        schedule_breath(b, now_ms);
    }
}

void db_brain_set_wall(struct db_brain *b, int64_t wall_s)
{
    b->wall_s = wall_s > 0 ? wall_s : 0;
}

/* ---- the face ------------------------------------------------------------------- */

/* A reaction's beat, or the snack it is watching: the eyes, the mouth, the
 * look and the lean. */
static void personality_face(const struct db_brain *b, struct db_face *f)
{
    int lx;
    int ly;

    if (b->react != DB_REACT_NONE && b->beats) {
        const struct db_beat *beat = &b->beats[b->beat];

        lx = clamp_pm(b->look_x, 1000);
        ly = clamp_pm(b->look_y, 1000);
        f->expr = beat->expr;
        f->mouth = beat->mouth;
        if (beat->look) {
            f->look = true;
            f->look_x = lx;
            f->look_y = ly;
        }
        f->off_x = beat->lean * LEAN_PM * lx / 1000 + beat->shake * SHAKE_PM;
        f->off_y = beat->lean * LEAN_PM * ly / 1000 - beat->bob * BOB_PM;
        return;
    }
    if (b->snack) {
        lx = clamp_pm(b->snack_x, 1000);
        ly = clamp_pm(b->snack_y, 1000);
        f->look = true;
        f->look_x = lx;
        f->look_y = ly;
        if (b->snack_near) {
            /* Here it comes: eyes wide, mouth open, leaning in. */
            f->expr = DB_EXPR_WIDE;
            f->mouth = DB_MOUTH_O_BIG;
            f->off_x = LEAN_PM * lx / 1000;
            f->off_y = LEAN_PM * ly / 1000;
        } else {
            f->expr = DB_EXPR_CURIOUS;
        }
    }
}

void db_brain_face(const struct db_brain *b, struct db_face *out)
{
    struct db_face f = { .expr = DB_EXPR_OPEN };

    switch (b->state) {
    case DB_ST_SLEEP:
        f.expr = DB_EXPR_CLOSED;
        break;
    case DB_ST_DROWSY:
        f.expr = DB_EXPR_SLEEPY;
        break;
    case DB_ST_WAKE:
    case DB_ST_GUARD_PERSON_DETECTED:
        f.expr = DB_EXPR_WIDE;
        break;
    case DB_ST_RECOGNIZING:
    case DB_ST_GUARD_ALERT_PENDING:
        f.expr = DB_EXPR_CURIOUS;
        break;
    case DB_ST_OWNER_GREETING:
    case DB_ST_GUARD_OWNER:
        f.expr = DB_EXPR_HAPPY;
        break;
    case DB_ST_UNKNOWN_REACTION:
    case DB_ST_GUARD_UNKNOWN:
        f.expr = DB_EXPR_SUSPICIOUS;
        break;
    case DB_ST_NIGHT_IDLE:
        f.expr = DB_EXPR_SLEEPY;
        f.dim = true;
        break;
    case DB_ST_NIGHT_PRESENCE:
        f.expr = DB_EXPR_OPEN;
        f.dim = true;
        break;
    case DB_ST_NIGHT_SLEEP:
        f.expr = DB_EXPR_CLOSED;
        f.dim = true;
        break;
    default:
        break;
    }
    switch (b->act) {
    case DB_ACT_BLINK:
        f.expr = DB_EXPR_CLOSED;
        break;
    case DB_ACT_GLANCE_LEFT:
        f.glance = -1;
        break;
    case DB_ACT_GLANCE_RIGHT:
        f.glance = 1;
        break;
    case DB_ACT_DOZE:
        /* Awake, a doze is heavy lids; drowsy, it is a nod: shut. */
        f.expr = b->state == DB_ST_DROWSY ? DB_EXPR_CLOSED : DB_EXPR_SLEEPY;
        break;
    default:
        break;
    }
    if (calm(b)) {
        personality_face(b, &f);
    }
    if (b->breath && b->react == DB_REACT_NONE) {
        f.off_y -= BREATH_PM;
    }
    if (b->reduced_motion) {
        /* End states only (DS §12): no lean, no hop, no chewing. */
        f.off_x = 0;
        f.off_y = 0;
        if (f.mouth == DB_MOUTH_CHEW_OPEN || f.mouth == DB_MOUTH_CHEW_SHUT) {
            f.mouth = DB_MOUTH_SMILE;
        }
    }
    *out = f;
}
