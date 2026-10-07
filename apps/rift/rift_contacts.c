/*
 * CONTACTS: the stored contacts as an address book. See rift_contacts.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_contacts.h"

#include <stdlib.h>
#include <string.h>

/* A to Z with ASCII case not counted, which is what a reader looking for a
 * name expects; equal names, and nameless nodes after every named one, by
 * key, so the order is total and a list does not shuffle on a repaint. */
static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int by_name(const void *pa, const void *pb)
{
    const struct rift_node *a = *(const struct rift_node *const *)pa;
    const struct rift_node *b = *(const struct rift_node *const *)pb;
    int an = a->have_name && a->name[0];
    int bn = b->have_name && b->name[0];

    if (an != bn) {
        return an ? -1 : 1;
    }
    if (an) {
        const unsigned char *x = (const unsigned char *)a->name;
        const unsigned char *y = (const unsigned char *)b->name;

        while (*x && lower(*x) == lower(*y)) {
            x++;
            y++;
        }
        if (lower(*x) != lower(*y)) {
            return lower(*x) - lower(*y);
        }
    }
    return strcmp(a->key, b->key);
}

int rift_contacts_list(const struct rift_model *m, enum rift_contacts_filter filter,
                       const char *query, const struct rift_node **out, int max)
{
    int n = 0;
    int i;

    if (!m || !out || max <= 0) {
        return 0;
    }
    if (filter == RIFT_CONTACTS_RECENT) {
        /* Newest message first, so each peer is met first at its newest
         * message and the list comes out in the order they were spoken
         * with. A peer the table no longer holds is not a contact. */
        for (i = m->msg_count - 1; i >= 0 && n < max; i--) {
            const struct rift_message *msg = &m->msg[i];
            const struct rift_node *node;
            int j;

            if (msg->is_channel || !msg->peer_key[0]) {
                continue;
            }
            for (j = 0; j < n; j++) {
                if (strcmp(out[j]->key, msg->peer_key) == 0) {
                    break;
                }
            }
            if (j < n) {
                continue;
            }
            node = rift_model_find(m, msg->peer_key);
            if (node && rift_node_matches(node, query)) {
                out[n++] = node;
            }
        }
        return n;
    }
    for (i = 0; i < m->node_count && n < max; i++) {
        if (rift_node_matches(&m->nodes[i], query)) {
            out[n++] = &m->nodes[i];
        }
    }
    qsort(out, (size_t)n, sizeof(out[0]), by_name);
    return n;
}
