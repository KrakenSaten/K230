/*
 * Video's layout: the list and the player in both shapes on the reference
 * panel's bodies (portrait under the shell's header, landscape with none),
 * every control inside the body and the safe box, at least the touch minimum,
 * nothing overlapping, a picture box a slot can fill - then fullscreen, the
 * corners, and bodies too small to use.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "video_layout.h"
#include "video_proto.h"

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

static int inside(const struct video_rect *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    return r->x >= x0 && r->y >= y0 && r->x + r->w <= x1 && r->y + r->h <= y1;
}

static int overlap(const struct video_rect *a, const struct video_rect *b)
{
    return a->w > 0 && b->w > 0 && a->h > 0 && b->h > 0 && a->x < b->x + b->w &&
           b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static void player(const char *shape, int32_t w, int32_t h, int32_t il, int32_t it, int32_t ir,
                   int32_t ib, int landscape)
{
    struct video_layout l;
    const struct video_rect *all[] = { &l.box, &l.status, &l.slider, &l.elapsed, &l.duration,
                                       &l.btn_back, &l.btn_stop, &l.btn_play, &l.btn_full };
    const struct video_rect *touch[] = { &l.btn_back, &l.btn_stop, &l.btn_play, &l.btn_full };
    char name[160];
    int ok = 1;
    int i;
    int j;
    int r = video_layout_compute(&l, w, h, il, it, ir, ib, landscape, 0);

    snprintf(name, sizeof(name), "%s: the player fits", shape);
    check(name, r == 0);
    for (i = 0; i < 9; i++) {
        ok &= inside(all[i], il, it, w - ir, h - ib);
    }
    snprintf(name, sizeof(name), "%s: everything inside the safe box", shape);
    check(name, ok);
    ok = 1;
    for (i = 0; i < 4; i++) {
        ok &= touch[i]->w >= TOUCH_MIN && touch[i]->h >= TOUCH_MIN;
    }
    snprintf(name, sizeof(name), "%s: every button at least the touch minimum", shape);
    check(name, ok);
    ok = 1;
    for (i = 0; i < 9; i++) {
        for (j = i + 1; j < 9; j++) {
            if (overlap(all[i], all[j])) {
                printf("     %d overlaps %d\n", i, j);
                ok = 0;
            }
        }
    }
    snprintf(name, sizeof(name), "%s: nothing overlaps", shape);
    check(name, ok);
    snprintf(name, sizeof(name), "%s: the progress bar is at least the touch target's height", shape);
    check(name, l.slider.h >= 40 && l.slider.w >= 3 * l.slider.h);
    snprintf(name, sizeof(name), "%s: the frame is the largest thing", shape);
    check(name, (int64_t)l.box.w * l.box.h > (int64_t)l.status.w * l.status.h * 3);
    snprintf(name, sizeof(name), "%s: the times sit under the bar, elapsed left of duration", shape);
    check(name, l.elapsed.y >= l.slider.y + l.slider.h && l.elapsed.x < l.duration.x &&
                    l.elapsed.y == l.duration.y);
}

int main(void)
{
    struct video_layout l;

    /* The reference panel: 568 x 1232. Portrait keeps the shell's 72 px
     * header; landscape has none (NONE_LANDSCAPE). */
    player("portrait", 568, 1160, 0, 0, 0, 0, 0);
    player("landscape", 1232, 568, 0, 0, 0, 0, 1);
    player("portrait with corners", 568, 1160, 0, 0, 0, 30, 0);
    player("landscape with corners", 1232, 568, 30, 30, 30, 30, 1);

    video_layout_compute(&l, 1232, 568, 30, 30, 30, 30, 1, 0);
    check("landscape: the buttons are a column right of the frame",
          l.btn_back.x == l.btn_play.x && l.btn_play.x == l.btn_stop.x &&
              l.btn_stop.x == l.btn_full.x && l.btn_back.x > l.box.x + l.box.w &&
              l.btn_back.y < l.btn_play.y && l.btn_play.y < l.btn_stop.y &&
              l.btn_stop.y < l.btn_full.y);
    check("landscape: a 16:9 picture fitted in the frame fits a slot",
          (int64_t)l.box.w * (l.box.w * 9 / 16) <= VIDEO_VIEW_MAX_PIXELS || l.box.h * 16 / 9 <= l.box.w);
    video_layout_compute(&l, 568, 1160, 0, 0, 0, 0, 0, 0);
    check("portrait: four buttons in one row under the bar",
          l.btn_back.y == l.btn_stop.y && l.btn_stop.y == l.btn_play.y &&
              l.btn_play.y == l.btn_full.y && l.btn_back.x < l.btn_stop.x &&
              l.btn_stop.x < l.btn_play.x && l.btn_play.x < l.btn_full.x &&
              l.btn_back.y > l.slider.y);
    check("portrait: the frame is as wide as the content", l.box.w == 568 - 2 * VIDEO_LAYOUT_PAD);
    check("portrait: no back slab on the list (the shell's header has one)", l.back.w == 0);
    check("portrait: the list under its title row",
          l.list.y >= l.title.y + l.title.h && l.list.h > 600 && l.rescan.w >= TOUCH_MIN &&
              l.rescan.h >= TOUCH_MIN);

    video_layout_compute(&l, 1232, 568, 30, 30, 30, 30, 1, 0);
    check("landscape: the list has its own back slab, clear of the corner",
          l.back.w >= TOUCH_MIN && l.back.h >= TOUCH_MIN && l.back.x >= 30 && l.back.y >= 30);
    check("landscape: title between back and RESCAN",
          l.title.x >= l.back.x + l.back.w && l.title.x + l.title.w <= l.rescan.x &&
              l.rescan.x + l.rescan.w <= 1232 - 30);
    check("landscape: the list inside the safe box",
          inside(&l.list, 30, 30, 1232 - 30, 568 - 30) && l.list.h > 300);

    /* Fullscreen: the frame is the whole body, corners and all, and nothing
     * else is placed. */
    check("fullscreen landscape", video_layout_compute(&l, 1232, 568, 30, 30, 30, 30, 1, 1) == 0 &&
                                      l.box.x == 0 && l.box.y == 0 && l.box.w == 1232 &&
                                      l.box.h == 568 && l.btn_play.w == 0 && l.slider.w == 0 &&
                                      l.status.w == 0 && l.fullscreen);
    check("fullscreen portrait", video_layout_compute(&l, 568, 1160, 0, 0, 0, 0, 0, 1) == 0 &&
                                     l.box.w == 568 && l.box.h == 1160 && l.btn_back.w == 0);

    check("too small is refused", video_layout_compute(&l, 200, 150, 0, 0, 0, 0, 0, 0) != 0);
    check("too small landscape is refused", video_layout_compute(&l, 400, 200, 0, 0, 0, 0, 1, 0) != 0);
    check("an empty body is refused", video_layout_compute(&l, 0, 0, 0, 0, 0, 0, 1, 0) != 0);
    check("insets eating the body are refused",
          video_layout_compute(&l, 568, 1160, 250, 0, 250, 0, 0, 0) != 0);
    check("a refused layout still places nothing with a negative size",
          l.box.w >= 0 && l.box.h >= 0 && l.list.w >= 0 && l.status.h >= 0);

    printf("video_layout_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
