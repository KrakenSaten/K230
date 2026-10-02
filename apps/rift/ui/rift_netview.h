/*
 * NET: the hop rings of handoff §7, drawn from the node cache.
 *
 * At the top, the PATH panel: the selected node (the same selection NODES
 * has), the route to it written out as the inline chain, what placed it on
 * its ring, and MESSAGE / DETAIL to go on from there. Under it, one row per
 * ring in portrait - 0 SELF, 1 DIRECT, 2 .. 9+, NO PATH - and one column per
 * ring in landscape, each holding the nodes on it as pills. A pill selects
 * its node and does nothing else. The nodes the selected route runs through
 * carry the focus outline, and the selected node is filled.
 *
 * Only what was observed is drawn (rift_net.h): no link between two nodes is
 * drawn at all, because the service reports none - only how far each one is
 * from here, and the one route a node's chain spells out.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_NET_VIEW_H
#define RIFT_NET_VIEW_H

#include "rift_app.h"

lv_obj_t *rift_net_view_create(struct rift_app *app, lv_obj_t *parent);
void rift_net_view_destroy(struct rift_app *app);
void rift_net_view_shape(struct rift_app *app);
void rift_net_view_refresh(struct rift_app *app);
/* Pills built on a ring, for a test: bounded by RIFT_NET_RING_SHOWN + 1. */
int rift_net_view_pills(const struct rift_app *app, int ring);

#endif
