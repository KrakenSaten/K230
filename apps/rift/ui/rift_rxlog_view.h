/*
 * RX LOG: the packet inspector behind ACTIVITY (docs/apps/RIFT.md, "RX LOG";
 * DS §54).
 *
 * A view over the session's ring (rift_rxlog.h), newest first: a fixed pool
 * of row objects is filled from wherever the reader is in the ring, so a
 * thousand entries cost the screen no more than the rows it shows. Each row
 * is two or three lines of fixed-size Mono 14 - the first line's fields,
 * the whole path, and what could be read - with the class of the frame in
 * its colours and its words.
 *
 * PAUSE freezes what is shown; the ring goes on filling underneath and the
 * caption counts what arrived. CLEAR empties the ring and nothing else.
 * FILTER steps through ALL, DUP, MSG, ADV and CTRL. A tap on a row, or
 * Enter on the selected one, opens its detail.
 *
 * Keys: Up/Down select, Left/Right a page, Enter the detail, Esc closes it
 * or goes back to ACTIVITY, P pause, F filter, C clear, Home back to the
 * newest. From ACTIVITY, R opens the log.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_RXLOG_VIEW_H
#define RIFT_RXLOG_VIEW_H

#include "rift_app.h"

lv_obj_t *rift_rxlog_view_create(struct rift_app *app, lv_obj_t *parent);
void rift_rxlog_view_refresh(struct rift_app *app);
void rift_rxlog_view_shape(struct rift_app *app);
void rift_rxlog_view_destroy(struct rift_app *app);
/* A key, on RX LOG or (R) on ACTIVITY. Returns 1 when it was taken. */
int rift_rxlog_view_key(struct rift_app *app, uint32_t key);
/* The Back action: closes the detail if it is open. Returns 1 when it did. */
int rift_rxlog_view_back(struct rift_app *app);

/* For the tests: the controls, a row's labels, the detail. NULL when the
 * screen does not exist or the row is not shown. */
lv_obj_t *rift_rxlog_view_button(const struct rift_app *app, int which); /* 0 pause, 1 clear, 2 filter */
lv_obj_t *rift_rxlog_view_caption(const struct rift_app *app);
/* line 0: the first line's fields joined by spaces; 1 the path; 2 who and
 * what. Into out. Returns 0, or -1 when row i is not shown. */
int rift_rxlog_view_row_text(const struct rift_app *app, int i, int line, char *out, size_t out_len);
int rift_rxlog_view_rows_shown(const struct rift_app *app);
lv_obj_t *rift_rxlog_view_row(const struct rift_app *app, int i);
lv_obj_t *rift_rxlog_view_detail(const struct rift_app *app); /* the detail's text, when open */
/* Scroll by rows, as a drag does: positive is older. */
void rift_rxlog_view_scroll(struct rift_app *app, int rows);

#endif
