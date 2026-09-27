/*
 * Frames heard per minute. See rift_traffic.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_traffic.h"

#include <string.h>

static int slot_of(int64_t minute)
{
    int64_t s = minute % RIFT_TRAFFIC_MINUTES;

    return (int)(s < 0 ? s + RIFT_TRAFFIC_MINUTES : s);
}

static int64_t minute_of(int64_t mono_ms)
{
    int64_t m = mono_ms / RIFT_TRAFFIC_MINUTE_MS;

    /* Floor, not truncation: a fixture's "this long ago" is negative, and
     * -0.5 of a minute is the minute before, not this one. */
    if (mono_ms < 0 && m * RIFT_TRAFFIC_MINUTE_MS != mono_ms) {
        m--;
    }
    return m;
}

void rift_traffic_init(struct rift_traffic *t)
{
    if (t) {
        memset(t, 0, sizeof(*t));
    }
}

enum rift_traffic_class rift_traffic_class_of(const char *payload_type)
{
    if (!payload_type) {
        return RIFT_TRAFFIC_OTHER;
    }
    /* The words are the service's (docs/api/mesh.md, mesh_runtime.cpp):
     * what carries something somebody said, and what announces somebody.
     * Everything else - acks, path replies, requests, traces, control - is
     * the mesh doing its work, and is counted as that. */
    if (strcmp(payload_type, "text") == 0 || strcmp(payload_type, "group_text") == 0 ||
        strcmp(payload_type, "group_data") == 0) {
        return RIFT_TRAFFIC_MSG;
    }
    if (strcmp(payload_type, "advert") == 0) {
        return RIFT_TRAFFIC_ADV;
    }
    return RIFT_TRAFFIC_OTHER;
}

/* Move the newest minute on to `minute`, clearing every minute passed over
 * (a jump of twenty or more clears the ring). */
static void roll(struct rift_traffic *t, int64_t minute)
{
    int64_t passed;
    int64_t m;

    if (!t->started) {
        memset(t->count, 0, sizeof(t->count));
        t->minute = minute;
        t->started = 1;
        return;
    }
    if (minute <= t->minute) {
        return;
    }
    passed = minute - t->minute;
    if (passed >= RIFT_TRAFFIC_MINUTES) {
        memset(t->count, 0, sizeof(t->count));
    } else {
        for (m = t->minute + 1; m <= minute; m++) {
            memset(t->count[slot_of(m)], 0, sizeof(t->count[0]));
        }
    }
    t->minute = minute;
}

void rift_traffic_note(struct rift_traffic *t, int64_t mono_ms, enum rift_traffic_class c)
{
    int64_t minute;
    uint16_t *n;

    if (!t || c < 0 || c >= RIFT_TRAFFIC_CLASSES) {
        return;
    }
    minute = minute_of(mono_ms);
    roll(t, minute);
    if (minute < t->minute - (RIFT_TRAFFIC_MINUTES - 1)) {
        /* Older than the window: a frame the feed delivered late, or a
         * fixture from long ago. Not in the last twenty minutes, so not
         * drawn as if it were. */
        return;
    }
    n = &t->count[slot_of(minute)][c];
    if (*n < 0xffff) {
        (*n)++;
    }
}

void rift_traffic_read(const struct rift_traffic *t, int64_t now_ms, struct rift_traffic_bins *out)
{
    int64_t newest;
    int i;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!t || !t->started) {
        return;
    }
    out->started = 1;
    /* The window ends now: minutes since the last frame are quiet minutes,
     * and a graph that stopped at the last frame would say the mesh was as
     * busy as it last was. A clock that has not reached the newest note
     * (a frame stamped slightly ahead) ends at the note instead. */
    newest = minute_of(now_ms);
    if (newest < t->minute) {
        newest = t->minute;
    }
    for (i = 0; i < RIFT_TRAFFIC_MINUTES; i++) {
        int64_t m = newest - (RIFT_TRAFFIC_MINUTES - 1) + i;

        if (m > t->minute || m < t->minute - (RIFT_TRAFFIC_MINUTES - 1)) {
            continue;
        }
        memcpy(out->count[i], t->count[slot_of(m)], sizeof(out->count[i]));
    }
}

unsigned rift_traffic_peak(const struct rift_traffic_bins *b)
{
    unsigned peak = 0;
    int i;
    int c;

    if (!b) {
        return 0;
    }
    for (i = 0; i < RIFT_TRAFFIC_MINUTES; i++) {
        unsigned total = 0;

        for (c = 0; c < RIFT_TRAFFIC_CLASSES; c++) {
            total += b->count[i][c];
        }
        if (total > peak) {
            peak = total;
        }
    }
    return peak;
}
