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

        if ((n != 7 && n != 8) || u32(f[1], &c->view_id) != 0) {
            return c->kind;
        }
        if (n == 8) {
            unsigned long sig = strtoul(f[7], &end, 16);

            if (*end || f[7][0] == '\0' || strlen(f[7]) > 2 || sig > 0x1f) {
                return c->kind;
            }
            c->signals = (uint8_t)sig;
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

int ppc_cmd_format_view_signals(char *buf, size_t n, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                                uint16_t port, int pinned, unsigned signals)
{
    if (sort >= sizeof(sort_words) / sizeof(sort_words[0]) || (host && *host && !host_ok(host)) || signals > 0x1f) {
        return -1;
    }
    if (signals == 0) {
        /* The protocol-2 line: a helper that knows no findings reads it. */
        return fmt(buf, n, "view\t%u\t%s\t%x\t%s\t%u\t%d", (unsigned)id, sort_words[sort], (unsigned)classes,
                   host && *host ? host : "-", (unsigned)port, pinned ? 1 : 0);
    }
    return fmt(buf, n, "view\t%u\t%s\t%x\t%s\t%u\t%d\t%x", (unsigned)id, sort_words[sort], (unsigned)classes,
               host && *host ? host : "-", (unsigned)port, pinned ? 1 : 0, signals);
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
               "\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%u\t%u\t%u\t%u\t%u\n",
               (unsigned long long)c->elapsed_ms, (unsigned long long)c->seen,
               (unsigned long long)c->stored, (unsigned long long)c->held, (unsigned long long)c->kdrop,
               (unsigned long long)c->ifdrop, (unsigned long long)c->shed, (unsigned long long)c->bytes,
               (unsigned long long)c->pps, (unsigned long long)c->bps, (unsigned)c->first_seq,
               (unsigned)c->last_seq, (unsigned)c->match, (unsigned)c->fill_permille,
               (unsigned long long)c->ui_skipped, (unsigned long long)c->cls[0],
               (unsigned long long)c->cls[1], (unsigned long long)c->cls[2], (unsigned long long)c->cls[3],
               (unsigned long long)c->cls[4], (unsigned long long)c->cls[5], (unsigned long long)c->cls[6],
               (unsigned long long)c->cls[7], (unsigned)c->sig[0], (unsigned)c->sig[1], (unsigned)c->sig[2],
               (unsigned)c->sig[3], (unsigned)c->sig[4]);
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
    } else if (strcmp(f[0], "finding") == 0) {
        if (n != 3 || u32(f[1], &e->u.finding.layer) != 0 ||
            copy(e->u.finding.text, sizeof(e->u.finding.text), f[2]) != 0) {
            return e->kind;
        }
        e->kind = PPC_EVT_FINDING;
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
