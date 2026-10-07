/*
 * ACTIVITY: what this node is doing, and what it has heard.
 *
 * Not a dashboard of cards, and no settings: those are SYSTEM's. Three
 * things, each of them a fact somebody can act on: what state the service
 * is in and why, which nodes were heard most recently, and the raw feed
 * underneath - frames in and submissions out, with the signal only where
 * one was measured.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_ACTIVITY_H
#define RIFT_ACTIVITY_H

#include "rift_app.h"

lv_obj_t *rift_activity_create(struct rift_app *app, lv_obj_t *parent);
void rift_activity_refresh(struct rift_app *app);
void rift_activity_shape(struct rift_app *app);
void rift_activity_destroy(struct rift_app *app);
/* The traffic graph (ui/rift_graph.h), for tests; NULL before the screen
 * exists. */
lv_obj_t *rift_activity_graph(const struct rift_app *app);
/* The RX LOG action under the feed, for tests; NULL before the screen
 * exists. */
lv_obj_t *rift_activity_rxlog_button(const struct rift_app *app);

#endif
