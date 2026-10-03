/*
 * The developer debug overlay (Settings > Developer, DS §52.6): one compact
 * line at the foot of every screen - CPU, memory, temperature, network and
 * radio traffic (overlay_model.h) - for watching what the device does while
 * using it.
 *
 * Off by default, stored as `debug_overlay=0|1` in settings.conf, so it
 * comes back after a restart, a rotation or a reboot exactly as it was left.
 * While it is off nothing of it exists: no object, no timer, no request.
 * While it is on: one label on the top layer that takes no touch, and one
 * timer that every OVERLAY_REFRESH_MS asks sysd for system.status and, when
 * the status bar's own poll says radiod is answering, radiod for
 * radio.stats - two bounded requests (SHELL_IPC_UI_TIMEOUT_MS) every two
 * seconds. Its type is the caption's at Small whatever the text size, like
 * the status cluster's mark: it is a measuring tool, and a Large one would
 * cover what it measures.
 *
 * It sits centred at the foot of the content area - above the touch keyboard
 * when that is up - clear of the panel's rounded corners, and nowhere near
 * the status cluster or an app header, which are at the top.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef DOORS_SHELL_OVERLAY_H
#define DOORS_SHELL_OVERLAY_H

#include "lvgl.h"

#include <stdbool.h>

#define OVERLAY_SETTING "debug_overlay"
#define OVERLAY_REFRESH_MS 2000

/* content: the shell's content area, which the overlay sits at the foot of.
 * Reads the stored choice and shows the overlay if it is on. */
void shell_overlay_init(lv_obj_t *content);
bool shell_overlay_enabled(void);
/* Persist and apply. Returns 0, or -1 when the choice could not be stored
 * (nothing changed). Setting what is already set does nothing. */
int shell_overlay_set_enabled(bool on);
/* The content area changed (the keyboard came up or went down). */
void shell_overlay_place(void);
/* The line on show ("" while off), and how many refreshes have run since
 * start: for shell.info and the tests. */
const char *shell_overlay_text(void);
unsigned shell_overlay_refreshes(void);
/* Whether its object and timer exist now (they must not while it is off). */
bool shell_overlay_alive(void);
/* The exit path: delete the timer before LVGL goes. */
void shell_overlay_shutdown(void);

#endif
