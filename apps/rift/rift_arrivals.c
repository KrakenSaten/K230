/*
 * RIFT's new direct messages: which of the messages the model files are a
 * direct message that has genuinely just arrived - the one question the DM
 * sound asks (rift_notify.h). The five conditions are at
 * rift_model_apply_live_message in rift_model.h; this is where they are
 * applied, over the same struct the rest of the model writes, and after the
 * message itself has been filed by rift_messages.c exactly as any other.
 *
 * No LVGL, no sockets, no clock: host-tested by tests/rift_notify_test.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_model.h"

#include <stdio.h>
#include <string.h>

/* FNV-1a over what makes a message the same message to the person who sent
 * it: who, when by their clock, and what. Not a digest of anything secret -
 * only a way to recognise the same three, byte for byte, a second time. */
static uint32_t fp_add(uint32_t h, const char *s)
{
    for (; s && *s; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    h ^= 0xffu; /* a separator no text contains, so "ab"+"c" is not "a"+"bc" */
    return h * 16777619u;
}

static uint32_t dm_fingerprint(const struct rift_message *msg)
{
    char stamp[24];

    snprintf(stamp, sizeof(stamp), "%lld", (long long)msg->timestamp);
    return fp_add(fp_add(fp_add(2166136261u, msg->peer_key), stamp), msg->text);
}

static int dm_seen_fp(const struct rift_model *m, uint32_t fp)
{
    int i;

    for (i = 0; i < m->dm_recent_count; i++) {
        if (m->dm_recent_fp[i] == fp) {
            return 1;
        }
    }
    return 0;
}

static void dm_remember_fp(struct rift_model *m, uint32_t fp)
{
    m->dm_recent_fp[m->dm_recent_at] = fp;
    m->dm_recent_at = (m->dm_recent_at + 1) % RIFT_DM_RECENT;
    if (m->dm_recent_count < RIFT_DM_RECENT) {
        m->dm_recent_count++;
    }
}

int rift_model_apply_live_message(struct rift_model *m, const cJSON *o)
{
    struct rift_message *msg = NULL;
    int fresh = 0;
    int above;
    uint32_t fp = 0;

    if (rift_model_file_message(m, o, &msg, &fresh) != 0) {
        return -1;
    }
    /* The five conditions of rift_model.h, in order. Everything that is not
     * an incoming direct message is simply not this question. */
    if (!msg || msg->dir != RIFT_MSG_IN || msg->is_channel) {
        return 0;
    }
    above = msg->id > m->dm_high_id;
    if (above) {
        m->dm_high_id = msg->id;
    }
    if (!fresh || !above) {
        m->dm_repeats++;
        return 0;
    }
    if (msg->have_timestamp) {
        fp = dm_fingerprint(msg);
        if (dm_seen_fp(m, fp)) {
            m->dm_repeats++;
            return 0;
        }
        dm_remember_fp(m, fp);
    }
    m->dm_arrivals++;
    snprintf(m->dm_last_key, sizeof(m->dm_last_key), "%s", msg->peer_key);
    return 0;
}
