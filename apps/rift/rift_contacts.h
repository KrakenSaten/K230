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
 * hops, routes and signal. CONTACTS is who can be found and written to -
 * by name, A to Z, or the ones this session has a conversation with, and a
 * search over name and public key prefix. There is no favourite: upstream's
 * ContactInfo.flags has one bit for it (companion_radio: "LSB used as
 * 'favourite' bit") and state.v1 stores the byte, but meshcored has no
 * method that reads or sets it, so it is not offered here.
 *
 * Plain C, no LVGL: tested by tests/rift_model_test.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_CONTACTS_H
#define RIFT_CONTACTS_H

#include "rift_model.h"

enum rift_contacts_filter {
    /* Every contact the service stores, by name (case not counted; a node
     * with no name after the named ones, by key). */
    RIFT_CONTACTS_ALL = 0,
    /* The contacts with a direct conversation in this app's message window,
     * the one most recently spoken with first. The window is bounded and
     * does not survive the service's restart, and RECENT says no more. */
    RIFT_CONTACTS_RECENT,
};

/* The contacts that answer query (rift_node_matches: name, or two or more
 * hex characters of the public key - the node hash is the first two) in the
 * order the filter gives. Writes at most max pointers into m->nodes and
 * returns how many. Changes nothing. */
int rift_contacts_list(const struct rift_model *m, enum rift_contacts_filter filter,
                       const char *query, const struct rift_node **out, int max);

#endif
