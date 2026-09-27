/*
 * RIFT's node queries: which node comes first in a list, how many were heard
 * lately, and which name a hop hash gets. Read-only questions over the node
 * cache that rift_model.c keeps, answered here so that file stays the one
 * that writes it. No LVGL and no sockets: host-tested by
 * tests/rift_model_test.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_model.h"

#include "rift_format.h"

#include <string.h>
#include <strings.h>

/* Three groups, in this order: heard within the stale boundary (newest
 * first), heard longer ago (newest first), never heard at all. */
static int group_of(const struct rift_node *n, int64_t now_ms)
{
    if (!n->have_heard) {
        return 2;
    }
    return rift_node_is_stale(n, now_ms) ? 1 : 0;
}

static int before(const struct rift_node *a, const struct rift_node *b, int64_t now_ms)
{
    int ga = group_of(a, now_ms);
    int gb = group_of(b, now_ms);

    if (ga != gb) {
        return ga < gb;
    }
    if (a->have_heard && b->have_heard && a->heard_mono_ms != b->heard_mono_ms) {
        return a->heard_mono_ms > b->heard_mono_ms;
    }
    /* A total order, so a list that is rebuilt every second does not
     * reshuffle rows whose ages are equal. */
    return strcmp(a->key, b->key) < 0;
}

/* A merge sort over pointers: n log n however the cache happens to be
 * ordered, where the insertion sort this replaced was n squared for a
 * cache the service listed oldest first - half a million comparisons a
 * second at a thousand nodes, on every repaint. `before` is a total order,
 * so the result is the same one in the same place either way. */
static void merge_order(const struct rift_node **a, const struct rift_node **tmp, int lo, int hi,
                        int64_t now_ms)
{
    int mid;
    int i;
    int j;
    int k;

    if (hi - lo < 2) {
        return;
    }
    mid = lo + (hi - lo) / 2;
    merge_order(a, tmp, lo, mid, now_ms);
    merge_order(a, tmp, mid, hi, now_ms);
    for (i = lo, j = mid, k = lo; k < hi; k++) {
        if (j >= hi || (i < mid && !before(a[j], a[i], now_ms))) {
            tmp[k] = a[i++];
        } else {
            tmp[k] = a[j++];
        }
    }
    memcpy(a + lo, tmp + lo, sizeof(a[0]) * (size_t)(hi - lo));
}

int rift_model_order(const struct rift_model *m, int64_t now_ms, const struct rift_node **out,
                     int max)
{
    /* Static, not on the stack: 8 KB at a thousand nodes, one caller at a
     * time on the one thread. */
    static const struct rift_node *tmp[RIFT_MAX_NODES];
    int n = 0;
    int i;

    if (!m || !out || max <= 0) {
        return 0;
    }
    for (i = 0; i < m->node_count && n < max; i++) {
        out[n++] = &m->nodes[i];
    }
    merge_order(out, tmp, 0, n, now_ms);
    return n;
}

int rift_model_fresh_count(const struct rift_model *m, int64_t now_ms)
{
    int n = 0;
    int i;

    if (!m) {
        return 0;
    }
    for (i = 0; i < m->node_count; i++) {
        if (group_of(&m->nodes[i], now_ms) == 0) {
            n++;
        }
    }
    return n;
}

const char *rift_model_name_for_hash(const struct rift_model *m, const char *hash_hex)
{
    const struct rift_node *hit = NULL;
    size_t len;
    int i;

    if (!m || !hash_hex) {
        return NULL;
    }
    len = strlen(hash_hex);
    if (len == 0 || len > RIFT_KEY_HEX - 1) {
        return NULL;
    }
    if (m->have_identity && strncasecmp(m->self_key, hash_hex, len) == 0) {
        return m->self_name[0] ? m->self_name : NULL;
    }
    for (i = 0; i < m->node_count; i++) {
        if (strncasecmp(m->nodes[i].key, hash_hex, len) != 0) {
            continue;
        }
        if (hit) {
            /* Two nodes share this prefix. mesh.node refuses an ambiguous
             * prefix rather than answering one (docs/api/mesh.md) and so
             * does this: a guessed name on a hop is a wrong route drawn
             * confidently. */
            return NULL;
        }
        hit = &m->nodes[i];
    }
    if (!hit || !hit->have_name || !hit->name[0]) {
        return NULL;
    }
    return hit->name;
}

int rift_model_conv_heard(const struct rift_model *m, const struct rift_conv *c, int64_t *ms)
{
    int have = 0;
    int64_t best = 0;

    if (!m || !c) {
        return 0;
    }
    if (c->have_last_in_mono) {
        have = 1;
        best = c->last_in_mono_ms;
    }
    if (!c->is_channel) {
        const struct rift_node *n = rift_model_find(m, c->key);

        if (n && n->have_heard && (!have || n->heard_mono_ms > best)) {
            have = 1;
            best = n->heard_mono_ms;
        }
    }
    if (have && ms) {
        *ms = best;
    }
    return have;
}

void rift_model_recent_frames(const struct rift_model *m, int64_t now_ms, int64_t window_ms,
                              int *rx, int *tx, int *at_least)
{
    int nrx = 0;
    int ntx = 0;
    int oldest_inside = 0;
    int i;

    if (m) {
        for (i = 0; i < m->activity_count; i++) {
            const struct rift_activity *a = rift_model_activity_at(m, i);
            int64_t age;

            if (!a || !a->have_mono) {
                continue;
            }
            age = now_ms - a->mono_ms;
            if (age < 0 || age > window_ms) {
                continue;
            }
            if (a->kind == RIFT_ACT_RX) {
                nrx++;
            } else {
                ntx++;
            }
            if (i == m->activity_count - 1) {
                oldest_inside = 1;
            }
        }
    }
    if (rx) {
        *rx = nrx;
    }
    if (tx) {
        *tx = ntx;
    }
    if (at_least) {
        *at_least = m && m->activity_count >= RIFT_MAX_ACTIVITY && oldest_inside;
    }
}
