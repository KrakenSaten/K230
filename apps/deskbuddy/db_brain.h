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
 * Companion also falls asleep by degrees: IDLE -> DROWSY (heavy lids, slow
 * blinks, a nod) -> SLEEP, counted from the last presence or touch.
 *
 * PERSONALITY. On top of the state, Companion's calm states (IDLE, DROWSY)
 * play short reactions to what is done to it - a poke, a stroke, a snack -
 * as a sequence of beats, each an expression, a mouth, where the eyes look
 * and how the whole character leans, held for a few hundred ms. A reaction
 * is told by a db_stimulus (db_brain_stimulus): its kind says what happened,
 * never how it was sensed, so the touch screen today and a camera that
 * sees a wave tomorrow lead to the same reaction. Guard and Night are not
 * changed by it: there a stimulus is the plain touch it always was
 * (db_brain_poke).
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
/* Falling asleep by degrees (Companion). */
#define DB_DROWSY_MS 60000         /* quiet this long: heavy lids; DB_IDLE_SLEEP_MS: asleep */
#define DB_DROWSY_BLINK_MS 520
#define DB_NOD_MS 1400
#define DB_DROWSY_GAP_MIN_MS 4000
#define DB_DROWSY_GAP_MAX_MS 9000
/* Breathing: the character rises and settles, slower asleep. */
#define DB_BREATH_MS 2000
#define DB_SLEEP_BREATH_MS 3200
/* Pokes: this many inside the window and it is annoyed, briefly; after
 * that it will not be wound up again for the cool-down. */
#define DB_POKE_WINDOW_MS 3000
#define DB_POKES_TO_ANNOY 4
#define DB_ANNOY_COOLDOWN_MS 6000
/* A snack nobody moves for this long is put away again, quietly. */
#define DB_SNACK_IDLE_MS 20000
/* Where the mouth sits, and how near a snack must come to be "at" it: from
 * the middle of the eye line, in per-mille of the eye box. The screen lays
 * the mouth out there (db_face.h) and the brain judges a snack's distance
 * from it, so the two cannot disagree. */
#define DB_MOUTH_Y_PM 700
#define DB_MOUTH_REACH_PM 520
/* The character's height from the top of the eyes, per-mille of the box:
 * the eyes, the gap and the deepest mouth. */
#define DB_CHARACTER_H_PM 1340

#define DB_NEVER INT64_MAX

enum db_state {
    DB_ST_SLEEP = 0,
    DB_ST_IDLE,
    DB_ST_WAKE,
    DB_ST_RECOGNIZING,
    DB_ST_OWNER_GREETING,
    DB_ST_UNKNOWN_REACTION,
    DB_ST_DROWSY,
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
    DB_EXPR_SQUINT,     /* "hey." - poked again */
    DB_EXPR_ANNOYED,    /* a flat glare: poked too often */
    DB_EXPR_CONTENT,    /* soft half-lids: petted, fed */
    DB_EXPR_WINK,       /* the mischievous one */
    DB_EXPR_COUNT
};

enum db_mouth {
    DB_MOUTH_NONE = 0,  /* the usual face is eyes alone */
    DB_MOUTH_SMILE,
    DB_MOUTH_SMIRK,
    DB_MOUTH_O,         /* surprised */
    DB_MOUTH_O_BIG,     /* a yawn, or a snack coming */
    DB_MOUTH_FLAT,      /* not amused */
    DB_MOUTH_CHEW_OPEN,
    DB_MOUTH_CHEW_SHUT,
    DB_MOUTH_COUNT
};

/* A reaction playing on top of a calm Companion state, at most one. */
enum db_react {
    DB_REACT_NONE = 0,
    DB_REACT_POKE,      /* startled, then a look at where it was touched */
    DB_REACT_HEY,       /* poked again soon: a squint */
    DB_REACT_ANNOYED,   /* poked too often: a huff, then it settles */
    DB_REACT_PET,       /* pleased */
    DB_REACT_EAT,
    DB_REACT_GREET,     /* hello */
    DB_REACT_STIR,      /* woken from sleep */
    DB_REACT_YAWN,      /* nodding off */
    DB_REACT_REST,      /* a yawn, then asleep */
    DB_REACT_COUNT
};

/* What happened to DeskBuddy. The kind is what it means, not how it was
 * sensed: a vision provider that sees a wave sends GREET as the screen
 * would. x_pm and y_pm, where it happened (POKE, PET, SNACK), are from the
 * middle of the eye line in per-mille of an eye box, y down. */
enum db_stim_kind {
    DB_STIM_POKE = 0,   /* a tap on the character */
    DB_STIM_PET,        /* a gentle stroke over it */
    DB_STIM_SNACK,      /* a snack is offered, or was moved, at x, y */
    DB_STIM_SNACK_GONE, /* put away uneaten */
    DB_STIM_FEED,       /* the snack reached its mouth */
    DB_STIM_GREET,      /* somebody says hello (no source sends it yet) */
    DB_STIM_REST,       /* sleep now */
    DB_STIM_WAKE,       /* wake up now */
    DB_STIM_COUNT
};

enum db_stim_source {
    DB_SRC_TOUCH = 0,
    DB_SRC_KEY,
    DB_SRC_VISION
};

struct db_stimulus {
    enum db_stim_kind kind;
    enum db_stim_source source;
    int x_pm;
    int y_pm;
};

/* One step of a reaction. */
struct db_beat {
    enum db_expr expr;
    enum db_mouth mouth;
    signed char look;   /* 1: at the stimulus, 0: ahead */
    signed char lean;   /* toward the stimulus (+1), away (-1), or not */
    signed char shake;  /* a sideways shift, -1 / +1 */
    signed char bob;    /* +1: a small hop up */
    int ms;
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
    /* Personality (appended; zero is no look, no mouth, no shift). */
    bool look;          /* the pupils look at look_x, look_y instead of the glance */
    int look_x;         /* -1000..1000 of the room a pupil has, x right, y down */
    int look_y;
    enum db_mouth mouth;
    int off_x;          /* the whole character's shift, per-mille of the eye box */
    int off_y;
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
    bool vision_pending;        /* a source started, nothing heard from it yet */
    /* Personality: the reaction playing, its beats and where it looks. */
    enum db_react react;
    const struct db_beat *beats;
    int beat;
    int beat_count;
    int64_t beat_until_ms;      /* DB_NEVER with no reaction */
    int look_x;                 /* the stimulus, per-mille of the eye box */
    int look_y;
    int64_t pokes[DB_POKES_TO_ANNOY]; /* the latest pokes, newest first */
    int64_t calm_until_ms;      /* after a huff: pokes do not wind it up again */
    /* The snack, while one is offered. */
    bool snack;
    bool snack_near;            /* at its mouth: let go and it eats */
    int snack_x;
    int snack_y;
    int64_t snack_until_ms;     /* put away when nobody moves it */
    bool breath;                /* the risen half of a breath */
    int64_t breath_ms;          /* next breath, DB_NEVER when still */
    unsigned fed;               /* counts, for tests and the screen */
    unsigned petted;
    unsigned poked;
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
/* A touch on the face: somebody is certainly here. The plain touch, with no
 * place and no personality: what Guard and Night do with every stimulus. */
void db_brain_poke(struct db_brain *b, int64_t now_ms);
/* Something happened to DeskBuddy (see db_stimulus). In Companion's calm
 * states it reacts; elsewhere a touch is db_brain_poke and the rest is
 * ignored. Returns true when a reaction started. Never fails on bad input:
 * a kind out of range is ignored, a place is clamped. */
bool db_brain_stimulus(struct db_brain *b, const struct db_stimulus *s, int64_t now_ms);
/* Whether it plays now - Companion mode with Companion on - so the screen
 * offers the snack and rest. */
bool db_brain_playful(const struct db_brain *b);
/* Asleep in Companion: the screen's rest control says WAKE. */
bool db_brain_asleep(const struct db_brain *b);
const char *db_react_name(enum db_react r);
/* Time passing: timeouts and idle behaviour. Call at db_brain_next_ms(). */
void db_brain_tick(struct db_brain *b, int64_t now_ms);
/* The earliest time db_brain_tick() has something to do; DB_NEVER if never. */
int64_t db_brain_next_ms(const struct db_brain *b);

void db_brain_set_reduced_motion(struct db_brain *b, bool on, int64_t now_ms);
/* A vision source has just been started and has not reported yet. Until
 * its first event, ARM waits for "nobody" (GUARD_ARMING) instead of taking
 * the silence for an empty desk. Any vision event clears it. */
void db_brain_set_vision_pending(struct db_brain *b, bool pending);
void db_brain_set_wall(struct db_brain *b, int64_t wall_s);

void db_brain_face(const struct db_brain *b, struct db_face *out);

#endif
