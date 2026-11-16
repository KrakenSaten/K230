/*
 * ACTIVITY's live traffic graph (DS §37.4, redesigned 2026-11).
 *
 * One custom-drawn object in two tiers, both read from the session's own
 * record of what was heard (rift_air.h):
 *
 *   the live scope   the last sixty seconds, one mark per frame: where it
 *                    sits across is when it was heard, how high is the RSSI
 *                    it was heard at (mid-height when none was measured),
 *                    its size follows its bytes and its colour its type. A
 *                    frame arrives with a ripple and drifts left as it ages.
 *                    A frame this device sent is an upright bar in radio_tx.
 *   the bars         sixty bars over the last twenty minutes (or five: a tap
 *                    turns the span over), each stacked by payload type. The
 *                    bars scroll with the clock rather than jumping a bar at
 *                    a time, and grow to a new count rather than snapping.
 *
 * Every mark is a count or a measurement the service reported: nothing is
 * inferred and nothing is decorative data. What moves when the air is quiet
 * is the sweep across the scope, which says only that the radio is
 * listening, and stops when it is not.
 *
 * The types are drawn in the packet palette (pos_theme.h, CoreScope's
 * colours); the legend beside the graph names each in words, so colour never
 * carries a type alone. Everything else is read from tokens in the draw
 * callback. Its captions are the caption role at Small whatever the text
 * size, like the debug overlay's: it is an instrument, and its height is
 * fixed.
 *
 * Cost: one frame every RIFT_GRAPH_FRAME_MS while the graph is on screen,
 * from an LVGL animation on the object (no timer of its own). A frame
 * repaints the scope tier only; the bars are repainted when a count,
 * the scale or their scroll position moved by a pixel. With reduced motion
 * on (DS §12) nothing animates: the graph is repainted when something was
 * heard and once a second.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_GRAPH_H
#define RIFT_GRAPH_H

#include "rift_air.h"
#include "rift_traffic.h"

#include "lvgl.h"

/* The tiers, top to bottom: the scope, a gap, the bars, the time axis. */
#define RIFT_GRAPH_SCOPE_H 56
#define RIFT_GRAPH_GAP 6
#define RIFT_GRAPH_BAND 92
#define RIFT_GRAPH_AXIS_H 18
#define RIFT_GRAPH_TOTAL_H \
    (RIFT_GRAPH_SCOPE_H + RIFT_GRAPH_GAP + RIFT_GRAPH_BAND + RIFT_GRAPH_AXIS_H)
#define RIFT_GRAPH_FRAME_MS 66
/* The spans a tap turns between, in slots a bar. */
#define RIFT_GRAPH_SPAN_20MIN (RIFT_AIR_SLOTS / RIFT_AIR_BARS)
#define RIFT_GRAPH_SPAN_5MIN 1
/* The legend's swatch: a small box in the type's colour. */
#define RIFT_SWATCH_PX 10
/* A swatch for a frame this device sent (radio_tx), beside the types. */
#define RIFT_SWATCH_TX (-1)

lv_obj_t *rift_traffic_graph_create(lv_obj_t *parent);
/* The record the graph draws from: the session's, which outlives the
 * graph. NULL draws an empty graph. */
void rift_traffic_graph_feed(lv_obj_t *graph, const struct rift_air *air);
/* The minute bins beside it (rift_traffic.h): what the caption's PEAK is
 * read from. Kept on the graph so one object answers for both. */
void rift_traffic_graph_set(lv_obj_t *graph, const struct rift_traffic_bins *bins);
const struct rift_traffic_bins *rift_traffic_graph_bins(lv_obj_t *graph);
/* The span: RIFT_GRAPH_SPAN_20MIN or RIFT_GRAPH_SPAN_5MIN. */
void rift_traffic_graph_set_span(lv_obj_t *graph, int slots_per_bar);
int rift_traffic_graph_span(lv_obj_t *graph);
/* Draw one type at full strength and the rest dimmed; -1 draws them all. */
void rift_traffic_graph_isolate(lv_obj_t *graph, int type);
int rift_traffic_graph_isolated(lv_obj_t *graph);
/* Whether the radio is listening: the sweep runs only while it is. */
void rift_traffic_graph_set_listening(lv_obj_t *graph, int listening);
/* The top of the bars' scale for a peak: the next of 4, 8, 12, 20, 40, 80,
 * 120, 200, ... at or above it, so the scale moves in steps and a bar's
 * height is steady between them. */
unsigned rift_graph_scale_for(unsigned peak);

/* The legend's swatch for a type (enum rift_air_type) or RIFT_SWATCH_TX. */
lv_obj_t *rift_traffic_swatch_create(lv_obj_t *parent, int type);
void rift_traffic_swatch_set(lv_obj_t *swatch, int type);

/* The mix: one bar as wide as its parent, split by type in proportion to
 * the counts given (RIFT_AIR_TYPES of them); an empty rule when all are 0. */
#define RIFT_MIX_H 8
lv_obj_t *rift_traffic_mix_create(lv_obj_t *parent);
void rift_traffic_mix_set(lv_obj_t *mix, const uint32_t *type_total);

#endif
