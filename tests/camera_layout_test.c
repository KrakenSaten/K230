/*
 * Camera's layout: both shapes on the reference panel's bodies, the picture
 * keeping the photo's shape and fitting a shared-memory slot, every control
 * inside the body and the safe box, at least the touch minimum, and nothing
 * overlapping - then the corners, and bodies too small to use.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "camera_layout.h"

#include <stdio.h>
#include <stdlib.h>

#define TOUCH_MIN 64 /* POCKETUI_TOUCH_MIN */

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static int inside(const struct camera_rect *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    return r->x >= x0 && r->y >= y0 && r->x + r->w <= x1 && r->y + r->h <= y1;
}

static int overlap(const struct camera_rect *a, const struct camera_rect *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static void common(const char *shape, const struct camera_layout *l, int32_t w, int32_t h,
                   int32_t il, int32_t it, int32_t ir, int32_t ib, uint32_t pw, uint32_t ph)
{
    const struct camera_rect *touch[] = { &l->shutter, &l->last, &l->btn_a, &l->btn_b };
    const struct camera_rect *all[] = { &l->picture, &l->status, &l->shutter, &l->last,
                                        &l->btn_a, &l->btn_b };
    char name[120];
    int ok = 1;
    int i;
    int j;
    int64_t err;

    for (i = 0; i < 6; i++) {
        ok &= inside(all[i], il, it, w - ir, h - ib);
    }
    snprintf(name, sizeof(name), "%s: everything inside the safe box", shape);
    check(name, ok);
    ok = 1;
    for (i = 0; i < 4; i++) {
        ok &= touch[i]->w >= TOUCH_MIN && touch[i]->h >= TOUCH_MIN;
    }
    snprintf(name, sizeof(name), "%s: every control at least %d px", shape, TOUCH_MIN);
    check(name, ok);
    /* The controls of one screen never overlap each other or the picture;
     * btn_a/btn_b replace the shutter and the last photo, so they are only
     * compared with the picture, the status line and each other. */
    ok = !overlap(&l->shutter, &l->last) && !overlap(&l->btn_a, &l->btn_b);
    for (i = 1; i < 6; i++) {
        ok &= !overlap(all[i], &l->picture);
    }
    for (j = 2; j < 6; j++) {
        ok &= !overlap(all[j], &l->status);
    }
    snprintf(name, sizeof(name), "%s: nothing overlaps", shape);
    check(name, ok);
    err = (int64_t)l->picture.w * ph - (int64_t)l->picture.h * pw;
    snprintf(name, sizeof(name), "%s: the picture has the photo's shape (%dx%d)", shape,
             (int)l->picture.w, (int)l->picture.h);
    check(name, llabs(err) <= (int64_t)(pw > ph ? pw : ph));
    snprintf(name, sizeof(name), "%s: and fits a slot", shape);
    check(name, l->picture.w <= CAMERA_PICTURE_MAX && l->picture.h <= CAMERA_PICTURE_MAX);
}

int main(void)
{
    struct camera_layout l;

    /* Portrait under NONE: 528 x 1116 (DS section 30.8). */
    check("tall lays out", camera_layout_compute(&l, 528, 1116, 0, 0, 0, 0, 1080, 1920) == 0);
    check("tall is not wide", !l.wide);
    check("the picture takes the width", l.picture.w == 528 && l.picture.x == 0 && l.picture.y == 0);
    check("and dominates the screen", l.picture.h * 100 / 1116 >= 80);
    check("the shutter is centred under it", l.shutter.x + l.shutter.w / 2 == 264 &&
                                                  l.shutter.y > l.picture.y + l.picture.h);
    check("the last photo is left of it", l.last.x + l.last.w <= l.shutter.x);
    check("the review buttons share the shutter's row",
          l.btn_a.y == l.btn_b.y && l.btn_a.y >= l.shutter.y &&
              l.btn_a.y + l.btn_a.h <= l.shutter.y + l.shutter.h);
    common("tall", &l, 528, 1116, 0, 0, 0, 0, 1080, 1920);

    /* Landscape under NONE: 1192 x 452. */
    check("wide lays out", camera_layout_compute(&l, 1192, 452, 0, 0, 0, 0, 1920, 1080) == 0);
    check("wide is wide", l.wide);
    check("the picture takes the height", l.picture.h == 452 && l.picture.x == 0);
    check("the column is on the right", l.shutter.x > l.picture.x + l.picture.w &&
                                            l.status.x > l.picture.x + l.picture.w);
    check("the review buttons stack", l.btn_a.x == l.btn_b.x && l.btn_a.y + l.btn_a.h < l.btn_b.y);
    check("the last photo above the shutter", l.last.y + l.last.h <= l.shutter.y);
    common("wide", &l, 1192, 452, 0, 0, 0, 0, 1920, 1080);

    /* Corners reaching in. */
    check("tall with corners", camera_layout_compute(&l, 528, 1116, 10, 0, 10, 10, 1080, 1920) == 0);
    common("tall with corners", &l, 528, 1116, 10, 0, 10, 10, 1080, 1920);
    check("wide with corners", camera_layout_compute(&l, 1192, 452, 10, 10, 10, 10, 1920, 1080) == 0);
    common("wide with corners", &l, 1192, 452, 10, 10, 10, 10, 1920, 1080);

    /* A body shorter than the picture wants: the picture gives way. */
    check("a short tall body", camera_layout_compute(&l, 528, 800, 0, 0, 0, 0, 1080, 1920) == 0);
    common("short tall", &l, 528, 800, 0, 0, 0, 0, 1080, 1920);
    check("the controls keep their room", l.shutter.y + l.shutter.h <= 800);

    /* A body the full panel wide: the picture is capped at a slot. */
    check("a huge body", camera_layout_compute(&l, 2000, 1200, 0, 0, 0, 0, 1920, 1080) == 0);
    common("huge", &l, 2000, 1200, 0, 0, 0, 0, 1920, 1080);

    /* A square sensor, and a photo of another shape. */
    check("a 4:3 photo", camera_layout_compute(&l, 528, 1116, 0, 0, 0, 0, 1080, 1440) == 0);
    common("4:3", &l, 528, 1116, 0, 0, 0, 0, 1080, 1440);

    check("too small is refused", camera_layout_compute(&l, 200, 100, 0, 0, 0, 0, 1920, 1080) != 0);
    check("a zero photo is refused", camera_layout_compute(&l, 528, 1116, 0, 0, 0, 0, 0, 0) != 0);
    check("insets eating the body are refused",
          camera_layout_compute(&l, 528, 1116, 300, 0, 300, 0, 1080, 1920) != 0);

    printf("camera_layout_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
