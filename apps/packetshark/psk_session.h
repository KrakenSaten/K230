/*
 * PacketShark's link to its helper: one pos-capture process per capture,
 * driven without ever blocking the LVGL thread (ADR-016 accepted; the
 * pattern of apps/zabbix/zabbix_session.h, ADR-007 accepted).
 *
 * Why a helper process: the capture socket, the parser of hostile bytes and
 * every packet live in pos-capture, so a decoder fault takes the helper and
 * not the panel, and the shell never holds a raw socket. One helper serves
 * ONE capture (it gives up its capabilities once the socket is open), so
 * NEW CAPTURE is a new helper.
 *
 * Lifecycle:
 *   - spawn: socketpair, fork, PR_SET_PDEATHSIG(SIGTERM) and a parent check,
 *     every inherited descriptor above stderr closed, stderr to /dev/null,
 *     execv. If the shell dies the helper gets SIGTERM; a rotation restart
 *     is an exec, which that signal does not see, so the shell closes the
 *     app first (shell.c app_close) and the socket is close-on-exec besides;
 *   - watchdog: the hello within PSK_HELLO_MS; while a capture is running,
 *     silence longer than PSK_SILENCE_MS (stats come four times a second)
 *     kills it and the session ends with PSK_EXIT_HUNG. An idle or stopped
 *     helper may be silent;
 *   - protocol damage: an over-long line, or more than PSK_BAD_LINES lines
 *     that do not parse, kills it (PSK_EXIT_PROTOCOL);
 *   - abandon: `quit`, SIGTERM, up to grace_ms, then SIGKILL and at most
 *     PSK_KILL_REAP_MS more. Never longer, whatever the helper does.
 *
 * No callbacks: lines are applied to the caller's model inside
 * psk_session_poll(), so nothing here calls into a destroyed app.
 *
 * Pure C, no LVGL, clock passed in (tests/psk_session_test.c runs it against
 * the real helper on the synthetic source).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PSK_SESSION_H
#define PSK_SESSION_H

#include "pocketpcap/ppc_proto.h"
#include "psk_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define PSK_HELLO_MS 3000
#define PSK_SILENCE_MS 5000
#define PSK_BAD_LINES 16
#define PSK_DESTROY_GRACE_MS 300
#define PSK_KILL_REAP_MS 200
#define PSK_HELPER_PATH_MAX 256
#define PSK_SOURCE_MAX 256

struct psk_session_config {
    const char *helper;  /* NULL: psk_session_helper_path() */
    const char *source;  /* NULL: psk_session_default_source(); "" for none */
};

struct psk_session {
    pid_t pid;
    int fd;              /* our end of the socketpair, or -1 */
    bool running;
    bool eof;
    bool killed;
    enum psk_exit exit_reason;
    char line[PPC_LINE_MAX + 1];
    size_t line_len;
    bool overlong;
    unsigned bad;
    int64_t hello_by;    /* 0 when not waiting */
    int64_t last_line_ms;
    uint32_t next_req;
};

void psk_session_init(struct psk_session *s);

/* Start a helper. 0, or -1 with a reason in err (the session stays idle). */
int psk_session_start(struct psk_session *s, const struct psk_session_config *cfg, int64_t now_ms,
                      char *err, size_t errlen);

/* Read what the helper wrote and apply it to m; enforce the deadlines; reap
 * it when it has gone (and say so in m). Never blocks. Returns
 * PSK_CHANGED_* bits. */
unsigned psk_session_poll(struct psk_session *s, struct psk_model *m, int64_t now_ms);

/* Commands. Each returns 0, or -1 when there is no helper or the line could
 * not be sent whole. */
int psk_session_capture(struct psk_session *s, const char *iface, enum ppc_keep keep, int promisc,
                        enum ppc_cfilter cf, uint32_t limit_s);
int psk_session_hold(struct psk_session *s, bool on);
int psk_session_stop(struct psk_session *s);
int psk_session_discard(struct psk_session *s);
/* Ask for a window of rows; the model takes only the answer to the newest
 * request (m->want_req is set here). seq 0: the tail. */
int psk_session_window(struct psk_session *s, struct psk_model *m, uint32_t seq, uint32_t before,
                       uint32_t after);
/* The same at any anchor: the tail, a seq, a rank, or a time in us. */
int psk_session_window_at(struct psk_session *s, struct psk_model *m, enum ppc_anchor anchor, uint64_t at,
                          uint32_t before, uint32_t after);
/* The list's display filter and sort (M5). */
int psk_session_view(struct psk_session *s, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                     uint16_t port, bool pinned);
/* One packet for the inspector, or its neighbour in the view (step -1, 1)
 * (M6); the model takes only the answer to the newest request. */
int psk_session_inspect(struct psk_session *s, struct psk_model *m, uint32_t seq, int step);
/* The view with the findings to show (PPC_SIG_* bits, 0: any packet) (M7). */
int psk_session_view_signals(struct psk_session *s, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                             uint16_t port, bool pinned, unsigned signals);
/* The same with a diagnosis to show (enum ppc_diag_code, 0: any) (M11.1). */
int psk_session_view_diag(struct psk_session *s, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                          uint16_t port, bool pinned, unsigned signals, unsigned diag);
/* Where the view's findings are, for the rail; the model takes only the
 * answer to the newest request. */
int psk_session_marks(struct psk_session *s, struct psk_model *m, uint32_t buckets);
/* The network intelligence's lists as they stand (M12.1); the model takes
 * only the answer to the newest request. */
int psk_session_net(struct psk_session *s, struct psk_model *m);
/* Write the stopped capture to a file in the captures folder (M8). */
int psk_session_save(struct psk_session *s);
int psk_session_pin(struct psk_session *s, uint32_t seq, bool on);

/* For a destroyed app or a new capture: quit, SIGTERM, wait up to grace_ms,
 * SIGKILL, wait up to PSK_KILL_REAP_MS. Idle afterwards whatever happened. */
void psk_session_abandon(struct psk_session *s, int grace_ms);

bool psk_session_active(const struct psk_session *s);

/* $POCKETOS_CAPTURE_HELPER, else /usr/bin/pos-capture. */
const char *psk_session_helper_path(void);
/* $POCKETOS_CAPTURE_SOURCE, else the build's default: the simulator
 * captures synthetic traffic (PSK_SOURCE_DEFAULT), the device the
 * interface the user chose (NULL). */
const char *psk_session_default_source(void);

#endif
