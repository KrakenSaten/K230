/*
 * RIFT's view of repeater control: the zero-hop scan, the one repeater
 * session, what the repeater answered, and the rule for which commands this
 * app will send. No LVGL, no socket and no file: the meshcored client
 * (rift_ipc_repeater.c) feeds it, the screens (ui/rift_scan.c,
 * ui/rift_repeater.c) read it, and tests/rift_model_test.c checks it.
 *
 * Everything here is what meshcored said (docs/api/mesh.md, "Repeater
 * control"). Two rules from the rest of the model hold: a value nobody
 * reported is absent, never 0; and nothing is called done before the
 * service says it is - a login is OK only when meshcored reports the
 * repeater's own OK.
 *
 * NO PASSWORD IS EVER HELD HERE. The login field's text goes from the field
 * into one request (rift_ipc_repeater_login) and is wiped there; this block
 * records only that a login was asked for.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_REPEATER_H
#define RIFT_REPEATER_H

#include <cjson/cJSON.h>

#include <stdint.h>

#define RIFT_REP_KEY 65
#define RIFT_REP_NAME 48
#define RIFT_REP_TEXT 96
/* meshcored keeps 16 discovered repeaters (MCD_DISCOVER_MAX). */
#define RIFT_REP_FOUND_MAX 16
/* What one GET_NEIGHBOURS answer can carry (MCD_REMOTE_NEIGHBOURS_MAX). */
#define RIFT_REP_NEIGHBOURS_MAX 11
/* The command transcript: bounded, newest last. A line is cut on a
 * character boundary. */
#define RIFT_REP_LINES 24
#define RIFT_REP_LINE 112
/* The longest command meshcored will send (one MeshCore text). */
#define RIFT_REP_CMD_MAX 160
/* The longest password upstream sends (sendLogin cuts at 15; meshcored
 * refuses longer rather than cutting). */
#define RIFT_REP_PASSWORD_MAX 15
/* Slack past meshcored's own deadline before this app stops waiting for an
 * answer event on its own: the service ends every wait itself, and this
 * covers only a service that went quiet without closing the socket. */
#define RIFT_REP_CLIENT_SLACK_MS 5000
/* And a request the service never even accepted (its answer lost or never
 * matched) is let go this long after it was written. */
#define RIFT_REP_ACCEPT_MS 15000

enum rift_rep_login {
    RIFT_REP_LOGIN_NONE = 0,
    RIFT_REP_LOGIN_WAITING,
    RIFT_REP_LOGIN_OK,
    RIFT_REP_LOGIN_REFUSED,
    RIFT_REP_LOGIN_TIMEOUT,
};

/* What is outstanding. One at a time, in this app as in meshcored. */
enum rift_rep_kind {
    RIFT_REP_NONE = 0,
    RIFT_REP_LOGIN,
    RIFT_REP_STATUS,
    RIFT_REP_NEIGHBOURS,
    RIFT_REP_OWNER,
    RIFT_REP_CLI,
};

/* A repeater that answered a scan. */
struct rift_rep_found {
    char key[RIFT_REP_KEY];
    char hash[3];
    char name[RIFT_REP_NAME]; /* empty when its advert was never heard */
    int known;                /* a contact meshcored holds: it can be logged in to */
    uint32_t round;
    int current;              /* answered the latest round */
    int64_t mono_ms;
    double their_snr_db;      /* how it heard us */
    int have_snr;             /* how we heard it */
    double snr_db;
    int have_rssi;
    double rssi_dbm;
};

struct rift_rep_scan {
    int unsupported;          /* this meshcored has no mesh.discover */
    int asking;               /* mesh.discover written, not answered yet */
    int have_round;
    uint32_t round;
    int open;
    int64_t until_ms;
    int64_t started_ms;
    int count;
    struct rift_rep_found found[RIFT_REP_FOUND_MAX];
    char error[RIFT_REP_TEXT];
};

struct rift_rep_status {
    int have;
    int64_t at_ms;
    double battery_mv, tx_queue, noise_floor_dbm, last_rssi_dbm, last_snr_db;
    double packets_recv, packets_sent, air_time_s, uptime_s;
    double sent_flood, sent_direct, recv_flood, recv_direct, err_events;
    int have_dups;
    double direct_dups, flood_dups;
    int have_rx_air;
    double rx_air_time_s, recv_errors;
};

struct rift_rep_neighbour {
    char prefix[13];
    char name[RIFT_REP_NAME];
    double heard_s_ago;
    double snr_db;
};

struct rift_rep_neighbours {
    int have;
    int64_t at_ms;
    int total;
    int count;
    struct rift_rep_neighbour e[RIFT_REP_NEIGHBOURS_MAX];
};

struct rift_rep_owner {
    int have;
    char firmware[RIFT_REP_TEXT];
    char name[RIFT_REP_NAME];
    char owner[RIFT_REP_TEXT];
};

struct rift_repeater {
    /* The scan, independent of any session. */
    struct rift_rep_scan scan;

    /* The repeater the control page is about (chosen from the scan list). */
    int have_target;
    char target[RIFT_REP_KEY];

    /* The session as meshcored last reported it. */
    int unsupported;          /* this meshcored has no mesh.remote_* */
    int active;
    char key[RIFT_REP_KEY];
    enum rift_rep_login login;
    int legacy;
    int admin_known;
    int admin;
    int fw_level;
    int have_clock;
    uint32_t repeater_clock;
    double stale_replies;
    double malformed_replies;

    /* The request in flight, from this app's side: written (asking), then
     * accepted with an id and a wait. Cleared by the event that ends it, by
     * the service going away, or past the wait plus slack. */
    enum rift_rep_kind asking;
    int64_t asked_ms;
    double request_id;        /* 0 until the service accepted it */
    int64_t deadline_ms;      /* by meshcored's clock, which is ours */

    /* The last word on the last request, for the page: what was asked and
     * how it ended - accepted, answered, timed out, refused - in words. */
    char note[RIFT_REP_TEXT];
    int note_is_error;

    struct rift_rep_status status;
    struct rift_rep_neighbours neighbours;
    struct rift_rep_owner owner;

    /* The command transcript: what was sent (secret-free, see
     * rift_rep_cli_class) and what came back. */
    char lines[RIFT_REP_LINES][RIFT_REP_LINE];
    int line_count;
    int line_head;            /* index of the oldest */
};

void rift_rep_init(struct rift_repeater *r);
/* meshcored went away, or a new connection began: the session it held is
 * gone with it (it is memory only) and nothing is outstanding. The scan list
 * is kept and marked not current; the target stays chosen. */
void rift_rep_service_lost(struct rift_repeater *r);
/* Forget the session view: logout, a new target. Keeps the scan. */
void rift_rep_session_clear(struct rift_repeater *r);
void rift_rep_set_target(struct rift_repeater *r, const char *key);

/* ---- what the client feeds in ------------------------------------------- */

/* A request was written (kind) at now; refuses (-1) while one is
 * outstanding. */
int rift_rep_begin(struct rift_repeater *r, enum rift_rep_kind kind, int64_t now_ms);
/* The service accepted it: its id and wait. */
void rift_rep_accepted(struct rift_repeater *r, const cJSON *result, int64_t now_ms);
/* The service refused it: why, in its words. */
void rift_rep_refused(struct rift_repeater *r, enum rift_rep_kind kind, const char *why);
/* An "unknown method": this meshcored predates repeater control. */
void rift_rep_mark_unsupported(struct rift_repeater *r, int scan);
/* mesh.discover's answer, and mesh.discovered's. */
void rift_rep_apply_discover(struct rift_repeater *r, const cJSON *result);
void rift_rep_apply_discovered(struct rift_repeater *r, const cJSON *result);
/* mesh.remote_session's answer, and the session in mesh.remote_logout's. */
void rift_rep_apply_session(struct rift_repeater *r, const cJSON *session);
/* An event: mesh.discover or mesh.remote. Returns 1 when it was one of
 * these (applied or refused as malformed), 0 for any other event. */
int rift_rep_apply_event(struct rift_repeater *r, const char *name, const cJSON *data,
                         int64_t now_ms);
/* Called on the poll: a request the service never ended is let go past its
 * wait plus slack. Returns 1 when one was. */
int rift_rep_expire(struct rift_repeater *r, int64_t now_ms);

/* Note what was sent, for the transcript. The command is classified first:
 * a refused one is never added (it is never sent). */
void rift_rep_note_command(struct rift_repeater *r, const char *command);

/* ---- reading ------------------------------------------------------------- */

int rift_rep_logged_in(const struct rift_repeater *r, const char *key);
int rift_rep_busy(const struct rift_repeater *r);
int rift_rep_scanning(const struct rift_repeater *r, int64_t now_ms);
const struct rift_rep_found *rift_rep_found_by_key(const struct rift_repeater *r,
                                                   const char *key);
/* The i'th transcript line, oldest first. */
const char *rift_rep_line(const struct rift_repeater *r, int i);

/* ---- the command rule ----------------------------------------------------
 *
 * Which of the repeater's CLI commands this app sends, decided here once
 * (upstream RIFT's riftCliIsDestructive and riftCliIsSecret, made stricter).
 *
 *   READ     read-only: ver, clock, board, neighbors, stats-*, get <key>,
 *            region, gps, powersaving, sensor list / get. Sent at once.
 *   CONFIRM  anything else upstream accepts that changes the repeater or
 *            makes it transmit - reboot, set ..., tempradio, clock sync,
 *            advert ... Sent only after a second, explicit press.
 *   REFUSED  never sent from RIFT: erase, start ota, poweroff / shutdown,
 *            log erase, and any line naming a secret (password, *.password,
 *            *.key, *.secret, prv.key, bridge.secret) - setting one would put
 *            it in an IPC frame, and reading one would put it on a screen.
 */
enum rift_cli_class {
    RIFT_CLI_READ = 0,
    RIFT_CLI_CONFIRM,
    RIFT_CLI_REFUSED,
};
enum rift_cli_class rift_rep_cli_class(const char *command, const char **why);
/* Whether a confirmable command changes the radio (frequency, power, the
 * whole radio): its confirmation says so. */
int rift_rep_cli_touches_radio(const char *command);

#endif
