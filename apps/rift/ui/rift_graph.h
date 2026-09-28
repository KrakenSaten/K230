/*
 * The traffic graph: what was heard on the air in the last twenty minutes,
 * a bar per minute (rift_traffic.h), on ACTIVITY.
 *
 * Every bar is a count of frames the service reported hearing in that
 * minute, stacked by what they were - messages, adverts, the rest - and
 * nothing else: not a signal meter, not airtime, not anything inferred.
 * The heights are a fixed ladder (1, 2-3, 4-7, 8-15, 16+), so a bar drawn
 * for a minute keeps its height when a busier minute comes along; the
 * T-Deck's RIFT settled on the same ladder for the same reason.
 *
 * It draws in one callback from the tokens - messages in status_ok,
 * adverts in radio_rx, the rest in text_muted, the baseline in line - so
 * it costs one draw pass per repaint and is invalidated only when a bin
 * changed. RIFT owns no colour: the words beside it name the classes, and
 * the swatches only agree with the words.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_GRAPH_H
#define RIFT_GRAPH_H

#include "rift_traffic.h"

#include "lvgl.h"

/* The band the bars grow in, and the baseline under them. */
#define RIFT_GRAPH_BAND 28
#define RIFT_GRAPH_TOTAL_H (RIFT_GRAPH_BAND + 2)
/* The legend's swatch: a small box in the class's colour. */
#define RIFT_SWATCH_PX 8

lv_obj_t *rift_traffic_graph_create(lv_obj_t *parent);
/* Show these bins; redraws only when they differ from what is shown. */
void rift_traffic_graph_set(lv_obj_t *graph, const struct rift_traffic_bins *bins);
/* What the graph is showing, for tests: the bins as last set, or NULL. */
const struct rift_traffic_bins *rift_traffic_graph_bins(lv_obj_t *graph);

/* The bar height, in pixels of the band, for a minute's count. */
int32_t rift_graph_height_of(unsigned count);

lv_obj_t *rift_traffic_swatch_create(lv_obj_t *parent, enum rift_traffic_class c);
/* The word for a class, as the legend prints it: MSG, ADV, OTHER. */
const char *rift_traffic_class_word(enum rift_traffic_class c);

#endif
