/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef SCOPE_MODEL_H
#define SCOPE_MODEL_H
#include "rift_model.h"
#include "rift_rxlog.h"

/* An index is usable only while the node snapshot is unchanged. Resolve
 * afresh when drawing; never save indices as node identities. */
#define SCOPE_UNKNOWN (-1)
#define SCOPE_AMBIGUOUS (-2)
int scope_resolve(const struct rift_model *m, const uint8_t *prefix, unsigned bytes);
/* Only received flood paths describe travelled hops. Direct paths contain
 * remaining destinations and TRACE carries SNR bytes, not node hashes. */
int scope_path(const struct rift_model *m, const struct rift_rx_entry *e,
               int out[RIFT_RXLOG_PATH_MAX]);
struct scope_stats {
    unsigned frames, duplicates, rejected, recent, located;
};
void scope_stats(const struct rift_model *m, const struct rift_rxlog *log,
                 int64_t now, struct scope_stats *out);
/* Counts packets whose observed flood path contains this unique node prefix.
 * A packet counts once even if its route visits the node twice. */
unsigned scope_node_packets(const struct rift_model *m, const struct rift_rxlog *log,
                            int index);
#endif
