/*
 * Card games: tweens. See cg_tween.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cg_tween.h"

int cg_tween_progress(const struct cg_tween *t, uint32_t now_ms)
{
    int64_t left;

    if (!t || now_ms < t->start_ms) {
        return 0;
    }
    if (t->dur_ms == 0 || now_ms - t->start_ms >= t->dur_ms) {
        return 1024;
    }
    /* 1 - (1 - u)^3 with u in 1/1024ths. */
    left = 1024 - ((int64_t)(now_ms - t->start_ms) * 1024) / t->dur_ms;
    return (int)(1024 - ((left * left * left) >> 20));
}

int cg_tween_at(const struct cg_tween *t, uint32_t now_ms, int *x, int *y)
{
    int p = cg_tween_progress(t, now_ms);

    if (!t) {
        return 1;
    }
    if (x) {
        *x = t->x0 + ((t->x1 - t->x0) * p) / 1024;
    }
    if (y) {
        *y = t->y0 + ((t->y1 - t->y0) * p) / 1024;
    }
    return p >= 1024;
}
