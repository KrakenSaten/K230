/*
 * The helper's Zabbix connection against the fake server, on a clock the
 * test owns: the schedule, every failure and what follows it, the version
 * rules, the password session, and what the app is sent - read back through
 * the app's own protocol reader.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketlog/pocketlog.h"
#include "zabbix/zbx_api.h"
#include "zabbix/zbx_client.h"
#include "zabbix/zbx_fake.h"

#include <stdio.h>
#include <stdlib.h>
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

/* ---- the harness ------------------------------------------------------------------- */

struct harness {
    struct zbx_fake fake;
    struct zbx_transport inner;
    struct zbx_transport tr;
    struct zbx_client c;
    struct zbx_config cfg;
    int64_t now;
    int requests;
    int versions;               /* apiinfo.version requests */
    int logins;
    int counts;                 /* countOutput requests */
    int bearer_on_version;
    int auth_on_version;
    int bearer_requests;
    int body_auth_requests;
    int expire_session;         /* answer this many session requests "Session terminated" */
    char last_bearer[64];

    FILE *out;
    char *buf;
    size_t len;
    size_t read_pos;
    struct zbx_rx rx;
    int sets_problems;
    int sets_hosts;
    int sets_detail;
    int detail_none;
    int states;
    int bad;
    int busy;
    int idle;
    struct zbx_rx_msg last_state;
    char version[ZBX_VERSION_MAX];
    int empty_problem_sets_after_first;
};

static int64_t clock_of(void *user)
{
    return ((struct harness *)user)->now;
}

static enum zbx_err wrap_post(struct zbx_transport *t, const struct zbx_http_req *req,
                              struct zbx_http_resp *resp)
{
    struct harness *h = t->ctx;
    bool version = strstr(req->body, "\"apiinfo.version\"") != NULL;

    h->requests++;
    h->now += 40; /* every request takes a little time */
    if (version) {
        h->versions++;
        h->bearer_on_version += req->bearer != NULL;
        h->auth_on_version += strstr(req->body, "\"auth\"") != NULL;
    }
    if (strstr(req->body, "\"user.login\"")) {
        h->logins++;
    }
    if (strstr(req->body, "\"countOutput\"")) {
        h->counts++;
    }
    if (req->bearer) {
        h->bearer_requests++;
        snprintf(h->last_bearer, sizeof(h->last_bearer), "%s", req->bearer);
    }
    if (strstr(req->body, "\"auth\"")) {
        h->body_auth_requests++;
    }
    if (h->expire_session > 0 && req->bearer && !version) {
        h->expire_session--;
        memset(resp, 0, sizeof(*resp));
        resp->date = -1;
        resp->status = 200;
        resp->body = strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":"
                            "\"Invalid params.\",\"data\":\"Session terminated, re-login, please.\"},"
                            "\"id\":null}");
        resp->len = strlen(resp->body);
        return ZBX_ERR_NONE;
    }
    return h->inner.post(&h->inner, req, resp);
}

static void wrap_close(struct zbx_transport *t)
{
    (void)t;
}

/* Read everything the client wrote since last time. */
static void drain(struct harness *h)
{
    fflush(h->out);
    while (h->read_pos < h->len) {
        char *start = h->buf + h->read_pos;
        char *nl = memchr(start, '\n', h->len - h->read_pos);
        char line[ZBX_LINE_MAX + 1];
        struct zbx_rx_msg m;
        size_t n;

        if (!nl) {
            break;
        }
        n = (size_t)(nl - start);
        if (n > ZBX_LINE_MAX) {
            h->bad++;
        } else {
            memcpy(line, start, n);
            line[n] = '\0';
            switch (zbx_rx_line(&h->rx, line, &m)) {
            case ZBX_RX_PROBLEMS:
                if (h->sets_problems > 0 && h->rx.problems.count == 0) {
                    h->empty_problem_sets_after_first++;
                }
                h->sets_problems++;
                break;
            case ZBX_RX_HOSTS:
                h->sets_hosts++;
                break;
            case ZBX_RX_DETAIL:
                h->sets_detail++;
                break;
            case ZBX_RX_DETAIL_NONE:
                h->detail_none++;
                break;
            case ZBX_RX_STATE:
                h->states++;
                h->last_state = m;
                break;
            case ZBX_RX_VERSION:
                snprintf(h->version, sizeof(h->version), "%.15s", m.word);
                break;
            case ZBX_RX_BUSY:
                h->busy++;
                break;
            case ZBX_RX_IDLE:
                h->idle++;
                break;
            case ZBX_RX_BAD:
                h->bad++;
                break;
            default:
                break;
            }
        }
        h->read_pos += n + 1;
    }
}

/* A live-looking configuration (not the fake mode: the client must take its
 * real paths), over the fake server. */
static struct harness *start(const char *scenario, enum zbx_auth auth, const char *secret)
{
    struct harness *h = calloc(1, sizeof(*h));

    zbx_fake_init(&h->fake, scenario);
    h->fake.realtime = false;
    zbx_transport_fake(&h->inner, &h->fake);
    h->tr.ctx = h;
    h->tr.post = wrap_post;
    h->tr.close = wrap_close;
    h->tr.name = "test";
    zbx_config_defaults(&h->cfg);
    h->cfg.configured = true;
    snprintf(h->cfg.url, sizeof(h->cfg.url), "https://zabbix.test/api_jsonrpc.php");
    snprintf(h->cfg.url_shown, sizeof(h->cfg.url_shown), "https://zabbix.test/");
    snprintf(h->cfg.label, sizeof(h->cfg.label), "Test");
    h->cfg.https = true;
    h->cfg.auth = auth;
    if (auth == ZBX_AUTH_PASSWORD) {
        snprintf(h->cfg.user, sizeof(h->cfg.user), "%s", ZBX_FAKE_USER);
    }
    if (secret) {
        snprintf(h->cfg.secret, sizeof(h->cfg.secret), "%s", secret);
        h->cfg.have_secret = true;
    }
    h->now = 1000000;
    h->out = open_memstream(&h->buf, &h->len);
    zbx_rx_init(&h->rx);
    zbx_client_init(&h->c, &h->cfg, &h->tr, h->out, clock_of, h);
    zbx_client_start(&h->c);
    drain(h);
    return h;
}

static int step(struct harness *h)
{
    int wait = zbx_client_step(&h->c);

    drain(h);
    return wait;
}

static void finish(struct harness *h)
{
    zbx_client_free(&h->c);
    fclose(h->out);
    free(h->buf);
    free(h);
}

static void cmd(struct harness *h, enum zbx_cmd_kind kind, const char *word)
{
    struct zbx_cmd c;

    memset(&c, 0, sizeof(c));
    c.kind = kind;
    snprintf(c.word, sizeof(c.word), "%s", word ? word : "");
    zbx_client_command(&h->c, &c);
}

/* ---- the tests ----------------------------------------------------------------------- */

static void test_healthy_round(void)
{
    struct harness *h = start("demo", ZBX_AUTH_TOKEN, "tok-123");
    int wait;

    check("demo: the first state line says connecting", h->last_state.state == ZBX_CONN_CONNECTING);
    wait = step(h);
    check("demo: online after one step", h->c.state == ZBX_CONN_ONLINE &&
                                             h->last_state.state == ZBX_CONN_ONLINE &&
                                             h->last_state.err == ZBX_ERR_NONE);
    check("demo: the server's version was reported", strcmp(h->version, "7.0.31") == 0);
    check("demo: one problem set and one host set", h->sets_problems == 1 && h->sets_hosts == 1);
    check("demo: nothing the app could not read", h->bad == 0);
    check("demo: nine problems, the disaster first", h->rx.problems.count == 9 &&
                                                         h->rx.problems.p[0].severity == 5 &&
                                                         strcmp(h->rx.problems.p[0].host, "edge-osl-01") == 0);
    check("demo: every problem knows its host", h->rx.problems.p[8].host[0] != '\0');
    check("demo: counts exact, six unacknowledged", h->rx.problems.total == 9 &&
                                                        h->rx.problems.total_exact &&
                                                        h->rx.problems.sev_exact &&
                                                        h->rx.problems.unacknowledged == 6);
    check("demo: 36 hosts, two down, one unknown, one in maintenance",
          h->rx.hosts.count == 36 && h->rx.hosts.total == 36 && h->rx.hosts.down == 2 &&
              h->rx.hosts.unknown == 1 && h->rx.hosts.maintenance == 1);
    check("demo: the host with the disaster leads the host list",
          strcmp(h->rx.hosts.h[0].name, "edge-osl-01") == 0 && h->rx.hosts.h[0].problems == 1 &&
              h->rx.hosts.h[0].max_severity == 5);
    check("demo: apiinfo.version went without a token, in the header or the body",
          h->versions == 1 && h->bearer_on_version == 0 && h->auth_on_version == 0);
    check("demo: every other request carried it in the header",
          h->bearer_requests == h->requests - 1 && h->body_auth_requests == 0 &&
              strcmp(h->last_bearer, "tok-123") == 0);
    check("demo: no count requests for a small estate", h->counts == 1); /* the host total */
    check("demo: busy and idle were said", h->busy >= 3 && h->idle == 1);
    check("demo: next wake in refresh_s", wait == 30000 - 0 || (wait > 29000 && wait <= 30000));
    finish(h);
}

static void test_schedule(void)
{
    struct harness *h = start("demo", ZBX_AUTH_TOKEN, "tok");
    int before;

    step(h);
    before = h->requests;
    h->now += 29000;
    step(h);
    check("nothing is asked before refresh_s", h->requests == before);
    h->now += 1200;
    step(h);
    check("problems at refresh_s, hosts not yet", h->sets_problems == 2 && h->sets_hosts == 2 &&
                                                      h->requests == before + 2);
    check("(the host set went again because its counts follow the problems)", h->sets_hosts == 2);
    check("no second version request while online", h->versions == 1);
    h->now += 31000;
    before = h->requests;
    step(h);
    /* problems (problem.get, trigger.get) and hosts (count, the unavailable
     * interfaces, the hosts that matter by id, the rest by name) */
    check("hosts at hosts_s", h->requests == before + 6);
    before = h->requests;
    h->now += 1000;
    cmd(h, ZBX_CMD_REFRESH, NULL);
    step(h);
    check("refresh: everything at once", h->requests == before + 6);
    before = h->requests;
    h->now += 1000;
    cmd(h, ZBX_CMD_REFRESH, NULL);
    step(h);
    check("a second refresh within 5 s is ignored", h->requests == before);
    h->now += 5000;
    cmd(h, ZBX_CMD_REFRESH, NULL);
    step(h);
    check("and taken after it", h->requests > before);
    finish(h);
}

static void test_auth(void)
{
    struct harness *h = start("auth", ZBX_AUTH_TOKEN, "wrong");
    int before;
    int wait;

    wait = step(h);
    check("auth: AUTH_FAILED", h->c.state == ZBX_CONN_AUTH_FAILED &&
                                   h->last_state.state == ZBX_CONN_AUTH_FAILED &&
                                   h->last_state.err == ZBX_ERR_AUTH);
    check("auth: the next try is in five minutes (the helper still wakes each minute)",
          h->last_state.retry_s == 300 && wait == 60000);
    check("auth: the server's words are kept", strstr(h->last_state.text, "Not authorized") != NULL);
    check("auth: no data was sent", h->sets_problems == 0 && h->sets_hosts == 0);
    before = h->requests;
    h->now += 200000;
    step(h);
    check("auth: nothing asked before then", h->requests == before);
    h->now += 101000;
    step(h);
    check("auth: tried again after five minutes", h->requests > before &&
                                                     h->c.state == ZBX_CONN_AUTH_FAILED);
    before = h->requests;
    h->now += 10000;
    cmd(h, ZBX_CMD_REFRESH, NULL);
    step(h);
    check("auth: refresh tries at once", h->requests > before);
    zbx_fake_set_scenario(&h->fake, "demo");
    h->now += 10000;
    cmd(h, ZBX_CMD_REFRESH, NULL);
    step(h);
    check("auth: a fixed server comes online", h->c.state == ZBX_CONN_ONLINE && h->sets_problems == 1);
    finish(h);

    h = start("expired", ZBX_AUTH_TOKEN, "old");
    step(h);
    check("an expired token is AUTH_FAILED too", h->c.state == ZBX_CONN_AUTH_FAILED &&
                                                     strstr(h->last_state.text, "expired") != NULL);
    finish(h);

    h = start("demo", ZBX_AUTH_TOKEN, NULL);
    step(h);
    check("no token stored: AUTH_FAILED without asking the server",
          h->c.state == ZBX_CONN_AUTH_FAILED && h->requests == 0 &&
              strstr(h->last_state.text, "No API token stored") != NULL);
    finish(h);
}

static void test_backoff(void)
{
    struct harness *h = start("timeout", ZBX_AUTH_TOKEN, "tok");
    const int expect[] = { 5, 10, 20, 40, 80, 120, 120 };
    bool ok = true;
    bool quiet = true;
    int i;

    for (i = 0; i < 7; i++) {
        int before;

        step(h);
        if (h->c.state != ZBX_CONN_RETRYING || h->last_state.retry_s != expect[i] ||
            h->last_state.attempt != i + 1 || h->last_state.err != ZBX_ERR_TIMEOUT) {
            ok = false;
            printf("     attempt %d: state %d retry %d err %d\n", i + 1, h->c.state,
                   h->last_state.retry_s, h->last_state.err);
        }
        before = h->requests;
        h->now += (int64_t)expect[i] * 1000 - 500;
        step(h);
        if (h->requests != before) {
            quiet = false;
        }
        h->now += 600;
    }
    check("timeout: backoff 5, 10, 20, 40, 80, 120, 120 s", ok);
    check("timeout: nothing asked inside a backoff", quiet);
    check("timeout: the fixed sequence", zbx_backoff_s(1) == 5 && zbx_backoff_s(3) == 20 &&
                                             zbx_backoff_s(50) == 120);
    finish(h);
}

static void test_flap_and_stale(void)
{
    struct harness *h = start("flap", ZBX_AUTH_TOKEN, "tok");
    int online = 0;
    int retrying = 0;
    int i;

    for (i = 0; i < 60; i++) {
        step(h);
        if (h->c.state == ZBX_CONN_ONLINE) {
            online++;
        } else if (h->c.state == ZBX_CONN_RETRYING) {
            retrying++;
        }
        h->now += 31000;
    }
    check("flap: it went down and came back, more than once", online > 5 && retrying > 5);
    check("flap: data was kept, never replaced by an empty set",
          h->empty_problem_sets_after_first == 0 && h->rx.problems.count == 9);
    check("flap: the connection was re-checked after each failure", h->versions > 3);
    check("flap: nothing unreadable", h->bad == 0);
    finish(h);

    h = start("drop", ZBX_AUTH_TOKEN, "tok");
    step(h);
    check("drop: online first", h->c.state == ZBX_CONN_ONLINE && h->sets_problems == 1);
    for (i = 0; i < 10; i++) {
        h->now += 130000;
        step(h);
    }
    check("drop: then retrying for good", h->c.state == ZBX_CONN_RETRYING &&
                                              h->last_state.err == ZBX_ERR_CONNECT &&
                                              h->last_state.attempt >= 5);
    check("drop: the last good data is still what the app holds (stale, not blank)",
          h->rx.problems.count == 9 && h->rx.hosts.count == 36);
    finish(h);
}

static void test_bad_answers(void)
{
    static const struct {
        const char *scenario;
        enum zbx_err err;
    } cases[] = {
        { "malformed", ZBX_ERR_MALFORMED }, { "apierror", ZBX_ERR_API }, { "http500", ZBX_ERR_HTTP },
        { "huge", ZBX_ERR_TOO_LARGE },      { "refused", ZBX_ERR_CONNECT }, { "dns", ZBX_ERR_DNS },
        { "tls", ZBX_ERR_TLS },
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct harness *h = start(cases[i].scenario, ZBX_AUTH_TOKEN, "tok");
        char name[96];

        step(h);
        snprintf(name, sizeof(name), "%s: retrying with %s", cases[i].scenario,
                 zbx_err_word(cases[i].err));
        check(name, h->c.state == ZBX_CONN_RETRYING && h->last_state.err == cases[i].err &&
                        h->last_state.retry_s == 5 && h->last_state.text[0]);
        finish(h);
    }
    {
        struct harness *h = start("malformed", ZBX_AUTH_TOKEN, "tok");

        step(h);
        h->now += 6000;
        step(h);
        check("malformed: the server is still there, so the version is not asked again",
              h->versions == 1 && h->last_state.attempt == 2);
        finish(h);
    }
}

static void test_versions(void)
{
    struct harness *h = start("old", ZBX_AUTH_TOKEN, "tok60");

    step(h);
    check("6.0: online", h->c.state == ZBX_CONN_ONLINE && strcmp(h->version, "6.0.48") == 0);
    check("6.0: the token went in the body, never in a header",
          h->bearer_requests == 0 && h->body_auth_requests == h->requests - 1);
    check("6.0: host.get did not ask for active_available (it would have been refused)",
          h->sets_hosts == 1);
    finish(h);

    h = start("v74", ZBX_AUTH_TOKEN, "tok74");
    step(h);
    check("7.4: online, the header only, apiinfo.version bare",
          h->c.state == ZBX_CONN_ONLINE && h->body_auth_requests == 0 && h->bearer_on_version == 0);
    finish(h);
}

static void test_large(void)
{
    struct harness *h = start("large", ZBX_AUTH_TOKEN, "tok");
    int i;
    bool sorted = true;

    step(h);
    check("large: online", h->c.state == ZBX_CONN_ONLINE && h->bad == 0);
    check("large: 100 problems kept of 1 200, exact totals from the server",
          h->rx.problems.count == ZBX_PROBLEM_MAX && h->rx.problems.total == 1200 &&
              h->rx.problems.total_exact && h->rx.problems.sev_exact &&
              h->rx.problems.unacknowledged == 800);
    check("large: the per-severity counts add up", h->rx.problems.sev_count[0] + h->rx.problems.sev_count[1] +
                                                           h->rx.problems.sev_count[2] + h->rx.problems.sev_count[3] +
                                                           h->rx.problems.sev_count[4] + h->rx.problems.sev_count[5] ==
                                                       1200);
    check("large: eight count requests on top (total, six severities, unacknowledged, hosts)",
          h->counts == 9);
    for (i = 1; i < h->rx.problems.count; i++) {
        if (h->rx.problems.p[i].severity > h->rx.problems.p[i - 1].severity) {
            sorted = false;
        }
    }
    check("large: the kept ones are the most severe", sorted && h->rx.problems.p[0].severity == 5);
    check("large: 200 hosts kept of 1 500, 500 counted",
          h->rx.hosts.count == ZBX_HOST_MAX && h->rx.hosts.total == 1500 && h->rx.hosts.fetched == 500);
    check("large: every down host counted, from the server's own list (38, exact)",
          h->rx.hosts.down == 38 && h->rx.hosts.down_exact);
    {
        bool found = false;

        for (i = 0; i < h->rx.hosts.count; i++) {
            if (strncmp(h->rx.hosts.h[i].name, "very-long-hostname", 18) == 0 &&
                h->rx.hosts.h[i].max_severity == 5) {
                found = true;
            }
        }
        check("large: a host with a disaster is kept however late its name sorts", found);
    }
    {
        struct zbx_overview o;

        zbx_overview_build(&o, &h->rx.problems, &h->rx.hosts);
        check("large: the overview says the host counts are partial", !o.hosts_exact && o.problems_exact);
    }
    finish(h);

    h = start("empty", ZBX_AUTH_TOKEN, "tok");
    step(h);
    check("empty: online with two empty sets", h->c.state == ZBX_CONN_ONLINE && h->sets_problems == 1 &&
                                                   h->rx.problems.count == 0 && h->rx.hosts.count == 0 &&
                                                   h->rx.hosts.total == 0);
    finish(h);
}

static void test_detail(void)
{
    struct harness *h = start("demo", ZBX_AUTH_TOKEN, "tok");

    step(h);
    cmd(h, ZBX_CMD_DETAIL, "10109");
    step(h);
    check("detail: one host's detail arrives at once", h->sets_detail == 1 &&
                                                           strcmp(h->rx.detail.host.name, "db-osl-02") == 0);
    check("detail: its problem and its values", h->rx.detail.problem_count == 1 &&
                                                    h->rx.detail.p[0].severity == 4 &&
                                                    h->rx.detail.item_count >= 8 &&
                                                    strcmp(h->rx.detail.item[0].name, "Zabbix agent ping") == 0);
    h->now += 31000;
    step(h);
    check("detail: read again with the problems", h->sets_detail == 2);
    cmd(h, ZBX_CMD_DETAIL, "99999");
    step(h);
    check("detail: a host that is not there says so", h->detail_none == 1);
    cmd(h, ZBX_CMD_DETAIL, "");
    h->now += 31000;
    step(h);
    check("detail: closed, no more of it", h->sets_detail == 2 && h->detail_none == 1);
    finish(h);
}

static void test_password(void)
{
    struct harness *h = start("demo", ZBX_AUTH_PASSWORD, "pw");

    step(h);
    check("password: logged in once, online", h->logins == 1 && h->c.state == ZBX_CONN_ONLINE);
    check("password: the session went in the header", strcmp(h->last_bearer, "pw") != 0 &&
                                                          h->last_bearer[0] != '\0');
    h->expire_session = 1;
    h->now += 31000;
    step(h);
    check("password: an expired session is logged in again, silently",
          h->logins == 2 && h->c.state == ZBX_CONN_ONLINE && h->sets_problems == 2);
    h->expire_session = 5;
    h->now += 31000;
    step(h);
    check("password: a session refused straight after login is a real failure",
          h->c.state == ZBX_CONN_AUTH_FAILED && h->logins == 3);
    finish(h);

    h = start("auth", ZBX_AUTH_PASSWORD, "wrong");
    step(h);
    check("password: a refused login is AUTH_FAILED", h->c.state == ZBX_CONN_AUTH_FAILED && h->logins == 1);
    h->now += 60000;
    step(h);
    check("password: and not tried again by itself within minutes (account lockout)", h->logins == 1);
    finish(h);
}

static void test_unconfigured_and_reset(void)
{
    struct harness *h = calloc(1, sizeof(*h));
    struct zbx_config cfg;
    int wait;

    zbx_config_defaults(&cfg);
    h->out = open_memstream(&h->buf, &h->len);
    zbx_rx_init(&h->rx);
    zbx_fake_init(&h->fake, "demo");
    zbx_transport_fake(&h->inner, &h->fake);
    h->tr.ctx = h;
    h->tr.post = wrap_post;
    h->tr.close = wrap_close;
    h->now = 5000;
    zbx_client_init(&h->c, &cfg, &h->tr, h->out, clock_of, h);
    zbx_client_start(&h->c);
    drain(h);
    wait = step(h);
    check("unconfigured: says so, asks nothing, sleeps long",
          h->last_state.state == ZBX_CONN_UNCONFIGURED && h->last_state.err == ZBX_ERR_CONFIG &&
              h->requests == 0 && wait == 60000);
    finish(h);

    h = start("demo", ZBX_AUTH_TOKEN, "tok");
    step(h);
    zbx_fake_set_scenario(&h->fake, "healthy");
    zbx_client_reset(&h->c);
    drain(h);
    check("reset: connecting again", h->last_state.state == ZBX_CONN_CONNECTING);
    step(h);
    check("reset: the new server is asked from the start", h->versions == 2 && h->rx.problems.count == 0 &&
                                                               h->rx.hosts.count == 24);
    finish(h);
}

static void test_soak(void)
{
    struct harness *h = start("flap", ZBX_AUTH_TOKEN, "tok");
    int i;

    /* Hundreds of refreshes and reconnects: under the sanitizers
     * (make zabbix-san-test) this is the leak and lifetime check. */
    cmd(h, ZBX_CMD_DETAIL, "10113");
    for (i = 0; i < 400; i++) {
        step(h);
        h->now += 31000;
        if (i % 50 == 0) {
            h->now += 6000;
            cmd(h, ZBX_CMD_REFRESH, NULL);
        }
    }
    printf("     soak: %d problem sets, %d detail sets, %d bad lines, %d requests\n", h->sets_problems,
           h->sets_detail, h->bad, h->requests);
    check("soak: 400 rounds of refresh and reconnect, every line readable", h->bad == 0 &&
                                                                              h->sets_problems > 50 &&
                                                                              h->sets_detail > 25);
    finish(h);
}

int main(void)
{
    /* The client logs every state change; the checks say what matters. */
    pocketlog_set_level(POCKETLOG_ERROR);
    test_healthy_round();
    test_schedule();
    test_auth();
    test_backoff();
    test_flap_and_stale();
    test_bad_answers();
    test_versions();
    test_large();
    test_detail();
    test_password();
    test_unconfigured_and_reset();
    test_soak();
    printf("zbx_client_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
