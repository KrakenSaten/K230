/*
 * The detection range (core/pocketvision/vision_range.c): the presets and
 * what each changes, the zoom window on every picture shape, the merge of
 * the zoom pass's boxes (cut by the window, the same object twice, groups,
 * capacity), and the range's own filter at its boundaries.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketvision/vision_range.h"
#include "pocketvision/vision_track.h"

#include <stdio.h>
#include <string.h>

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

static struct vision_det det(uint16_t cls, uint16_t conf, int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct vision_det d = { { x, y, w, h }, cls, conf };

    return d;
}

static int box_is(const struct vision_box *b, int32_t x, int32_t y, int32_t w, int32_t h)
{
    return b->x == x && b->y == y && b->w == w && b->h == h;
}

static void test_presets(void)
{
    struct vision_range_params n;
    struct vision_range_params near;
    struct vision_range_params far;

    vision_range_params(VISION_RANGE_NORMAL, &n);
    vision_range_params(VISION_RANGE_NEAR, &near);
    vision_range_params(VISION_RANGE_FAR, &far);
    check("NORMAL is the pipeline as it was: 0.35, every size, no zoom, the tracker's own defaults",
          n.conf_min == 350 && n.min_side_pm == 0 && !n.zoom && n.min_hits == VISION_TRACK_MIN_HITS &&
              n.max_misses == VISION_TRACK_MAX_MISSES);
    check("NEAR: NORMAL's threshold, a smallest box, let go sooner", near.conf_min == n.conf_min &&
                                                                         near.min_side_pm > 0 && !near.zoom &&
                                                                         near.min_hits == n.min_hits &&
                                                                         near.max_misses < n.max_misses);
    check("FAR: the same threshold as NORMAL - it does not lower it", far.conf_min == n.conf_min && far.min_side_pm == 0);
    check("FAR: a zoom pass, one more sighting to confirm, kept longer unseen",
          far.zoom && far.min_hits == n.min_hits + 1 && far.max_misses > n.max_misses);
    check("the words", strcmp(vision_range_word(VISION_RANGE_NEAR), "near") == 0 &&
                           strcmp(vision_range_word(VISION_RANGE_FAR), "far") == 0 &&
                           vision_range_parse("normal") == VISION_RANGE_NORMAL && vision_range_parse("NEAR") == -1 &&
                           vision_range_parse(NULL) == -1 && strcmp(vision_range_word(VISION_RANGES), "normal") == 0);
    vision_range_params(VISION_RANGES, &n);
    check("an unknown range is NORMAL", n.conf_min == 350 && !n.zoom && n.min_hits == 2);
}

static void test_window(void)
{
    struct vision_box w;

    check("landscape 640 x 360 into 320 x 320: the centre at the model's own size",
          vision_range_zoom_window(640, 360, 320, 320, &w) == 0 && box_is(&w, 160, 20, 320, 320));
    check("portrait 360 x 640 too", vision_range_zoom_window(360, 640, 320, 320, &w) == 0 && box_is(&w, 20, 160, 320, 320));
    check("a 1280 x 720 picture: the same size, centred", vision_range_zoom_window(1280, 720, 320, 320, &w) == 0 &&
                                                             box_is(&w, 480, 200, 320, 320));
    check("a picture lower than the model: the window its height, square",
          vision_range_zoom_window(640, 240, 320, 320, &w) == 0 && box_is(&w, 200, 0, 240, 240));
    check("a wide model on a narrow picture keeps the model's shape",
          vision_range_zoom_window(300, 640, 640, 320, &w) == 0 && w.w == 300 && w.h == 150 && w.x == 0 && w.y == 245);
    check("no window when the picture is no larger than the model",
          vision_range_zoom_window(320, 320, 320, 320, &w) == -1 && vision_range_zoom_window(300, 200, 320, 320, &w) == -1);
    check("nor for nonsense", vision_range_zoom_window(0, 360, 320, 320, &w) == -1 &&
                                  vision_range_zoom_window(640, 360, 0, 320, &w) == -1 &&
                                  vision_range_zoom_window(640, 360, 320, 320, NULL) == -1);
}

static void test_merge(void)
{
    struct vision_box win = { 160, 20, 320, 320 };
    struct vision_det full[8];
    struct vision_det zoom[4];
    uint8_t group[8] = { 0, 0, 1, 0, 0, 1, 0, 1 }; /* 2 car, 5 bus, 7 truck: one group */
    int n;

    full[0] = det(2, 800, 10, 10, 80, 60);
    zoom[0] = det(2, 500, 300, 150, 20, 14);
    n = vision_range_merge(full, 1, 8, zoom, 1, &win, 640, 360, NULL, 0);
    check("a small box found only by the zoom is added", n == 2 && full[1].box.x == 300 && full[1].conf == 500);

    full[0] = det(2, 800, 10, 10, 80, 60);
    zoom[0] = det(2, 900, 160, 100, 40, 30);
    zoom[1] = det(2, 900, 300, 20, 40, 30);
    zoom[2] = det(2, 900, 450, 100, 30, 30);
    zoom[3] = det(2, 900, 300, 320, 40, 20);
    n = vision_range_merge(full, 1, 8, zoom, 4, &win, 640, 360, NULL, 0);
    check("a zoom box on any inner edge of the window was cut by it, and is dropped", n == 1);

    zoom[0] = det(2, 900, 163, 100, 40, 30);
    n = vision_range_merge(full, 1, 8, zoom, 1, &win, 640, 360, NULL, 0);
    check("one clear of the edge by more than the margin stays", n == 2);

    {
        struct vision_box top = { 160, 0, 320, 320 };

        zoom[0] = det(2, 900, 300, 0, 40, 30);
        n = vision_range_merge(full, 1, 8, zoom, 1, &top, 640, 320, NULL, 0);
        check("a window edge that is the picture's own cuts nothing", n == 2);
    }

    full[0] = det(2, 400, 300, 150, 40, 30);
    zoom[0] = det(2, 700, 302, 151, 38, 30);
    n = vision_range_merge(full, 1, 8, zoom, 1, &win, 640, 360, NULL, 0);
    check("the same object, surer in the zoom: the zoom's box stands, once",
          n == 1 && full[0].conf == 700 && full[0].box.x == 302);

    full[0] = det(2, 800, 300, 150, 40, 30);
    zoom[0] = det(2, 700, 302, 151, 38, 30);
    n = vision_range_merge(full, 1, 8, zoom, 1, &win, 640, 360, NULL, 0);
    check("surer in the full picture: the full box stands, once", n == 1 && full[0].conf == 800 && full[0].box.x == 300);

    full[0] = det(2, 800, 300, 150, 60, 40);
    zoom[0] = det(2, 600, 305, 155, 20, 20);
    n = vision_range_merge(full, 1, 8, zoom, 1, &win, 640, 360, NULL, 0);
    check("a zoom box inside a full box of its class is that object: not added", n == 1 && full[0].box.w == 60);

    full[0] = det(2, 500, 300, 150, 40, 30);
    zoom[0] = det(7, 700, 301, 150, 40, 30);
    n = vision_range_merge(full, 1, 8, zoom, 1, &win, 640, 360, group, 8);
    check("a car and a truck on one spot, one group: one object", n == 1 && full[0].cls == 7);
    full[0] = det(2, 500, 300, 150, 40, 30);
    n = vision_range_merge(full, 1, 8, zoom, 1, &win, 640, 360, NULL, 0);
    check("without the groups: two", n == 2);
    full[0] = det(0, 500, 300, 150, 40, 30);
    n = vision_range_merge(full, 1, 8, zoom, 1, &win, 640, 360, group, 8);
    check("a person and a truck on one spot are two, groups or not", n == 2);

    full[0] = det(2, 800, 10, 10, 80, 60);
    full[1] = det(2, 800, 100, 10, 80, 60);
    zoom[0] = det(2, 900, 300, 150, 20, 14);
    n = vision_range_merge(full, 2, 2, zoom, 1, &win, 640, 360, NULL, 0);
    check("no room: the full pass's boxes are kept, the zoom's extra dropped", n == 2 && full[1].box.x == 100);
    zoom[0] = det(2, 900, 300, 150, 0, 14);
    n = vision_range_merge(full, 0, 8, zoom, 1, &win, 640, 360, NULL, 0);
    check("an empty zoom box is nothing", n == 0);
}

static void test_filter(void)
{
    struct vision_range_params near;
    struct vision_range_params normal;
    struct vision_det d[6];
    int n;

    vision_range_params(VISION_RANGE_NEAR, &near);
    vision_range_params(VISION_RANGE_NORMAL, &normal);
    /* 640 x 360: NEAR's smallest side is 360 x 0.125 = 45. */
    d[0] = det(2, 900, 0, 0, 45, 45);
    d[1] = det(2, 900, 0, 0, 44, 200);
    d[2] = det(2, 350, 0, 0, 100, 100);
    d[3] = det(2, 349, 0, 0, 100, 100);
    d[4] = det(2, 900, 0, 0, 200, 44);
    d[5] = det(2, 350, 0, 0, 10, 10);
    n = vision_range_filter(d, 6, &near, 640, 360);
    check("NEAR keeps 45 px from 0.35, drops 44 px (either side) and 0.349",
          n == 2 && d[0].box.w == 45 && d[1].conf == 350);
    d[0] = det(2, 900, 0, 0, 45, 45);
    d[1] = det(2, 900, 0, 0, 44, 200);
    d[2] = det(2, 350, 0, 0, 3, 3);
    d[3] = det(2, 349, 0, 0, 100, 100);
    n = vision_range_filter(d, 4, &normal, 640, 360);
    check("NORMAL keeps every size from 0.35", n == 3 && d[2].box.w == 3);
    d[0] = det(2, 900, 0, 0, 30, 30);
    n = vision_range_filter(d, 1, &near, 360, 200);
    check("NEAR's smallest side follows the picture: 25 px on a 200 px side", n == 1);
    check("nothing in, nothing out", vision_range_filter(d, 0, &near, 640, 360) == 0);
}

int main(void)
{
    test_presets();
    test_window();
    test_merge();
    test_filter();
    printf("vision_range_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
