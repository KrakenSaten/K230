/*
 * PacketShark's model in the shell. See psk_model.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void psk_model_init(struct psk_model *m)
{
    memset(m, 0, sizeof(*m));
    m->state = PPC_STATE_IDLE;
}

void psk_model_helper_started(struct psk_model *m, int64_t now_ms)
{
    (void)now_ms;
    m->running = true;
    m->hello = false;
    m->exit = PSK_EXIT_NONE;
    m->exit_value = 0;
    m->had_capture = false;
    m->shed = false;
    m->caps[0] = '\0';
    m->err_code[0] = '\0';
    m->err_text[0] = '\0';
    psk_model_clear_packets(m);
}

void psk_model_clear_packets(struct psk_model *m)
{
    m->rows_open = false;
    m->row_count = 0;
    m->rows_match = 0;
    m->rows_first_rank = 0;
    m->rows_anchor = 0;
    m->pkt_open = false;
    m->have_pkt = false;
    m->pins = 0;
    m->pin_seq = 0;
}

void psk_model_helper_stopped(struct psk_model *m, enum psk_exit why, int value, int64_t now_ms)
{
    (void)now_ms;
    m->running = false;
    m->hello = false;
    /* A helper that left on its own while it held a capture went wrong,
     * whatever its status: the packets went with it. */
    if (why == PSK_EXIT_NORMAL && m->had_capture &&
        (m->state == PPC_STATE_CAPTURING || m->state == PPC_STATE_HOLD || m->state == PPC_STATE_SAVING)) {
        why = PSK_EXIT_UNEXPECTED;
    }
    m->exit = why;
    m->exit_value = value;
    /* The packets went with the helper (D10): no row or packet stays. */
    psk_model_clear_packets(m);
}

bool psk_model_has_packets(const struct psk_model *m)
{
    return m->running && m->had_capture && m->state != PPC_STATE_IDLE;
}

static void bin_store(struct psk_model *m, uint64_t second, const uint32_t cls[PPC_CLASS_COUNT], uint32_t held)
{
    unsigned i = (unsigned)(second % PSK_BINS);
    uint64_t s;

    /* Seconds the helper skipped (a dropped snapshot) are cleared, so an
     * old second never shows in a new one's place. */
    if (second >= m->bin_next && second - m->bin_next < PSK_BINS) {
        for (s = m->bin_next; s < second; s++) {
            memset(m->bins[s % PSK_BINS], 0, sizeof(m->bins[0]));
            m->bin_held[s % PSK_BINS] = 0;
        }
    } else if (second >= m->bin_next) {
        memset(m->bins, 0, sizeof(m->bins));
        memset(m->bin_held, 0, sizeof(m->bin_held));
    }
    memcpy(m->bins[i], cls, sizeof(m->bins[i]));
    m->bin_held[i] = held;
    if (second + 1u > m->bin_next) {
        m->bin_next = second + 1u;
    }
}

unsigned psk_model_apply(struct psk_model *m, const struct ppc_evt *e, int64_t now_ms)
{
    switch (e->kind) {
    case PPC_EVT_HELLO:
        m->hello = true;
        snprintf(m->version, sizeof(m->version), "%s", e->u.hello.version);
        snprintf(m->backend, sizeof(m->backend), "%s", e->u.hello.backend);
        return PSK_CHANGED_HELLO;
    case PPC_EVT_STATE:
        if (e->u.state.state == PPC_STATE_CAPTURING && m->state == PPC_STATE_IDLE) {
            /* A new capture: the last one's figures and graph go. */
            memset(&m->c, 0, sizeof(m->c));
            m->have_stats = false;
            memset(m->bins, 0, sizeof(m->bins));
            memset(m->bin_held, 0, sizeof(m->bin_held));
            m->bin_next = 0;
        }
        if (e->u.state.state != PPC_STATE_IDLE) {
            m->had_capture = true;
        }
        m->state = e->u.state.state;
        m->reason = e->u.state.reason;
        snprintf(m->iface, sizeof(m->iface), "%s", e->u.state.iface);
        m->speed = e->u.state.speed;
        m->clock_set = e->u.state.clock_set != 0;
        return PSK_CHANGED_STATE;
    case PPC_EVT_STATS:
        /* Counters only grow within a session; a stats line from before the
         * session's own state (there is none) cannot arrive, so this is a
         * plain replace. */
        m->c = e->u.stats;
        m->have_stats = true;
        m->stats_ms = now_ms;
        return PSK_CHANGED_STATS;
    case PPC_EVT_BIN:
        bin_store(m, e->u.bin.second, e->u.bin.cls, e->u.bin.held);
        return PSK_CHANGED_BIN;
    case PPC_EVT_ERROR:
        snprintf(m->err_code, sizeof(m->err_code), "%s", e->u.error.code);
        snprintf(m->err_text, sizeof(m->err_text), "%s", e->u.error.text);
        m->err_ms = now_ms;
        return PSK_CHANGED_ERROR;
    case PPC_EVT_NOTE:
        if (strcmp(e->u.note.key, "caps") == 0) {
            snprintf(m->caps, sizeof(m->caps), "%s", e->u.note.value);
        } else if (strcmp(e->u.note.key, "shed") == 0) {
            m->shed = strcmp(e->u.note.value, "1") == 0;
        }
        return PSK_CHANGED_NOTE;
    case PPC_EVT_ROWS_BEGIN:
        if (e->u.rows.req != m->want_req || m->want_req == 0) {
            m->rows_open = false; /* stale: an answer to an older request */
            return 0;
        }
        m->rows_open = true;
        m->row_count = 0;
        m->rows_first_seq = e->u.rows.first_seq;
        m->rows_last_seq = e->u.rows.last_seq;
        m->rows_filter_id = e->u.rows.filter_id;
        m->rows_first_rank = e->u.rows.first_rank;
        m->rows_match = e->u.rows.match;
        m->rows_anchor = e->u.rows.anchor_rank;
        return 0;
    case PPC_EVT_ROW:
        if (m->rows_open && m->row_count < PSK_ROW_CACHE) {
            m->rows[m->row_count++] = e->u.row;
        }
        return 0;
    case PPC_EVT_ROWS_END:
        if (m->rows_open && e->u.rows.req == m->want_req) {
            m->rows_open = false;
            return PSK_CHANGED_ROWS;
        }
        m->rows_open = false;
        return 0;
    case PPC_EVT_PKT_BEGIN:
        if (e->u.pkt.req != m->want_pkt || m->want_pkt == 0) {
            m->pkt_open = false;
            return 0;
        }
        memset(&m->pkt_next, 0, sizeof(m->pkt_next));
        m->pkt_next.head = e->u.pkt;
        m->pkt_open = true;
        return 0;
    case PPC_EVT_PKT_ENDS:
        if (m->pkt_open) {
            m->pkt_next.src = e->u.ends.src;
            m->pkt_next.dst = e->u.ends.dst;
        }
        return 0;
    case PPC_EVT_LAYER:
        if (m->pkt_open && m->pkt_next.nlayers < PSK_LAYERS && e->u.layer.index == m->pkt_next.nlayers) {
            struct psk_layer *l = &m->pkt_next.layer[m->pkt_next.nlayers++];

            l->cls = e->u.layer.cls;
            snprintf(l->name, sizeof(l->name), "%s", e->u.layer.name);
            snprintf(l->text, sizeof(l->text), "%s", e->u.layer.text);
        }
        return 0;
    case PPC_EVT_FIELD:
        if (m->pkt_open && m->pkt_next.nfields < PSK_FIELDS && e->u.field.layer < m->pkt_next.nlayers) {
            struct psk_field *f = &m->pkt_next.field[m->pkt_next.nfields++];

            f->layer = (uint8_t)e->u.field.layer;
            snprintf(f->name, sizeof(f->name), "%s", e->u.field.name);
            snprintf(f->text, sizeof(f->text), "%s", e->u.field.text);
            f->off = e->u.field.off;
            f->len = e->u.field.len;
            snprintf(f->filter, sizeof(f->filter), "%s", e->u.field.filter);
        }
        return 0;
    case PPC_EVT_HEX:
        if (m->pkt_open && m->pkt_next.nhex < PSK_HEX_LINES) {
            struct psk_hex *h = &m->pkt_next.hex[m->pkt_next.nhex++];

            h->off = e->u.hex.off;
            snprintf(h->hex, sizeof(h->hex), "%s", e->u.hex.hex);
            snprintf(h->ascii, sizeof(h->ascii), "%s", e->u.hex.ascii);
        }
        return 0;
    case PPC_EVT_PKT_END:
        if (m->pkt_open && e->u.pkt.req == m->want_pkt) {
            m->pkt_open = false;
            m->pkt = m->pkt_next;
            m->have_pkt = true;
            return PSK_CHANGED_PACKET;
        }
        m->pkt_open = false;
        return 0;
    case PPC_EVT_PINNED:
        m->pins = e->u.pinned.pins;
        m->pin_seq = e->u.pinned.seq;
        m->pin_on = e->u.pinned.on != 0;
        if (m->have_pkt && m->pkt.head.seq == e->u.pinned.seq) {
            if (m->pin_on) {
                m->pkt.head.flags |= PPC_ROWF_PINNED;
            } else {
                m->pkt.head.flags &= (uint8_t)~PPC_ROWF_PINNED;
            }
        }
        return PSK_CHANGED_PINS;
    case PPC_EVT_BYE:
    case PPC_EVT_NONE:
    case PPC_EVT_BAD:
    default:
        return 0;
    }
}

const char *psk_exit_text(enum psk_exit e, int value, char *buf, size_t buflen)
{
    switch (e) {
    case PSK_EXIT_CRASHED:
        /* SIGKILL is never the helper's own fault: something killed it. */
        snprintf(buf, buflen, "%s (SIG %d)", value == 128 + 9 ? "KILLED" : "CRASHED",
                 value > 128 ? value - 128 : value);
        break;
    case PSK_EXIT_HUNG:
        snprintf(buf, buflen, "NOT RESPONDING");
        break;
    case PSK_EXIT_PROTOCOL:
        snprintf(buf, buflen, "PROTOCOL ERROR");
        break;
    case PSK_EXIT_START:
        snprintf(buf, buflen, "HELPER MISSING");
        break;
    case PSK_EXIT_UNEXPECTED:
        snprintf(buf, buflen, "EXITED (%d)", value);
        break;
    case PSK_EXIT_NORMAL:
        snprintf(buf, buflen, "ENDED");
        break;
    case PSK_EXIT_NONE:
    default:
        snprintf(buf, buflen, "RUNNING");
        break;
    }
    return buf;
}

/* ---- figures ------------------------------------------------------------------------ */

const char *psk_fmt_count(uint64_t n, char *buf, size_t buflen)
{
    char digits[24];
    size_t len;
    size_t i;
    size_t o = 0;

    snprintf(digits, sizeof(digits), "%llu", (unsigned long long)n);
    len = strlen(digits);
    for (i = 0; i < len && o + 2 < buflen; i++) {
        if (i > 0 && (len - i) % 3 == 0) {
            buf[o++] = ' ';
        }
        buf[o++] = digits[i];
    }
    buf[o] = '\0';
    return buf;
}

static const char *scaled(uint64_t n, const char *const *units, unsigned count, char *buf, size_t buflen)
{
    double v = (double)n;
    unsigned u = 0;

    while (v >= 1000.0 && u + 1 < count) {
        v /= 1000.0;
        u++;
    }
    if (u == 0) {
        snprintf(buf, buflen, "%llu %s", (unsigned long long)n, units[0]);
    } else {
        snprintf(buf, buflen, v < 100.0 ? "%.1f %s" : "%.0f %s", v, units[u]);
    }
    return buf;
}

const char *psk_fmt_bytes(uint64_t n, char *buf, size_t buflen)
{
    static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };

    return scaled(n, units, 5, buf, buflen);
}

const char *psk_fmt_bits(uint64_t bps, char *buf, size_t buflen)
{
    static const char *const units[] = { "BIT/S", "KBIT/S", "MBIT/S", "GBIT/S" };

    return scaled(bps, units, 4, buf, buflen);
}

const char *psk_fmt_elapsed(uint64_t ms, char *buf, size_t buflen)
{
    uint64_t s = ms / 1000u;

    if (s >= 3600u) {
        snprintf(buf, buflen, "%llu:%02u:%02u", (unsigned long long)(s / 3600u), (unsigned)(s / 60u % 60u),
                 (unsigned)(s % 60u));
    } else {
        snprintf(buf, buflen, "%02u:%02u", (unsigned)(s / 60u), (unsigned)(s % 60u));
    }
    return buf;
}

/* ---- the graph and the protocol distribution ---------------------------------------- */

int32_t psk_graph_height_of(uint64_t n)
{
    double h;

    if (n == 0) {
        return 0;
    }
    h = 4.0 + 50.0 * log10((double)n);
    if (h >= PSK_GRAPH_BAND) {
        return PSK_GRAPH_BAND;
    }
    return (int32_t)(h + 0.5);
}

int psk_class_hue(unsigned cls)
{
    /* DS §59.2 (accepted 2026-11-16). Coral, index 0, is not used: it sits too
     * close to status_error. */
    static const int hue[PPC_CLASS_COUNT] = { 6, 5, 4, 1, 2, 7, 3, -1 };

    return cls < PPC_CLASS_COUNT ? hue[cls] : -1;
}

const char *psk_class_word(unsigned cls)
{
    static const char *const words[PPC_CLASS_COUNT] = { "TCP", "UDP", "DNS", "DHCP",
                                                        "ARP", "ICMP", "IPV6", "OTHER" };

    return cls < PPC_CLASS_COUNT ? words[cls] : "OTHER";
}

void psk_model_graph(const struct psk_model *m, struct psk_graph_window *w)
{
    int i;

    memset(w, 0, sizeof(*w));
    w->newest = (int64_t)m->bin_next - 1;
    for (i = 0; i < PSK_GRAPH_SECONDS; i++) {
        int64_t sec = w->newest - (PSK_GRAPH_SECONDS - 1) + i;
        unsigned slot;

        if (sec < 0) {
            continue;
        }
        slot = (unsigned)(sec % PSK_BINS);
        memcpy(w->cls[i], m->bins[slot], sizeof(w->cls[i]));
        w->held[i] = m->bin_held[slot];
        w->watched[i] = 1;
    }
}

static uint64_t bin_total(const uint32_t cls[PPC_CLASS_COUNT], unsigned *largest)
{
    uint64_t t = 0;
    unsigned c;

    *largest = 0;
    for (c = 0; c < PPC_CLASS_COUNT; c++) {
        t += cls[c];
        if (cls[c] > cls[*largest]) {
            *largest = c;
        }
    }
    return t;
}

const char *psk_model_readout(const struct psk_model *m, int64_t sec, char *buf, size_t buflen)
{
    int64_t newest = (int64_t)m->bin_next - 1;
    const uint32_t *cls;
    unsigned largest;
    uint64_t total;
    uint32_t held;
    char t[24];
    char n[32];

    if (sec < 0) {
        sec = newest;
    }
    if (!m->had_capture || (m->have_stats && m->c.seen == 0) || newest < 0) {
        snprintf(buf, buflen, "WAITING FOR PACKETS");
        return buf;
    }
    if (sec > newest || newest - sec >= PSK_BINS) {
        snprintf(buf, buflen, "-");
        return buf;
    }
    cls = m->bins[sec % PSK_BINS];
    held = m->bin_held[sec % PSK_BINS];
    total = bin_total(cls, &largest);
    psk_fmt_elapsed((uint64_t)sec * 1000u, t, sizeof(t));
    if (total == 0 && held > 0) {
        snprintf(buf, buflen, "%s · HELD · %s PASSED", t, psk_fmt_count(held, n, sizeof(n)));
    } else if (total == 0) {
        snprintf(buf, buflen, "%s · 0 PKT", t);
    } else {
        snprintf(buf, buflen, "%s · %s PKT · %s %u%%", t, psk_fmt_count(total, n, sizeof(n)),
                 psk_class_word(largest), (unsigned)((cls[largest] * 100u + total / 2u) / total));
    }
    return buf;
}

uint32_t psk_model_class_rate(const struct psk_model *m, unsigned cls)
{
    int64_t newest = (int64_t)m->bin_next - 1;

    if (cls >= PPC_CLASS_COUNT || newest < 0) {
        return 0;
    }
    return m->bins[newest % PSK_BINS][cls];
}

uint32_t psk_model_class_share_permille(const struct psk_model *m, unsigned cls)
{
    uint64_t total = 0;
    unsigned c;

    for (c = 0; c < PPC_CLASS_COUNT; c++) {
        total += m->c.cls[c];
    }
    if (cls >= PPC_CLASS_COUNT || total == 0) {
        return 0;
    }
    return (uint32_t)((m->c.cls[cls] * 1000u + total / 2u) / total);
}

/* ---- the list and the inspector ------------------------------------------------- */

const char *psk_fmt_rel(uint64_t us, char *buf, size_t buflen)
{
    uint64_t s = us / 1000000u;

    snprintf(buf, buflen, "%02llu:%02llu.%06llu", (unsigned long long)(s / 60u), (unsigned long long)(s % 60u),
             (unsigned long long)(us % 1000000u));
    return buf;
}

const char *psk_fmt_gap(uint64_t us, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "+%llu.%06llu", (unsigned long long)(us / 1000000u), (unsigned long long)(us % 1000000u));
    return buf;
}

const char *psk_dir_glyph(const char *dir)
{
    if (strcmp(dir, "out") == 0) {
        return "\xe2\x86\x91"; /* up: sent by this device */
    }
    if (strcmp(dir, "in") == 0) {
        return "\xe2\x86\x93";
    }
    return "\xc2\xb7";
}

int psk_filter_token(const char *token, uint8_t *classes, char *host, size_t hostlen, uint16_t *port)
{
    char *end;
    unsigned long v;

    if (!token) {
        return -1;
    }
    if (strncmp(token, "host:", 5) == 0 && token[5] && strlen(token + 5) < hostlen &&
        strlen(token + 5) < PPC_HOST_TEXT) {
        snprintf(host, hostlen, "%s", token + 5);
        return 0;
    }
    if (strncmp(token, "port:", 5) == 0) {
        v = strtoul(token + 5, &end, 10);
        if (token[5] && !*end && v > 0 && v <= 65535) {
            *port = (uint16_t)v;
            return 0;
        }
        return -1;
    }
    if (strncmp(token, "class:", 6) == 0) {
        v = strtoul(token + 6, &end, 10);
        if (token[6] && !*end && v < PPC_CLASS_COUNT) {
            *classes = (uint8_t)(1u << v);
            return 0;
        }
        return -1;
    }
    return -1;
}
