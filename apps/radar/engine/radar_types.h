/*
 * PocketRadar vocabulary: contact classes, contact and run states, scope
 * geometry, engagement outcomes and the run event stream.
 *
 * PocketRadar is a game. It models no radio hardware, performs no RF
 * sensing and reads no sensor of any kind: every contact on the scope is
 * invented by radar_rng.c from a recorded seed. The vocabulary below
 * borrows the language of a tactical sensor because the game is played in
 * that idiom, not because anything here observes the physical world. This
 * is stated once here and again in docs/apps/POCKETRADAR.md so that no
 * reader of the source can mistake it for an instrument.
 *
 * Geometry. The scope is a circle. A contact sits at a bearing in
 * decidegrees (0 is straight up on the display, increasing clockwise) and
 * at a range in permille of the scope radius (0 at the centre, 1000 at the
 * rim). Integers throughout: the engine does no floating-point arithmetic
 * and calls no libm, so a run replays bit-identically on the host and on
 * the K230, and tests/radar_lint.sh keeps it that way.
 *
 * Time. Everything in the engine is counted in ticks of RADAR_TICK_MS.
 * The engine never reads a clock; the caller decides when a tick happens.
 *
 * Pure C, no LVGL and no I/O, so the whole engine is unit-tested natively.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETRADAR_TYPES_H
#define POCKETRADAR_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* Engine step. The UI ticks a run at this cadence and every duration in the
 * engine is a whole number of these. 20 Hz is fine enough for a 600 ms
 * acquisition to feel exact and coarse enough to leave the frame budget to
 * the sweep. */
#define RADAR_TICK_MS 50

/* Bearing, decidegrees clockwise from the top of the scope. */
#define RADAR_BEARING_MAX 3600
/* "045" plus terminator: the classic three-digit bearing readout. */
#define RADAR_BEARING_NAME_MAX 4

/* Range, permille of the scope radius. A contact is born in the outer band
 * and closes inbound as its time runs out, but it never reaches the centre:
 * the hub carries the readout, and a marker pinned under it could not be
 * tapped. */
#define RADAR_RANGE_MAX 1000
#define RADAR_RANGE_MIN 120
#define RADAR_SPAWN_RANGE_MIN 820

/* One decidegree is 1/572.9578 radian; 573 is within 0.008 % of that and
 * keeps the arc-length arithmetic in integers. */
#define RADAR_DECIDEG_PER_RADIAN 573

enum radar_class {
    RADAR_CLASS_NORMAL = 0,     /* valid target, standard score */
    RADAR_CLASS_FAST,           /* valid target, short-lived, worth more */
    RADAR_CLASS_DECOY,          /* must not be engaged */
    RADAR_CLASS_HIGH_VALUE,     /* valid target, rare, worth most */
    RADAR_CLASS_COUNT
};

/* A contact's class is hidden until acquisition completes, so the three
 * steps below are also the three things the player knows about it. */
enum radar_contact_state {
    RADAR_CONTACT_NEW = 0,      /* on the scope, unclassified */
    RADAR_CONTACT_SELECTED,     /* acquisition running */
    RADAR_CONTACT_ACQUIRED,     /* locked, class revealed, ENGAGE armed */
    RADAR_CONTACT_STATE_COUNT
};

enum radar_run_state {
    RADAR_RUN_READY = 0,        /* built from a seed, not yet started */
    RADAR_RUN_ACTIVE,
    RADAR_RUN_OVER,             /* sector integrity spent */
    RADAR_RUN_STATE_COUNT
};

enum radar_engage {
    RADAR_ENGAGE_INVALID = 0,   /* nothing acquired, or the run is not active */
    RADAR_ENGAGE_HIT,           /* a valid target was destroyed */
    RADAR_ENGAGE_FOUL,          /* the acquired contact was a decoy */
    RADAR_ENGAGE_COUNT
};

enum radar_event_type {
    RADAR_EVENT_NONE = 0,
    RADAR_EVENT_SPAWN,
    RADAR_EVENT_ACQUIRED,
    RADAR_EVENT_FADED,          /* the track ran out of time */
    RADAR_EVENT_HIT,
    RADAR_EVENT_FOUL,
    RADAR_EVENT_LEVEL,          /* sector activity stepped up */
    RADAR_EVENT_OVER,
    RADAR_EVENT_COUNT
};

/* ---- class table ------------------------------------------------------ */

/* Display name ("HIGH VALUE"), or "?" for an out-of-range class. */
const char *radar_class_name(enum radar_class cls);
/* Lifetime in ticks at level 0, before the difficulty scale is applied.
 * Returns 0 for an out-of-range class. */
uint16_t radar_class_base_ttl(enum radar_class cls);
/* Score before the response bonus and the streak multiplier; negative for a
 * class that must not be engaged. Returns 0 for an out-of-range class. */
int32_t radar_class_base_points(enum radar_class cls);
/* 1 when the class is a valid target, 0 for a decoy or an out-of-range
 * class. Letting a decoy fade is correct play and costs nothing. */
int radar_class_is_target(enum radar_class cls);

const char *radar_contact_state_name(enum radar_contact_state state);
const char *radar_run_state_name(enum radar_run_state state);
const char *radar_engage_name(enum radar_engage result);
const char *radar_event_name(enum radar_event_type type);

/* ---- geometry --------------------------------------------------------- */

/* Fold any bearing into [0, RADAR_BEARING_MAX). */
int radar_bearing_wrap(int bearing);
/* Shortest signed difference a - b, in (-1800, 1800]. */
int radar_bearing_delta(int a, int b);
/* Three-digit bearing readout in whole degrees ("045"). Writes at most n
 * bytes. Returns 0, or -1 when n is too small (buf then holds "?" when n
 * allows). */
int radar_bearing_name(int bearing, char *buf, size_t n);
/* Squared distance between two points on the scope, in permille units.
 * The tangential term is the small-angle arc length r*theta, which is what
 * touch picking needs and costs no trigonometry. Ranges outside
 * [0, RADAR_RANGE_MAX] are clamped so the result cannot overflow. */
int32_t radar_polar_dist2(int bearing_a, int range_a, int bearing_b, int range_b);

#endif
