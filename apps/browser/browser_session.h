/*
 * The Browser app's link to its helper: one pos-browser process for as
 * long as the Browser screen is open (started on the first page, not at
 * open), driven without ever blocking the LVGL thread. The pattern is
 * Zabbix's (ADR-007 option D, zabbix_session.h); ADR-009 says why it fits a
 * browser even better:
 *
 *   - every network wait, TLS, the HTML reader and the image decoders run
 *     in another process; the LVGL thread only reads a socket that already
 *     has data (MSG_DONTWAIT) and small files the helper finished writing;
 *   - libcurl, OpenSSL, libjpeg and libpng never enter the shell, which links
 *     only the bounded document and the line protocol (tests/browser_lint.sh);
 *     a crash in any of them takes the helper, not the panel;
 *   - if the shell dies, the helper gets SIGTERM (PR_SET_PDEATHSIG); the
 *     socket is close-on-exec, so a rotation restart (an exec) ends it too.
 *
 * PICTURES arrive as files: the session makes a private 0700 directory under
 * the runtime directory, the helper writes each decoded picture there as raw
 * RGB565, and browser_session_poll() reads it (exactly w*h*2 bytes from a
 * name web_rx_file_name_ok() accepts, never through a link) and removes it.
 * Leaving removes the directory and anything left in it.
 *
 * WATCHDOG. The first line within BROWSER_HELLO_MS. While a page loads,
 * silence longer than BROWSER_BUSY_SILENCE_MS - more than any request's own
 * timeouts allow - kills the helper. A STOP the helper has not answered
 * within BROWSER_STOP_GRACE_MS (a resolver stuck in the kernel) kills it
 * too; the next page starts a fresh one.
 *
 * Pure C, no LVGL, clock passed in: tests/browser_session_test.c runs it
 * against the real helper on the fake network.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BROWSER_SESSION_H
#define BROWSER_SESSION_H

#include "browser_view.h"
#include "web/web_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define BROWSER_HELLO_MS 3000
#define BROWSER_BUSY_SILENCE_MS 60000
#define BROWSER_STOP_GRACE_MS 4000
/* abandon(): from quit to SIGKILL, and after SIGKILL how long to reap. */
#define BROWSER_KILL_REAP_MS 200
#define BROWSER_PATH_MAX 256

struct browser_session_config {
    const char *helper;     /* NULL: browser_session_helper_path() */
    bool fake;              /* the fake network */
    const char *ca_file;    /* NULL: the system store */
    const char *runtime;    /* where the picture directory goes; NULL: pocketos_runtime_dir() */
};

struct browser_session {
    pid_t pid;
    int fd;
    bool running;
    bool eof;
    bool killed;
    enum browser_exit exit_reason;
    char line[WEB_LINE_MAX + 1];
    size_t line_len;
    bool overlong;
    int64_t hello_by;           /* 0 when not waiting */
    int64_t last_line_ms;
    int stop_seq;               /* a stop sent and not yet answered, or 0 */
    int64_t stop_by;
    bool stop_killed;           /* killed for not answering a stop */
    char img_dir[BROWSER_PATH_MAX];
    struct web_rx rx;
};

void browser_session_init(struct browser_session *s);

/* Start the helper. 0, or -1 with a reason in err (the session stays idle). */
int browser_session_start(struct browser_session *s, const struct browser_session_config *cfg,
                          int64_t now_ms, char *err, size_t errlen);

/* Read what the helper wrote and apply it to v; enforce the deadlines; reap
 * the helper when it has gone (and tell v). Never blocks. Returns a mask of
 * BROWSER_CHANGED_* for what changed in v. */
unsigned browser_session_poll(struct browser_session *s, struct browser_view *v, int64_t now_ms);

/* Carry out what the view asked for. 0, or -1 when there is no helper. */
int browser_session_send(struct browser_session *s, const struct browser_cmd *cmd, int64_t now_ms);

/* Ask the helper to quit, wait up to grace_ms, then SIGKILL and wait up to
 * BROWSER_KILL_REAP_MS; remove the picture directory. Idle afterwards;
 * blocks at most grace_ms + BROWSER_KILL_REAP_MS. */
void browser_session_abandon(struct browser_session *s, int grace_ms);

bool browser_session_active(const struct browser_session *s);

/* $POCKETOS_BROWSER_HELPER, else /usr/bin/pos-browser. */
const char *browser_session_helper_path(void);

#endif
