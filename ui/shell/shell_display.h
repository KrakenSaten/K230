/*
 * The shell's display ownership: which panel this build drives, and which
 * orientation this run uses. The effective rotation is decided here once,
 * before the display exists, and the resulting geometry is the only one the
 * display, the touch transform and PocketUI ever see.
 *
 * Changing the orientation takes effect when the shell next starts: the
 * vendor DRM path sizes its framebuffers for the rotation when the display is
 * opened (LVGL patch 0002), and the vendor launcher itself restarts its
 * process to go between portrait and landscape. See
 * docs/hardware/DOORS_DISPLAY_GEOMETRY_GATE.md.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_SHELL_DISPLAY_H
#define POCKETOS_SHELL_DISPLAY_H

#include "kbd_presence.h"
#include "orientation.h"
#include "pos_display.h"

#include <stdbool.h>

/* The panel: the T-Display K230's RM69A10, 568x1232 native portrait, as
 * described in platform.h. Both backends describe the same panel so the
 * simulator lays out what the board shows. POCKETOS_SAFE_CORNERS=tl,tr,br,bl
 * (non-negative pixels) overrides the corner squares on the bench or in a
 * test, in both orientations; an invalid value logs a warning and the
 * default stays. The default here is portrait's (30 px at every corner);
 * shell_display_resolve raises the landscape top corners to 50 px once the
 * orientation is known (platform.h). Returns 1 when the override was used. */
int shell_display_panel(struct pos_panel *out);

struct shell_display {
    struct pos_panel panel;
    enum orientation_mode mode;     /* as stored, or as given on the command line */
    bool mode_valid;                /* false: the stored value was not a mode */
    bool mode_from_arg;             /* mode is --rotation's, which every restart keeps */
    enum kbd_presence keyboard;     /* at the moment the rotation was decided */
    bool bench_override;            /* POCKETOS_DRM_ROTATION decided instead */
    enum pos_rotation requested;    /* what the policy (or the override) asked for */
    /* The open app that runs in portrait only (app.h `orientation`), by its
     * id, or NULL: while one is set the policy is held at portrait
     * (orientation_hold). The shell sets it as such an app opens and clears
     * it as it closes; the bench override is never held. */
    const char *portrait_app;
    struct pos_display_geometry geometry; /* what this run uses, after the backend */
};

/* Decide this run's orientation: the mode from mode_arg (--rotation) or the
 * settings store, the keyboard's presence, the app this run opens with when
 * that app runs in portrait only (portrait_app, NULL for none), and
 * POCKETOS_DRM_ROTATION as a bench override that turns display and touch
 * together. Fills everything but the backend's verdict; geometry is the
 * requested one until the backend confirms or lowers it. */
void shell_display_resolve(const char *mode_arg, const char *portrait_app, struct shell_display *d);

/* The rotation the shell would open the display at if it started now: the
 * stored mode - or, in a run given --rotation, that mode, since a restart
 * in place keeps its arguments - with the keyboard as it is now and the
 * portrait-only app as it is now. Differs from geometry.rotation when the
 * stored mode, the keyboard or the open app changed since start, or while a
 * bench override is in force. */
enum pos_rotation shell_display_next_rotation(const struct shell_display *d);

#endif
