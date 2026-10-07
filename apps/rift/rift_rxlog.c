/*
 * RX LOG: the ring and the mesh.rx reader. See rift_rxlog.h; the words are
 * in rift_rxlog_fmt.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_rxlog.h"

#include "rift_format.h"
#include "rift_json.h"

#include <string.h>

/* 2024-01-01T00:00:00Z: a wall clock before it was never set. The board has
 * no clock that survives a power cut (docs/hardware/T-DISPLAY-K230.md), and
 * a time of day from 1970 is not one to print as if it were. */
#define WALL_SET_MS 1704067200000LL
/* Larger than any monotonic or wall time this device will see, and well
 * inside the 2^53 a JSON number holds exactly. */
#define TIME_MAX_MS 9.0e15

void rift_rxlog_init(struct rift_rxlog *log)
{
    if (!log) {
        return;
    }
    memset(log, 0, sizeof(*log));
    log->next_uid = 1;
    log->supported = -1;
}

void rift_rxlog_clear(struct rift_rxlog *log)
{
    if (!log) {
        return;
    }
    log->head = 0;
    log->count = 0;
    log->top_uid = 0;
    log->selected_uid = 0;
    log->revision++;
}

/* ---- the ring ------------------------------------------------------------ */

static int phys(const struct rift_rxlog *log, int i)
{
    return (log->head + i) % RIFT_RXLOG_MAX;
}

const struct rift_rx_entry *rift_rxlog_at(const struct rift_rxlog *log, int i)
{
    if (!log || i < 0 || i >= log->count) {
        return NULL;
    }
    return &log->e[phys(log, log->count - 1 - i)];
}

const struct rift_rx_entry *rift_rxlog_find(const struct rift_rxlog *log, uint32_t uid)
{
    int i;

    for (i = 0; log && uid && i < log->count; i++) {
        const struct rift_rx_entry *e = &log->e[phys(log, i)];

        if (e->uid == uid) {
            return e;
        }
    }
    return NULL;
}

/* Received earlier than b: by the reception's time, then by the service's
 * order for two in the same millisecond. */
static int earlier(const struct rift_rx_entry *a, const struct rift_rx_entry *b)
{
    if (a->mono_ms != b->mono_ms) {
        return a->mono_ms < b->mono_ms;
    }
    return a->kind == RIFT_RX_FRAME && b->kind == RIFT_RX_FRAME && a->seq < b->seq;
}

static struct rift_rx_entry *insert(struct rift_rxlog *log, struct rift_rx_entry *e)
{
    int p = log->count;
    int back = 0;
    int i;

    /* In the order the frames were received: a flood MeshCore held back for
     * its receive delay is reported late, and goes in where it belongs. */
    while (p > 0 && back < RIFT_RXLOG_REORDER_MAX && earlier(e, &log->e[phys(log, p - 1)])) {
        p--;
        back++;
    }
    if (log->count == RIFT_RXLOG_MAX) {
        /* Full: the oldest goes. If the new one belongs before everything
         * kept, it is the oldest, and it is the one that goes. */
        if (p == 0) {
            log->evicted++;
            return NULL;
        }
        log->head = (log->head + 1) % RIFT_RXLOG_MAX;
        log->count--;
        log->evicted++;
        p--;
    }
    for (i = log->count; i > p; i--) {
        log->e[phys(log, i)] = log->e[phys(log, i - 1)];
    }
    e->uid = log->next_uid++;
    if (log->next_uid == 0) {
        log->next_uid = 1;
    }
    log->e[phys(log, p)] = *e;
    log->count++;
    log->total++;
    log->revision++;
    return &log->e[phys(log, p)];
}

void rift_rxlog_mark(struct rift_rxlog *log, const char *note, int64_t now_mono, int64_t now_wall)
{
    struct rift_rx_entry e;

    if (!log) {
        return;
    }
    memset(&e, 0, sizeof(e));
    e.kind = RIFT_RX_MARK;
    e.mono_ms = now_mono;
    e.wall_ms = now_wall;
    if (now_wall >= WALL_SET_MS) {
        e.flags |= RIFT_RXF_HAVE_WALL;
    }
    rift_utf8_copy(e.text, sizeof(e.text), note ? note : "");
    insert(log, &e);
}

/* ---- the service ---------------------------------------------------------- */

cJSON *rift_rxlog_subscribe_params(const struct rift_rxlog *log)
{
    cJSON *params;

    if (!log || !(params = cJSON_CreateObject())) {
        return NULL;
    }
    cJSON_AddBoolToObject(params, "rx_log", 1);
    return params;
}

void rift_rxlog_service_lost(struct rift_rxlog *log, int64_t now_mono)
{
    if (!log || log->lost) {
        return;
    }
    rift_rxlog_mark(log, "MESHCORED NOT ANSWERING" RIFT_SEP "NOTHING HEARD IS LOGGED", now_mono,
                    rift_rxlog_wall_now());
    log->lost = 1;
}

void rift_rxlog_service_answered(struct rift_rxlog *log, const cJSON *result, int64_t now_mono)
{
    if (!log) {
        return;
    }
    log->supported = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "rx_log")) ? 1 : 0;
    log->revision++;
    if (log->lost) {
        rift_rxlog_mark(log, log->supported ? "MESHCORED ANSWERING AGAIN"
                                            : "MESHCORED ANSWERING" RIFT_SEP "IT KEEPS NO RECEIVE LOG",
                        now_mono, rift_rxlog_wall_now());
        log->lost = 0;
    }
}

/* ---- reading one event --------------------------------------------------- */

/* A number that may be absent, but is in range when present. 1 found,
 * 0 absent, -1 present and wrong. */
static int opt_num(const cJSON *o, const char *key, double lo, double hi, int whole, double *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    if (!v) {
        return 0;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi ||
        (whole && v->valuedouble != (double)(int64_t)v->valuedouble)) {
        return -1;
    }
    *out = v->valuedouble;
    return 1;
}

/* A string that may be absent: 1 found, 0 absent, -1 present and not one. */
static int opt_str(const cJSON *o, const char *key, const char **out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    if (!v) {
        return 0;
    }
    if (!cJSON_IsString(v) || !v->valuestring) {
        return -1;
    }
    *out = v->valuestring;
    return 1;
}

static int opt_bool(const cJSON *o, const char *key, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    if (!v) {
        return 0;
    }
    if (!cJSON_IsBool(v)) {
        return -1;
    }
    *out = cJSON_IsTrue(v) ? 1 : 0;
    return 1;
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Exactly hex for at most max bytes, an even number of digits. Returns the
 * byte count, or -1. */
static int unhex(const char *s, uint8_t *out, size_t max)
{
    size_t n = strlen(s);
    size_t i;

    if (n == 0 || n % 2 != 0 || n / 2 > max) {
        return -1;
    }
    for (i = 0; i < n / 2; i++) {
        int hi = nibble(s[2 * i]);
        int lo = nibble(s[2 * i + 1]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(n / 2);
}

/* An optional one-byte hash ("a7"): 1 found, 0 absent, -1 wrong. */
static int opt_hash_byte(const cJSON *o, const char *key, uint8_t *out)
{
    const char *s = NULL;
    int r = opt_str(o, key, &s);

    if (r <= 0) {
        return r;
    }
    return unhex(s, out, 1) == 1 && strlen(s) == 2 ? 1 : -1;
}

static int read_route(const char *s, uint8_t *out)
{
    static const char *const words[4] = { "transport_flood", "flood", "direct",
                                          "transport_direct" };
    int i;

    for (i = 0; i < 4; i++) {
        if (strcmp(s, words[i]) == 0) {
            *out = (uint8_t)i;
            return 0;
        }
    }
    return -1;
}

/* The header, the path and the hash: everything the service read off the
 * packet itself. 0, or -1 for anything the API does not allow. */
static int read_packet(const cJSON *data, struct rift_rx_entry *e)
{
    const char *s = NULL;
    double d;
    int r;

    r = opt_num(data, "type_code", 0, 15, 1, &d);
    if (r < 0) {
        return -1;
    }
    if (r > 0) {
        e->type_code = (uint8_t)d;
        if (opt_str(data, "route", &s) != 1 || read_route(s, &e->route) != 0) {
            return -1;
        }
        e->flags |= RIFT_RXF_HAVE_HEADER;
    }
    r = opt_str(data, "path_kind", &s);
    if (r < 0) {
        return -1;
    }
    if (r > 0) {
        double size;
        double hops;
        int is_snr = strcmp(s, "snr") == 0;
        int got = 0;

        if (!is_snr && strcmp(s, "hops") != 0) {
            return -1;
        }
        if (opt_num(data, "path_hash_size", 1, 3, 1, &size) != 1 ||
            opt_num(data, "path_hops", 0, 63, 1, &hops) != 1) {
            return -1;
        }
        r = opt_str(data, "path_hex", &s);
        if (r < 0) {
            return -1;
        }
        if (r > 0 && (got = unhex(s, e->path, sizeof(e->path))) < 0) {
            return -1;
        }
        /* The hop count and the bytes must agree, or the path cannot be
         * split into hops honestly. */
        if (got != (int)(hops * size) || (is_snr && size != 1)) {
            return -1;
        }
        e->path_hash_size = (uint8_t)size;
        e->path_hops = (uint8_t)hops;
        e->path_bytes = (uint8_t)got;
        e->flags |= RIFT_RXF_PARSED | (is_snr ? RIFT_RXF_PATH_SNR : 0);
    }
    r = opt_str(data, "hash", &s);
    if (r < 0 || (r > 0 && (strlen(s) != 2 * RIFT_RXLOG_HASH_LEN ||
                            unhex(s, e->hash, RIFT_RXLOG_HASH_LEN) != RIFT_RXLOG_HASH_LEN))) {
        return -1;
    }
    if (r > 0) {
        e->flags |= RIFT_RXF_HAVE_HASH;
    }
    return 0;
}

/* The addressing and what MeshCore made of it. */
static int read_verdict(const cJSON *data, struct rift_rx_entry *e, const char *verdict)
{
    const char *s = NULL;
    double d;
    int b;
    int r;

    r = opt_num(data, "dup", 1, 1.0e9, 1, &d);
    if (r < 0) {
        return -1;
    }
    e->dup = r > 0 ? (uint32_t)d : 0;
    if ((r = opt_bool(data, "relayed", &b)) < 0) {
        return -1;
    }
    e->flags |= (r > 0 && b) ? RIFT_RXF_RELAYED : 0;
    if ((r = opt_bool(data, "own", &b)) < 0) {
        return -1;
    }
    e->flags |= (r > 0 && b) ? RIFT_RXF_OWN : 0;

    if ((r = opt_hash_byte(data, "channel_hash", &e->channel_hash)) < 0) {
        return -1;
    }
    if (r > 0) {
        e->flags |= RIFT_RXF_HAVE_CHANNEL;
        if (opt_bool(data, "channel_known", &b) < 0) {
            return -1;
        }
        e->flags |= bool_of(data, "channel_known", 0) ? RIFT_RXF_CHANNEL_HELD : 0;
    }
    if ((r = opt_hash_byte(data, "dest_hash", &e->dest_hash)) < 0) {
        return -1;
    }
    if (r > 0) {
        e->flags |= RIFT_RXF_HAVE_DEST;
        if (opt_bool(data, "for_us", &b) < 0) {
            return -1;
        }
        e->flags |= bool_of(data, "for_us", 0) ? RIFT_RXF_FOR_US : 0;
    }
    if ((r = opt_hash_byte(data, "src_hash", &e->src_hash)) < 0) {
        return -1;
    }
    e->flags |= r > 0 ? RIFT_RXF_HAVE_SRC : 0;
    if ((r = opt_num(data, "control_flags", 0, 255, 1, &d)) < 0) {
        return -1;
    }
    if (r > 0) {
        e->control_flags = (uint8_t)d;
        e->flags |= RIFT_RXF_HAVE_CONTROL;
    }

    if (strcmp(verdict, "rejected") == 0) {
        r = opt_str(data, "reject", &s);
        if (r != 1) {
            return -1;
        }
        if (strcmp(s, "queue_full") == 0) {
            e->reject = RIFT_RXR_QUEUE_FULL;
        } else if (strcmp(s, "unparsed") == 0) {
            e->reject = RIFT_RXR_UNPARSED;
        } else if (strcmp(s, "no_buffer") == 0) {
            e->reject = RIFT_RXR_NO_BUFFER;
        } else {
            return -1;
        }
        e->state = RIFT_RXS_REJECTED;
    } else if (strcmp(verdict, "unresolved") == 0) {
        e->state = RIFT_RXS_UNRESOLVED;
    } else if (strcmp(verdict, "new") == 0 || strcmp(verdict, "duplicate") == 0) {
        if (verdict[0] == 'd') {
            e->flags |= RIFT_RXF_MC_DUPLICATE;
        }
        e->state = (e->flags & RIFT_RXF_OWN)                                 ? RIFT_RXS_ECHO
                   : (e->dup > 1 || (e->flags & RIFT_RXF_MC_DUPLICATE)) ? RIFT_RXS_DUP
                                                                          : RIFT_RXS_RX;
    } else {
        return -1;
    }
    return 0;
}

/* What the service decoded: only the fields its kind carries. */
static int read_decoded(const cJSON *data, struct rift_rx_entry *e)
{
    const char *kind = NULL;
    const char *sender = NULL;
    const char *where = NULL;
    const char *text = NULL;
    const char *key = NULL;
    int r = opt_str(data, "decoded", &kind);

    if (r < 0) {
        return -1;
    }
    if (opt_str(data, "sender", &sender) < 0 || opt_str(data, "text", &text) < 0 ||
        opt_str(data, "recipient", &where) < 0 || opt_str(data, "sender_public_key", &key) < 0) {
        return -1;
    }
    if (r == 0) {
        return 0;
    }
    if (key) {
        if (!hex_only(key, 64)) {
            return -1;
        }
        e->sender_ident = rift_ident_hash(key);
        e->flags |= RIFT_RXF_HAVE_KEY;
    }
    if (strcmp(kind, "direct") == 0) {
        if (!sender || !text) {
            return -1;
        }
        e->decode = RIFT_RXD_DIRECT;
    } else if (strcmp(kind, "channel") == 0) {
        where = NULL;
        if (!text || opt_str(data, "channel_name", &where) < 0) {
            return -1;
        }
        e->decode = RIFT_RXD_CHANNEL;
    } else if (strcmp(kind, "advert") == 0) {
        if (!sender) {
            return -1;
        }
        e->decode = RIFT_RXD_ADVERT;
        where = NULL;
        text = NULL;
    } else {
        return -1;
    }
    rift_utf8_copy(e->sender, sizeof(e->sender), sender ? sender : "");
    rift_utf8_copy(e->where, sizeof(e->where), where ? where : "");
    rift_utf8_copy(e->text, sizeof(e->text), text ? text : "");
    return 0;
}

static int same_packet(const struct rift_rx_entry *a, const struct rift_rx_entry *b)
{
    return a->kind == RIFT_RX_FRAME && b->kind == RIFT_RX_FRAME && (a->flags & RIFT_RXF_HAVE_HASH) &&
           (b->flags & RIFT_RXF_HAVE_HASH) && memcmp(a->hash, b->hash, RIFT_RXLOG_HASH_LEN) == 0;
}

static void borrow(struct rift_rx_entry *to, const struct rift_rx_entry *from)
{
    to->decode = from->decode;
    memcpy(to->sender, from->sender, sizeof(to->sender));
    memcpy(to->where, from->where, sizeof(to->where));
    memcpy(to->text, from->text, sizeof(to->text));
    to->sender_ident = from->sender_ident;
    to->flags |= RIFT_RXF_TEXT_BORROWED | (from->flags & RIFT_RXF_HAVE_KEY);
}

/* A repeat MeshCore did not read again shows what the copy it did read said:
 * the same packet hash is the same bytes, so this is not a guess. Whichever
 * of the two arrived first, both end up showing it. */
static void share_text(struct rift_rxlog *log, struct rift_rx_entry *added)
{
    int i;
    int looked = 0;

    for (i = log->count - 1; i >= 0 && looked < RIFT_RXLOG_TEXT_LOOKBACK; i--, looked++) {
        struct rift_rx_entry *other = &log->e[phys(log, i)];

        if (other == added || !same_packet(other, added)) {
            continue;
        }
        if (added->decode == RIFT_RXD_NONE && other->decode != RIFT_RXD_NONE) {
            borrow(added, other);
            return;
        }
        if (added->decode != RIFT_RXD_NONE && other->decode == RIFT_RXD_NONE) {
            borrow(other, added);
        }
    }
}

int rift_rxlog_apply(struct rift_rxlog *log, const cJSON *data, int64_t now_mono, int64_t now_wall)
{
    struct rift_rx_entry e;
    struct rift_rx_entry *added;
    const char *verdict = NULL;
    double d;
    int r;

    if (!log) {
        return -1;
    }
    memset(&e, 0, sizeof(e));
    if (!cJSON_IsObject(data)) {
        goto bad;
    }
    /* One version, and only that one: a v2 may mean a field differently, and
     * guessing at it would be printing what nobody said. */
    if (opt_num(data, "v", 1, 1, 1, &d) != 1 || opt_num(data, "seq", 0, TIME_MAX_MS, 1, &d) != 1) {
        goto bad;
    }
    e.seq = (uint64_t)d;
    if (opt_num(data, "mono_ms", 0, TIME_MAX_MS, 1, &d) != 1) {
        goto bad;
    }
    e.mono_ms = (int64_t)d;
    if (opt_num(data, "bytes", 0, 255, 1, &d) != 1) {
        goto bad;
    }
    e.bytes = (uint16_t)d;
    if ((r = opt_num(data, "rssi_dbm", -200, 50, 0, &d)) < 0) {
        goto bad;
    }
    if (r > 0) {
        e.rssi_dbm = (float)d;
        e.flags |= RIFT_RXF_HAVE_RSSI;
    }
    if ((r = opt_num(data, "snr_db", -64, 64, 0, &d)) < 0) {
        goto bad;
    }
    if (r > 0) {
        e.snr_db = (float)d;
        e.flags |= RIFT_RXF_HAVE_SNR;
    }
    if (opt_str(data, "verdict", &verdict) != 1 || read_packet(data, &e) != 0 ||
        read_verdict(data, &e, verdict) != 0 || read_decoded(data, &e) != 0) {
        goto bad;
    }
    e.kind = RIFT_RX_FRAME;
    e.wall_ms = now_wall - (now_mono - e.mono_ms);
    if (now_wall >= WALL_SET_MS) {
        e.flags |= RIFT_RXF_HAVE_WALL;
    }
    added = insert(log, &e);
    if (added) {
        share_text(log, added);
    }
    return 0;

bad:
    log->malformed++;
    log->revision++;
    return -1;
}

/* ---- what a screen reads ------------------------------------------------- */

int rift_rxlog_matches(const struct rift_rx_entry *e, int filter)
{
    int t;

    if (!e) {
        return 0;
    }
    if (e->kind == RIFT_RX_MARK || filter == RIFT_RXF_ALL) {
        return 1;
    }
    if (filter == RIFT_RXF_DUP) {
        return e->state == RIFT_RXS_DUP || e->state == RIFT_RXS_ECHO;
    }
    t = (e->flags & RIFT_RXF_HAVE_HEADER) ? e->type_code : -1;
    switch (filter) {
    case RIFT_RXF_MSG:
        return t == 2 || t == 5 || t == 6;
    case RIFT_RXF_ADV:
        return t == 4;
    case RIFT_RXF_CTRL:
        return t != 2 && t != 4 && t != 5 && t != 6;
    default:
        return 0;
    }
}

int rift_rxlog_count(const struct rift_rxlog *log, int filter)
{
    int n = 0;
    int i;

    for (i = 0; log && i < log->count; i++) {
        n += rift_rxlog_matches(&log->e[phys(log, i)], filter);
    }
    return n;
}

int rift_rxlog_window(const struct rift_rxlog *log, int filter, int skip,
                      const struct rift_rx_entry **out, int max)
{
    int n = 0;
    int i;

    for (i = 0; log && i < log->count && n < max; i++) {
        const struct rift_rx_entry *e = rift_rxlog_at(log, i);

        if (!rift_rxlog_matches(e, filter)) {
            continue;
        }
        if (skip > 0) {
            skip--;
            continue;
        }
        out[n++] = e;
    }
    return n;
}

int rift_rxlog_position(const struct rift_rxlog *log, int filter, uint32_t uid)
{
    int pos = 0;
    int i;

    for (i = 0; log && uid && i < log->count; i++) {
        const struct rift_rx_entry *e = rift_rxlog_at(log, i);

        if (!rift_rxlog_matches(e, filter)) {
            continue;
        }
        if (e->uid == uid) {
            return pos;
        }
        pos++;
    }
    return -1;
}
