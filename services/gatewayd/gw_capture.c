/*
 * gatewayd: PacketShark's capture for a paired client (gw_capture.h).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gw_capture.h"

#include <math.h>
#include <string.h>

static const char *const STATES[] = {"idle", "capturing", "hold", "saving", "stopped", "ended", NULL};
static const char *const REASONS[] = {"none",  "user",           "limit",       "link", "end",     "error",
                                      "helper_crashed", "helper_hung", "left", "refused", "timeout", NULL};
static const char *const KEEPS[] = {"headers", "full", NULL};
static const char *const FIGURES[] = {"elapsed_ms",  "limit_s", "seen",  "stored", "held", "kernel_drops",
                                      "iface_drops", "shed",    "bytes", "pps",    "bps",  "fill_permille",
                                      NULL};
static const char *const CLASSES[] = {"tcp", "udp", "dns", "dhcp", "arp", "icmp", "ipv6", "other", NULL};
static const char *const DIAG[] = {"dns_queries",    "dns_retries",    "dns_answered",  "dns_late",
                                   "dns_noreply",    "dns_waiting",    "dns_nxdomain",  "dns_servfail",
                                   "dns_refused",    "dns_rtt_sum_us", "dns_rtt_max_us", "tcp_attempts",
                                   "tcp_connected",  "tcp_timed",      "tcp_hs_sum_us", "tcp_hs_max_us",
                                   "tcp_refused",    "tcp_aborted",    NULL};
#define FINDINGS 6

/* The word of `list` that v is, or `otherwise`. */
static const char *word(const cJSON *v, const char *const *list, const char *otherwise)
{
    size_t i;

    for (i = 0; cJSON_IsString(v) && list[i] != NULL; i++) {
        if (strcmp(v->valuestring, list[i]) == 0) {
            return list[i];
        }
    }
    return otherwise;
}

/* A count: a whole number from 0 to the largest that prints whole, or 0. */
static double count(const cJSON *v)
{
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= GW_CAPTURE_FIGURE_MAX) ||
        v->valuedouble != floor(v->valuedouble)) {
        return 0;
    }
    return v->valuedouble;
}

/* An interface name that says its kind and number and nothing more: eth,
 * wlan, usb or synth and one or two digits. Anything else is not passed on:
 * a name can be chosen, and a kernel's enx.../wlx... names are a hardware
 * address. */
static bool iface_plain(const char *s)
{
    static const char *const kinds[] = {"eth", "wlan", "usb", "synth"};
    size_t k;

    for (k = 0; s != NULL && k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        size_t n = strlen(kinds[k]);

        if (strncmp(s, kinds[k], n) == 0 && s[n] >= '0' && s[n] <= '9' &&
            (s[n + 1] == '\0' || (s[n + 1] >= '0' && s[n + 1] <= '9' && s[n + 2] == '\0'))) {
            return true;
        }
    }
    return false;
}

static bool only(const cJSON *params, const char *const *names)
{
    const cJSON *el;
    size_t i;

    if (params == NULL) {
        return true;
    }
    if (!cJSON_IsObject(params)) {
        return false;
    }
    cJSON_ArrayForEach(el, params) {
        for (i = 0; names[i] != NULL; i++) {
            if (el->string != NULL && strcmp(el->string, names[i]) == 0) {
                break;
            }
        }
        if (names[i] == NULL) {
            return false;
        }
    }
    return true;
}

cJSON *gw_capture_status(const cJSON *result)
{
    const cJSON *av, *iface, *cls, *found, *diag;
    cJSON *o, *sub;
    const char *state;
    size_t i;
    bool ok;

    if (!cJSON_IsObject(result)) {
        return NULL;
    }
    o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    av = cJSON_GetObjectItemCaseSensitive(result, "available");
    if (!cJSON_IsTrue(av)) {
        /* Not there to be read, or an answer that does not say: nothing else. */
        if (cJSON_AddBoolToObject(o, "available", false) == NULL) {
            cJSON_Delete(o);
            return NULL;
        }
        return o;
    }
    state = word(cJSON_GetObjectItemCaseSensitive(result, "state"), STATES, "idle");
    iface = cJSON_GetObjectItemCaseSensitive(result, "iface");
    ok = cJSON_AddBoolToObject(o, "available", true) != NULL && cJSON_AddStringToObject(o, "state", state) != NULL &&
         cJSON_AddStringToObject(o, "reason",
                                 word(cJSON_GetObjectItemCaseSensitive(result, "reason"), REASONS, "none")) != NULL &&
         cJSON_AddStringToObject(o, "iface",
                                 cJSON_IsString(iface) && iface_plain(iface->valuestring) ? iface->valuestring
                                                                                          : "") != NULL &&
         cJSON_AddStringToObject(o, "keep", word(cJSON_GetObjectItemCaseSensitive(result, "keep"), KEEPS, "headers")) !=
             NULL;
    for (i = 0; ok && FIGURES[i] != NULL; i++) {
        double v = count(cJSON_GetObjectItemCaseSensitive(result, FIGURES[i]));

        if (strcmp(FIGURES[i], "fill_permille") == 0 && v > 1000) {
            v = 1000;
        }
        ok = cJSON_AddNumberToObject(o, FIGURES[i], v) != NULL;
    }
    cls = cJSON_GetObjectItemCaseSensitive(result, "classes");
    sub = ok ? cJSON_AddObjectToObject(o, "classes") : NULL;
    for (i = 0; ok && sub != NULL && CLASSES[i] != NULL; i++) {
        ok = cJSON_AddNumberToObject(sub, CLASSES[i],
                                     count(cJSON_IsObject(cls) ? cJSON_GetObjectItemCaseSensitive(cls, CLASSES[i])
                                                               : NULL)) != NULL;
    }
    ok = ok && sub != NULL;
    found = cJSON_GetObjectItemCaseSensitive(result, "findings");
    sub = ok ? cJSON_AddArrayToObject(o, "findings") : NULL;
    for (i = 0; ok && sub != NULL && i < FINDINGS; i++) {
        cJSON *n = cJSON_CreateNumber(count(cJSON_IsArray(found) ? cJSON_GetArrayItem(found, (int)i) : NULL));

        ok = n != NULL && cJSON_AddItemToArray(sub, n);
        if (!ok) {
            cJSON_Delete(n);
        }
    }
    ok = ok && sub != NULL;
    diag = cJSON_GetObjectItemCaseSensitive(result, "diag");
    sub = ok ? cJSON_AddObjectToObject(o, "diag") : NULL;
    for (i = 0; ok && sub != NULL && DIAG[i] != NULL; i++) {
        ok = cJSON_AddNumberToObject(sub, DIAG[i],
                                     count(cJSON_IsObject(diag) ? cJSON_GetObjectItemCaseSensitive(diag, DIAG[i])
                                                                : NULL)) != NULL;
    }
    ok = ok && sub != NULL &&
         cJSON_AddBoolToObject(o, "requested_by_companion",
                               cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "requested_by_companion"))) != NULL;
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

cJSON *gw_capture_command(const cJSON *result)
{
    const cJSON *ch = cJSON_IsObject(result) ? cJSON_GetObjectItemCaseSensitive(result, "changing") : NULL;
    cJSON *o;

    if (!cJSON_IsObject(result) || !cJSON_IsBool(ch)) {
        return NULL;
    }
    o = cJSON_CreateObject();
    if (o == NULL ||
        cJSON_AddStringToObject(o, "state", word(cJSON_GetObjectItemCaseSensitive(result, "state"), STATES, "idle")) ==
            NULL ||
        cJSON_AddBoolToObject(o, "changing", cJSON_IsTrue(ch)) == NULL) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

cJSON *gw_capture_shown(const cJSON *result)
{
    cJSON *o;

    if (!cJSON_IsObject(result) || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "shown"))) {
        return NULL;
    }
    o = cJSON_CreateObject();
    if (o == NULL || cJSON_AddBoolToObject(o, "shown", true) == NULL) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

int gw_capture_params_hold(const cJSON *in, cJSON **out)
{
    static const char *const names[] = {"on", NULL};
    const cJSON *on = cJSON_IsObject(in) ? cJSON_GetObjectItemCaseSensitive(in, "on") : NULL;

    *out = NULL;
    if (!cJSON_IsObject(in) || !only(in, names) || !cJSON_IsBool(on)) {
        return -1;
    }
    *out = cJSON_CreateObject();
    if (*out == NULL || cJSON_AddBoolToObject(*out, "on", cJSON_IsTrue(on)) == NULL) {
        cJSON_Delete(*out);
        *out = NULL;
        return -1;
    }
    return 0;
}

int gw_capture_params_ask(const cJSON *in, cJSON **out)
{
    static const char *const names[] = {"iface", "limit_s", NULL};
    const cJSON *iface = cJSON_IsObject(in) ? cJSON_GetObjectItemCaseSensitive(in, "iface") : NULL;
    const cJSON *limit = cJSON_IsObject(in) ? cJSON_GetObjectItemCaseSensitive(in, "limit_s") : NULL;

    *out = NULL;
    if (!cJSON_IsObject(in) || !only(in, names) || !cJSON_IsString(iface) || !iface_plain(iface->valuestring) ||
        !cJSON_IsNumber(limit) ||
        !(limit->valuedouble == 300 || limit->valuedouble == 900 || limit->valuedouble == 3600)) {
        return -1;
    }
    *out = cJSON_CreateObject();
    if (*out == NULL || cJSON_AddStringToObject(*out, "iface", iface->valuestring) == NULL ||
        cJSON_AddNumberToObject(*out, "limit_s", limit->valuedouble) == NULL) {
        cJSON_Delete(*out);
        *out = NULL;
        return -1;
    }
    return 0;
}

static cJSON *state_json(const char *state, const char *reason, const char *iface, const char *keep, double limit_s)
{
    cJSON *o = cJSON_CreateObject();

    if (o == NULL || cJSON_AddStringToObject(o, "state", state) == NULL ||
        cJSON_AddStringToObject(o, "reason", reason) == NULL || cJSON_AddStringToObject(o, "iface", iface) == NULL ||
        cJSON_AddStringToObject(o, "keep", keep) == NULL || cJSON_AddNumberToObject(o, "limit_s", limit_s) == NULL) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsString(v) ? v->valuestring : "";
}

cJSON *gw_capture_told(const struct gw_capture *c)
{
    return c->told ? state_json(c->state, c->reason, c->iface, c->keep, c->limit_s) : NULL;
}

cJSON *gw_capture_outcome(const char *reason)
{
    return state_json("idle", strcmp(reason, "timeout") == 0 ? "timeout" : "refused", "", "headers", 0);
}

void gw_capture_init(struct gw_capture *c)
{
    memset(c, 0, sizeof(*c));
}

static void copy(char *dst, size_t n, const char *src)
{
    size_t len = strlen(src);

    if (len >= n) {
        len = n - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void gw_capture_lost(struct gw_capture *c, cJSON **state)
{
    *state = NULL;
    if (c->told && strcmp(c->state, "idle") != 0 && strcmp(c->state, "ended") != 0) {
        /* A capture clients were told of is out of sight: its last word. */
        *state = state_json("ended", "left", c->iface, c->keep, c->limit_s);
    }
    c->told = false;
    c->stats_owed = false;
    c->state[0] = '\0';
}

void gw_capture_step(struct gw_capture *c, const cJSON *raw, int64_t sent_ms, cJSON **state, cJSON **stats,
                     const char **outcome)
{
    cJSON *st = gw_capture_status(raw);
    const char *now_state, *now_reason, *req;
    bool running, changed;

    *state = NULL;
    *stats = NULL;
    *outcome = NULL;
    if (st == NULL) {
        return;
    }
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "available"))) {
        /* PacketShark is not in front. A request that was waiting there is
         * over; a capture that was there is out of sight. */
        gw_capture_lost(c, state);
        if (c->asking && c->shown && sent_ms >= c->shown_ms) {
            *outcome = "refused";
        }
        cJSON_Delete(st);
        return;
    }
    now_state = str(st, "state");
    /* On setup the unit's reason is the outcome of a request, there for a
     * moment; that is said through *outcome, once, and not as a state. */
    now_reason = strcmp(now_state, "idle") == 0 ? "none" : str(st, "reason");
    running = strcmp(now_state, "capturing") == 0 || strcmp(now_state, "hold") == 0;
    changed = !c->told || strcmp(c->state, now_state) != 0 || strcmp(c->reason, now_reason) != 0 ||
              strcmp(c->iface, str(st, "iface")) != 0 || strcmp(c->keep, str(st, "keep")) != 0 ||
              c->limit_s != count(cJSON_GetObjectItemCaseSensitive(st, "limit_s"));
    if (changed) {
        cJSON *ev = state_json(now_state, now_reason, str(st, "iface"), str(st, "keep"),
                               count(cJSON_GetObjectItemCaseSensitive(st, "limit_s")));

        if (ev != NULL) {
            *state = ev;
            c->told = true;
            copy(c->state, sizeof(c->state), now_state);
            copy(c->reason, sizeof(c->reason), now_reason);
            copy(c->iface, sizeof(c->iface), str(st, "iface"));
            copy(c->keep, sizeof(c->keep), str(st, "keep"));
            c->limit_s = count(cJSON_GetObjectItemCaseSensitive(st, "limit_s"));
            c->stats_owed = strcmp(now_state, "idle") != 0;
        }
    }
    /* The waiting request, read from the unit's own word for it (which is
     * not passed on) - but only from an answer asked for after the unit
     * said the sheet was up: an older answer speaks of an older request. */
    if (c->asking && c->shown && sent_ms >= c->shown_ms) {
        req = str(raw, "request");
        if (strcmp(now_state, "idle") != 0) {
            gw_capture_ask_end(c); /* a capture: started by a tap, or the person's own */
        } else if (strcmp(req, "timeout") == 0) {
            *outcome = "timeout";
        } else if (strcmp(req, "refused") == 0) {
            *outcome = "refused";
        }
        /* Any other word, or none (an older PacketShark): undecided. The
         * sheet is up, or START was tapped and the capture is starting. */
    }
    /* Figures only of the state that was said: with it when it changes,
     * then with every answer while it runs. */
    if (c->told && strcmp(c->state, now_state) == 0 && (running || c->stats_owed)) {
        *stats = st;
        c->stats_owed = false;
        return;
    }
    cJSON_Delete(st);
}

enum gw_capture_ask gw_capture_may_ask(const struct gw_capture *c, const uint8_t *fp, int64_t now_ms)
{
    size_t i;

    if (c->asking) {
        return GW_ASK_BUSY;
    }
    for (i = 0; i < GW_MAX_CLIENTS; i++) {
        if (c->last[i].used && memcmp(c->last[i].fp, fp, GW_FP_LEN) == 0 && now_ms >= c->last[i].ms &&
            now_ms - c->last[i].ms < GW_CAPTURE_ASK_EVERY_MS) {
            return GW_ASK_RATE;
        }
    }
    return GW_ASK_OK;
}

void gw_capture_ask(struct gw_capture *c, const uint8_t *fp, uint64_t sid, const char *token, int64_t now_ms)
{
    size_t i, at = 0;

    /* Its own slot, a free one, or the oldest. */
    for (i = 0; i < GW_MAX_CLIENTS; i++) {
        if (c->last[i].used && memcmp(c->last[i].fp, fp, GW_FP_LEN) == 0) {
            at = i;
            break;
        }
        if (!c->last[i].used) {
            if (c->last[at].used) {
                at = i;
            }
        } else if (c->last[at].used && c->last[i].ms < c->last[at].ms) {
            at = i;
        }
    }
    c->last[at].used = true;
    memcpy(c->last[at].fp, fp, GW_FP_LEN);
    c->last[at].ms = now_ms;
    c->asking = true;
    c->shown = false;
    c->shown_ms = 0;
    c->asked_ms = now_ms;
    c->sid = sid;
    memcpy(c->fp, fp, GW_FP_LEN);
    copy(c->token, sizeof(c->token), token);
}

void gw_capture_ask_shown(struct gw_capture *c, int64_t now_ms)
{
    if (c->asking) {
        c->shown = true;
        c->shown_ms = now_ms;
    }
}

void gw_capture_ask_end(struct gw_capture *c)
{
    c->asking = false;
    c->shown = false;
    c->sid = 0;
    memset(c->fp, 0, sizeof(c->fp));
    memset(c->token, 0, sizeof(c->token));
}
