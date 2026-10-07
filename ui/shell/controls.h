/*
 * DOORS Controls (DS §31.5): the package's system menu, over the home
 * photograph. Quick state and the few controls the shell itself owns, and a
 * way into the apps that own the rest. It adds no system function of its
 * own and shows only what a real provider answers:
 *
 *   Wi-Fi       netd's wifi.status                                -> Settings
 *   Bluetooth   sysd's system.status.bluetooth; read-only: no controller on
 *               unit A and nothing in Doors switches one
 *   LoRa radio  radiod's state; a tap switches it (radio.set_enabled), and
 *               off -> on asks about the antenna first
 *   Battery     sysd's system.status.power; read-only
 *   Rotation    the stored orientation mode; a tap moves to the next
 *   Display     the display mode (Normal, Night, Outdoor); a tap moves on
 *   Brightness  the panel's backlight, or "Not available" without one
 *   Volume      the system volume (volume.h); the speaker glyph mutes
 *   Settings, Mesh messages, About DOORS                            -> apps
 *   Lock        the lock screen;  Power -> the System app, whose power
 *               actions confirm before they act
 *
 * What each says, the antenna question and the layout are decided in
 * controls_model.c, which is tested on the host; this file draws them.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_CONTROLS_H
#define DOORS_CONTROLS_H

#include "controls_model.h"
#include "lvgl.h"

#include <stdbool.h>

struct controls_actions {
    void (*open_app)(const char *id);
    void (*lock)(void);
    void (*close)(void);
};

/* Build (hidden) under parent, the shell's content area at launcher size.
 * keepout is the status cluster's widest box in parent's coordinates (DS
 * §36), which nothing of Controls is placed over; NULL: none. */
lv_obj_t *controls_create(lv_obj_t *parent, bool landscape, const struct controls_rect *keepout,
                          const struct controls_actions *actions);
/* Delete what controls_create built, so it can be built again (the shell
 * does when the text size changes, DS §46). Hidden or not; safe to repeat. */
void controls_destroy(void);
void controls_show(void);
void controls_hide(void);
bool controls_visible(void);
/* Once a second from the shell's tick: refresh what is shown, while shown. */
void controls_tick(void);

#endif
