/*
 * Display and input backend for the shell: SDL window on a PC, DRM plus
 * evdev on the K230. Selected at build time (POCKETOS_DISPLAY).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_PLATFORM_H
#define POCKETOS_PLATFORM_H

#include "lvgl.h"

#define POCKETOS_PANEL_W 568
#define POCKETOS_PANEL_H 1232

/* Create the display and pointer input. Returns the display or NULL. */
lv_display_t *pocketos_platform_init(void);
/* Milliseconds to sleep between lv_timer_handler calls. */
void pocketos_platform_sleep_ms(unsigned ms);

/* The host keyboard, if this backend has one - the SDL simulator does, the
 * panel does not. The shell adopts it as a source of the one logical key
 * stream (DS §17.4); the backend does not wire it up itself, so devices are
 * created after the display and the stream is built in one place. */
lv_indev_t *pocketos_platform_keyboard(void);

#endif
