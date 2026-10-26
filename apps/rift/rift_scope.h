/*
 * TRAFFIC's analysis: what the receive log (rift_rxlog.h) says about the
 * paths packets travelled, read against the node cache (rift_model.h).
 *
 * MAP's TRAFFIC mode (ui/rift_scope_view.h) draws from this. Everything here
 * is an observation at this one receiver, and every name it puts to a hop is
 * an inference:
 *
 *   - A path byte group is matched against the cache's public keys at the
 *     width the packet reported (one, two or three bytes). One match is a
 *     unique prefix match - still only an inference from the cache, never
 *     proof of identity. None is UNKNOWN; two or more is AMBIGUOUS, whether
 *     or not the others have a location.
 *   - Only a received flood's path is the hops it travelled. A direct
 *     packet's path is the route still ahead of it, and a TRACE carries
 *     per-hop SNR samples, not node hashes: neither is a travelled path.
 *   - Nothing is ever resolved to a saved index: an index is valid only for
 *     the node snapshot it was taken from, so resolve again when drawing.
 *
 * No LVGL: host-tested by tests/rift_scope_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_SCOPE_H
#define RIFT_SCOPE_H

#include "rift_model.h"
#include "rift_rxlog.h"

#define RIFT_SCOPE_UNKNOWN (-1)
#define RIFT_SCOPE_AMBIGUOUS (-2)
/* How long the newest flood path is the LIVE path on the map. */
#define RIFT_SCOPE_LIVE_MS 8000
/* What "recent" means in the counts. */
#define RIFT_SCOPE_RECENT_MS 60000

/* The node whose key starts with these bytes, or RIFT_SCOPE_UNKNOWN /
 * RIFT_SCOPE_AMBIGUOUS. bytes is 1..3; anything else is UNKNOWN. */
int rift_scope_resolve(const struct rift_model *m, const uint8_t *prefix, unsigned bytes);
/* A received flood's travelled hops, each resolved as above, in the order
 * travelled. Returns the hop count, or 0 for anything that is not a parsed
 * flood with a consistent path (a direct route, a TRACE, a mark). */
int rift_scope_path(const struct rift_model *m, const struct rift_rx_entry *e,
                    int out[RIFT_RXLOG_PATH_MAX]);

/* The newest flood path heard within RIFT_SCOPE_LIVE_MS of now, newer than
 * the latest service gap (a mark), or NULL. Never one from before a gap:
 * the log does not bridge one. */
const struct rift_rx_entry *rift_scope_live(const struct rift_model *m,
                                            const struct rift_rxlog *log, int64_t now);

/* What a path's hops come to. */
struct rift_scope_hops {
    int hops, resolved, unknown, ambiguous, unlocated;
    int segments; /* adjacent pairs both resolved and located: the lines drawn */
};
void rift_scope_hops_of(const struct rift_model *m, const int *path, int n,
                        struct rift_scope_hops *out);

/* The ring as it stands. */
struct rift_scope_stats {
    unsigned frames, duplicates, rejected, recent, located;
};
void rift_scope_stats(const struct rift_model *m, const struct rift_rxlog *log, int64_t now,
                      struct rift_scope_stats *out);
/* How many held packets have an observed flood path that holds this node's
 * prefix, at a width where that prefix is unique in the cache. A packet
 * counts once even if its path holds the node twice. A count over the ring,
 * not a lifetime one. */
unsigned rift_scope_node_packets(const struct rift_model *m, const struct rift_rxlog *log,
                                 int index);

#endif
