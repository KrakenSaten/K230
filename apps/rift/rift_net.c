/*
 * NET's placement: which ring each node is on, and which nodes a route runs
 * through. See rift_net.h. Read-only over the node cache, like rift_order.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_net.h"

#include "rift_format.h"

#include <string.h>
#include <strings.h>

static int ring_for_relays(int relays)
{
    if (relays < 0) {
        return RIFT_NET_RING_NO_PATH;
    }
    /* Ring 1 is zero relays (DIRECT), ring k is k-1 relays, and the last
     * counted ring holds everything from eight relays out. */
    if (relays + 1 >= RIFT_NET_RING_LAST_HOPS) {
        return RIFT_NET_RING_LAST_HOPS;
    }
    return relays + 1;
}

int rift_net_ring_of(const struct rift_node *n, enum rift_net_source *source)
{
    enum rift_net_source src = RIFT_NET_SOURCE_NONE;
    int ring = RIFT_NET_RING_NO_PATH;

    if (n && n->path_known) {
        src = RIFT_NET_SOURCE_ROUTE;
        ring = ring_for_relays(n->direct ? 0 : n->hops);
    } else if (n && n->have_advert_hops) {
        src = RIFT_NET_SOURCE_ADVERT;
        ring = ring_for_relays(n->advert_hops);
    }
    if (source) {
        *source = src;
    }
    return ring;
}

void rift_net_build(const struct rift_model *m, int64_t now_ms, struct rift_net *out)
{
    /* Static, as rift_model_order's own scratch: a thousand pointers, one
     * caller at a time on the one thread. */
    static const struct rift_node *order[RIFT_MAX_NODES];
    int count;
    int i;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!m) {
        return;
    }
    count = rift_model_order(m, now_ms, order, RIFT_MAX_NODES);
    for (i = 0; i < count; i++) {
        enum rift_net_source src;
        int ring = rift_net_ring_of(order[i], &src);
        struct rift_net_ring *r = &out->ring[ring];

        r->count++;
        if (src == RIFT_NET_SOURCE_ROUTE) {
            r->route++;
        } else if (src == RIFT_NET_SOURCE_ADVERT) {
            r->advert++;
        }
        if (order[i]->have_heard && !rift_node_is_stale(order[i], now_ms)) {
            r->fresh++;
        }
        if (r->shown < RIFT_NET_RING_SHOWN) {
            r->node[r->shown++] = order[i];
        }
        out->nodes++;
        if (ring != RIFT_NET_RING_NO_PATH) {
            out->hop_known++;
            if (ring > out->deepest) {
                out->deepest = ring;
            }
        }
    }
}

const char *rift_net_ring_word(int ring)
{
    static const char *const words[RIFT_NET_RINGS] = {
        "SELF", "DIRECT", "2", "3", "4", "5", "6", "7", "8", "9+", "NO PATH",
    };

    return (ring >= 0 && ring < RIFT_NET_RINGS) ? words[ring] : "?";
}

int rift_net_path_nodes(const struct rift_model *m, const struct rift_node *target,
                        const struct rift_node **out, int max)
{
    struct rift_path p;
    int n = 0;
    int h;

    if (!m || !target || !out || max <= 0 || !target->path_known) {
        return 0;
    }
    if (rift_path_parse(target, &p) != 0) {
        return 0;
    }
    for (h = 0; h < p.count && n < max; h++) {
        const struct rift_node *hit = NULL;
        size_t len;
        int i;

        if (!p.hop[h].have_id) {
            continue;
        }
        len = strlen(p.hop[h].id);
        for (i = 0; i < m->node_count; i++) {
            if (strncasecmp(m->nodes[i].key, p.hop[h].id, len) != 0) {
                continue;
            }
            if (hit) {
                /* Two nodes answer to this hop: it names neither. */
                hit = NULL;
                break;
            }
            hit = &m->nodes[i];
        }
        if (hit && hit != target) {
            out[n++] = hit;
        }
    }
    return n;
}
