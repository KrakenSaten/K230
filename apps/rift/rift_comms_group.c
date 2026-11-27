/*
 * COMMS' three groups. See rift_comms_group.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_comms_group.h"

#include <string.h>

enum rift_conv_group rift_conv_group_of(const struct rift_model *m, const struct rift_conv *c)
{
    const struct rift_node *n;

    if (!c || c->is_channel) {
        return RIFT_GROUP_CHANNELS;
    }
    n = m ? rift_model_find(m, c->key) : NULL;
    /* The advertised type, and only that: a node whose type nobody has
     * said is a direct message, not a room on a guess. */
    if (n && n->have_type && n->type == RIFT_NODE_TYPE_ROOM) {
        return RIFT_GROUP_ROOMS;
    }
    return RIFT_GROUP_DIRECT;
}

int rift_conv_group_sort(const struct rift_model *m, struct rift_conv *list, int count,
                         int counts[RIFT_GROUP_COUNT], int unread[RIFT_GROUP_COUNT])
{
    /* Static, as COMMS' own list is: 256 conversations of a few hundred
     * bytes, on the one thread that draws. */
    static struct rift_conv out[RIFT_MAX_CONVERSATIONS];
    static unsigned char group[RIFT_MAX_CONVERSATIONS];
    static unsigned char dropped[RIFT_MAX_CONVERSATIONS];
    int kept = 0;
    int g;
    int i;
    int j;

    for (g = 0; g < RIFT_GROUP_COUNT; g++) {
        if (counts) {
            counts[g] = 0;
        }
        if (unread) {
            unread[g] = 0;
        }
    }
    if (!list || count <= 0) {
        return 0;
    }
    if (count > RIFT_MAX_CONVERSATIONS) {
        count = RIFT_MAX_CONVERSATIONS;
    }
    /* Public first among the channels: the service's word about the key,
     * which moves it to the front of the whole list, and so of its group. */
    (void)rift_conv_public_first(m, list, count);
    for (i = 0; i < count; i++) {
        dropped[i] = 0;
        for (j = 0; j < i; j++) {
            if (!dropped[j] && strcmp(list[j].key, list[i].key) == 0) {
                dropped[i] = 1;
                break;
            }
        }
        group[i] = (unsigned char)rift_conv_group_of(m, &list[i]);
    }
    for (g = 0; g < RIFT_GROUP_COUNT; g++) {
        /* Channels keep the order they had. The other two put what is
         * unread first; each pass keeps the order the rows had, newest
         * first, so "recent" is what it was. */
        int passes = g == RIFT_GROUP_CHANNELS ? 1 : 2;
        int p;

        for (p = 0; p < passes; p++) {
            for (i = 0; i < count; i++) {
                if (dropped[i] || group[i] != g) {
                    continue;
                }
                if (passes == 2 && (p == 0) != (list[i].unread > 0)) {
                    continue;
                }
                out[kept++] = list[i];
                if (counts) {
                    counts[g]++;
                }
                if (unread && list[i].unread > 0) {
                    unread[g] += list[i].unread;
                }
            }
        }
    }
    memcpy(list, out, sizeof(out[0]) * (size_t)kept);
    return kept;
}

const char *rift_conv_group_word(enum rift_conv_group g)
{
    switch (g) {
    case RIFT_GROUP_CHANNELS:
        return "CHANNELS";
    case RIFT_GROUP_DIRECT:
        return "DIRECT MESSAGES";
    case RIFT_GROUP_ROOMS:
        return "CHAT ROOMS";
    default:
        return "";
    }
}
