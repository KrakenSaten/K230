/*
 * PocketFleet local widgets: compositions of the PocketUI role styles that
 * the app needs and PocketUI does not provide yet (titled panel, segmented
 * control, secondary button, disabled state, plain rows).
 *
 * These add existing role styles to plain LVGL objects. Nothing here defines
 * a colour or a font, so the app follows the current PocketOS theme and mode
 * exactly like the shell does, and tests/style_lint.sh stays green.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_WIDGETS_H
#define POCKETFLEET_WIDGETS_H

#include "lvgl.h"
#include "pocketui.h"

/* How far a panel's caption rises above the panel's top border, so a layout
 * that puts a panel at the very top of a clipping box can leave room for it.
 * Half a caption line, which is what centres the caption on the border: 9 px
 * at Small (mono 14), more at the larger text sizes (DS §46), so it is
 * measured rather than a number. */
int32_t fleet_caption_rise(void);
#define FLEET_CAPTION_RISE fleet_caption_rise()

/* Panel with the Design System caption set into its top border (DS §2).
 * title may be NULL for an untitled panel; it is shown as given, so pass
 * upper case. */
lv_obj_t *fleet_panel(lv_obj_t *parent, const char *title);
/* A panel whose rows run edge to edge (DS §7 list panel: padding 0 20). */
lv_obj_t *fleet_list_panel(lv_obj_t *parent, const char *title);

/* Full-width secondary button, 64 px (DS §9). */
lv_obj_t *fleet_button_secondary(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                                 void *user);
/* Half-width paired button inside a panel, 56 px (DS §7). Put two in a row. */
lv_obj_t *fleet_button_paired(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                              void *user);
/* Enable or disable a button made by pocketui_button(): swaps the primary
 * role for the Design System's disabled role and takes away the click. */
void fleet_button_set_enabled(lv_obj_t *button, int enabled);

/* Segmented control (DS §9): count segments, 56 px tall, 4 px apart. The
 * caller attaches its own handler to each segment (lv_obj_get_child(bar, i))
 * so it can pass whatever user data it needs. */
lv_obj_t *fleet_segments(lv_obj_t *parent, const char *const *labels, int count);
/* Fit the segments to the room the bar has (bar_w, px): in one row while the
 * widest word fits every segment, else two to a row, each row as tall as one
 * segment. The bar's height follows. A word may run a segment's width by 2 px
 * before it counts as not fitting - the slack the layout audit allows, and
 * what Small's "COMMANDER" has always used (DS §46). */
void fleet_segments_fit(lv_obj_t *bar, int32_t bar_w);
void fleet_segments_select(lv_obj_t *bar, int index);

/* A row inside a list panel: height px tall, flex row, divider below unless
 * it is the last one. Add children directly. */
lv_obj_t *fleet_row(lv_obj_t *parent, int height, int divider);
/* Horizontal box that spreads its children apart. */
lv_obj_t *fleet_hbox(lv_obj_t *parent, int height, int gap);

#endif
