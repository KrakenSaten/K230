/*
 * NET: the mesh as rings of hop count around this device (handoff §7,
 * "Hop rings"). No LVGL: the placement is host-tested by
 * tests/rift_model_test.c, and ui/rift_net.c only draws it.
 *
 * A ring is an OBSERVED hop count, not a distance, and nothing here infers a
 * link. Ring 0 is this device; ring 1 is DIRECT (heard with no relay in
 * between); ring k is k-1 relays, up to ring 9, which holds eight relays and
 * more ("9+"); and the last ring is NO PATH - a node held with no hop count
 * observed at all. Which observation places a node is said with it:
 *
 *   ROUTE   the route back the service learned (mesh.nodes path_known,
 *           hops): the path a message to the node would take. Preferred,
 *           because it is the one RIFT can draw as a chain.
 *   ADVERT  no route is learned, and the node's last advert was heard
 *           (mesh.nodes advert_hops): how many relays that advert came
 *           through. The other direction, and only a count - there is no
 *           chain to draw for it.
 *
 * The handoff's decision §11.5 ("ring membership when a node has several
 * observed paths: last observed path") is the service's: mesh.nodes reports
 * one learned route and one last advert per node, and that is what is used.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_NET_H
#define RIFT_NET_H

#include "rift_model.h"

#define RIFT_NET_RING_SELF 0
#define RIFT_NET_RING_DIRECT 1
#define RIFT_NET_RING_LAST_HOPS 9 /* eight relays and more */
#define RIFT_NET_RING_NO_PATH 10
#define RIFT_NET_RINGS 11
/* Nodes drawn on one ring. The rest are counted ("+12"), so a ring of a
 * hundred nodes costs what a ring of two dozen does. */
#define RIFT_NET_RING_SHOWN 24

enum rift_net_source {
    RIFT_NET_SOURCE_NONE = 0,
    RIFT_NET_SOURCE_ROUTE,
    RIFT_NET_SOURCE_ADVERT,
};

/* The ring a node sits on, and what placed it there. Never 0: ring 0 is
 * this device alone. */
int rift_net_ring_of(const struct rift_node *n, enum rift_net_source *source);

struct rift_net_ring {
    int count;  /* every node on the ring */
    int route;  /* of which placed by a learned route */
    int advert; /* of which placed by an advert's hop count */
    int fresh;  /* of which heard within RIFT_STALE_MS */
    int shown;  /* entries in node[] */
    const struct rift_node *node[RIFT_NET_RING_SHOWN];
};

struct rift_net {
    struct rift_net_ring ring[RIFT_NET_RINGS];
    int nodes;    /* every node placed, NO PATH included */
    int hop_known; /* of which on a ring with a hop count (1..9) */
    int deepest;  /* the outermost ring 1..9 holding a node, 0 when none */
};

/* Every held node onto its ring, each ring in list order (rift_model_order:
 * heard lately first). Ring 0 is left empty: this device is drawn from the
 * identity, not from the node cache. */
void rift_net_build(const struct rift_model *m, int64_t now_ms, struct rift_net *out);

/* The ring's word: "SELF", "DIRECT", "2" .. "8", "9+", "NO PATH". */
const char *rift_net_ring_word(int ring);

/* The nodes a target's learned route runs through, nearest first: each hop
 * the route names that exactly one held node answers to (rift_model_name_for_
 * hash's rule - an ambiguous hash names nobody). Writes at most max and
 * returns how many; 0 when the target has no learned route or names none. */
int rift_net_path_nodes(const struct rift_model *m, const struct rift_node *target,
                        const struct rift_node **out, int max);

#endif
