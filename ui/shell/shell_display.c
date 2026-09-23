/*
 * The shell's display ownership. See shell_display.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "shell_display.h"
#include "platform.h"
#include "pocketlog/pocketlog.h"
#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Four comma-separated non-negative integers, each at most a quarter of the
 * panel's shorter side; -1 with out untouched otherwise. */
static int parse_corners(const char *s, int32_t limit, struct pos_corners *out)
{
    long v[4];
    const char *p = s;
    int n;

    for (n = 0; n < 4; n++) {
        char *end;

        errno = 0;
        v[n] = strtol(p, &end, 10);
        if (end == p || errno != 0 || v[n] < 0 || v[n] > limit) {
            return -1;
        }
        p = end;
        if (n < 3) {
            if (*p != ',') {
                return -1;
            }
            p++;
        }
    }
    if (*p != '\0') {
        return -1;
    }
    out->top_left = (int32_t)v[0];
    out->top_right = (int32_t)v[1];
    out->bottom_right = (int32_t)v[2];
    out->bottom_left = (int32_t)v[3];
    return 0;
}

int shell_display_panel(struct pos_panel *out)
{
    const char *corners = getenv("POCKETOS_SAFE_CORNERS");

    memset(out, 0, sizeof(*out));
    out->width = POCKETOS_PANEL_W;
    out->height = POCKETOS_PANEL_H;
    out->corners.top_left = POCKETOS_PANEL_CORNER;
    out->corners.top_right = POCKETOS_PANEL_CORNER;
    out->corners.bottom_right = POCKETOS_PANEL_CORNER;
    out->corners.bottom_left = POCKETOS_PANEL_CORNER;
    if (corners && *corners) {
        struct pos_corners c;

        if (parse_corners(corners, POCKETOS_PANEL_W / 4, &c) == 0) {
            out->corners = c;
            LOG_INFO("display: corner insets %d,%d,%d,%d (POCKETOS_SAFE_CORNERS)", (int)c.top_left,
                     (int)c.top_right, (int)c.bottom_right, (int)c.bottom_left);
            return 1;
        }
        LOG_WARN("POCKETOS_SAFE_CORNERS=%s is not four pixel counts tl,tr,br,bl of at most %d; "
                 "using the defaults", corners, POCKETOS_PANEL_W / 4);
    }
    return 0;
}

/* POCKETOS_DRM_ROTATION: 0, 90, 180 or 270, a bench override of the policy.
 * Returns 1 and sets *out when set and valid, 0 when unset, and logs and
 * returns 0 for anything else. */
static int bench_rotation(enum pos_rotation *out)
{
    const char *v = getenv("POCKETOS_DRM_ROTATION");
    char *end;
    long deg;

    if (!v || !*v) {
        return 0;
    }
    errno = 0;
    deg = strtol(v, &end, 10);
    if (errno != 0 || *end != '\0' || deg < 0 || deg > 270 || pos_rotation_from_degrees((int)deg, out) < 0) {
        LOG_WARN("POCKETOS_DRM_ROTATION=%s is not 0, 90, 180 or 270; ignored", v);
        return 0;
    }
    return 1;
}

void shell_display_resolve(const char *mode_arg, struct shell_display *d)
{
    const char *stored = settings_get(ORIENTATION_SETTING, NULL);
    const char *source = mode_arg ? "--rotation" : stored ? "stored" : "default";

    int corners_given;

    memset(d, 0, sizeof(*d));
    corners_given = shell_display_panel(&d->panel);
    /* The keyboard was probed before this (shell_kbd_probe), because the
     * display is rotated when it is opened: a keyboard noticed afterwards
     * would cost a restart on every boot with the base attached. */
    d->mode = orientation_mode_from_setting(mode_arg ? mode_arg : stored, &d->mode_valid);
    if (!d->mode_valid) {
        LOG_WARN("display: %s rotation mode \"%s\" is not automatic, portrait or landscape; using automatic",
                 source, mode_arg ? mode_arg : stored);
    }
    d->keyboard = kbd_presence_get();
    d->requested = orientation_resolve(d->mode, d->keyboard);
    if (bench_rotation(&d->requested)) {
        d->bench_override = true;
        LOG_WARN("display: rotation %d from POCKETOS_DRM_ROTATION overrides the %s mode; display and touch "
                 "both follow it", pos_rotation_degrees(d->requested), orientation_mode_name(d->mode));
    }
    /* Landscape's top corners are the native left ones, and they need more
     * than portrait does (platform.h). Decided here, with the rotation, and
     * never over corners the bench gave explicitly. */
    if (!corners_given && pos_rotation_is_landscape_of(d->requested, d->panel.width, d->panel.height)) {
        d->panel.corners.top_left = POCKETOS_PANEL_CORNER_LANDSCAPE_TOP;
        d->panel.corners.bottom_left = POCKETOS_PANEL_CORNER_LANDSCAPE_TOP;
    }
    pos_display_geometry_init(&d->geometry, &d->panel, d->requested);
    LOG_INFO("display: rotation mode %s (%s), keyboard %s: rotation %d, %dx%d, corners %d,%d,%d,%d",
             orientation_mode_name(d->mode), source, kbd_presence_name(d->keyboard),
             pos_rotation_degrees(d->requested), (int)d->geometry.width, (int)d->geometry.height,
             (int)d->panel.corners.top_left, (int)d->panel.corners.top_right,
             (int)d->panel.corners.bottom_right, (int)d->panel.corners.bottom_left);
}

enum pos_rotation shell_display_next_rotation(const struct shell_display *d)
{
    enum pos_rotation r;
    bool valid;

    if (d->bench_override) {
        return d->requested;
    }
    r = orientation_resolve(orientation_mode_from_setting(settings_get(ORIENTATION_SETTING, NULL), &valid),
                            kbd_presence_get());
    return r;
}
