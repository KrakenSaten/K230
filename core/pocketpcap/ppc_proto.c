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
        } else if (strncmp(f[2], "seq:", 4) == 0 && u32(f[2] + 4, &c->seq) == 0 && c->seq > 0) {
            c->tail = 0;
        } else {
            return c->kind;
        }
        c->kind = PPC_CMD_WINDOW;
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

/* ---- events ------------------------------------------------------------------------- */

/* Free text for a field: no tabs, newlines or anything else unprintable. */
static void field_text(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src && *src ? src : "-");
    ppc_sanitize(dst);
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
    return fmt(buf, n, "row\t%u\t%llu\t%llu\t%s\t%u\t%s\t%u\t%u\t%s\t%s\t%s\n", (unsigned)r->seq,
               (unsigned long long)r->rel_us, (unsigned long long)r->gap_us, dir, (unsigned)r->cls,
               word, (unsigned)r->len, (unsigned)r->signal, src, dst, info);
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
        if (n != 7 || u32(f[1], &e->u.rows.req) != 0 || u32(f[2], &e->u.rows.filter_id) != 0 ||
            u32(f[3], &e->u.rows.first_rank) != 0 || u32(f[4], &e->u.rows.match) != 0 ||
            u32(f[5], &e->u.rows.first_seq) != 0 || u32(f[6], &e->u.rows.last_seq) != 0) {
            return e->kind;
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

        if (n != 12 || u32(f[1], &r->seq) != 0 || u64(f[2], &r->rel_us) != 0 ||
            u64(f[3], &r->gap_us) != 0 || copy(r->dir, sizeof(r->dir), f[4]) != 0 ||
            u32(f[5], &cls) != 0 || cls >= PPC_CLASS_COUNT || copy(r->word, sizeof(r->word), f[6]) != 0 ||
            u32(f[7], &r->len) != 0 || u32(f[8], &sig) != 0 || sig > 0xffffu ||
            copy(r->src, sizeof(r->src), f[9]) != 0 || copy(r->dst, sizeof(r->dst), f[10]) != 0 ||
            copy(r->info, sizeof(r->info), f[11]) != 0) {
            return e->kind;
        }
        r->cls = (uint8_t)cls;
        r->signal = (uint16_t)sig;
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
    } else {
        e->kind = PPC_EVT_NONE;
    }
    return e->kind;
}
