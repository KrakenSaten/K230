/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef NINJA_SESSION_H
#define NINJA_SESSION_H
#include "ninjaone/ninja_model.h"
#include <stddef.h>
#include <sys/types.h>
struct ninja_session {
    pid_t pid;
    int fd;
    size_t used;
    int64_t deadline;
    struct ninja_snapshot *incoming;
};
void ninja_session_init(struct ninja_session *s);
/* One new process per snapshot. Closing the old descriptor and killing/reaping
 * the previous child cancels old work and makes late replies unreachable. */
int ninja_session_start(struct ninja_session *s, int64_t now, const char *fake);
/* 0 pending, 1 complete validated frame, -1 failed/watchdog. At most 64 KiB
 * read per call; no waits and no JSON parsing on the UI thread. */
int ninja_session_poll(struct ninja_session *s, int64_t now, struct ninja_snapshot *out);
void ninja_session_close(struct ninja_session *s);
#endif
