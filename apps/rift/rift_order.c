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

/* ---- searching the node list ----------------------------------------------
 *
 * What a reader can type to find a node: part of its name, or the first hex
 * of its key - the node hash, which is what a hop is written as. Nothing
 * here changes a node or the cache; a filter is a question asked of the
 * order the list already has. */

/* Fold for comparing: ASCII upper case, and the Latin-1 capitals of UTF-8
 * (C3 80..C3 9E, but not the multiplication sign C3 97) onto their small
 * letters, which is every letter a Norwegian name has (Æ Ø Å). Anything else
 * is compared byte for byte, so a search never matches half a character. */
static size_t fold(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    size_t i;

    if (!out || out_len == 0) {
        return 0;
    }
    for (i = 0; in && in[i] && o + 1 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];

        if (c >= 'A' && c <= 'Z') {
            out[o++] = (char)(c - 'A' + 'a');
        } else if (c == 0xC3 && in[i + 1] && o + 2 < out_len) {
            unsigned char d = (unsigned char)in[i + 1];

            out[o++] = (char)c;
            out[o++] = (char)((d >= 0x80 && d <= 0x9E && d != 0x97) ? d + 0x20 : d);
            i++;
        } else if (c == 0xC3) {
            break; /* no room for the whole character: stop before it */
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return o;
}

/* The query without the spaces around it, folded. 0 when nothing is left. */
static size_t query_of(const char *query, char *out, size_t out_len)
{
    char trimmed[RIFT_QUERY_MAX];
    size_t len;
    size_t start = 0;

    if (!query) {
        out[0] = '\0';
        return 0;
    }
    while (query[start] == ' ' || query[start] == '\t') {
        start++;
    }
    rift_utf8_copy(trimmed, sizeof(trimmed), query + start);
    len = strlen(trimmed);
    while (len > 0 && (trimmed[len - 1] == ' ' || trimmed[len - 1] == '\t')) {
        trimmed[--len] = '\0';
    }
    return fold(trimmed, out, out_len);
}

static int all_hex(const char *s)
{
    size_t i;

    for (i = 0; s[i]; i++) {
        char c = s[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return i > 0;
}

int rift_node_matches(const struct rift_node *n, const char *query)
{
    char q[RIFT_QUERY_MAX];
    char name[RIFT_NAME_MAX];
    size_t len;

    if (!n) {
        return 0;
    }
    len = query_of(query, q, sizeof(q));
    if (len == 0) {
        return 1; /* nothing asked: everything answers */
    }
    if (n->have_name && n->name[0]) {
        fold(n->name, name, sizeof(name));
        if (strstr(name, q)) {
            return 1;
        }
    }
    /* Two hex characters at least: one would be a sixteenth of the mesh,
     * which is not finding anything. The key is stored in lower case. */
    if (len >= 2 && len <= 64 && all_hex(q) && strncmp(n->key, q, len) == 0) {
        return 1;
    }
    return 0;
}

int rift_node_is_repeater(const struct rift_node *n)
{
    return n && n->have_type && n->type == RIFT_NODE_TYPE_REPEATER;
}

int rift_node_can_message(const struct rift_node *n)
{
    return !n || !n->have_type ||
           (n->type != RIFT_NODE_TYPE_REPEATER && n->type != RIFT_NODE_TYPE_SENSOR);
}

const char *rift_node_no_message_why(const struct rift_node *n)
{
    if (rift_node_can_message(n)) {
        return NULL;
    }
    return n->type == RIFT_NODE_TYPE_REPEATER
               ? "A repeater takes no direct messages: it reads text only from a logged-in "
                 "admin, as a command."
               : "A sensor takes no direct messages: it reads text only from a logged-in "
                 "admin, as a command.";
}

int rift_node_zero_hop(const struct rift_node *n)
{
    if (!n) {
        return 0;
    }
    /* Heard straight from it: its last advert came through no relay, or the
     * route the service learned back to it has none. Either is a fact the
     * service reported; nothing is inferred from a strong signal. */
    return (n->have_advert_hops && n->advert_hops == 0) || (n->path_known && n->direct);
}

int rift_node_filter(const struct rift_node **list, int count, const char *query,
                     int zero_hop_repeaters)
{
    int kept = 0;
    int i;

    if (!list || count <= 0) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        const struct rift_node *n = list[i];

        if (!rift_node_matches(n, query)) {
            continue;
        }
        if (zero_hop_repeaters && !(rift_node_is_repeater(n) && rift_node_zero_hop(n))) {
            continue;
        }
        list[kept++] = n;
    }
    return kept;
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
