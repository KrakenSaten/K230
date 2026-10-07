/*
 * RX LOG: every reception meshcored reports, as a bounded ring.
 *
 * Not the ACTIVITY feed. That one is a summary RIFT shows as status; this is
 * the packet inspector behind it (docs/apps/RIFT.md, "RX LOG"): one entry per
 * frame the radio took off the air, duplicates included, from meshcored's
 * mesh.rx (docs/api/mesh.md, "The receive log"), which reads each frame
 * before MeshCore's deduplication has had a chance to fold it away.
 *
 * What an entry holds is what the event said and nothing inferred: a field
 * the service did not report stays unknown here, and the screen prints a
 * placeholder for it rather than a plausible value. Text is held only where
 * the service decoded it for its own use - a message to this node, a message
 * on a channel this node holds, a node's advertised name - and a later copy
 * of the same packet, which MeshCore does not read again, borrows the text
 * of the earlier one by its packet hash.
 *
 * Bounded: RIFT_RXLOG_MAX entries in one array allocated with the session,
 * the oldest dropped first, nothing allocated per event. The ring is the
 * session's (rift_app.h), so it keeps filling while the screen is closed and
 * while RIFT runs in the background; CLEAR empties it and nothing else.
 *
 * No LVGL: host-tested by tests/rift_rxlog_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_RXLOG_H
#define RIFT_RXLOG_H

#include <cjson/cJSON.h>

#include <stddef.h>
#include <stdint.h>

/* 1000 entries of about 360 bytes: some 360 KB, once, with the session. */
#define RIFT_RXLOG_MAX 1000
#define RIFT_RXLOG_PATH_MAX 64        /* MeshCore's MAX_PATH_SIZE, bytes */
#define RIFT_RXLOG_HASH_LEN 8         /* MeshCore's MAX_HASH_SIZE, bytes */
#define RIFT_RXLOG_NAME_MAX 33        /* a node or channel name, 32 bytes */
#define RIFT_RXLOG_TEXT_MAX 161       /* a MeshCore text, 160 bytes */
/* How far back a repeat looks for the text of the copy MeshCore decoded. */
#define RIFT_RXLOG_TEXT_LOOKBACK 256
/* How far back an entry that arrived late is moved to keep the ring in the
 * order the frames were received. A flood MeshCore held back for its receive
 * delay is reported up to that delay late (seconds); further than this is
 * not a delay but a different clock, and is appended as it came. */
#define RIFT_RXLOG_REORDER_MAX 64

enum rift_rx_kind {
    RIFT_RX_FRAME = 0, /* a reception */
    RIFT_RX_MARK,      /* this app's own note: the service went away, came back */
};

/* What the reception was, in the log's words (rift_rxlog_state_word). */
enum rift_rx_state {
    RIFT_RXS_RX = 0,      /* the first reception of this packet the log knows of */
    RIFT_RXS_DUP,         /* a repeat: dup > 1, or MeshCore's seen-table matched it */
    RIFT_RXS_ECHO,        /* this node's own packet, repeated back to it */
    RIFT_RXS_REJECTED,    /* it never reached MeshCore (rift_rx_reject) */
    RIFT_RXS_UNRESOLVED,  /* the service lost track of it before MeshCore was done */
};

enum rift_rx_reject {
    RIFT_RXR_NONE = 0,
    RIFT_RXR_QUEUE_FULL,
    RIFT_RXR_UNPARSED,
    RIFT_RXR_NO_BUFFER,
};

/* What meshcored could read of it. */
enum rift_rx_decode {
    RIFT_RXD_NONE = 0,
    RIFT_RXD_DIRECT,   /* a direct message to this node: sender, recipient, text */
    RIFT_RXD_CHANNEL,  /* a message on a held channel: channel, claimed sender, text */
    RIFT_RXD_ADVERT,   /* an advert from a node the table holds: its name */
};

/* MeshCore's ROUTE_TYPE_*. */
enum rift_rx_route {
    RIFT_RXRT_TRANSPORT_FLOOD = 0,
    RIFT_RXRT_FLOOD,
    RIFT_RXRT_DIRECT,
    RIFT_RXRT_TRANSPORT_DIRECT,
};

/* The filters (DS §54). */
enum rift_rx_filter {
    RIFT_RXF_ALL = 0,
    RIFT_RXF_DUP,   /* repeats and echoes */
    RIFT_RXF_MSG,   /* text, group text, group data */
    RIFT_RXF_ADV,   /* adverts */
    RIFT_RXF_CTRL,  /* everything the mesh does to run itself, and what was rejected */
    RIFT_RXF_COUNT,
};

/* Bits of rift_rx_entry.flags: which of the fields below were reported. */
#define RIFT_RXF_HAVE_RSSI     0x0001u
#define RIFT_RXF_HAVE_SNR      0x0002u
#define RIFT_RXF_HAVE_HEADER   0x0004u  /* type_code and route are known */
#define RIFT_RXF_PARSED        0x0008u  /* path and hash are known */
#define RIFT_RXF_HAVE_HASH     0x0010u
#define RIFT_RXF_HAVE_CHANNEL  0x0020u
#define RIFT_RXF_CHANNEL_HELD  0x0040u
#define RIFT_RXF_HAVE_DEST     0x0080u
#define RIFT_RXF_FOR_US        0x0100u
#define RIFT_RXF_HAVE_SRC      0x0200u
#define RIFT_RXF_RELAYED       0x0400u
#define RIFT_RXF_OWN           0x0800u
#define RIFT_RXF_PATH_SNR      0x1000u  /* a TRACE: the path bytes are per-hop SNRs */
#define RIFT_RXF_HAVE_CONTROL  0x2000u
#define RIFT_RXF_TEXT_BORROWED 0x4000u  /* the decoded text is an earlier copy's */
#define RIFT_RXF_HAVE_WALL     0x8000u  /* wall_ms is a time of day (the clock was set) */
#define RIFT_RXF_MC_DUPLICATE  0x10000u /* MeshCore's own seen-table matched it */
#define RIFT_RXF_HAVE_KEY      0x20000u /* sender_ident is the sender's key's */

struct rift_rx_entry {
    uint32_t uid;            /* this app's, 1 upwards, never reused in a session */
    uint32_t flags;
    uint64_t seq;            /* the service's, per run */
    int64_t mono_ms;         /* the reception, on the shared monotonic clock */
    int64_t wall_ms;         /* the same instant on this device's wall clock */
    uint32_t dup;            /* receptions of this hash so far, this one included; 0 unknown */
    uint32_t sender_ident;   /* rift_ident_hash of the sender's key, with HAVE_KEY */
    float rssi_dbm;
    float snr_db;
    uint16_t bytes;
    uint8_t kind;            /* enum rift_rx_kind */
    uint8_t state;           /* enum rift_rx_state */
    uint8_t reject;          /* enum rift_rx_reject */
    uint8_t decode;          /* enum rift_rx_decode */
    uint8_t type_code;       /* MeshCore's PAYLOAD_TYPE_*, with HAVE_HEADER */
    uint8_t route;           /* enum rift_rx_route, with HAVE_HEADER */
    uint8_t path_hash_size;  /* 1..3 */
    uint8_t path_hops;
    uint8_t path_bytes;
    uint8_t channel_hash;
    uint8_t dest_hash;
    uint8_t src_hash;
    uint8_t control_flags;
    uint8_t hash[RIFT_RXLOG_HASH_LEN];
    uint8_t path[RIFT_RXLOG_PATH_MAX];
    char sender[RIFT_RXLOG_NAME_MAX];
    /* DIRECT: the recipient; CHANNEL: the channel's name. */
    char where[RIFT_RXLOG_NAME_MAX];
    /* The decoded body; for a MARK, the note. */
    char text[RIFT_RXLOG_TEXT_MAX];
};

struct rift_rxlog {
    struct rift_rx_entry e[RIFT_RXLOG_MAX];
    int head;                /* where the oldest entry is */
    int count;
    uint32_t next_uid;
    unsigned total;          /* entries ever added, marks included */
    unsigned evicted;        /* dropped to make room */
    unsigned malformed;      /* mesh.rx events refused */
    /* Whether the service said it reports receptions (the mesh.subscribe
     * reply): -1 not asked yet or no answer, 0 an older meshcored, 1 yes. */
    int supported;
    /* Bumped by every change, so a screen repaints when something happened. */
    unsigned revision;
    /* The log was told the service went away and not yet that it is back. */
    int lost;

    /* The reader's view of it, kept with the session so a screen that is
     * closed and opened again shows what it showed (rift_rxlog_view.c):
     * the filter; PAUSE, which freezes what is shown while capture goes on;
     * the entry held at the top while paused or scrolled (0: the newest);
     * the entry selected for the detail. */
    int filter;
    int paused;
    uint32_t top_uid;
    uint32_t selected_uid;
};

void rift_rxlog_init(struct rift_rxlog *log);
/* CLEAR: the entries go, and nothing else. The counters that say what was
 * refused stay, and so does what the service said it supports. */
void rift_rxlog_clear(struct rift_rxlog *log);

/* One mesh.rx event. Returns 0 when it was added, -1 when it was refused as
 * malformed (counted in malformed). Every length is bounded and every field
 * is checked: an event carrying something the API does not allow is
 * refused whole rather than half-believed. now_mono is the shared monotonic
 * clock, now_wall this device's wall clock, both in ms; they place the
 * reception on the wall clock. */
int rift_rxlog_apply(struct rift_rxlog *log, const cJSON *data, int64_t now_mono,
                     int64_t now_wall);
/* A note of this app's own in the log, at now_mono: the service went away,
 * or answered again. */
void rift_rxlog_mark(struct rift_rxlog *log, const char *note, int64_t now_mono, int64_t now_wall);

/* The meshcored client's three points (rift_ipc.c); each takes a NULL log
 * and does nothing with it.
 *
 * The receive log is asked for by name: a meshcored that knows it then
 * reports every reception as mesh.rx, one that does not ignores the
 * parameter and answers without rx_log (docs/api/mesh.md). The params for
 * mesh.subscribe, or NULL for a plain subscribe. */
cJSON *rift_rxlog_subscribe_params(const struct rift_rxlog *log);
/* The subscribed connection went: noted in the log once, so a gap in it
 * reads as one. */
void rift_rxlog_service_lost(struct rift_rxlog *log, int64_t now_mono);
/* mesh.subscribe was answered: whether this meshcored keeps a receive log
 * (absent is an older one), and the log noted that it is back. */
void rift_rxlog_service_answered(struct rift_rxlog *log, const cJSON *result, int64_t now_mono);

/* The wall clock as the log reads it, in ms. In rift_clock.c with the
 * monotonic one, so a test can put a clock of its own in its place. */
int64_t rift_rxlog_wall_now(void);

/* The entries as a screen reads them, newest first: the i'th is i back from
 * the newest. NULL past the end. */
const struct rift_rx_entry *rift_rxlog_at(const struct rift_rxlog *log, int i);
const struct rift_rx_entry *rift_rxlog_find(const struct rift_rxlog *log, uint32_t uid);
int rift_rxlog_matches(const struct rift_rx_entry *e, int filter);
/* How many entries the filter shows, and up to max of them newest first,
 * starting `skip` matches down. Returns how many were written. */
int rift_rxlog_count(const struct rift_rxlog *log, int filter);
int rift_rxlog_window(const struct rift_rxlog *log, int filter, int skip,
                      const struct rift_rx_entry **out, int max);
/* Where uid is among the filter's entries, newest first; -1 when it is not
 * one of them (gone from the ring, or filtered out). */
int rift_rxlog_position(const struct rift_rxlog *log, int filter, uint32_t uid);

/* ---- words (DS §54) -------------------------------------------------- *
 *
 * The type label of MeshCore's PAYLOAD_TYPE_*, mapped one to one:
 *
 *   0 REQ    request              6 DATA   group datagram    11 CTRL  control
 *   1 RESP   response             7 ANON   anonymous request    (DISC when its
 *   2 MSG    direct text          8 PATH   returned path         type is node
 *   3 ACK    acknowledgement      9 TRACE  trace                 discovery)
 *   4 ADV    advert              10 MULTI  multipart         15 RAW   raw custom
 *   5 MSG    group text                                   12-14 UNK  reserved
 *
 * and UNK for a frame whose header was never read. */
const char *rift_rxlog_type_word(const struct rift_rx_entry *e);
/* "RX", "DUP #2", "DUP", "ECHO", "ECHO #3", "REJ", "LOST"; "--" for a mark. */
void rift_rxlog_state_word(const struct rift_rx_entry *e, char *out, size_t out_len);
const char *rift_rxlog_filter_word(int filter);

#define RIFT_RXLOG_LINE_MAX 400
/* The first line's fields, each on its own so each can carry its colour:
 * "12:43:08.412", "CH:A7" / "CH:--", "H:3C9A" / "H:--", "42B",
 * "RSSI:-71" / "RSSI:--", "SNR:9" / "SNR:7.5" / "SNR:--". */
void rift_rxlog_fmt_time(const struct rift_rx_entry *e, char *out, size_t out_len);
void rift_rxlog_fmt_channel(const struct rift_rx_entry *e, char *out, size_t out_len);
void rift_rxlog_fmt_hash(const struct rift_rx_entry *e, char *out, size_t out_len);
void rift_rxlog_fmt_size(const struct rift_rx_entry *e, char *out, size_t out_len);
void rift_rxlog_fmt_rssi(const struct rift_rx_entry *e, char *out, size_t out_len);
void rift_rxlog_fmt_snr(const struct rift_rx_entry *e, char *out, size_t out_len);
/* The whole path, never shortened: "PATH  6E > 67 > 74 > 73" for a flood
 * (the relays it came through, first first), "ROUTE 6E > 67" for a direct
 * packet (the hops still to go), "PATH  0 HOP" / "ROUTE 0 HOP" when there are
 * none, "SNRS  +9.0 > +3.5" for a TRACE, "PATH  --" when nothing was parsed. */
void rift_rxlog_fmt_path(const struct rift_rx_entry *e, char *out, size_t out_len);
/* The third line's two halves: who (with the claim mark "?" on a channel,
 * because nothing signs a group frame) and what - the text in quotes, or a
 * bracketed word for what could not be read. Both "" when there is nothing
 * to say (an ACK, a TRACE). */
void rift_rxlog_fmt_who(const struct rift_rx_entry *e, char *out, size_t out_len);
void rift_rxlog_fmt_what(const struct rift_rx_entry *e, char *out, size_t out_len);
/* Every field, one per line, for the detail. */
void rift_rxlog_fmt_detail(const struct rift_rx_entry *e, char *out, size_t out_len);

/* The signal grades the colours follow (DS §54): 2 good, 1 fair, 0 weak,
 * -1 not measured. RSSI good from -85 dBm, fair from -105; SNR good from
 * +5 dB, fair from -5. */
int rift_rxlog_rssi_grade(const struct rift_rx_entry *e);
int rift_rxlog_snr_grade(const struct rift_rx_entry *e);

#endif
