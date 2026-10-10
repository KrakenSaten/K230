/*
 * DeskBuddy's touch gestures. See db_gesture.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "db_gesture.h"

#include <string.h>

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/* The larger of the two axes plus half the smaller: within 12 % of the true
 * distance, and no square root. */
static int dist(int dx, int dy)
{
    int a = iabs(dx);
    int b = iabs(dy);

    return a > b ? a + b / 2 : b + a / 2;
}

void db_gesture_init(struct db_gesture *g)
{
    memset(g, 0, sizeof(*g));
}

void db_gesture_press(struct db_gesture *g, int x, int y, int64_t now_ms)
{
    memset(g, 0, sizeof(*g));
    g->down = true;
    g->x0 = g->x = x;
    g->y0 = g->y = y;
    g->t0 = now_ms;
}

static void follow(struct db_gesture *g, int x, int y)
{
    int span;

    g->path += dist(x - g->x, y - g->y);
    g->x = x;
    g->y = y;
    span = dist(x - g->x0, y - g->y0);
    if (span > g->span) {
        g->span = span;
    }
}

static bool is_stroke(const struct db_gesture *g, int64_t now_ms)
{
    int64_t ms = now_ms - g->t0;

    return g->path >= DB_STROKE_PATH_PX && g->span >= DB_STROKE_SPAN_PX && ms >= DB_STROKE_MIN_MS &&
           g->path * 1000 <= (int64_t)DB_STROKE_MAX_SPEED * ms;
}

enum db_gesture_kind db_gesture_move(struct db_gesture *g, int x, int y, int64_t now_ms)
{
    if (!g->down) {
        return DB_GESTURE_NONE;
    }
    follow(g, x, y);
    if (!g->reported && is_stroke(g, now_ms)) {
        g->reported = true;
        return DB_GESTURE_STROKE;
    }
    return DB_GESTURE_NONE;
}

enum db_gesture_kind db_gesture_release(struct db_gesture *g, int x, int y, int64_t now_ms)
{
    enum db_gesture_kind k = DB_GESTURE_NONE;

    if (!g->down) {
        return DB_GESTURE_NONE;
    }
    follow(g, x, y);
    if (!g->reported) {
        if (g->span <= DB_TAP_SLOP_PX && now_ms - g->t0 <= DB_TAP_MS) {
            k = DB_GESTURE_TAP;
        } else if (is_stroke(g, now_ms)) {
            k = DB_GESTURE_STROKE;
        }
    }
    g->down = false;
    g->reported = true;
    return k;
}

void db_gesture_cancel(struct db_gesture *g)
{
    g->down = false;
    g->reported = true;
}
