/*
 * The Zabbix viewer's data model. See zbx_model.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zabbix/zbx_model.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- severity --------------------------------------------------------------- */

const char *zbx_severity_word(int severity)
{
    static const char *const words[ZBX_SEV_COUNT] = { "N/C", "INFO", "WARNING",
                                                       "AVERAGE", "HIGH", "DISASTER" };

    return severity >= 0 && severity < ZBX_SEV_COUNT ? words[severity] : "OK";
}

const char *zbx_severity_name(int severity)
{
    static const char *const names[ZBX_SEV_COUNT] = { "Not classified", "Information", "Warning",
                                                       "Average", "High", "Disaster" };

    return severity >= 0 && severity < ZBX_SEV_COUNT ? names[severity] : "None";
}

int zbx_severity_clamp(long v)
{
    if (v < ZBX_SEV_NONE) {
        return ZBX_SEV_NONE;
    }
    if (v > ZBX_SEV_DISASTER) {
        return ZBX_SEV_DISASTER;
    }
    return (int)v;
}

/* ---- availability ------------------------------------------------------------ */

const char *zbx_avail_word(enum zbx_avail a)
{
    switch (a) {
    case ZBX_AVAIL_UP:
        return "UP";
    case ZBX_AVAIL_DOWN:
        return "DOWN";
    case ZBX_AVAIL_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

enum zbx_avail zbx_avail_combine(enum zbx_avail host, long iface_available, bool *seen_up)
{
    if (host == ZBX_AVAIL_DOWN || iface_available == 2) {
        return ZBX_AVAIL_DOWN;
    }
    if (iface_available == 1) {
        if (seen_up) {
            *seen_up = true;
        }
        /* One available interface and one never checked is still UP: the
         * frontend shows the unchecked one grey, not the host red. */
        return ZBX_AVAIL_UP;
    }
    return host;
}

/* ---- text -------------------------------------------------------------------- */

/* The length of the UTF-8 sequence that starts with byte c, or 0 when c
 * cannot start one (a continuation byte or an invalid lead). */
static size_t utf8_len(unsigned char c)
{
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return c >= 0xC2 ? 2 : 0;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return c <= 0xF4 ? 4 : 0;
    }
    return 0;
}

bool zbx_copy_text(char *dst, size_t dstlen, const char *src)
{
    size_t o = 0;
    size_t i = 0;
    const unsigned char *s = (const unsigned char *)(src ? src : "");

    if (!dst || dstlen == 0) {
        return false;
    }
    while (s[i]) {
        size_t n = utf8_len(s[i]);
        size_t k;
        bool valid = n > 0;

        for (k = 1; valid && k < n; k++) {
            if ((s[i + k] & 0xC0) != 0x80) {
                valid = false;
            }
        }
        if (!valid) {
            n = 1; /* a stray byte becomes one '?' */
        }
        if (o + n >= dstlen) {
            /* It does not fit: end in "..." where there is room for it,
             * backing off whole characters only. */
            size_t dots = dstlen >= 4 ? 3 : dstlen - 1;

            while (o + dots >= dstlen && o > 0) {
                do {
                    o--;
                } while (o > 0 && ((unsigned char)dst[o] & 0xC0) == 0x80);
            }
            memset(dst + o, '.', dots);
            dst[o + dots] = '\0';
            return false;
        }
        if (!valid) {
            dst[o++] = '?';
        } else if (n == 1 && (s[i] < 0x20 || s[i] == 0x7F)) {
            dst[o++] = ' ';
        } else {
            memcpy(dst + o, s + i, n);
            o += n;
        }
        i += n;
    }
    dst[o] = '\0';
    return true;
}

static bool all_digits(const char *s)
{
    if (!*s) {
        return false;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
    }
    return true;
}

int zbx_id_cmp(const char *a, const char *b)
{
    bool da = all_digits(a);
    bool db = all_digits(b);
    size_t la;
    size_t lb;

    if (da != db) {
        return da ? -1 : 1;
    }
    if (!da) {
        return strcmp(a, b);
    }
    while (*a == '0' && a[1]) {
        a++;
    }
    while (*b == '0' && b[1]) {
        b++;
    }
    la = strlen(a);
    lb = strlen(b);
    if (la != lb) {
        return la < lb ? -1 : 1;
    }
    return strcmp(a, b);
}

/* ---- sorting ------------------------------------------------------------------- */

static int problem_cmp(const void *pa, const void *pb)
{
    const struct zbx_problem *a = pa;
    const struct zbx_problem *b = pb;

    if (a->severity != b->severity) {
        return a->severity > b->severity ? -1 : 1;
    }
    if (a->clock != b->clock) {
        return a->clock > b->clock ? -1 : 1;
    }
    return -zbx_id_cmp(a->eventid, b->eventid);
}

void zbx_problems_sort(struct zbx_problem *p, int n)
{
    if (p && n > 1) {
        qsort(p, (size_t)n, sizeof(*p), problem_cmp);
    }
}

static int avail_rank(enum zbx_avail a)
{
    switch (a) {
    case ZBX_AVAIL_DOWN:
        return 0;
    case ZBX_AVAIL_UNKNOWN:
        return 1;
    case ZBX_AVAIL_UP:
    default:
        return 2;
    }
}

static int host_cmp(const void *pa, const void *pb)
{
    const struct zbx_host *a = pa;
    const struct zbx_host *b = pb;
    int c;

    if (a->max_severity != b->max_severity) {
        return a->max_severity > b->max_severity ? -1 : 1;
    }
    if (a->maintenance != b->maintenance) {
        return a->maintenance ? 1 : -1;
    }
    if (avail_rank(a->avail) != avail_rank(b->avail)) {
        return avail_rank(a->avail) < avail_rank(b->avail) ? -1 : 1;
    }
    c = strcasecmp(a->name, b->name);
    if (c != 0) {
        return c;
    }
    return zbx_id_cmp(a->hostid, b->hostid);
}

void zbx_hosts_sort(struct zbx_host *h, int n)
{
    if (h && n > 1) {
        qsort(h, (size_t)n, sizeof(*h), host_cmp);
    }
}

/* ---- counting ---------------------------------------------------------------- */

void zbx_problem_set_count(struct zbx_problem_set *s)
{
    int i;

    s->unacknowledged = 0;
    memset(s->sev_count, 0, sizeof(s->sev_count));
    for (i = 0; i < s->count; i++) {
        int sev = s->p[i].severity;

        if (!s->p[i].acknowledged) {
            s->unacknowledged++;
        }
        if (sev >= 0 && sev < ZBX_SEV_COUNT) {
            s->sev_count[sev]++;
        }
    }
    s->sev_exact = s->total_exact && s->count == s->total;
}

void zbx_hosts_apply(struct zbx_host *h, int nh, const struct zbx_problem *p, int np)
{
    int i;
    int k;

    for (i = 0; i < nh; i++) {
        h[i].problems = 0;
        h[i].max_severity = ZBX_SEV_NONE;
    }
    for (k = 0; p && k < np; k++) {
        if (!p[k].hostid[0]) {
            continue;
        }
        for (i = 0; i < nh; i++) {
            if (strcmp(h[i].hostid, p[k].hostid) == 0) {
                h[i].problems++;
                if (p[k].severity > h[i].max_severity) {
                    h[i].max_severity = p[k].severity;
                }
                break;
            }
        }
    }
}

void zbx_host_set_count(struct zbx_host_set *s)
{
    int i;

    s->down = 0;
    s->unknown = 0;
    s->maintenance = 0;
    for (i = 0; i < s->count; i++) {
        if (s->h[i].avail == ZBX_AVAIL_DOWN) {
            s->down++;
        } else if (s->h[i].avail == ZBX_AVAIL_UNKNOWN) {
            s->unknown++;
        }
        if (s->h[i].maintenance) {
            s->maintenance++;
        }
    }
}

void zbx_overview_build(struct zbx_overview *o, const struct zbx_problem_set *problems,
                        const struct zbx_host_set *hosts)
{
    int s;

    memset(o, 0, sizeof(*o));
    o->max_severity = ZBX_SEV_NONE;
    if (hosts) {
        o->hosts_total = hosts->total;
        o->hosts_exact = hosts->total_exact && hosts->fetched >= hosts->total;
        o->hosts_down = hosts->down;
        o->down_exact = o->hosts_exact || hosts->down_exact;
        o->hosts_unknown = hosts->unknown;
        o->hosts_maintenance = hosts->maintenance;
    }
    if (problems) {
        o->problems_total = problems->total;
        o->problems_exact = problems->total_exact && problems->sev_exact;
        o->unacknowledged = problems->unacknowledged;
        memcpy(o->sev_count, problems->sev_count, sizeof(o->sev_count));
        for (s = ZBX_SEV_COUNT - 1; s >= 0; s--) {
            if (o->sev_count[s] > 0) {
                o->max_severity = (int8_t)s;
                break;
            }
        }
    }
}

/* ---- age ----------------------------------------------------------------------- */

void zbx_format_age(char *out, size_t len, int64_t now, int64_t clock)
{
    int64_t d;

    if (!out || len == 0) {
        return;
    }
    if (clock <= 0 || now <= 0 || clock - now > 60) {
        snprintf(out, len, "--");
        return;
    }
    d = now - clock;
    if (d < 0) {
        d = 0; /* a minute of clock skew between two machines is "now" */
    }
    if (d < 10) {
        snprintf(out, len, "now");
    } else if (d < 60) {
        snprintf(out, len, "%llds", (long long)d);
    } else if (d < 3600) {
        snprintf(out, len, "%lldm", (long long)(d / 60));
    } else if (d < 86400) {
        long long h = d / 3600;
        long long m = (d % 3600) / 60;

        if (m) {
            snprintf(out, len, "%lldh %lldm", h, m);
        } else {
            snprintf(out, len, "%lldh", h);
        }
    } else if (d < 86400LL * 10) {
        long long dd = d / 86400;
        long long h = (d % 86400) / 3600;

        if (h) {
            snprintf(out, len, "%lldd %lldh", dd, h);
        } else {
            snprintf(out, len, "%lldd", dd);
        }
    } else {
        snprintf(out, len, "%lldd", (long long)(d / 86400));
    }
}
