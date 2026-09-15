/*
 * The shell's display ownership. See shell_display.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "shell_display.h"
#include "platform.h"
#include "pocketlog/pocketlog.h"

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

void shell_display_panel(struct pos_panel *out)
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
        } else {
            LOG_WARN("POCKETOS_SAFE_CORNERS=%s is not four pixel counts tl,tr,br,bl of at most %d; "
                     "using %d", corners, POCKETOS_PANEL_W / 4, POCKETOS_PANEL_CORNER);
        }
    }
}
