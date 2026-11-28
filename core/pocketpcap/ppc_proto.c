/*
 * PacketShark's line protocol. See ppc_proto.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_proto.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ppc_sanitize(char *s)
{
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (c < 0x20 || c >= 0x7f) {
            *s = '.';
        }
    }
}

void ppc_sanitize_marks(char *s)
{
    unsigned char *p = (unsigned char *)s;

    while (p && *p) {
        if (p[0] == 0xc2 && p[1] == 0xb7) {
            p += 2; /* the middle dot */
        } else if (p[0] == 0xe2 && p[1] == 0x86 && p[2] == 0x92) {
            p += 3; /* the right arrow */
        } else {
            if (*p < 0x20 || *p >= 0x7f) {
                *p = '.';
            }
            p++;
        }
    }
}

/* ---- names ------------------------------------------------------------------------- */

static const char *const state_names[] = { "idle", "capturing", "hold", "saving", "stopped" };
static const char *const reason_names[] = { "none", "user", "limit", "link", "end", "error" };
static const char *const cfilter_ids[PPC_CFILTER_COUNT] = { "all", "ip", "names", "noself" };

const char *ppc_state_name(enum ppc_state s)
{
    return (unsigned)s < sizeof(state_names) / sizeof(state_names[0]) ? state_names[s] : "idle";
}

const char *ppc_reason_name(enum ppc_reason r)
{
    return (unsigned)r < sizeof(reason_names) / sizeof(reason_names[0]) ? reason_names[r] : "none";
}

const char *ppc_cfilter_id(enum ppc_cfilter f)
{
    return (unsigned)f < PPC_CFILTER_COUNT ? cfilter_ids[f] : "all";
}

int ppc_cfilter_parse(const char *id)
{
    int i;

    for (i = 0; id && i < PPC_CFILTER_COUNT; i++) {
        if (strcmp(id, cfilter_ids[i]) == 0) {
            return i;
        }
    }
    return -1;
}

uint8_t ppc_cfilter_excludes(enum ppc_cfilter f)
{
    switch (f) {
    case PPC_CFILTER_NAMES:
        return (uint8_t)(1u << PPC_CLASS_ARP);
    case PPC_CFILTER_IP:
    case PPC_CFILTER_NOSELF:
    case PPC_CFILTER_ALL:
    default:
        return 0;
    }
}

const char *ppc_cfilter_expr(enum ppc_cfilter f, const char *self, char *buf, size_t buflen)
{
    switch (f) {
    case PPC_CFILTER_IP:
        snprintf(buf, buflen, "ip or ip6 or arp");
        break;
    case PPC_CFILTER_NAMES:
        snprintf(buf, buflen,
                 "port 53 or port 67 or port 68 or port 5353 or port 5355 or port 546 or port 547");
        break;
    case PPC_CFILTER_NOSELF:
        if (self && *self) {
            snprintf(buf, buflen, "not (tcp port 22 and host %s)", self);
        } else {
            snprintf(buf, buflen, "not tcp port 22");
        }
        break;
    case PPC_CFILTER_ALL:
    default:
        snprintf(buf, buflen, "%s", "");
        break;
    }
    return buf;
}

static int parse_state(const char *w)
{
    unsigned i;

    for (i = 0; i < sizeof(state_names) / sizeof(state_names[0]); i++) {
        if (strcmp(w, state_names[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int parse_reason(const char *w)
{
    unsigned i;

    for (i = 0; i < sizeof(reason_names) / sizeof(reason_names[0]); i++) {
        if (strcmp(w, reason_names[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* ---- fields ------------------------------------------------------------------------ */

/* Split a copy of line into fields. Tabs always separate; spaces too when
 * spaces is set (commands). Runs of separators count as one only for
 * spaces, so an empty tab field stays a field. Returns the count, or -1 for
 * a line too long or with too many fields. */
static int split(const char *line, char *buf, size_t buflen, char **f, int max, int spaces)
{
    size_t len = strlen(line);
    int n = 0;
    char *p;

    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    if (len >= buflen || len >= PPC_LINE_MAX) {
        return -1;
    }
    memcpy(buf, line, len);
    buf[len] = '\0';
    p = buf;
    if (spaces) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            return 0;
        }
    }
    for (;;) {
        char *s = p;

        if (n == max) {
            return -1;
        }
        f[n++] = p;
        while (*s && *s != '\t' && !(spaces && *s == ' ')) {
            s++;
        }
        if (!*s) {
            break;
        }
        *s++ = '\0';
        if (spaces) {
            while (*s == ' ' || *s == '\t') {
                s++;
            }
            if (!*s) {
                break;
            }
        }
        p = s;
    }
    return n;
}

static int u64(const char *s, uint64_t *out)
{
    char *end;
    unsigned long long v;

    if (!s || *s < '0' || *s > '9' || strlen(s) > 20) {
        return -1;
    }
    v = strtoull(s, &end, 10);
    if (*end) {
        return -1;
    }
    *out = v;
    return 0;
}

static int u32(const char *s, uint32_t *out)
{
    uint64_t v;

    if (u64(s, &v) != 0 || v > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static int copy(char *dst, size_t n, const char *src)
{
    size_t len = strlen(src);

    if (len >= n) {
        return -1;
    }
    memcpy(dst, src, len + 1);
    return 0;
}

/* "-" on the line stands for an empty field. */
static void none_dash(char *s)
{
    if (strcmp(s, "-") == 0) {
        s[0] = '\0';
    }
}

int ppc_iface_ok(const char *name)
{
    size_t i;
    size_t len = name ? strlen(name) : 0;

    if (len == 0 || len >= PPC_IFACE_MAX || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c <= ' ' || c >= 0x7f || c == '/' || c == ':') {
            return 0;
        }
    }
    return 1;
}

static const char *const sort_words[] = { "time", "newest", "len" };

static int sort_parse(const char *w)
{
    unsigned i;

    for (i = 0; i < sizeof(sort_words) / sizeof(sort_words[0]); i++) {
        if (strcmp(w, sort_words[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* An address as a view names its host: hex digits, dots and colons only
 * (IPv4, IPv6, MAC). The helper parses it properly; this keeps anything
 * else off the line. */
static int host_ok(const char *h)
{
    size_t i;
    size_t len = strlen(h);

    if (len == 0 || len >= PPC_HOST_TEXT) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        char c = h[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '.' || c == ':')) {
            return 0;
        }
    }
    return 1;
}

/* ---- commands ---------------------------------------------------------------------- */

enum ppc_cmd_kind ppc_cmd_parse(const char *line, struct ppc_cmd *c)
{
    char buf[PPC_LINE_MAX];
    char *f[PPC_FIELDS_MAX];
    int n = split(line, buf, sizeof(buf), f, PPC_FIELDS_MAX, 1);
    uint32_t v;

    memset(c, 0, sizeof(*c));
    if (n <= 0) {
        c->kind = n < 0 ? PPC_CMD_BAD : PPC_CMD_NONE;
        return c->kind;
    }
    c->kind = PPC_CMD_BAD;
    if (strcmp(f[0], "start") == 0) {
        int cf;

        if (n != 6 || !ppc_iface_ok(f[1]) || copy(c->iface, sizeof(c->iface), f[1]) != 0) {
            return c->kind;
        }
        if (strcmp(f[2], "headers") == 0) {
            c->keep = PPC_KEEP_HEADERS;
        } else if (strcmp(f[2], "full") == 0) {
            c->keep = PPC_KEEP_FULL;
        } else {
            return c->kind;
        }
        if (u32(f[3], &v) != 0 || v > 1) {
            return c->kind;
        }
        c->promisc = (uint8_t)v;
        cf = ppc_cfilter_parse(f[4]);
        if (cf < 0 || u32(f[5], &c->limit_s) != 0) {
            return c->kind;
        }
        c->cfilter = (uint8_t)cf;
        c->kind = PPC_CMD_START;
    } else if (strcmp(f[0], "hold") == 0) {
        if (n != 2 || u32(f[1], &v) != 0 || v > 1) {
            return c->kind;
        }
        c->on = (uint8_t)v;
        c->kind = PPC_CMD_HOLD;
    } else if (strcmp(f[0], "marks") == 0) {
        if (n != 3 || u32(f[1], &c->req) != 0 || u32(f[2], &c->buckets) != 0 || c->buckets == 0 ||
            c->buckets > PPC_MARK_BUCKETS) {
            return c->kind;
        }
        c->kind = PPC_CMD_MARKS;
    } else if (strcmp(f[0], "net") == 0) {
        if (n != 2 || u32(f[1], &c->req) != 0) {
            return c->kind;
        }
        c->kind = PPC_CMD_NET;
    } else if (strcmp(f[0], "mark") == 0) {
        c->kind = n == 1 ? PPC_CMD_MARK : PPC_CMD_BAD;
    } else if (strcmp(f[0], "evid") == 0) {
        if (n != 3 || u32(f[1], &c->req) != 0 || u32(f[2], &c->mark_id) != 0) {
            return c->kind;
        }
        c->kind = PPC_CMD_EVID;
    } else if (strcmp(f[0], "intel") == 0) {
        uint32_t pair = 0;

        if (n != 4 || u32(f[1], &c->req) != 0 || u32(f[2], &c->mark_id) != 0 || u32(f[3], &pair) != 0 || pair > 1u) {
            return c->kind;
        }
        c->pair = (uint8_t)pair;
        c->kind = PPC_CMD_INTEL;
    } else if (strcmp(f[0], "save") == 0) {
        c->kind = n == 1 ? PPC_CMD_SAVE : PPC_CMD_BAD;
    } else if (strcmp(f[0], "stop") == 0) {
        c->kind = n == 1 ? PPC_CMD_STOP : PPC_CMD_BAD;
    } else if (strcmp(f[0], "discard") == 0) {
        c->kind = n == 1 ? PPC_CMD_DISCARD : PPC_CMD_BAD;
    } else if (strcmp(f[0], "stats") == 0) {
        c->kind = n == 1 ? PPC_CMD_STATS : PPC_CMD_BAD;
    } else if (strcmp(f[0], "quit") == 0) {
        c->kind = n == 1 ? PPC_CMD_QUIT : PPC_CMD_BAD;
    } else if (strcmp(f[0], "window") == 0) {
        if (n != 5 || u32(f[1], &c->req) != 0 || u32(f[3], &c->before) != 0 ||
            u32(f[4], &c->after) != 0) {
            return c->kind;
        }
        if (strcmp(f[2], "tail") == 0) {
            c->tail = 1;
            c->anchor = PPC_ANCHOR_TAIL;
        } else if (strncmp(f[2], "seq:", 4) == 0 && u32(f[2] + 4, &c->seq) == 0 && c->seq > 0) {
            c->tail = 0;
            c->anchor = PPC_ANCHOR_SEQ;
        } else if (strncmp(f[2], "rank:", 5) == 0 && u32(f[2] + 5, &v) == 0) {
            c->anchor = PPC_ANCHOR_RANK;
            c->at = v;
        } else if (strncmp(f[2], "us:", 3) == 0 && u64(f[2] + 3, &c->at) == 0) {
            c->anchor = PPC_ANCHOR_US;
        } else {
            return c->kind;
        }
        c->kind = PPC_CMD_WINDOW;
    } else if (strcmp(f[0], "view") == 0) {
        int sort;
        unsigned long classes;
        char *end;

        if ((n != 7 && n != 8 && n != 9) || u32(f[1], &c->view_id) != 0) {
            return c->kind;
        }
        if (n >= 8) {
            unsigned long sig = strtoul(f[7], &end, 16);

            if (*end || f[7][0] == '\0' || strlen(f[7]) > 2 || sig > PPC_SIG_FINDINGS_ALL) {
                return c->kind;
            }
            c->signals = (uint8_t)sig;
        }
        if (n == 9) {
            if (u32(f[8], &v) != 0 || v >= PPC_DIAG_COUNT) {
                return c->kind;
            }
            c->diag = (uint8_t)v;
        }
        sort = sort_parse(f[2]);
        classes = strtoul(f[3], &end, 16);
        if (sort < 0 || *end || f[3][0] == '\0' || strlen(f[3]) > 2 || classes > 0xff) {
            return c->kind;
        }
        if (strcmp(f[4], "-") != 0 && (!host_ok(f[4]) || copy(c->host, sizeof(c->host), f[4]) != 0)) {
            return c->kind;
        }
        if (u32(f[5], &v) != 0 || v > 65535) {
            return c->kind;
        }
        c->port = (uint16_t)v;
        if (u32(f[6], &v) != 0 || v > 1) {
            return c->kind;
        }
        c->pinned = (uint8_t)v;
        c->sort = (uint8_t)sort;
        c->classes = (uint8_t)classes;
        c->kind = PPC_CMD_VIEW;
    } else if (strcmp(f[0], "inspect") == 0) {
        if (n != 4 || u32(f[1], &c->req) != 0 || u32(f[2], &c->seq) != 0 || c->seq == 0) {
            return c->kind;
        }
        if (strcmp(f[3], "-1") == 0) {
            c->step = -1;
        } else if (strcmp(f[3], "0") == 0) {
            c->step = 0;
        } else if (strcmp(f[3], "1") == 0) {
            c->step = 1;
        } else {
            return c->kind;
        }
        c->kind = PPC_CMD_INSPECT;
    } else if (strcmp(f[0], "pin") == 0) {
        if (n != 3 || u32(f[1], &c->seq) != 0 || c->seq == 0 || u32(f[2], &v) != 0 || v > 1) {
            return c->kind;
        }
        c->on = (uint8_t)v;
        c->kind = PPC_CMD_PIN;
    } else {
        c->kind = PPC_CMD_NONE;
    }
    return c->kind;
}

static int fmt(char *buf, size_t n, const char *f, ...) __attribute__((format(printf, 3, 4)));

static int fmt(char *buf, size_t n, const char *f, ...)
{
    va_list ap;
    int len;

    if (!buf || n == 0) {
        return -1;
    }
    va_start(ap, f);
    len = vsnprintf(buf, n, f, ap);
    va_end(ap);
    if (len < 0 || (size_t)len >= n || len >= PPC_LINE_MAX) {
        buf[0] = '\0';
        return -1;
    }
    return len;
}

int ppc_cmd_format_start(char *buf, size_t n, const char *iface, enum ppc_keep keep, int promisc,
                         enum ppc_cfilter cf, uint32_t limit_s)
{
    if (!ppc_iface_ok(iface) || (unsigned)cf >= PPC_CFILTER_COUNT) {
        return -1;
    }
    return fmt(buf, n, "start\t%s\t%s\t%d\t%s\t%u", iface, keep == PPC_KEEP_FULL ? "full" : "headers",
               promisc ? 1 : 0, ppc_cfilter_id(cf), (unsigned)limit_s);
}

int ppc_cmd_format_hold(char *buf, size_t n, int on)
{
    return fmt(buf, n, "hold\t%d", on ? 1 : 0);
}

int ppc_cmd_format_window(char *buf, size_t n, uint32_t req, uint32_t seq, uint32_t before,
                          uint32_t after)
{
    if (seq == 0) {
        return fmt(buf, n, "window\t%u\ttail\t%u\t%u", (unsigned)req, (unsigned)before, (unsigned)after);
    }
    return fmt(buf, n, "window\t%u\tseq:%u\t%u\t%u", (unsigned)req, (unsigned)seq, (unsigned)before,
               (unsigned)after);
}

int ppc_cmd_format_window_at(char *buf, size_t n, uint32_t req, enum ppc_anchor anchor, uint64_t at,
                             uint32_t before, uint32_t after)
{
    switch (anchor) {
    case PPC_ANCHOR_TAIL:
        return ppc_cmd_format_window(buf, n, req, 0, before, after);
    case PPC_ANCHOR_SEQ:
        if (at == 0 || at > UINT32_MAX) {
            return -1;
        }
        return ppc_cmd_format_window(buf, n, req, (uint32_t)at, before, after);
    case PPC_ANCHOR_RANK:
        if (at > UINT32_MAX) {
            return -1;
        }
        return fmt(buf, n, "window\t%u\trank:%u\t%u\t%u", (unsigned)req, (unsigned)at, (unsigned)before,
                   (unsigned)after);
    case PPC_ANCHOR_US:
        return fmt(buf, n, "window\t%u\tus:%llu\t%u\t%u", (unsigned)req, (unsigned long long)at, (unsigned)before,
                   (unsigned)after);
    default:
        return -1;
    }
}

int ppc_cmd_format_view_diag(char *buf, size_t n, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                             uint16_t port, int pinned, unsigned signals, unsigned diag)
{
    if (sort >= sizeof(sort_words) / sizeof(sort_words[0]) || (host && *host && !host_ok(host)) ||
        signals > PPC_SIG_FINDINGS_ALL || diag >= PPC_DIAG_COUNT) {
        return -1;
    }
    if (diag != 0) {
        return fmt(buf, n, "view\t%u\t%s\t%x\t%s\t%u\t%d\t%x\t%u", (unsigned)id, sort_words[sort], (unsigned)classes,
                   host && *host ? host : "-", (unsigned)port, pinned ? 1 : 0, signals, diag);
    }
    if (signals == 0) {
        /* The protocol-2 line: a helper that knows no findings reads it. */
        return fmt(buf, n, "view\t%u\t%s\t%x\t%s\t%u\t%d", (unsigned)id, sort_words[sort], (unsigned)classes,
                   host && *host ? host : "-", (unsigned)port, pinned ? 1 : 0);
    }
    return fmt(buf, n, "view\t%u\t%s\t%x\t%s\t%u\t%d\t%x", (unsigned)id, sort_words[sort], (unsigned)classes,
               host && *host ? host : "-", (unsigned)port, pinned ? 1 : 0, signals);
}

int ppc_cmd_format_view_signals(char *buf, size_t n, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                                uint16_t port, int pinned, unsigned signals)
{
    return ppc_cmd_format_view_diag(buf, n, id, sort, classes, host, port, pinned, signals, 0);
}

int ppc_cmd_format_view(char *buf, size_t n, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                        uint16_t port, int pinned)
{
    return ppc_cmd_format_view_signals(buf, n, id, sort, classes, host, port, pinned, 0);
}

int ppc_cmd_format_marks(char *buf, size_t n, uint32_t req, uint32_t buckets)
{
    if (buckets == 0 || buckets > PPC_MARK_BUCKETS) {
        return -1;
    }
    return fmt(buf, n, "marks\t%u\t%u", (unsigned)req, (unsigned)buckets);
}

int ppc_cmd_format_inspect(char *buf, size_t n, uint32_t req, uint32_t seq, int step)
{
    if (seq == 0 || step < -1 || step > 1) {
        return -1;
    }
    return fmt(buf, n, "inspect\t%u\t%u\t%d", (unsigned)req, (unsigned)seq, step);
}

int ppc_cmd_format_pin(char *buf, size_t n, uint32_t seq, int on)
{
    if (seq == 0) {
        return -1;
    }
    return fmt(buf, n, "pin\t%u\t%d", (unsigned)seq, on ? 1 : 0);
}

int ppc_cmd_format_net(char *buf, size_t n, uint32_t req)
{
    return fmt(buf, n, "net\t%u", (unsigned)req);
}

int ppc_cmd_format_mark(char *buf, size_t n)
{
    return fmt(buf, n, "mark");
}

int ppc_cmd_format_evid(char *buf, size_t n, uint32_t req, uint32_t id)
{
    return fmt(buf, n, "evid\t%u\t%u", (unsigned)req, (unsigned)id);
}

int ppc_cmd_format_intel(char *buf, size_t n, uint32_t req, uint32_t id, unsigned pair)
{
    if (pair > 1u) {
        return -1;
    }
    return fmt(buf, n, "intel\t%u\t%u\t%u", (unsigned)req, (unsigned)id, pair);
}

/* ---- events ------------------------------------------------------------------------- */

/* Free text for a field: no tabs, newlines or anything else unprintable. */
static void field_text(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src && *src ? src : "-");
    ppc_sanitize(dst);
}

/* The same for the inspector's own words, which may carry the two marks. A
 * mark cut in half by the buffer's end is turned into dots like any other
 * stray byte. */
static void mark_text(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src && *src ? src : "-");
    ppc_sanitize_marks(dst);
}

int ppc_evt_format_hello(char *buf, size_t n, const char *version, const char *backend)
{
    char v[32];
    char b[16];

    field_text(v, sizeof(v), version);
    field_text(b, sizeof(b), backend);
    return fmt(buf, n, "hello\t%d\t%s\t%s\n", PPC_PROTO_VERSION, v, b);
}

int ppc_evt_format_state(char *buf, size_t n, enum ppc_state s, enum ppc_reason r, const char *iface,
                         uint32_t speed, int clock_set)
{
    return fmt(buf, n, "state\t%s\t%s\t%s\t%u\t%d\n", ppc_state_name(s), ppc_reason_name(r),
               ppc_iface_ok(iface) ? iface : "-", (unsigned)speed, clock_set ? 1 : 0);
}

int ppc_evt_format_stats(char *buf, size_t n, const struct ppc_counters *c)
{
    return fmt(buf, n,
               "stats\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%u\t%u\t%u\t%u\t%llu"
               "\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\n",
               (unsigned long long)c->elapsed_ms, (unsigned long long)c->seen,
               (unsigned long long)c->stored, (unsigned long long)c->held, (unsigned long long)c->kdrop,
               (unsigned long long)c->ifdrop, (unsigned long long)c->shed, (unsigned long long)c->bytes,
               (unsigned long long)c->pps, (unsigned long long)c->bps, (unsigned)c->first_seq,
               (unsigned)c->last_seq, (unsigned)c->match, (unsigned)c->fill_permille,
               (unsigned long long)c->ui_skipped, (unsigned long long)c->cls[0],
               (unsigned long long)c->cls[1], (unsigned long long)c->cls[2], (unsigned long long)c->cls[3],
               (unsigned long long)c->cls[4], (unsigned long long)c->cls[5], (unsigned long long)c->cls[6],
               (unsigned long long)c->cls[7], (unsigned)c->sig[0], (unsigned)c->sig[1], (unsigned)c->sig[2],
               (unsigned)c->sig[3], (unsigned)c->sig[4], (unsigned)c->sig[5]);
}

/* The diagnosis's figures in struct order: every one but the two sums is
 * 32 bits. */
#define DIAG_LIST(X)                                                                                                   \
    X(dns_queries) X(dns_retries) X(dns_answered) X(dns_late) X(dns_noreply) X(dns_waiting) X(dns_orphan)            \
    X(dns_unjudged) X(dns_nxdomain) X(dns_servfail) X(dns_refused) X(dns_other) X(dns_rtt_sum_us) X(dns_rtt_max_us) \
    X(dns_rtt_max_seq) X(tcp_attempts) X(tcp_connected) X(tcp_timed) X(tcp_hs_max_us) X(tcp_hs_sum_us)               \
    X(tcp_hs_max_seq) X(tcp_refused) X(tcp_aborted) X(tcp_rst_closed) X(tcp_rst_unseen) X(tcp_segments)              \
    X(tcp_retrans) X(tcp_retrans_acked) X(tcp_retrans_unseen) X(tcp_syn_again) X(tcp_ooo) X(tcp_midstream)           \
    X(tcp_flows) X(tcp_oneway) X(tcp_replaced) X(blinded)
#define DIAG_ONE(name) +1
_Static_assert((0 DIAG_LIST(DIAG_ONE)) == PPC_DIAG_FIELDS, "the diag line names every figure of struct ppc_diag");
_Static_assert(sizeof(struct ppc_diag) == (PPC_DIAG_FIELDS + 2) * 4, "struct ppc_diag: 34 figures of 32 bits and 2 of 64, no padding");
#undef DIAG_ONE

int ppc_evt_format_diag(char *buf, size_t n, const struct ppc_diag *d)
{
    size_t o;
    int k;

    if (!buf || n < 8) {
        return -1;
    }
    memcpy(buf, "diag", 4);
    o = 4;
#define DIAG_OUT(name)                                                                                                 \
    k = snprintf(buf + o, n - o, "\t%llu", (unsigned long long)d->name);                                              \
    if (k < 0 || (size_t)k >= n - o) {                                                                                 \
        return -1;                                                                                                     \
    }                                                                                                                  \
    o += (size_t)k;
    DIAG_LIST(DIAG_OUT)
#undef DIAG_OUT
    if (o + 2 > n) {
        return -1;
    }
    buf[o++] = '\n';
    buf[o] = '\0';
    return (int)o;
}

/* ---- the network intelligence (M12.1) ----------------------------------------------- */

/* The totals' figures in struct order, all 32 bits. */
#define NET_LIST(X)                                                                                                    \
    X(hosts) X(hosts_replaced) X(arp_requests) X(arp_replies) X(arp_gratuitous) X(arp_probes) X(arp_other)            \
    X(arp_binds) X(arp_moved) X(arp_flips) X(arp_conflicts) X(arp_noreply) X(dhcp_discover) X(dhcp_offer)             \
    X(dhcp_request) X(dhcp_ack) X(dhcp_nak) X(dhcp_decline) X(dhcp_release) X(dhcp_inform) X(dhcp_tx)                 \
    X(dhcp_complete) X(dhcp_no_offer) X(dhcp_no_ack) X(dhcp_unseen) X(dhcp_unjudged) X(dhcp_late) X(dhcp_waiting)     \
    X(dhcp_servers) X(events_warn) X(events_note) X(events_lost) X(blinded)
#define NET_ONE(name) +1
_Static_assert((0 NET_LIST(NET_ONE)) == PPC_NET_FIELDS, "the net line names every figure of struct ppc_net_totals");
_Static_assert(sizeof(struct ppc_net_totals) == PPC_NET_FIELDS * 4, "struct ppc_net_totals: figures of 32 bits, no padding");
#undef NET_ONE

int ppc_evt_format_net(char *buf, size_t n, const struct ppc_net_totals *t)
{
    size_t o;
    int k;

    if (!buf || n < 8) {
        return -1;
    }
    memcpy(buf, "net", 3);
    o = 3;
#define NET_OUT(name)                                                                                                  \
    k = snprintf(buf + o, n - o, "\t%u", (unsigned)t->name);                                                          \
    if (k < 0 || (size_t)k >= n - o) {                                                                                 \
        return -1;                                                                                                     \
    }                                                                                                                  \
    o += (size_t)k;
    NET_LIST(NET_OUT)
#undef NET_OUT
    if (o + 2 > n) {
        return -1;
    }
    buf[o++] = '\n';
    buf[o] = '\0';
    return (int)o;
}

/* An address as the helper writes one: an IPv4 address in dotted form or a
 * MAC with colons, or nothing. Anything else is not put on a line and not
 * taken from one. */
static int addr_ok(const char *s, size_t max, int mac)
{
    size_t i;
    size_t len = strlen(s);

    if (len >= max) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        char c = s[i];

        if (!((c >= '0' && c <= '9') || (mac ? ((c >= 'a' && c <= 'f') || c == ':') : c == '.'))) {
            return 0;
        }
    }
    return 1;
}

static const char *dash(const char *s)
{
    return s && *s ? s : "-";
}

int ppc_evt_format_net_begin(char *buf, size_t n, uint32_t req)
{
    return fmt(buf, n, "net.begin\t%u\n", (unsigned)req);
}

int ppc_evt_format_net_end(char *buf, size_t n, uint32_t req)
{
    return fmt(buf, n, "net.end\t%u\n", (unsigned)req);
}

int ppc_evt_format_net_host(char *buf, size_t n, const struct ppc_evt_net_host *h)
{
    char name[PPC_NET_NAME];

    if (!h->mac[0] || !addr_ok(h->mac, PPC_MAC_TEXT, 1) || !addr_ok(h->ip, PPC_IP_TEXT, 0)) {
        return -1;
    }
    field_text(name, sizeof(name), h->name);
    return fmt(buf, n, "net.host\t%s\t%s\t%u\t%u\t%llu\t%s\n", h->mac, dash(h->ip), (unsigned)(h->flags & PPC_NH_ALL),
               (unsigned)h->pkts, (unsigned long long)h->bytes, name);
}

int ppc_evt_format_net_srv(char *buf, size_t n, const struct ppc_evt_net_srv *s)
{
    if (!s->ip[0] || !addr_ok(s->ip, PPC_IP_TEXT, 0) || !addr_ok(s->mac, PPC_MAC_TEXT, 1) ||
        !addr_ok(s->router, PPC_IP_TEXT, 0) || !addr_ok(s->dns, PPC_IP_TEXT, 0) || !addr_ok(s->mask, PPC_IP_TEXT, 0)) {
        return -1;
    }
    return fmt(buf, n, "net.srv\t%s\t%s\t%d\t%u\t%u\t%u\t%u\t%u\t%s\t%s\t%s\n", s->ip, dash(s->mac), s->relayed ? 1 : 0,
               (unsigned)s->offers, (unsigned)s->acks, (unsigned)s->naks, (unsigned)s->seq, (unsigned)s->lease_s,
               dash(s->router), dash(s->dns), dash(s->mask));
}

int ppc_evt_format_net_lease(char *buf, size_t n, const struct ppc_evt_net_lease *l)
{
    char name[PPC_NET_NAME];

    if (!l->ip[0] || !addr_ok(l->ip, PPC_IP_TEXT, 0) || !addr_ok(l->mac, PPC_MAC_TEXT, 1) ||
        !addr_ok(l->server, PPC_IP_TEXT, 0)) {
        return -1;
    }
    field_text(name, sizeof(name), l->name);
    return fmt(buf, n, "net.lease\t%s\t%s\t%s\t%u\t%llu\t%u\t%s\n", l->ip, dash(l->mac), dash(l->server),
               (unsigned)l->lease_s, (unsigned long long)l->rel_us, (unsigned)l->seq, name);
}

int ppc_evt_format_net_evt(char *buf, size_t n, const struct ppc_evt_net_event *e)
{
    if (e->kind == PPC_NK_NONE || e->kind >= PPC_NK_COUNT || e->sev > PPC_NS_WARN || !addr_ok(e->ip, PPC_IP_TEXT, 0) ||
        !addr_ok(e->mac, PPC_MAC_TEXT, 1) || !addr_ok(e->ip2, PPC_IP_TEXT, 0) || !addr_ok(e->mac2, PPC_MAC_TEXT, 1)) {
        return -1;
    }
    return fmt(buf, n, "net.evt\t%u\t%u\t%u\t%u\t%llu\t%llu\t%s\t%s\t%s\t%s\n", (unsigned)e->kind, (unsigned)e->sev,
               (unsigned)e->count, (unsigned)e->seq, (unsigned long long)e->first_us, (unsigned long long)e->last_us,
               dash(e->ip), dash(e->mac), dash(e->ip2), dash(e->mac2));
}

/* ---- marks and their evidence (N1.0) -------------------------------------------------- */

_Static_assert(PPC_EV_KINDS == 19, "the ev.win line carries eighteen counts: a new kind is a new protocol");

int ppc_evt_format_mark(char *buf, size_t n, const struct ppc_evt_mark *m)
{
    if (!m->id || !m->first || m->first > m->id || m->state > PPC_STATE_STOPPED) {
        return -1;
    }
    return fmt(buf, n, "mark\t%u\t%u\t%u\t%s\t%u\t%llu\n", (unsigned)m->id, (unsigned)m->first, (unsigned)m->at_ms,
               ppc_state_name((enum ppc_state)m->state), (unsigned)m->seq, (unsigned long long)m->wall_s);
}

int ppc_evt_format_ev_begin(char *buf, size_t n, const struct ppc_evt_ev_head *h)
{
    char edge[64];

    if (!h->id || !h->first || h->first > h->id || h->id > h->last || h->state > PPC_STATE_STOPPED ||
        h->cfilter >= PPC_CFILTER_COUNT || (h->unanalyzed & ~PPC_EV_CLASSES) || h->edge > PPC_EV_EDGE_UNKNOWN ||
        (h->edge == PPC_EV_EDGE_COUNTED && h->edge_to_ms <= h->edge_from_ms)) {
        return -1;
    }
    if (h->edge == PPC_EV_EDGE_COUNTED) {
        snprintf(edge, sizeof(edge), "%llu\t%u\t%u", (unsigned long long)h->edge_lost, (unsigned)h->edge_from_ms,
                 (unsigned)h->edge_to_ms);
    } else if (h->edge == PPC_EV_EDGE_UNKNOWN) {
        snprintf(edge, sizeof(edge), "-\t%u\t-", (unsigned)h->edge_from_ms);
    } else {
        snprintf(edge, sizeof(edge), "0\t-\t-");
    }
    return fmt(buf, n, "ev.begin\t%u\t%u\t%u\t%u\t%u\t%s\t%u\t%llu\t%u\t%s\t%x\t%s\n", (unsigned)h->req,
               (unsigned)h->id, (unsigned)h->first, (unsigned)h->last, (unsigned)h->at_ms,
               ppc_state_name((enum ppc_state)h->state), (unsigned)h->seq, (unsigned long long)h->wall_s,
               (unsigned)h->now_ms, ppc_cfilter_id((enum ppc_cfilter)h->cfilter), (unsigned)h->unanalyzed, edge);
}

int ppc_evt_format_ev_win(char *buf, size_t n, const struct ppc_evt_ev_win *w)
{
    size_t o;
    int k;
    int i;

    if (!buf || w->side > 1 || (w->flags & ~PPC_EVW_ALL) || w->kept > PPC_EV_KEEP || w->span_ms > PPC_EV_WINDOW_MS) {
        return -1;
    }
    k = snprintf(buf, n, "ev.win\t%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%u\t%u", (unsigned)w->side, (unsigned)w->flags,
                 (unsigned)w->span_ms, (unsigned)w->watched_s, (unsigned)w->held_s, (unsigned)w->shed_s,
                 (unsigned long long)w->lost, (unsigned)w->kept, (unsigned)w->more);
    if (k < 0 || (size_t)k >= n) {
        return -1;
    }
    o = (size_t)k;
    for (i = 1; i < PPC_EV_KINDS; i++) {
        k = snprintf(buf + o, n - o, "\t%u", (unsigned)w->count[i]);
        if (k < 0 || (size_t)k >= n - o) {
            return -1;
        }
        o += (size_t)k;
    }
    if (w->unknown) {
        k = snprintf(buf + o, n - o, "\t%llu\t%u", (unsigned long long)w->lost_near, (unsigned)w->unknown_from_ms);
    } else {
        k = snprintf(buf + o, n - o, "\t%llu\t-", (unsigned long long)w->lost_near);
    }
    if (k < 0 || (size_t)k >= n - o) {
        return -1;
    }
    o += (size_t)k;
    if (o + 2 > n || o + 1 >= PPC_LINE_MAX) {
        return -1;
    }
    buf[o++] = '\n';
    buf[o] = '\0';
    return (int)o;
}

int ppc_evt_format_ev_obs(char *buf, size_t n, const struct ppc_evt_ev_obs *o)
{
    if (o->side > 1 || o->kind == PPC_EV_NONE || o->kind >= PPC_EV_KINDS || (o->flags & ~PPC_EVF_ALL) ||
        o->alive > PPC_EV_WHOLE || !o->seq || !addr_ok(o->mac, PPC_MAC_TEXT, 1) || !addr_ok(o->ip, PPC_IP_TEXT, 0) ||
        !addr_ok(o->ip2, PPC_IP_TEXT, 0)) {
        return -1;
    }
    return fmt(buf, n, "ev.obs\t%u\t%u\t%u\t%u\t%u\t%u\t%s\t%s\t%s\t%u\t%u\n", (unsigned)o->side, (unsigned)o->kind,
               (unsigned)o->flags, (unsigned)o->t_ms, (unsigned)o->seq, (unsigned)o->alive, dash(o->mac), dash(o->ip),
               dash(o->ip2), (unsigned)o->a, (unsigned)o->b);
}

int ppc_evt_format_ev_end(char *buf, size_t n, uint32_t req)
{
    return fmt(buf, n, "ev.end\t%u\n", (unsigned)req);
}

/* ---- protocol 7 (N1.2): a mark's comparisons ---- */

int ppc_evt_format_in_begin(char *buf, size_t n, const struct ppc_evt_in_head *h)
{
    if (!h->mark || h->pair > 1u || h->final > 1u) {
        return -1;
    }
    return fmt(buf, n, "in.begin\t%u\t%u\t%u\t%u\t%u\t%u\n", (unsigned)h->req, (unsigned)h->mark,
               (unsigned)h->pair, (unsigned)h->at_ms, (unsigned)h->now_ms, (unsigned)h->final);
}

int ppc_evt_format_in_vis(char *buf, size_t n, const struct ppc_evt_in_vis *v)
{
    if (v->side > 1u || v->loss_known > 1u || v->from_ms > v->to_ms) {
        return -1;
    }
    return fmt(buf, n, "in.vis\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n", (unsigned)v->side,
               (unsigned)v->from_ms, (unsigned)v->to_ms, (unsigned)v->watched_s, (unsigned)v->held_s,
               (unsigned)v->shed_s, (unsigned)v->gap_s, (unsigned)v->frames, (unsigned)v->lost_lo,
               (unsigned)v->lost_hi, (unsigned)v->loss_known, (unsigned)v->own_in, (unsigned)v->own_out);
}

int ppc_evt_format_in_cmp(char *buf, size_t n, const struct ppc_evt_in_cmp *m)
{
    char refs[PPC_IN_REF_MAX][24];
    unsigned i;

    if (m->metric >= PPC_IN_METRICS || m->verdict > 5u || m->quality > 2u || m->reason > 11u || m->notes > 0x3fu) {
        return -1;
    }
    for (i = 0; i < PPC_IN_REF_MAX; i++) {
        if (m->ref[i] && m->alive[i] > PPC_EV_WHOLE) {
            return -1;
        }
        if (m->ref[i]) {
            snprintf(refs[i], sizeof(refs[i]), "%u:%u", (unsigned)m->ref[i], (unsigned)m->alive[i]);
        } else {
            snprintf(refs[i], sizeof(refs[i]), "-");
        }
    }
    return fmt(buf, n, "in.cmp\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%s\t%s\t%s\t%s\n",
               (unsigned)m->metric, (unsigned)m->verdict, (unsigned)m->quality, (unsigned)m->reason,
               (unsigned)m->notes, (unsigned)m->n[0], (unsigned)m->n[1], (unsigned)m->k[0], (unsigned)m->k[1],
               (unsigned)m->stat[0], (unsigned)m->stat[1], (unsigned)m->und[0], (unsigned)m->und[1],
               (unsigned)m->p_e6, refs[0], refs[1], refs[2], refs[3]);
}

int ppc_evt_format_in_end(char *buf, size_t n, uint32_t req)
{
    return fmt(buf, n, "in.end\t%u\n", (unsigned)req);
}

int ppc_evt_format_bin(char *buf, size_t n, uint64_t second, const uint32_t c[PPC_CLASS_COUNT],
                       uint32_t held)
{
    return fmt(buf, n, "bin\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n", (unsigned long long)second,
               (unsigned)c[0], (unsigned)c[1], (unsigned)c[2], (unsigned)c[3], (unsigned)c[4],
               (unsigned)c[5], (unsigned)c[6], (unsigned)c[7], (unsigned)held);
}

int ppc_evt_format_rows_begin(char *buf, size_t n, uint32_t req, uint32_t filter_id,
                              uint32_t first_rank, uint32_t match, uint32_t first_seq,
                              uint32_t last_seq)
{
    return fmt(buf, n, "rows.begin\t%u\t%u\t%u\t%u\t%u\t%u\n", (unsigned)req, (unsigned)filter_id,
               (unsigned)first_rank, (unsigned)match, (unsigned)first_seq, (unsigned)last_seq);
}

int ppc_evt_format_rows_begin_at(char *buf, size_t n, uint32_t req, uint32_t filter_id, uint32_t first_rank,
                                 uint32_t match, uint32_t first_seq, uint32_t last_seq, uint32_t anchor_rank)
{
    return fmt(buf, n, "rows.begin\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n", (unsigned)req, (unsigned)filter_id,
               (unsigned)first_rank, (unsigned)match, (unsigned)first_seq, (unsigned)last_seq, (unsigned)anchor_rank);
}

int ppc_evt_format_row(char *buf, size_t n, const struct ppc_evt_row *r)
{
    char dir[sizeof(r->dir)];
    char word[sizeof(r->word)];
    char src[PPC_ADDR_TEXT];
    char dst[PPC_ADDR_TEXT];
    char info[PPC_INFO_TEXT];

    field_text(dir, sizeof(dir), r->dir);
    field_text(word, sizeof(word), r->word);
    field_text(src, sizeof(src), r->src);
    field_text(dst, sizeof(dst), r->dst);
    field_text(info, sizeof(info), r->info);
    return fmt(buf, n, "row\t%u\t%llu\t%llu\t%s\t%u\t%s\t%u\t%u\t%s\t%s\t%s\t%u\n", (unsigned)r->seq,
               (unsigned long long)r->rel_us, (unsigned long long)r->gap_us, dir, (unsigned)r->cls,
               word, (unsigned)r->len, (unsigned)r->signal, src, dst, info, (unsigned)r->flags);
}

int ppc_evt_format_rows_end(char *buf, size_t n, uint32_t req)
{
    return fmt(buf, n, "rows.end\t%u\n", (unsigned)req);
}

int ppc_evt_format_error(char *buf, size_t n, const char *code, const char *text)
{
    char c[16];
    char t[PPC_TEXT_MAX];

    field_text(c, sizeof(c), code);
    field_text(t, sizeof(t), text);
    return fmt(buf, n, "error\t%s\t%s\n", c, t);
}

int ppc_evt_format_note(char *buf, size_t n, const char *key, const char *value)
{
    char k[16];
    char v[64];

    field_text(k, sizeof(k), key);
    field_text(v, sizeof(v), value);
    return fmt(buf, n, "note\t%s\t%s\n", k, v);
}

int ppc_evt_format_bye(char *buf, size_t n)
{
    return fmt(buf, n, "bye\n");
}

int ppc_evt_format_pkt_begin(char *buf, size_t n, const struct ppc_evt_pkt *p)
{
    char dir[sizeof(p->dir)];
    char word[sizeof(p->word)];

    field_text(dir, sizeof(dir), p->dir);
    field_text(word, sizeof(word), p->word);
    return fmt(buf, n, "pkt.begin\t%u\t%u\t%u\t%u\t%llu\t%llu\t%s\t%u\t%s\t%u\t%u\t%u\t%u\n", (unsigned)p->req,
               (unsigned)p->seq, (unsigned)p->rank, (unsigned)p->match, (unsigned long long)p->rel_us,
               (unsigned long long)p->gap_us, dir, (unsigned)p->cls, word, (unsigned)p->wire_len,
               (unsigned)p->stored, (unsigned)p->signal, (unsigned)p->flags);
}

int ppc_evt_format_pkt_ends(char *buf, size_t n, const struct ppc_evt_end *src, const struct ppc_evt_end *dst)
{
    char t[6][PPC_FILTER_TEXT];

    field_text(t[0], sizeof(t[0]), src->addr);
    mark_text(t[1], sizeof(t[1]), src->sub);
    field_text(t[2], sizeof(t[2]), src->filter);
    field_text(t[3], sizeof(t[3]), dst->addr);
    mark_text(t[4], sizeof(t[4]), dst->sub);
    field_text(t[5], sizeof(t[5]), dst->filter);
    return fmt(buf, n, "pkt.ends\t%s\t%s\t%s\t%s\t%s\t%s\n", t[0], t[1], t[2], t[3], t[4], t[5]);
}

int ppc_evt_format_layer(char *buf, size_t n, uint32_t index, int cls, const char *name, const char *text)
{
    char nm[PPC_FIELD_NAME];
    char tx[PPC_FIELD_TEXT];
    char c[8];

    field_text(nm, sizeof(nm), name);
    mark_text(tx, sizeof(tx), text);
    if (cls < 0 || cls >= PPC_CLASS_COUNT) {
        snprintf(c, sizeof(c), "-");
    } else {
        snprintf(c, sizeof(c), "%d", cls);
    }
    return fmt(buf, n, "layer\t%u\t%s\t%s\t%s\n", (unsigned)index, c, nm, tx);
}

int ppc_evt_format_field(char *buf, size_t n, uint32_t layer, const char *name, const char *text, int32_t off,
                         uint32_t len, const char *filter)
{
    char nm[PPC_FIELD_NAME];
    char tx[PPC_FIELD_TEXT];
    char fl[PPC_FILTER_TEXT];
    char o[16];

    field_text(nm, sizeof(nm), name);
    mark_text(tx, sizeof(tx), text);
    field_text(fl, sizeof(fl), filter);
    if (off < 0) {
        snprintf(o, sizeof(o), "-");
        len = 0;
    } else {
        snprintf(o, sizeof(o), "%d", (int)off);
    }
    return fmt(buf, n, "field\t%u\t%s\t%s\t%s\t%u\t%s\n", (unsigned)layer, nm, tx, o, (unsigned)len, fl);
}

int ppc_evt_format_hex(char *buf, size_t n, uint32_t off, const char *hex, const char *ascii)
{
    char hx[PPC_HEX_TEXT];
    char as[PPC_ASCII_TEXT];

    field_text(hx, sizeof(hx), hex);
    field_text(as, sizeof(as), ascii);
    return fmt(buf, n, "hex\t%u\t%s\t%s\n", (unsigned)off, hx, as);
}

int ppc_evt_format_pkt_end(char *buf, size_t n, uint32_t req)
{
    return fmt(buf, n, "pkt.end\t%u\n", (unsigned)req);
}

int ppc_evt_format_pinned(char *buf, size_t n, uint32_t seq, int on, uint32_t pins)
{
    return fmt(buf, n, "pinned\t%u\t%d\t%u\n", (unsigned)seq, on ? 1 : 0, (unsigned)pins);
}

int ppc_evt_format_finding(char *buf, size_t n, uint32_t layer, const char *text)
{
    char t[PPC_FIELD_TEXT];

    mark_text(t, sizeof(t), text);
    return fmt(buf, n, "finding\t%u\t%s\n", (unsigned)layer, t);
}

int ppc_evt_format_detail(char *buf, size_t n, uint32_t layer, const char *text)
{
    char t[PPC_FIELD_TEXT];

    mark_text(t, sizeof(t), text);
    return fmt(buf, n, "detail\t%u\t%s\n", (unsigned)layer, t);
}

int ppc_evt_format_marks(char *buf, size_t n, uint32_t req, uint32_t view_id, uint32_t match, uint32_t buckets,
                         uint64_t mask)
{
    return fmt(buf, n, "marks\t%u\t%u\t%u\t%u\t%llx\n", (unsigned)req, (unsigned)view_id, (unsigned)match,
               (unsigned)buckets, (unsigned long long)mask);
}

static const char *const save_words[] = { "run", "done", "fail" };

int ppc_evt_format_saved(char *buf, size_t n, enum ppc_save_phase phase, uint32_t packets, uint32_t total,
                         uint64_t bytes, const char *name)
{
    char nm[PPC_SAVE_NAME];

    if ((unsigned)phase >= sizeof(save_words) / sizeof(save_words[0])) {
        return -1;
    }
    field_text(nm, sizeof(nm), name);
    return fmt(buf, n, "saved\t%s\t%u\t%u\t%llu\t%s\n", save_words[phase], (unsigned)packets, (unsigned)total,
               (unsigned long long)bytes, nm);
}

const char *ppc_captures_dir(char *buf, size_t n)
{
    const char *env = getenv("POCKETOS_CAPTURES_DIR");
    const char *home = getenv("HOME");
    int len;

    if (env && *env) {
        len = snprintf(buf, n, "%s", env);
    } else {
        len = snprintf(buf, n, "%s/Captures", home && *home ? home : "/root");
    }
    if (len <= 0 || (size_t)len >= n || buf[0] != '/' || strncmp(buf, "/tmp", 4) == 0) {
        return NULL;
    }
    return buf;
}

int ppc_save_name_ok(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    size_t i;

    if (len < 14 || len >= PPC_SAVE_NAME || strncmp(name, "capture-", 8) != 0 ||
        strcmp(name + len - 5, ".pcap") != 0) {
        return 0;
    }
    for (i = 8; i < len - 5; i++) {
        if (!((name[i] >= '0' && name[i] <= '9') || name[i] == '-')) {
            return 0;
        }
    }
    return 1;
}

enum ppc_evt_kind ppc_evt_parse(const char *line, struct ppc_evt *e)
{
    char buf[PPC_LINE_MAX];
    char *f[PPC_FIELDS_MAX];
    int n = split(line, buf, sizeof(buf), f, PPC_FIELDS_MAX, 0);
    int i;

    memset(e, 0, sizeof(*e));
    if (n <= 0 || (n == 1 && f[0][0] == '\0')) {
        e->kind = n < 0 ? PPC_EVT_BAD : PPC_EVT_NONE;
        return e->kind;
    }
    e->kind = PPC_EVT_BAD;
    if (strcmp(f[0], "hello") == 0) {
        if (n != 4 || u32(f[1], &e->u.hello.proto) != 0 ||
            copy(e->u.hello.version, sizeof(e->u.hello.version), f[2]) != 0 ||
            copy(e->u.hello.backend, sizeof(e->u.hello.backend), f[3]) != 0) {
            return e->kind;
        }
        e->kind = PPC_EVT_HELLO;
    } else if (strcmp(f[0], "state") == 0) {
        int s;
        int r;
        uint32_t cs;

        if (n != 6) {
            return e->kind;
        }
        s = parse_state(f[1]);
        r = parse_reason(f[2]);
        if (s < 0 || r < 0 || u32(f[4], &e->u.state.speed) != 0 || u32(f[5], &cs) != 0 || cs > 1) {
            return e->kind;
        }
        if (strcmp(f[3], "-") != 0) {
            if (!ppc_iface_ok(f[3])) {
                return e->kind;
            }
            copy(e->u.state.iface, sizeof(e->u.state.iface), f[3]);
        }
        e->u.state.state = (enum ppc_state)s;
        e->u.state.reason = (enum ppc_reason)r;
        e->u.state.clock_set = (uint8_t)cs;
        e->kind = PPC_EVT_STATE;
    } else if (strcmp(f[0], "stats") == 0) {
        struct ppc_counters *c = &e->u.stats;
        uint64_t *wide[] = { &c->elapsed_ms, &c->seen, &c->stored, &c->held, &c->kdrop, &c->ifdrop,
                             &c->shed,       &c->bytes, &c->pps,   &c->bps };
        uint32_t *narrow[] = { &c->first_seq, &c->last_seq, &c->match, &c->fill_permille };
        int k = 1;

        if (n != 1 + 10 + 4 + 1 + PPC_CLASS_COUNT + PPC_SIGNAL_COUNT) {
            return e->kind;
        }
        for (i = 0; i < 10; i++) {
            if (u64(f[k++], wide[i]) != 0) {
                return e->kind;
            }
        }
        for (i = 0; i < 4; i++) {
            if (u32(f[k++], narrow[i]) != 0) {
                return e->kind;
            }
        }
        if (u64(f[k++], &c->ui_skipped) != 0) {
            return e->kind;
        }
        for (i = 0; i < PPC_CLASS_COUNT; i++) {
            if (u64(f[k++], &c->cls[i]) != 0) {
                return e->kind;
            }
        }
        for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
            if (u32(f[k++], &c->sig[i]) != 0) {
                return e->kind;
            }
        }
        e->kind = PPC_EVT_STATS;
    } else if (strcmp(f[0], "bin") == 0) {
        if (n != 2 + PPC_CLASS_COUNT + 1 || u64(f[1], &e->u.bin.second) != 0) {
            return e->kind;
        }
        for (i = 0; i < PPC_CLASS_COUNT; i++) {
            if (u32(f[2 + i], &e->u.bin.cls[i]) != 0) {
                return e->kind;
            }
        }
        if (u32(f[2 + PPC_CLASS_COUNT], &e->u.bin.held) != 0) {
            return e->kind;
        }
        e->kind = PPC_EVT_BIN;
    } else if (strcmp(f[0], "rows.begin") == 0) {
        if ((n != 7 && n != 8) || u32(f[1], &e->u.rows.req) != 0 || u32(f[2], &e->u.rows.filter_id) != 0 ||
            u32(f[3], &e->u.rows.first_rank) != 0 || u32(f[4], &e->u.rows.match) != 0 ||
            u32(f[5], &e->u.rows.first_seq) != 0 || u32(f[6], &e->u.rows.last_seq) != 0 ||
            (n == 8 && u32(f[7], &e->u.rows.anchor_rank) != 0)) {
            return e->kind;
        }
        if (n == 7) {
            e->u.rows.anchor_rank = e->u.rows.first_rank;
        }
        e->kind = PPC_EVT_ROWS_BEGIN;
    } else if (strcmp(f[0], "rows.end") == 0) {
        if (n != 2 || u32(f[1], &e->u.rows.req) != 0) {
            return e->kind;
        }
        e->kind = PPC_EVT_ROWS_END;
    } else if (strcmp(f[0], "row") == 0) {
        struct ppc_evt_row *r = &e->u.row;
        uint32_t cls;
        uint32_t sig;

        uint32_t flags = 0;

        if ((n != 12 && n != 13) || (n == 13 && (u32(f[12], &flags) != 0 || flags > 0xff)) ||
            u32(f[1], &r->seq) != 0 || u64(f[2], &r->rel_us) != 0 ||
            u64(f[3], &r->gap_us) != 0 || copy(r->dir, sizeof(r->dir), f[4]) != 0 ||
            u32(f[5], &cls) != 0 || cls >= PPC_CLASS_COUNT || copy(r->word, sizeof(r->word), f[6]) != 0 ||
            u32(f[7], &r->len) != 0 || u32(f[8], &sig) != 0 || sig > 0xffffu ||
            copy(r->src, sizeof(r->src), f[9]) != 0 || copy(r->dst, sizeof(r->dst), f[10]) != 0 ||
            copy(r->info, sizeof(r->info), f[11]) != 0) {
            return e->kind;
        }
        r->cls = (uint8_t)cls;
        r->signal = (uint16_t)sig;
        r->flags = (uint8_t)flags;
        e->kind = PPC_EVT_ROW;
    } else if (strcmp(f[0], "error") == 0) {
        if (n != 3 || copy(e->u.error.code, sizeof(e->u.error.code), f[1]) != 0) {
            return e->kind;
        }
        snprintf(e->u.error.text, sizeof(e->u.error.text), "%s", f[2]);
        e->kind = PPC_EVT_ERROR;
    } else if (strcmp(f[0], "note") == 0) {
        if (n != 3 || copy(e->u.note.key, sizeof(e->u.note.key), f[1]) != 0) {
            return e->kind;
        }
        snprintf(e->u.note.value, sizeof(e->u.note.value), "%s", f[2]);
        e->kind = PPC_EVT_NOTE;
    } else if (strcmp(f[0], "bye") == 0) {
        e->kind = n == 1 ? PPC_EVT_BYE : PPC_EVT_BAD;
    } else if (strcmp(f[0], "pkt.begin") == 0) {
        struct ppc_evt_pkt *p = &e->u.pkt;
        uint32_t cls;
        uint32_t sig;
        uint32_t flags;

        if (n != 14 || u32(f[1], &p->req) != 0 || u32(f[2], &p->seq) != 0 || u32(f[3], &p->rank) != 0 ||
            u32(f[4], &p->match) != 0 || u64(f[5], &p->rel_us) != 0 || u64(f[6], &p->gap_us) != 0 ||
            copy(p->dir, sizeof(p->dir), f[7]) != 0 || u32(f[8], &cls) != 0 || cls >= PPC_CLASS_COUNT ||
            copy(p->word, sizeof(p->word), f[9]) != 0 || u32(f[10], &p->wire_len) != 0 ||
            u32(f[11], &p->stored) != 0 || u32(f[12], &sig) != 0 || sig > 0xffffu || u32(f[13], &flags) != 0 ||
            flags > 0xff) {
            return e->kind;
        }
        p->cls = (uint8_t)cls;
        p->signal = (uint16_t)sig;
        p->flags = (uint8_t)flags;
        e->kind = PPC_EVT_PKT_BEGIN;
    } else if (strcmp(f[0], "pkt.ends") == 0) {
        if (n != 7 || copy(e->u.ends.src.addr, sizeof(e->u.ends.src.addr), f[1]) != 0 ||
            copy(e->u.ends.src.sub, sizeof(e->u.ends.src.sub), f[2]) != 0 ||
            copy(e->u.ends.src.filter, sizeof(e->u.ends.src.filter), f[3]) != 0 ||
            copy(e->u.ends.dst.addr, sizeof(e->u.ends.dst.addr), f[4]) != 0 ||
            copy(e->u.ends.dst.sub, sizeof(e->u.ends.dst.sub), f[5]) != 0 ||
            copy(e->u.ends.dst.filter, sizeof(e->u.ends.dst.filter), f[6]) != 0) {
            return e->kind;
        }
        none_dash(e->u.ends.src.sub);
        none_dash(e->u.ends.src.filter);
        none_dash(e->u.ends.dst.sub);
        none_dash(e->u.ends.dst.filter);
        e->kind = PPC_EVT_PKT_ENDS;
    } else if (strcmp(f[0], "layer") == 0) {
        uint32_t cls = 0;

        if (n != 5 || u32(f[1], &e->u.layer.index) != 0 ||
            (strcmp(f[2], "-") != 0 && (u32(f[2], &cls) != 0 || cls >= PPC_CLASS_COUNT)) ||
            copy(e->u.layer.name, sizeof(e->u.layer.name), f[3]) != 0 ||
            copy(e->u.layer.text, sizeof(e->u.layer.text), f[4]) != 0) {
            return e->kind;
        }
        e->u.layer.cls = strcmp(f[2], "-") == 0 ? -1 : (int)cls;
        none_dash(e->u.layer.text);
        e->kind = PPC_EVT_LAYER;
    } else if (strcmp(f[0], "field") == 0) {
        uint32_t off = 0;

        if (n != 7 || u32(f[1], &e->u.field.layer) != 0 ||
            copy(e->u.field.name, sizeof(e->u.field.name), f[2]) != 0 ||
            copy(e->u.field.text, sizeof(e->u.field.text), f[3]) != 0 ||
            (strcmp(f[4], "-") != 0 && (u32(f[4], &off) != 0 || off > 65535u)) || u32(f[5], &e->u.field.len) != 0 ||
            e->u.field.len > 65535u || copy(e->u.field.filter, sizeof(e->u.field.filter), f[6]) != 0) {
            return e->kind;
        }
        e->u.field.off = strcmp(f[4], "-") == 0 ? -1 : (int32_t)off;
        if (e->u.field.off < 0) {
            e->u.field.len = 0;
        }
        none_dash(e->u.field.filter);
        e->kind = PPC_EVT_FIELD;
    } else if (strcmp(f[0], "hex") == 0) {
        if (n != 4 || u32(f[1], &e->u.hex.off) != 0 || copy(e->u.hex.hex, sizeof(e->u.hex.hex), f[2]) != 0 ||
            copy(e->u.hex.ascii, sizeof(e->u.hex.ascii), f[3]) != 0) {
            return e->kind;
        }
        e->kind = PPC_EVT_HEX;
    } else if (strcmp(f[0], "pkt.end") == 0) {
        if (n != 2 || u32(f[1], &e->u.pkt.req) != 0) {
            return e->kind;
        }
        e->kind = PPC_EVT_PKT_END;
    } else if (strcmp(f[0], "pinned") == 0) {
        uint32_t on;

        if (n != 4 || u32(f[1], &e->u.pinned.seq) != 0 || u32(f[2], &on) != 0 || on > 1 ||
            u32(f[3], &e->u.pinned.pins) != 0) {
            return e->kind;
        }
        e->u.pinned.on = (uint8_t)on;
        e->kind = PPC_EVT_PINNED;
    } else if (strcmp(f[0], "finding") == 0 || strcmp(f[0], "detail") == 0) {
        if (n != 3 || u32(f[1], &e->u.finding.layer) != 0 ||
            copy(e->u.finding.text, sizeof(e->u.finding.text), f[2]) != 0) {
            return e->kind;
        }
        e->kind = f[0][0] == 'f' ? PPC_EVT_FINDING : PPC_EVT_DETAIL;
    } else if (strcmp(f[0], "diag") == 0) {
        struct ppc_diag *d = &e->u.diag;
        uint64_t v64;
        int k = 1;

        if (n != 1 + PPC_DIAG_FIELDS) {
            return e->kind;
        }
        /* Each figure parsed as 64 bits, then held to its field's width:
         * a 32-bit field given more is refused. */
#define DIAG_IN(name)                                                                                                  \
    if (u64(f[k++], &v64) != 0 || (sizeof(d->name) == 4 && v64 > 0xffffffffull)) {                                   \
        return e->kind;                                                                                                \
    }                                                                                                                  \
    d->name = (__typeof__(d->name))v64;
        DIAG_LIST(DIAG_IN)
#undef DIAG_IN
        e->kind = PPC_EVT_DIAG;
    } else if (strcmp(f[0], "net") == 0) {
        struct ppc_net_totals *t = &e->u.net;
        int k = 1;

        if (n != 1 + PPC_NET_FIELDS) {
            return e->kind;
        }
#define NET_IN(name)                                                                                                   \
    if (u32(f[k++], &t->name) != 0) {                                                                                  \
        return e->kind;                                                                                                \
    }
        NET_LIST(NET_IN)
#undef NET_IN
        e->kind = PPC_EVT_NET;
    } else if (strcmp(f[0], "net.begin") == 0 || strcmp(f[0], "net.end") == 0) {
        if (n != 2 || u32(f[1], &e->u.net_req) != 0) {
            return e->kind;
        }
        e->kind = f[0][4] == 'b' ? PPC_EVT_NET_BEGIN : PPC_EVT_NET_END;
    } else if (strcmp(f[0], "net.host") == 0) {
        struct ppc_evt_net_host *h = &e->u.net_host;
        uint32_t flags;

        if (n != 7) {
            return e->kind;
        }
        none_dash(f[2]);
        none_dash(f[6]);
        if (!f[1][0] || !addr_ok(f[1], PPC_MAC_TEXT, 1) || !addr_ok(f[2], PPC_IP_TEXT, 0) || u32(f[3], &flags) != 0 ||
            flags > PPC_NH_ALL || u32(f[4], &h->pkts) != 0 || u64(f[5], &h->bytes) != 0 ||
            copy(h->name, sizeof(h->name), f[6]) != 0) {
            return e->kind;
        }
        copy(h->mac, sizeof(h->mac), f[1]);
        copy(h->ip, sizeof(h->ip), f[2]);
        h->flags = (uint8_t)flags;
        ppc_sanitize(h->name);
        e->kind = PPC_EVT_NET_HOST;
    } else if (strcmp(f[0], "net.srv") == 0) {
        struct ppc_evt_net_srv *s = &e->u.net_srv;
        uint32_t relayed;

        if (n != 12) {
            return e->kind;
        }
        none_dash(f[2]);
        none_dash(f[9]);
        none_dash(f[10]);
        none_dash(f[11]);
        if (!f[1][0] || !addr_ok(f[1], PPC_IP_TEXT, 0) || !addr_ok(f[2], PPC_MAC_TEXT, 1) || u32(f[3], &relayed) != 0 ||
            relayed > 1 || u32(f[4], &s->offers) != 0 || u32(f[5], &s->acks) != 0 || u32(f[6], &s->naks) != 0 ||
            u32(f[7], &s->seq) != 0 || u32(f[8], &s->lease_s) != 0 || !addr_ok(f[9], PPC_IP_TEXT, 0) ||
            !addr_ok(f[10], PPC_IP_TEXT, 0) || !addr_ok(f[11], PPC_IP_TEXT, 0)) {
            return e->kind;
        }
        copy(s->ip, sizeof(s->ip), f[1]);
        copy(s->mac, sizeof(s->mac), f[2]);
        s->relayed = (uint8_t)relayed;
        copy(s->router, sizeof(s->router), f[9]);
        copy(s->dns, sizeof(s->dns), f[10]);
        copy(s->mask, sizeof(s->mask), f[11]);
        e->kind = PPC_EVT_NET_SRV;
    } else if (strcmp(f[0], "net.lease") == 0) {
        struct ppc_evt_net_lease *l = &e->u.net_lease;

        if (n != 8) {
            return e->kind;
        }
        none_dash(f[2]);
        none_dash(f[3]);
        none_dash(f[7]);
        if (!f[1][0] || !addr_ok(f[1], PPC_IP_TEXT, 0) || !addr_ok(f[2], PPC_MAC_TEXT, 1) ||
            !addr_ok(f[3], PPC_IP_TEXT, 0) || u32(f[4], &l->lease_s) != 0 || u64(f[5], &l->rel_us) != 0 ||
            u32(f[6], &l->seq) != 0 || copy(l->name, sizeof(l->name), f[7]) != 0) {
            return e->kind;
        }
        copy(l->ip, sizeof(l->ip), f[1]);
        copy(l->mac, sizeof(l->mac), f[2]);
        copy(l->server, sizeof(l->server), f[3]);
        ppc_sanitize(l->name);
        e->kind = PPC_EVT_NET_LEASE;
    } else if (strcmp(f[0], "net.evt") == 0) {
        struct ppc_evt_net_event *v = &e->u.net_evt;
        uint32_t kind;
        uint32_t sev;

        if (n != 11) {
            return e->kind;
        }
        for (i = 7; i <= 10; i++) {
            none_dash(f[i]);
        }
        if (u32(f[1], &kind) != 0 || kind == PPC_NK_NONE || kind >= PPC_NK_COUNT || u32(f[2], &sev) != 0 ||
            sev > PPC_NS_WARN || u32(f[3], &v->count) != 0 || u32(f[4], &v->seq) != 0 || u64(f[5], &v->first_us) != 0 ||
            u64(f[6], &v->last_us) != 0 || !addr_ok(f[7], PPC_IP_TEXT, 0) || !addr_ok(f[8], PPC_MAC_TEXT, 1) ||
            !addr_ok(f[9], PPC_IP_TEXT, 0) || !addr_ok(f[10], PPC_MAC_TEXT, 1)) {
            return e->kind;
        }
        v->kind = (uint8_t)kind;
        v->sev = (uint8_t)sev;
        copy(v->ip, sizeof(v->ip), f[7]);
        copy(v->mac, sizeof(v->mac), f[8]);
        copy(v->ip2, sizeof(v->ip2), f[9]);
        copy(v->mac2, sizeof(v->mac2), f[10]);
        e->kind = PPC_EVT_NET_EVT;
    } else if (strcmp(f[0], "mark") == 0) {
        struct ppc_evt_mark *m = &e->u.mark;
        int st;

        if (n != 7) {
            return e->kind;
        }
        st = parse_state(f[4]);
        if (u32(f[1], &m->id) != 0 || !m->id || u32(f[2], &m->first) != 0 || !m->first || m->first > m->id ||
            u32(f[3], &m->at_ms) != 0 || st < 0 || u32(f[5], &m->seq) != 0 || u64(f[6], &m->wall_s) != 0) {
            return e->kind;
        }
        m->state = (uint8_t)st;
        e->kind = PPC_EVT_MARK;
    } else if (strcmp(f[0], "ev.begin") == 0) {
        struct ppc_evt_ev_head *h = &e->u.ev_head;
        unsigned long mask;
        char *end = NULL;
        int st;
        int cf;

        if (n != 15) {
            return e->kind;
        }
        st = parse_state(f[6]);
        cf = ppc_cfilter_parse(f[10]);
        mask = strtoul(f[11], &end, 16);
        if (u32(f[1], &h->req) != 0 || u32(f[2], &h->id) != 0 || !h->id || u32(f[3], &h->first) != 0 || !h->first ||
            u32(f[4], &h->last) != 0 || h->first > h->id || h->id > h->last || u32(f[5], &h->at_ms) != 0 || st < 0 ||
            u32(f[7], &h->seq) != 0 || u64(f[8], &h->wall_s) != 0 || u32(f[9], &h->now_ms) != 0 || cf < 0 ||
            !f[11][0] || f[11][0] == '-' || f[11][0] == '+' || !end || *end || (mask & ~(unsigned long)PPC_EV_CLASSES)) {
            return e->kind;
        }
        /* The counters at the mark: read ("0 - -"), not read and told later
         * ("<lost> <from> <to>", to after from), or not known ("- <from> -"). */
        if (strcmp(f[12], "0") == 0 && strcmp(f[13], "-") == 0 && strcmp(f[14], "-") == 0) {
            h->edge = PPC_EV_EDGE_NONE;
        } else if (strcmp(f[12], "-") == 0 && strcmp(f[14], "-") == 0 && u32(f[13], &h->edge_from_ms) == 0) {
            h->edge = PPC_EV_EDGE_UNKNOWN;
        } else if (u64(f[12], &h->edge_lost) == 0 && u32(f[13], &h->edge_from_ms) == 0 &&
                   u32(f[14], &h->edge_to_ms) == 0 && h->edge_to_ms > h->edge_from_ms) {
            h->edge = PPC_EV_EDGE_COUNTED;
        } else {
            return e->kind;
        }
        h->state = (uint8_t)st;
        h->cfilter = (uint8_t)cf;
        h->unanalyzed = (uint8_t)mask;
        e->kind = PPC_EVT_EV_BEGIN;
    } else if (strcmp(f[0], "ev.win") == 0) {
        struct ppc_evt_ev_win *w = &e->u.ev_win;
        uint32_t side;
        uint32_t flags;

        if (n != 10 + (PPC_EV_KINDS - 1) + 2) {
            return e->kind;
        }
        if (u32(f[1], &side) != 0 || side > 1 || u32(f[2], &flags) != 0 || (flags & ~PPC_EVW_ALL) ||
            u32(f[3], &w->span_ms) != 0 || w->span_ms > PPC_EV_WINDOW_MS || u32(f[4], &w->watched_s) != 0 ||
            u32(f[5], &w->held_s) != 0 || u32(f[6], &w->shed_s) != 0 || u64(f[7], &w->lost) != 0 ||
            u32(f[8], &w->kept) != 0 || w->kept > PPC_EV_KEEP || u32(f[9], &w->more) != 0) {
            return e->kind;
        }
        for (i = 1; i < PPC_EV_KINDS; i++) {
            if (u32(f[9 + i], &w->count[i]) != 0) {
                return e->kind;
            }
        }
        if (u64(f[9 + PPC_EV_KINDS], &w->lost_near) != 0) {
            return e->kind;
        }
        if (strcmp(f[10 + PPC_EV_KINDS], "-") != 0) {
            if (u32(f[10 + PPC_EV_KINDS], &w->unknown_from_ms) != 0) {
                return e->kind;
            }
            w->unknown = 1;
        }
        w->side = (uint8_t)side;
        w->flags = (uint8_t)flags;
        e->kind = PPC_EVT_EV_WIN;
    } else if (strcmp(f[0], "ev.obs") == 0) {
        struct ppc_evt_ev_obs *o = &e->u.ev_obs;
        uint32_t side;
        uint32_t kind;
        uint32_t flags;
        uint32_t alive;

        if (n != 12) {
            return e->kind;
        }
        for (i = 7; i <= 9; i++) {
            none_dash(f[i]);
        }
        if (u32(f[1], &side) != 0 || side > 1 || u32(f[2], &kind) != 0 || kind == PPC_EV_NONE || kind >= PPC_EV_KINDS ||
            u32(f[3], &flags) != 0 || (flags & ~PPC_EVF_ALL) || u32(f[4], &o->t_ms) != 0 || u32(f[5], &o->seq) != 0 ||
            !o->seq || u32(f[6], &alive) != 0 || alive > PPC_EV_WHOLE || !addr_ok(f[7], PPC_MAC_TEXT, 1) ||
            !addr_ok(f[8], PPC_IP_TEXT, 0) || !addr_ok(f[9], PPC_IP_TEXT, 0) || u32(f[10], &o->a) != 0 ||
            u32(f[11], &o->b) != 0) {
            return e->kind;
        }
        o->side = (uint8_t)side;
        o->kind = (uint8_t)kind;
        o->flags = (uint8_t)flags;
        o->alive = (uint8_t)alive;
        copy(o->mac, sizeof(o->mac), f[7]);
        copy(o->ip, sizeof(o->ip), f[8]);
        copy(o->ip2, sizeof(o->ip2), f[9]);
        e->kind = PPC_EVT_EV_OBS;
    } else if (strcmp(f[0], "ev.end") == 0) {
        if (n != 2 || u32(f[1], &e->u.ev_req) != 0) {
            return e->kind;
        }
        e->kind = PPC_EVT_EV_END;
    } else if (strcmp(f[0], "in.begin") == 0) {
        struct ppc_evt_in_head *ih = &e->u.in_head;
        uint32_t pair = 0, fin = 0;

        if (n != 7 || u32(f[1], &ih->req) != 0 || u32(f[2], &ih->mark) != 0 || !ih->mark || u32(f[3], &pair) != 0 ||
            pair > 1u || u32(f[4], &ih->at_ms) != 0 || u32(f[5], &ih->now_ms) != 0 || u32(f[6], &fin) != 0 || fin > 1u) {
            return e->kind;
        }
        ih->pair = (uint8_t)pair;
        ih->final = (uint8_t)fin;
        e->kind = PPC_EVT_IN_BEGIN;
    } else if (strcmp(f[0], "in.vis") == 0) {
        struct ppc_evt_in_vis *v = &e->u.in_vis;
        uint32_t side = 0, known = 0;

        if (n != 14 || u32(f[1], &side) != 0 || side > 1u || u32(f[2], &v->from_ms) != 0 || u32(f[3], &v->to_ms) != 0 ||
            v->from_ms > v->to_ms || u32(f[4], &v->watched_s) != 0 || u32(f[5], &v->held_s) != 0 ||
            u32(f[6], &v->shed_s) != 0 || u32(f[7], &v->gap_s) != 0 || u32(f[8], &v->frames) != 0 ||
            u32(f[9], &v->lost_lo) != 0 || u32(f[10], &v->lost_hi) != 0 || u32(f[11], &known) != 0 || known > 1u ||
            u32(f[12], &v->own_in) != 0 || u32(f[13], &v->own_out) != 0) {
            return e->kind;
        }
        v->side = (uint8_t)side;
        v->loss_known = (uint8_t)known;
        e->kind = PPC_EVT_IN_VIS;
    } else if (strcmp(f[0], "in.cmp") == 0) {
        struct ppc_evt_in_cmp *m = &e->u.in_cmp;
        uint32_t v[5];
        unsigned i;

        if (n != 19) {
            return e->kind;
        }
        for (i = 0; i < 5; i++) {
            if (u32(f[1 + i], &v[i]) != 0) {
                return e->kind;
            }
        }
        if (v[0] >= PPC_IN_METRICS || v[1] > 5u || v[2] > 2u || v[3] > 11u || v[4] > 0x3fu) {
            return e->kind;
        }
        m->metric = (uint8_t)v[0];
        m->verdict = (uint8_t)v[1];
        m->quality = (uint8_t)v[2];
        m->reason = (uint8_t)v[3];
        m->notes = (uint8_t)v[4];
        if (u32(f[6], &m->n[0]) != 0 || u32(f[7], &m->n[1]) != 0 || u32(f[8], &m->k[0]) != 0 ||
            u32(f[9], &m->k[1]) != 0 || u32(f[10], &m->stat[0]) != 0 || u32(f[11], &m->stat[1]) != 0 ||
            u32(f[12], &m->und[0]) != 0 || u32(f[13], &m->und[1]) != 0 || u32(f[14], &m->p_e6) != 0 ||
            m->p_e6 > 1000000u) {
            return e->kind;
        }
        for (i = 0; i < PPC_IN_REF_MAX; i++) {
            const char *s = f[15 + i];
            char *colon;
            char tmp[24];
            uint32_t a = 0;

            m->ref[i] = 0;
            m->alive[i] = 0;
            if (strcmp(s, "-") == 0) {
                continue;
            }
            if (strlen(s) >= sizeof(tmp)) {
                return e->kind;
            }
            memcpy(tmp, s, strlen(s) + 1);
            colon = strchr(tmp, ':');
            if (!colon) {
                return e->kind;
            }
            *colon = '\0';
            if (u32(tmp, &m->ref[i]) != 0 || !m->ref[i] || u32(colon + 1, &a) != 0 || a > PPC_EV_WHOLE) {
                return e->kind;
            }
            m->alive[i] = (uint8_t)a;
        }
        e->kind = PPC_EVT_IN_CMP;
    } else if (strcmp(f[0], "in.end") == 0) {
        if (n != 2 || u32(f[1], &e->u.ev_req) != 0) {
            return e->kind;
        }
        e->kind = PPC_EVT_IN_END;
    } else if (strcmp(f[0], "marks") == 0) {
        char *end = NULL;

        if (n != 6 || u32(f[1], &e->u.marks.req) != 0 || u32(f[2], &e->u.marks.view_id) != 0 ||
            u32(f[3], &e->u.marks.match) != 0 || u32(f[4], &e->u.marks.buckets) != 0 ||
            e->u.marks.buckets > PPC_MARK_BUCKETS || f[5][0] == '\0' || strlen(f[5]) > 16) {
            return e->kind;
        }
        e->u.marks.mask = strtoull(f[5], &end, 16);
        if (*end) {
            return e->kind;
        }
        e->kind = PPC_EVT_MARKS;
    } else if (strcmp(f[0], "saved") == 0) {
        unsigned ph;

        if (n != 6) {
            return e->kind;
        }
        for (ph = 0; ph < sizeof(save_words) / sizeof(save_words[0]); ph++) {
            if (strcmp(f[1], save_words[ph]) == 0) {
                break;
            }
        }
        if (ph >= sizeof(save_words) / sizeof(save_words[0]) || u32(f[2], &e->u.saved.packets) != 0 ||
            u32(f[3], &e->u.saved.total) != 0 || u64(f[4], &e->u.saved.bytes) != 0 ||
            copy(e->u.saved.name, sizeof(e->u.saved.name), f[5]) != 0) {
            return e->kind;
        }
        e->u.saved.phase = (uint8_t)ph;
        e->kind = PPC_EVT_SAVED;
    } else {
        e->kind = PPC_EVT_NONE;
    }
    return e->kind;
}
