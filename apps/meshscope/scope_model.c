/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "scope_model.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

int scope_resolve(const struct rift_model *m, const uint8_t *prefix, unsigned bytes)
{
    char hex[7];
    int found = SCOPE_UNKNOWN;
    if (!m || !prefix || bytes < 1 || bytes > 3) return SCOPE_UNKNOWN;
    for (unsigned i = 0; i < bytes; ++i) snprintf(hex + i * 2, 3, "%02x", prefix[i]);
    for (int i = 0; i < m->node_count; ++i) {
        if (strncasecmp(m->nodes[i].key, hex, bytes * 2)) continue;
        if (found >= 0) return SCOPE_AMBIGUOUS;
        found = i;
    }
    return found;
}

int scope_path(const struct rift_model *m, const struct rift_rx_entry *e,
               int out[RIFT_RXLOG_PATH_MAX])
{
    if (!m || !e || !out || e->kind != RIFT_RX_FRAME ||
        !(e->flags & RIFT_RXF_PARSED) || (e->flags & RIFT_RXF_PATH_SNR) ||
        (e->route != RIFT_RXRT_FLOOD && e->route != RIFT_RXRT_TRANSPORT_FLOOD) ||
        e->path_hash_size < 1 || e->path_hash_size > 3 ||
        e->path_hops > RIFT_RXLOG_PATH_MAX ||
        e->path_hops * e->path_hash_size != e->path_bytes ||
        e->path_bytes > RIFT_RXLOG_PATH_MAX) return 0;
    for (int i = 0; i < e->path_hops; ++i)
        out[i] = scope_resolve(m, e->path + i * e->path_hash_size, e->path_hash_size);
    return e->path_hops;
}

void scope_stats(const struct rift_model *m, const struct rift_rxlog *log,
                 int64_t now, struct scope_stats *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < m->node_count; ++i) out->located += m->nodes[i].have_location != 0;
    for (int i = 0; i < log->count; ++i) {
        const struct rift_rx_entry *e = rift_rxlog_at(log, i);
        if (e->kind != RIFT_RX_FRAME) continue;
        out->frames++;
        out->duplicates += e->state == RIFT_RXS_DUP || e->state == RIFT_RXS_ECHO;
        out->rejected += e->state == RIFT_RXS_REJECTED;
        out->recent += now >= e->mono_ms && now - e->mono_ms < 60000;
    }
}

unsigned scope_node_packets(const struct rift_model *m, const struct rift_rxlog *log, int index)
{
    unsigned count = 0;
    if (index < 0 || index >= m->node_count) return 0;
    uint8_t prefix[3];
    int unique[4] = {0};
    for (int b = 0; b < 3; ++b) {
        unsigned value = 0;
        if (sscanf(m->nodes[index].key + 2*b, "%2x", &value) != 1) return 0;
        prefix[b] = value;
        unique[b+1] = scope_resolve(m, prefix, b+1) == index;
    }
    for (int i = 0; i < log->count; ++i) {
        const struct rift_rx_entry *e = rift_rxlog_at(log, i);
        unsigned b = e->path_hash_size;
        if (e->kind != RIFT_RX_FRAME || !(e->flags & RIFT_RXF_PARSED) ||
            (e->flags & RIFT_RXF_PATH_SNR) ||
            (e->route != RIFT_RXRT_FLOOD && e->route != RIFT_RXRT_TRANSPORT_FLOOD) ||
            b < 1 || b > 3 || !unique[b] || e->path_bytes > RIFT_RXLOG_PATH_MAX ||
            e->path_hops * b != e->path_bytes) continue;
        for (unsigned j = 0; j < e->path_hops; ++j) {
            if (!memcmp(e->path + j*b, prefix, b)) { count++; break; }
        }
    }
    return count;
}
