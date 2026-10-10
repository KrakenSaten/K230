/*
 * DeskBuddy's touch gestures on the character: a tap (a poke) or a gentle
 * stroke (petting), told apart from one finger's path. Pure C, no LVGL and
 * no clock: the screen feeds it points and times in px and ms, so tests
 * draw any gesture (tests/db_personality_test.c).
 *
 *   tap     down and up within DB_TAP_MS, never more than DB_TAP_SLOP_PX
 *           from where it started
 *   stroke  at least DB_STROKE_PATH_PX of path, DB_STROKE_SPAN_PX away
 *           from the start at some point, over at least DB_STROKE_MIN_MS,
 *           and no faster on average than DB_STROKE_MAX_SPEED - a flick is
 *           not a stroke, and neither is a long press that never moves
 *
 * One gesture is one result at most: a stroke is reported once, as soon as
 * it qualifies (so the response comes while the finger is still moving),
 * and never again however long the finger goes on; its release then reports
 * nothing. A cancelled gesture (the press was lost) reports nothing.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DB_GESTURE_H
#define DB_GESTURE_H

#include <stdbool.h>
#include <stdint.h>

#define DB_TAP_MS 350
#define DB_TAP_SLOP_PX 16
#define DB_STROKE_PATH_PX 110
#define DB_STROKE_SPAN_PX 50
#define DB_STROKE_MIN_MS 220
#define DB_STROKE_MAX_SPEED 1500 /* px per 1000 ms, averaged over the gesture */

enum db_gesture_kind {
    DB_GESTURE_NONE = 0,
    DB_GESTURE_TAP,
    DB_GESTURE_STROKE
};

struct db_gesture {
    bool down;
    bool reported;      /* this gesture has had its one result */
    int x0;
    int y0;
    int x;
    int y;
    int64_t t0;
    int64_t path;       /* px travelled */
    int span;           /* the furthest it has been from the start, px */
};

void db_gesture_init(struct db_gesture *g);
void db_gesture_press(struct db_gesture *g, int x, int y, int64_t now_ms);
/* The finger moved. STROKE once, when the stroke qualifies. */
enum db_gesture_kind db_gesture_move(struct db_gesture *g, int x, int y, int64_t now_ms);
/* The finger lifted. TAP, a STROKE that qualified only now, or NONE. */
enum db_gesture_kind db_gesture_release(struct db_gesture *g, int x, int y, int64_t now_ms);
/* The press was lost (scrolled away, the object went): nothing. */
void db_gesture_cancel(struct db_gesture *g);

#endif
