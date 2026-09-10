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
#include "pos_input.h"
#include "pos_styles.h"

#include <stdbool.h>

/* Reference panel: 568x1232 portrait. Layout constants in pixels (DS §7). */
#define POCKETUI_STATUS_BAR_H 56
#define POCKETUI_HEADER_H 72
#define POCKETUI_TOUCH_MIN 64     /* C1: 64 until glove testing says otherwise */
#define POCKETUI_PAD 20
#define POCKETUI_BODY_PAD_TOP 24  /* DS §7: body top padding */
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

/* ---- Text field (DS §17.1) --------------------------------------------- */

/* The text-entry primitive. Single-line is POCKETUI_ROW_H tall and never
 * wraps; multi-line wraps, scrolls vertically and starts at three body lines.
 * placeholder may be NULL.
 *
 * Returns the LVGL text area, which is what an app reads and writes. It is
 * wrapped in a transparent full-width container so an error caption has
 * somewhere to go (pocketui_text_field_set_error); the container is the
 * returned object's parent, so a field drops into a column flex like any
 * other component.
 *
 * The field joins the focus group of pos_input.h, so a tap and a key focus it
 * the same way (DS §17.2), and it takes its characters from that one stream
 * whatever produced them (DS §17.4). It never talks to a keyboard. */
lv_obj_t *pocketui_text_field(lv_obj_t *parent, const char *placeholder, bool single_line);

/* Show or clear the error state; message NULL clears it. A message is
 * required when showing, because DS §2 forbids colour from carrying meaning
 * alone: the caption below the field is not optional. */
void pocketui_text_field_set_error(lv_obj_t *field, const char *message);

/* A disabled field takes no focus, shows no caret and cannot be edited. */
void pocketui_text_field_set_enabled(lv_obj_t *field, bool enabled);

/* Reduced motion (DS §12): the caret is drawn solid instead of blinking.
 * The shell sets this from the "reduced_motion" setting before it builds
 * anything; fields created afterwards follow it. */
void pocketui_set_reduced_motion(bool on);

#endif
