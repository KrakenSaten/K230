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

#endif
