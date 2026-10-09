/*
 * DeskBuddy's eyes and mouth as numbers. See db_face.h.
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
    case DB_EXPR_SQUINT:
        /* "Hey." Lids half down, small pupils on whoever did it. */
        e->w = pm(s, 720);
        e->h = pm(s, 700);
        e->dy = pm(s, 40);
        e->lid = pm(e->h, 420);
        e->pupil_d = pm(s, 240);
        e->pupil_dy = pm(s, 60);
        break;
    case DB_EXPR_ANNOYED:
        /* A flat glare: wide, low lids, pinpoint pupils - a little lower
         * on the side it is turning away from. */
        e->w = pm(s, 780);
        e->h = pm(s, 560);
        e->dy = pm(s, 80);
        e->lid = pm(e->h, right ? 500 : 560);
        e->pupil_d = pm(s, 180);
        e->pupil_dy = pm(s, 90);
        break;
    case DB_EXPR_CONTENT:
        /* Petted, fed: soft half-lids, the pupils turned up. */
        e->w = pm(s, 700);
        e->h = pm(s, 700);
        e->dy = pm(s, 30);
        e->lid = pm(e->h, 440);
        e->pupil_d = pm(s, 280);
        e->pupil_dy = pm(s, 70);
        break;
    case DB_EXPR_WINK:
        /* The mischievous one: the right eye shut, the left open. */
        if (right) {
            e->w = pm(s, 700);
            e->h = clampi(pm(s, 80), 6, 24);
        } else {
            e->w = pm(s, 700);
            e->h = pm(s, 860);
            e->pupil_d = pm(s, 320);
        }
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
        if (e->pupil_d > 0 && f->look) {
            /* Looking at something: the pupils go where it is. Under a lid
             * the visible part of the eye is lower, so is its middle. */
            int lid_mid = e->lid / 2;
            int lx = clampi(f->look_x, -1000, 1000);
            int ly = clampi(f->look_y, -1000, 1000);
            int len = (lx < 0 ? -lx : lx) > (ly < 0 ? -ly : ly)
                          ? (lx < 0 ? -lx : lx) + (ly < 0 ? -ly : ly) / 2
                          : (ly < 0 ? -ly : ly) + (lx < 0 ? -lx : lx) / 2;

            /* Within a circle, not a square: a pupil looking into a corner
             * would leave the rounded eye. */
            if (len > 1000) {
                lx = lx * 1000 / len;
                ly = ly * 1000 / len;
            }
            /* 85 % of the way at most: a hard look stays off the rim. */
            e->pupil_dx = lx * room_x * 85 / 100000;
            e->pupil_dy = lid_mid + ly * (room_y - lid_mid) * 85 / 100000;
        }
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

/* ---- the mouth ------------------------------------------------------------------ */

static int half_min(int w, int h)
{
    return w < h ? w / 2 : h / 2;
}

/* A plain mouth: the body fills its box. Its empty cut-out waits in the
 * middle, so a hole tweened away closes where it was, not into a corner. */
static void filled(struct db_mouth_shape *m, int w, int h)
{
    m->w = m->body_w = w;
    m->h = m->body_h = h < 4 ? 4 : h;
    m->body_r = half_min(m->body_w, m->body_h);
    m->cut_x = m->body_w / 2;
    m->cut_y = m->body_h / 2;
}

/* A hole in the middle of the body. */
static void hole(struct db_mouth_shape *m, int w, int h)
{
    m->cut_w = w;
    m->cut_h = h;
    m->cut_x = (m->body_w - w) / 2;
    m->cut_y = (m->body_h - h) / 2;
    m->cut_r = half_min(w, h);
}

/* A crescent smile in a w x h box: the bottom of a disc of diameter d, with
 * a larger disc (cut_d) cut from it whose bottom is `thick` higher. The two
 * edges meet in points, so the tips taper inside a shallow box rather than
 * being cut off by it: for d 460, cut_d 700, thick 75 they meet 149 above
 * the bottom and 215 to each side. shift moves the disc sideways, so one
 * corner rises more than the other. */
static void crescent(struct db_mouth_shape *m, int w, int h, int d, int cut_d, int thick, int shift)
{
    m->w = w;
    m->h = h;
    m->body_w = m->body_h = d;
    m->body_r = d / 2;
    m->body_x = (w - d) / 2 + shift;
    m->body_y = h - d;
    m->cut_w = m->cut_h = cut_d;
    m->cut_r = cut_d / 2;
    m->cut_x = (d - cut_d) / 2;
    m->cut_y = d - thick - cut_d;
}

void db_face_mouth(const struct db_face *f, int box, struct db_mouth_shape *m)
{
    int s = box < 8 ? 8 : box;

    memset(m, 0, sizeof(*m));
    switch (f->mouth) {
    case DB_MOUTH_SMILE:
        crescent(m, pm(s, 440), pm(s, 150), pm(s, 460), pm(s, 700), pm(s, 75), 0);
        break;
    case DB_MOUTH_SMIRK:
        /* Lopsided: a smaller smile, the disc pushed left in a box pushed
         * right, so the right corner rises and the left one fades. */
        crescent(m, pm(s, 300), pm(s, 130), pm(s, 400), pm(s, 620), pm(s, 65), -pm(s, 60));
        m->dx = pm(s, 70);
        break;
    case DB_MOUTH_O:
        filled(m, pm(s, 170), pm(s, 170));
        hole(m, pm(s, 80), pm(s, 80));
        break;
    case DB_MOUTH_O_BIG:
        filled(m, pm(s, 220), pm(s, 260));
        hole(m, pm(s, 130), pm(s, 170));
        break;
    case DB_MOUTH_FLAT:
        filled(m, pm(s, 240), pm(s, 50));
        break;
    case DB_MOUTH_CHEW_OPEN:
        filled(m, pm(s, 260), pm(s, 150));
        hole(m, pm(s, 170), pm(s, 60));
        break;
    case DB_MOUTH_CHEW_SHUT:
        filled(m, pm(s, 240), pm(s, 65));
        break;
    case DB_MOUTH_NONE:
    default:
        break;
    }
}

bool db_mouth_shape_equal(const struct db_mouth_shape *a, const struct db_mouth_shape *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* ---- tweening --------------------------------------------------------------------- */

static int mix(int a, int b, int permille)
{
    return a + (b - a) * permille / 1000;
}

void db_eye_shape_mix(const struct db_eye_shape *a, const struct db_eye_shape *b, int permille,
                      struct db_eye_shape *out)
{
    struct db_eye_shape r;

    permille = clampi(permille, 0, 1000);
    r.w = mix(a->w, b->w, permille);
    r.h = mix(a->h, b->h, permille);
    r.dy = mix(a->dy, b->dy, permille);
    r.pupil_d = mix(a->pupil_d, b->pupil_d, permille);
    r.pupil_dx = mix(a->pupil_dx, b->pupil_dx, permille);
    r.pupil_dy = mix(a->pupil_dy, b->pupil_dy, permille);
    r.lid = mix(a->lid, b->lid, permille);
    r.arch_d = mix(a->arch_d, b->arch_d, permille);
    r.arch_y = mix(a->arch_y, b->arch_y, permille);
    /* The radius follows the mixed size, so a mid-way eye is as round as
     * either end is. */
    r.radius = r.w < r.h ? r.w / 2 : r.h / 2;
    *out = r;
}

void db_mouth_shape_mix(const struct db_mouth_shape *a, const struct db_mouth_shape *b, int permille,
                        struct db_mouth_shape *out)
{
    struct db_mouth_shape r;

    permille = clampi(permille, 0, 1000);
    r.w = mix(a->w, b->w, permille);
    r.h = mix(a->h, b->h, permille);
    r.dx = mix(a->dx, b->dx, permille);
    r.body_x = mix(a->body_x, b->body_x, permille);
    r.body_y = mix(a->body_y, b->body_y, permille);
    r.body_w = mix(a->body_w, b->body_w, permille);
    r.body_h = mix(a->body_h, b->body_h, permille);
    r.body_r = mix(a->body_r, b->body_r, permille);
    r.cut_x = mix(a->cut_x, b->cut_x, permille);
    r.cut_y = mix(a->cut_y, b->cut_y, permille);
    r.cut_w = mix(a->cut_w, b->cut_w, permille);
    r.cut_h = mix(a->cut_h, b->cut_h, permille);
    r.cut_r = mix(a->cut_r, b->cut_r, permille);
    *out = r;
}
