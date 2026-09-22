/*
 * RIFT's channels: the table mesh.channels reports, and the mesh.channel
 * events that change it. The third translation unit over struct rift_model,
 * beside rift_model.c (the service and the nodes) and rift_messages.c (the
 * messages); all three obey the rules at the top of rift_model.h.
 *
 * There is no key here and no way to ask for one: meshcored does not report
 * it through any method (docs/api/mesh.md). What this holds is what a screen
 * needs - the slot a channel is named by, the local name, the one-byte hash
 * that actually goes on the air, and how long a body may be on it.
 *
 * No LVGL and no sockets: host-tested by tests/rift_model_test.c and
 * tests/rift_comms_test.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_model.h"

#include "rift_format.h"
#include "rift_json.h"

#include <stdio.h>
#include <string.h>

const struct rift_channel *rift_model_channel(const struct rift_model *m, int slot)
{
    int i;

    if (!m || slot < 0) {
        return NULL;
    }
    for (i = 0; i < m->channel_count; i++) {
        if (m->channels[i].slot == slot) {
            return &m->channels[i];
        }
    }
    return NULL;
}

/* A channel slot, only when it is one this build can hold: a whole number in
 * range. Returns the slot, or -1. */
static int slot_of(const cJSON *o)
{
    double d;

    if (!num_of(o, "channel", &d) || d < 0 || d >= (double)RIFT_MAX_CHANNELS ||
        d != (double)(int)d) {
        return -1;
    }
    return (int)d;
}

/* One channel object, in the shape of docs/api/mesh.md, mesh.channels.
 * Returns 0 when it was taken, -1 when it was not one this model will hold. */
static int apply_channel(struct rift_model *m, const cJSON *o)
{
    struct rift_channel *ch = NULL;
    const char *name;
    const char *hash;
    double d;
    int slot;
    int i;

    if (!cJSON_IsObject(o)) {
        return -1;
    }
    /* The slot is the identity a channel is named by, so a channel without a
     * usable one is refused rather than held as a row nothing can address. */
    slot = slot_of(o);
    if (slot < 0) {
        return -1;
    }
    for (i = 0; i < m->channel_count; i++) {
        if (m->channels[i].slot == slot) {
            ch = &m->channels[i];
            break;
        }
    }
    if (!ch) {
        if (m->channel_count >= RIFT_MAX_CHANNELS) {
            /* The service holds more channels than this build can show.
             * Counted rather than silently dropped, so a screen can say the
             * list is short instead of implying it is complete. */
            m->channels_dropped++;
            return -1;
        }
        ch = &m->channels[m->channel_count++];
    }
    memset(ch, 0, sizeof(*ch));
    ch->slot = slot;
    name = str_of(o, "name");
    if (name && name[0]) {
        rift_utf8_copy(ch->name, sizeof(ch->name), name);
        ch->have_name = 1;
    }
    hash = str_of(o, "channel_hash");
    if (hex_only(hash, 2)) {
        snprintf(ch->hash, sizeof(ch->hash), "%s", hash);
        ch->have_hash = 1;
    }
    if (num_of(o, "key_bits", &d)) {
        ch->have_key_bits = 1;
        ch->key_bits = (int)d;
    }
    if (num_of(o, "text_limit", &d) && d > 0) {
        ch->have_text_limit = 1;
        ch->text_limit = (int)d;
    }
    return 0;
}

int rift_model_apply_channels(struct rift_model *m, const cJSON *result)
{
    const cJSON *arr;
    const cJSON *item;
    double d;

    if (!m || !cJSON_IsObject(result)) {
        return -1;
    }
    arr = cJSON_GetObjectItemCaseSensitive(result, "channels");
    if (!cJSON_IsArray(arr)) {
        return -1;
    }
    /* A snapshot replaces the list, for the reason mesh.nodes does: a
     * channel the service no longer holds has been left, and keeping it here
     * would offer a reader somewhere to write that nothing would carry. */
    m->channel_count = 0;
    memset(m->channels, 0, sizeof(m->channels));
    cJSON_ArrayForEach (item, arr) {
        if (apply_channel(m, item) != 0) {
            m->events_malformed++;
        }
    }
    m->have_channels_reported = num_of(result, "count", &d);
    if (m->have_channels_reported) {
        m->channels_reported = (int)d;
    }
    if (num_of(result, "max", &d)) {
        m->channels_max = (int)d;
    }
    m->channels_valid = 1;
    m->stale = 0;
    return 0;
}

int rift_model_apply_channel_event(struct rift_model *m, const cJSON *data)
{
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(data, "channel");
    const char *reason = str_of(data, "reason");
    int slot;
    int i;

    if (!m || !cJSON_IsObject(ch) || !reason) {
        return -1;
    }
    slot = slot_of(ch);
    if (slot < 0) {
        return -1;
    }
    if (strcmp(reason, "removed") != 0) {
        return apply_channel(m, ch);
    }
    /* The channel is gone from the service. It goes from the list too, so
     * nothing offers a reader somewhere to write that nothing would carry.
     * Its messages stay: they happened, and they are still what this node
     * heard. */
    for (i = 0; i < m->channel_count; i++) {
        if (m->channels[i].slot != slot) {
            continue;
        }
        for (; i + 1 < m->channel_count; i++) {
            m->channels[i] = m->channels[i + 1];
        }
        m->channel_count--;
        memset(&m->channels[m->channel_count], 0, sizeof(m->channels[0]));
        break;
    }
    return 0;
}
