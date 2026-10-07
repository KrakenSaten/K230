/*
 * Wave's history. See wave_history.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_history.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wave_history_init(struct wave_history *h)
{
    memset(h, 0, sizeof(*h));
    h->next_seq = 1;
    h->fold_heard_ms = -1;
}

int wave_history_count(const struct wave_history *h)
{
    return h ? h->count : 0;
}

const struct wave_history_entry *wave_history_at(const struct wave_history *h, int i)
{
    if (!h || i < 0 || i >= h->count) {
        return NULL;
    }
    return &h->ring[(h->head + h->count - 1 - i) % WAVE_HISTORY_MAX];
}

static struct wave_history_entry *newest(struct wave_history *h)
{
    return h->count ? &h->ring[(h->head + h->count - 1) % WAVE_HISTORY_MAX] : NULL;
}

/* A fresh entry at the new end of the ring, the oldest dropped when full. */
static struct wave_history_entry *append(struct wave_history *h, enum wave_dir dir,
                                         const char *preset, const char *data, size_t len,
                                         int64_t when)
{
    struct wave_history_entry *e;

    if (h->count == WAVE_HISTORY_MAX) {
        h->head = (h->head + 1) % WAVE_HISTORY_MAX;
        h->count--;
    }
    e = &h->ring[(h->head + h->count) % WAVE_HISTORY_MAX];
    h->count++;
    memset(e, 0, sizeof(*e));
    e->seq = h->next_seq++;
    if (h->next_seq == 0) {
        h->next_seq = 1;
    }
    e->when = when > 0 ? when : 0;
    e->dir = dir;
    e->count = 1;
    snprintf(e->preset, sizeof(e->preset), "%s", preset && *preset ? preset : "-");
    if (len > WAVE_HISTORY_DATA_MAX) {
        len = WAVE_HISTORY_DATA_MAX;
        e->truncated = 1;
    }
    if (data && len) {
        memcpy(e->data, data, len);
        e->len = len;
    }
    e->data[e->len] = '\0';
    h->fold_heard_ms = -1;
    h->changes++;
    return e;
}

const struct wave_history_entry *wave_history_add_tx(struct wave_history *h, const char *preset,
                                                     const char *data, size_t len,
                                                     enum wave_result result, int copies,
                                                     int64_t when)
{
    struct wave_history_entry *e = append(h, WAVE_DIR_TX, preset, data, len, when);

    e->result = result;
    e->count = copies < 0 ? 0 : copies;
    return e;
}

int wave_history_add_rx(struct wave_history *h, const char *preset, const char *data, size_t len,
                        int captured, int64_t when, int64_t now_ms, int window_ms)
{
    struct wave_history_entry *e = newest(h);
    size_t kept = len > WAVE_HISTORY_DATA_MAX ? WAVE_HISTORY_DATA_MAX : len;

    if (e && h->fold_heard_ms >= 0 && e->dir == WAVE_DIR_RX && e->result == WAVE_RESULT_OK &&
        e->len == kept && e->truncated == (len > WAVE_HISTORY_DATA_MAX) &&
        memcmp(e->data, data, kept) == 0 && now_ms - h->fold_heard_ms <= window_ms &&
        now_ms >= h->fold_heard_ms) {
        e->count++;
        h->fold_heard_ms = now_ms;
        h->changes++;
        return 1;
    }
    e = append(h, WAVE_DIR_RX, preset, data, len, when);
    e->result = WAVE_RESULT_OK;
    e->captured = captured ? 1 : 0;
    h->fold_heard_ms = now_ms;
    return 0;
}

void wave_history_add_undecoded(struct wave_history *h, const char *preset, int64_t when)
{
    struct wave_history_entry *e = append(h, WAVE_DIR_RX, preset, NULL, 0, when);

    e->result = WAVE_RESULT_UNDECODED;
    e->captured = 1;
}

void wave_history_clear(struct wave_history *h)
{
    uint32_t seq = h->next_seq;
    unsigned changes = h->changes;

    wave_history_init(h);
    /* Sequence numbers keep counting, so an entry from before the clear is
     * never mistaken for one after it. */
    h->next_seq = seq;
    h->changes = changes + 1;
}

/* ---- the text form ------------------------------------------------------ */

static const char *const result_words[] = { "ok", "stopped", "failed", "undecoded" };

long wave_history_format(const struct wave_history *h, char *out, size_t n)
{
    size_t off = 0;
    int i;
    int w;

    if (!out || n == 0) {
        return -1;
    }
    w = snprintf(out, n, "%s\n", WAVE_HISTORY_MAGIC);
    if (w < 0 || (size_t)w >= n) {
        return -1;
    }
    off = (size_t)w;
    /* Oldest first, so a reader appending in file order rebuilds the ring. */
    for (i = h->count - 1; i >= 0; i--) {
        const struct wave_history_entry *e = wave_history_at(h, i);
        char flags[4];
        size_t f = 0;
        size_t k;

        if (e->captured) {
            flags[f++] = 'c';
        }
        if (e->truncated) {
            flags[f++] = 't';
        }
        if (f == 0) {
            flags[f++] = '-';
        }
        flags[f] = '\0';
        w = snprintf(out + off, n - off, "%" PRIu32 " %" PRId64 " %c %s %d %s %s ", e->seq, e->when,
                     e->dir == WAVE_DIR_TX ? 'T' : 'R', result_words[e->result], e->count, flags,
                     e->preset);
        if (w < 0 || (size_t)w >= n - off) {
            return -1;
        }
        off += (size_t)w;
        if (e->len == 0) {
            if (off + 2 >= n) {
                return -1;
            }
            out[off++] = '-';
        }
        for (k = 0; k < e->len; k++) {
            if (off + 3 >= n) {
                return -1;
            }
            snprintf(out + off, 3, "%02x", (unsigned char)e->data[k]);
            off += 2;
        }
        out[off++] = '\n';
        out[off] = '\0';
    }
    return (long)off;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static int word_ok(const char *w, size_t max)
{
    size_t n = 0;

    for (; *w; w++, n++) {
        if (!((*w >= 'a' && *w <= 'z') || (*w >= '0' && *w <= '9') || *w == '_' || *w == '-')) {
            return 0;
        }
    }
    return n > 0 && n < max;
}

/* One entry line (no newline). 0 with *e filled, -1 when it is not one. */
static int parse_entry(char *line, struct wave_history_entry *e)
{
    char *field[8];
    char *save = NULL;
    char *end;
    int nf = 0;
    char *t;
    size_t i;
    unsigned long long seq;
    long long when;
    long count;

    for (t = strtok_r(line, " ", &save); t && nf < 8; t = strtok_r(NULL, " ", &save)) {
        field[nf++] = t;
    }
    /* t is the token after the eighth: there must be none. */
    if (nf != 8 || t != NULL) {
        return -1;
    }
    memset(e, 0, sizeof(*e));
    seq = strtoull(field[0], &end, 10);
    if (*end || end == field[0] || seq == 0 || seq > 0xFFFFFFFFull) {
        return -1;
    }
    e->seq = (uint32_t)seq;
    when = strtoll(field[1], &end, 10);
    if (*end || end == field[1] || when < 0) {
        return -1;
    }
    e->when = when;
    if (strcmp(field[2], "R") == 0) {
        e->dir = WAVE_DIR_RX;
    } else if (strcmp(field[2], "T") == 0) {
        e->dir = WAVE_DIR_TX;
    } else {
        return -1;
    }
    for (i = 0; i < sizeof(result_words) / sizeof(result_words[0]); i++) {
        if (strcmp(field[3], result_words[i]) == 0) {
            break;
        }
    }
    if (i == sizeof(result_words) / sizeof(result_words[0])) {
        return -1;
    }
    e->result = (enum wave_result)i;
    /* RX is decoded or undecoded; TX is sent, stopped or failed. */
    if (e->dir == WAVE_DIR_RX ? (e->result != WAVE_RESULT_OK && e->result != WAVE_RESULT_UNDECODED)
                              : e->result == WAVE_RESULT_UNDECODED) {
        return -1;
    }
    count = strtol(field[4], &end, 10);
    if (*end || end == field[4] || count < 0 || count > 999) {
        return -1;
    }
    e->count = (int)count;
    if (strcmp(field[5], "-") != 0) {
        for (t = field[5]; *t; t++) {
            if (*t == 'c') {
                e->captured = 1;
            } else if (*t == 't') {
                e->truncated = 1;
            } else {
                return -1;
            }
        }
    }
    if (!word_ok(field[6], sizeof(e->preset))) {
        return -1;
    }
    snprintf(e->preset, sizeof(e->preset), "%s", field[6]);
    if (strcmp(field[7], "-") != 0) {
        size_t hl = strlen(field[7]);

        if (hl % 2 != 0 || hl / 2 > WAVE_HISTORY_DATA_MAX) {
            return -1;
        }
        for (i = 0; i < hl; i += 2) {
            int hi = hexval(field[7][i]);
            int lo = hexval(field[7][i + 1]);

            if (hi < 0 || lo < 0) {
                return -1;
            }
            e->data[i / 2] = (char)(hi * 16 + lo);
        }
        e->len = hl / 2;
    }
    e->data[e->len] = '\0';
    /* A decoded message or a send has bytes; an undecoded capture has none. */
    if ((e->len == 0) != (e->result == WAVE_RESULT_UNDECODED)) {
        return -1;
    }
    return 0;
}

int wave_history_parse(struct wave_history *h, const char *text, size_t n, int *skipped)
{
    char line[160 + 2 * WAVE_HISTORY_DATA_MAX];
    size_t off = 0;
    size_t magic = strlen(WAVE_HISTORY_MAGIC);
    int bad = 0;

    wave_history_init(h);
    if (skipped) {
        *skipped = 0;
    }
    if (!text || n < magic || memcmp(text, WAVE_HISTORY_MAGIC, magic) != 0 ||
        (n > magic && text[magic] != '\n')) {
        return -1;
    }
    off = magic + 1;
    while (off < n) {
        const char *nl = memchr(text + off, '\n', n - off);
        size_t len = nl ? (size_t)(nl - (text + off)) : n - off;
        struct wave_history_entry e;

        if (len == 0) {
            off += 1;
            continue;
        }
        if (len < sizeof(line)) {
            memcpy(line, text + off, len);
            line[len] = '\0';
            if (memchr(line, '\0', len) == NULL && parse_entry(line, &e) == 0) {
                struct wave_history_entry *slot;

                if (h->count == WAVE_HISTORY_MAX) {
                    h->head = (h->head + 1) % WAVE_HISTORY_MAX;
                    h->count--;
                }
                slot = &h->ring[(h->head + h->count) % WAVE_HISTORY_MAX];
                *slot = e;
                h->count++;
                if (e.seq >= h->next_seq) {
                    h->next_seq = e.seq + 1 ? e.seq + 1 : 1;
                }
            } else {
                bad++;
            }
        } else {
            bad++;
        }
        off += len + 1;
    }
    if (skipped) {
        *skipped = bad;
    }
    h->fold_heard_ms = -1;
    return 0;
}
