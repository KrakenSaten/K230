/*
 * CONTACTS: the stored contacts as an address book (rift_contacts.h), under
 * COMMS the way NET is under NODES - reached from the conversation list's
 * header (or C on its keys), the COMMS tab stays lit, and Esc or "‹ COMMS"
 * goes back.
 *
 * ALL lists the favourites A to Z and then every other contact A to Z,
 * FAVORITES the favourites alone, RECENT the ones this session has a direct
 * conversation with; the search narrows any of them by name or by public
 * key prefix (two hex characters or more - the node hash is the first two).
 * Repeaters and sensors are not contacts here (rift_contacts.h): they are
 * NODES', with everything that administers them. A row is the star, the
 * name, what kind of node it is (a room server says ROOM), the key's first
 * eight hex characters and when it was last heard. A tap on the star (or F
 * on the selected row) makes it a favourite or not, kept in RIFT's
 * preferences by the whole key; a tap anywhere else (or Enter) opens the
 * conversation. Nothing here asks the service for anything or changes a
 * contact.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_CONTACTS_VIEW_H
#define RIFT_CONTACTS_VIEW_H

#include "rift_app.h"

lv_obj_t *rift_contacts_view_create(struct rift_app *app, lv_obj_t *parent);
void rift_contacts_view_destroy(struct rift_app *app);
void rift_contacts_view_shape(struct rift_app *app);
void rift_contacts_view_refresh(struct rift_app *app);
/* The list's keys: UP and DOWN move the selection, ENTER opens it, F (or *)
 * stars it or takes the star off. */
int rift_contacts_view_key(struct rift_app *app, uint32_t key);

/* For the tests: how many contacts the list holds now, the key of the i-th,
 * the rows in the pool, the search field, the filter buttons (by enum
 * rift_contacts_filter), and the star of the row bound to item i (NULL when
 * that item is not on screen). */
int rift_contacts_view_count(const struct rift_app *app);
const char *rift_contacts_view_key_at(const struct rift_app *app, int i);
int rift_contacts_view_rows_built(const struct rift_app *app);
lv_obj_t *rift_contacts_view_field(const struct rift_app *app);
lv_obj_t *rift_contacts_view_filter_button(const struct rift_app *app, int filter);
lv_obj_t *rift_contacts_view_star(const struct rift_app *app, int i);
/* The line under the list: why a node cannot be written to, or how many
 * there are. */
const char *rift_contacts_view_note(const struct rift_app *app);

#endif
