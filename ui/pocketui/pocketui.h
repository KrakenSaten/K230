/*
 * PocketUI: the shared widgets the shell and in-process apps use, built on
 * the Design System v0.1 role styles (pos_styles.h) and tokens (pos_theme.h).
 * Components never name colours or fonts; they add role styles.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETUI_H
#define POCKETUI_H

#include "lvgl.h"
#include "pos_styles.h"

/* Reference panel: 568x1232 portrait. Layout constants in pixels (DS §7). */
#define POCKETUI_STATUS_BAR_H 56
#define POCKETUI_HEADER_H 72
#define POCKETUI_TOUCH_MIN 64     /* C1: 64 until glove testing says otherwise */
#define POCKETUI_PAD 20
#define POCKETUI_RADIUS 6
#define POCKETUI_ROW_H 64
#define POCKETUI_TILE_H 150

/* Initialise styles from the current theme. Call once after lv_init(). */
void pocketui_init(void);
/* Apply the screen role to a screen object. */
void pocketui_style_screen(lv_obj_t *screen);

/* Panel: hairline-bordered container with 20 px padding, vertical flex. */
lv_obj_t *pocketui_card(lv_obj_t *parent);
/* Launcher tile (C7): slab, symbol icon top-left, row-title label bottom-left. */
lv_obj_t *pocketui_tile(lv_obj_t *parent, const char *icon, const char *label,
                        lv_event_cb_t on_click, void *user_data);
/* Key/value row: secondary label left, value right; returns the value label. */
lv_obj_t *pocketui_kv_row(lv_obj_t *parent, const char *key, const char *value);
/* Primary button, full width of parent, 64 px tall. */
lv_obj_t *pocketui_button(lv_obj_t *parent, const char *text, lv_event_cb_t on_click,
                          void *user_data);
/* Plain label with a role style (text, caption, value, status colours). */
lv_obj_t *pocketui_label(lv_obj_t *parent, const char *text, enum pos_style_role role);

#endif
