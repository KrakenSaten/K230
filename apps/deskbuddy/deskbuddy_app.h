/*
 * DeskBuddy: what tests/deskbuddy_app_test.c may look at and do. The app
 * itself is registered with the shell as app_deskbuddy (deskbuddy_app.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef DESKBUDDY_APP_H
#define DESKBUDDY_APP_H

#include "db_brain.h"
#include "lvgl.h"

const struct db_brain *deskbuddy_app_brain(void *priv);
/* The app's one timer, NULL once destroyed; for lifetime checks. */
lv_timer_t *deskbuddy_app_timer(void *priv);
/* Hand the app a vision event as a provider would, and let it act on it. */
void deskbuddy_app_inject(void *priv, enum db_vision_kind kind);
/* Run the app's timer body now (what LVGL would do when it falls due). */
void deskbuddy_app_pump(void *priv);

lv_obj_t *deskbuddy_app_mode_button(void *priv, enum db_mode mode);
lv_obj_t *deskbuddy_app_action_button(void *priv);   /* ARM / DISARM / SEEN IT */
lv_obj_t *deskbuddy_app_settings_button(void *priv);
lv_obj_t *deskbuddy_app_settings_panel(void *priv);
lv_obj_t *deskbuddy_app_toggle(void *priv, enum db_pref pref);
lv_obj_t *deskbuddy_app_settings_done(void *priv);
lv_obj_t *deskbuddy_app_face(void *priv);
lv_obj_t *deskbuddy_app_caption(void *priv);
lv_obj_t *deskbuddy_app_clock(void *priv);
/* 0 or 1: the eye as drawn now (its accent "white"). */
lv_obj_t *deskbuddy_app_eye(void *priv, int index);
/* How many times the eyes were restyled: a face that did not change must
 * not repaint. */
unsigned deskbuddy_app_face_paints(void *priv);
/* How many times the timer has run: the wake-up budget. */
unsigned deskbuddy_app_steps(void *priv);

#endif
