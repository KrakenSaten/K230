/*
 * The Zabbix screen's words, on a clock the test owns: what the banner says
 * in every connection state, when data turns stale, what OVERVIEW makes of
 * zero, some and very many problems, the rows, and STATUS.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zabbix_view.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static struct zbx_rx rx;

static void apply_state(struct zabbix_model *m, enum zbx_conn_state st, enum zbx_err err, int attempt,
                        int retry_s, const char *text, int64_t now)
{
    struct zbx_rx_msg msg;

    memset(&msg, 0, sizeof(msg));
    msg.kind = ZBX_RX_STATE;
    msg.state = st;
    msg.err = err;
    msg.attempt = attempt;
    msg.retry_s = retry_s;
    snprintf(msg.text, sizeof(msg.text), "%s", text);
    zabbix_model_apply(m, &msg, &rx, now);
}

static void apply_problems(struct zabbix_model *m, int n, int total, int64_t now)
{
    struct zbx_rx_msg msg;
    int i;

    memset(&rx.problems, 0, sizeof(rx.problems));
    for (i = 0; i < n; i++) {
        struct zbx_problem *p = &rx.problems.p[i];

        snprintf(p->eventid, sizeof(p->eventid), "%d", 100 + i);
        snprintf(p->host, sizeof(p->host), "host-%d", i);
        snprintf(p->name, sizeof(p->name), "Problem %d", i);
        p->severity = (int8_t)(i == 0 ? 4 : 2);
        p->acknowledged = i % 2 == 1;
        p->clock = 1790000000 - 600;
    }
    rx.problems.count = n;
    rx.problems.total = total;
    rx.problems.total_exact = true;
    zbx_problem_set_count(&rx.problems);
    if (total > n) {
        rx.problems.sev_count[4] = 1;
        rx.problems.sev_count[2] = total - 1;
        rx.problems.sev_exact = true;
        rx.problems.unacknowledged = total / 2;
    }
    rx.problems.ref_clock = 1790000000;
    memset(&msg, 0, sizeof(msg));
    msg.kind = ZBX_RX_PROBLEMS;
    zabbix_model_apply(m, &msg, &rx, now);
}

static void apply_hosts(struct zabbix_model *m, int total, int fetched, int down, int64_t now)
{
    struct zbx_rx_msg msg;

    memset(&rx.hosts, 0, sizeof(rx.hosts));
    rx.hosts.count = fetched < ZBX_HOST_MAX ? fetched : ZBX_HOST_MAX;
    rx.hosts.total = total;
    rx.hosts.total_exact = true;
    rx.hosts.fetched = fetched;
    rx.hosts.down = down;
    rx.hosts.unknown = 1;
    rx.hosts.maintenance = 2;
    memset(&msg, 0, sizeof(msg));
    msg.kind = ZBX_RX_HOSTS;
    zabbix_model_apply(m, &msg, &rx, now);
}

static struct zabbix_model *fresh(void)
{
    static struct zabbix_model m;
    struct zbx_rx_msg msg;

    zabbix_model_init(&m);
    zabbix_model_helper_started(&m, 1000);
    memset(&msg, 0, sizeof(msg));
    msg.kind = ZBX_RX_CONFIG;
    snprintf(msg.label, sizeof(msg.label), "Production");
    snprintf(msg.url, sizeof(msg.url), "https://zabbix.example.com/zabbix/");
    snprintf(msg.word, sizeof(msg.word), "token");
    msg.verify = true;
    msg.refresh_s = 30;
    msg.hosts_s = 60;
    zabbix_model_apply(&m, &msg, &rx, 1000);
    memset(&msg, 0, sizeof(msg));
    msg.kind = ZBX_RX_VERSION;
    snprintf(msg.word, sizeof(msg.word), "7.0.31");
    zabbix_model_apply(&m, &msg, &rx, 1000);
    return &m;
}

static void test_banner_and_stale(void)
{
    struct zabbix_model *m = fresh();
    struct zabbix_banner b;
    char cap[64];

    zabbix_view_banner(m, 1500, &b);
    check("connecting: a quiet banner naming the server", b.show && b.tone == ZABBIX_TONE_QUIET &&
                                                              strstr(b.text, "Connecting to Production"));
    zabbix_view_caption(m, 1500, cap, sizeof(cap));
    check("connecting: the caption says so", strcmp(cap, "CONNECTING") == 0);

    apply_state(m, ZBX_CONN_ONLINE, ZBX_ERR_NONE, 0, 0, "", 2000);
    apply_problems(m, 3, 3, 2000);
    apply_hosts(m, 36, 36, 2, 2000);
    zabbix_view_banner(m, 20000, &b);
    check("online and fresh: no banner", !b.show);
    check("online and fresh: not stale", !zabbix_problems_stale(m, 20000) && !zabbix_hosts_stale(m, 20000));
    zabbix_view_caption(m, 20000, cap, sizeof(cap));
    check("online: the caption is the age", strcmp(cap, "18s ago") == 0);

    check("problems turn stale after twice their interval plus grace",
          !zabbix_problems_stale(m, 2000 + 60000 + ZABBIX_STALE_GRACE_MS) &&
              zabbix_problems_stale(m, 2000 + 60000 + ZABBIX_STALE_GRACE_MS + 1));
    zabbix_view_banner(m, 2000 + 200000, &b);
    check("online but late: a warning with the age", b.show && b.tone == ZABBIX_TONE_WARN &&
                                                         strstr(b.text, "Data is late") &&
                                                         strstr(b.text, "3m ago"));

    apply_state(m, ZBX_CONN_RETRYING, ZBX_ERR_TIMEOUT, 2, 10, "Operation timed out", 30000);
    check("retrying: stale at once", zabbix_problems_stale(m, 30001) && zabbix_hosts_stale(m, 30001));
    zabbix_view_banner(m, 34000, &b);
    check("retrying: an error banner with the reason, the countdown and the age",
          b.show && b.tone == ZABBIX_TONE_ERROR && strstr(b.text, "did not answer in time") &&
              strstr(b.text, "retry in 6s") && strstr(b.text, "showing data from 32s ago"));
    zabbix_view_caption(m, 34000, cap, sizeof(cap));
    check("retrying: OFFLINE", strcmp(cap, "OFFLINE") == 0);
    check("retrying: the data is still there", m->problems.count == 3 && m->hosts.count == 36);

    apply_state(m, ZBX_CONN_CONNECTING, ZBX_ERR_TIMEOUT, 2, 0, "", 40000);
    zabbix_view_banner(m, 40000, &b);
    check("reconnecting: a warning, still stale", b.show && b.tone == ZABBIX_TONE_WARN &&
                                                     strstr(b.text, "Reconnecting (try 3)") &&
                                                     zabbix_problems_stale(m, 40000));
    apply_state(m, ZBX_CONN_ONLINE, ZBX_ERR_NONE, 0, 0, "", 41000);
    apply_problems(m, 3, 3, 41000);
    check("online again: fresh", !zabbix_problems_stale(m, 42000));
    check("the last error is remembered after recovery", m->last_err == ZBX_ERR_TIMEOUT &&
                                                             strcmp(m->last_err_text, "Operation timed out") == 0);

    apply_state(m, ZBX_CONN_AUTH_FAILED, ZBX_ERR_AUTH, 1, 300, "Not authorized.", 50000);
    zabbix_view_banner(m, 50000, &b);
    check("auth failed: says to check the token", b.show && b.tone == ZABBIX_TONE_ERROR &&
                                                      strstr(b.text, "check the API token") &&
                                                      !strstr(b.text, "REFRESH NOW"));
    snprintf(m->auth, sizeof(m->auth), "password");
    zabbix_view_banner(m, 50000, &b);
    check("auth failed with a password: says how to try again (never retried on its own)",
          b.show && b.tone == ZABBIX_TONE_ERROR && strstr(b.text, "check the user and password") &&
              strstr(b.text, "REFRESH NOW to try again") && !strstr(b.text, "retry in"));
    snprintf(m->auth, sizeof(m->auth), "token");

    zabbix_model_helper_stopped(m, ZABBIX_EXIT_CRASHED, 139, 60000);
    zabbix_view_banner(m, 60500, &b);
    check("helper crashed: says so, and when it restarts",
          b.show && b.tone == ZABBIX_TONE_ERROR && strstr(b.text, "helper stopped (crashed)") &&
              strstr(b.text, "restarting in 2s"));
    check("helper stopped: stale", zabbix_problems_stale(m, 60500));

    m = fresh();
    apply_state(m, ZBX_CONN_UNCONFIGURED, ZBX_ERR_CONFIG, 0, 0, "No Zabbix server is set up", 1000);
    zabbix_view_banner(m, 1000, &b);
    check("unconfigured: no banner (OVERVIEW shows the setup)", !b.show);
    zabbix_view_caption(m, 1000, cap, sizeof(cap));
    check("unconfigured: NOT SET UP", strcmp(cap, "NOT SET UP") == 0);
    check("nothing is stale when there is nothing", !zabbix_problems_stale(m, 999999));
}

static void test_overview(void)
{
    struct zabbix_model *m = fresh();
    struct zabbix_overview_view v;

    zabbix_view_overview(m, 1000, &v);
    check("before data: loading, --", v.loading && strcmp(v.headline, "--") == 0 && !v.setup);

    apply_state(m, ZBX_CONN_ONLINE, ZBX_ERR_NONE, 0, 0, "", 2000);
    apply_problems(m, 0, 0, 2000);
    apply_hosts(m, 24, 24, 0, 2000);
    zabbix_view_overview(m, 3000, &v);
    check("zero problems: ALL CLEAR in the OK tone", strcmp(v.headline, "ALL CLEAR") == 0 &&
                                                         v.headline_tone == ZABBIX_TONE_OK &&
                                                         strcmp(v.problems, "0 open · 0 unacknowledged") == 0);
    check("no host down: the hosts line in the OK tone", v.hosts_tone == ZABBIX_TONE_OK &&
                                                             strcmp(v.hosts, "24 monitored · 0 down") == 0);
    check("server and version", strcmp(v.server, "Production · Zabbix 7.0.31") == 0);
    check("updated", strcmp(v.updated, "Updated 1s ago") == 0);

    apply_problems(m, 3, 3, 4000);
    apply_hosts(m, 36, 36, 2, 4000);
    zabbix_view_overview(m, 4000, &v);
    check("problems: the highest severity leads, in the error tone",
          strcmp(v.headline, "HIGH") == 0 && v.headline_tone == ZABBIX_TONE_ERROR &&
              strstr(v.headline_note, "1 at this severity, of 3 open"));
    check("problems: counts", strcmp(v.problems, "3 open · 2 unacknowledged") == 0 && v.sev_count[4] == 1 &&
                                  v.sev_count[2] == 2 && v.sev_exact);
    check("hosts down: the hosts line in the error tone", v.hosts_tone == ZABBIX_TONE_ERROR &&
                                                              strcmp(v.hosts, "36 monitored · 2 down") == 0 &&
                                                              strcmp(v.hosts_note, "1 unknown · 2 in maintenance") == 0);

    apply_problems(m, 100, 1734, 5000);
    apply_hosts(m, 1500, 500, 26, 5000);
    zabbix_view_overview(m, 5000, &v);
    check("very many: exact totals with grouped digits", strcmp(v.problems, "1 734 open · 867 unacknowledged") == 0);
    check("very many hosts: down is at least, and says over which",
          strcmp(v.hosts, "1 500 monitored · >=26 down") == 0 && strstr(v.hosts_note, "(of the hosts read)"));
    m->hosts.down_exact = true;
    zabbix_view_overview(m, 5000, &v);
    check("very many hosts, down read from the server's own list: exact",
          strcmp(v.hosts, "1 500 monitored · 26 down") == 0);
    {
        char t[96];

        zabbix_view_problems_title(m, t, sizeof(t));
        check("problem list title: 100 of 1 734", strcmp(t, "100 of 1 734 open · most severe first") == 0);
        zabbix_view_hosts_title(m, t, sizeof(t));
        check("host list title: 200 of 1 500", strcmp(t, "200 of 1 500 · attention first") == 0);
    }

    m = fresh();
    apply_state(m, ZBX_CONN_UNCONFIGURED, ZBX_ERR_CONFIG, 0, 0, "", 1000);
    zabbix_view_overview(m, 1000, &v);
    check("unconfigured: the setup panel", v.setup);
}

static void test_rows(void)
{
    struct zbx_problem p;
    struct zbx_host h;
    struct zbx_item it;
    struct zabbix_problem_row pr;
    struct zabbix_host_row hr;
    struct zabbix_item_row ir;

    memset(&p, 0, sizeof(p));
    p.severity = 5;
    p.clock = 1790000000 - 3 * 3600 - 12 * 60;
    p.acknowledged = true;
    snprintf(p.host, sizeof(p.host), "edge-osl-01");
    snprintf(p.name, sizeof(p.name), "Unavailable by ICMP ping");
    zabbix_view_problem(&p, 1790000000, &pr);
    check("problem row: word, tone, age and ACK", strcmp(pr.severity, "DISASTER") == 0 &&
                                                     pr.tone == ZABBIX_TONE_ERROR &&
                                                     strcmp(pr.meta, "3h 12m · ACK") == 0 &&
                                                     strcmp(pr.host, "edge-osl-01") == 0);
    p.acknowledged = false;
    p.severity = 1;
    p.host[0] = '\0';
    zabbix_view_problem(&p, 0, &pr);
    check("problem row: no server time, no host", strcmp(pr.meta, "--") == 0 &&
                                                     strcmp(pr.host, "(no host)") == 0 &&
                                                     pr.tone == ZABBIX_TONE_QUIET &&
                                                     strcmp(pr.severity, "INFO") == 0);
    check("tones: average is a warning, not classified quiet, none OK",
          zabbix_severity_tone(3) == ZABBIX_TONE_WARN && zabbix_severity_tone(0) == ZABBIX_TONE_QUIET &&
              zabbix_severity_tone(-1) == ZABBIX_TONE_OK && zabbix_severity_tone(4) == ZABBIX_TONE_ERROR);

    memset(&h, 0, sizeof(h));
    snprintf(h.name, sizeof(h.name), "db-osl-02");
    h.avail = ZBX_AVAIL_DOWN;
    h.problems = 2;
    h.max_severity = 4;
    zabbix_view_host(&h, &hr);
    check("host row: DOWN in the error tone, its problems", strcmp(hr.avail, "DOWN") == 0 &&
                                                               hr.avail_tone == ZABBIX_TONE_ERROR &&
                                                               strcmp(hr.problems, "2 problems · HIGH") == 0 &&
                                                               hr.problems_tone == ZABBIX_TONE_ERROR);
    h.avail = ZBX_AVAIL_UP;
    h.problems = 0;
    h.max_severity = -1;
    h.maintenance = true;
    zabbix_view_host(&h, &hr);
    check("host row: in maintenance, no problems", hr.maintenance && hr.avail_tone == ZABBIX_TONE_QUIET &&
                                                       strcmp(hr.problems, "no problems") == 0);
    h.problems = 1;
    h.max_severity = 2;
    h.maintenance = false;
    zabbix_view_host(&h, &hr);
    check("host row: one problem, singular", strcmp(hr.problems, "1 problem · WARNING") == 0 &&
                                                 hr.avail_tone == ZABBIX_TONE_OK);

    memset(&it, 0, sizeof(it));
    snprintf(it.name, sizeof(it.name), "CPU utilization");
    snprintf(it.value, sizeof(it.value), "23.4 %%");
    it.clock = 1790000000 - 45;
    zabbix_view_item(&it, 1790000000, &ir);
    check("item row", strcmp(ir.value, "23.4 %") == 0 && strcmp(ir.age, "45s") == 0);
    it.value[0] = '\0';
    zabbix_view_item(&it, 1790000000, &ir);
    check("an empty value shows --", strcmp(ir.value, "--") == 0);
}

static void test_status(void)
{
    struct zabbix_model *m = fresh();
    struct zabbix_status_view v;
    int i;
    bool token_word = false;
    bool enc_ok = false;

    apply_state(m, ZBX_CONN_ONLINE, ZBX_ERR_NONE, 0, 0, "", 2000);
    zabbix_view_status(m, 5000, &v);
    for (i = 0; i < v.count; i++) {
        if (strcmp(v.key[i], "SIGN-IN") == 0 && strstr(v.value[i], "never shown")) {
            token_word = true;
        }
        if (strcmp(v.key[i], "ENCRYPTION") == 0 && v.tone[i] == ZABBIX_TONE_OK) {
            enc_ok = true;
        }
    }
    check("status: the token is named, never shown", token_word);
    check("status: HTTPS with the certificate checked, in the OK tone", enc_ok);
    check("status: last success", strcmp(v.key[7], "LAST SUCCESS") == 0 && strcmp(v.value[7], "3s ago") == 0);
    check("status: no error yet", strcmp(v.value[8], "none") == 0);

    m->verify = false;
    zabbix_view_status(m, 5000, &v);
    check("status: certificate not checked is a warning", strcmp(v.key[4], "ENCRYPTION") == 0 &&
                                                              v.tone[4] == ZABBIX_TONE_WARN);
    snprintf(m->url, sizeof(m->url), "http://10.0.0.5/zabbix/");
    zabbix_view_status(m, 5000, &v);
    check("status: plain http is an error", v.tone[4] == ZABBIX_TONE_ERROR);

    apply_state(m, ZBX_CONN_RETRYING, ZBX_ERR_TLS, 1, 5, "SSL certificate problem", 6000);
    zabbix_view_status(m, 10000, &v);
    check("status: the last error, when, and its detail",
          strcmp(v.key[8], "LAST ERROR") == 0 && strstr(v.value[8], "Secure connection failed · 4s ago") &&
              strcmp(v.key[9], "DETAIL") == 0 && strcmp(v.value[9], "SSL certificate problem") == 0);
    check("status: never more lines than it has room for", v.count <= ZABBIX_STATUS_LINES);
    {
        char s[24];

        zabbix_format_count(s, sizeof(s), 1234567);
        check("digits group in threes", strcmp(s, "1 234 567") == 0);
        zabbix_format_count(s, sizeof(s), 999);
        check("three digits stay together", strcmp(s, "999") == 0);
    }
}

int main(void)
{
    test_banner_and_stale();
    test_overview();
    test_rows();
    test_status();
    printf("zabbix_view_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
