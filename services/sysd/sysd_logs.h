/*
 * system.logs and system.crashes: what the device has logged, for someone
 * diagnosing it without SSH (the System app's Diagnostics page, `doors call`).
 *
 * Reads the log directory only ($POCKETOS_LOG_DIR, /var/lib/pocketos/log):
 *
 *   <name>.log, <name>.log.1   pocketlog files (core/pocketlog), one line each:
 *                              "2026-09-04T13:20:01.123Z radiod WARN  text"
 *   supervise-<name>.log       pos-supervise's own lines:
 *                              "2026-09-04T13:20:01Z supervise radiod exited rc=1 ..."
 *   crash-<name>-<t>-<pid>.txt crash reports (pocketlog_install_crash_handler)
 *
 * Bounded by construction, whatever is on the card: at most
 * SYSD_LOGS_MAX_FILES files are looked at, and of each only the last
 * SYSD_LOGS_TAIL_BYTES are read (with the rotated .1 file filling the rest
 * of that budget when the current one is shorter). Per file only the newest
 * `limit` matching lines are kept, so memory is limit x files entries, and
 * every message is cut to SYSD_LOGS_MESSAGE_MAX bytes on a UTF-8 boundary with
 * control characters replaced. A line in neither format is counted as
 * skipped, never shown half-parsed; a missing directory answers an empty
 * list with available false. The *.stdio.log captures are not read: they
 * hold only what did not go through pocketlog (docs/api/system.md).
 *
 * Pure C with cJSON; tested against a temporary directory
 * (tests/sysd_logs_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_SYSD_LOGS_H
#define POCKETOS_SYSD_LOGS_H

#include <cjson/cJSON.h>
#include <stddef.h>

#define SYSD_LOGS_MAX_FILES 16
#define SYSD_LOGS_TAIL_BYTES (32 * 1024)
#define SYSD_LOGS_MESSAGE_MAX 240
#define SYSD_LOGS_LIMIT_DEFAULT 50
#define SYSD_LOGS_LIMIT_MAX 100
#define SYSD_CRASHES_MAX 10
#define SYSD_CRASH_FRAMES 3

enum sysd_log_level {
    SYSD_LOG_DEBUG = 0,
    SYSD_LOG_INFO = 1,
    SYSD_LOG_WARN = 2,
    SYSD_LOG_ERROR = 3
};

struct sysd_logs_query {
    enum sysd_log_level min_level; /* the least severe level returned */
    int limit;                     /* 1..SYSD_LOGS_LIMIT_MAX */
    const char *source;            /* one source's lines only, or NULL for all */
};

/* Parse system.logs params ({level: "all"|"warn"|"error", limit, source}).
 * Returns 0, or -1 with a reason in err. */
int sysd_logs_parse_query(const cJSON *params, struct sysd_logs_query *q, char *err, size_t n);

/* The newest matching lines, newest first:
 * {available, entries: [{ts, source, level, message}], returned, skipped,
 *  sources: [names read], scanned_bytes, older_not_scanned}. */
cJSON *sysd_logs_query(const char *log_dir, const struct sysd_logs_query *q);

/* The newest crash reports, newest first:
 * {available, reports: [{file, process, pid, signal, signal_name, time,
 *  version, build, frames: [..]}], total}. */
cJSON *sysd_crashes_list(const char *log_dir);

#endif
