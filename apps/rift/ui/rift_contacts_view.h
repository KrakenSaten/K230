/*
 * CONTACTS: the stored contacts as an address book (rift_contacts.h), under
 * COMMS the way NET is under NODES - reached from the conversation list's
 * header (or C on its keys), the COMMS tab stays lit, and Esc or "‹ COMMS"
 * goes back.
 *
 * ALL lists every contact A to Z, RECENT the ones this session has a direct
 * conversation with; the search narrows either by name or by public key
 * prefix (two hex characters or more - the node hash is the first two). A
 * row is the name, what kind of node it is, the key's first eight hex
 * characters and when it was last heard; a tap (or Enter) on a node that
 * takes direct messages opens its conversation, and one that does not - a
 * repeater, a sensor (rift_node_can_message) - says why instead. Nothing
 * here asks the service for anything or changes a contact.
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
/* The list's keys: UP and DOWN move the selection, ENTER opens it. */
int rift_contacts_view_key(struct rift_app *app, uint32_t key);

/* For the tests: how many contacts the list holds now, the key of the i-th,
 * the rows in the pool, the search field, and the ALL / RECENT buttons. */
int rift_contacts_view_count(const struct rift_app *app);
const char *rift_contacts_view_key_at(const struct rift_app *app, int i);
int rift_contacts_view_rows_built(const struct rift_app *app);
lv_obj_t *rift_contacts_view_field(const struct rift_app *app);
lv_obj_t *rift_contacts_view_filter_button(const struct rift_app *app, int recent);
/* The line under the list: why a node cannot be written to, or how many
 * there are. */
const char *rift_contacts_view_note(const struct rift_app *app);

#endif
