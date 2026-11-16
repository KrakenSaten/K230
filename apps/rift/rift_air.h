/*
 * The air, as ACTIVITY's live graph reads it: what was heard, by MeshCore
 * payload type, in five-second slots for the last twenty minutes, and the
 * last few frames one by one.
 *
 * rift_traffic.h keeps the same feed by the minute in three classes, for the
 * caption's PEAK and the tests that pin it. This is the finer picture the
 * redesigned ACTIVITY draws (ui/rift_graph.c): every payload type on its
 * own, so each can carry the colour operators know it by, at a resolution
 * where a burst looks like a burst; and a short ring of single frames, with
 * the signal each was heard at, for the live scope above the bars.
 *
 * Like rift_traffic it holds only what mesh.activity said: frames MeshCore
 * parsed, counted. The bars count receptions only. A transmission this
 * device made (result "ok" and nothing else) is kept in the ring of single
 * frames so the scope can mark it, and is never added to a bar.
 *
 * No LVGL, no clock of its own: host-tested by tests/rift_model_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_AIR_H
#define RIFT_AIR_H

#include <stdint.h>

#define RIFT_AIR_SLOT_MS 5000LL
#define RIFT_AIR_SLOTS 240 /* twenty minutes */
/* How many bars a view has, whatever span it covers. */
#define RIFT_AIR_BARS 60
/* The single frames kept, and how far back the live scope shows them. */
#define RIFT_AIR_RECENT 64
#define RIFT_AIR_SCOPE_MS 60000LL

/* MeshCore's payload types as the service words them (docs/api/mesh.md),
 * in the order of the packet palette (pos_theme.h enum pos_packet_kind):
 * the two enums are index for index, and ui/rift_graph.c relies on it. */
enum rift_air_type {
    RIFT_AIR_ADVERT = 0, /* advert */
    RIFT_AIR_GRP_TXT,    /* group_text */
    RIFT_AIR_GRP_DATA,   /* group_data */
    RIFT_AIR_TXT_MSG,    /* text */
    RIFT_AIR_ACK,        /* ack */
    RIFT_AIR_REQ,        /* req */
    RIFT_AIR_RESPONSE,   /* response */
    RIFT_AIR_TRACE,      /* trace */
    RIFT_AIR_PATH,       /* path */
    RIFT_AIR_ANON_REQ,   /* anon_req */
    RIFT_AIR_MULTIPART,  /* multipart */
    RIFT_AIR_CONTROL,    /* control */
    RIFT_AIR_RAW_CUSTOM, /* raw_custom */
    RIFT_AIR_UNKNOWN,    /* type12..14, and any word this build does not know */
    RIFT_AIR_TYPES
};

struct rift_air_packet {
    int64_t mono_ms;
    float rssi_dbm;
    int16_t bytes;     /* -1 when not reported */
    uint8_t type;      /* enum rift_air_type; meaningless for a transmission */
    uint8_t tx;        /* this device sent it */
    uint8_t have_rssi;
};

struct rift_air {
    uint8_t count[RIFT_AIR_SLOTS][RIFT_AIR_TYPES]; /* a ring of slots; saturating */
    int64_t slot; /* the newest slot held, mono_ms / RIFT_AIR_SLOT_MS */
    int started;
    struct rift_air_packet recent[RIFT_AIR_RECENT]; /* a ring, newest at recent_head */
    int recent_head;
    int recent_count;
    /* When the newest frame was heard (not sent), by its own stamp: the
     * ring is in the order frames were reported, which a late report makes
     * not quite the order they were heard in. */
    int64_t last_rx_ms;
    int have_last_rx;
    /* Bumped by every note, so a screen knows when to look again. */
    unsigned serial;
};

/* The slots as a graph reads them: RIFT_AIR_BARS bars, oldest first, each
 * the sum of slots_per_bar slots, the last one still filling. */
struct rift_air_view {
    uint16_t bar[RIFT_AIR_BARS][RIFT_AIR_TYPES];
    uint32_t type_total[RIFT_AIR_TYPES]; /* over the bars shown */
    uint32_t total;
    unsigned peak;      /* the tallest bar, all types */
    int phase_permille; /* how far through the newest bar's time `now` is */
    int64_t bar_ms;     /* the time one bar covers */
    int64_t newest_bar; /* the newest bar's own number: it goes up by one a bar */
    int started;
};

void rift_air_init(struct rift_air *t);
/* Which type a mesh.activity rx payload_type word is. */
enum rift_air_type rift_air_type_of(const char *payload_type);
/* The word a legend prints for it: ADVERT, CHANNEL, DIRECT, ACK, ... */
const char *rift_air_type_word(enum rift_air_type type);

/* One frame heard at mono_ms. bytes < 0: not reported. */
void rift_air_note_rx(struct rift_air *t, int64_t mono_ms, enum rift_air_type type, int bytes,
                      int have_rssi, double rssi_dbm);
/* One frame this device put on the air (a tx activity whose result is ok). */
void rift_air_note_tx(struct rift_air *t, int64_t mono_ms, int bytes);

/* Read the slots as of now_ms. slots_per_bar is 1 (five minutes across the
 * bars) to RIFT_AIR_SLOTS / RIFT_AIR_BARS (the whole twenty). */
void rift_air_read(const struct rift_air *t, int64_t now_ms, int slots_per_bar,
                   struct rift_air_view *out);
/* Frames heard in the last window_ms, to the slot. */
unsigned rift_air_heard_within(const struct rift_air *t, int64_t now_ms, int64_t window_ms);
/* The i'th single frame back from the newest reported, or NULL past the
 * end. Nearly, not strictly, newest first: a reader looks at each one's own
 * stamp and does not stop at the first old one. */
const struct rift_air_packet *rift_air_recent(const struct rift_air *t, int i);
/* The mean RSSI of the frames heard within window_ms that carried one.
 * Returns how many there were; 0 leaves *dbm alone. */
int rift_air_mean_rssi(const struct rift_air *t, int64_t now_ms, int64_t window_ms, double *dbm);

#endif
