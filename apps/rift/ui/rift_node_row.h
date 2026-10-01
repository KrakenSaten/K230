/*
 * One row of the NODES list: what it is made of, and what it says about a
 * node. The list (rift_nodes.c) keeps a pool of these and binds them to
 * whichever nodes are on screen; this file builds one, fills it from a node,
 * and gives it or takes from it the selection's look. Private to NODES.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_NODE_ROW_H
#define RIFT_NODE_ROW_H

#include "rift_app.h"

/* Column widths. Every column but the name is fixed, so a row cannot reflow
 * as its values change width, and the header and the rows are built from the
 * same list: a role tag of its own would be one item wider than the header
 * and would put every column after it out by the tag's width, so the name
 * and its tag share one box that is the column.
 *
 * The widths are what the widest value in each column measures in Mono 14
 * (RSSI is "−103", HEARD is "HEARD" in the header) with room to spare
 * in Outdoor. The activity pulse has a column of its own after HEARD, the
 * age it buckets. */
#define COL_GAP 8
#define COL_GLYPH RIFT_GLYPH_BOX
#define COL_HOPS 40
#define COL_RSSI 48
#define COL_SNR 44
#define COL_PULSE RIFT_PULSE_W
#define COL_HEARD 48
#define TAG_GAP 6
/* The pulse sits 5 px after the age it buckets rather than a column gap
 * away: a flex margin pulls it in. */
#define PULSE_PULL (5 - COL_GAP)
#define HEAD_CELLS 7
/* The selected row keeps the list's rhythm: a slab 2 px taller top and
 * bottom than a row, inset 8 px, and 2 px of room around it for the focus
 * outline, which LVGL draws outside the box. */
#define SELECTED_INSET_H 8
#define SELECTED_INSET_V 2
#define SELECTED_AIR 2

struct rift_node_row {
    lv_obj_t *slot;
    lv_obj_t *line;
    lv_obj_t *ident; /* the identity mark (DS §37.3): chat and room nodes */
    lv_obj_t *glyph;
    lv_obj_t *namebox;
    lv_obj_t *name;
    lv_obj_t *tag;
    lv_obj_t *strip;
    lv_obj_t *hops;
    lv_obj_t *rssi;
    lv_obj_t *snr;
    lv_obj_t *pulse;
    lv_obj_t *heard;
    /* The expansion, portrait and selected only. */
    lv_obj_t *expand;
    lv_obj_t *exp_glyph;
    lv_obj_t *exp_state;
    lv_obj_t *exp_chain;
    lv_obj_t *exp_signal;
    /* What it is showing: a key, and where that key is in the list. An
     * empty key is a row in the pool that shows nothing and is hidden. */
    char key[RIFT_KEY_HEX];
    int item;
    int selected; /* styled as the selection */
    struct rift_app *app;
};

/* A 36 px (or height px) row of cells, which takes no taps itself. */
lv_obj_t *rift_node_row_line(lv_obj_t *parent, int32_t height);
/* The hop strip's width in this shape. */
int32_t rift_node_strip_width(const struct rift_app *a);

/* Build a row into list, hidden and showing nothing. */
void rift_node_row_build(struct rift_node_row *r, struct rift_app *app, lv_obj_t *list);
/* Give the row the selection's look, or take it away; in portrait the
 * selection also carries the expansion, built here and dropped here. */
void rift_node_row_select(struct rift_node_row *r, int selected);
void rift_node_row_drop_expansion(struct rift_node_row *r);
/* Fill the row from a node. */
void rift_node_row_update(struct rift_node_row *r, const struct rift_node *n, int64_t now);

#endif
