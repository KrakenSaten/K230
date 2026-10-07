/*
 * RX LOG's two halves, for each other only: rift_rxlog_view.c builds and
 * draws, rift_rxlog_input.c takes the presses, the drag and the keys. Nothing
 * outside these two includes this.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_RXLOG_VIEW_INT_H
#define RIFT_RXLOG_VIEW_INT_H

#include "rift_rxlog_view.h"

#include "pos_styles.h"
#include "rift_rxlog.h"

/* Rows built once. A portrait body shows about twenty of two lines; the
 * rest of the pool is laid out under the fold and clipped. */
#define RXV_POOL 24
/* A drag of this far moves the log one entry. */
#define RXV_DRAG_ROW_PX 36

/* The identity hues (pos_theme.h, DS §37) RX LOG borrows as its class
 * colours (DS §54). The words carry the class; the colour only agrees. */
#define HUE_CORAL 0u  /* rejected */
#define HUE_ORANGE 1u /* relayed: a path of one hop or more */
#define HUE_GOLD 2u   /* advert */
#define HUE_GREEN 3u  /* direct: no hop between */
#define HUE_TEAL 4u   /* RX: a first reception */
#define HUE_SKY 5u    /* the mesh at work: ACK, REQ, RESP, PATH, TRACE, CTRL */
#define HUE_VIOLET 6u /* DUP: a repeat */
#define HUE_PINK 7u   /* ECHO: this node's own packet, repeated back */

struct field {
    lv_obj_t *label;
    lv_style_t *tone; /* the colour style on it now, NULL for its base role's */
};

struct row {
    lv_obj_t *row;
    lv_obj_t *line1;
    lv_obj_t *bar;
    struct field time, state, type, ch, hash, size, rssi, snr;
    struct field path;
    lv_obj_t *line3;
    struct field who, what;
    uint32_t uid;
    int selected;
};

struct rift_rxlog_view {
    struct rift_app *app;
    lv_obj_t *root;
    lv_obj_t *pause;
    lv_obj_t *clear;
    lv_obj_t *filter;
    lv_obj_t *caption;
    lv_obj_t *list;
    lv_obj_t *note;
    lv_obj_t *detail;
    lv_obj_t *detail_text;
    struct row rows[RXV_POOL];
    int shown;
    int visible;
    /* What the screen shows now, so a repaint with nothing new is free. */
    unsigned drawn_rev;
    uint32_t drawn_top;
    uint32_t drawn_sel;
    int drawn_filter;
    int drawn_paused;
    int drawn_supported;
    int drawn_connected;
    int drawn_once;
    int64_t drawn_ms;
    int32_t drag_acc;
    int dragged;
    lv_style_t text_font; /* Mono 14 with the colour emoji behind it */
};

struct rift_rxlog_view *rxv_of(const struct rift_app *a);
/* Draw now, or (now_too 0) when what is shown would change and the live
 * repaint interval allows; a repaint owed is asked of the app's timer. */
void rxv_repaint(struct rift_rxlog_view *v, int now_too);
void rxv_paint_detail(struct rift_rxlog_view *v);
/* The window's first row among the filter's entries (the hold put right
 * where it cannot be), the entry at a position, and the top moved there. */
int rxv_window_top(struct rift_rxlog *log);
uint32_t rxv_uid_at(const struct rift_rxlog *log, int pos);
void rxv_set_top(struct rift_rxlog *log, int pos);

/* rift_rxlog_input.c: the controls, the list and the detail's CLOSE. */
void rxv_on_pause(lv_event_t *e);
void rxv_on_clear(lv_event_t *e);
void rxv_on_filter(lv_event_t *e);
void rxv_on_list(lv_event_t *e);
void rxv_on_detail_close(lv_event_t *e);

#endif
