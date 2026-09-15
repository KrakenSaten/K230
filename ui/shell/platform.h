/*
 * Display and input backend for the shell: SDL window on a PC, DRM plus
 * evdev on the K230. Selected at build time (POCKETOS_DISPLAY).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_PLATFORM_H
#define POCKETOS_PLATFORM_H

#include "lvgl.h"
#include "pos_display.h"

/* The T-Display K230's RM69A10 panel, native portrait. */
#define POCKETOS_PANEL_W 568
#define POCKETOS_PANEL_H 1232
/* Its corners are visibly rounded; the straight edges show every pixel. No
 * datasheet or source in reach gives the corner radius or mask. 30 px is the
 * side inset the vendor launcher gives its own status bar on this panel
 * (k230_phone_ui main.c, STATUS_BAR_SAFE_SIDE), taken as the side of each
 * corner square (pos_display.h). PROVISIONAL: accepted or corrected on unit A
 * (docs/hardware/DOORS_DISPLAY_GEOMETRY_GATE.md), where POCKETOS_SAFE_CORNERS
 * can try another value without a rebuild. */
#define POCKETOS_PANEL_CORNER 30

/* Create the display and pointer input for one geometry. The display is
 * rotated to geometry->rotation and the touch transform is derived from the
 * same value (pos_display_touch_config), in one call, so the two cannot be
 * set apart. A backend that cannot rotate lowers geometry->rotation to 0 for
 * both, and one whose display comes up at a different size re-derives the
 * geometry from what it got; the caller lays out in *geometry afterwards.
 * Returns the display or NULL. */
lv_display_t *pocketos_platform_init(const struct pos_panel *panel, struct pos_display_geometry *geometry);
/* Milliseconds to sleep between lv_timer_handler calls. */
void pocketos_platform_sleep_ms(unsigned ms);

/* The host keyboard, if this backend has one - the SDL simulator does, the
 * panel does not. The shell adopts it as a source of the one logical key
 * stream (DS �17.4); the backend does not wire it up itself, so devices are
 * created after the display and the stream is built in one place. */
lv_indev_t *pocketos_platform_keyboard(void);

#endif
