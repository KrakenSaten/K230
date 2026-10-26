/*
 * TRAFFIC's analysis. See rift_scope.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_scope.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

int rift_scope_resolve(const struct rift_model *m, const uint8_t *prefix, unsigned bytes)
{
    char hex[7];
    int found = RIFT_SCOPE_UNKNOWN;
    int i;
    unsigned b;

    if (!m || !prefix || bytes < 1 || bytes > 3) {
        return RIFT_SCOPE_UNKNOWN;
    }
    for (b = 0; b < bytes; b++) {
        snprintf(hex + b * 2, 3, "%02x", prefix[b]);
    }
    for (i = 0; i < m->node_count; i++) {
        if (strncasecmp(m->nodes[i].key, hex, bytes * 2) != 0) {
            continue;
        }
        if (found >= 0) {
            return RIFT_SCOPE_AMBIGUOUS;
        }
        found = i;
    }
    return found;
}

/* A parsed flood whose path holds whole hashes of the width it reported. */
static int travelled(const struct rift_rx_entry *e)
{
    return e && e->kind == RIFT_RX_FRAME && (e->flags & RIFT_RXF_PARSED) &&
           !(e->flags & RIFT_RXF_PATH_SNR) &&
           (e->route == RIFT_RXRT_FLOOD || e->route == RIFT_RXRT_TRANSPORT_FLOOD) &&
           e->path_hash_size >= 1 && e->path_hash_size <= 3 &&
           e->path_bytes <= RIFT_RXLOG_PATH_MAX &&
           (unsigned)e->path_hops * e->path_hash_size == e->path_bytes;
}

int rift_scope_path(const struct rift_model *m, const struct rift_rx_entry *e,
                    int out[RIFT_RXLOG_PATH_MAX])
{
    int i;

    if (!m || !out || !travelled(e)) {
        return 0;
    }
    for (i = 0; i < e->path_hops; i++) {
        out[i] = rift_scope_resolve(m, e->path + i * e->path_hash_size, e->path_hash_size);
    }
    return e->path_hops;
}

const struct rift_rx_entry *rift_scope_live(const struct rift_model *m,
                                            const struct rift_rxlog *log, int64_t now)
{
    int i;

    for (i = 0; m && log && i < log->count; i++) {
        const struct rift_rx_entry *e = rift_rxlog_at(log, i);

        if (e->kind == RIFT_RX_MARK) {
            break; /* never bridge a service gap */
        }
        if (now < e->mono_ms || now - e->mono_ms >= RIFT_SCOPE_LIVE_MS) {
            break; /* newest first: the rest are older still */
        }
        if (travelled(e) && e->path_hops > 0) {
            return e;
        }
    }
    return NULL;
}

void rift_scope_hops_of(const struct rift_model *m, const int *path, int n,
                        struct rift_scope_hops *out)
{
    int i;

    memset(out, 0, sizeof(*out));
    out->hops = n;
    for (i = 0; i < n; i++) {
        if (path[i] == RIFT_SCOPE_UNKNOWN) {
            out->unknown++;
        } else if (path[i] < 0) {
            out->ambiguous++;
        } else {
            out->resolved++;
            out->unlocated += !m->nodes[path[i]].have_location;
            if (i > 0 && path[i - 1] >= 0 && m->nodes[path[i - 1]].have_location &&
                m->nodes[path[i]].have_location) {
                out->segments++;
            }
        }
    }
}

void rift_scope_stats(const struct rift_model *m, const struct rift_rxlog *log, int64_t now,
                      struct rift_scope_stats *out)
{
    int i;

    memset(out, 0, sizeof(*out));
    for (i = 0; i < m->node_count; i++) {
        out->located += m->nodes[i].have_location != 0;
    }
    for (i = 0; i < log->count; i++) {
        const struct rift_rx_entry *e = rift_rxlog_at(log, i);

        if (e->kind != RIFT_RX_FRAME) {
            continue;
        }
        out->frames++;
        out->duplicates += e->state == RIFT_RXS_DUP || e->state == RIFT_RXS_ECHO;
        out->rejected += e->state == RIFT_RXS_REJECTED;
        out->recent += now >= e->mono_ms && now - e->mono_ms < RIFT_SCOPE_RECENT_MS;
    }
}

unsigned rift_scope_node_packets(const struct rift_model *m, const struct rift_rxlog *log,
                                 int index)
{
    unsigned count = 0;
    uint8_t prefix[3];
    int unique[4] = { 0 };
    int b;
    int i;

    if (index < 0 || index >= m->node_count) {
        return 0;
    }
    for (b = 0; b < 3; b++) {
        unsigned value = 0;

        if (sscanf(m->nodes[index].key + 2 * b, "%2x", &value) != 1) {
            return 0;
        }
        prefix[b] = (uint8_t)value;
        unique[b + 1] = rift_scope_resolve(m, prefix, (unsigned)b + 1) == index;
    }
    for (i = 0; i < log->count; i++) {
        const struct rift_rx_entry *e = rift_rxlog_at(log, i);
        unsigned w;
        int j;

        if (!travelled(e)) {
            continue;
        }
        w = e->path_hash_size;
        if (!unique[w]) {
            continue;
        }
        for (j = 0; j < e->path_hops; j++) {
            if (memcmp(e->path + j * w, prefix, w) == 0) {
                count++;
                break;
            }
        }
    }
    return count;
}
