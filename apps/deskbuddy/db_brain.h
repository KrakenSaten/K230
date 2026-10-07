/*
 * DeskBuddy's state machine: what it is doing, what its face shows, and
 * what the guard writes down. Pure C: no LVGL, no I/O, no clock of its own -
 * every call is given the monotonic time in ms, so tests drive it through
 * hours in microseconds (tests/db_brain_test.c).
 *
 * ONE STATE, NOT FLAGS. The mode (Companion, Guard, Night) and the state in
 * it are one enum, db_state; a state belongs to exactly one mode
 * (db_state_mode). What vision last said is one enum too, db_seen. Nothing
 * is a combination of booleans: "armed" is "the state is one of the armed
 * guard states", and the persisted prefs.guard_armed follows the state.
 *
 *   Companion  SLEEP -> WAKE -> RECOGNIZING -> OWNER_GREETING | UNKNOWN_REACTION -> IDLE
 *   Guard      DISARMED -> ARMING (waits for the owner to leave) -> ARMED
 *              ARMED -> PERSON_DETECTED -> UNKNOWN (logged) -> ARMED when they go
 *              any armed state + owner -> OWNER (welcome back) | ALERT_PENDING
 *              ALERT_PENDING -> acknowledge -> DISARMED
 *   Night      NIGHT_IDLE <-> NIGHT_PRESENCE, NIGHT_IDLE -> NIGHT_SLEEP when quiet
 *
 * Providers report changes; the brain holds the last report until the next
 * one. Repeats are cheap: an arrival is a change from nobody to somebody, the
 * owner is greeted again only after DB_OWNER_AWAY_MS unseen, a stranger is
 * reacted to again only after DB_UNKNOWN_AWAY_MS, and one guard visit is one
 * log line however many events it produces (DB_VISIT_MERGE_MS joins a
 * visitor who flickers out and back).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DB_BRAIN_H
#define DB_BRAIN_H

#include "db_guard.h"
#include "db_prefs.h"
#include "db_vision.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- timing (ms) ------------------------------------------------------------ */
#define DB_WAKE_MS 800             /* eyes pop open before anything else */
#define DB_RECOGNIZE_MS 4000       /* how long to wait for an identity */
#define DB_GREETING_MS 3500
#define DB_UNKNOWN_REACTION_MS 4000
#define DB_IDLE_SLEEP_MS 120000    /* nobody, no touch: Companion falls asleep */
#define DB_OWNER_AWAY_MS 60000     /* unseen this long before a new greeting */
#define DB_UNKNOWN_AWAY_MS 30000   /* the same for the curious look at a stranger */
#define DB_GUARD_IDENTIFY_MS 4000  /* a person not identified by then is logged as a person */
#define DB_GUARD_OWNER_MS 3500     /* "welcome back" */
#define DB_VISIT_MERGE_MS 15000    /* a visitor back within this is the same visit */
#define DB_NIGHT_PRESENCE_MS 5000
#define DB_NIGHT_SLEEP_MS 300000   /* quiet this long: the night face closes its eyes */
/* Idle behaviour: blinks, glances, a doze. */
#define DB_BLINK_MS 160
#define DB_NIGHT_BLINK_MS 420
#define DB_GLANCE_MS 1200
#define DB_DOZE_MS 3000
#define DB_ACT_GAP_MIN_MS 2500
#define DB_ACT_GAP_MAX_MS 7000
#define DB_NIGHT_ACT_GAP_MIN_MS 9000
#define DB_NIGHT_ACT_GAP_MAX_MS 16000

#define DB_NEVER INT64_MAX

enum db_state {
    DB_ST_SLEEP = 0,
    DB_ST_IDLE,
    DB_ST_WAKE,
    DB_ST_RECOGNIZING,
    DB_ST_OWNER_GREETING,
    DB_ST_UNKNOWN_REACTION,
    DB_ST_GUARD_DISARMED,
    DB_ST_GUARD_ARMING,
    DB_ST_GUARD_ARMED,
    DB_ST_GUARD_PERSON_DETECTED,
    DB_ST_GUARD_OWNER,
    DB_ST_GUARD_UNKNOWN,
    DB_ST_GUARD_ALERT_PENDING,
    DB_ST_NIGHT_IDLE,
    DB_ST_NIGHT_PRESENCE,
    DB_ST_NIGHT_SLEEP,
    DB_STATE_COUNT
};

/* What vision last said. */
enum db_seen {
    DB_SEEN_UNAVAILABLE = 0, /* no vision, or it failed: DeskBuddy runs blind */
    DB_SEEN_NOBODY,
    DB_SEEN_PERSON,          /* somebody, identity unknown */
    DB_SEEN_OWNER,
    DB_SEEN_UNKNOWN          /* somebody who is not the owner */
};

enum db_expr {
    DB_EXPR_OPEN = 0,   /* ◉ ◉ */
    DB_EXPR_CLOSED,     /* — — */
    DB_EXPR_SLEEPY,     /* half-lidded */
    DB_EXPR_HAPPY,      /* ^ ^ */
    DB_EXPR_SUSPICIOUS, /* ಠ ಠ : narrowed, brows down */
    DB_EXPR_CURIOUS,    /* one eye a little bigger, looking up */
    DB_EXPR_WIDE,       /* startled awake */
    DB_EXPR_COUNT
};

/* The idle behaviour running now, at most one. */
enum db_act {
    DB_ACT_NONE = 0,
    DB_ACT_BLINK,
    DB_ACT_GLANCE_LEFT,
    DB_ACT_GLANCE_RIGHT,
    DB_ACT_DOZE
};

/* What the screen draws. */
struct db_face {
    enum db_expr expr;
    int glance;  /* -1 left, 0 ahead, +1 right */
    bool dim;    /* night: draw in the quiet colour */
};

struct db_brain {
    struct db_prefs prefs;
    struct db_guard_log *log;   /* the app's; the brain appends and acknowledges */
    enum db_state state;
    int64_t since_ms;           /* when the state was entered */
    int64_t deadline_ms;        /* when the state times out, DB_NEVER if it does not */
    enum db_seen seen;
    int64_t activity_ms;        /* last presence, touch or mode change: the sleep clocks count from here */
    int64_t owner_seen_ms;      /* last owner event, -1 never */
    int64_t unknown_seen_ms;    /* last stranger event, -1 never */
    /* The guard visit in progress, or the last one: its log entry and when
     * it ended (DB_NEVER while it is still going on). */
    uint32_t visit_id;
    int64_t visit_end_ms;
    int16_t pending_conf;       /* confidence carried into a PERSON_DETECTED visit */
    enum db_act act;
    int64_t act_until_ms;
    int64_t next_act_ms;        /* DB_NEVER when idle behaviour is off here */
    bool reduced_motion;
    int64_t wall_s;             /* the wall clock the app last gave, 0 when unset */
    uint32_t rng;
    unsigned prefs_rev;         /* bumped when prefs (mode, armed) change: save them */
    unsigned log_rev;           /* bumped when the log changes: save it */
    unsigned events;            /* vision events taken, for the screen's debug line */
};

const char *db_state_name(enum db_state s);
enum db_mode db_state_mode(enum db_state s);
bool db_state_armed(enum db_state s);
const char *db_seen_name(enum db_seen s);
bool db_seen_present(enum db_seen s);

/* log must outlive the brain. prefs are copied; the mode and armed state
 * start from them. */
void db_brain_init(struct db_brain *b, const struct db_prefs *prefs, struct db_guard_log *log, uint32_t seed,
                   int64_t now_ms);
/* New preferences (the settings panel). Re-resolves the mode; turning the
 * guard off disarms it. */
void db_brain_set_prefs(struct db_brain *b, const struct db_prefs *prefs, int64_t now_ms);
/* false when the preferences do not offer that mode. Leaving Guard disarms. */
bool db_brain_set_mode(struct db_brain *b, enum db_mode mode, int64_t now_ms);
/* false when not in Guard mode, the guard is off, or already armed. */
bool db_brain_arm(struct db_brain *b, int64_t now_ms);
/* false when not armed. */
bool db_brain_disarm(struct db_brain *b, int64_t now_ms);
/* Acknowledge the visitors shown in ALERT_PENDING. false when none pending. */
bool db_brain_acknowledge(struct db_brain *b, int64_t now_ms);

void db_brain_vision(struct db_brain *b, const struct db_vision_event *ev, int64_t now_ms);
/* A touch on the face: somebody is certainly here. */
void db_brain_poke(struct db_brain *b, int64_t now_ms);
/* Time passing: timeouts and idle behaviour. Call at db_brain_next_ms(). */
void db_brain_tick(struct db_brain *b, int64_t now_ms);
/* The earliest time db_brain_tick() has something to do; DB_NEVER if never. */
int64_t db_brain_next_ms(const struct db_brain *b);

void db_brain_set_reduced_motion(struct db_brain *b, bool on, int64_t now_ms);
void db_brain_set_wall(struct db_brain *b, int64_t wall_s);

void db_brain_face(const struct db_brain *b, struct db_face *out);

#endif
