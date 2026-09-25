/*
 * The helper's side of a Zabbix connection: what to ask, when, and what to
 * do when it fails. It owns the transport, reads the answers through
 * zbx_api.c into the bounded model, and writes the result as protocol lines
 * (zbx_proto.h) to the app.
 *
 * STATES (enum zbx_conn_state):
 *
 *   UNCONFIGURED --(never leaves; the app offers the demo instead)
 *   CONNECTING ----apiinfo.version, then user.login for a password----> ONLINE
 *        ^                                                              |
 *        |                        any failure but AUTH                  v
 *        +--------------------- RETRYING <-------------------------- ONLINE
 *                         (after the backoff: 5, 10, 20, 40, 80, 120 s)
 *   AUTH refused ------------------------------------------------> AUTH_FAILED
 *        (a token is retried after ZBX_AUTH_RETRY_S; a password is never
 *         retried on its own, because Zabbix blocks an account after a few
 *         failed logins; either is tried at once on `refresh`)
 *
 * A session that expires (password logins) is logged in again once, at
 * once, before anything is reported as a failure.
 *
 * SCHEDULE while ONLINE: problems every refresh_s (the overview is built
 * from them), the open host's detail with them, hosts every hosts_s. One
 * request at a time, never two. A `refresh` from the app moves everything to
 * now, at most once per ZBX_MANUAL_REFRESH_S.
 *
 * DATA IS KEPT across failures: the helper never sends an empty set because
 * a request failed. The app decides what is stale from its own clock and the
 * state lines.
 *
 * Time comes from a function the caller supplies, so a test can run hours
 * of backoff in milliseconds.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_ZBX_CLIENT_H
#define POCKETOS_ZBX_CLIENT_H

#include "zabbix/zbx_config.h"
#include "zabbix/zbx_http.h"
#include "zabbix/zbx_model.h"
#include "zabbix/zbx_proto.h"

#include <stdint.h>
#include <stdio.h>

#define ZBX_BACKOFF_FIRST_S 5
#define ZBX_BACKOFF_MAX_S 120
#define ZBX_AUTH_RETRY_S 300
#define ZBX_MANUAL_REFRESH_S 5
#define ZBX_CONNECT_TIMEOUT_MS 5000
#define ZBX_SESSION_MAX 128
/* next_try_ms when no try is scheduled at all (a refused password). */
#define ZBX_TRY_NEVER INT64_MAX
/* user.logout when the helper ends: short, it only tidies the server. */
#define ZBX_LOGOUT_TIMEOUT_MS 3000

struct zbx_client {
    struct zbx_config cfg;
    struct zbx_transport *tr;
    FILE *out;
    int64_t (*now_ms)(void *user);
    void *clock_user;

    enum zbx_conn_state state;
    enum zbx_err err;
    char err_text[ZBX_TEXT_MAX];
    int attempt;                /* failures in a row */
    int64_t next_try_ms;        /* RETRYING / AUTH_FAILED: when */
    bool reported;              /* the state line for the current state was sent */

    bool connected;             /* the version is known and the credentials worked */
    char version[ZBX_VERSION_MAX];
    long vnum;
    char session[ZBX_SESSION_MAX]; /* user.login's, for password configurations */
    bool session_fresh;         /* made in this round: an AUTH now is a real refusal */
    int timeout_override_ms;    /* 0: timeout_s; else this, for the next request only */
    int rpc_id;

    int64_t due_problems_ms;
    int64_t due_hosts_ms;
    int64_t last_manual_ms;
    char detail_hostid[ZBX_ID_MAX];
    bool detail_due;
    unsigned seq;

    /* The kept sets, and the working space an answer is read into. */
    struct zbx_problem_set *problems;
    struct zbx_host_set *hosts;
    struct zbx_detail *detail;
    struct zbx_problem *pscratch; /* ZBX_PROBLEM_FETCH */
    int pscratch_n;               /* every fetched problem, with its host */
    struct zbx_host *hscratch;    /* ZBX_HOST_FETCH */
    struct zbx_host *hscratch2;   /* ZBX_HOST_FETCH: the by-name fill */
    char (*hostids)[ZBX_ID_MAX];  /* ZBX_HOST_FETCH: the hosts read first */
    const char **hostid_ptrs;     /* ZBX_HOST_FETCH: the same, for the request */
    bool have_problems;
    bool have_hosts;
};

/* 0, or -1 when out of memory. The transport stays the caller's. */
int zbx_client_init(struct zbx_client *c, const struct zbx_config *cfg, struct zbx_transport *tr,
                    FILE *out, int64_t (*now_ms)(void *user), void *clock_user);
void zbx_client_free(struct zbx_client *c);

/* Send the opening lines: config and the first state. */
void zbx_client_start(struct zbx_client *c);

/* Forget the server (the fake's scenario changed): not connected, no
 * version, no session, everything due now. The kept sets stay until new
 * ones replace them. Sends the state line. */
void zbx_client_reset(struct zbx_client *c);

/* Act on one command from the app. */
void zbx_client_command(struct zbx_client *c, const struct zbx_cmd *cmd);

/* Do what is due now: at most one refresh round, which may take several
 * requests. Returns the milliseconds until something is next due (for the
 * caller's poll), never less than 0 and never more than 60 000. */
int zbx_client_step(struct zbx_client *c);

/* Password setups: whether a user.login session is open on the server. */
bool zbx_client_has_session(const struct zbx_client *c);

/* End the session on the server (user.logout), as the API asks of every
 * user.login ("to prevent the generation of a large number of open session
 * records"), within timeout_ms. The session is forgotten whatever the
 * answer. 0 when there was nothing to end or it ended, -1 otherwise. Writes
 * no protocol line. */
int zbx_client_logout(struct zbx_client *c, int timeout_ms);

/* The delay after the n-th failure in a row (n >= 1), in seconds. */
int zbx_backoff_s(int n);

#endif
