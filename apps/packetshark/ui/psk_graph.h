/*
 * PacketShark's traffic graph (DS §59.5, accepted 2026-11-16): one column
 * per elapsed second, the last 60, newest at the right, on a fixed
 * logarithmic ladder, stacked by protocol class in its identity hue.
 *
 * One object and one LV_EVENT_DRAW_MAIN callback, the pattern of
 * apps/rift/ui/rift_graph.c: at most 60 x 8 rectangles, redrawn only when a
 * new second arrives or the selection changes, and on a theme or display-mode
 * change (pos_theme_watch). Its state lives in the object's user data and is
 * freed with it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PSK_UI_GRAPH_H
#define PSK_UI_GRAPH_H

#include "lvgl.h"
#include "psk_model.h"

#define PSK_GRAPH_COL_W 5
#define PSK_GRAPH_COL_GAP 2
#define PSK_GRAPH_W (PSK_GRAPH_SECONDS * (PSK_GRAPH_COL_W + PSK_GRAPH_COL_GAP) - PSK_GRAPH_COL_GAP) /* 418 */
#define PSK_GRAPH_H (PSK_GRAPH_BAND + 1)                                                          /* 209 */

lv_obj_t *psk_graph_create(lv_obj_t *parent);
/* Replace the data; redraws only when it changed. */
void psk_graph_set(lv_obj_t *graph, const struct psk_graph_window *w);
/* A tap at screen x: selects the column under it (the nearest), or clears the
 * selection when it is that column already. Returns the selected absolute
 * second, or -1 when none is selected afterwards. */
int64_t psk_graph_tap(lv_obj_t *graph, int32_t x);
/* The selected absolute second, or -1. A selection that has scrolled out of
 * the 60 seconds is dropped. */
int64_t psk_graph_selected(lv_obj_t *graph);
void psk_graph_clear(lv_obj_t *graph);

#endif
