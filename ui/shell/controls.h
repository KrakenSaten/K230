/*
 * DOORS Controls (DS §31.5): the package's system menu, over the home
 * photograph. Quick state and the few controls the shell itself owns, and a
 * way into the apps that own the rest. It adds no system function of its
 * own and shows only what a real provider answers:
 *
 *   Radio      radiod's state, as the status bar last polled it  -> Radio app
 *   Wi-Fi      netd's wifi.status                                 -> Settings
 *   Rotation   the stored orientation mode; a tap moves to the next
 *   Display    the display mode (Normal, Night, Outdoor); a tap moves on
 *   Brightness the panel's backlight, or "Not available" without one
 *   Settings, Mesh messages, About DOORS                          -> apps
 *   Lock       the lock screen;  Power -> the System app, whose power
 *              actions confirm before they act
 *
 * Bluetooth and sound are in the package's mock-up and not here: nothing
 * on the device provides them yet, and a tile for them could only pretend.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef DOORS_CONTROLS_H
#define DOORS_CONTROLS_H

#include "lvgl.h"

#include <stdbool.h>

struct controls_actions {
    void (*open_app)(const char *id);
    void (*lock)(void);
    void (*close)(void);
};

/* Build (hidden) under parent, the shell's content area at launcher size. */
lv_obj_t *controls_create(lv_obj_t *parent, bool landscape, const struct controls_actions *actions);
void controls_show(void);
void controls_hide(void);
bool controls_visible(void);
/* Once a second from the shell's tick: refresh what is shown, while shown. */
void controls_tick(void);

#endif
