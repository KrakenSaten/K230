/*
 * DeskBuddy's guard log: who was at the desk while it was armed.
 *
 * A fixed ring of DB_GUARD_CAP events. The newest always fits; when the ring
 * is full the oldest goes, so the log can never grow past its size however
 * long the desk is watched or however often a source flaps.
 *
 * An event is a timestamp and a classification - no picture. The optional
 * snapshot_id is reserved for a later, opt-in snapshot store (0 = none; the
 * id would name a file beside the log); v0.1 always writes 0.
 *
 * The text form is what db_store.c writes to guard.v1:
 *
 *     # DeskBuddy guard log. Written by DeskBuddy; see docs/apps/DESKBUDDY.md.
 *     next=<id>
 *     e <id> <wall_s> <subject> <confidence_pm|-1> <acknowledged 0|1> <snapshot_id>
 *
 * oldest first. A line that is not exactly one this build writes is skipped
 * and counted; the rest of the file still loads.
 *
 * Pure C, no LVGL, no I/O (tests/db_guard_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DB_GUARD_H
#define DB_GUARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DB_GUARD_CAP 32
/* Longest text db_guard_format() can produce for a full ring. */
#define DB_GUARD_TEXT_MAX (128 + DB_GUARD_CAP * 80)

enum db_subject {
    DB_SUBJECT_UNKNOWN = 0, /* a person the recognizer said is not the owner */
    DB_SUBJECT_OWNER,       /* the owner came back */
    DB_SUBJECT_PERSON,      /* a person, never identified (recognition off or too slow) */
    DB_SUBJECT_COUNT
};

struct db_guard_event {
    uint32_t id;            /* 1, 2, 3 ... never reused within a log */
    int64_t wall_s;         /* epoch seconds, 0 when the clock was not set */
    enum db_subject subject;
    int16_t confidence_pm;  /* 0..1000, -1 none */
    bool acknowledged;
    uint32_t snapshot_id;   /* reserved, 0 */
};

struct db_guard_log {
    struct db_guard_event ev[DB_GUARD_CAP];
    unsigned head;          /* index of the oldest */
    unsigned count;
    uint32_t next_id;
};

void db_guard_init(struct db_guard_log *log);
/* Append, overwriting the oldest when full. Returns the stored event. */
struct db_guard_event *db_guard_add(struct db_guard_log *log, int64_t wall_s, enum db_subject subject,
                                    int16_t confidence_pm, bool acknowledged);
unsigned db_guard_count(const struct db_guard_log *log);
/* i = 0 is the newest. NULL out of range. */
const struct db_guard_event *db_guard_at(const struct db_guard_log *log, unsigned i);
/* By id; NULL when it has left the ring. */
struct db_guard_event *db_guard_find(struct db_guard_log *log, uint32_t id);
/* Visitors (not the owner) nobody has acknowledged yet. */
unsigned db_guard_unacknowledged(const struct db_guard_log *log);
/* Mark every event acknowledged. Returns how many changed. */
unsigned db_guard_acknowledge_all(struct db_guard_log *log);

const char *db_subject_name(enum db_subject s); /* "unknown", "owner", "person" */

/* Text form. Returns the length, or -1 when out is too small. */
int db_guard_format(const struct db_guard_log *log, char *out, size_t out_len);
/* Replace log with the text's events. Returns the number of lines skipped. */
int db_guard_parse(struct db_guard_log *log, const char *text);

#endif
