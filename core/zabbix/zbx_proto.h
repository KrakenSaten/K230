/*
 * The link between the Zabbix app (apps/zabbix/zabbix_session.c) and its
 * helper, pos-zabbix (tools/zabbix/pos_zabbix.c).
 *
 * TRANSPORT. The session starts `pos-zabbix session ...` with a socketpair on
 * stdin and stdout. Commands go in one per line, events come out one per
 * line. Fields are separated by one TAB; every text field went through
 * zbx_copy_text() on the helper's side, so it holds no TAB, no newline and no
 * control character, and is cut to the model's size. A line longer than
 * ZBX_LINE_MAX is a protocol error.
 *
 * The app never sees JSON, a URL with credentials in it, or a token: the
 * helper turns the server's answers into the bounded model of zbx_model.h
 * and sends only that.
 *
 * COMMANDS (app to helper):
 *   refresh                      read everything again now (rate limited)
 *   detail <hostid>              also read this host's problems and values,
 *                                now and on every problem refresh
 *   detail -                     stop reading a host
 *   scenario <name>              the fake backend only: switch the scenario
 *   ctest <url> <auth> <user> <secret>
 *   csave <url> <auth> <user> <secret>
 *                                the CONNECTION screen: try these settings
 *                                (ctest), or try them and store them only
 *                                if they connect (csave); answered by
 *                                `cresult`. url, user and secret are hex
 *                                (any byte travels, none is a TAB or a
 *                                newline); secret "-" keeps the stored one.
 *                                The only line that may be longer than
 *                                ZBX_LINE_MAX: up to ZBX_CMD_LINE_MAX.
 *   quit                         leave; answered by `bye`
 *
 * EVENTS (helper to app):
 *   hello <proto> <live|fake> <scenario|->
 *   config <label> <url> <token|password|none> <verify 0|1> <refresh_s> <hosts_s>
 *                                what it is connecting to; url is scheme,
 *                                host and path only (never user:password@)
 *   state <state> <attempt> <retry_s> <error> <text>
 *                                see enum zbx_conn_state and enum zbx_err
 *   busy <what>                  a request is on its way (version, login,
 *                                problems, hosts, detail)
 *   idle                         nothing is on its way
 *   version <api version>
 *   pbegin <seq> <ref> <total> <exact> <sev_exact> <unack> <c0> .. <c5>
 *   pb <eventid> <objectid> <hostid> <sev> <ack> <supp> <clock> <host> <name>
 *   pend <seq> <count>           the problem set is complete
 *   hbegin <seq> <ref> <total> <exact> <fetched> <down> <unknown> <maint> <down_exact>
 *   ho <hostid> <avail> <maint> <problems> <maxsev> <name>
 *   hend <seq> <count>
 *   dbegin <seq> <ref> <hostid> <avail> <maint> <name>
 *   dp ... (as pb)
 *   di <itemid> <clock> <value> <units> <name>
 *   dend <seq> <problems> <items>
 *   dnone <hostid> <text>        the host could not be read (gone, no access)
 *   settings <url> <token|password> <user> <stored 0|1> <note>
 *                                the connection as zabbix.conf has it, for
 *                                the CONNECTION screen: the frontend url,
 *                                the sign-in, the user, and whether a token
 *                                or password is stored - never the secret
 *                                itself; note says why the files are not
 *                                usable, if they are not
 *   cresult <test|save> <connected|authfail|unreachable|invalid> <saved 0|1> <text>
 *                                the answer to ctest or csave
 *   bye
 *
 * A set is only handed to the app once its end line arrives with the same
 * sequence number and count as its begin: a helper that dies halfway through
 * a set leaves the app with the previous, complete one. Unknown lines are
 * ignored by both sides.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_ZBX_PROTO_H
#define POCKETOS_ZBX_PROTO_H

#include "zabbix/zbx_model.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define ZBX_PROTO_VERSION 1
#define ZBX_LINE_MAX 1024
/* A command line: ctest/csave carry three hex fields. */
#define ZBX_CMD_LINE_MAX 2048
/* The sizes of what ctest/csave carry, decoded (zbx_config.h has the same
 * ZBX_USER_MAX and ZBX_SECRET_MAX; zbx_config.c checks they agree). */
#define ZBX_PROTO_USER_MAX 64
#define ZBX_PROTO_SECRET_MAX 256

/* What trying a connection's settings came to (the CONNECTION screen). */
enum zbx_cresult {
    ZBX_CRESULT_CONNECTED = 0,  /* the server answered and took the credentials */
    ZBX_CRESULT_AUTH_FAILED,    /* the server answered and refused them */
    ZBX_CRESULT_UNREACHABLE,    /* no usable answer: DNS, network, TLS, HTTP, API */
    ZBX_CRESULT_INVALID,        /* the settings themselves are wrong; nothing was sent */
    ZBX_CRESULT_COUNT
};

/* The connection, as the helper sees it. */
enum zbx_conn_state {
    ZBX_CONN_UNCONFIGURED = 0,  /* no server set up: nothing will be tried */
    ZBX_CONN_CONNECTING,        /* first contact, or again after a failure */
    ZBX_CONN_ONLINE,            /* the last request worked */
    ZBX_CONN_RETRYING,          /* the last request failed; next try in retry_s */
    ZBX_CONN_AUTH_FAILED,       /* the server refused the credentials; only a
                                   `refresh` (or a long wait) tries again */
};

/* What went wrong, most recently. */
enum zbx_err {
    ZBX_ERR_NONE = 0,
    ZBX_ERR_CONFIG,             /* the configuration is missing or invalid */
    ZBX_ERR_DNS,                /* the server's name did not resolve */
    ZBX_ERR_CONNECT,            /* refused, unreachable, reset */
    ZBX_ERR_TIMEOUT,
    ZBX_ERR_TLS,                /* handshake or certificate */
    ZBX_ERR_HTTP,               /* an HTTP status other than 200 */
    ZBX_ERR_MALFORMED,          /* not JSON, or not the shape the API answers in */
    ZBX_ERR_TOO_LARGE,          /* the answer is over ZBX_RESPONSE_MAX */
    ZBX_ERR_API,                /* a JSON-RPC error other than authentication */
    ZBX_ERR_AUTH,               /* token or login refused, session ended */
    ZBX_ERR_UNSUPPORTED,        /* HTTPS asked for, but this build has no TLS */
    ZBX_ERR_INTERNAL,
    ZBX_ERR_COUNT
};

const char *zbx_conn_state_word(enum zbx_conn_state s);
const char *zbx_err_word(enum zbx_err e);
/* A short sentence for a screen: "Server not reachable", ... */
const char *zbx_err_text(enum zbx_err e);
int zbx_conn_state_parse(const char *w);
int zbx_err_parse(const char *w);
const char *zbx_cresult_word(enum zbx_cresult r);
int zbx_cresult_parse(const char *w);

/* Hex for the ctest/csave fields: lower-case pairs. encode returns 0, or -1
 * when it does not fit; decode returns the decoded length, or -1 for odd
 * length, a non-hex digit, a NUL byte or no room (dst is always ended). */
int zbx_hex_encode(char *dst, size_t len, const char *src);
int zbx_hex_decode(char *dst, size_t len, const char *src);

/* ---- the helper's side: writing ------------------------------------------------- */

/* Each writes one complete line to out and returns 0, or -1 when the line
 * could not be written. Text fields are cleaned with zbx_copy_text(). */
int zbx_proto_hello(FILE *out, bool fake, const char *scenario);
int zbx_proto_config(FILE *out, const char *label, const char *url, const char *auth,
                     bool verify, int refresh_s, int hosts_s);
int zbx_proto_state(FILE *out, enum zbx_conn_state st, int attempt, int retry_s, enum zbx_err err,
                    const char *text);
int zbx_proto_busy(FILE *out, const char *what);
int zbx_proto_idle(FILE *out);
int zbx_proto_version(FILE *out, const char *version);
int zbx_proto_problems(FILE *out, unsigned seq, const struct zbx_problem_set *s);
int zbx_proto_hosts(FILE *out, unsigned seq, const struct zbx_host_set *s);
int zbx_proto_detail(FILE *out, unsigned seq, const struct zbx_detail *d);
int zbx_proto_detail_none(FILE *out, const char *hostid, const char *text);
int zbx_proto_settings(FILE *out, const char *url, const char *auth, const char *user,
                       bool stored, const char *note);
int zbx_proto_cresult(FILE *out, bool save, enum zbx_cresult r, bool saved, const char *text);
int zbx_proto_bye(FILE *out);

/* The app's side of ctest/csave: the command line (newline included) into
 * buf, which then holds the secret in hex: the caller wipes it. secret NULL
 * or "" keeps the stored one. 0, or -1 when a field is too long. */
int zbx_proto_cmd_settings(char *buf, size_t len, bool save, const char *url, const char *auth,
                           const char *user, const char *secret);

/* ---- the app's side: reading ----------------------------------------------------- */

enum zbx_rx_kind {
    ZBX_RX_NONE = 0,            /* a line inside a set, or an ignored line */
    ZBX_RX_HELLO,
    ZBX_RX_CONFIG,
    ZBX_RX_STATE,
    ZBX_RX_BUSY,
    ZBX_RX_IDLE,
    ZBX_RX_VERSION,
    ZBX_RX_PROBLEMS,            /* rx->problems holds a complete new set */
    ZBX_RX_HOSTS,               /* rx->hosts */
    ZBX_RX_DETAIL,              /* rx->detail */
    ZBX_RX_DETAIL_NONE,
    ZBX_RX_SETTINGS,            /* url, word (auth), user, flag (stored), text (note) */
    ZBX_RX_CRESULT,             /* save, result, flag (saved), text */
    ZBX_RX_BYE,
    ZBX_RX_BAD,                 /* a known word with broken fields */
};

struct zbx_rx_msg {
    enum zbx_rx_kind kind;
    bool fake;
    int proto;
    enum zbx_conn_state state;
    enum zbx_err err;
    int attempt;
    int retry_s;
    bool verify;
    int refresh_s;
    int hosts_s;
    char word[ZBX_TEXT_MAX];    /* scenario, auth, busy's what, version, hostid */
    char label[ZBX_TEXT_MAX];
    char url[ZBX_URL_MAX];
    char text[ZBX_TEXT_MAX];
    char user[ZBX_PROTO_USER_MAX];
    bool flag;
    bool save;
    enum zbx_cresult result;
};

/* The reader's staging area. Large (the three sets), so the app keeps it in
 * its own heap block rather than on a stack. */
struct zbx_rx {
    struct zbx_problem_set problems;
    struct zbx_host_set hosts;
    struct zbx_detail detail;
    unsigned p_seq;
    unsigned h_seq;
    unsigned d_seq;
    bool in_problems;
    bool in_hosts;
    bool in_detail;
    unsigned bad;               /* lines refused, for tests and the log */
};

void zbx_rx_init(struct zbx_rx *rx);
/* Take one line (no newline; modified in place). Returns msg->kind. */
enum zbx_rx_kind zbx_rx_line(struct zbx_rx *rx, char *line, struct zbx_rx_msg *msg);

/* ---- commands --------------------------------------------------------------------- */

enum zbx_cmd_kind {
    ZBX_CMD_NONE = 0,
    ZBX_CMD_REFRESH,
    ZBX_CMD_DETAIL,             /* word = hostid, "" to stop */
    ZBX_CMD_SCENARIO,           /* word = name */
    ZBX_CMD_QUIT,
    ZBX_CMD_TEST,               /* word = auth; url, user, secret / keep_secret */
    ZBX_CMD_SAVE,               /* the same */
};

struct zbx_cmd {
    enum zbx_cmd_kind kind;
    char word[ZBX_TEXT_MAX];
    char url[ZBX_URL_MAX];
    char user[ZBX_PROTO_USER_MAX];
    char secret[ZBX_PROTO_SECRET_MAX]; /* TEST/SAVE: the new token or password; wipe it */
    bool keep_secret;           /* TEST/SAVE: use the stored one */
};

/* Parse one command line (no newline; modified in place). A TEST or SAVE
 * whose fields do not decode is ZBX_CMD_NONE. */
enum zbx_cmd_kind zbx_cmd_parse(char *line, struct zbx_cmd *cmd);

#endif
