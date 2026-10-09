/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "db_personality.h"
#include <stdlib.h>
#include <string.h>

static int clamp(int v) { return v < -1000 ? -1000 : v > 1000 ? 1000 : v; }
static void react(struct db_personality *p, enum db_reaction r, int64_t duration, int64_t now)
{
    p->reaction = r;
    p->since_ms = now;
    p->until_ms = duration ? now + duration : DB_NEVER;
}
void db_personality_init(struct db_personality *p, int64_t now)
{
    memset(p, 0, sizeof(*p));
    p->activity_ms = now;
    p->last_poke_ms = -2000;
    react(p, DB_REACT_CALM, 0, now);
}
void db_personality_event(struct db_personality *p, enum db_interaction event, int x, int y, int64_t now)
{
    p->activity_ms = now;
    p->x = clamp(x);
    p->y = clamp(y);
    switch (event) {
    case DB_INTERACT_POKE:
        if (p->reaction == DB_REACT_ASLEEP || p->reaction == DB_REACT_DROWSY) {
            p->pokes = 0;
            react(p, DB_REACT_WAKE, 240, now);
            break;
        }
        p->pokes = now - p->last_poke_ms < 900 ? p->pokes + 1 : 1;
        p->last_poke_ms = now;
        p->variation++;
        react(p, p->pokes >= 3 ? DB_REACT_ANNOYED : DB_REACT_POKE,
              p->pokes >= 3 ? 850 : 180 + (p->variation % 3) * 30, now);
        break;
    case DB_INTERACT_PET:
        p->pokes = 0;
        p->variation++;
        react(p, DB_REACT_HAPPY, 1400 + (p->variation % 3) * 100, now);
        break;
    case DB_INTERACT_SNACK:
        p->snack = true;
        react(p, DB_REACT_CURIOUS, 0, now);
        break;
    case DB_INTERACT_FOLLOW:
        if (p->snack) react(p, DB_REACT_CURIOUS, 0, now);
        break;
    case DB_INTERACT_FEED:
        if (!p->snack) break;
        p->snack = false;
        p->pokes = 0;
        react(p, DB_REACT_EATING, 1200, now);
        break;
    case DB_INTERACT_CANCEL:
        p->snack = false;
        p->x = p->y = 0;
        react(p, DB_REACT_CALM, 0, now);
        break;
    case DB_INTERACT_REST:
        p->snack = false;
        p->pokes = 0;
        react(p, DB_REACT_DROWSY, 900, now);
        break;
    case DB_INTERACT_WAKE:
        p->snack = false;
        p->pokes = 0;
        react(p, DB_REACT_WAKE, 240, now);
        break;
    }
}
void db_personality_tick(struct db_personality *p, int64_t now)
{
    if (now >= p->until_ms) {
        switch (p->reaction) {
        case DB_REACT_POKE:
        case DB_REACT_WAKE: react(p, DB_REACT_CURIOUS, 650, now); break;
        case DB_REACT_EATING: react(p, DB_REACT_HAPPY, 1000, now); break;
        case DB_REACT_DROWSY: react(p, DB_REACT_ASLEEP, 0, now); break;
        default: p->x = p->y = 0; react(p, DB_REACT_CALM, 0, now); break;
        }
    }
    if (p->snack && now - p->activity_ms >= 15000) {
        db_personality_event(p, DB_INTERACT_CANCEL, 0, 0, now);
    }
    if (p->reaction == DB_REACT_CALM && now - p->activity_ms >= DB_DROWSY_MS) {
        react(p, DB_REACT_DROWSY, DB_PERSONALITY_SLEEP_MS - (now - p->activity_ms), now);
        if (now - p->activity_ms >= DB_PERSONALITY_SLEEP_MS) react(p, DB_REACT_ASLEEP, 0, now);
    }
}
int64_t db_personality_next_ms(const struct db_personality *p)
{
    if (p->snack) return p->activity_ms + 15000;
    if (p->reaction == DB_REACT_CALM) return p->activity_ms + DB_DROWSY_MS;
    return p->until_ms;
}
void db_personality_pose(const struct db_personality *p, int64_t now, bool reduced, struct db_pose *o)
{
    memset(o, 0, sizeof(*o));
    o->override = p->reaction != DB_REACT_CALM;
    o->x = p->x;
    o->y = p->y;
    switch (p->reaction) {
    case DB_REACT_POKE:
    case DB_REACT_WAKE: o->expr = DB_EXPR_WIDE; o->mouth = 2; break;
    case DB_REACT_CURIOUS: o->expr = DB_EXPR_CURIOUS; break;
    case DB_REACT_ANNOYED: o->expr = DB_EXPR_SUSPICIOUS; break;
    case DB_REACT_HAPPY:
        o->expr = DB_EXPR_HAPPY; o->mouth = 1;
        if (!reduced) o->lift = -20 - (int)(p->variation % 3) * 8;
        break;
    case DB_REACT_EATING:
        o->expr = DB_EXPR_HAPPY;
        o->mouth = reduced || ((now - p->since_ms) / 200 % 2) ? 1 : 2;
        if (!reduced) o->lift = o->mouth == 2 ? 15 : -15;
        break;
    case DB_REACT_DROWSY: o->expr = DB_EXPR_SLEEPY; break;
    case DB_REACT_ASLEEP: o->expr = DB_EXPR_CLOSED; break;
    default: o->expr = DB_EXPR_OPEN; break;
    }
}
void db_gesture_begin(struct db_gesture *g, int x, int y, int64_t now)
{
    memset(g, 0, sizeof(*g));
    g->active = true;
    g->start_x = g->last_x = x;
    g->start_y = g->last_y = y;
    g->since_ms = now;
}
void db_gesture_move(struct db_gesture *g, int x, int y, bool inside)
{
    if (!g->active) return;
    if (!inside) { db_gesture_cancel(g); return; }
    g->path += abs(x - g->last_x) + abs(y - g->last_y);
    g->last_x = x; g->last_y = y;
    int extent = abs(x - g->start_x) + abs(y - g->start_y);
    if (extent > g->extent) g->extent = extent;
}
enum db_gesture_kind db_gesture_end(struct db_gesture *g, int64_t now)
{
    if (!g->active) return DB_GESTURE_NONE;
    g->active = false; /* Exactly one result per press. */
    int64_t dt = now - g->since_ms;
    int chord = abs(g->last_x - g->start_x) + abs(g->last_y - g->start_y);
    if (dt >= 0 && dt <= 500 && g->extent <= 16 && g->path <= 24) return DB_GESTURE_TAP;
    if (dt >= 350 && dt <= 2500 && chord >= 48 && chord * 3 >= g->path * 2)
        return DB_GESTURE_STROKE;
    return DB_GESTURE_NONE;
}
void db_gesture_cancel(struct db_gesture *g) { g->active = false; }
