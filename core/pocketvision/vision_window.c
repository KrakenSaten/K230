/*
 * Traffic's recent statistics. See vision_window.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_window.h"

#include <string.h>

void vision_window_init(struct vision_window *w)
{
    memset(w, 0, sizeof(*w));
}

static void push(struct vision_window *w, const struct vision_window_event *e)
{
    if (w->count == VISION_WINDOW_EVENTS) {
        /* Full: the oldest goes. It may still have been inside the window,
         * so the summary no longer covers all of it. */
        w->dropped_t = w->ev[w->head].t_ms;
        w->head = (w->head + 1) % VISION_WINDOW_EVENTS;
        w->count--;
        w->saturated = true;
    }
    w->ev[(w->head + w->count) % VISION_WINDOW_EVENTS] = *e;
    w->count++;
}

void vision_window_crossed(struct vision_window *w, const struct vision_traffic *tf,
                           const struct vision_crossing *x, int n, int64_t now_ms)
{
    int i;

    for (i = 0; i < n; i++) {
        struct vision_window_event e = { now_ms, 0, -1, x[i].dir > 0 ? 1 : -1 };

        e.cls = (int8_t)vision_traffic_class(tf, x[i].cls);
        push(w, &e);
    }
}

void vision_window_speed(struct vision_window *w, uint32_t kmh10, int64_t now_ms)
{
    struct vision_window_event e = { now_ms, kmh10, -1, 0 };

    if (kmh10 > 0) {
        push(w, &e);
    }
}

bool vision_window_summary(struct vision_window *w, int64_t now_ms, struct vision_window_summary *s)
{
    bool forgot = false;
    uint64_t sum = 0;
    uint32_t i;

    while (w->count > 0 && now_ms - w->ev[w->head].t_ms > VISION_WINDOW_MS) {
        w->head = (w->head + 1) % VISION_WINDOW_EVENTS;
        w->count--;
        forgot = true;
    }
    if (w->saturated && now_ms - w->dropped_t > VISION_WINDOW_MS) {
        /* What was pushed out would have aged out by now: whole again. */
        w->saturated = false;
        forgot = true;
    }
    memset(s, 0, sizeof(*s));
    s->window_s = VISION_WINDOW_MS / 1000;
    s->saturated = w->saturated;
    for (i = 0; i < w->count; i++) {
        const struct vision_window_event *e = &w->ev[(w->head + i) % VISION_WINDOW_EVENTS];

        if (e->kmh10 > 0) {
            s->speeds++;
            sum += e->kmh10;
            continue;
        }
        s->crossed++;
        if (e->dir > 0) {
            s->ab++;
        } else {
            s->ba++;
        }
        if (e->cls >= 0 && e->cls < VISION_TRAFFIC_CLASSES) {
            s->cls[e->cls]++;
        }
    }
    s->mean_kmh10 = s->speeds ? (uint32_t)(sum / s->speeds) : 0;
    return forgot;
}
