/*
 * Traffic's recent statistics: what crossed the count line, and the speeds
 * measured, over the last VISION_WINDOW_MS (pocketvision.h).
 *
 * A fixed ring of VISION_WINDOW_EVENTS events: a crossing (its traffic class
 * and direction) or a speed measurement. Events older than the window are
 * forgotten as time passes; when more than the ring holds happen inside the
 * window, the oldest go first and the summary says it is `saturated` - it
 * then covers less than the whole window, and says so rather than guessing.
 * Nothing grows with time or traffic.
 *
 * The clock is the caller's (monotonic ms), passed in. Pure C, integer, no
 * allocation (tests/vision_window_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VISION_WINDOW_H
#define POCKETOS_VISION_WINDOW_H

#include "vision_traffic.h"

#define VISION_WINDOW_MS (5 * 60 * 1000)
#define VISION_WINDOW_EVENTS 512

struct vision_window_event {
    int64_t t_ms;
    uint32_t kmh10;   /* a speed measurement; 0 for a crossing */
    int8_t cls;       /* a crossing's traffic class, or -1 */
    int8_t dir;       /* a crossing's direction: +1 A to B (IN), -1 B to A (OUT) */
};

struct vision_window {
    struct vision_window_event ev[VISION_WINDOW_EVENTS];
    uint32_t head;     /* the oldest */
    uint32_t count;
    bool saturated;    /* an event still inside the window was pushed out */
    int64_t dropped_t; /* when the newest pushed-out event happened */
};

struct vision_window_summary {
    uint32_t window_s;          /* the window, in seconds */
    uint32_t crossed;           /* crossings in the window */
    uint32_t ab;                /* ... IN */
    uint32_t ba;                /* ... OUT */
    uint32_t cls[VISION_TRAFFIC_CLASSES];
    uint32_t speeds;            /* speed measurements in the window */
    uint32_t mean_kmh10;        /* their mean, 0 with none */
    bool saturated;
};

void vision_window_init(struct vision_window *w);
/* The count line's crossings at now_ms, classed as vision_traffic does. */
void vision_window_crossed(struct vision_window *w, const struct vision_traffic *tf,
                           const struct vision_crossing *x, int n, int64_t now_ms);
/* A speed measured at now_ms. 0 is not a measurement and is ignored. */
void vision_window_speed(struct vision_window *w, uint32_t kmh10, int64_t now_ms);
/* Forget what has left the window; then sum what is in it. Returns true when
 * something was forgotten (the summary changed without a new event). */
bool vision_window_summary(struct vision_window *w, int64_t now_ms, struct vision_window_summary *s);

#endif
