/*
 * Frames heard, by type, in five-second slots. See rift_air.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_air.h"

#include "rift_model.h"

#include <string.h>

/* Floor division: a fixture's "this long ago" is negative, and half a slot
 * before zero is the slot before, not slot zero. */
static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;

    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static int ring_of(int64_t slot)
{
    int64_t s = slot % RIFT_AIR_SLOTS;

    return (int)(s < 0 ? s + RIFT_AIR_SLOTS : s);
}

void rift_air_init(struct rift_air *t)
{
    if (t) {
        memset(t, 0, sizeof(*t));
    }
}

static const struct {
    const char *word;   /* the service's */
    const char *legend; /* the screen's */
} types[RIFT_AIR_TYPES] = {
    { "advert", "ADVERT" },     { "group_text", "CHANNEL" }, { "group_data", "DATA" },
    { "text", "DIRECT" },       { "ack", "ACK" },            { "req", "REQ" },
    { "response", "RESP" },     { "trace", "TRACE" },        { "path", "PATH" },
    { "anon_req", "ANON" },     { "multipart", "MULTI" },    { "control", "CTRL" },
    { "raw_custom", "RAW" },    { NULL, "OTHER" },
};

enum rift_air_type rift_air_type_of(const char *payload_type)
{
    int i;

    for (i = 0; payload_type && i < RIFT_AIR_TYPES; i++) {
        if (types[i].word && strcmp(payload_type, types[i].word) == 0) {
            return (enum rift_air_type)i;
        }
    }
    return RIFT_AIR_UNKNOWN;
}

const char *rift_air_type_word(enum rift_air_type type)
{
    return types[(unsigned)type < RIFT_AIR_TYPES ? (unsigned)type : RIFT_AIR_UNKNOWN].legend;
}

/* Move the newest slot on, clearing every slot passed over. */
static void roll(struct rift_air *t, int64_t slot)
{
    int64_t s;

    if (!t->started) {
        memset(t->count, 0, sizeof(t->count));
        t->slot = slot;
        t->started = 1;
        return;
    }
    if (slot <= t->slot) {
        return;
    }
    if (slot - t->slot >= RIFT_AIR_SLOTS) {
        memset(t->count, 0, sizeof(t->count));
    } else {
        for (s = t->slot + 1; s <= slot; s++) {
            memset(t->count[ring_of(s)], 0, sizeof(t->count[0]));
        }
    }
    t->slot = slot;
}

static void remember(struct rift_air *t, const struct rift_air_packet *p)
{
    t->recent_head = (t->recent_head + 1) % RIFT_AIR_RECENT;
    t->recent[t->recent_head] = *p;
    if (t->recent_count < RIFT_AIR_RECENT) {
        t->recent_count++;
    }
    t->serial++;
}

void rift_air_note_rx(struct rift_air *t, int64_t mono_ms, enum rift_air_type type, int bytes,
                      int have_rssi, double rssi_dbm)
{
    struct rift_air_packet p;
    int64_t slot;

    if (!t || (unsigned)type >= RIFT_AIR_TYPES) {
        return;
    }
    slot = floor_div(mono_ms, RIFT_AIR_SLOT_MS);
    roll(t, slot);
    if (slot > t->slot - RIFT_AIR_SLOTS) {
        uint8_t *n = &t->count[ring_of(slot)][type];

        if (*n < 0xff) {
            (*n)++;
        }
    }
    memset(&p, 0, sizeof(p));
    p.mono_ms = mono_ms;
    p.type = (uint8_t)type;
    p.bytes = (int16_t)(bytes < 0 ? -1 : bytes > 0x7fff ? 0x7fff : bytes);
    p.have_rssi = have_rssi ? 1 : 0;
    p.rssi_dbm = (float)rssi_dbm;
    remember(t, &p);
    if (!t->have_last_rx || mono_ms > t->last_rx_ms) {
        t->last_rx_ms = mono_ms;
        t->have_last_rx = 1;
    }
}

void rift_air_note_tx(struct rift_air *t, int64_t mono_ms, int bytes)
{
    struct rift_air_packet p;

    if (!t) {
        return;
    }
    memset(&p, 0, sizeof(p));
    p.mono_ms = mono_ms;
    p.tx = 1;
    p.bytes = (int16_t)(bytes < 0 ? -1 : bytes > 0x7fff ? 0x7fff : bytes);
    remember(t, &p);
}

void rift_air_read(const struct rift_air *t, int64_t now_ms, int slots_per_bar,
                   struct rift_air_view *out)
{
    int64_t now_slot;
    int64_t newest_bar;
    int64_t s;
    int64_t span;
    int j;
    int c;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (slots_per_bar < 1) {
        slots_per_bar = 1;
    }
    if (slots_per_bar > RIFT_AIR_SLOTS / RIFT_AIR_BARS) {
        slots_per_bar = RIFT_AIR_SLOTS / RIFT_AIR_BARS;
    }
    out->bar_ms = slots_per_bar * RIFT_AIR_SLOT_MS;
    now_slot = floor_div(now_ms, RIFT_AIR_SLOT_MS);
    if (t && t->started && now_slot < t->slot) {
        now_slot = t->slot; /* a frame stamped slightly ahead of this clock */
        now_ms = now_slot * RIFT_AIR_SLOT_MS;
    }
    /* Bars are cut on multiples of their own span, so a bar that has been
     * drawn keeps the frames it had: the graph scrolls, it does not reshuffle. */
    newest_bar = floor_div(now_slot, slots_per_bar);
    span = out->bar_ms;
    out->newest_bar = newest_bar;
    out->phase_permille = (int)(((now_ms - newest_bar * span) * 1000) / span);
    if (out->phase_permille < 0) {
        out->phase_permille = 0;
    } else if (out->phase_permille > 999) {
        out->phase_permille = 999;
    }
    if (!t || !t->started) {
        return;
    }
    out->started = 1;
    for (s = t->slot; s > t->slot - RIFT_AIR_SLOTS && s > now_slot - RIFT_AIR_SLOTS; s--) {
        int64_t back = newest_bar - floor_div(s, slots_per_bar);

        if (back < 0 || back >= RIFT_AIR_BARS) {
            continue;
        }
        j = RIFT_AIR_BARS - 1 - (int)back;
        for (c = 0; c < RIFT_AIR_TYPES; c++) {
            out->bar[j][c] = (uint16_t)(out->bar[j][c] + t->count[ring_of(s)][c]);
        }
    }
    for (j = 0; j < RIFT_AIR_BARS; j++) {
        unsigned sum = 0;

        for (c = 0; c < RIFT_AIR_TYPES; c++) {
            sum += out->bar[j][c];
            out->type_total[c] += out->bar[j][c];
        }
        out->total += sum;
        if (sum > out->peak) {
            out->peak = sum;
        }
    }
}

unsigned rift_air_heard_within(const struct rift_air *t, int64_t now_ms, int64_t window_ms)
{
    int64_t first = floor_div(now_ms - window_ms, RIFT_AIR_SLOT_MS) + 1;
    unsigned sum = 0;
    int64_t s;
    int c;

    if (!t || !t->started) {
        return 0;
    }
    for (s = t->slot; s >= first && s > t->slot - RIFT_AIR_SLOTS; s--) {
        for (c = 0; c < RIFT_AIR_TYPES; c++) {
            sum += t->count[ring_of(s)][c];
        }
    }
    return sum;
}

const struct rift_air_packet *rift_air_recent(const struct rift_air *t, int i)
{
    if (!t || i < 0 || i >= t->recent_count) {
        return NULL;
    }
    return &t->recent[(t->recent_head - i + 2 * RIFT_AIR_RECENT) % RIFT_AIR_RECENT];
}

int rift_air_mean_rssi(const struct rift_air *t, int64_t now_ms, int64_t window_ms, double *dbm)
{
    double sum = 0;
    int n = 0;
    int i;

    for (i = 0; t && i < t->recent_count; i++) {
        const struct rift_air_packet *p = rift_air_recent(t, i);

        if (!p->tx && p->have_rssi && now_ms - p->mono_ms <= window_ms) {
            sum += p->rssi_dbm;
            n++;
        }
    }
    if (n > 0 && dbm) {
        *dbm = sum / n;
    }
    return n;
}

void rift_model_note_air(struct rift_model *m, const struct rift_activity *a)
{
    if (!m || !a || !a->have_mono) {
        return;
    }
    if (a->kind == RIFT_ACT_RX) {
        rift_traffic_note(&m->traffic, a->mono_ms, rift_traffic_class_of(a->word));
        rift_air_note_rx(&m->air, a->mono_ms, rift_air_type_of(a->word),
                         a->have_bytes ? a->bytes : -1, a->have_rssi, a->rssi_dbm);
    } else if (strcmp(a->word, "ok") == 0) {
        rift_air_note_tx(&m->air, a->mono_ms, a->have_bytes ? a->bytes : -1);
    }
}
