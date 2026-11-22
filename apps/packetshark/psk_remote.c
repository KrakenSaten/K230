/*
 * The figures of a capture that may be read from outside the app, and their
 * JSON (psk_remote.h).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_remote.h"

#include <cjson/cJSON.h>
#include <string.h>

static const char *const state_words[PSK_RS_COUNT] = { "idle", "capturing", "hold", "saving", "stopped", "ended" };
static const char *const reason_words[PSK_RR_COUNT] = { "none", "user",  "limit",          "link",
                                                        "end",  "error", "helper_crashed", "helper_hung",
                                                        "left" };
static const char *const class_keys[PPC_CLASS_COUNT] = { "tcp", "udp", "dns", "dhcp", "arp", "icmp", "ipv6", "other" };
static const char *const diag_keys[PSK_REMOTE_DIAG] = {
    "dns_queries",    "dns_retries",    "dns_answered",  "dns_late",      "dns_noreply",   "dns_waiting",
    "dns_nxdomain",   "dns_servfail",   "dns_refused",   "dns_rtt_sum_us", "dns_rtt_max_us", "tcp_attempts",
    "tcp_connected",  "tcp_timed",      "tcp_hs_sum_us", "tcp_hs_max_us", "tcp_refused",   "tcp_aborted",
};

const char *psk_remote_state_name(unsigned state)
{
    return state < PSK_RS_COUNT ? state_words[state] : state_words[PSK_RS_IDLE];
}

const char *psk_remote_reason_name(unsigned reason)
{
    return reason < PSK_RR_COUNT ? reason_words[reason] : reason_words[PSK_RR_NONE];
}

const char *psk_remote_diag_name(unsigned i)
{
    return i < PSK_REMOTE_DIAG ? diag_keys[i] : NULL;
}

const char *psk_remote_class_name(unsigned i)
{
    return i < PPC_CLASS_COUNT ? class_keys[i] : NULL;
}

/* An interface name that says what kind of link it is and nothing more: one
 * of four words and one or two digits (eth0, wlan0, usb0, synth0). Any other
 * name is not passed on: a name can be chosen by someone, and the kernel's
 * own predictable names for an adapter (enx..., wlx...) are its hardware
 * address. */
static bool iface_plain(const char *s)
{
    static const char *const kinds[] = { "eth", "wlan", "usb", "synth" };
    size_t k;

    for (k = 0; s && k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        size_t n = strlen(kinds[k]);

        /* Each test reads the next byte only when the one before it was not
         * the end, and none reads past n + 2 < PPC_IFACE_MAX. */
        if (strncmp(s, kinds[k], n) == 0 && s[n] >= '0' && s[n] <= '9' &&
            (s[n + 1] == '\0' || (s[n + 1] >= '0' && s[n + 1] <= '9' && s[n + 2] == '\0'))) {
            return true;
        }
    }
    return false;
}

static uint8_t state_of(enum ppc_state s)
{
    switch (s) {
    case PPC_STATE_CAPTURING:
        return PSK_RS_CAPTURING;
    case PPC_STATE_HOLD:
        return PSK_RS_HOLD;
    case PPC_STATE_SAVING:
        return PSK_RS_SAVING;
    case PPC_STATE_STOPPED:
        return PSK_RS_STOPPED;
    case PPC_STATE_IDLE:
    default:
        return PSK_RS_IDLE;
    }
}

static uint8_t reason_of(enum ppc_reason r)
{
    switch (r) {
    case PPC_REASON_USER:
        return PSK_RR_USER;
    case PPC_REASON_LIMIT:
        return PSK_RR_LIMIT;
    case PPC_REASON_LINK:
        return PSK_RR_LINK;
    case PPC_REASON_END:
        return PSK_RR_END;
    case PPC_REASON_ERROR:
        return PSK_RR_ERROR;
    case PPC_REASON_NONE:
    default:
        return PSK_RR_NONE;
    }
}

/* How the helper ended, in the two words there are for it: it stopped
 * answering, or it is gone. */
static uint8_t reason_of_exit(enum psk_exit e)
{
    return e == PSK_EXIT_HUNG ? PSK_RR_HELPER_HUNG : PSK_RR_HELPER_CRASHED;
}

void psk_remote_take(const struct psk_model *m, enum psk_remote_phase phase, bool keep_full, uint32_t limit_s,
                     struct psk_remote_figures *out)
{
    unsigned i;

    memset(out, 0, sizeof(*out));
    if (!m || phase == PSK_REMOTE_IDLE) {
        return; /* setup: no capture, so no figure of one */
    }
    if (phase == PSK_REMOTE_ENDED) {
        out->state = PSK_RS_ENDED;
        out->reason = reason_of_exit(m->exit);
    } else {
        out->state = state_of(m->state);
        if (out->state == PSK_RS_IDLE) {
            return; /* the screen and the model disagree for a moment: say the lesser */
        }
        /* Only a capture that has stopped has a reason for it. */
        out->reason = out->state == PSK_RS_STOPPED || out->state == PSK_RS_SAVING ? reason_of(m->reason) : PSK_RR_NONE;
    }
    out->keep_full = keep_full;
    out->limit_s = limit_s;
    if (iface_plain(m->iface)) {
        memcpy(out->iface, m->iface, strlen(m->iface) + 1);
    }
    if (m->have_stats) {
        out->elapsed_ms = m->c.elapsed_ms;
        out->seen = m->c.seen;
        out->stored = m->c.stored;
        out->held = m->c.held;
        out->kernel_drops = m->c.kdrop;
        out->iface_drops = m->c.ifdrop;
        out->shed = m->c.shed;
        out->bytes = m->c.bytes;
        out->fill_permille = m->c.fill_permille > 1000 ? 1000 : m->c.fill_permille;
        /* A rate is of now: a capture that is not running has none. */
        if (out->state == PSK_RS_CAPTURING || out->state == PSK_RS_HOLD) {
            out->pps = m->c.pps;
            out->bps = m->c.bps;
        }
        for (i = 0; i < PPC_CLASS_COUNT; i++) {
            out->classes[i] = m->c.cls[i];
        }
        for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
            out->findings[i] = m->c.sig[i];
        }
    }
    if (m->have_diag) {
        /* Totals and times only. The diagnosis also knows which packet was
         * the slowest; that is a packet, and stays. */
        const uint64_t d[PSK_REMOTE_DIAG] = {
            m->d.dns_queries,    m->d.dns_retries,    m->d.dns_answered,  m->d.dns_late,      m->d.dns_noreply,
            m->d.dns_waiting,    m->d.dns_nxdomain,   m->d.dns_servfail,  m->d.dns_refused,   m->d.dns_rtt_sum_us,
            m->d.dns_rtt_max_us, m->d.tcp_attempts,   m->d.tcp_connected, m->d.tcp_timed,     m->d.tcp_hs_sum_us,
            m->d.tcp_hs_max_us,  m->d.tcp_refused,    m->d.tcp_aborted,
        };

        memcpy(out->diag, d, sizeof(d));
    }
}

static double num(uint64_t v)
{
    return (double)(v > PSK_REMOTE_MAX ? PSK_REMOTE_MAX : v);
}

struct cJSON *psk_remote_json(const struct psk_remote_figures *f)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *cls = NULL;
    cJSON *found = NULL;
    cJSON *diag = NULL;
    unsigned i;
    bool ok = o != NULL;

    /* Every node is made already attached to `o`, so there is one thing to
     * free whatever fails. */
    if (ok) {
        ok = cJSON_AddBoolToObject(o, "available", true) &&
             cJSON_AddStringToObject(o, "state", psk_remote_state_name(f->state)) &&
             cJSON_AddStringToObject(o, "reason", psk_remote_reason_name(f->reason)) &&
             cJSON_AddStringToObject(o, "iface", f->iface) &&
             cJSON_AddStringToObject(o, "keep", f->keep_full ? "full" : "headers") &&
             cJSON_AddNumberToObject(o, "elapsed_ms", num(f->elapsed_ms)) &&
             cJSON_AddNumberToObject(o, "limit_s", num(f->limit_s)) &&
             cJSON_AddNumberToObject(o, "seen", num(f->seen)) &&
             cJSON_AddNumberToObject(o, "stored", num(f->stored)) &&
             cJSON_AddNumberToObject(o, "held", num(f->held)) &&
             cJSON_AddNumberToObject(o, "kernel_drops", num(f->kernel_drops)) &&
             cJSON_AddNumberToObject(o, "iface_drops", num(f->iface_drops)) &&
             cJSON_AddNumberToObject(o, "shed", num(f->shed)) &&
             cJSON_AddNumberToObject(o, "bytes", num(f->bytes)) &&
             cJSON_AddNumberToObject(o, "pps", num(f->pps)) &&
             cJSON_AddNumberToObject(o, "bps", num(f->bps)) &&
             cJSON_AddNumberToObject(o, "fill_permille", num(f->fill_permille));
    }
    if (ok) {
        cls = cJSON_AddObjectToObject(o, "classes");
        found = cJSON_AddArrayToObject(o, "findings");
        diag = cJSON_AddObjectToObject(o, "diag");
        ok = cls && found && diag;
    }
    for (i = 0; ok && i < PPC_CLASS_COUNT; i++) {
        ok = cJSON_AddNumberToObject(cls, class_keys[i], num(f->classes[i])) != NULL;
    }
    for (i = 0; ok && i < PPC_SIGNAL_COUNT; i++) {
        cJSON *n = cJSON_CreateNumber(num(f->findings[i]));

        ok = n != NULL && cJSON_AddItemToArray(found, n);
        if (!ok) {
            cJSON_Delete(n);
        }
    }
    for (i = 0; ok && i < PSK_REMOTE_DIAG; i++) {
        ok = cJSON_AddNumberToObject(diag, diag_keys[i], num(f->diag[i])) != NULL;
    }
    /* No companion can ask for a capture yet (ADR-016 amendment 1 is
     * proposed): every capture was started by a tap on this screen. */
    ok = ok && cJSON_AddBoolToObject(o, "requested_by_companion", false);
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

struct cJSON *psk_remote_state_json(const struct psk_remote_figures *f)
{
    cJSON *o = cJSON_CreateObject();

    if (!o || !cJSON_AddStringToObject(o, "state", psk_remote_state_name(f->state)) ||
        !cJSON_AddStringToObject(o, "reason", psk_remote_reason_name(f->reason)) ||
        !cJSON_AddStringToObject(o, "iface", f->iface) ||
        !cJSON_AddStringToObject(o, "keep", f->keep_full ? "full" : "headers") ||
        !cJSON_AddNumberToObject(o, "limit_s", num(f->limit_s))) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

void psk_remote_left(struct psk_remote_figures *f)
{
    f->state = PSK_RS_ENDED;
    f->reason = PSK_RR_LEFT;
    f->pps = 0;
    f->bps = 0;
}

/* Long enough ago that the first attempt of each kind is due at once. */
#define NEVER_MS (-((int64_t)1 << 40))

void psk_remote_emit_init(struct psk_remote_emit *e)
{
    memset(e, 0, sizeof(*e));
    e->state_try_ms = NEVER_MS;
    e->stats_try_ms = NEVER_MS;
}

/* Whether `gap` has passed since `then`. A clock that went back (the tick
 * counter wrapped) counts as passed, so nothing waits for ever. */
static bool passed(int64_t now_ms, int64_t then_ms, int64_t gap)
{
    return now_ms < then_ms || now_ms - then_ms >= gap;
}

static bool same_state(const struct psk_remote_emit *e, const struct psk_remote_figures *f)
{
    return e->announced && e->state == f->state && e->reason == f->reason && e->keep_full == f->keep_full &&
           e->limit_s == f->limit_s && strcmp(e->iface, f->iface) == 0;
}

unsigned psk_remote_due(const struct psk_remote_emit *e, const struct psk_remote_figures *f, int64_t now_ms)
{
    bool running = f->state == PSK_RS_CAPTURING || f->state == PSK_RS_HOLD;

    if (!same_state(e, f)) {
        return passed(now_ms, e->state_try_ms, PSK_REMOTE_STATE_GAP_MS) ? PSK_REMOTE_DUE_STATE : 0;
    }
    if (e->owed && passed(now_ms, e->stats_try_ms, PSK_REMOTE_STATE_GAP_MS)) {
        return PSK_REMOTE_DUE_STATS;
    }
    if (running && passed(now_ms, e->stats_try_ms, PSK_REMOTE_STATS_GAP_MS)) {
        return PSK_REMOTE_DUE_STATS;
    }
    return 0;
}

void psk_remote_tried(struct psk_remote_emit *e, unsigned what, const struct psk_remote_figures *f, int64_t now_ms,
                      bool raised)
{
    if (what == PSK_REMOTE_DUE_STATE) {
        e->state_try_ms = now_ms;
        if (raised) {
            e->announced = true;
            e->state = f->state;
            e->reason = f->reason;
            e->keep_full = f->keep_full;
            e->limit_s = f->limit_s;
            memcpy(e->iface, f->iface, sizeof(e->iface));
            /* Setup has no figures to send after it. */
            e->owed = f->state != PSK_RS_IDLE;
        }
    } else if (what == PSK_REMOTE_DUE_STATS) {
        e->stats_try_ms = now_ms;
        if (raised) {
            e->owed = false;
        }
    }
}
