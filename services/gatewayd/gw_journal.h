/*
 * gatewayd: the message journal (ADR-019 rule 3 and "Limits";
 * docs/api/gateway.md §7).
 *
 * The last GW_JOURNAL_MAX mesh.message events the gateway saw, in memory
 * only, each under a sequence number of the gateway's own: from 1, one more
 * per entry, never reused while this journal lives. A client that was away
 * resumes from the last number it holds (mesh.sync after_seq).
 *
 * Two identifiers say when a cursor has stopped meaning anything:
 *   journal_id  this journal: new with every start of the gateway
 *   run_id      meshcored's run: its message ids start again at 1 with each,
 *               so the entries of an earlier run are discarded when it
 *               changes. The sequence goes on; an old cursor then lies below
 *               first_seq and is answered with gap.
 *
 * Not history: nothing here reaches storage (meshcored's own rule).
 *
 * Pure: cJSON only, no I/O, no clock.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_JOURNAL_H
#define GATEWAYD_GW_JOURNAL_H

#include "gw.h"

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_JOURNAL_ID_MAX 32
#define GW_RUN_ID_MAX 32

struct gw_journal_entry {
    uint64_t seq;
    double msg_id;      /* the message's own id, within its run */
    size_t size;        /* bytes as JSON */
    cJSON *message;
};

struct gw_journal {
    char id[GW_JOURNAL_ID_MAX + 1];
    char run_id[GW_RUN_ID_MAX + 1];   /* "" until meshcored has said */
    uint64_t next_seq;
    int head;                         /* index of the oldest entry */
    int count;
    struct gw_journal_entry e[GW_JOURNAL_MAX];
};

/* 1 to 32 characters of [A-Za-z0-9_-]: what an identifier here may be. */
bool gw_ident_ok(const char *s);

/* id is this journal's journal_id. 0, or -1 when it is not an identifier. */
int gw_journal_init(struct gw_journal *j, const char *id);
void gw_journal_free(struct gw_journal *j);

/* meshcored's run. Returns 1 when it differs from the run held (the entries
 * are gone), 0 when it is the same, -1 when it is not an identifier (nothing
 * changes). */
int gw_journal_set_run(struct gw_journal *j, const char *run_id);

/* Journals one message, which is taken (and freed when not kept). Returns
 * its seq, or 0 when it was not journaled: no run is known, it has no
 * usable id, it is larger than GW_JOURNAL_MSG_MAX, there is no memory, or
 * it equals the newest entry of the same id (a repeated event, or a
 * snapshot that says what an event already said). *stored, when given, is
 * the journal's own copy: valid until the next call. */
uint64_t gw_journal_put(struct gw_journal *j, cJSON *message, const cJSON **stored);

uint64_t gw_journal_seq(const struct gw_journal *j);        /* the newest held; 0: none yet */
uint64_t gw_journal_first_seq(const struct gw_journal *j);  /* the oldest held, or the next */

/* The answer to mesh.sync: journal_id, run_id, seq, first_seq, messages[]
 * of {seq, message}, more, and gap when the cursor is not one this journal
 * can continue from (older than first_seq, or newer than seq). Without a
 * cursor: the newest `limit` entries. limit is 1 to GW_SYNC_LIMIT; the
 * answer also stays under GW_SYNC_BYTES. NULL when there is no memory. */
cJSON *gw_journal_sync(const struct gw_journal *j, bool have_after, uint64_t after_seq, int limit);

#endif
