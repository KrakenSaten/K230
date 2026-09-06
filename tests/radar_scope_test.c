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
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "radar_scope.h"
#include "pocketui.h"

#include <stdio.h>
#include <stdlib.h>

#define PANEL_W 568
#define PANEL_H 1232
#define SCOPE_SIZE 520

/* lv_atan2 is accurate to one degree and a pixel is up to a degree at the
 * shortest range tested, so allow two degrees; one pixel of the 258 px
 * radius is four permille of range, so allow ten. */
#define BEARING_TOL_DD 20
#define RANGE_TOL_PERMILLE 10

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
                             abs(range - ranges[r]) <= RANGE_TOL_PERMILLE);
        }
    }

    /* Direct taps: the pixel offsets a finger produces, without the forward
     * transform in the loop, so a mirrored inverse cannot hide. */
    for (b = 0; b < sizeof(taps) / sizeof(taps[0]); b++) {
        snprintf(label, sizeof(label), "tap %s is on the scope", taps[b].name);
        check(label, radar_scope_polar(scope, taps[b].dx, taps[b].dy, &bearing, &range) == 0);
        snprintf(label, sizeof(label), "tap %s reads bearing %d (got %d)", taps[b].name,
                 taps[b].bearing, bearing);
        check(label, abs(radar_bearing_delta(bearing, taps[b].bearing)) <= BEARING_TOL_DD);
    }

    /* Edges: the centre is bearing 0 range 0; outside the rim is rejected. */
    check("centre is range 0", radar_scope_polar(scope, 0, 0, &bearing, &range) == 0 && range == 0 && bearing == 0);
    check("outside the scope is rejected", radar_scope_polar(scope, SCOPE_SIZE, 0, &bearing, &range) == -1);
    check("just outside the rim is rejected", radar_scope_polar(scope, 0, -(SCOPE_SIZE / 2), &bearing, &range) == -1);

    printf("radar_scope_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
