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

/* PHOTOS: a usable target in the safe box, clear of what shares the
 * preview's screen (the picture, the status line, the shutter, the last
 * photo) and of TRY AGAIN, which takes the shutter's place (tall) or btn_a's
 * (wide). */
static void photos_button(const char *shape, const struct camera_layout *l, int32_t w, int32_t h,
                          int32_t il, int32_t it, int32_t ir, int32_t ib)
{
    char name[120];

    snprintf(name, sizeof(name), "%s: PHOTOS has a place, a usable target", shape);
    check(name, l->gallery.w >= CAMERA_PHOTOS_MIN_W && l->gallery.h >= TOUCH_MIN &&
                    inside(&l->gallery, il, it, w - ir, h - ib));
    snprintf(name, sizeof(name), "%s: clear of the preview's controls and TRY AGAIN", shape);
    check(name, !overlap(&l->gallery, &l->picture) && !overlap(&l->gallery, &l->status) &&
                    !overlap(&l->gallery, &l->shutter) && !overlap(&l->gallery, &l->last) &&
                    !overlap(&l->gallery, l->wide ? &l->btn_a : &l->shutter));
}

/* Every rect of one gallery view inside the safe box, the touch minimum, and
 * apart. */
static void gallery_view(const char *what, const struct camera_rect *const *r, int n, int32_t w,
                         int32_t h, int32_t il, int32_t it, int32_t ir, int32_t ib)
{
    char name[140];
    int ok = 1;
    int i;
    int j;

    for (i = 0; i < n; i++) {
        ok &= inside(r[i], il, it, w - ir, h - ib) && r[i]->w > 0 && r[i]->h > 0;
        for (j = i + 1; j < n; j++) {
            ok &= !overlap(r[i], r[j]);
        }
    }
    snprintf(name, sizeof(name), "%s: inside the safe box and apart", what);
    check(name, ok);
}

static void gallery(const char *shape, int32_t w, int32_t h, int32_t il, int32_t it, int32_t ir,
                    int32_t ib)
{
    struct camera_gallery_layout l;
    struct camera_rect cells[GALLERY_CELLS_MAX];
    const struct camera_rect *grid[GALLERY_CELLS_MAX + 5];
    const struct camera_rect *photo[] = { &l.photo, &l.info, &l.status, &l.p_newer, &l.p_older,
                                          &l.p_left, &l.p_middle, &l.p_right };
    const struct camera_rect *show[] = { &l.show_touch, &l.show_status };
    const struct camera_rect *buttons[] = { &l.newer, &l.older, &l.left, &l.middle, &l.p_newer,
                                            &l.p_older, &l.p_left, &l.p_middle, &l.p_right };
    char name[140];
    int n = 0;
    int i;
    int ok = 1;

    snprintf(name, sizeof(name), "gallery %s lays out", shape);
    check(name, camera_gallery_layout_compute(&l, w, h, il, it, ir, ib) == 0);
    snprintf(name, sizeof(name), "gallery %s: %d x %d cells of %d px, at most %d", shape, l.cols,
             l.rows, l.cell, GALLERY_CELLS_MAX);
    check(name, l.cols >= 2 && l.rows >= 1 && l.cols * l.rows <= GALLERY_CELLS_MAX &&
                    l.cell >= TOUCH_MIN && l.cell <= CAMERA_PICTURE_MAX);
    for (i = 0; i < l.cols * l.rows; i++) {
        cells[i] = camera_gallery_cell_rect(&l, i);
        grid[n++] = &cells[i];
    }
    grid[n++] = &l.status;
    grid[n++] = &l.newer;
    grid[n++] = &l.older;
    grid[n++] = &l.left;
    grid[n++] = &l.middle;
    snprintf(name, sizeof(name), "gallery %s grid", shape);
    gallery_view(name, grid, n, w, h, il, it, ir, ib);
    snprintf(name, sizeof(name), "gallery %s photo", shape);
    gallery_view(name, photo, 8, w, h, il, it, ir, ib);
    snprintf(name, sizeof(name), "gallery %s slideshow", shape);
    gallery_view(name, show, 2, w, h, il, it, ir, ib);
    for (i = 0; i < 9; i++) {
        ok &= buttons[i]->w >= TOUCH_MIN && buttons[i]->h >= TOUCH_MIN;
    }
    snprintf(name, sizeof(name), "gallery %s: every button at least %d px", shape, TOUCH_MIN);
    check(name, ok);
    snprintf(name, sizeof(name), "gallery %s: the slideshow's picture is inside what a tap stops it on",
             shape);
    check(name, l.show.x >= l.show_touch.x && l.show.y >= l.show_touch.y &&
                    l.show.x + l.show.w <= l.show_touch.x + l.show_touch.w &&
                    l.show.y + l.show.h <= l.show_touch.y + l.show_touch.h &&
                    l.show_touch.w >= l.show.w && l.show_touch.h >= l.show.h);
    snprintf(name, sizeof(name), "gallery %s: every picture box fits a slot", shape);
    check(name, l.photo.w <= CAMERA_PICTURE_MAX && l.photo.h <= CAMERA_PICTURE_MAX &&
                    l.show.w <= CAMERA_PICTURE_MAX && l.show.h <= CAMERA_PICTURE_MAX &&
                    l.cell <= CAMERA_PICTURE_MAX);
    snprintf(name, sizeof(name), "gallery %s: the photo gets most of the room, or a slot (%d%%)", shape,
             (int)((int64_t)l.photo.w * l.photo.h * 100 / ((int64_t)w * h)));
    check(name, (int64_t)l.photo.w * l.photo.h * 100 / ((int64_t)w * h) >= 45 ||
                    l.photo.w == CAMERA_PICTURE_MAX || l.photo.h == CAMERA_PICTURE_MAX);
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
    photos_button("tall", &l, 528, 1116, 0, 0, 0, 0);
    check("tall: PHOTOS right of the shutter, in its row",
          l.gallery.x >= l.shutter.x + l.shutter.w && l.gallery.y >= l.shutter.y &&
              l.gallery.y + l.gallery.h <= l.shutter.y + l.shutter.h);

    /* Landscape under NONE: 1192 x 452. */
    check("wide lays out", camera_layout_compute(&l, 1192, 452, 0, 0, 0, 0, 1920, 1080) == 0);
    check("wide is wide", l.wide);
    check("the picture takes the height", l.picture.h == 452 && l.picture.x == 0);
    check("the column is on the right", l.shutter.x > l.picture.x + l.picture.w &&
                                            l.status.x > l.picture.x + l.picture.w);
    check("the review buttons stack", l.btn_a.x == l.btn_b.x && l.btn_a.y + l.btn_a.h < l.btn_b.y);
    check("the last photo above the shutter", l.last.y + l.last.h <= l.shutter.y);
    common("wide", &l, 1192, 452, 0, 0, 0, 0, 1920, 1080);
    photos_button("wide", &l, 1192, 452, 0, 0, 0, 0);
    check("wide: PHOTOS under the shutter", l.gallery.y >= l.shutter.y + l.shutter.h);

    /* Corners reaching in. */
    check("tall with corners", camera_layout_compute(&l, 528, 1116, 10, 0, 10, 10, 1080, 1920) == 0);
    common("tall with corners", &l, 528, 1116, 10, 0, 10, 10, 1080, 1920);
    photos_button("tall with corners", &l, 528, 1116, 10, 0, 10, 10);
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

    check("a narrow tall body has no room for PHOTOS, and says so",
          camera_layout_compute(&l, 300, 900, 0, 0, 0, 0, 1080, 1920) == 0 && l.gallery.w == 0);

    /* The gallery: the reference bodies, corners, a huge body. */
    gallery("tall", 528, 1116, 0, 0, 0, 0);
    {
        struct camera_gallery_layout g;

        camera_gallery_layout_compute(&g, 528, 1116, 0, 0, 0, 0);
        check("gallery tall: three columns of five", g.cols == 3 && g.rows == 5 && g.cell == 170);
        check("gallery tall: the photo across the width, above its three lines",
              g.photo.w == 528 && g.photo.y + g.photo.h <= g.info.y);
        camera_gallery_layout_compute(&g, 1192, 452, 0, 0, 0, 0);
        check("gallery wide: five columns of two", g.cols == 5 && g.rows == 2);
        check("gallery wide: the photo at the full height, left of the column",
              g.photo.h == 452 && g.photo.x + g.photo.w < g.info.x);
        check("gallery: too small is refused", camera_gallery_layout_compute(&g, 200, 150, 0, 0, 0, 0) != 0);
        check("gallery: a wide body too narrow for its column is refused",
              camera_gallery_layout_compute(&g, 400, 300, 0, 0, 0, 0) != 0);
    }
    gallery("wide", 1192, 452, 0, 0, 0, 0);
    gallery("tall with corners", 528, 1116, 10, 0, 10, 10);
    gallery("wide with corners", 1192, 452, 10, 10, 10, 10);
    gallery("huge", 2000, 1200, 0, 0, 0, 0);
    gallery("short tall", 528, 800, 0, 0, 0, 0);

    check("too small is refused", camera_layout_compute(&l, 200, 100, 0, 0, 0, 0, 1920, 1080) != 0);
    check("a zero photo is refused", camera_layout_compute(&l, 528, 1116, 0, 0, 0, 0, 0, 0) != 0);
    check("insets eating the body are refused",
          camera_layout_compute(&l, 528, 1116, 300, 0, 300, 0, 1080, 1920) != 0);

    printf("camera_layout_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
