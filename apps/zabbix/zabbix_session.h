/*
 * The Zabbix app's link to its helper: one pos-zabbix process for as long
 * as the Zabbix screen is open, driven without ever blocking the LVGL
 * thread.
 *
 * Why a helper process (docs/decisions/ADR-007-zabbix-viewer.md, PROPOSED):
 *
 *   - every network wait - DNS, TCP, TLS, a server that answers slowly or
 *     never - happens in another process; the LVGL thread only ever reads a
 *     socket that already has data (MSG_DONTWAIT);
 *   - libcurl, OpenSSL and the JSON parser stay out of the shell: the shell
 *     links only the bounded model and the line protocol (core/zabbix,
 *     tests/zabbix_lint.sh), and a crash in any of them takes the helper,
 *     not the panel;
 *   - the token is read by the helper from its 0600 file and never enters the
 *     shell's memory at all;
 *   - if the shell dies, or restarts itself to rotate, the helper gets
 *     SIGTERM (PR_SET_PDEATHSIG) and its connection goes with it;
 *   - the bench tool and the app are one binary and one code path.
 *
 * WATCHDOG. The first line within ZABBIX_HELLO_MS. While the helper says it
 * is busy, silence longer than ZABBIX_BUSY_SILENCE_MS - far more than any
 * request's own timeout allows - kills it and the session ends with
 * ZABBIX_EXIT_HUNG. An idle helper may be silent for as long as it likes.
 *
 * No callbacks: what the helper says is applied to the caller's model
 * (zabbix_view.h) inside zabbix_session_poll(), so nothing here can call into
 * an app that has been destroyed.
 *
 * Pure C, no LVGL, clock passed in: tested on a host against the real helper
 * with the fake backend (tests/zabbix_session_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef ZABBIX_SESSION_H
#define ZABBIX_SESSION_H

#include "zabbix/zbx_proto.h"
#include "zabbix_view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ZABBIX_HELLO_MS 3000
#define ZABBIX_BUSY_SILENCE_MS 120000
/* abandon(): from quit to SIGKILL, and after SIGKILL how long to reap. */
#define ZABBIX_KILL_REAP_MS 200
#define ZABBIX_HELPER_PATH_MAX 256
#define ZABBIX_ARG_MAX 256

struct zabbix_session_config {
    const char *helper;         /* NULL: zabbix_session_helper_path() */
    const char *fake;           /* a fake scenario, or NULL for the build's default */
};

struct zabbix_session {
    pid_t pid;
    int fd;                     /* our end of the socketpair, or -1 */
    bool running;
    bool eof;
    bool killed;
    enum zabbix_exit exit_reason;
    char line[ZBX_LINE_MAX + 1];
    size_t line_len;
    bool overlong;
    int64_t hello_by;           /* 0 when not waiting */
    int64_t last_line_ms;
    struct zbx_rx rx;           /* the staging sets: large, so the session lives in the heap */
};

void zabbix_session_init(struct zabbix_session *s);

/* Start the helper. 0, or -1 with a reason in err (the session stays idle). */
int zabbix_session_start(struct zabbix_session *s, const struct zabbix_session_config *cfg,
                         int64_t now_ms, char *err, size_t errlen);

/* Read what the helper wrote and apply it to m; enforce the deadlines; reap
 * it when it has gone (and say so in m). Never blocks. Returns a mask of
 * ZABBIX_CHANGED_* for what changed in m. */
unsigned zabbix_session_poll(struct zabbix_session *s, struct zabbix_model *m, int64_t now_ms);

/* Commands. Each returns 0, or -1 when there is no helper to send it to. */
int zabbix_session_refresh(struct zabbix_session *s);
/* hostid NULL or "": stop reading a host. */
int zabbix_session_detail(struct zabbix_session *s, const char *hostid);
int zabbix_session_scenario(struct zabbix_session *s, const char *name);

/* For a destroyed app or a restart: ask the helper to quit, wait up to
 * grace_ms, then SIGKILL and wait up to ZABBIX_KILL_REAP_MS. Idle afterwards
 * whatever happened; blocks at most grace_ms + ZABBIX_KILL_REAP_MS. */
void zabbix_session_abandon(struct zabbix_session *s, int grace_ms);

bool zabbix_session_active(const struct zabbix_session *s);

/* $POCKETOS_ZABBIX_HELPER, else /usr/bin/pos-zabbix. */
const char *zabbix_session_helper_path(void);
/* The fake scenario to start with when nothing else says: $POCKETOS_ZABBIX_FAKE
 * when $POCKETOS_ZABBIX_BACKEND is "fake", else the build's default (the
 * simulator runs the fake, the device the real backend: NULL). */
const char *zabbix_session_default_fake(void);

#endif
