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
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
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

/* An external display (HDMI, docs/hardware/HDMI_OUTPUT.md): the board booted
 * its HDMI tree, so the shell draws on a monitor and the panel is dark. A
 * monitor is upright and square-cornered whichever way the unit itself is
 * held, so the display runs at rotation 0 with no corner insets, the
 * orientation mode and the keyboard do not turn it, and a portrait-only app
 * is not held (there is no portrait to turn the monitor to). The unit's own
 * hold - which way up the camera sees the world - is still the mode's and
 * the keyboard's (shell_display_hold_rotation).
 *
 * Detected by the backend (pocketos_platform_external_display) before the
 * display is opened. POCKETOS_EXTERNAL_DISPLAY overrides that: "0" never,
 * "1" always, or WxH always with that size - which is also the size the
 * simulator's window takes, since it has no monitor to ask. Until the
 * backend reports the monitor's own mode, the size is SHELL_EXTERNAL_W x
 * SHELL_EXTERNAL_H. POCKETOS_DRM_ROTATION still turns an external display
 * (a monitor stood on its side). */
#define SHELL_EXTERNAL_DISPLAY_ENV "POCKETOS_EXTERNAL_DISPLAY"
#define SHELL_EXTERNAL_W 1920
#define SHELL_EXTERNAL_H 1080

struct shell_display {
    struct pos_panel panel;         /* the panel, or the external display's size */
    bool external;                  /* an external display, not the panel (above) */
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
 * bench override is in force. On an external display it is always the
 * rotation this run opened with: nothing turns a monitor. */
enum pos_rotation shell_display_next_rotation(const struct shell_display *d);

/* The rotation of the unit as it is held, in the panel's terms: which way
 * up its camera and its keyboard are. On the panel it is the display's own
 * rotation, as it always was. On an external display it is the rotation the
 * panel would be given now by the mode and the keyboard, so a unit on its
 * keyboard base still counts as landscape while the monitor shows the
 * picture at rotation 0. */
enum pos_rotation shell_display_hold_rotation(const struct shell_display *d);

/* Whether the unit is held in landscape: the hold rotation against the
 * panel's own native shape (portrait), not the monitor's. */
bool shell_display_hold_landscape(const struct shell_display *d);

/* After the backend has opened the display: the external display's size is
 * the mode it actually came up in, so landscape and portrait are judged
 * against the monitor rather than against the size guessed beforehand. No
 * change on the panel. */
void shell_display_opened(struct shell_display *d);

#endif
