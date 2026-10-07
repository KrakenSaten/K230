/*
 * The shell's alarm alert: one full-panel screen, owned by the shell, that
 * appears over whatever is running when the clock runtime says something is
 * ringing.
 *
 * It is built the way the touch keyboard is built (DS §17.4): created once
 * on the screen rather than inside an app, hidden by default, and never
 * rebuilt. That is what lets an alarm interrupt the launcher, PocketNotes or
 * PocketClock itself with one piece of code and one visual answer.
 *
 * PocketClock has no ringing screen of its own. There is one alert, and this
 * is it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_SHELL_ALARM_H
#define POCKETOS_SHELL_ALARM_H

#include "lvgl.h"

/* Build it hidden on parent, and register with the clock runtime so a ring
 * puts it up without waiting for the next tick. */
void shell_alarm_create(lv_obj_t *parent);
/* Show, hide and re-label from the runtime's state. Safe to call at any
 * time; it is what the runtime's ring-change callback does. */
void shell_alarm_sync(void);
int shell_alarm_visible(void);

#endif
