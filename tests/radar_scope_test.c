/*
 * PocketRadar scope pixel round trip: a bearing and range placed on the
 * scope by radar_scope_point() must come back from radar_scope_polar(),
 * and a tap in each cardinal direction must read as that direction. The
 * inverse used LVGL's lv_atan2 with its arguments swapped once, which the
 * polar-level engine tests could not see; this test drives the pixel path.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt next to the shell
 * (host builds only) and run by tests/radar_shell_test.sh. A display with
 * a no-op flush is enough: nothing is rendered.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "radar_scope.h"
#include "pocketui.h"

#include <stdio.h>
#include <stdlib.h>

#define PANEL_W 568
#define PANEL_H 1232
#define SCOPE_SIZE 520
/* What a landscape body gives it on the reference panel (DS 29.1). */
#define WIDE_SCOPE_SIZE 386

/* lv_atan2 is accurate to one degree and a pixel is up to a degree at the
 * shortest range tested, so allow two degrees. Range is carried by the radius
 * in pixels, so its tolerance is three of them - a larger share of the range
 * on a smaller scope, and so worked out from the size rather than fixed. */
#define BEARING_TOL_DD 20
#define RANGE_TOL_FOR(size) (3 * RADAR_RANGE_MAX / ((size) / 2))

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

int main(void)
{
    static uint8_t draw_buf[PANEL_W * 40 * 2];
    static const int bearings[] = { 0, 450, 900, 1350, 1800, 2250, 2700, 3150 };
    static const int ranges[] = { 250, 500, 950 };
    static const struct {
        int dx;
        int dy;
        int bearing;
        const char *name;
    } taps[] = {
        { 0, -100, 0, "up" },
        { 100, 0, 900, "right" },
        { 0, 100, 1800, "down" },
        { -100, 0, 2700, "left" },
        { 70, -70, 450, "up-right" },
        { -70, 70, 2250, "down-left" },
    };
    lv_display_t *disp;
    lv_obj_t *scope;
    size_t b;
    size_t r;
    char label[96];
    int bearing;
    int range;

    int pass;
    int size = SCOPE_SIZE;

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    pocketui_init();
    scope = radar_scope_create(lv_screen_active(), SCOPE_SIZE);
    check("scope created", scope != NULL);
    if (!scope) {
        printf("radar_scope_test: %d failure(s)\n", failed);
        return 1;
    }

    /* Everything below runs twice: at the size the scope is drawn down the
     * page, and at the size the landscape body gives it (DS 29.1). The
     * conversion reads the one geometry the object stores, so the second pass
     * is what proves a resized scope is hit where it is drawn. */
    for (pass = 0; pass < 2; pass++) {
    if (pass == 1) {
        size = WIDE_SCOPE_SIZE;
        radar_scope_set_size(scope, size);
        check("the scope resizes in place", radar_scope_size(scope) == size);
    }

    /* Round trip: 8 bearings x 3 ranges through the pixel grid and back. */
    for (b = 0; b < sizeof(bearings) / sizeof(bearings[0]); b++) {
        for (r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
            int dx = 0;
            int dy = 0;

            radar_scope_point(scope, bearings[b], ranges[r], &dx, &dy);
            snprintf(label, sizeof(label), "bearing %d range %d is on the scope", bearings[b], ranges[r]);
            check(label, radar_scope_polar(scope, dx, dy, &bearing, &range) == 0);
            snprintf(label, sizeof(label), "bearing %d range %d round trips (got %d, %d)",
                     bearings[b], ranges[r], bearing, range);
            check(label, abs(radar_bearing_delta(bearing, bearings[b])) <= BEARING_TOL_DD &&
                             abs(range - ranges[r]) <= RANGE_TOL_FOR(size));
        }
    }

    /* Direct taps: the pixel offsets a finger produces, without the forward
     * transform in the loop, so a mirrored inverse cannot hide. */
    for (b = 0; b < sizeof(taps) / sizeof(taps[0]); b++) {
        /* The table is written for the 520 px scope; at another size the same
         * fractions of the radius are the same directions. */
        int dx = taps[b].dx * size / SCOPE_SIZE;
        int dy = taps[b].dy * size / SCOPE_SIZE;

        snprintf(label, sizeof(label), "tap %s is on the %d px scope", taps[b].name, size);
        check(label, radar_scope_polar(scope, dx, dy, &bearing, &range) == 0);
        snprintf(label, sizeof(label), "tap %s on the %d px scope reads bearing %d (got %d)",
                 taps[b].name, size, taps[b].bearing, bearing);
        check(label, abs(radar_bearing_delta(bearing, taps[b].bearing)) <= BEARING_TOL_DD);
    }

    /* Edges: the centre is bearing 0 range 0; outside the rim is rejected. */
    check("centre is range 0", radar_scope_polar(scope, 0, 0, &bearing, &range) == 0 && range == 0 && bearing == 0);
    check("outside the scope is rejected", radar_scope_polar(scope, size, 0, &bearing, &range) == -1);
    check("just outside the rim is rejected", radar_scope_polar(scope, 0, -(size / 2), &bearing, &range) == -1);
    }

    /* And back again, because a layout turns both ways. */
    radar_scope_set_size(scope, SCOPE_SIZE);
    check("the scope resizes back", radar_scope_size(scope) == SCOPE_SIZE);
    check("and reads the middle of it as range 0",
          radar_scope_polar(scope, 0, 0, &bearing, &range) == 0 && range == 0);
    check("a size it already has changes nothing",
          (radar_scope_set_size(scope, SCOPE_SIZE), radar_scope_size(scope)) == SCOPE_SIZE);
    check("and nor does one that is not a size",
          (radar_scope_set_size(scope, 0), radar_scope_size(scope)) == SCOPE_SIZE);

    printf("radar_scope_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
