/*
 * The app/helper line protocol. See zbx_proto.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE /* explicit_bzero */
#include "zabbix/zbx_proto.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define FIELDS_MAX 16

/* ---- words --------------------------------------------------------------------- */

static const char *const state_words[] = { "unconfigured", "connecting", "online", "retrying",
                                           "authfail" };
static const char *const err_words[ZBX_ERR_COUNT] = {
    "none", "config", "dns", "connect", "timeout", "tls", "http", "malformed",
    "toolarge", "api", "auth", "unsupported", "internal",
};
static const char *const err_texts[ZBX_ERR_COUNT] = {
    "",
    "Not configured",
    "Server name not found",
    "Server not reachable",
    "Server did not answer in time",
    "Secure connection failed",
    "Server answered with an HTTP error",
    "Server sent an answer this app cannot read",
    "Server answer too large",
    "Zabbix API error",
    "Access refused by the server",
    "HTTPS is not available in this build",
    "Internal error",
};

const char *zbx_conn_state_word(enum zbx_conn_state s)
{
    return (unsigned)s < sizeof(state_words) / sizeof(state_words[0]) ? state_words[s] : "unknown";
}

const char *zbx_err_word(enum zbx_err e)
{
    return (unsigned)e < ZBX_ERR_COUNT ? err_words[e] : "internal";
}

const char *zbx_err_text(enum zbx_err e)
{
    return (unsigned)e < ZBX_ERR_COUNT ? err_texts[e] : err_texts[ZBX_ERR_INTERNAL];
}

int zbx_conn_state_parse(const char *w)
{
    size_t i;

    for (i = 0; i < sizeof(state_words) / sizeof(state_words[0]); i++) {
        if (strcmp(w, state_words[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int zbx_err_parse(const char *w)
{
    int i;

    for (i = 0; i < ZBX_ERR_COUNT; i++) {
        if (strcmp(w, err_words[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static const char *const cresult_words[ZBX_CRESULT_COUNT] = { "connected", "authfail", "unreachable",
                                                              "invalid" };

const char *zbx_cresult_word(enum zbx_cresult r)
{
    return (unsigned)r < ZBX_CRESULT_COUNT ? cresult_words[r] : "invalid";
}

int zbx_cresult_parse(const char *w)
{
    int i;

    for (i = 0; i < ZBX_CRESULT_COUNT; i++) {
        if (strcmp(w, cresult_words[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* ---- hex ---------------------------------------------------------------------- */

int zbx_hex_encode(char *dst, size_t len, const char *src)
{
    static const char digits[] = "0123456789abcdef";
    size_t n = strlen(src);
    size_t i;

    if (len == 0 || n > (len - 1) / 2) {
        if (len) {
            dst[0] = '\0';
        }
        return -1;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];

        dst[2 * i] = digits[c >> 4];
        dst[2 * i + 1] = digits[c & 15];
    }
    dst[2 * n] = '\0';
    return 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int zbx_hex_decode(char *dst, size_t len, const char *src)
{
    size_t n = strlen(src);
    size_t i;

    if (len == 0) {
        return -1;
    }
    dst[0] = '\0';
    if (n % 2 != 0 || n / 2 >= len) {
        return -1;
    }
    for (i = 0; i < n / 2; i++) {
        int hi = hex_digit(src[2 * i]);
        int lo = hex_digit(src[2 * i + 1]);

        if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
            explicit_bzero(dst, i);
            dst[0] = '\0';
            return -1;
        }
        dst[i] = (char)(hi << 4 | lo);
    }
    dst[n / 2] = '\0';
    return (int)(n / 2);
}

/* ---- writing -------------------------------------------------------------------- */

/* A line is built here, then written with one fputs + fflush, so a reader
 * never sees half of one from this side. */
struct line {
    char buf[ZBX_LINE_MAX];
    size_t len;
    bool overflow;
};

static void l_start(struct line *l, const char *word)
{
    l->len = 0;
    l->overflow = false;
    l->buf[0] = '\0';
    if (word) {
        size_t n = strlen(word);

        if (n < sizeof(l->buf)) {
            memcpy(l->buf, word, n + 1);
            l->len = n;
        }
    }
}

static void l_raw(struct line *l, const char *s)
{
    size_t n = strlen(s);

    if (l->len + 1 + n >= sizeof(l->buf)) {
        l->overflow = true;
        return;
    }
    l->buf[l->len++] = '\t';
    memcpy(l->buf + l->len, s, n + 1);
    l->len += n;
}

/* A text field: cleaned (no TAB, no newline), and "-" for an empty one so
 * that every field has at least one character and a split cannot shift. */
static void l_text(struct line *l, const char *s, size_t max)
{
    char tmp[ZBX_LINE_MAX];

    if (max > sizeof(tmp)) {
        max = sizeof(tmp);
    }
    zbx_copy_text(tmp, max, s);
    l_raw(l, tmp[0] ? tmp : "-");
}

static void l_int(struct line *l, long long v)
{
    char tmp[24];

    snprintf(tmp, sizeof(tmp), "%lld", v);
    l_raw(l, tmp);
}

static void l_id(struct line *l, const char *id)
{
    l_text(l, id, ZBX_ID_MAX);
}

static int l_emit(FILE *out, struct line *l)
{
    if (l->overflow || l->len + 2 > sizeof(l->buf)) {
        return -1;
    }
    l->buf[l->len++] = '\n';
    l->buf[l->len] = '\0';
    if (fputs(l->buf, out) == EOF) {
        return -1;
    }
    return fflush(out) == 0 ? 0 : -1;
}

int zbx_proto_hello(FILE *out, bool fake, const char *scenario)
{
    struct line l;

    l_start(&l, "hello");
    l_int(&l, ZBX_PROTO_VERSION);
    l_raw(&l, fake ? "fake" : "live");
    l_text(&l, scenario, ZBX_TEXT_MAX);
    return l_emit(out, &l);
}

int zbx_proto_config(FILE *out, const char *label, const char *url, const char *auth,
                     bool verify, int refresh_s, int hosts_s)
{
    struct line l;

    l_start(&l, "config");
    l_text(&l, label, ZBX_TEXT_MAX);
    l_text(&l, url, ZBX_URL_MAX);
    l_text(&l, auth, ZBX_TEXT_MAX);
    l_int(&l, verify ? 1 : 0);
    l_int(&l, refresh_s);
    l_int(&l, hosts_s);
    return l_emit(out, &l);
}

int zbx_proto_state(FILE *out, enum zbx_conn_state st, int attempt, int retry_s, enum zbx_err err,
                    const char *text)
{
    struct line l;

    l_start(&l, "state");
    l_raw(&l, zbx_conn_state_word(st));
    l_int(&l, attempt);
    l_int(&l, retry_s);
    l_raw(&l, zbx_err_word(err));
    l_text(&l, text, ZBX_TEXT_MAX);
    return l_emit(out, &l);
}

int zbx_proto_busy(FILE *out, const char *what)
{
    struct line l;

    l_start(&l, "busy");
    l_text(&l, what, ZBX_TEXT_MAX);
    return l_emit(out, &l);
}

int zbx_proto_idle(FILE *out)
{
    struct line l;

    l_start(&l, "idle");
    return l_emit(out, &l);
}

int zbx_proto_version(FILE *out, const char *version)
{
    struct line l;

    l_start(&l, "version");
    l_text(&l, version, ZBX_VERSION_MAX);
    return l_emit(out, &l);
}

static void l_problem(struct line *l, const char *word, const struct zbx_problem *p)
{
    l_start(l, word);
    l_id(l, p->eventid);
    l_id(l, p->objectid);
    l_id(l, p->hostid);
    l_int(l, p->severity);
    l_int(l, p->acknowledged ? 1 : 0);
    l_int(l, p->suppressed ? 1 : 0);
    l_int(l, p->clock);
    l_text(l, p->host, ZBX_HOST_NAME_MAX);
    l_text(l, p->name, ZBX_PROBLEM_NAME_MAX);
}

int zbx_proto_problems(FILE *out, unsigned seq, const struct zbx_problem_set *s)
{
    struct line l;
    int i;

    l_start(&l, "pbegin");
    l_int(&l, seq);
    l_int(&l, s->ref_clock);
    l_int(&l, s->total);
    l_int(&l, s->total_exact ? 1 : 0);
    l_int(&l, s->sev_exact ? 1 : 0);
    l_int(&l, s->unacknowledged);
    for (i = 0; i < ZBX_SEV_COUNT; i++) {
        l_int(&l, s->sev_count[i]);
    }
    if (l_emit(out, &l) != 0) {
        return -1;
    }
    for (i = 0; i < s->count && i < ZBX_PROBLEM_MAX; i++) {
        l_problem(&l, "pb", &s->p[i]);
        if (l_emit(out, &l) != 0) {
            return -1;
        }
    }
    l_start(&l, "pend");
    l_int(&l, seq);
    l_int(&l, i);
    return l_emit(out, &l);
}

int zbx_proto_hosts(FILE *out, unsigned seq, const struct zbx_host_set *s)
{
    struct line l;
    int i;

    l_start(&l, "hbegin");
    l_int(&l, seq);
    l_int(&l, s->ref_clock);
    l_int(&l, s->total);
    l_int(&l, s->total_exact ? 1 : 0);
    l_int(&l, s->fetched);
    l_int(&l, s->down);
    l_int(&l, s->unknown);
    l_int(&l, s->maintenance);
    l_int(&l, s->down_exact ? 1 : 0);
    if (l_emit(out, &l) != 0) {
        return -1;
    }
    for (i = 0; i < s->count && i < ZBX_HOST_MAX; i++) {
        const struct zbx_host *h = &s->h[i];

        l_start(&l, "ho");
        l_id(&l, h->hostid);
        l_int(&l, h->avail);
        l_int(&l, h->maintenance ? 1 : 0);
        l_int(&l, h->problems);
        l_int(&l, h->max_severity);
        l_text(&l, h->name, ZBX_HOST_NAME_MAX);
        if (l_emit(out, &l) != 0) {
            return -1;
        }
    }
    l_start(&l, "hend");
    l_int(&l, seq);
    l_int(&l, i);
    return l_emit(out, &l);
}

int zbx_proto_detail(FILE *out, unsigned seq, const struct zbx_detail *d)
{
    struct line l;
    int i;
    int k;

    l_start(&l, "dbegin");
    l_int(&l, seq);
    l_int(&l, d->ref_clock);
    l_id(&l, d->host.hostid);
    l_int(&l, d->host.avail);
    l_int(&l, d->host.maintenance ? 1 : 0);
    l_text(&l, d->host.name, ZBX_HOST_NAME_MAX);
    if (l_emit(out, &l) != 0) {
        return -1;
    }
    for (i = 0; i < d->problem_count && i < ZBX_DETAIL_PROBLEM_MAX; i++) {
        l_problem(&l, "dp", &d->p[i]);
        if (l_emit(out, &l) != 0) {
            return -1;
        }
    }
    for (k = 0; k < d->item_count && k < ZBX_ITEM_MAX; k++) {
        const struct zbx_item *it = &d->item[k];

        l_start(&l, "di");
        l_id(&l, it->itemid);
        l_int(&l, it->clock);
        l_text(&l, it->value, ZBX_VALUE_MAX);
        l_text(&l, it->units, ZBX_UNITS_MAX);
        l_text(&l, it->name, ZBX_ITEM_NAME_MAX);
        if (l_emit(out, &l) != 0) {
            return -1;
        }
    }
    l_start(&l, "dend");
    l_int(&l, seq);
    l_int(&l, i);
    l_int(&l, k);
    return l_emit(out, &l);
}

int zbx_proto_detail_none(FILE *out, const char *hostid, const char *text)
{
    struct line l;

    l_start(&l, "dnone");
    l_id(&l, hostid);
    l_text(&l, text, ZBX_TEXT_MAX);
    return l_emit(out, &l);
}

int zbx_proto_settings(FILE *out, const char *url, const char *auth, const char *user,
                       bool stored, const char *note)
{
    struct line l;

    l_start(&l, "settings");
    l_text(&l, url, ZBX_URL_MAX);
    l_raw(&l, auth && strcmp(auth, "password") == 0 ? "password" : "token");
    l_text(&l, user, ZBX_PROTO_USER_MAX);
    l_int(&l, stored ? 1 : 0);
    l_text(&l, note, ZBX_TEXT_MAX);
    return l_emit(out, &l);
}

int zbx_proto_cresult(FILE *out, bool save, enum zbx_cresult r, bool saved, const char *text)
{
    struct line l;

    l_start(&l, "cresult");
    l_raw(&l, save ? "save" : "test");
    l_raw(&l, zbx_cresult_word(r));
    l_int(&l, saved ? 1 : 0);
    l_text(&l, text, ZBX_TEXT_MAX);
    return l_emit(out, &l);
}

int zbx_proto_cmd_settings(char *buf, size_t len, bool save, const char *url, const char *auth,
                           const char *user, const char *secret)
{
    char hurl[2 * ZBX_URL_MAX];
    char huser[2 * ZBX_PROTO_USER_MAX];
    char hsecret[2 * ZBX_PROTO_SECRET_MAX];
    bool keep = !secret || !*secret;
    int n;
    int rc = -1;

    if (len) {
        buf[0] = '\0';
    }
    if (strlen(url ? url : "") >= ZBX_URL_MAX || strlen(user ? user : "") >= ZBX_PROTO_USER_MAX ||
        (!keep && strlen(secret) >= ZBX_PROTO_SECRET_MAX) || zbx_hex_encode(hurl, sizeof(hurl), url ? url : "") ||
        zbx_hex_encode(huser, sizeof(huser), user ? user : "") ||
        zbx_hex_encode(hsecret, sizeof(hsecret), keep ? "" : secret)) {
        goto out;
    }
    n = snprintf(buf, len, "%s\t%s\t%s\t%s\t%s\n", save ? "csave" : "ctest", hurl[0] ? hurl : "-",
                 auth && strcmp(auth, "password") == 0 ? "password" : "token", huser[0] ? huser : "-",
                 keep ? "-" : hsecret);
    if (n < 0 || (size_t)n >= len) {
        explicit_bzero(buf, len);
        buf[0] = '\0';
        goto out;
    }
    rc = 0;
out:
    explicit_bzero(hsecret, sizeof(hsecret));
    return rc;
}

int zbx_proto_bye(FILE *out)
{
    struct line l;

    l_start(&l, "bye");
    return l_emit(out, &l);
}

/* ---- reading --------------------------------------------------------------------- */

static int split(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;

    while (n < max) {
        char *tab = strchr(p, '\t');

        f[n++] = p;
        if (!tab) {
            return n;
        }
        *tab = '\0';
        p = tab + 1;
    }
    return -1; /* more fields than any line has */
}

static bool num(const char *s, long long lo, long long hi, long long *out)
{
    char *end;
    long long v;

    if (!s || !*s || (*s != '-' && (*s < '0' || *s > '9'))) {
        return false;
    }
    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno || *end || v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}

/* "-" is how an empty text field travels. */
static void text(char *dst, size_t len, const char *s)
{
    zbx_copy_text(dst, len, strcmp(s, "-") == 0 ? "" : s);
}

static bool id_ok(const char *s)
{
    return strlen(s) < ZBX_ID_MAX;
}

static bool parse_problem(char **f, int n, struct zbx_problem *p)
{
    long long sev;
    long long ack;
    long long supp;
    long long clock;

    if (n != 10 || !id_ok(f[1]) || !id_ok(f[2]) || !id_ok(f[3]) ||
        !num(f[4], ZBX_SEV_NOT_CLASSIFIED, ZBX_SEV_DISASTER, &sev) || !num(f[5], 0, 1, &ack) ||
        !num(f[6], 0, 1, &supp) || !num(f[7], 0, INT64_MAX, &clock)) {
        return false;
    }
    memset(p, 0, sizeof(*p));
    text(p->eventid, sizeof(p->eventid), f[1]);
    text(p->objectid, sizeof(p->objectid), f[2]);
    text(p->hostid, sizeof(p->hostid), f[3]);
    p->severity = (int8_t)sev;
    p->acknowledged = ack == 1;
    p->suppressed = supp == 1;
    p->clock = clock;
    text(p->host, sizeof(p->host), f[8]);
    text(p->name, sizeof(p->name), f[9]);
    return true;
}

void zbx_rx_init(struct zbx_rx *rx)
{
    memset(rx, 0, sizeof(*rx));
}

static enum zbx_rx_kind bad(struct zbx_rx *rx, struct zbx_rx_msg *msg)
{
    rx->bad++;
    msg->kind = ZBX_RX_BAD;
    return ZBX_RX_BAD;
}

enum zbx_rx_kind zbx_rx_line(struct zbx_rx *rx, char *line, struct zbx_rx_msg *msg)
{
    char *f[FIELDS_MAX];
    int n;
    long long v[12];
    int i;

    memset(msg, 0, sizeof(*msg));
    n = split(line, f, FIELDS_MAX);
    if (n < 1) {
        return bad(rx, msg);
    }

    if (strcmp(f[0], "hello") == 0) {
        if (n != 4 || !num(f[1], 0, 1000, &v[0]) ||
            (strcmp(f[2], "fake") != 0 && strcmp(f[2], "live") != 0)) {
            return bad(rx, msg);
        }
        msg->proto = (int)v[0];
        msg->fake = strcmp(f[2], "fake") == 0;
        text(msg->word, sizeof(msg->word), f[3]);
        return msg->kind = ZBX_RX_HELLO;
    }
    if (strcmp(f[0], "config") == 0) {
        if (n != 7 || !num(f[4], 0, 1, &v[0]) || !num(f[5], 0, 86400, &v[1]) ||
            !num(f[6], 0, 86400, &v[2])) {
            return bad(rx, msg);
        }
        text(msg->label, sizeof(msg->label), f[1]);
        text(msg->url, sizeof(msg->url), f[2]);
        text(msg->word, sizeof(msg->word), f[3]);
        msg->verify = v[0] == 1;
        msg->refresh_s = (int)v[1];
        msg->hosts_s = (int)v[2];
        return msg->kind = ZBX_RX_CONFIG;
    }
    if (strcmp(f[0], "state") == 0) {
        int st;
        int err;

        if (n != 6 || (st = zbx_conn_state_parse(f[1])) < 0 || !num(f[2], 0, 1000000, &v[0]) ||
            !num(f[3], 0, 86400, &v[1]) || (err = zbx_err_parse(f[4])) < 0) {
            return bad(rx, msg);
        }
        msg->state = (enum zbx_conn_state)st;
        msg->attempt = (int)v[0];
        msg->retry_s = (int)v[1];
        msg->err = (enum zbx_err)err;
        text(msg->text, sizeof(msg->text), f[5]);
        return msg->kind = ZBX_RX_STATE;
    }
    if (strcmp(f[0], "busy") == 0) {
        if (n != 2) {
            return bad(rx, msg);
        }
        text(msg->word, sizeof(msg->word), f[1]);
        return msg->kind = ZBX_RX_BUSY;
    }
    if (strcmp(f[0], "idle") == 0) {
        return msg->kind = (n == 1 ? ZBX_RX_IDLE : bad(rx, msg));
    }
    if (strcmp(f[0], "version") == 0) {
        if (n != 2) {
            return bad(rx, msg);
        }
        text(msg->word, ZBX_VERSION_MAX, f[1]);
        return msg->kind = ZBX_RX_VERSION;
    }
    if (strcmp(f[0], "bye") == 0) {
        return msg->kind = ZBX_RX_BYE;
    }

    /* ---- problems ---- */
    if (strcmp(f[0], "pbegin") == 0) {
        struct zbx_problem_set *s = &rx->problems;

        if (n != 13) {
            return bad(rx, msg);
        }
        for (i = 1; i < 13; i++) {
            if (!num(f[i], 0, i == 1 ? UINT_MAX : i == 2 ? INT64_MAX : INT_MAX, &v[i - 1])) {
                return bad(rx, msg);
            }
        }
        if (v[3] > 1 || v[4] > 1) {
            return bad(rx, msg);
        }
        memset(s, 0, sizeof(*s));
        rx->p_seq = (unsigned)v[0];
        s->ref_clock = v[1];
        s->total = (int)v[2];
        s->total_exact = v[3] == 1;
        s->sev_exact = v[4] == 1;
        s->unacknowledged = (int)v[5];
        for (i = 0; i < ZBX_SEV_COUNT; i++) {
            s->sev_count[i] = (int)v[6 + i];
        }
        rx->in_problems = true;
        return msg->kind = ZBX_RX_NONE;
    }
    if (strcmp(f[0], "pb") == 0) {
        struct zbx_problem_set *s = &rx->problems;

        if (!rx->in_problems || s->count >= ZBX_PROBLEM_MAX ||
            !parse_problem(f, n, &s->p[s->count])) {
            rx->in_problems = false;
            return bad(rx, msg);
        }
        s->count++;
        return msg->kind = ZBX_RX_NONE;
    }
    if (strcmp(f[0], "pend") == 0) {
        bool ok = rx->in_problems && n == 3 && num(f[1], 0, UINT_MAX, &v[0]) &&
                  num(f[2], 0, ZBX_PROBLEM_MAX, &v[1]) && (unsigned)v[0] == rx->p_seq &&
                  v[1] == rx->problems.count;

        rx->in_problems = false;
        return ok ? (msg->kind = ZBX_RX_PROBLEMS) : bad(rx, msg);
    }

    /* ---- hosts ---- */
    if (strcmp(f[0], "hbegin") == 0) {
        struct zbx_host_set *s = &rx->hosts;

        if (n != 10) {
            return bad(rx, msg);
        }
        for (i = 1; i < 10; i++) {
            if (!num(f[i], 0, i == 1 ? UINT_MAX : i == 2 ? INT64_MAX : INT_MAX, &v[i - 1])) {
                return bad(rx, msg);
            }
        }
        if (v[3] > 1 || v[8] > 1) {
            return bad(rx, msg);
        }
        memset(s, 0, sizeof(*s));
        rx->h_seq = (unsigned)v[0];
        s->ref_clock = v[1];
        s->total = (int)v[2];
        s->total_exact = v[3] == 1;
        s->fetched = (int)v[4];
        s->down = (int)v[5];
        s->unknown = (int)v[6];
        s->maintenance = (int)v[7];
        s->down_exact = v[8] == 1;
        rx->in_hosts = true;
        return msg->kind = ZBX_RX_NONE;
    }
    if (strcmp(f[0], "ho") == 0) {
        struct zbx_host_set *s = &rx->hosts;
        struct zbx_host *h;

        if (!rx->in_hosts || s->count >= ZBX_HOST_MAX || n != 7 || !id_ok(f[1]) ||
            !num(f[2], 0, 2, &v[0]) || !num(f[3], 0, 1, &v[1]) || !num(f[4], 0, INT_MAX, &v[2]) ||
            !num(f[5], ZBX_SEV_NONE, ZBX_SEV_DISASTER, &v[3])) {
            rx->in_hosts = false;
            return bad(rx, msg);
        }
        h = &s->h[s->count++];
        memset(h, 0, sizeof(*h));
        text(h->hostid, sizeof(h->hostid), f[1]);
        h->avail = (enum zbx_avail)v[0];
        h->maintenance = v[1] == 1;
        h->problems = (int)v[2];
        h->max_severity = (int8_t)v[3];
        text(h->name, sizeof(h->name), f[6]);
        return msg->kind = ZBX_RX_NONE;
    }
    if (strcmp(f[0], "hend") == 0) {
        bool ok = rx->in_hosts && n == 3 && num(f[1], 0, UINT_MAX, &v[0]) &&
                  num(f[2], 0, ZBX_HOST_MAX, &v[1]) && (unsigned)v[0] == rx->h_seq &&
                  v[1] == rx->hosts.count;

        rx->in_hosts = false;
        return ok ? (msg->kind = ZBX_RX_HOSTS) : bad(rx, msg);
    }

    /* ---- detail ---- */
    if (strcmp(f[0], "dbegin") == 0) {
        struct zbx_detail *d = &rx->detail;

        if (n != 7 || !num(f[1], 0, UINT_MAX, &v[0]) || !num(f[2], 0, INT64_MAX, &v[1]) ||
            !id_ok(f[3]) || !num(f[4], 0, 2, &v[2]) || !num(f[5], 0, 1, &v[3])) {
            return bad(rx, msg);
        }
        memset(d, 0, sizeof(*d));
        rx->d_seq = (unsigned)v[0];
        d->ref_clock = v[1];
        text(d->host.hostid, sizeof(d->host.hostid), f[3]);
        d->host.avail = (enum zbx_avail)v[2];
        d->host.maintenance = v[3] == 1;
        d->host.max_severity = ZBX_SEV_NONE;
        text(d->host.name, sizeof(d->host.name), f[6]);
        rx->in_detail = true;
        return msg->kind = ZBX_RX_NONE;
    }
    if (strcmp(f[0], "dp") == 0) {
        struct zbx_detail *d = &rx->detail;
        struct zbx_problem *p;

        if (!rx->in_detail || d->problem_count >= ZBX_DETAIL_PROBLEM_MAX ||
            !parse_problem(f, n, &d->p[d->problem_count])) {
            rx->in_detail = false;
            return bad(rx, msg);
        }
        p = &d->p[d->problem_count++];
        d->host.problems = d->problem_count;
        if (p->severity > d->host.max_severity) {
            d->host.max_severity = p->severity;
        }
        return msg->kind = ZBX_RX_NONE;
    }
    if (strcmp(f[0], "di") == 0) {
        struct zbx_detail *d = &rx->detail;
        struct zbx_item *it;

        if (!rx->in_detail || d->item_count >= ZBX_ITEM_MAX || n != 6 || !id_ok(f[1]) ||
            !num(f[2], 0, INT64_MAX, &v[0])) {
            rx->in_detail = false;
            return bad(rx, msg);
        }
        it = &d->item[d->item_count++];
        memset(it, 0, sizeof(*it));
        text(it->itemid, sizeof(it->itemid), f[1]);
        it->clock = v[0];
        text(it->value, sizeof(it->value), f[3]);
        text(it->units, sizeof(it->units), f[4]);
        text(it->name, sizeof(it->name), f[5]);
        return msg->kind = ZBX_RX_NONE;
    }
    if (strcmp(f[0], "dend") == 0) {
        bool ok = rx->in_detail && n == 4 && num(f[1], 0, UINT_MAX, &v[0]) &&
                  num(f[2], 0, ZBX_DETAIL_PROBLEM_MAX, &v[1]) && num(f[3], 0, ZBX_ITEM_MAX, &v[2]) &&
                  (unsigned)v[0] == rx->d_seq && v[1] == rx->detail.problem_count &&
                  v[2] == rx->detail.item_count;

        rx->in_detail = false;
        return ok ? (msg->kind = ZBX_RX_DETAIL) : bad(rx, msg);
    }
    if (strcmp(f[0], "dnone") == 0) {
        if (n != 3 || !id_ok(f[1])) {
            return bad(rx, msg);
        }
        text(msg->word, sizeof(msg->word), f[1]);
        text(msg->text, sizeof(msg->text), f[2]);
        return msg->kind = ZBX_RX_DETAIL_NONE;
    }

    /* ---- the CONNECTION screen ---- */
    if (strcmp(f[0], "settings") == 0) {
        if (n != 6 || (strcmp(f[2], "token") != 0 && strcmp(f[2], "password") != 0) ||
            !num(f[4], 0, 1, &v[0])) {
            return bad(rx, msg);
        }
        text(msg->url, sizeof(msg->url), f[1]);
        text(msg->word, sizeof(msg->word), f[2]);
        text(msg->user, sizeof(msg->user), f[3]);
        msg->flag = v[0] == 1;
        text(msg->text, sizeof(msg->text), f[5]);
        return msg->kind = ZBX_RX_SETTINGS;
    }
    if (strcmp(f[0], "cresult") == 0) {
        int r;

        if (n != 5 || (strcmp(f[1], "test") != 0 && strcmp(f[1], "save") != 0) ||
            (r = zbx_cresult_parse(f[2])) < 0 || !num(f[3], 0, 1, &v[0])) {
            return bad(rx, msg);
        }
        msg->save = strcmp(f[1], "save") == 0;
        msg->result = (enum zbx_cresult)r;
        msg->flag = v[0] == 1;
        text(msg->text, sizeof(msg->text), f[4]);
        return msg->kind = ZBX_RX_CRESULT;
    }
    return msg->kind = ZBX_RX_NONE; /* a word from a newer helper */
}

/* ---- commands ------------------------------------------------------------------------ */

/* "-" for an empty hex field. */
static bool hex_field(char *dst, size_t len, const char *src)
{
    if (strcmp(src, "-") == 0) {
        dst[0] = '\0';
        return true;
    }
    return zbx_hex_decode(dst, len, src) > 0;
}

enum zbx_cmd_kind zbx_cmd_parse(char *line, struct zbx_cmd *cmd)
{
    char *f[6];
    int n;

    memset(cmd, 0, sizeof(*cmd));
    n = split(line, f, 6);
    if (n == 5 && (strcmp(f[0], "ctest") == 0 || strcmp(f[0], "csave") == 0)) {
        if ((strcmp(f[2], "token") != 0 && strcmp(f[2], "password") != 0) ||
            !hex_field(cmd->url, sizeof(cmd->url), f[1]) ||
            !hex_field(cmd->user, sizeof(cmd->user), f[3]) ||
            !hex_field(cmd->secret, sizeof(cmd->secret), f[4])) {
            explicit_bzero(cmd, sizeof(*cmd));
            return ZBX_CMD_NONE;
        }
        cmd->keep_secret = strcmp(f[4], "-") == 0;
        snprintf(cmd->word, sizeof(cmd->word), "%s", f[2]);
        cmd->kind = strcmp(f[0], "csave") == 0 ? ZBX_CMD_SAVE : ZBX_CMD_TEST;
        return cmd->kind;
    }
    if (n < 1) {
        return ZBX_CMD_NONE;
    }
    if (strcmp(f[0], "refresh") == 0 && n == 1) {
        cmd->kind = ZBX_CMD_REFRESH;
    } else if (strcmp(f[0], "detail") == 0 && n == 2 && strlen(f[1]) < ZBX_ID_MAX) {
        cmd->kind = ZBX_CMD_DETAIL;
        text(cmd->word, sizeof(cmd->word), f[1]);
    } else if (strcmp(f[0], "scenario") == 0 && n == 2) {
        cmd->kind = ZBX_CMD_SCENARIO;
        text(cmd->word, sizeof(cmd->word), f[1]);
    } else if (strcmp(f[0], "quit") == 0 && n == 1) {
        cmd->kind = ZBX_CMD_QUIT;
    }
    return cmd->kind;
}
