/*
 * DeskBuddy's eyes as numbers. See db_face.h.
 *
 * Proportions are per-mille of s, the smaller side of an eye's box, so the
 * face is the same face in portrait, landscape and any body size.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "db_face.h"

#include <string.h>

static int pm(int s, int permille)
{
    return s * permille / 1000;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void shape(enum db_expr expr, int s, bool right, struct db_eye_shape *e)
{
    memset(e, 0, sizeof(*e));
    switch (expr) {
    case DB_EXPR_CLOSED:
        e->w = pm(s, 720);
        e->h = clampi(pm(s, 80), 6, 24);
        break;
    case DB_EXPR_SLEEPY:
        /* Heavy lids: the lower part of the eye, the pupil sunk into it. */
        e->w = pm(s, 700);
        e->h = pm(s, 640);
        e->dy = pm(s, 60);
        e->lid = pm(e->h, 560);
        e->pupil_d = pm(s, 260);
        e->pupil_dy = pm(s, 160);
        break;
    case DB_EXPR_HAPPY:
        /* ^ ^ : an arch - the top of the eye, a disc cut away below. */
        e->w = pm(s, 700);
        e->h = pm(s, 520);
        e->dy = -pm(s, 60);
        e->arch_d = pm(e->w, 1400);
        e->arch_y = pm(e->h, 420);
        break;
    case DB_EXPR_SUSPICIOUS:
        /* ಠ ಠ : narrowed under a flat lid, the pupil pressed to it. */
        e->w = pm(s, 740);
        e->h = pm(s, 560);
        e->dy = pm(s, 60);
        e->lid = pm(e->h, 380);
        e->pupil_d = pm(s, 220);
        e->pupil_dy = -pm(s, 20);
        break;
    case DB_EXPR_CURIOUS:
        /* One eye a little bigger, both looking up: "hm?" */
        e->w = pm(s, right ? 640 : 700);
        e->h = pm(s, right ? 700 : 880);
        e->pupil_d = pm(s, 300);
        e->pupil_dy = -pm(s, 140);
        break;
    case DB_EXPR_WIDE:
        e->w = pm(s, 780);
        e->h = pm(s, 960);
        e->pupil_d = pm(s, 240);
        break;
    case DB_EXPR_OPEN:
    default:
        e->w = pm(s, 680);
        e->h = pm(s, 880);
        e->pupil_d = pm(s, 320);
        break;
    }
    e->radius = e->w < e->h ? e->w / 2 : e->h / 2;
}

void db_face_eyes(const struct db_face *f, int box_w, int box_h, struct db_eye_shape *left,
                  struct db_eye_shape *right)
{
    int s = box_w < box_h ? box_w : box_h;
    struct db_eye_shape *eyes[2] = { left, right };
    int k;

    if (s < 8) {
        s = 8;
    }
    for (k = 0; k < 2; k++) {
        struct db_eye_shape *e = eyes[k];
        int room_x;
        int room_y;

        shape(f->expr, s, k == 1, e);
        if (e->pupil_d > 0 && f->glance != 0) {
            e->pupil_dx = f->glance * pm(e->w, 220);
        }
        /* Keep the pupil inside its eye and the eye inside its box. */
        room_x = (e->w - e->pupil_d) / 2;
        room_y = (e->h - e->pupil_d) / 2;
        if (e->pupil_d > e->w || e->pupil_d > e->h) {
            e->pupil_d = 0;
        }
        if (e->pupil_d == 0) {
            e->pupil_dx = 0;
            e->pupil_dy = 0;
        } else {
            e->pupil_dx = clampi(e->pupil_dx, -room_x, room_x);
            e->pupil_dy = clampi(e->pupil_dy, -room_y, room_y);
        }
        e->dy = clampi(e->dy, -(box_h - e->h) / 2, (box_h - e->h) / 2);
    }
}

bool db_eye_shape_equal(const struct db_eye_shape *a, const struct db_eye_shape *b)
{
    return a->w == b->w && a->h == b->h && a->radius == b->radius && a->dy == b->dy &&
           a->pupil_d == b->pupil_d && a->pupil_dx == b->pupil_dx && a->pupil_dy == b->pupil_dy &&
           a->lid == b->lid && a->arch_d == b->arch_d && a->arch_y == b->arch_y;
}
