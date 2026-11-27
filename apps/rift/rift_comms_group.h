/*
 * COMMS' three groups: CHANNELS, DIRECT MESSAGES, CHAT ROOMS.
 *
 * One list still, one row per conversation, the same rows (DS §37.2) - only
 * in three runs, each under its label, in this order:
 *
 *   - CHANNELS: every channel conversation. MeshCore's Public channel first,
 *     known by the service's word about its key (rift_conv_public_first,
 *     mesh.channels well_known), then the order they had: newest talk
 *     first, then the joined channels nothing has been said on.
 *   - DIRECT MESSAGES: a conversation with a peer that is not known to be a
 *     room server - a chat node, or a node whose type the table has not said
 *     (an unknown type is never guessed to be a room). Unread first, then
 *     the most recent; within each, the order they had.
 *   - CHAT ROOMS: a conversation with a node whose advertised type is a
 *     room server (MeshCore ADV_TYPE_ROOM, 3) - by type, never by name.
 *     Ordered as the direct messages are.
 *
 * Nothing is sent to a repeater or a sensor (rift_node_can_message); should
 * the message window hold a conversation with one all the same, it stays
 * under DIRECT MESSAGES - it is not a room. A key that appears twice is
 * shown once, where it first appeared.
 *
 * No LVGL: tested by tests/rift_organize_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_COMMS_GROUP_H
#define RIFT_COMMS_GROUP_H

#include "rift_model.h"

#define RIFT_NODE_TYPE_ROOM 3

enum rift_conv_group {
    RIFT_GROUP_CHANNELS = 0,
    RIFT_GROUP_DIRECT,
    RIFT_GROUP_ROOMS,
    RIFT_GROUP_COUNT,
};

/* Which group a conversation is in. */
enum rift_conv_group rift_conv_group_of(const struct rift_model *m, const struct rift_conv *c);
/* list[0..count) put in group order, in place, as above; duplicates of a
 * key dropped. Fills counts[RIFT_GROUP_COUNT] with each group's rows and
 * unread[RIFT_GROUP_COUNT] (may be NULL) with each group's unread messages.
 * Returns the rows kept. */
int rift_conv_group_sort(const struct rift_model *m, struct rift_conv *list, int count,
                         int counts[RIFT_GROUP_COUNT], int unread[RIFT_GROUP_COUNT]);
/* "CHANNELS", "DIRECT MESSAGES", "CHAT ROOMS". */
const char *rift_conv_group_word(enum rift_conv_group g);

#endif
