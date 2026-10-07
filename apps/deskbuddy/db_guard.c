/*
 * DeskBuddy's guard log. See db_guard.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "db_guard.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define LINE_MAX_LEN 128

static const char *const subject_names[DB_SUBJECT_COUNT] = { "unknown", "owner", "person" };

void db_guard_init(struct db_guard_log *log)
{
    if (log) {
        memset(log, 0, sizeof(*log));
        log->next_id = 1;
    }
}

struct db_guard_event *db_guard_add(struct db_guard_log *log, int64_t wall_s, enum db_subject subject,
                                    int16_t confidence_pm, bool acknowledged)
{
    struct db_guard_event *e;

    if (!log || (int)subject < 0 || subject >= DB_SUBJECT_COUNT) {
        return NULL;
    }
    if (log->count == DB_GUARD_CAP) {
        log->head = (log->head + 1) % DB_GUARD_CAP;
        log->count--;
    }
    e = &log->ev[(log->head + log->count) % DB_GUARD_CAP];
    memset(e, 0, sizeof(*e));
    e->id = log->next_id++;
    if (log->next_id == 0) {
        log->next_id = 1;
    }
    e->wall_s = wall_s > 0 ? wall_s : 0;
    e->subject = subject;
    e->confidence_pm = (confidence_pm >= 0 && confidence_pm <= 1000) ? confidence_pm : -1;
    e->acknowledged = acknowledged;
    log->count++;
    return e;
}

unsigned db_guard_count(const struct db_guard_log *log)
{
    return log ? log->count : 0;
}

const struct db_guard_event *db_guard_at(const struct db_guard_log *log, unsigned i)
{
    if (!log || i >= log->count) {
        return NULL;
    }
    return &log->ev[(log->head + log->count - 1 - i) % DB_GUARD_CAP];
}

struct db_guard_event *db_guard_find(struct db_guard_log *log, uint32_t id)
{
    unsigned i;

    for (i = 0; log && id && i < log->count; i++) {
        struct db_guard_event *e = &log->ev[(log->head + i) % DB_GUARD_CAP];

        if (e->id == id) {
            return e;
        }
    }
    return NULL;
}

unsigned db_guard_unacknowledged(const struct db_guard_log *log)
{
    unsigned i;
    unsigned n = 0;

    for (i = 0; log && i < log->count; i++) {
        const struct db_guard_event *e = &log->ev[(log->head + i) % DB_GUARD_CAP];

        n += !e->acknowledged && e->subject != DB_SUBJECT_OWNER;
    }
    return n;
}

unsigned db_guard_acknowledge_all(struct db_guard_log *log)
{
    unsigned i;
    unsigned n = 0;

    for (i = 0; log && i < log->count; i++) {
        struct db_guard_event *e = &log->ev[(log->head + i) % DB_GUARD_CAP];

        n += !e->acknowledged;
        e->acknowledged = true;
    }
    return n;
}

const char *db_subject_name(enum db_subject s)
{
    return ((int)s >= 0 && s < DB_SUBJECT_COUNT) ? subject_names[s] : "?";
}

int db_guard_format(const struct db_guard_log *log, char *out, size_t out_len)
{
    size_t used;
    unsigned i;
    int n;

    if (!log || !out) {
        return -1;
    }
    n = snprintf(out, out_len,
                 "# DeskBuddy guard log. Written by DeskBuddy; see docs/apps/DESKBUDDY.md.\n"
                 "next=%" PRIu32 "\n",
                 log->next_id);
    if (n < 0 || (size_t)n >= out_len) {
        return -1;
    }
    used = (size_t)n;
    for (i = 0; i < log->count; i++) {
        const struct db_guard_event *e = &log->ev[(log->head + i) % DB_GUARD_CAP];

        n = snprintf(out + used, out_len - used, "e %" PRIu32 " %" PRId64 " %s %d %d %" PRIu32 "\n", e->id,
                     e->wall_s, db_subject_name(e->subject), (int)e->confidence_pm, e->acknowledged ? 1 : 0,
                     e->snapshot_id);
        if (n < 0 || (size_t)n >= out_len - used) {
            return -1;
        }
        used += (size_t)n;
    }
    return (int)used;
}

static int parse_event(const char *line, struct db_guard_event *e)
{
    char subject[16];
    uint32_t id;
    int64_t wall;
    int conf;
    int ack;
    uint32_t snap;
    int end = -1;
    int k;

    if (sscanf(line, "e %" SCNu32 " %" SCNd64 " %15s %d %d %" SCNu32 "%n", &id, &wall, subject, &conf, &ack,
               &snap, &end) != 6 ||
        line[end] != '\0' || id == 0 || wall < 0 || conf < -1 || conf > 1000 || (ack != 0 && ack != 1)) {
        return -1;
    }
    for (k = 0; k < DB_SUBJECT_COUNT; k++) {
        if (strcmp(subject, subject_names[k]) == 0) {
            break;
        }
    }
    if (k == DB_SUBJECT_COUNT) {
        return -1;
    }
    memset(e, 0, sizeof(*e));
    e->id = id;
    e->wall_s = wall;
    e->subject = (enum db_subject)k;
    e->confidence_pm = (int16_t)conf;
    e->acknowledged = ack == 1;
    e->snapshot_id = snap;
    return 0;
}

int db_guard_parse(struct db_guard_log *log, const char *text)
{
    uint32_t max_id = 0;
    uint32_t next = 0;
    int skipped = 0;

    if (!log) {
        return 0;
    }
    db_guard_init(log);
    while (text && *text) {
        char line[LINE_MAX_LEN];
        size_t n = strcspn(text, "\n");
        struct db_guard_event e;
        unsigned long v;
        char tail;

        if (n < sizeof(line)) {
            memcpy(line, text, n);
            line[n] = '\0';
            if (n > 0 && line[n - 1] == '\r') {
                line[n - 1] = '\0';
            }
            if (line[0] == '\0' || line[0] == '#') {
                /* nothing */
            } else if (sscanf(line, "next=%lu%c", &v, &tail) == 1 && v > 0 && v <= UINT32_MAX) {
                next = (uint32_t)v;
            } else if (parse_event(line, &e) == 0 && e.id > max_id) {
                /* Ids only go up; one that does not is not this build's. */
                if (log->count == DB_GUARD_CAP) {
                    log->head = (log->head + 1) % DB_GUARD_CAP;
                    log->count--;
                }
                log->ev[(log->head + log->count) % DB_GUARD_CAP] = e;
                log->count++;
                max_id = e.id;
            } else {
                skipped++;
            }
        } else {
            skipped++;
        }
        text += n;
        if (*text == '\n') {
            text++;
        }
    }
    log->next_id = next > max_id ? next : max_id + 1;
    if (log->next_id == 0) {
        log->next_id = 1;
    }
    return skipped;
}
