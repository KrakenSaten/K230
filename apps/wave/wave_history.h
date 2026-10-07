/*
 * Wave's history: what was sent and what was heard, newest first, bounded.
 *
 * A ring of WAVE_HISTORY_MAX entries in memory, and the same entries as one
 * small text file (wave_store.c). When the ring is full the oldest entry
 * goes: the file never grows past the ring, whatever happens.
 *
 * Each entry says which way the message went (RX or TX), when (wall-clock
 * seconds, or 0 while the board's clock is not set), the preset in use, how
 * it ended (sent, stopped, failed, heard but not decoded), the payload
 * bytes, and a count: the copies a send played, or how many times the same
 * message was heard.
 *
 * REPEATS. A sender that plays two copies, or two people pressing send, give
 * the listener the same bytes twice within a few seconds. The second hearing
 * folds into the first entry (count 2) when it arrives within the preset's
 * window of the last hearing, so a ROBUST exchange reads as one message. The
 * window is measured on the monotonic clock of this run and is not stored:
 * after a restart the next hearing is a new entry.
 *
 * Pure C, no LVGL, no files, no clocks read: tests/wave_model_test.c. The
 * text form is here so it is tested without a filesystem; wave_store.c only
 * moves it to and from disk.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETWAVE_HISTORY_H
#define POCKETWAVE_HISTORY_H

#include "wave_preset.h"
#include "wave_protocol.h"

#include <stddef.h>
#include <stdint.h>

/* Entries kept. At most about 200 bytes each on disk: 8 KB in all. */
#define WAVE_HISTORY_MAX 40
/* Payload bytes kept per entry: every message Wave can send or accept (the
 * modem reports a longer one as not decoded, never as a message). */
#define WAVE_HISTORY_DATA_MAX WAVE_MAX_MESSAGE_BYTES
/* The first line of the text form, so a later format is refused rather than
 * misread. */
#define WAVE_HISTORY_MAGIC "wave-history 1"
/* The longest text form: the magic and every entry at its longest. */
#define WAVE_HISTORY_TEXT_MAX (32 + WAVE_HISTORY_MAX * (96 + 2 * WAVE_HISTORY_DATA_MAX))

enum wave_dir {
    WAVE_DIR_RX = 0,
    WAVE_DIR_TX
};

enum wave_result {
    WAVE_RESULT_OK = 0,     /* RX: decoded. TX: every copy played. */
    WAVE_RESULT_STOPPED,    /* TX: stopped by the person before the last copy */
    WAVE_RESULT_FAILED,     /* TX: the helper failed (count = copies that did play) */
    WAVE_RESULT_UNDECODED   /* RX: a capture held nothing that decoded */
};

struct wave_history_entry {
    uint32_t seq;           /* grows by one per entry, never reused in a file */
    int64_t when;           /* wall-clock seconds, 0 when unknown */
    enum wave_dir dir;
    enum wave_result result;
    int count;              /* TX: copies played. RX: times heard. */
    int captured;           /* RX: decoded from a capture rather than live */
    int truncated;          /* RX: more bytes arrived than are kept */
    char preset[WAVE_PRESET_ID_MAX];
    size_t len;
    char data[WAVE_HISTORY_DATA_MAX + 1]; /* NUL-terminated for convenience */
};

struct wave_history {
    struct wave_history_entry ring[WAVE_HISTORY_MAX];
    int head;               /* index of the oldest entry */
    int count;
    uint32_t next_seq;
    /* Folding: when the newest entry was last heard (monotonic ms), -1 when
     * the newest entry cannot be folded into. */
    int64_t fold_heard_ms;
    unsigned changes;       /* bumped on every change, for saving and painting */
};

void wave_history_init(struct wave_history *h);
int wave_history_count(const struct wave_history *h);
/* 0 is the newest. NULL outside 0..count-1. */
const struct wave_history_entry *wave_history_at(const struct wave_history *h, int i);

/* Append a TX entry; returns it. */
const struct wave_history_entry *wave_history_add_tx(struct wave_history *h, const char *preset,
                                                     const char *data, size_t len,
                                                     enum wave_result result, int copies,
                                                     int64_t when);

/* A decoded message. Folds into the newest entry, and returns 1, when that
 * entry is a decoded RX of the same bytes last heard at most window_ms ago
 * (now_ms on the monotonic clock); otherwise appends and returns 0. */
int wave_history_add_rx(struct wave_history *h, const char *preset, const char *data, size_t len,
                        int captured, int64_t when, int64_t now_ms, int window_ms);

/* A capture that held nothing decodable. */
void wave_history_add_undecoded(struct wave_history *h, const char *preset, int64_t when);

void wave_history_clear(struct wave_history *h);

/* The text form: WAVE_HISTORY_MAGIC, then one line per entry, oldest first:
 *
 *   <seq> <when> <R|T> <ok|stopped|failed|undecoded> <count> <flags> <preset> <hex|->
 *
 * flags is a word of letters: c captured, t truncated, - none. Writes at most
 * n bytes including the terminator; returns the length, or -1 when it did
 * not fit. */
long wave_history_format(const struct wave_history *h, char *out, size_t n);

/* Replace h with what a text form holds. A line that is not an entry is
 * skipped (and counted in *skipped when not NULL), so one damaged line costs
 * one entry, not the history. More than WAVE_HISTORY_MAX entries keep the
 * newest. 0, or -1 when the first line is not the magic (h is then left
 * empty). */
int wave_history_parse(struct wave_history *h, const char *text, size_t n, int *skipped);

#endif
