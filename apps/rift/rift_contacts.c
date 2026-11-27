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

/* A favourite, read straight off the preferences: both sides are whole
 * lowercase keys (rift_store.c writes them so, the model holds them so), and
 * the model's tests link no file store. */
static int is_favorite(const struct rift_prefs *favs, const struct rift_node *n)
{
    int i;

    for (i = 0; favs && i < favs->fav_count; i++) {
        if (strcmp(favs->fav[i], n->key) == 0) {
            return 1;
        }
    }
    return 0;
}

int rift_contacts_is_contact(const struct rift_node *n)
{
    return n && !(n->have_type &&
                  (n->type == RIFT_NODE_TYPE_REPEATER || n->type == RIFT_NODE_TYPE_SENSOR));
}

int rift_contacts_list(const struct rift_model *m, const struct rift_prefs *favs,
                       enum rift_contacts_filter filter, const char *query,
                       const struct rift_node **out, int max)
{
    int n = 0;
    int first;
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
            if (rift_contacts_is_contact(node) && rift_node_matches(node, query)) {
                out[n++] = node;
            }
        }
        return n;
    }
    /* The favourites first, A to Z, then - for ALL - everyone else, A to Z.
     * Two runs over the table rather than one sort with a favourite key, so
     * the order within each is exactly the plain A to Z. */
    for (i = 0; i < m->node_count && n < max; i++) {
        const struct rift_node *node = &m->nodes[i];

        if (rift_contacts_is_contact(node) && is_favorite(favs, node) &&
            rift_node_matches(node, query)) {
            out[n++] = node;
        }
    }
    qsort(out, (size_t)n, sizeof(out[0]), by_name);
    if (filter == RIFT_CONTACTS_FAVORITES) {
        return n;
    }
    first = n;
    for (i = 0; i < m->node_count && n < max; i++) {
        const struct rift_node *node = &m->nodes[i];

        if (rift_contacts_is_contact(node) && !is_favorite(favs, node) &&
            rift_node_matches(node, query)) {
            out[n++] = node;
        }
    }
    qsort(out + first, (size_t)(n - first), sizeof(out[0]), by_name);
    return n;
}

int rift_contacts_favorites_absent(const struct rift_model *m, const struct rift_prefs *favs)
{
    int absent = 0;
    int i;

    for (i = 0; m && favs && i < favs->fav_count; i++) {
        absent += rift_model_find(m, favs->fav[i]) == NULL;
    }
    return absent;
}

const char *rift_contacts_filter_word(enum rift_contacts_filter filter)
{
    switch (filter) {
    case RIFT_CONTACTS_RECENT:
        return "RECENT";
    case RIFT_CONTACTS_FAVORITES:
        return "FAVORITES";
    case RIFT_CONTACTS_ALL:
    default:
        return "ALL";
    }
}
