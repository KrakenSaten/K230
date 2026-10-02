/*
 * What the Zabbix screen knows and what it shows, without LVGL.
 *
 * THE MODEL is everything the helper has said (zbx_proto.h), applied line by
 * line by zabbix_session_poll(), plus when each part arrived on this board's
 * monotonic clock. A set replaces the previous one only when it arrived
 * whole; a failed refresh never empties anything.
 *
 * THE VIEW functions turn the model into the words and tones of each screen:
 * the connection banner, OVERVIEW, a PROBLEMS row, a HOSTS row, the host
 * detail and STATUS. Every one of them takes the time as an argument, so
 * "3m ago" and "stale" are tested on a host with TZ=UTC and no real clock.
 *
 * STALE. Data is stale when the last attempt to refresh it failed, or when
 * it is older than twice its refresh interval plus a grace period (a helper
 * that is busy for too long). Stale data stays on screen, with the banner
 * saying how old it is: a monitoring terminal that blanks itself when the
 * server hiccups hides exactly what its user needs to see.
 *
 * AGES. A problem's age is the server's clock at the refresh (the HTTP Date
 * header) plus the time since, on this board's monotonic clock, minus the
 * problem's start. The board's own wall clock is never used: it reads 1970
 * until NTP answers (docs/hardware/T-DISPLAY-K230.md).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef ZABBIX_VIEW_H
#define ZABBIX_VIEW_H

#include "zabbix/zbx_model.h"
#include "zabbix/zbx_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How far past its interval a set may be before it counts as stale. */
#define ZABBIX_STALE_GRACE_MS 15000
/* How long the app waits before starting a helper that stopped, doubling
 * up to the maximum. */
#define ZABBIX_RESTART_FIRST_MS 2000
#define ZABBIX_RESTART_MAX_MS 30000

enum zabbix_exit {
    ZABBIX_EXIT_NORMAL = 0,     /* it left by itself */
    ZABBIX_EXIT_HUNG,           /* a deadline passed and it was killed */
    ZABBIX_EXIT_PROTOCOL,       /* it said something impossible and was killed */
    ZABBIX_EXIT_CRASHED,        /* a signal nobody here sent */
    ZABBIX_EXIT_START,          /* it could not be started at all */
};

/* What zabbix_session_poll() changed. */
#define ZABBIX_CHANGED_STATE 0x01u
#define ZABBIX_CHANGED_PROBLEMS 0x02u
#define ZABBIX_CHANGED_HOSTS 0x04u
#define ZABBIX_CHANGED_DETAIL 0x08u
#define ZABBIX_CHANGED_EXITED 0x10u
#define ZABBIX_CHANGED_SETTINGS 0x20u  /* a settings or a cresult line */

struct zabbix_model {
    /* what the helper said about itself */
    bool have_hello;
    bool fake;
    char scenario[ZBX_TEXT_MAX];
    bool have_config;
    char label[ZBX_TEXT_MAX];
    char url[ZBX_URL_MAX];
    char auth[ZBX_TEXT_MAX];    /* "token", "password", "none" */
    bool verify;
    int refresh_s;
    int hosts_s;
    char version[ZBX_VERSION_MAX];

    /* the connection */
    enum zbx_conn_state state;
    enum zbx_err err;
    char err_text[ZBX_TEXT_MAX];
    int attempt;
    int retry_s;
    int64_t state_ms;           /* when the state line arrived */
    enum zbx_err last_err;      /* the most recent failure, kept after recovery */
    char last_err_text[ZBX_TEXT_MAX];
    int64_t last_err_ms;
    int64_t last_ok_ms;         /* the last time the helper said online, or a set arrived */
    bool busy;
    char busy_what[ZBX_TEXT_MAX];

    /* the data, each with its arrival time (0: never) */
    struct zbx_problem_set problems;
    int64_t problems_ms;
    struct zbx_host_set hosts;
    int64_t hosts_ms;
    struct zbx_detail detail;
    int64_t detail_ms;
    char detail_missing[ZBX_ID_MAX]; /* the host the helper could not read */
    char detail_missing_text[ZBX_TEXT_MAX];

    /* the CONNECTION screen: what the files hold (never the secret, only
     * whether one is stored), and the last answer to a test or save */
    bool have_settings;
    char set_url[ZBX_URL_MAX];
    bool set_password;          /* auth=password, else a token */
    char set_user[ZBX_PROTO_USER_MAX];
    bool set_stored;
    char set_note[ZBX_TEXT_MAX];
    unsigned cresult_seq;       /* counts the answers; 0: none yet */
    bool cresult_save;
    enum zbx_cresult cresult;
    bool cresult_saved;
    char cresult_text[ZBX_TEXT_MAX];

    /* the helper itself */
    bool helper_running;
    unsigned helper_starts;
    enum zabbix_exit exit_reason;
    int exit_value;
    int64_t restart_at_ms;      /* 0: not waiting to restart */
    int restart_delay_ms;
};

void zabbix_model_init(struct zabbix_model *m);

/* Apply one message; returns a ZABBIX_CHANGED_* mask. rx holds the staging
 * sets a PROBLEMS, HOSTS or DETAIL message refers to. */
unsigned zabbix_model_apply(struct zabbix_model *m, const struct zbx_rx_msg *msg,
                            const struct zbx_rx *rx, int64_t now_ms);

/* The helper started, or stopped (reason, value). A stop schedules the next
 * start with a doubling delay; a start that stays up resets the delay once
 * data arrives. */
void zabbix_model_helper_started(struct zabbix_model *m, int64_t now_ms);
void zabbix_model_helper_stopped(struct zabbix_model *m, enum zabbix_exit reason, int value,
                                 int64_t now_ms);

/* The screen forgets the connection (Try the demo, Try again): everything
 * but the helper bookkeeping. */
void zabbix_model_forget(struct zabbix_model *m);

/* ---- the view ---------------------------------------------------------------------- */

/* A tone is a colour role the screen maps onto its styles, never a colour. */
enum zabbix_tone {
    ZABBIX_TONE_PLAIN = 0,      /* text_primary */
    ZABBIX_TONE_QUIET,          /* text_secondary */
    ZABBIX_TONE_OK,             /* status_ok */
    ZABBIX_TONE_WARN,           /* status_warn */
    ZABBIX_TONE_ERROR,          /* status_error */
};

/* Disaster and High are errors, Average and Warning warnings, Information
 * and Not classified quiet, no problem OK. The word always goes with it
 * (DS §2: colour never carries meaning alone). */
enum zabbix_tone zabbix_severity_tone(int severity);

/* Whether the problem list (or the hosts list) should be treated as stale
 * now. False when there is no data at all (nothing to call stale). */
bool zabbix_problems_stale(const struct zabbix_model *m, int64_t now_ms);
bool zabbix_hosts_stale(const struct zabbix_model *m, int64_t now_ms);

/* "18s ago", "3m ago", "2h ago"; "never" for 0. */
void zabbix_format_since(char *out, size_t len, int64_t now_ms, int64_t then_ms);

/* The server's clock now, from a set's reference clock and when it arrived;
 * 0 when unknown. */
int64_t zabbix_server_now(int64_t ref_clock, int64_t arrived_ms, int64_t now_ms);

#define ZABBIX_LINE_TEXT 160

/* The banner over every tab: shown whenever something is not right. */
struct zabbix_banner {
    bool show;
    enum zabbix_tone tone;
    char text[ZABBIX_LINE_TEXT];
};
void zabbix_view_banner(const struct zabbix_model *m, int64_t now_ms, struct zabbix_banner *b);

/* The caption at the right of the tab strip: "LIVE · 18s", "SIMULATED",
 * "UPDATING". */
void zabbix_view_caption(const struct zabbix_model *m, int64_t now_ms, char *out, size_t len);

/* OVERVIEW. */
struct zabbix_overview_view {
    bool setup;                 /* nothing configured: show the setup panel instead */
    bool loading;               /* no data yet */
    char headline[32];          /* "DISASTER", "ALL CLEAR", "--" */
    enum zabbix_tone headline_tone;
    char headline_note[ZABBIX_LINE_TEXT]; /* "highest open severity" */
    char problems[ZABBIX_LINE_TEXT];      /* "9 open · 6 unacknowledged" */
    char hosts[ZABBIX_LINE_TEXT];         /* "36 monitored · 2 down" */
    enum zabbix_tone hosts_tone;
    char hosts_note[ZABBIX_LINE_TEXT];    /* "1 unknown · 1 in maintenance" */
    int sev_count[ZBX_SEV_COUNT];
    bool sev_exact;
    char server[ZABBIX_LINE_TEXT];        /* "Production · Zabbix 7.0.31" */
    char updated[ZABBIX_LINE_TEXT];       /* "Updated 18s ago" */
};
void zabbix_view_overview(const struct zabbix_model *m, int64_t now_ms,
                          struct zabbix_overview_view *v);

/* One PROBLEMS row. */
struct zabbix_problem_row {
    char severity[16];          /* "HIGH" */
    enum zabbix_tone tone;
    char host[ZBX_HOST_NAME_MAX];
    char name[ZBX_PROBLEM_NAME_MAX];
    char age[24];               /* "3h 12m" */
    bool acknowledged;
    char meta[64];              /* "3h 12m · ACK" */
};
void zabbix_view_problem(const struct zbx_problem *p, int64_t server_now,
                         struct zabbix_problem_row *r);
/* The heading of the list: "9 open problems", "100 of 1 734 (most severe)". */
void zabbix_view_problems_title(const struct zabbix_model *m, char *out, size_t len);

/* One HOSTS row. */
struct zabbix_host_row {
    char name[ZBX_HOST_NAME_MAX];
    char avail[16];             /* "UP", "DOWN", "UNKNOWN" */
    enum zabbix_tone avail_tone;
    char problems[48];          /* "2 problems · HIGH", "no problems" */
    enum zabbix_tone problems_tone;
    bool maintenance;
};
void zabbix_view_host(const struct zbx_host *h, struct zabbix_host_row *r);
void zabbix_view_hosts_title(const struct zabbix_model *m, char *out, size_t len);

/* One latest value of the host detail. */
struct zabbix_item_row {
    char name[ZBX_ITEM_NAME_MAX];
    char value[ZBX_VALUE_MAX];
    char age[24];
};
void zabbix_view_item(const struct zbx_item *it, int64_t server_now, struct zabbix_item_row *r);

/* STATUS: key/value lines. */
#define ZABBIX_STATUS_LINES 14
struct zabbix_status_view {
    int count;
    char key[ZABBIX_STATUS_LINES][24];
    char value[ZABBIX_STATUS_LINES][ZABBIX_LINE_TEXT];
    enum zabbix_tone tone[ZABBIX_STATUS_LINES];
};
void zabbix_view_status(const struct zabbix_model *m, int64_t now_ms, struct zabbix_status_view *v);

/* Group digits for a count: 1734 -> "1 734". */
void zabbix_format_count(char *out, size_t len, int n);

/* ---- CONNECTION ---------------------------------------------------------------------- */

/* How a test or save came out, in the screen's words: "CONNECTED", "AUTH
 * FAILED", "UNREACHABLE", "INVALID CONFIG"; and the sentence under it, which
 * says for a save whether anything was stored. */
struct zabbix_cresult_view {
    char word[24];
    enum zabbix_tone tone;
    char text[ZABBIX_LINE_TEXT];
};
void zabbix_view_cresult(enum zbx_cresult r, bool save, bool saved, const char *text,
                         struct zabbix_cresult_view *v);

/* The secret field: its caption ("API TOKEN", "PASSWORD"), and the line
 * under it, which never holds the secret: whether one is stored and what an
 * empty field means, or how many characters are typed. */
void zabbix_view_secret_caption(bool password, char *out, size_t len);
void zabbix_view_secret_note(bool password, bool stored, size_t typed, char *out, size_t len);

#endif
