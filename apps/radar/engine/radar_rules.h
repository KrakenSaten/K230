/*
 * PocketRadar run state and rules: how contacts appear, move and fade, and
 * how the player selects and acquires one.
 *
 * The interaction is SELECT -> ACQUIRE -> ENGAGE, and each step is a
 * separate thing the player knows. A tap selects a contact and nothing
 * more. Acquisition then runs by itself for RADAR_ACQUIRE_TICKS while the
 * selection holds, and only when it completes is the contact's class
 * revealed and a shot armed. That is what gives the decoy something to do:
 * a decoy cannot be told from a target until it has been locked, so the
 * price of a decoy is the acquisition time it steals from the tracks that
 * are still fading. Engaging is P3.
 *
 * A contact closes inbound as its time runs out. Range is not integrated
 * per tick; it is derived from the remaining lifetime,
 *
 *     range = RADAR_RANGE_MIN + (spawn_range - RADAR_RANGE_MIN)
 *             * ttl / ttl_max
 *
 * so it cannot accumulate rounding error and a contact reaches the inner
 * limit exactly as its track fades. This makes the scope readable without
 * a single number on it: how close a contact is to the hub is how little
 * time is left to work it. It also collapses two tuning knobs into one -
 * a shorter-lived class is a visibly faster one - which is the whole of
 * the reason a fast contact needs no speed of its own.
 *
 * Bearing drift is the independent axis: it is what makes a contact hard
 * to tap rather than merely urgent.
 *
 * Determinism. A run is reproduced by (seed, the ordered list of player
 * actions at their ticks). Only spawning draws from the generator, and it
 * draws a fixed four values on a fixed schedule whether or not there is
 * room on the scope, so the sequence of contacts a seed produces is a
 * function of the tick count alone and never of how the player plays. Two
 * players on the same seed meet the same contacts.
 *
 * Pure C, no LVGL and no I/O.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETRADAR_RULES_H
#define POCKETRADAR_RULES_H

#include "radar_rng.h"
#include "radar_types.h"

/* Contacts live in a fixed array; the engine allocates nothing. */
#define RADAR_CONTACTS_MAX 12
/* Ticks of unbroken selection before a contact is locked and its class is
 * revealed. 12 ticks is 600 ms: long enough to be a deliberate act, short
 * enough that several fit inside one contact's life. */
#define RADAR_ACQUIRE_TICKS 12
/* How near a tap has to land, in permille of the scope radius. On a 240 px
 * scope that is a 38 px pick radius, so the smallest thing the player has
 * to hit is 76 px across against the 64 px minimum touch target of the
 * Design System (DS section 7). */
#define RADAR_PICK_RADIUS 160
/* The sweep belongs to the run so that it is deterministic and the UI has
 * nothing to invent. 45 decidegrees a tick is one turn every four seconds. */
#define RADAR_SWEEP_DD_PER_TICK 45

/* Baseline spawning. The difficulty ramp that scales these with elapsed
 * time is P3; until then every run plays at the opening rate. */
#define RADAR_SPAWN_TICKS_BASE 40
#define RADAR_CONTACTS_BASE 4
#define RADAR_DRIFT_BASE 3
/* Spawn shares in percent; NORMAL takes the remainder. */
#define RADAR_DECOY_PCT_BASE 15
#define RADAR_HIGH_VALUE_PCT_BASE 6
#define RADAR_FAST_PCT_BASE 18

/* One tick can retire the whole scope and start something new, so the
 * queue holds more than that. A caller that drains it every tick can never
 * overflow it; one that does not loses the newest events and is told so by
 * events_dropped. */
#define RADAR_EVENTS_MAX 24

/* An id of 0 is never handed out, so it means "no contact" everywhere. */
#define RADAR_NO_CONTACT 0u

struct radar_contact {
    uint32_t id;            /* unique within a run; 0 in an unused slot */
    uint8_t active;
    uint8_t cls;            /* enum radar_class */
    uint8_t state;          /* enum radar_contact_state */
    /* Sticky: what acquisition revealed stays revealed, so a decoy that has
     * been identified once never has to be identified again. A firing
     * solution is not sticky, and re-selecting starts the lock afresh. */
    uint8_t classified;
    uint16_t bearing;       /* decidegrees */
    uint16_t range;         /* permille, derived from ttl every tick */
    uint16_t spawn_range;   /* permille, where the track was first painted */
    int16_t drift;          /* decidegrees per tick, signed */
    uint16_t ttl;           /* ticks of life remaining */
    uint16_t ttl_max;
    uint16_t lock;          /* acquisition ticks accumulated */
    uint16_t age;           /* ticks since the track appeared */
};

/* What happened, for the UI to react to and for tests to assert on. The
 * run state alone cannot carry this: a contact that faded is gone from the
 * array by the time anyone looks. */
struct radar_event {
    uint8_t type;           /* enum radar_event_type */
    uint8_t cls;            /* enum radar_class of the contact involved */
    uint16_t bearing;       /* where it happened, for a mark on the scope */
    uint16_t range;
    uint32_t id;            /* the contact, or RADAR_NO_CONTACT */
    int32_t value;          /* meaning depends on type; 0 in P2 */
};

struct radar_run {
    uint32_t seed;
    struct radar_rng rng;
    uint8_t state;          /* enum radar_run_state */
    uint32_t ticks;         /* ticks since the run started */
    uint32_t next_id;
    uint16_t spawn_timer;   /* ticks until the next spawn attempt */
    uint16_t sweep;         /* decidegrees, 0..3599 */
    uint32_t selected;      /* id of the selected contact, or RADAR_NO_CONTACT */
    struct radar_contact contacts[RADAR_CONTACTS_MAX];
    struct radar_event events[RADAR_EVENTS_MAX];
    uint8_t event_count;
    uint16_t events_dropped;
};

/* ---- run ------------------------------------------------------------- */

/* Build a run from a seed. It is RADAR_RUN_READY: nothing spawns and no
 * tick counts until it is started. */
void radar_run_new(struct radar_run *run, uint32_t seed);
/* Begin. Returns 0, or -1 when the run is not RADAR_RUN_READY. */
int radar_run_start(struct radar_run *run);
/* One step of RADAR_TICK_MS. Ages every track, moves it, retires it when
 * its time is up, advances an acquisition in progress and attempts a spawn
 * on schedule. Does nothing unless the run is RADAR_RUN_ACTIVE, so a
 * caller that keeps ticking a finished run cannot corrupt it. */
void radar_run_tick(struct radar_run *run);
int radar_run_is_over(const struct radar_run *run);

/* ---- contacts -------------------------------------------------------- */

int radar_run_contact_count(const struct radar_run *run);
/* Slot by index for the UI to walk, active or not. NULL when out of range. */
const struct radar_contact *radar_run_slot(const struct radar_run *run, int slot);
/* The active contact with this id, or NULL. */
const struct radar_contact *radar_run_find(const struct radar_run *run, uint32_t id);
const struct radar_contact *radar_run_selected(const struct radar_run *run);

/* Acquisition progress, 0 to 1000, for the lock ring. */
int radar_contact_lock_permille(const struct radar_contact *contact);
/* Life remaining, 1000 at the moment it appeared down to 0. */
int radar_contact_ttl_permille(const struct radar_contact *contact);

/* ---- selection ------------------------------------------------------- */

/* Select a contact, which starts its acquisition. Selecting the contact
 * that is already selected changes nothing, so a second tap cannot throw
 * away the lock it has built. Any other selection abandons the previous
 * one. Returns 0, or -1 when the run is not active or the id is not on the
 * scope. */
int radar_run_select(struct radar_run *run, uint32_t id);
/* Abandon the current selection. Safe at any time. */
void radar_run_deselect(struct radar_run *run);
/* Which contact a tap at (bearing, range) picks: the nearest active
 * contact within RADAR_PICK_RADIUS, the older of two at the same distance,
 * or RADAR_NO_CONTACT when the tap found open water. The UI converts touch
 * coordinates to polar and asks; picking itself is a rule and is tested as
 * one. */
uint32_t radar_run_pick(const struct radar_run *run, int bearing, int range);

/* ---- events ---------------------------------------------------------- */

/* Take the oldest pending event. Returns 1 and fills out, or 0 when the
 * queue is empty. */
int radar_run_take_event(struct radar_run *run, struct radar_event *out);
void radar_run_clear_events(struct radar_run *run);

#endif
