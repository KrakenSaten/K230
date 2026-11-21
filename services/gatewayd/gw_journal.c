/*
 * gatewayd: the message journal. See gw_journal.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gw_journal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

bool gw_ident_ok(const char *s)
{
    size_t i;

    if (s == NULL || s[0] == '\0') {
        return false;
    }
    for (i = 0; s[i] != '\0'; i++) {
        char c = s[i];

        if (i >= GW_RUN_ID_MAX || !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                    c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static struct gw_journal_entry *at(const struct gw_journal *j, int i)
{
    return (struct gw_journal_entry *)&j->e[(j->head + i) % GW_JOURNAL_MAX];
}

static void clear(struct gw_journal *j)
{
    int i;

    for (i = 0; i < j->count; i++) {
        cJSON_Delete(at(j, i)->message);
        at(j, i)->message = NULL;
    }
    j->head = 0;
    j->count = 0;
}

int gw_journal_init(struct gw_journal *j, const char *id)
{
    memset(j, 0, sizeof(*j));
    j->next_seq = 1;
    if (!gw_ident_ok(id)) {
        return -1;
    }
    memcpy(j->id, id, strlen(id) + 1);
    return 0;
}

void gw_journal_free(struct gw_journal *j)
{
    clear(j);
}

int gw_journal_set_run(struct gw_journal *j, const char *run_id)
{
    if (!gw_ident_ok(run_id)) {
        return -1;
    }
    if (strcmp(j->run_id, run_id) == 0) {
        return 0;
    }
    /* Another run: its ids start at 1 again, so an entry of the old run and
     * one of the new would be the same message to a client. */
    clear(j);
    memcpy(j->run_id, run_id, strlen(run_id) + 1);
    return 1;
}

uint64_t gw_journal_seq(const struct gw_journal *j)
{
    return j->next_seq - 1;
}

uint64_t gw_journal_first_seq(const struct gw_journal *j)
{
    return j->count > 0 ? at(j, 0)->seq : j->next_seq;
}

uint64_t gw_journal_put(struct gw_journal *j, cJSON *message, const cJSON **stored)
{
    const cJSON *id = cJSON_IsObject(message) ? cJSON_GetObjectItemCaseSensitive(message, "id") : NULL;
    struct gw_journal_entry *e;
    char *text;
    size_t size;
    int i;

    if (stored != NULL) {
        *stored = NULL;
    }
    if (j->run_id[0] == '\0' || !cJSON_IsNumber(id) || !(id->valuedouble >= 1.0) ||
        id->valuedouble > 9007199254740992.0 || id->valuedouble != floor(id->valuedouble)) {
        cJSON_Delete(message);
        return 0;
    }
    /* The newest entry of this message decides: the same again is not news. */
    for (i = j->count - 1; i >= 0; i--) {
        e = at(j, i);
        if (e->msg_id == id->valuedouble) {
            if (cJSON_Compare(e->message, message, 1)) {
                cJSON_Delete(message);
                return 0;
            }
            break;
        }
    }
    text = cJSON_PrintUnformatted(message);
    if (text == NULL) {
        cJSON_Delete(message);
        return 0;
    }
    size = strlen(text);
    free(text);
    if (size > GW_JOURNAL_MSG_MAX) {
        cJSON_Delete(message);
        return 0;
    }
    if (j->count == GW_JOURNAL_MAX) {
        cJSON_Delete(at(j, 0)->message);
        at(j, 0)->message = NULL;
        j->head = (j->head + 1) % GW_JOURNAL_MAX;
        j->count--;
    }
    e = at(j, j->count);
    e->seq = j->next_seq++;
    e->msg_id = id->valuedouble;
    e->size = size;
    e->message = message;
    j->count++;
    if (stored != NULL) {
        *stored = message;
    }
    return e->seq;
}

const cJSON *gw_journal_find(const struct gw_journal *j, double msg_id)
{
    int i;

    for (i = j->count - 1; i >= 0; i--) {
        if (at(j, i)->msg_id == msg_id) {
            return at(j, i)->message;
        }
    }
    return NULL;
}

cJSON *gw_journal_sync(const struct gw_journal *j, bool have_after, uint64_t after_seq, int limit)
{
    uint64_t first = gw_journal_first_seq(j), newest = gw_journal_seq(j);
    cJSON *r = cJSON_CreateObject(), *arr = cJSON_CreateArray();
    bool gap = false, failed = false;
    size_t bytes = 0;
    int from, to, i;

    if (limit < 1) {
        limit = 1;
    }
    if (limit > GW_SYNC_LIMIT) {
        limit = GW_SYNC_LIMIT;
    }
    if (!have_after) {
        /* The newest, and of those as many as fit, counted from the end. */
        to = j->count;
        from = to;
        while (from > 0 && to - from < limit && bytes + at(j, from - 1)->size + 64 <= GW_SYNC_BYTES) {
            from--;
            bytes += at(j, from)->size + 64;
        }
    } else {
        if (after_seq > newest || after_seq + 1 < first) {
            /* Not a cursor into what is held: what lay between is lost to
             * this client, and it is told so. */
            gap = true;
            from = 0;
        } else {
            from = (int)(after_seq + 1 - first);
        }
        to = from;
        while (to < j->count && to - from < limit && bytes + at(j, to)->size + 64 <= GW_SYNC_BYTES) {
            bytes += at(j, to)->size + 64;
            to++;
        }
    }
    if (r == NULL || arr == NULL) {
        cJSON_Delete(r);
        cJSON_Delete(arr);
        return NULL;
    }
    for (i = from; i < to && !failed; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON *copy = cJSON_Duplicate(at(j, i)->message, 1);

        if (item == NULL || copy == NULL || cJSON_AddNumberToObject(item, "seq", (double)at(j, i)->seq) == NULL) {
            cJSON_Delete(item);
            cJSON_Delete(copy);
            failed = true;
            break;
        }
        cJSON_AddItemToObject(item, "message", copy);
        cJSON_AddItemToArray(arr, item);
    }
    if (failed) {
        cJSON_Delete(r);
        cJSON_Delete(arr);
        return NULL;
    }
    cJSON_AddStringToObject(r, "journal_id", j->id);
    cJSON_AddStringToObject(r, "run_id", j->run_id);
    cJSON_AddNumberToObject(r, "seq", (double)newest);
    cJSON_AddNumberToObject(r, "first_seq", (double)first);
    cJSON_AddItemToObject(r, "messages", arr);
    cJSON_AddBoolToObject(r, "more", have_after && to < j->count);
    if (gap) {
        cJSON_AddBoolToObject(r, "gap", 1);
    }
    return r;
}
