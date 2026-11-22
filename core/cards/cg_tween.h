/*
 * Card games: one object moving from a point to a point over a time, eased
 * out so it settles rather than stops. The caller owns the clock.
 *
 * Pure C: no LVGL, no I/O, no floating point, no clock.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CG_TWEEN_H
#define CG_TWEEN_H

#include <stdint.h>

struct cg_tween {
    int x0, y0;         /* from */
    int x1, y1;         /* to */
    uint32_t start_ms;  /* on the caller's clock */
    uint32_t dur_ms;    /* 0 moves at once */
};

/* Progress at `now_ms`, 0 before the start .. 1024 at the end and after,
 * eased out (cubic). */
int cg_tween_progress(const struct cg_tween *t, uint32_t now_ms);
/* Where the object is at `now_ms`. Returns 1 once it has arrived. */
int cg_tween_at(const struct cg_tween *t, uint32_t now_ms, int *x, int *y);

#endif
