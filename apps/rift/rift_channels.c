/*
 * RIFT's channels: the table mesh.channels reports, and the mesh.channel
 * events that change it. The third translation unit over struct rift_model,
 * beside rift_model.c (the service and the nodes) and rift_messages.c (the
 * messages); all three obey the rules at the top of rift_model.h.
 *
 * There is no key here and no way to ask for one: meshcored does not report
 * it through any method (docs/api/mesh.md). What this holds is what a screen
 * needs - the slot a channel is named by, the local name, the one-byte hash
 * that actually goes on the air, and how long a body may be on it - and the
 * conversation key a channel is filed under, which is built from those three
 * and not from the slot alone (rift_model.h, RIFT_KEY_HEX).
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

/* ---- the conversation key -----------------------------------------------
 *
 * A direct conversation is keyed by the peer's public key, which is 64 hex
 * characters. A channel is keyed by its slot, its hash and a fingerprint of
 * its local name (rift_model.h, RIFT_KEY_HEX, says why the slot alone is not
 * enough). The two cannot collide - no hex character is '#' - and that is
 * what lets a channel be an ordinary conversation everywhere below rather
 * than a second implementation of the list, the thread, the read mark and
 * the unread count.
 */

/* FNV-1a over the name as this app holds it - both the list and every
 * message keep it through rift_utf8_copy into RIFT_CHANNEL_NAME_MAX, so the
 * same name gives the same bytes whichever it came from. */
static uint32_t name_fingerprint(const char *name)
{
    uint32_t h = 2166136261u;

    for (; *name; name++) {
        h ^= (uint8_t)*name;
        h *= 16777619u;
    }
    return h;
}

static int is_hex_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

void rift_channel_key(int slot, const char *hash, const char *name, char *out, size_t out_len)
{
    char lower[RIFT_HASH_HEX];
    int i;

    if (!out || out_len == 0) {
        return;
    }
    if (slot < 0 || slot >= RIFT_MAX_CHANNELS) {
        out[0] = '\0';
        return;
    }
    if (!hash || !is_hex_char(hash[0]) || !is_hex_char(hash[1]) || hash[2] || !name || !name[0]) {
        snprintf(out, out_len, "#%d:?", slot);
        return;
    }
    for (i = 0; i < 2; i++) {
        lower[i] = (hash[i] >= 'A' && hash[i] <= 'F') ? (char)(hash[i] - 'A' + 'a') : hash[i];
    }
    lower[2] = '\0';
    snprintf(out, out_len, "#%d:%s:%08x", slot, lower, (unsigned)name_fingerprint(name));
}

void rift_channel_conv_key(const struct rift_channel *ch, char *out, size_t out_len)
{
    if (!ch) {
        if (out && out_len) {
            out[0] = '\0';
        }
        return;
    }
    rift_channel_key(ch->slot, ch->have_hash ? ch->hash : NULL, ch->have_name ? ch->name : NULL,
                     out, out_len);
}

int rift_key_is_channel(const char *key)
{
    int slot = 0;
    int i;

    if (!key || key[0] != '#' || key[1] < '0' || key[1] > '9') {
        return -1;
    }
    for (i = 1; key[i] >= '0' && key[i] <= '9'; i++) {
        slot = slot * 10 + (key[i] - '0');
        if (slot >= RIFT_MAX_CHANNELS) {
            return -1;
        }
    }
    if (key[i] != ':') {
        return -1;
    }
    i++;
    if (key[i] == '?' && !key[i + 1]) {
        return slot;
    }
    /* <2 lowercase hex>:<8 lowercase hex>, exactly as rift_channel_key writes it */
    {
        static const char shape[] = "hh:ffffffff";
        int k;

        for (k = 0; shape[k]; k++, i++) {
            char c = key[i];

            if (shape[k] == ':') {
                if (c != ':') {
                    return -1;
                }
            } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return -1;
            }
        }
        if (key[i]) {
            return -1;
        }
    }
    return slot;
}

const struct rift_channel *rift_model_key_channel(const struct rift_model *m, const char *key)
{
    const struct rift_channel *ch;
    char now[RIFT_KEY_HEX];
    int slot = rift_key_is_channel(key);

    if (!m || slot < 0) {
        return NULL;
    }
    ch = rift_model_channel(m, slot);
    if (!ch) {
        return NULL;
    }
    rift_channel_conv_key(ch, now, sizeof(now));
    /* A channel the list cannot identify has the "#<slot>:?" key, which is
     * never a live one: nothing may be written to what cannot be told apart. */
    if (strchr(now, '?') || strcmp(now, key) != 0) {
        return NULL;
    }
    return ch;
}
