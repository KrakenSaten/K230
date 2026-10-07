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
    "UNKNOWN_REACTION", "GUARD_DISARMED", "GUARD_ARMING", "GUARD_ARMED",       "GUARD_PERSON_DETECTED",
    "GUARD_OWNER",    "GUARD_UNKNOWN", "GUARD_ALERT_PENDING", "NIGHT_IDLE",    "NIGHT_PRESENCE",
    "NIGHT_SLEEP",
};

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

/* Idle behaviour runs in the calm states only, and never under reduced
 * motion (DS §12: end states only) or with the preference off. */
static bool acts_allowed(const struct db_brain *b)
{
    if (b->reduced_motion || !b->prefs.on[DB_PREF_IDLE_ANIMATION]) {
        return false;
    }
    switch (b->state) {
    case DB_ST_IDLE:
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

static void enter(struct db_brain *b, enum db_state s, int64_t now)
{
    int64_t t = timeout_of(s);

    b->state = s;
    b->since_ms = now;
    b->deadline_ms = t ? now + t : DB_NEVER;
    if (db_state_mode(s) == DB_MODE_GUARD) {
        set_armed(b, db_state_armed(s));
    }
    schedule_act(b, now);
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
        } else if (b->state == DB_ST_RECOGNIZING) {
            enter(b, DB_ST_IDLE, now);
        }
        break;
    case DB_VISION_UNKNOWN_PERSON:
        if (react_ok) {
            enter(b, DB_ST_UNKNOWN_REACTION, now);
        } else if (b->state == DB_ST_SLEEP) {
            enter(b, DB_ST_WAKE, now);
        } else if (b->state == DB_ST_RECOGNIZING) {
            enter(b, DB_ST_IDLE, now);
        }
        break;
    case DB_VISION_PERSON_DETECTED:
        if (arrived && (b->state == DB_ST_SLEEP || b->state == DB_ST_IDLE)) {
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

/* ---- time ------------------------------------------------------------------------- */

static int64_t sleep_at(const struct db_brain *b)
{
    if (db_seen_present(b->seen)) {
        return DB_NEVER;
    }
    if (b->state == DB_ST_IDLE) {
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
        } else if (sleep_at(b) <= now_ms) {
            enter(b, b->state == DB_ST_IDLE ? DB_ST_SLEEP : DB_ST_NIGHT_SLEEP, now_ms);
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
    return next;
}

void db_brain_set_reduced_motion(struct db_brain *b, bool on, int64_t now_ms)
{
    if (b->reduced_motion != on) {
        b->reduced_motion = on;
        schedule_act(b, now_ms);
    }
}

void db_brain_set_wall(struct db_brain *b, int64_t wall_s)
{
    b->wall_s = wall_s > 0 ? wall_s : 0;
}

/* ---- the face ------------------------------------------------------------------- */

void db_brain_face(const struct db_brain *b, struct db_face *out)
{
    struct db_face f = { DB_EXPR_OPEN, 0, false };

    switch (b->state) {
    case DB_ST_SLEEP:
        f.expr = DB_EXPR_CLOSED;
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
        f.expr = DB_EXPR_SLEEPY;
        break;
    default:
        break;
    }
    *out = f;
}
