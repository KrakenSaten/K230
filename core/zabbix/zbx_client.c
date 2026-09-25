/*
 * The helper's Zabbix connection. See zbx_client.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "zabbix/zbx_client.h"

#include "pocketlog/pocketlog.h"
#include "zabbix/zbx_api.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The fake accepts any token; this is the one it is given when the demo has
 * no secret of its own. */
#define FAKE_TOKEN "doors-demo-token"
/* A wall clock before this is a board that has not heard from NTP yet. */
#define CLOCK_VALID_AFTER 1700000000LL
#define WAKE_MAX_MS 60000

int zbx_backoff_s(int n)
{
    int s = ZBX_BACKOFF_FIRST_S;

    while (--n > 0 && s < ZBX_BACKOFF_MAX_S) {
        s *= 2;
    }
    return s > ZBX_BACKOFF_MAX_S ? ZBX_BACKOFF_MAX_S : s;
}

static int64_t now_of(struct zbx_client *c)
{
    return c->now_ms(c->clock_user);
}

int zbx_client_init(struct zbx_client *c, const struct zbx_config *cfg, struct zbx_transport *tr,
                    FILE *out, int64_t (*now_ms)(void *user), void *clock_user)
{
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;
    c->tr = tr;
    c->out = out;
    c->now_ms = now_ms;
    c->clock_user = clock_user;
    c->state = cfg->configured ? ZBX_CONN_CONNECTING : ZBX_CONN_UNCONFIGURED;
    c->problems = calloc(1, sizeof(*c->problems));
    c->hosts = calloc(1, sizeof(*c->hosts));
    c->detail = calloc(1, sizeof(*c->detail));
    c->pscratch = calloc(ZBX_PROBLEM_FETCH, sizeof(*c->pscratch));
    c->hscratch = calloc(ZBX_HOST_FETCH, sizeof(*c->hscratch));
    c->hscratch2 = calloc(ZBX_HOST_FETCH, sizeof(*c->hscratch2));
    c->hostids = calloc(ZBX_HOST_FETCH, sizeof(*c->hostids));
    c->hostid_ptrs = calloc(ZBX_HOST_FETCH, sizeof(*c->hostid_ptrs));
    if (!c->problems || !c->hosts || !c->detail || !c->pscratch || !c->hscratch || !c->hscratch2 ||
        !c->hostids || !c->hostid_ptrs) {
        zbx_client_free(c);
        return -1;
    }
    return 0;
}

void zbx_client_free(struct zbx_client *c)
{
    free(c->problems);
    free(c->hosts);
    free(c->detail);
    free(c->pscratch);
    free(c->hscratch);
    free(c->hscratch2);
    free(c->hostids);
    free(c->hostid_ptrs);
    c->hscratch2 = NULL;
    c->hostids = NULL;
    c->hostid_ptrs = NULL;
    c->problems = NULL;
    c->hosts = NULL;
    c->detail = NULL;
    c->pscratch = NULL;
    c->hscratch = NULL;
    explicit_bzero(c->session, sizeof(c->session));
    zbx_config_forget_secret(&c->cfg);
}

/* ---- reporting ------------------------------------------------------------------ */

static void report(struct zbx_client *c)
{
    int retry_s = 0;

    /* NEVER: no try is scheduled (a refused password), and 0 says so. */
    if ((c->state == ZBX_CONN_RETRYING || c->state == ZBX_CONN_AUTH_FAILED) &&
        c->next_try_ms != ZBX_TRY_NEVER) {
        int64_t left = c->next_try_ms - now_of(c);

        retry_s = left > 0 ? (int)((left + 999) / 1000) : 0;
    }
    zbx_proto_state(c->out, c->state, c->attempt, retry_s, c->err, c->err_text);
    c->reported = true;
}

static void set_state(struct zbx_client *c, enum zbx_conn_state st)
{
    if (st != c->state || !c->reported) {
        if (st != c->state) {
            LOG_INFO("zabbix: %s -> %s%s%s", zbx_conn_state_word(c->state), zbx_conn_state_word(st),
                     c->err != ZBX_ERR_NONE ? " " : "", c->err != ZBX_ERR_NONE ? zbx_err_word(c->err) : "");
        }
        c->state = st;
        report(c);
    }
}

void zbx_client_start(struct zbx_client *c)
{
    const char *auth = c->cfg.fake ? "token" : c->cfg.auth == ZBX_AUTH_PASSWORD ? "password" : "token";

    if (!c->cfg.configured) {
        auth = "none";
    }
    zbx_proto_config(c->out, c->cfg.label, c->cfg.url_shown, auth, c->cfg.verify, c->cfg.refresh_s,
                     c->cfg.hosts_s);
    if (!c->cfg.configured) {
        c->err = ZBX_ERR_CONFIG;
        snprintf(c->err_text, sizeof(c->err_text), "No Zabbix server is set up");
    }
    c->reported = false;
    set_state(c, c->state);
}

static bool clock_valid(void)
{
    return (long long)time(NULL) >= CLOCK_VALID_AFTER;
}

/* ---- one request ---------------------------------------------------------------- */

static const char *credential(const struct zbx_client *c)
{
    if (c->cfg.auth == ZBX_AUTH_PASSWORD && !c->cfg.fake) {
        return c->session[0] ? c->session : NULL;
    }
    if (c->cfg.have_secret) {
        return c->cfg.secret;
    }
    return c->cfg.fake ? FAKE_TOKEN : NULL;
}

static struct zbx_req_ctx ctx_for(struct zbx_client *c)
{
    struct zbx_req_ctx x;

    memset(&x, 0, sizeof(x));
    x.id = ++c->rpc_id;
    if (c->vnum < ZBX_API_BEARER_VERSION) {
        x.auth_in_body = credential(c);
    }
    x.active_available = c->vnum >= ZBX_API_ACTIVE_AVAIL_VERSION;
    return x;
}

static void wipe_free(char *s)
{
    if (s) {
        explicit_bzero(s, strlen(s));
        free(s);
    }
}

/* POST body (which this frees, wiped: it may hold a token or a password)
 * and read the answer to request id into r. */
static enum zbx_err call(struct zbx_client *c, char *body, int id, bool with_auth,
                         struct zbx_reply *r, long long *date)
{
    struct zbx_http_req req;
    struct zbx_http_resp resp;
    enum zbx_err err;

    memset(r, 0, sizeof(*r));
    if (!body) {
        snprintf(c->err_text, sizeof(c->err_text), "out of memory");
        return ZBX_ERR_INTERNAL;
    }
    memset(&req, 0, sizeof(req));
    req.url = c->cfg.fake ? "fake://zabbix/api_jsonrpc.php" : c->cfg.url;
    req.body = body;
    req.bearer = with_auth && c->vnum >= ZBX_API_BEARER_VERSION ? credential(c) : NULL;
    req.connect_timeout_ms = ZBX_CONNECT_TIMEOUT_MS;
    req.timeout_ms = c->timeout_override_ms > 0 ? c->timeout_override_ms : c->cfg.timeout_s * 1000;
    c->timeout_override_ms = 0;
    req.verify = c->cfg.verify;
    req.ca_file = c->cfg.ca_file;
    req.max_bytes = ZBX_RESPONSE_MAX;
    err = c->tr->post(c->tr, &req, &resp);
    wipe_free(body);
    if (err != ZBX_ERR_NONE) {
        zbx_copy_text(c->err_text, sizeof(c->err_text), resp.text[0] ? resp.text : zbx_err_text(err));
        zbx_http_resp_free(&resp);
        return err;
    }
    if (date) {
        *date = resp.date;
    }
    err = zbx_reply_parse(r, resp.body, resp.len, id);
    explicit_bzero(resp.body, resp.len);
    zbx_http_resp_free(&resp);
    if (err != ZBX_ERR_NONE) {
        zbx_copy_text(c->err_text, sizeof(c->err_text), r->text[0] ? r->text : zbx_err_text(err));
        zbx_reply_free(r);
    } else if (with_auth) {
        /* The session has served a request: from now on its refusal means
         * it ran out, not that the login did not count. */
        c->session_fresh = false;
    }
    return err;
}

static enum zbx_err shape(struct zbx_client *c, struct zbx_reply *r, const char *what)
{
    snprintf(c->err_text, sizeof(c->err_text), "%s: unexpected answer", what);
    zbx_reply_free(r);
    return ZBX_ERR_MALFORMED;
}

static enum zbx_err count_call(struct zbx_client *c, int severity, bool unack, int *out)
{
    struct zbx_req_ctx x = ctx_for(c);
    struct zbx_reply r;
    enum zbx_err err = call(c, zbx_req_problem_count(&x, severity, unack), x.id, true, &r, NULL);

    if (err != ZBX_ERR_NONE) {
        return err;
    }
    if (zbx_parse_count(r.result, out) != 0) {
        return shape(c, &r, "problem count");
    }
    zbx_reply_free(&r);
    return ZBX_ERR_NONE;
}

/* ---- connecting -------------------------------------------------------------------- */



static enum zbx_err do_connect(struct zbx_client *c)
{
    struct zbx_reply r;
    enum zbx_err err;
    char version[ZBX_VERSION_MAX];
    int id = ++c->rpc_id;

    if (!c->cfg.fake && !c->cfg.have_secret) {
        snprintf(c->err_text, sizeof(c->err_text), "No %s stored for this server",
                 c->cfg.auth == ZBX_AUTH_PASSWORD ? "password" : "API token");
        return ZBX_ERR_AUTH;
    }
    zbx_proto_busy(c->out, "version");
    c->vnum = 0; /* apiinfo.version is always asked without credentials */
    err = call(c, zbx_req_version(id), id, false, &r, NULL);
    if (err != ZBX_ERR_NONE) {
        return err;
    }
    if (zbx_parse_version(r.result, version, sizeof(version)) != 0) {
        return shape(c, &r, "apiinfo.version");
    }
    zbx_reply_free(&r);
    c->vnum = zbx_version_num(version);
    if (strcmp(version, c->version) != 0) {
        snprintf(c->version, sizeof(c->version), "%s", version);
        zbx_proto_version(c->out, c->version);
        LOG_INFO("zabbix: server API %s", c->version);
    }
    if (c->vnum < ZBX_API_MIN_VERSION) {
        snprintf(c->err_text, sizeof(c->err_text), "Zabbix %s is older than 6.0", c->version);
        return ZBX_ERR_API;
    }
    if (c->cfg.auth == ZBX_AUTH_PASSWORD && !c->cfg.fake && !c->session[0]) {
        id = ++c->rpc_id;
        zbx_proto_busy(c->out, "login");
        err = call(c, zbx_req_login(id, c->cfg.user, c->cfg.secret), id, false, &r, NULL);
        if (err != ZBX_ERR_NONE) {
            /* user.login refusing a password is an authentication failure
             * whatever code it came with. */
            return err == ZBX_ERR_API ? ZBX_ERR_AUTH : err;
        }
        if (zbx_parse_login(r.result, c->session, sizeof(c->session)) != 0) {
            return shape(c, &r, "user.login");
        }
        explicit_bzero(r.result->valuestring, strlen(r.result->valuestring));
        zbx_reply_free(&r);
        c->session_fresh = true;
        LOG_INFO("zabbix: logged in");
    }
    c->connected = true;
    return ZBX_ERR_NONE;
}

/* ---- problems ------------------------------------------------------------------------ */

static void kept_hosts_refresh(struct zbx_client *c)
{
    zbx_hosts_apply(c->hosts->h, c->hosts->count, c->pscratch, c->pscratch_n);
    zbx_hosts_sort(c->hosts->h, c->hosts->count);
}

static enum zbx_err fill_hosts(struct zbx_client *c, struct zbx_problem *p, int n)
{
    const char **ids;
    struct zbx_req_ctx x;
    struct zbx_reply r;
    enum zbx_err err;
    int k = 0;
    int i;
    int j;

    if (n == 0) {
        return ZBX_ERR_NONE;
    }
    ids = calloc((size_t)n, sizeof(*ids));
    if (!ids) {
        snprintf(c->err_text, sizeof(c->err_text), "out of memory");
        return ZBX_ERR_INTERNAL;
    }
    for (i = 0; i < n; i++) {
        for (j = 0; j < k && strcmp(ids[j], p[i].objectid) != 0; j++) {
        }
        if (j == k) {
            ids[k++] = p[i].objectid;
        }
    }
    x = ctx_for(c);
    err = call(c, zbx_req_trigger_hosts(&x, ids, k), x.id, true, &r, NULL);
    free(ids);
    if (err != ZBX_ERR_NONE) {
        return err;
    }
    if (zbx_parse_trigger_hosts(r.result, p, n) != 0) {
        return shape(c, &r, "trigger.get");
    }
    zbx_reply_free(&r);
    return ZBX_ERR_NONE;
}

static enum zbx_err sync_problems(struct zbx_client *c)
{
    struct zbx_problem_set *s = c->problems;
    struct zbx_req_ctx x = ctx_for(c);
    struct zbx_reply r;
    enum zbx_err err;
    long long date = -1;
    int n = 0;
    int i;
    bool truncated;

    zbx_proto_busy(c->out, "problems");
    err = call(c, zbx_req_problems(&x, NULL, ZBX_PROBLEM_FETCH), x.id, true, &r, &date);
    if (err != ZBX_ERR_NONE) {
        return err;
    }
    /* The answer may hold suppressed problems the parser drops, so the array
     * size is what says whether the limit was reached. */
    truncated = cJSON_GetArraySize(r.result) >= ZBX_PROBLEM_FETCH;
    if (zbx_parse_problems(r.result, c->pscratch, ZBX_PROBLEM_FETCH, &n) != 0) {
        return shape(c, &r, "problem.get");
    }
    zbx_reply_free(&r);
    if (n > ZBX_PROBLEM_FETCH) {
        n = ZBX_PROBLEM_FETCH;
    }
    c->pscratch_n = n;
    err = fill_hosts(c, c->pscratch, n);
    if (err != ZBX_ERR_NONE) {
        return err;
    }

    memset(s, 0, sizeof(*s));
    for (i = 0; i < n; i++) {
        int sev = c->pscratch[i].severity;

        if (!c->pscratch[i].acknowledged) {
            s->unacknowledged++;
        }
        if (sev >= 0 && sev < ZBX_SEV_COUNT) {
            s->sev_count[sev]++;
        }
    }
    s->total = n;
    s->total_exact = true;
    s->sev_exact = true;
    if (truncated) {
        /* More open problems than were fetched: the totals from the server,
         * one count each, so the overview is exact even when the list is
         * not. Eight small requests, only on a server this busy. */
        err = count_call(c, -1, false, &s->total);
        for (i = 0; err == ZBX_ERR_NONE && i < ZBX_SEV_COUNT; i++) {
            err = count_call(c, i, false, &s->sev_count[i]);
        }
        if (err == ZBX_ERR_NONE) {
            err = count_call(c, -1, true, &s->unacknowledged);
        }
        if (err != ZBX_ERR_NONE) {
            return err;
        }
    }
    zbx_problems_sort(c->pscratch, n);
    s->count = n < ZBX_PROBLEM_MAX ? n : ZBX_PROBLEM_MAX;
    memcpy(s->p, c->pscratch, (size_t)s->count * sizeof(s->p[0]));
    s->ref_clock = date > 0 ? date : clock_valid() ? (long long)time(NULL) : 0;
    c->seq++;
    zbx_proto_problems(c->out, c->seq, s);
    c->have_problems = true;
    if (c->have_hosts) {
        /* The per-host counts follow the problems, not the host refresh. */
        kept_hosts_refresh(c);
        c->seq++;
        zbx_proto_hosts(c->out, c->seq, c->hosts);
    }
    return ZBX_ERR_NONE;
}

/* ---- hosts ---------------------------------------------------------------------------- */

static enum zbx_err sync_hosts(struct zbx_client *c)
{
    struct zbx_host_set *s = c->hosts;
    struct zbx_req_ctx x = ctx_for(c);
    struct zbx_reply r;
    enum zbx_err err;
    long long date = -1;
    int total = 0;
    int n = 0;
    int i;

    int nids = 0;
    int seen = 0;
    bool down_exact;

    zbx_proto_busy(c->out, "hosts");
    err = call(c, zbx_req_host_count(&x), x.id, true, &r, NULL);
    if (err != ZBX_ERR_NONE) {
        return err;
    }
    if (zbx_parse_count(r.result, &total) != 0) {
        return shape(c, &r, "host count");
    }
    zbx_reply_free(&r);

    /* The hosts that matter are read first, whatever their names: the ones
     * the server lists as having an unavailable interface, then the ones
     * with a problem. Only then is the rest filled in by name - on an estate
     * larger than ZBX_HOST_FETCH, a host called "zulu" that is down would
     * otherwise never be seen. Down hosts go first because they are few and
     * because the down count is only exact if every one of them is read. */
    x = ctx_for(c);
    err = call(c, zbx_req_down_interfaces(&x, ZBX_HOST_FETCH), x.id, true, &r, NULL);
    if (err != ZBX_ERR_NONE) {
        return err;
    }
    if (zbx_parse_hostids(r.result, c->hostids, ZBX_HOST_FETCH, &nids, &seen) != 0) {
        return shape(c, &r, "hostinterface.get");
    }
    zbx_reply_free(&r);
    /* Every unavailable interface was listed (and each of their hosts has a
     * place, as they come first): the down count covers the whole estate. */
    down_exact = seen < ZBX_HOST_FETCH;
    for (i = 0; i < c->pscratch_n && nids < ZBX_HOST_FETCH; i++) {
        const char *id = c->pscratch[i].hostid;
        int k;

        if (!id[0]) {
            continue;
        }
        for (k = 0; k < nids && strcmp(c->hostids[k], id) != 0; k++) {
        }
        if (k == nids) {
            snprintf(c->hostids[nids++], ZBX_ID_MAX, "%s", id);
        }
    }
    if (nids > 0) {
        for (i = 0; i < nids; i++) {
            c->hostid_ptrs[i] = c->hostids[i];
        }
        x = ctx_for(c);
        err = call(c, zbx_req_hosts_by_id(&x, c->hostid_ptrs, nids), x.id, true, &r, &date);
        if (err != ZBX_ERR_NONE) {
            return err;
        }
        if (zbx_parse_hosts(r.result, c->hscratch, ZBX_HOST_FETCH, &n) != 0) {
            return shape(c, &r, "host.get");
        }
        zbx_reply_free(&r);
        if (n > ZBX_HOST_FETCH) {
            n = ZBX_HOST_FETCH;
        }
    }
    if (n < ZBX_HOST_FETCH && n < total) {
        int more = 0;
        int k;

        x = ctx_for(c);
        err = call(c, zbx_req_hosts(&x, NULL, ZBX_HOST_FETCH), x.id, true, &r, &date);
        if (err != ZBX_ERR_NONE) {
            return err;
        }
        if (zbx_parse_hosts(r.result, c->hscratch2, ZBX_HOST_FETCH, &more) != 0) {
            return shape(c, &r, "host.get");
        }
        zbx_reply_free(&r);
        for (k = 0; k < more && k < ZBX_HOST_FETCH && n < ZBX_HOST_FETCH; k++) {
            int j;

            for (j = 0; j < n && strcmp(c->hscratch[j].hostid, c->hscratch2[k].hostid) != 0; j++) {
            }
            if (j == n) {
                c->hscratch[n++] = c->hscratch2[k];
            }
        }
    }
    zbx_hosts_apply(c->hscratch, n, c->pscratch, c->pscratch_n);

    memset(s, 0, sizeof(*s));
    for (i = 0; i < n; i++) {
        if (c->hscratch[i].avail == ZBX_AVAIL_DOWN) {
            s->down++;
        } else if (c->hscratch[i].avail == ZBX_AVAIL_UNKNOWN) {
            s->unknown++;
        }
        if (c->hscratch[i].maintenance) {
            s->maintenance++;
        }
    }
    zbx_hosts_sort(c->hscratch, n);
    s->count = n < ZBX_HOST_MAX ? n : ZBX_HOST_MAX;
    memcpy(s->h, c->hscratch, (size_t)s->count * sizeof(s->h[0]));
    s->total = total > n ? total : n;
    s->total_exact = true;
    s->fetched = n;
    s->down_exact = down_exact || n >= s->total;
    s->ref_clock = date > 0 ? date : clock_valid() ? (long long)time(NULL) : 0;
    c->seq++;
    zbx_proto_hosts(c->out, c->seq, s);
    c->have_hosts = true;
    return ZBX_ERR_NONE;
}

/* ---- one host ----------------------------------------------------------------------------- */

static enum zbx_err sync_detail(struct zbx_client *c)
{
    struct zbx_detail *d = c->detail;
    struct zbx_problem tmp[ZBX_DETAIL_PROBLEM_MAX * 5];
    struct zbx_req_ctx x = ctx_for(c);
    struct zbx_reply r;
    enum zbx_err err;
    long long date = -1;
    char hostid[ZBX_ID_MAX];
    int n = 0;
    int i;

    snprintf(hostid, sizeof(hostid), "%s", c->detail_hostid);
    zbx_proto_busy(c->out, "detail");
    err = call(c, zbx_req_hosts(&x, hostid, 1), x.id, true, &r, &date);
    if (err != ZBX_ERR_NONE) {
        return err;
    }
    memset(d, 0, sizeof(*d));
    if (zbx_parse_hosts(r.result, &d->host, 1, &n) != 0) {
        return shape(c, &r, "host.get");
    }
    zbx_reply_free(&r);
    if (n == 0) {
        zbx_proto_detail_none(c->out, hostid, "Host not found, or not visible to this user");
        return ZBX_ERR_NONE;
    }

    x = ctx_for(c);
    err = call(c, zbx_req_problems(&x, hostid, (int)(sizeof(tmp) / sizeof(tmp[0]))), x.id, true, &r,
               NULL);
    if (err != ZBX_ERR_NONE) {
        return err;
    }
    if (zbx_parse_problems(r.result, tmp, (int)(sizeof(tmp) / sizeof(tmp[0])), &n) != 0) {
        return shape(c, &r, "problem.get");
    }
    zbx_reply_free(&r);
    if (n > (int)(sizeof(tmp) / sizeof(tmp[0]))) {
        n = (int)(sizeof(tmp) / sizeof(tmp[0]));
    }
    for (i = 0; i < n; i++) {
        snprintf(tmp[i].hostid, sizeof(tmp[i].hostid), "%s", d->host.hostid);
        snprintf(tmp[i].host, sizeof(tmp[i].host), "%s", d->host.name);
    }
    zbx_problems_sort(tmp, n);
    d->problem_count = n < ZBX_DETAIL_PROBLEM_MAX ? n : ZBX_DETAIL_PROBLEM_MAX;
    memcpy(d->p, tmp, (size_t)d->problem_count * sizeof(d->p[0]));
    zbx_hosts_apply(&d->host, 1, tmp, n);

    x = ctx_for(c);
    err = call(c, zbx_req_items(&x, hostid, ZBX_ITEM_FETCH), x.id, true, &r, NULL);
    if (err != ZBX_ERR_NONE) {
        return err;
    }
    if (zbx_parse_items(r.result, d->item, ZBX_ITEM_MAX, &d->item_count) != 0) {
        return shape(c, &r, "item.get");
    }
    zbx_reply_free(&r);
    d->ref_clock = date > 0 ? date : clock_valid() ? (long long)time(NULL) : 0;
    c->seq++;
    zbx_proto_detail(c->out, c->seq, d);
    return ZBX_ERR_NONE;
}

/* ---- failures ------------------------------------------------------------------------------- */

static void failed(struct zbx_client *c, enum zbx_err err)
{
    int64_t t = now_of(c);

    c->err = err;
    c->attempt++;
    if (err == ZBX_ERR_AUTH) {
        c->connected = false;
        explicit_bzero(c->session, sizeof(c->session));
        if (c->cfg.auth == ZBX_AUTH_PASSWORD && !c->cfg.fake) {
            /* A refused password is never tried again on its own. Zabbix
             * blocks an account after "Login attempts" failures (5 by
             * default, for 30 s) and keeps counting until a login succeeds,
             * so a helper retrying a wrong password every few minutes would
             * lock its user out of the frontend again and again. Only
             * REFRESH (or opening the app again) tries. */
            c->next_try_ms = ZBX_TRY_NEVER;
            LOG_WARN("zabbix: authentication refused; the password is not tried again until "
                     "REFRESH");
        } else {
            c->next_try_ms = t + (int64_t)ZBX_AUTH_RETRY_S * 1000;
            LOG_WARN("zabbix: authentication refused");
        }
        c->reported = false;
        set_state(c, ZBX_CONN_AUTH_FAILED);
        return;
    }
    switch (err) {
    case ZBX_ERR_API:
    case ZBX_ERR_MALFORMED:
    case ZBX_ERR_TOO_LARGE:
        /* The server is there and talking; what it said was wrong. */
        break;
    default:
        c->connected = false;
        break;
    }
    c->next_try_ms = t + (int64_t)zbx_backoff_s(c->attempt) * 1000;
    LOG_WARN("zabbix: %s (%s), attempt %d, next in %d s", zbx_err_word(err), c->err_text,
             c->attempt, zbx_backoff_s(c->attempt));
    c->reported = false;
    set_state(c, ZBX_CONN_RETRYING);
}

static void succeeded(struct zbx_client *c)
{
    if (c->state != ZBX_CONN_ONLINE || c->attempt != 0 || c->err != ZBX_ERR_NONE) {
        c->attempt = 0;
        c->err = ZBX_ERR_NONE;
        c->err_text[0] = '\0';
        c->reported = false;
        set_state(c, ZBX_CONN_ONLINE);
    }
}

/* A session that ran out is logged in again once before anything fails. */
static bool relogin(struct zbx_client *c, enum zbx_err err)
{
    if (err != ZBX_ERR_AUTH || c->cfg.auth != ZBX_AUTH_PASSWORD || c->cfg.fake || c->session_fresh) {
        return false;
    }
    LOG_INFO("zabbix: session ended, logging in again");
    explicit_bzero(c->session, sizeof(c->session));
    c->connected = false;
    return true;
}

/* ---- leaving -------------------------------------------------------------------------------- */

bool zbx_client_has_session(const struct zbx_client *c)
{
    return c->cfg.auth == ZBX_AUTH_PASSWORD && !c->cfg.fake && c->session[0] != '\0';
}

int zbx_client_logout(struct zbx_client *c, int timeout_ms)
{
    struct zbx_req_ctx x;
    struct zbx_reply r;
    enum zbx_err err;

    if (!zbx_client_has_session(c)) {
        return 0;
    }
    x = ctx_for(c);
    c->timeout_override_ms = timeout_ms;
    err = call(c, zbx_req_logout(&x), x.id, true, &r, NULL);
    if (err == ZBX_ERR_NONE) {
        zbx_reply_free(&r);
    }
    explicit_bzero(c->session, sizeof(c->session));
    c->connected = false;
    LOG_INFO("zabbix: %s", err == ZBX_ERR_NONE ? "logged out" : "logout failed; the session ends by "
                                                               "itself on the server");
    return err == ZBX_ERR_NONE ? 0 : -1;
}

/* ---- the schedule ------------------------------------------------------------------------ */

void zbx_client_reset(struct zbx_client *c)
{
    c->connected = false;
    explicit_bzero(c->session, sizeof(c->session));
    c->version[0] = '\0';
    c->vnum = 0;
    c->attempt = 0;
    c->err = ZBX_ERR_NONE;
    c->err_text[0] = '\0';
    c->next_try_ms = 0;
    c->due_problems_ms = 0;
    c->due_hosts_ms = 0;
    c->pscratch_n = 0;
    c->have_problems = false;
    c->have_hosts = false;
    c->detail_due = c->detail_hostid[0] != '\0';
    c->reported = false;
    set_state(c, c->cfg.configured ? ZBX_CONN_CONNECTING : ZBX_CONN_UNCONFIGURED);
}

void zbx_client_command(struct zbx_client *c, const struct zbx_cmd *cmd)
{
    int64_t t = now_of(c);

    switch (cmd->kind) {
    case ZBX_CMD_REFRESH:
        if (c->last_manual_ms && t - c->last_manual_ms < (int64_t)ZBX_MANUAL_REFRESH_S * 1000) {
            return;
        }
        c->last_manual_ms = t;
        c->due_problems_ms = t;
        c->due_hosts_ms = t;
        c->detail_due = c->detail_hostid[0] != '\0';
        if (c->state == ZBX_CONN_RETRYING || c->state == ZBX_CONN_AUTH_FAILED) {
            c->next_try_ms = t;
        }
        break;
    case ZBX_CMD_DETAIL:
        snprintf(c->detail_hostid, sizeof(c->detail_hostid), "%.23s", cmd->word);
        c->detail_due = c->detail_hostid[0] != '\0';
        break;
    default:
        break;
    }
}

static int wake_in(struct zbx_client *c)
{
    int64_t t = now_of(c);
    int64_t next;

    if (!c->cfg.configured) {
        return WAKE_MAX_MS;
    }
    if (c->state == ZBX_CONN_RETRYING || c->state == ZBX_CONN_AUTH_FAILED) {
        next = c->next_try_ms;
    } else if (!c->connected || c->detail_due) {
        next = t;
    } else {
        next = c->due_problems_ms < c->due_hosts_ms ? c->due_problems_ms : c->due_hosts_ms;
    }
    next -= t;
    if (next < 0) {
        return 0;
    }
    return next > WAKE_MAX_MS ? WAKE_MAX_MS : (int)next;
}

int zbx_client_step(struct zbx_client *c)
{
    int64_t t = now_of(c);
    enum zbx_err err = ZBX_ERR_NONE;
    bool worked = false;
    int tries;

    if (!c->cfg.configured) {
        return WAKE_MAX_MS;
    }
    if ((c->state == ZBX_CONN_RETRYING || c->state == ZBX_CONN_AUTH_FAILED) && t < c->next_try_ms) {
        return wake_in(c);
    }
    /* Once, and again after each session that ran out: a relogin is only
     * made for a session that had served a request (session_fresh), so this
     * cannot loop on a server that refuses every new session; the bound is
     * for a server whose sessions end after a request or two. */
    for (tries = 0; tries < 4; tries++) {
        if (!c->connected) {
            if (c->state != ZBX_CONN_ONLINE) {
                set_state(c, ZBX_CONN_CONNECTING);
            }
            err = do_connect(c);
            if (err != ZBX_ERR_NONE) {
                break;
            }
            worked = true;
        }
        t = now_of(c);
        if (t >= c->due_problems_ms) {
            err = sync_problems(c);
            if (err != ZBX_ERR_NONE) {
                if (relogin(c, err)) {
                    continue;
                }
                break;
            }
            c->due_problems_ms = now_of(c) + (int64_t)c->cfg.refresh_s * 1000;
            c->detail_due = c->detail_hostid[0] != '\0';
            worked = true;
        }
        if (c->detail_due && c->detail_hostid[0]) {
            err = sync_detail(c);
            if (err != ZBX_ERR_NONE) {
                if (relogin(c, err)) {
                    continue;
                }
                break;
            }
            c->detail_due = false;
            worked = true;
        }
        t = now_of(c);
        if (t >= c->due_hosts_ms) {
            err = sync_hosts(c);
            if (err != ZBX_ERR_NONE) {
                if (relogin(c, err)) {
                    continue;
                }
                break;
            }
            c->due_hosts_ms = now_of(c) + (int64_t)c->cfg.hosts_s * 1000;
            worked = true;
        }
        break;
    }
    if (err != ZBX_ERR_NONE) {
        failed(c, err);
    } else if (worked) {
        succeeded(c);
    }
    if (worked || err != ZBX_ERR_NONE) {
        zbx_proto_idle(c->out);
    }
    return wake_in(c);
}
