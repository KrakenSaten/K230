/*
 * The Zabbix screen's model and words. See zabbix_view.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "zabbix_view.h"

#include <stdio.h>
#include <string.h>

void zabbix_model_init(struct zabbix_model *m)
{
    memset(m, 0, sizeof(*m));
    m->state = ZBX_CONN_CONNECTING;
    m->restart_delay_ms = ZABBIX_RESTART_FIRST_MS;
    m->refresh_s = 30;
    m->hosts_s = 60;
}

void zabbix_model_forget(struct zabbix_model *m)
{
    unsigned starts = m->helper_starts;
    bool running = m->helper_running;

    zabbix_model_init(m);
    m->helper_starts = starts;
    m->helper_running = running;
}

void zabbix_model_helper_started(struct zabbix_model *m, int64_t now_ms)
{
    (void)now_ms;
    m->helper_running = true;
    m->helper_starts++;
    m->restart_at_ms = 0;
}

void zabbix_model_helper_stopped(struct zabbix_model *m, enum zabbix_exit reason, int value,
                                 int64_t now_ms)
{
    m->helper_running = false;
    m->exit_reason = reason;
    m->exit_value = value;
    m->busy = false;
    m->restart_at_ms = now_ms + m->restart_delay_ms;
    m->restart_delay_ms *= 2;
    if (m->restart_delay_ms > ZABBIX_RESTART_MAX_MS) {
        m->restart_delay_ms = ZABBIX_RESTART_MAX_MS;
    }
}

static void got_data(struct zabbix_model *m, int64_t now_ms)
{
    m->last_ok_ms = now_ms;
    /* A helper that delivers is a helper that works: the next restart, if
     * one is ever needed, starts from the short delay again. */
    m->restart_delay_ms = ZABBIX_RESTART_FIRST_MS;
}

unsigned zabbix_model_apply(struct zabbix_model *m, const struct zbx_rx_msg *msg,
                            const struct zbx_rx *rx, int64_t now_ms)
{
    switch (msg->kind) {
    case ZBX_RX_HELLO:
        m->have_hello = true;
        m->fake = msg->fake;
        snprintf(m->scenario, sizeof(m->scenario), "%s", msg->word);
        return ZABBIX_CHANGED_STATE;
    case ZBX_RX_CONFIG:
        m->have_config = true;
        snprintf(m->label, sizeof(m->label), "%s", msg->label);
        snprintf(m->url, sizeof(m->url), "%s", msg->url);
        snprintf(m->auth, sizeof(m->auth), "%s", msg->word);
        m->verify = msg->verify;
        m->refresh_s = msg->refresh_s > 0 ? msg->refresh_s : 30;
        m->hosts_s = msg->hosts_s > 0 ? msg->hosts_s : 60;
        return ZABBIX_CHANGED_STATE;
    case ZBX_RX_STATE:
        m->state = msg->state;
        m->err = msg->err;
        snprintf(m->err_text, sizeof(m->err_text), "%s", msg->text);
        m->attempt = msg->attempt;
        m->retry_s = msg->retry_s;
        m->state_ms = now_ms;
        if (msg->err != ZBX_ERR_NONE && msg->state != ZBX_CONN_CONNECTING) {
            m->last_err = msg->err;
            snprintf(m->last_err_text, sizeof(m->last_err_text), "%s", msg->text);
            m->last_err_ms = now_ms;
        }
        if (msg->state == ZBX_CONN_ONLINE) {
            got_data(m, now_ms);
        }
        return ZABBIX_CHANGED_STATE;
    case ZBX_RX_BUSY:
        m->busy = true;
        snprintf(m->busy_what, sizeof(m->busy_what), "%s", msg->word);
        return ZABBIX_CHANGED_STATE;
    case ZBX_RX_IDLE:
        m->busy = false;
        m->busy_what[0] = '\0';
        return ZABBIX_CHANGED_STATE;
    case ZBX_RX_VERSION:
        snprintf(m->version, sizeof(m->version), "%.15s", msg->word);
        return ZABBIX_CHANGED_STATE;
    case ZBX_RX_PROBLEMS:
        m->problems = rx->problems;
        m->problems_ms = now_ms;
        got_data(m, now_ms);
        return ZABBIX_CHANGED_PROBLEMS;
    case ZBX_RX_HOSTS:
        m->hosts = rx->hosts;
        m->hosts_ms = now_ms;
        got_data(m, now_ms);
        return ZABBIX_CHANGED_HOSTS;
    case ZBX_RX_DETAIL:
        m->detail = rx->detail;
        m->detail_ms = now_ms;
        m->detail_missing[0] = '\0';
        got_data(m, now_ms);
        return ZABBIX_CHANGED_DETAIL;
    case ZBX_RX_DETAIL_NONE:
        snprintf(m->detail_missing, sizeof(m->detail_missing), "%.23s", msg->word);
        snprintf(m->detail_missing_text, sizeof(m->detail_missing_text), "%s", msg->text);
        return ZABBIX_CHANGED_DETAIL;
    case ZBX_RX_BYE:
    case ZBX_RX_NONE:
    case ZBX_RX_BAD:
    default:
        return 0;
    }
}

/* ---- words ------------------------------------------------------------------------ */

enum zabbix_tone zabbix_severity_tone(int severity)
{
    if (severity >= ZBX_SEV_HIGH) {
        return ZABBIX_TONE_ERROR;
    }
    if (severity >= ZBX_SEV_WARNING) {
        return ZABBIX_TONE_WARN;
    }
    if (severity >= ZBX_SEV_NOT_CLASSIFIED) {
        return ZABBIX_TONE_QUIET;
    }
    return ZABBIX_TONE_OK;
}

void zabbix_format_count(char *out, size_t len, int n)
{
    char digits[16];
    char tmp[24];
    int nd;
    int i;
    int o = 0;

    if (n < 0) {
        n = 0;
    }
    nd = snprintf(digits, sizeof(digits), "%d", n);
    for (i = 0; i < nd && o < (int)sizeof(tmp) - 2; i++) {
        if (i > 0 && (nd - i) % 3 == 0) {
            tmp[o++] = ' ';
        }
        tmp[o++] = digits[i];
    }
    tmp[o] = '\0';
    snprintf(out, len, "%s", tmp);
}

void zabbix_format_since(char *out, size_t len, int64_t now_ms, int64_t then_ms)
{
    int64_t s;

    if (then_ms <= 0) {
        snprintf(out, len, "never");
        return;
    }
    s = (now_ms - then_ms) / 1000;
    if (s < 0) {
        s = 0;
    }
    if (s < 60) {
        snprintf(out, len, "%llds ago", (long long)s);
    } else if (s < 3600) {
        snprintf(out, len, "%lldm ago", (long long)(s / 60));
    } else if (s < 86400) {
        snprintf(out, len, "%lldh %lldm ago", (long long)(s / 3600), (long long)((s % 3600) / 60));
    } else {
        snprintf(out, len, "%lldd ago", (long long)(s / 86400));
    }
}

int64_t zabbix_server_now(int64_t ref_clock, int64_t arrived_ms, int64_t now_ms)
{
    if (ref_clock <= 0 || arrived_ms <= 0) {
        return 0;
    }
    return ref_clock + (now_ms - arrived_ms) / 1000;
}

static bool stale(const struct zabbix_model *m, int64_t at_ms, int interval_s, int64_t now_ms)
{
    if (at_ms <= 0) {
        return false;
    }
    if (!m->helper_running || m->state == ZBX_CONN_RETRYING ||
        m->state == ZBX_CONN_AUTH_FAILED ||
        (m->state == ZBX_CONN_CONNECTING && m->attempt > 0)) {
        return true;
    }
    return now_ms - at_ms > (int64_t)interval_s * 2000 + ZABBIX_STALE_GRACE_MS;
}

bool zabbix_problems_stale(const struct zabbix_model *m, int64_t now_ms)
{
    return stale(m, m->problems_ms, m->refresh_s, now_ms);
}

bool zabbix_hosts_stale(const struct zabbix_model *m, int64_t now_ms)
{
    return stale(m, m->hosts_ms, m->hosts_s, now_ms);
}

static const char *what_failed(const struct zabbix_model *m)
{
    if (m->err == ZBX_ERR_NONE) {
        return "Not connected";
    }
    return zbx_err_text(m->err);
}

void zabbix_view_banner(const struct zabbix_model *m, int64_t now_ms, struct zabbix_banner *b)
{
    char age[40];
    char data[64];
    bool have = m->problems_ms > 0 || m->hosts_ms > 0;
    int64_t newest = m->problems_ms > m->hosts_ms ? m->problems_ms : m->hosts_ms;

    memset(b, 0, sizeof(*b));
    zabbix_format_since(age, sizeof(age), now_ms, newest);
    if (have) {
        snprintf(data, sizeof(data), "showing data from %s", age);
    } else {
        snprintf(data, sizeof(data), "no data yet");
    }
    if (!m->helper_running && m->helper_starts > 0) {
        int64_t left = m->restart_at_ms > now_ms ? (m->restart_at_ms - now_ms + 999) / 1000 : 0;

        b->show = true;
        b->tone = ZABBIX_TONE_ERROR;
        snprintf(b->text, sizeof(b->text), "Zabbix helper stopped%s · restarting in %llds · %s",
                 m->exit_reason == ZABBIX_EXIT_HUNG ? " (hung)" :
                 m->exit_reason == ZABBIX_EXIT_CRASHED ? " (crashed)" :
                 m->exit_reason == ZABBIX_EXIT_START ? " (could not start)" : "",
                 (long long)left, data);
        return;
    }
    switch (m->state) {
    case ZBX_CONN_UNCONFIGURED:
        return; /* OVERVIEW shows the setup panel */
    case ZBX_CONN_CONNECTING:
        b->show = true;
        b->tone = m->attempt > 0 ? ZABBIX_TONE_WARN : ZABBIX_TONE_QUIET;
        if (m->attempt > 0) {
            snprintf(b->text, sizeof(b->text), "Reconnecting (try %d) · %s", m->attempt + 1, data);
        } else {
            snprintf(b->text, sizeof(b->text), "Connecting to %s", m->label[0] ? m->label : "Zabbix");
        }
        return;
    case ZBX_CONN_RETRYING: {
        int64_t left = (int64_t)m->retry_s * 1000 - (now_ms - m->state_ms);

        b->show = true;
        b->tone = ZABBIX_TONE_ERROR;
        snprintf(b->text, sizeof(b->text), "%s · retry in %llds · %s", what_failed(m),
                 (long long)(left > 0 ? (left + 999) / 1000 : 0), data);
        return;
    }
    case ZBX_CONN_AUTH_FAILED:
        b->show = true;
        b->tone = ZABBIX_TONE_ERROR;
        snprintf(b->text, sizeof(b->text), "Access refused: check the %s · %s",
                 strcmp(m->auth, "password") == 0 ? "user and password" : "API token", data);
        return;
    case ZBX_CONN_ONLINE:
    default:
        if (zabbix_problems_stale(m, now_ms) || zabbix_hosts_stale(m, now_ms)) {
            b->show = true;
            b->tone = ZABBIX_TONE_WARN;
            snprintf(b->text, sizeof(b->text), "Data is late · %s%s", data,
                     m->busy ? " · still refreshing" : "");
        }
        return;
    }
}

void zabbix_view_caption(const struct zabbix_model *m, int64_t now_ms, char *out, size_t len)
{
    char since[40];
    int64_t newest = m->problems_ms > m->hosts_ms ? m->problems_ms : m->hosts_ms;

    if (m->busy) {
        snprintf(out, len, "UPDATING");
        return;
    }
    if (m->state != ZBX_CONN_ONLINE || newest <= 0) {
        snprintf(out, len, "%s", m->state == ZBX_CONN_UNCONFIGURED ? "NOT SET UP" :
                                 m->state == ZBX_CONN_ONLINE ? "" :
                                 m->state == ZBX_CONN_CONNECTING ? "CONNECTING" : "OFFLINE");
        return;
    }
    zabbix_format_since(since, sizeof(since), now_ms, newest);
    snprintf(out, len, "%s", since);
}

void zabbix_view_overview(const struct zabbix_model *m, int64_t now_ms,
                          struct zabbix_overview_view *v)
{
    struct zbx_overview o;
    char a[24];
    char b[24];
    char since[40];

    memset(v, 0, sizeof(*v));
    v->setup = m->state == ZBX_CONN_UNCONFIGURED;
    v->loading = m->problems_ms <= 0;
    zbx_overview_build(&o, m->problems_ms > 0 ? &m->problems : NULL,
                       m->hosts_ms > 0 ? &m->hosts : NULL);
    memcpy(v->sev_count, o.sev_count, sizeof(v->sev_count));
    v->sev_exact = o.problems_exact;

    if (v->loading) {
        snprintf(v->headline, sizeof(v->headline), "--");
        v->headline_tone = ZABBIX_TONE_QUIET;
        snprintf(v->headline_note, sizeof(v->headline_note), "waiting for the first refresh");
    } else if (o.max_severity == ZBX_SEV_NONE) {
        snprintf(v->headline, sizeof(v->headline), "ALL CLEAR");
        v->headline_tone = ZABBIX_TONE_OK;
        snprintf(v->headline_note, sizeof(v->headline_note), "no open problems");
    } else {
        snprintf(v->headline, sizeof(v->headline), "%s", zbx_severity_word(o.max_severity));
        v->headline_tone = zabbix_severity_tone(o.max_severity);
        zabbix_format_count(a, sizeof(a), o.sev_count[o.max_severity]);
        zabbix_format_count(b, sizeof(b), o.problems_total);
        snprintf(v->headline_note, sizeof(v->headline_note), "%s at this severity, of %s open", a, b);
    }
    if (!v->loading) {
        zabbix_format_count(a, sizeof(a), o.problems_total);
        zabbix_format_count(b, sizeof(b), o.unacknowledged);
        snprintf(v->problems, sizeof(v->problems), "%s open · %s unacknowledged", a, b);
    }
    if (m->hosts_ms > 0) {
        zabbix_format_count(a, sizeof(a), o.hosts_total);
        zabbix_format_count(b, sizeof(b), o.hosts_down);
        /* Down comes from the server's own list of unavailable interfaces,
         * exact up to ZBX_HOST_FETCH of them; past that it is "at least". */
        snprintf(v->hosts, sizeof(v->hosts), "%s monitored · %s%s down", a, o.down_exact ? "" : ">=", b);
        v->hosts_tone = o.hosts_down > 0 ? ZABBIX_TONE_ERROR : ZABBIX_TONE_OK;
        zabbix_format_count(a, sizeof(a), o.hosts_unknown);
        zabbix_format_count(b, sizeof(b), o.hosts_maintenance);
        /* Unknown and maintenance are counted over the hosts read. */
        snprintf(v->hosts_note, sizeof(v->hosts_note), "%s unknown · %s in maintenance%s", a, b,
                 o.hosts_exact ? "" : " (of the hosts read)");
    } else {
        v->hosts_tone = ZABBIX_TONE_QUIET;
    }
    snprintf(v->server, sizeof(v->server), "%s%s%s%s", m->label[0] ? m->label : "Zabbix",
             m->version[0] ? " · Zabbix " : "", m->version, m->fake ? " · SIMULATED" : "");
    zabbix_format_since(since, sizeof(since), now_ms,
                        m->problems_ms > m->hosts_ms ? m->problems_ms : m->hosts_ms);
    snprintf(v->updated, sizeof(v->updated), "Updated %s", since);
}

void zabbix_view_problem(const struct zbx_problem *p, int64_t server_now,
                         struct zabbix_problem_row *r)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->severity, sizeof(r->severity), "%s", zbx_severity_word(p->severity));
    r->tone = zabbix_severity_tone(p->severity);
    snprintf(r->host, sizeof(r->host), "%s", p->host[0] ? p->host : "(no host)");
    snprintf(r->name, sizeof(r->name), "%s", p->name);
    zbx_format_age(r->age, sizeof(r->age), server_now, p->clock);
    r->acknowledged = p->acknowledged;
    snprintf(r->meta, sizeof(r->meta), "%s%s", r->age, p->acknowledged ? " · ACK" : "");
}

void zabbix_view_problems_title(const struct zabbix_model *m, char *out, size_t len)
{
    char a[24];
    char b[24];

    if (m->problems_ms <= 0) {
        snprintf(out, len, "PROBLEMS");
        return;
    }
    zabbix_format_count(a, sizeof(a), m->problems.total);
    if (m->problems.count < m->problems.total) {
        zabbix_format_count(b, sizeof(b), m->problems.count);
        snprintf(out, len, "%s of %s open · most severe first", b, a);
    } else {
        snprintf(out, len, "%s open · most severe first", a);
    }
}

void zabbix_view_host(const struct zbx_host *h, struct zabbix_host_row *r)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", h->name);
    snprintf(r->avail, sizeof(r->avail), "%s", zbx_avail_word(h->avail));
    r->avail_tone = h->avail == ZBX_AVAIL_DOWN ? ZABBIX_TONE_ERROR :
                    h->avail == ZBX_AVAIL_UP ? ZABBIX_TONE_OK : ZABBIX_TONE_QUIET;
    if (h->problems > 0) {
        snprintf(r->problems, sizeof(r->problems), "%d problem%s · %s", h->problems,
                 h->problems == 1 ? "" : "s", zbx_severity_word(h->max_severity));
        r->problems_tone = zabbix_severity_tone(h->max_severity);
    } else {
        snprintf(r->problems, sizeof(r->problems), "no problems");
        r->problems_tone = ZABBIX_TONE_QUIET;
    }
    r->maintenance = h->maintenance;
    if (h->maintenance) {
        r->avail_tone = ZABBIX_TONE_QUIET;
    }
}

void zabbix_view_hosts_title(const struct zabbix_model *m, char *out, size_t len)
{
    char a[24];
    char b[24];

    if (m->hosts_ms <= 0) {
        snprintf(out, len, "HOSTS");
        return;
    }
    zabbix_format_count(a, sizeof(a), m->hosts.total);
    if (m->hosts.count < m->hosts.total) {
        zabbix_format_count(b, sizeof(b), m->hosts.count);
        snprintf(out, len, "%s of %s · attention first", b, a);
    } else {
        snprintf(out, len, "%s monitored · attention first", a);
    }
}

void zabbix_view_item(const struct zbx_item *it, int64_t server_now, struct zabbix_item_row *r)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", it->name);
    snprintf(r->value, sizeof(r->value), "%s", it->value[0] ? it->value : "--");
    zbx_format_age(r->age, sizeof(r->age), server_now, it->clock);
}

static void line(struct zabbix_status_view *v, const char *key, enum zabbix_tone tone,
                 const char *fmt, const char *a, const char *b)
{
    if (v->count >= ZABBIX_STATUS_LINES) {
        return;
    }
    snprintf(v->key[v->count], sizeof(v->key[0]), "%s", key);
    snprintf(v->value[v->count], sizeof(v->value[0]), fmt, a ? a : "", b ? b : "");
    v->tone[v->count] = tone;
    v->count++;
}

void zabbix_view_status(const struct zabbix_model *m, int64_t now_ms, struct zabbix_status_view *v)
{
    char t1[64];
    char t2[64];

    memset(v, 0, sizeof(*v));
    line(v, "SERVER", ZABBIX_TONE_PLAIN, "%s%s", m->label[0] ? m->label : "(no label)", NULL);
    line(v, "ADDRESS", ZABBIX_TONE_PLAIN, "%s%s", m->url[0] ? m->url : "(not set up)", NULL);
    line(v, "MODE", m->fake ? ZABBIX_TONE_WARN : ZABBIX_TONE_PLAIN, "%s%s",
         m->fake ? "SIMULATED · scenario " : "Live", m->fake ? m->scenario : NULL);
    line(v, "SIGN-IN", ZABBIX_TONE_PLAIN, "%s%s",
         strcmp(m->auth, "password") == 0 ? "User and password (session)" :
         strcmp(m->auth, "token") == 0 ? "API token (never shown)" : "None",
         NULL);
    if (!m->fake && strncmp(m->url, "http://", 7) == 0) {
        line(v, "ENCRYPTION", ZABBIX_TONE_ERROR, "%s%s", "None: plain http://", NULL);
    } else if (!m->fake) {
        line(v, "ENCRYPTION", m->verify ? ZABBIX_TONE_OK : ZABBIX_TONE_WARN, "%s%s",
             m->verify ? "HTTPS, certificate checked" : "HTTPS, certificate NOT checked", NULL);
    }
    line(v, "API VERSION", ZABBIX_TONE_PLAIN, "%s%s", m->version[0] ? m->version : "--", NULL);
    {
        enum zabbix_tone tone = m->state == ZBX_CONN_ONLINE ? ZABBIX_TONE_OK :
                                m->state == ZBX_CONN_CONNECTING ? ZABBIX_TONE_QUIET :
                                m->state == ZBX_CONN_UNCONFIGURED ? ZABBIX_TONE_QUIET :
                                ZABBIX_TONE_ERROR;
        const char *words = m->state == ZBX_CONN_ONLINE ? "Online" :
                            m->state == ZBX_CONN_CONNECTING ? "Connecting" :
                            m->state == ZBX_CONN_RETRYING ? "Offline, retrying" :
                            m->state == ZBX_CONN_AUTH_FAILED ? "Access refused" : "Not set up";

        if (!m->helper_running && m->helper_starts > 0) {
            words = "Helper stopped";
            tone = ZABBIX_TONE_ERROR;
        }
        line(v, "CONNECTION", tone, "%s%s", words, NULL);
    }
    zabbix_format_since(t1, sizeof(t1), now_ms, m->last_ok_ms);
    line(v, "LAST SUCCESS", ZABBIX_TONE_PLAIN, "%s%s", t1, NULL);
    if (m->last_err != ZBX_ERR_NONE) {
        zabbix_format_since(t2, sizeof(t2), now_ms, m->last_err_ms);
        snprintf(t1, sizeof(t1), "%s", zbx_err_text(m->last_err));
        line(v, "LAST ERROR", ZABBIX_TONE_ERROR, "%s · %s", t1, t2);
        if (m->last_err_text[0]) {
            line(v, "DETAIL", ZABBIX_TONE_QUIET, "%s%s", m->last_err_text, NULL);
        }
    } else {
        line(v, "LAST ERROR", ZABBIX_TONE_QUIET, "%s%s", "none", NULL);
    }
    snprintf(t1, sizeof(t1), "%d s", m->refresh_s);
    snprintf(t2, sizeof(t2), "%d s", m->hosts_s);
    line(v, "REFRESH", ZABBIX_TONE_PLAIN, "problems every %s, hosts every %s", t1, t2);
}
