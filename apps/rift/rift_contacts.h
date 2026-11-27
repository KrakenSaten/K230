/*
 * RIFT's CONTACTS: the identities this node can address, as an address book.
 *
 * What a contact is, on this build (VERIFIED in the source, not assumed):
 * MeshCore keeps ONE table, BaseChatMesh::contacts[MAX_CONTACTS], and it is
 * both the "seen on the mesh" table and the "stored contacts" table - every
 * advert from a new node is added to it (isAutoAddEnabled() is upstream's
 * default true and meshcored does not override it), and a direct message
 * can only be encrypted to a node that is in it. meshcored persists that
 * table (state.v1, mesh_store.cpp) and lists it as mesh.nodes. MAX_CONTACTS
 * is 1000, fixed at compile time in protocols/meshcore/compat/mc_contacts.h
 * (an -D override is an #error); when it is full MeshCore evicts nothing
 * (shouldOverwriteWhenFull() is false), a new node's advert is counted as
 * nodes_unretained, and that node cannot be written to until one is
 * forgotten. RIFT holds all of it (RIFT_MAX_NODES is the same 1000).
 *
 * So CONTACTS is not a second table: it is the same stored contacts asked a
 * different question. NODES is topology - heard most recently first, with
 * hops, routes and signal, and every repeater and sensor. CONTACTS is who
 * can be found and written to - by name, A to Z, the favourites, or the ones
 * this session has a conversation with, and a search over name and public
 * key prefix - so a node whose advertised type is a repeater (2) or a
 * sensor (4) is in none of its lists: those are infrastructure, administered
 * from NODES. A room server (3) is a contact, and says ROOM on its row; a
 * node whose type is not known yet is not left out on a guess.
 *
 * Favourites are this app's, not MeshCore's. Upstream's ContactInfo.flags
 * has a bit for one (companion_radio: "LSB used as 'favourite' bit") and
 * state.v1 stores the byte, but meshcored has no method that reads or sets
 * it, and changing the contact table for a reader's bookmark is not this
 * app's to do. So a favourite is a whole public key in RIFT's preferences
 * file (rift_store.h, favorite=) and nothing else: the table, the node and
 * NODES are exactly as they were.
 *
 * Plain C, no LVGL: tested by tests/rift_reliability_test.c and
 * tests/rift_organize_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_CONTACTS_H
#define RIFT_CONTACTS_H

#include "rift_model.h"
#include "rift_store.h"

enum rift_contacts_filter {
    /* Every contact the service stores: the favourites first, then the
     * rest, each by name (case not counted; a node with no name after the
     * named ones, by key). */
    RIFT_CONTACTS_ALL = 0,
    /* The contacts with a direct conversation in this app's message window,
     * the one most recently spoken with first. The window is bounded and
     * does not survive the service's restart, and RECENT says no more. */
    RIFT_CONTACTS_RECENT,
    /* The favourites the table holds, by name. */
    RIFT_CONTACTS_FAVORITES,
    RIFT_CONTACTS_FILTERS,
};

/* Whether a node is a contact here: not a repeater or a sensor by its
 * advertised type. NULL is not one. */
int rift_contacts_is_contact(const struct rift_node *n);
/* The contacts that answer query (rift_node_matches: name, or two or more
 * hex characters of the public key - the node hash is the first two) in the
 * order the filter gives; favs (may be NULL: none) says which are
 * favourites. Writes at most max pointers into m->nodes and returns how
 * many. Changes nothing. */
int rift_contacts_list(const struct rift_model *m, const struct rift_prefs *favs,
                       enum rift_contacts_filter filter, const char *query,
                       const struct rift_node **out, int max);
/* How many favourites the table does not hold now (forgotten, or not heard
 * since the table was emptied). */
int rift_contacts_favorites_absent(const struct rift_model *m, const struct rift_prefs *favs);
/* "ALL", "RECENT", "FAVORITES". */
const char *rift_contacts_filter_word(enum rift_contacts_filter filter);

#endif
