/*
 * RIFT's view of repeater control. See rift_repeater.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_repeater.h"

#include "rift_format.h"
#include "rift_json.h"

#include <stdio.h>
#include <string.h>

void rift_rep_init(struct rift_repeater *r)
{
    if (r) {
        memset(r, 0, sizeof(*r));
    }
}

static void clear_answers(struct rift_repeater *r)
{
    memset(&r->status, 0, sizeof(r->status));
    memset(&r->neighbours, 0, sizeof(r->neighbours));
    memset(&r->owner, 0, sizeof(r->owner));
    /* Zeroed, not only counted down: a transcript is somebody else's
     * repeater's answers, and it goes with the session. */
    memset(r->lines, 0, sizeof(r->lines));
    r->line_count = 0;
    r->line_head = 0;
}

void rift_rep_session_clear(struct rift_repeater *r)
{
    if (!r) {
        return;
    }
    r->active = 0;
    r->key[0] = '\0';
    r->login = RIFT_REP_LOGIN_NONE;
    r->legacy = 0;
    r->admin_known = 0;
    r->admin = 0;
    r->fw_level = 0;
    r->have_clock = 0;
    r->repeater_clock = 0;
    r->stale_replies = 0;
    r->malformed_replies = 0;
    r->asking = RIFT_REP_NONE;
    r->request_id = 0;
    r->deadline_ms = 0;
    clear_answers(r);
}

void rift_rep_service_lost(struct rift_repeater *r)
{
    int i;

    if (!r) {
        return;
    }
    /* meshcored keeps a session in memory only, so a service that went away
     * took it with it - whatever the page last showed is no longer true. */
    rift_rep_session_clear(r);
    r->note[0] = '\0';
    /* The next service may know what this one did not (deploy.sh while RIFT
     * ran in the background): asked again on connecting. */
    r->unsupported = 0;
    r->scan.unsupported = 0;
    r->scan.asking = 0;
    r->scan.open = 0;
    for (i = 0; i < r->scan.count; i++) {
        r->scan.found[i].current = 0;
    }
}

void rift_rep_set_target(struct rift_repeater *r, const char *key)
{
    if (!r || !key || !hex_only(key, 64)) {
        return;
    }
    if (r->have_target && strcmp(r->target, key) == 0) {
        return;
    }
    snprintf(r->target, sizeof(r->target), "%s", key);
    r->have_target = 1;
    r->note[0] = '\0';
    r->note_is_error = 0;
}

static void note(struct rift_repeater *r, int error, const char *text)
{
    rift_utf8_copy(r->note, sizeof(r->note), text ? text : "");
    r->note_is_error = error;
}

static const char *kind_word(enum rift_rep_kind k)
{
    switch (k) {
    case RIFT_REP_LOGIN:
        return "LOGIN";
    case RIFT_REP_STATUS:
        return "STATUS";
    case RIFT_REP_NEIGHBOURS:
        return "NEIGHBOURS";
    case RIFT_REP_OWNER:
        return "VERSION";
    case RIFT_REP_CLI:
        return "COMMAND";
    case RIFT_REP_NONE:
    default:
        return "REQUEST";
    }
}

static enum rift_rep_kind kind_of(const char *s)
{
    if (!s) {
        return RIFT_REP_NONE;
    }
    if (strcmp(s, "login") == 0) {
        return RIFT_REP_LOGIN;
    }
    if (strcmp(s, "status") == 0) {
        return RIFT_REP_STATUS;
    }
    if (strcmp(s, "neighbours") == 0) {
        return RIFT_REP_NEIGHBOURS;
    }
    if (strcmp(s, "owner") == 0) {
        return RIFT_REP_OWNER;
    }
    if (strcmp(s, "cli") == 0) {
        return RIFT_REP_CLI;
    }
    return RIFT_REP_NONE;
}

int rift_rep_begin(struct rift_repeater *r, enum rift_rep_kind kind, int64_t now_ms)
{
    char text[RIFT_REP_TEXT];

    if (!r || kind == RIFT_REP_NONE || r->asking != RIFT_REP_NONE) {
        return -1;
    }
    r->asking = kind;
    r->asked_ms = now_ms;
    r->request_id = 0;
    r->deadline_ms = 0;
    snprintf(text, sizeof(text), "%s ASKED", kind_word(kind));
    note(r, 0, text);
    if (kind == RIFT_REP_LOGIN) {
        r->login = RIFT_REP_LOGIN_WAITING;
    }
    return 0;
}

void rift_rep_accepted(struct rift_repeater *r, const cJSON *result, int64_t now_ms)
{
    double id = 0;
    double wait = 0;
    const char *route;
    char text[RIFT_REP_TEXT];

    if (!r || r->asking == RIFT_REP_NONE) {
        return;
    }
    if (!num_of(result, "request_id", &id) || !num_of(result, "wait_ms", &wait) || id <= 0) {
        /* An answer that names no request cannot be matched to the event
         * that ends it; the request is let go rather than waited on for
         * ever. */
        r->asking = RIFT_REP_NONE;
        note(r, 1, "meshcored's answer could not be read; nothing is being waited for");
        return;
    }
    r->request_id = id;
    r->deadline_ms = now_ms + (int64_t)wait;
    route = str_of(result, "route");
    /* Accepted is not answered: it is queued for the air, by flood or on the
     * route this node learned. */
    snprintf(text, sizeof(text), "%s SENT %s" RIFT_SEP "WAITING UP TO %d S", kind_word(r->asking),
             (route && strcmp(route, "direct") == 0) ? "DIRECT" : "BY FLOOD",
             (int)((wait + 999) / 1000));
    note(r, 0, text);
}

void rift_rep_refused(struct rift_repeater *r, enum rift_rep_kind kind, const char *why)
{
    char text[RIFT_REP_TEXT];

    if (!r) {
        return;
    }
    if (r->asking == kind) {
        r->asking = RIFT_REP_NONE;
    }
    if (kind == RIFT_REP_LOGIN && r->login == RIFT_REP_LOGIN_WAITING) {
        r->login = RIFT_REP_LOGIN_NONE;
    }
    snprintf(text, sizeof(text), "%s NOT SENT: %s", kind_word(kind), why ? why : "refused");
    note(r, 1, text);
}

void rift_rep_mark_unsupported(struct rift_repeater *r, int scan)
{
    if (!r) {
        return;
    }
    if (scan) {
        r->scan.unsupported = 1;
        r->scan.asking = 0;
    } else {
        r->unsupported = 1;
        r->asking = RIFT_REP_NONE;
        if (r->login == RIFT_REP_LOGIN_WAITING) {
            r->login = RIFT_REP_LOGIN_NONE;
        }
    }
}

/* ---- the scan ------------------------------------------------------------ */

static void apply_round(struct rift_rep_scan *s, const cJSON *o)
{
    double v;

    if (num_of(o, "round", &v) && v >= 0) {
        s->round = (uint32_t)v;
        s->have_round = v > 0;
    }
    s->open = bool_of(o, "open", 0);
    if (num_of(o, "until_mono_ms", &v)) {
        s->until_ms = (int64_t)v;
    }
    if (num_of(o, "started_mono_ms", &v)) {
        s->started_ms = (int64_t)v;
    }
}

/* One repeater from the service. Returns 0, or -1 when it names no key. */
static int found_of(const cJSON *o, struct rift_rep_found *f)
{
    const char *key = str_of(o, "public_key");
    const char *hash = str_of(o, "node_hash");
    const char *name = str_of(o, "name");
    double v;

    if (!hex_only(key, 64)) {
        return -1;
    }
    memset(f, 0, sizeof(*f));
    snprintf(f->key, sizeof(f->key), "%s", key);
    if (hex_only(hash, 2)) {
        snprintf(f->hash, sizeof(f->hash), "%s", hash);
    } else {
        memcpy(f->hash, key, 2);
        f->hash[2] = '\0';
    }
    if (name) {
        rift_utf8_copy(f->name, sizeof(f->name), name);
    }
    f->known = bool_of(o, "known", 0);
    if (num_of(o, "round", &v) && v >= 0) {
        f->round = (uint32_t)v;
    }
    f->current = bool_of(o, "current", 0);
    if (num_of(o, "mono_ms", &v)) {
        f->mono_ms = (int64_t)v;
    }
    (void)num_of(o, "their_snr_db", &f->their_snr_db);
    f->have_snr = num_of(o, "snr_db", &f->snr_db);
    f->have_rssi = num_of(o, "rssi_dbm", &f->rssi_dbm);
    return 0;
}

static void upsert(struct rift_rep_scan *s, const struct rift_rep_found *f)
{
    int i;
    int slot = -1;

    for (i = 0; i < s->count; i++) {
        if (strcmp(s->found[i].key, f->key) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (s->count < RIFT_REP_FOUND_MAX) {
            slot = s->count++;
        } else {
            /* The oldest answer gives way, as meshcored's own table does. */
            slot = 0;
            for (i = 1; i < s->count; i++) {
                if (s->found[i].mono_ms < s->found[slot].mono_ms) {
                    slot = i;
                }
            }
        }
    }
    s->found[slot] = *f;
}

void rift_rep_apply_discover(struct rift_repeater *r, const cJSON *result)
{
    uint32_t before;
    int i;

    if (!r || !cJSON_IsObject(result)) {
        return;
    }
    r->scan.asking = 0;
    r->scan.error[0] = '\0';
    before = r->scan.round;
    apply_round(&r->scan, result);
    /* A new round: every repeater listed so far answered an earlier one. */
    if (r->scan.round != before) {
        for (i = 0; i < r->scan.count; i++) {
            r->scan.found[i].current = 0;
        }
    }
}

void rift_rep_apply_discovered(struct rift_repeater *r, const cJSON *result)
{
    const cJSON *arr;
    const cJSON *e;
    struct rift_rep_found f;

    if (!r || !cJSON_IsObject(result)) {
        return;
    }
    arr = cJSON_GetObjectItemCaseSensitive(result, "repeaters");
    if (!cJSON_IsArray(arr)) {
        return;
    }
    apply_round(&r->scan, result);
    r->scan.count = 0;
    cJSON_ArrayForEach(e, arr)
    {
        if (found_of(e, &f) == 0) {
            upsert(&r->scan, &f);
        }
    }
}

/* ---- the session --------------------------------------------------------- */

static enum rift_rep_login login_of(const char *s)
{
    if (!s) {
        return RIFT_REP_LOGIN_NONE;
    }
    if (strcmp(s, "waiting") == 0) {
        return RIFT_REP_LOGIN_WAITING;
    }
    if (strcmp(s, "ok") == 0) {
        return RIFT_REP_LOGIN_OK;
    }
    if (strcmp(s, "refused") == 0) {
        return RIFT_REP_LOGIN_REFUSED;
    }
    if (strcmp(s, "timeout") == 0) {
        return RIFT_REP_LOGIN_TIMEOUT;
    }
    return RIFT_REP_LOGIN_NONE;
}

void rift_rep_apply_session(struct rift_repeater *r, const cJSON *s)
{
    const char *key;
    double v;

    if (!r || !cJSON_IsObject(s)) {
        return;
    }
    if (!bool_of(s, "active", 0)) {
        enum rift_rep_kind asking = r->asking;
        int64_t asked = r->asked_ms;

        rift_rep_session_clear(r);
        /* A login just written may be answered by a session report from
         * before it reached the service; the request itself stays. */
        if (asking == RIFT_REP_LOGIN) {
            r->asking = asking;
            r->asked_ms = asked;
            r->login = RIFT_REP_LOGIN_WAITING;
        }
        return;
    }
    key = str_of(s, "node");
    if (!hex_only(key, 64)) {
        return;
    }
    /* Another repeater's session: nothing shown for the old one is about
     * this one. */
    if (!r->active || strcmp(r->key, key) != 0) {
        clear_answers(r);
    }
    r->active = 1;
    snprintf(r->key, sizeof(r->key), "%s", key);
    r->login = login_of(str_of(s, "login"));
    r->legacy = bool_of(s, "legacy", 0);
    r->admin_known = cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(s, "admin"));
    r->admin = bool_of(s, "admin", 0);
    r->fw_level = num_of(s, "firmware_level", &v) ? (int)v : 0;
    r->have_clock = num_of(s, "repeater_clock", &v) && v > 0;
    r->repeater_clock = r->have_clock ? (uint32_t)v : 0;
    (void)num_of(s, "stale_replies", &r->stale_replies);
    (void)num_of(s, "malformed_replies", &r->malformed_replies);
}

/* ---- the transcript ------------------------------------------------------ */

static void push_line(struct rift_repeater *r, const char *prefix, const char *text, size_t n)
{
    char line[RIFT_REP_LINE];
    char body[RIFT_REP_LINE];
    int at;

    if (n >= sizeof(body)) {
        n = sizeof(body) - 1;
    }
    memcpy(body, text, n);
    body[n] = '\0';
    snprintf(line, sizeof(line), "%s%s", prefix, body);
    if (r->line_count < RIFT_REP_LINES) {
        at = (r->line_head + r->line_count) % RIFT_REP_LINES;
        r->line_count++;
    } else {
        at = r->line_head;
        r->line_head = (r->line_head + 1) % RIFT_REP_LINES;
    }
    /* Cut on a character boundary: a node name is where a two-byte Nordic
     * letter lives, and a split one is a broken line. */
    rift_utf8_copy(r->lines[at], sizeof(r->lines[at]), line);
}

static void push_reply(struct rift_repeater *r, const char *text)
{
    const char *p = text;
    int any = 0;

    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);

        if (n > 0) {
            push_line(r, "  ", p, n);
            any = 1;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    if (!any) {
        push_line(r, "  ", "(empty answer)", 14);
    }
}

void rift_rep_note_command(struct rift_repeater *r, const char *command)
{
    if (!r || !command || rift_rep_cli_class(command, NULL) == RIFT_CLI_REFUSED) {
        return;
    }
    push_line(r, "> ", command, strlen(command));
}

const char *rift_rep_line(const struct rift_repeater *r, int i)
{
    if (!r || i < 0 || i >= r->line_count) {
        return NULL;
    }
    return r->lines[(r->line_head + i) % RIFT_REP_LINES];
}

/* ---- events -------------------------------------------------------------- */

static void apply_status(struct rift_rep_status *st, const cJSON *o, int64_t now_ms)
{
    memset(st, 0, sizeof(*st));
    st->have = 1;
    st->at_ms = now_ms;
    (void)num_of(o, "battery_mv", &st->battery_mv);
    (void)num_of(o, "tx_queue", &st->tx_queue);
    (void)num_of(o, "noise_floor_dbm", &st->noise_floor_dbm);
    (void)num_of(o, "last_rssi_dbm", &st->last_rssi_dbm);
    (void)num_of(o, "last_snr_db", &st->last_snr_db);
    (void)num_of(o, "packets_recv", &st->packets_recv);
    (void)num_of(o, "packets_sent", &st->packets_sent);
    (void)num_of(o, "air_time_s", &st->air_time_s);
    (void)num_of(o, "uptime_s", &st->uptime_s);
    (void)num_of(o, "sent_flood", &st->sent_flood);
    (void)num_of(o, "sent_direct", &st->sent_direct);
    (void)num_of(o, "recv_flood", &st->recv_flood);
    (void)num_of(o, "recv_direct", &st->recv_direct);
    (void)num_of(o, "err_events", &st->err_events);
    st->have_dups = num_of(o, "direct_dups", &st->direct_dups) &&
                    num_of(o, "flood_dups", &st->flood_dups);
    st->have_rx_air = num_of(o, "rx_air_time_s", &st->rx_air_time_s) &&
                      num_of(o, "recv_errors", &st->recv_errors);
}

static void apply_neighbours(struct rift_rep_neighbours *nb, const cJSON *o, int64_t now_ms)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, "entries");
    const cJSON *e;
    double v;

    memset(nb, 0, sizeof(*nb));
    nb->have = 1;
    nb->at_ms = now_ms;
    if (num_of(o, "total", &v) && v >= 0) {
        nb->total = (int)v;
    }
    cJSON_ArrayForEach(e, arr)
    {
        struct rift_rep_neighbour *n;
        const char *prefix = str_of(e, "prefix");
        const char *name = str_of(e, "name");

        if (nb->count >= RIFT_REP_NEIGHBOURS_MAX || !hex_only(prefix, 0) ||
            strlen(prefix) >= sizeof(n->prefix)) {
            continue;
        }
        n = &nb->e[nb->count++];
        snprintf(n->prefix, sizeof(n->prefix), "%s", prefix);
        if (name) {
            rift_utf8_copy(n->name, sizeof(n->name), name);
        }
        (void)num_of(e, "heard_s_ago", &n->heard_s_ago);
        (void)num_of(e, "snr_db", &n->snr_db);
    }
}

static void apply_owner(struct rift_rep_owner *ow, const cJSON *o)
{
    const char *s;

    memset(ow, 0, sizeof(*ow));
    ow->have = 1;
    if ((s = str_of(o, "firmware")) != NULL) {
        rift_utf8_copy(ow->firmware, sizeof(ow->firmware), s);
    }
    if ((s = str_of(o, "name")) != NULL) {
        rift_utf8_copy(ow->name, sizeof(ow->name), s);
    }
    if ((s = str_of(o, "owner")) != NULL) {
        rift_utf8_copy(ow->owner, sizeof(ow->owner), s);
    }
}

static void apply_reply(struct rift_repeater *r, const cJSON *reply, int64_t now_ms)
{
    enum rift_rep_kind kind = kind_of(str_of(reply, "kind"));
    const char *outcome = str_of(reply, "outcome");
    const char *node = str_of(reply, "node");
    double id = 0;
    char text[RIFT_REP_TEXT];
    int ours;

    (void)num_of(reply, "request_id", &id);
    /* The request this app is waiting on ends here, whoever's it was: the
     * service has one at a time, and the event says it is over. */
    ours = r->asking != RIFT_REP_NONE &&
           (id == r->request_id || (r->request_id == 0 && kind == r->asking));
    if (ours) {
        r->asking = RIFT_REP_NONE;
        r->request_id = 0;
    }
    if (!outcome || kind == RIFT_REP_NONE) {
        return;
    }
    /* An answer is always the repeater's word and is shown: meshcored has
     * already matched it to its request and dropped stale ones. But a
     * timeout or a cancellation of something this app is not waiting for -
     * a request it let go, one it ended itself - is not drawn over the
     * caption of what it is waiting for; the session, already applied, is
     * all it changes. */
    if (!ours && strcmp(outcome, "replied") != 0) {
        return;
    }
    /* And while this app waits for its own request, an answer to any other
     * one - a late one above all - is not drawn and does not end the wait:
     * it would put an old CLOCK's time under a new command. */
    if (!ours && r->asking != RIFT_REP_NONE) {
        return;
    }
    /* An answer about another repeater than the session's is not drawn as
     * this one's. The session object that rides with it says which. */
    if (node && r->active && strcmp(node, r->key) != 0) {
        return;
    }
    if (strcmp(outcome, "replied") == 0) {
        switch (kind) {
        case RIFT_REP_LOGIN:
            snprintf(text, sizeof(text), "LOGGED IN%s",
                     bool_of(reply, "late", 0) ? RIFT_SEP "THE ANSWER CAME LATE" : "");
            note(r, 0, text);
            break;
        case RIFT_REP_STATUS:
            apply_status(&r->status, cJSON_GetObjectItemCaseSensitive(reply, "status"), now_ms);
            note(r, 0, "STATUS ANSWERED");
            break;
        case RIFT_REP_NEIGHBOURS:
            apply_neighbours(&r->neighbours, cJSON_GetObjectItemCaseSensitive(reply, "neighbours"),
                             now_ms);
            note(r, 0, "NEIGHBOURS ANSWERED");
            break;
        case RIFT_REP_OWNER:
            apply_owner(&r->owner, cJSON_GetObjectItemCaseSensitive(reply, "owner"));
            note(r, 0, "VERSION ANSWERED");
            break;
        case RIFT_REP_CLI:
            push_reply(r, str_of(reply, "text"));
            note(r, 0, "COMMAND ANSWERED");
            break;
        default:
            break;
        }
    } else if (strcmp(outcome, "timeout") == 0) {
        if (kind == RIFT_REP_LOGIN) {
            /* Upstream's repeater answers a wrong password with nothing at
             * all, so this is the only way one ever shows. */
            note(r, 1, "NO ANSWER: wrong password, or the repeater did not hear this node");
        } else {
            snprintf(text, sizeof(text), "%s: NO ANSWER IN TIME", kind_word(kind));
            note(r, 1, text);
            if (kind == RIFT_REP_CLI) {
                push_line(r, "  ", "(no answer)", 11);
            }
        }
    } else if (strcmp(outcome, "refused") == 0) {
        note(r, 1, kind == RIFT_REP_LOGIN ? "LOGIN REFUSED by the repeater"
                                          : "REFUSED by the repeater");
    } else {
        snprintf(text, sizeof(text), "%s CANCELLED", kind_word(kind));
        note(r, 0, text);
    }
}

int rift_rep_apply_event(struct rift_repeater *r, const char *name, const cJSON *data,
                         int64_t now_ms)
{
    if (!r || !name) {
        return 0;
    }
    if (strcmp(name, "mesh.discover") == 0) {
        const char *reason = str_of(data, "reason");
        struct rift_rep_found f;

        if (!cJSON_IsObject(data) || !reason) {
            return 1;
        }
        {
            uint32_t before = r->scan.round;
            int i;

            apply_round(&r->scan, data);
            /* A round another client started: what answered before it is
             * not current any more. */
            if (r->scan.round != before) {
                for (i = 0; i < r->scan.count; i++) {
                    r->scan.found[i].current = 0;
                }
            }
        }
        if (strcmp(reason, "reply") == 0 &&
            found_of(cJSON_GetObjectItemCaseSensitive(data, "repeater"), &f) == 0) {
            upsert(&r->scan, &f);
        }
        return 1;
    }
    if (strcmp(name, "mesh.remote") == 0) {
        const cJSON *reply = cJSON_GetObjectItemCaseSensitive(data, "reply");
        const cJSON *session = cJSON_GetObjectItemCaseSensitive(data, "session");

        if (!cJSON_IsObject(reply) || !cJSON_IsObject(session)) {
            return 1;
        }
        /* The session first, so an answer is filed under the session it
         * belongs to and a new repeater's session clears the old answers
         * before the new one is written. */
        rift_rep_apply_session(r, session);
        apply_reply(r, reply, now_ms);
        return 1;
    }
    return 0;
}

int rift_rep_expire(struct rift_repeater *r, int64_t now_ms)
{
    if (!r || r->asking == RIFT_REP_NONE) {
        return 0;
    }
    if ((r->request_id > 0 && now_ms > r->deadline_ms + RIFT_REP_CLIENT_SLACK_MS) ||
        (r->request_id <= 0 && now_ms > r->asked_ms + RIFT_REP_ACCEPT_MS)) {
        r->asking = RIFT_REP_NONE;
        r->request_id = 0;
        note(r, 1, "meshcored never said how that request ended");
        return 1;
    }
    return 0;
}

int rift_rep_logged_in(const struct rift_repeater *r, const char *key)
{
    return r && key && r->active && r->login == RIFT_REP_LOGIN_OK && strcmp(r->key, key) == 0;
}

int rift_rep_busy(const struct rift_repeater *r)
{
    return r && r->asking != RIFT_REP_NONE;
}

int rift_rep_scanning(const struct rift_repeater *r, int64_t now_ms)
{
    return r && (r->scan.asking || (r->scan.open && now_ms < r->scan.until_ms));
}

const struct rift_rep_found *rift_rep_found_by_key(const struct rift_repeater *r, const char *key)
{
    int i;

    if (!r || !key) {
        return NULL;
    }
    for (i = 0; i < r->scan.count; i++) {
        if (strcmp(r->scan.found[i].key, key) == 0) {
            return &r->scan.found[i];
        }
    }
    return NULL;
}

/* ---- the command rule ---------------------------------------------------- */

static int token_is_secret(const char *tok, size_t len)
{
    static const char *const named[] = { "password", "guest.password", "prv.key",
                                         "bridge.secret" };
    static const char *const suffixes[] = { ".password", ".secret", ".key" };
    size_t i;

    for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (strlen(named[i]) == len && memcmp(tok, named[i], len) == 0) {
            return 1;
        }
    }
    for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        size_t n = strlen(suffixes[i]);

        if (len > n && memcmp(tok + len - n, suffixes[i], n) == 0) {
            return 1;
        }
    }
    return 0;
}

static int starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

enum rift_cli_class rift_rep_cli_class(const char *command, const char **why)
{
    static const char *const read_exact[] = { "ver", "clock", "board", "neighbors",
                                              "stats-core", "stats-radio", "stats-packets",
                                              "region", "gps", "powersaving", "sensor list",
                                              "get" };
    static const char *const read_prefix[] = { "get ", "sensor get " };
    static const char *const refused[] = { "erase", "start ota", "poweroff", "shutdown",
                                           "log erase" };
    const char *p;
    size_t i;

    if (why) {
        *why = NULL;
    }
    if (!command) {
        return RIFT_CLI_REFUSED;
    }
    while (*command == ' ') {
        command++;
    }
    /* A secret named anywhere on the line: setting one would put it in an
     * IPC frame and a transcript, and reading one would put it on a screen
     * somebody else can see. */
    for (p = command; *p;) {
        const char *start;

        while (*p == ' ') {
            p++;
        }
        start = p;
        while (*p && *p != ' ') {
            p++;
        }
        if (p > start && token_is_secret(start, (size_t)(p - start))) {
            if (why) {
                *why = "names a password or key; not sent from RIFT";
            }
            return RIFT_CLI_REFUSED;
        }
    }
    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        if (starts(command, refused[i])) {
            if (why) {
                *why = "destructive; not sent from RIFT";
            }
            return RIFT_CLI_REFUSED;
        }
    }
    for (i = 0; i < sizeof(read_exact) / sizeof(read_exact[0]); i++) {
        size_t n = strlen(read_exact[i]);

        if (strncmp(command, read_exact[i], n) == 0) {
            const char *rest = command + n;

            while (*rest == ' ') {
                rest++;
            }
            if (*rest == '\0') {
                return RIFT_CLI_READ;
            }
        }
    }
    for (i = 0; i < sizeof(read_prefix) / sizeof(read_prefix[0]); i++) {
        if (starts(command, read_prefix[i])) {
            return RIFT_CLI_READ;
        }
    }
    if (why) {
        *why = rift_rep_cli_touches_radio(command)
                   ? "changes the repeater's radio; it may stop hearing this node"
                   : "changes the repeater or makes it transmit";
    }
    return RIFT_CLI_CONFIRM;
}

int rift_rep_cli_touches_radio(const char *command)
{
    static const char *const radio[] = { "set radio", "set freq", "set tx", "set bw", "set sf",
                                         "set cr", "tempradio" };
    size_t i;

    if (!command) {
        return 0;
    }
    while (*command == ' ') {
        command++;
    }
    for (i = 0; i < sizeof(radio) / sizeof(radio[0]); i++) {
        if (starts(command, radio[i])) {
            return 1;
        }
    }
    return 0;
}
