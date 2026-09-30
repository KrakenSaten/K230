/*
 * Terminal: the app, for its tests.
 *
 * The app is struct pocketos_app app_terminal (terminal_app.c). These give a
 * test what it cannot see through LVGL: the session behind the screen, and
 * how far the view is scrolled back. Nothing in the shell calls them.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef TERMINAL_APP_H
#define TERMINAL_APP_H

#include "term_session.h"

#include "lvgl.h"

/* The app's timer period. */
#define TERMINAL_TICK_MS 30

const struct term_session *terminal_app_session(void *priv);
int terminal_app_view_back(void *priv);
/* Whether the next tick will measure the grid again (a layout still owed). */
bool terminal_app_layout_pending(void *priv);
/* The object that draws the grid and takes the keys. */
lv_obj_t *terminal_app_grid(void *priv);
/* One timer tick, now, for tests that step time themselves. */
void terminal_app_tick_now(void *priv);

#endif
