/*
 * Terminal: the app, for its tests.
 *
 * The app is struct pocketos_app app_terminal (terminal_app.c). These give a
 * test what it cannot see through LVGL: the session behind the screen (which
 * outlives the screen: leaving the app keeps it, CLOSE SESSION ends it), and
 * how far the view is scrolled back. Nothing in the shell calls them.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef TERMINAL_APP_H
#define TERMINAL_APP_H

#include "term_session.h"

#include "lvgl.h"

/* The session's timer period. */
#define TERMINAL_TICK_MS 30

/* The session, or NULL when there is none (never opened, or closed). It is
 * the same whether or not a screen shows it; priv is not needed. */
const struct term_session *terminal_app_session(void *priv);
int terminal_app_view_back(void *priv);
/* Whether the next tick will measure the grid again (a layout still owed). */
bool terminal_app_layout_pending(void *priv);
/* The object that draws the grid and takes the keys. */
lv_obj_t *terminal_app_grid(void *priv);
/* The session row's CLOSE SESSION, and the confirmation's two buttons
 * (close: CLOSE SESSION, else CANCEL); whether the confirmation is up. */
lv_obj_t *terminal_app_close_button(void *priv);
lv_obj_t *terminal_app_confirm_button(void *priv, bool close);
bool terminal_app_confirming(void *priv);
/* What the session row says. */
const char *terminal_app_bar_text(void *priv);
/* The grid's cell in pixels, in the face of the text size in force. */
void terminal_app_cell(void *priv, int32_t *w, int32_t *h);
/* Sessions started since the process began: a reopen that kept the session
 * does not count. */
unsigned terminal_app_sessions_started(void);
/* Whether the session's timer exists and is not paused. */
bool terminal_app_timer_running(void);
/* One timer tick, now, for tests that step time themselves. */
void terminal_app_tick_now(void *priv);

#endif
