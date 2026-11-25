/*
 * PacketShark's model in the shell. See psk_model.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_model.h"
#include "psk_net.h"

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
    m->want_marks = 0;
    m->marks_buckets = 0;
    m->marks_mask = 0;
    /* The network intelligence's lists name packets: they go too. */
    m->want_net = 0;
    m->net_open = false;
    m->have_net_lists = false;
    memset(&m->net, 0, sizeof(m->net));
    /* And the marks: they are moments of that capture. */
    m->nmarks = 0;
    m->first_mark = 0;
    memset(&m->last_mark, 0, sizeof(m->last_mark));
    m->want_ev = 0;
    m->ev_open = false;
    m->have_ev = false;
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
            m->save = PSK_SAVE_NONE;
            m->save_name[0] = '\0';
            m->save_fail[0] = '\0';
            memset(&m->d, 0, sizeof(m->d));
            m->have_diag = false;
            memset(&m->nt, 0, sizeof(m->nt));
            m->have_net = false;
            memset(&m->net, 0, sizeof(m->net));
            m->have_net_lists = false;
            m->net_open = false;
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
    case PPC_EVT_FINDING:
        if (m->pkt_open && m->pkt_next.nfindings < PSK_FINDINGS) {
            struct psk_finding *fd = &m->pkt_next.finding[m->pkt_next.nfindings++];

            fd->layer = (uint8_t)(e->u.finding.layer < PSK_LAYERS ? e->u.finding.layer : 0);
            snprintf(fd->text, sizeof(fd->text), "%s", e->u.finding.text);
        }
        return 0;
    case PPC_EVT_DETAIL:
        if (m->pkt_open && m->pkt_next.ndetails < PSK_DETAILS) {
            struct psk_finding *fd = &m->pkt_next.detail[m->pkt_next.ndetails++];

            fd->layer = (uint8_t)(e->u.finding.layer < PSK_LAYERS ? e->u.finding.layer : 0);
            snprintf(fd->text, sizeof(fd->text), "%s", e->u.finding.text);
        }
        return 0;
    case PPC_EVT_DIAG:
        m->d = e->u.diag;
        m->have_diag = true;
        return PSK_CHANGED_DIAG;
    case PPC_EVT_NET:
        m->nt = e->u.net;
        m->have_net = true;
        return PSK_CHANGED_NET;
    case PPC_EVT_NET_BEGIN:
        if (e->u.net_req != m->want_net || m->want_net == 0) {
            m->net_open = false; /* stale: an answer to an older request */
            return 0;
        }
        memset(&m->net_next, 0, sizeof(m->net_next));
        m->net_open = true;
        return 0;
    case PPC_EVT_NET_HOST:
        if (m->net_open && m->net_next.nhosts < PPC_NET_WIRE_HOSTS) {
            m->net_next.host[m->net_next.nhosts++] = e->u.net_host;
        }
        return 0;
    case PPC_EVT_NET_SRV:
        if (m->net_open && m->net_next.nservers < PPC_NET_WIRE_SERVERS) {
            m->net_next.srv[m->net_next.nservers++] = e->u.net_srv;
        }
        return 0;
    case PPC_EVT_NET_LEASE:
        if (m->net_open && m->net_next.nleases < PPC_NET_WIRE_LEASES) {
            m->net_next.lease[m->net_next.nleases++] = e->u.net_lease;
        }
        return 0;
    case PPC_EVT_NET_EVT:
        if (m->net_open && m->net_next.nevents < PPC_NET_WIRE_EVENTS) {
            m->net_next.evt[m->net_next.nevents++] = e->u.net_evt;
        }
        return 0;
    case PPC_EVT_NET_END:
        if (m->net_open && e->u.net_req == m->want_net) {
            m->net_open = false;
            m->net = m->net_next;
            m->have_net_lists = true;
            return PSK_CHANGED_NETLIST;
        }
        m->net_open = false;
        return 0;
    case PPC_EVT_MARK:
        m->last_mark = e->u.mark;
        m->nmarks = e->u.mark.id;
        m->first_mark = e->u.mark.first;
        m->last_mark_ms = now_ms;
        return PSK_CHANGED_MARK;
    case PPC_EVT_EV_BEGIN:
        if (e->u.ev_head.req != m->want_ev || m->want_ev == 0) {
            m->ev_open = false; /* stale: an answer to an older request */
            return 0;
        }
        memset(&m->ev_next, 0, sizeof(m->ev_next));
        m->ev_next.head = e->u.ev_head;
        m->ev_open = true;
        return 0;
    case PPC_EVT_EV_WIN:
        if (m->ev_open) {
            m->ev_next.win[e->u.ev_win.side] = e->u.ev_win;
        }
        return 0;
    case PPC_EVT_EV_OBS:
        if (m->ev_open && m->ev_next.nobs[e->u.ev_obs.side] < PPC_EV_KEEP) {
            m->ev_next.obs[e->u.ev_obs.side][m->ev_next.nobs[e->u.ev_obs.side]++] = e->u.ev_obs;
        }
        return 0;
    case PPC_EVT_EV_END:
        if (m->ev_open && e->u.ev_req == m->want_ev) {
            m->ev_open = false;
            m->ev = m->ev_next;
            m->have_ev = true;
            /* The answer says which marks are held now. */
            m->first_mark = m->ev.head.first;
            if (m->ev.head.last > m->nmarks) {
                m->nmarks = m->ev.head.last;
            }
            return PSK_CHANGED_EVID;
        }
        m->ev_open = false;
        return 0;
    case PPC_EVT_MARKS:
        if (e->u.marks.req != m->want_marks || m->want_marks == 0) {
            return 0;
        }
        m->marks_view = e->u.marks.view_id;
        m->marks_match = e->u.marks.match;
        m->marks_buckets = e->u.marks.buckets;
        m->marks_mask = e->u.marks.mask;
        return PSK_CHANGED_MARKS;
    case PPC_EVT_SAVED:
        m->save_packets = e->u.saved.packets;
        m->save_total = e->u.saved.total;
        m->save_bytes = e->u.saved.bytes;
        if (e->u.saved.phase == PPC_SAVE_FAIL) {
            m->save = PSK_SAVE_FAILED;
            snprintf(m->save_fail, sizeof(m->save_fail), "%.*s", (int)sizeof(m->save_fail) - 1, e->u.saved.name);
        } else {
            /* Only a name the helper could have made is ever shown. */
            m->save = e->u.saved.phase == PPC_SAVE_DONE ? PSK_SAVE_DONE : PSK_SAVE_RUNNING;
            snprintf(m->save_name, sizeof(m->save_name), "%s", ppc_save_name_ok(e->u.saved.name) ? e->u.saved.name : "");
            m->save_fail[0] = '\0';
        }
        return PSK_CHANGED_SAVE;
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

const char *psk_signal_word(unsigned index)
{
    static const char *const words[PPC_SIGNAL_COUNT] = { "RETRANS", "TCP RST",   "DNS FAIL",
                                                         "UNREACH", "NO ANSWER", "NO DNS REPLY" };

    return index < PPC_SIGNAL_COUNT ? words[index] : "?";
}

const char *psk_signal_plural(unsigned index)
{
    static const char *const words[PPC_SIGNAL_COUNT] = { "RETRANSMISSIONS", "RESETS",          "DNS FAILURES",
                                                         "UNREACHABLES",    "UNANSWERED SYNS", "UNANSWERED DNS QUERIES" };

    return index < PPC_SIGNAL_COUNT ? words[index] : "?";
}

const char *psk_save_fail_text(const char *code)
{
    if (strcmp(code, "nospace") == 0) {
        return "NOT ENOUGH FREE STORAGE";
    }
    if (strcmp(code, "nodir") == 0) {
        return "THE CAPTURES FOLDER CANNOT BE USED";
    }
    if (strcmp(code, "empty") == 0) {
        return "NO PACKET BYTES ARE LEFT IN THE BUFFER";
    }
    return "THE FILE COULD NOT BE WRITTEN";
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

/* ---- the DNS and TCP diagnosis (M11.1) ------------------------------------------------ */

#define DOT " \xc2\xb7 "

const char *psk_fmt_ms(uint64_t us, char *buf, size_t buflen)
{
    unsigned long long u = (unsigned long long)us;

    if (u < 10000ull) {
        snprintf(buf, buflen, "%llu.%llu MS", u / 1000u, u % 1000u / 100u);
    } else if (u < 1000000ull) {
        snprintf(buf, buflen, "%llu MS", (u + 500u) / 1000u);
    } else {
        snprintf(buf, buflen, "%llu.%llu S", u / 1000000u, u % 1000000u / 100000u);
    }
    return buf;
}

/* Lower case for the explanations, which are sentences: "23 ms". */
static const char *ms_lower(uint64_t us, char *buf, size_t buflen)
{
    char *p;

    psk_fmt_ms(us, buf, buflen);
    for (p = buf; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = (char)(*p - 'A' + 'a');
        }
    }
    return buf;
}

static uint32_t dns_failed(const struct ppc_diag *d)
{
    return d->dns_nxdomain + d->dns_servfail + d->dns_refused + d->dns_other;
}

static uint32_t tcp_retrans(const struct ppc_diag *d)
{
    return d->tcp_retrans + d->tcp_retrans_acked + d->tcp_retrans_unseen;
}

/* Retransmissions worth a look: at least three, and at least one in a
 * hundred segments (a few are normal on any network). */
static int retrans_high(const struct ppc_diag *d)
{
    uint32_t n = tcp_retrans(d);

    return n >= 3 && d->tcp_segments > 0 && (uint64_t)n * 100u >= d->tcp_segments;
}

static unsigned dns_sev(const struct psk_model *m)
{
    const struct ppc_diag *d = &m->d;

    if (d->dns_servfail || d->dns_refused || d->dns_other || d->dns_noreply || d->dns_rtt_max_us >= 1000000u) {
        return PSK_SEV_WARN;
    }
    if (d->dns_nxdomain || d->dns_orphan || d->dns_late || d->dns_retries) {
        return PSK_SEV_NOTE;
    }
    return PSK_SEV_OK;
}

static unsigned tcp_sev(const struct psk_model *m)
{
    const struct ppc_diag *d = &m->d;

    if (m->c.sig[4] || d->tcp_aborted || retrans_high(d)) {
        return PSK_SEV_WARN;
    }
    if (d->tcp_refused || d->tcp_rst_unseen || tcp_retrans(d) || d->tcp_syn_again || d->tcp_oneway) {
        return PSK_SEV_NOTE;
    }
    return PSK_SEV_OK;
}

/* One more part of a summary line, after a middle dot when it is not the
 * first. Returns the new length, held inside the buffer. */
static size_t append(char *buf, size_t buflen, size_t o, const char *fmt, const char *arg)
{
    int k;

    if (o >= buflen) {
        return o;
    }
    if (o > 0) {
        k = snprintf(buf + o, buflen - o, DOT);
        o = k > 0 ? o + (size_t)k : o;
        if (o >= buflen) {
            return buflen - 1;
        }
    }
    k = snprintf(buf + o, buflen - o, fmt, arg);
    o = k > 0 ? o + (size_t)k : o;
    return o < buflen ? o : buflen - 1;
}

unsigned psk_diag_summary(const struct psk_model *m, unsigned kind, char *buf, size_t buflen)
{
    const struct ppc_diag *d = &m->d;
    char n[24];
    char t[24];
    size_t o;

    if (kind >= PSK_DIAG_KINDS) {
        return psk_net_summary(m, kind, buf, buflen);
    }
    if (kind == PSK_DIAG_DNS) {
        uint32_t timed = d->dns_answered + d->dns_late;

        if (d->dns_queries == 0 && d->dns_orphan == 0 && dns_failed(d) == 0) {
            snprintf(buf, buflen, "NO QUERY SEEN YET");
            return PSK_SEV_OK;
        }
        /* What needs a look first, so a narrow line still shows it. */
        o = 0;
        buf[0] = '\0';
        if (d->dns_noreply) {
            o = append(buf, buflen, o, "%s NO REPLY", psk_fmt_count(d->dns_noreply, n, sizeof(n)));
        }
        if (dns_failed(d)) {
            o = append(buf, buflen, o, "%s FAILED", psk_fmt_count(dns_failed(d), n, sizeof(n)));
        }
        o = append(buf, buflen, o, "%s ANSWERED", psk_fmt_count(timed, n, sizeof(n)));
        if (timed) {
            append(buf, buflen, o, "AVG %s", psk_fmt_ms(d->dns_rtt_sum_us / timed, t, sizeof(t)));
        }
        return dns_sev(m);
    }
    if (d->tcp_attempts == 0 && d->tcp_connected == 0 && d->tcp_midstream == 0 && m->c.sig[1] == 0) {
        snprintf(buf, buflen, "NO CONNECTION SEEN YET");
        return PSK_SEV_OK;
    }
    o = 0;
    buf[0] = '\0';
    if (m->c.sig[4]) {
        o = append(buf, buflen, o, "%s NO ANSWER", psk_fmt_count(m->c.sig[4], n, sizeof(n)));
    }
    if (d->tcp_aborted) {
        o = append(buf, buflen, o, "%s ABORTED", psk_fmt_count(d->tcp_aborted, n, sizeof(n)));
    }
    if (d->tcp_refused) {
        o = append(buf, buflen, o, "%s REFUSED", psk_fmt_count(d->tcp_refused, n, sizeof(n)));
    }
    if (retrans_high(d)) { /* a few are normal: the sheet counts them */
        o = append(buf, buflen, o, "%s RETRANS", psk_fmt_count(tcp_retrans(d), n, sizeof(n)));
    }
    o = append(buf, buflen, o, "%s CONNECTED", psk_fmt_count(d->tcp_connected, n, sizeof(n)));
    if (d->tcp_timed) {
        append(buf, buflen, o, "AVG %s", psk_fmt_ms(d->tcp_hs_sum_us / d->tcp_timed, t, sizeof(t)));
    }
    return tcp_sev(m);
}

static struct psk_diag_line *add_line(struct psk_diag_line *out, unsigned *n, unsigned max, unsigned sev)
{
    struct psk_diag_line *l;

    if (*n >= max) {
        return NULL;
    }
    l = &out[(*n)++];
    memset(l, 0, sizeof(*l));
    l->sev = (uint8_t)sev;
    return l;
}

static void list_of(struct psk_diag_line *l, unsigned signals, unsigned diag)
{
    l->go = PSK_GO_LIST;
    l->signals = (uint8_t)signals;
    l->diag = (uint8_t)diag;
}

static unsigned dns_lines(const struct psk_model *m, struct psk_diag_line *out, unsigned max)
{
    const struct ppc_diag *d = &m->d;
    uint32_t timed = d->dns_answered + d->dns_late;
    struct psk_diag_line *l;
    unsigned n = 0;
    char a[24];
    char b[24];
    char c[24];

    l = add_line(out, &n, max, d->dns_rtt_max_us >= 1000000u ? PSK_SEV_WARN : PSK_SEV_OK);
    if (l && d->dns_queries == 0) {
        snprintf(l->text, sizeof(l->text), "NO DNS QUERY SEEN");
        snprintf(l->why, sizeof(l->why),
                 "No query to a DNS server (UDP port 53) has passed this interface. mDNS and LLMNR are not judged, "
                 "and DNS over TCP is not read.");
    } else if (l) {
        snprintf(l->text, sizeof(l->text), "%s OF %s QUERIES ANSWERED", psk_fmt_count(timed, a, sizeof(a)),
                 psk_fmt_count(d->dns_queries, b, sizeof(b)));
        if (timed) {
            snprintf(l->why, sizeof(l->why),
                     "Answers took %s on average and %s at most, timed from the latest copy of each query.",
                     ms_lower(d->dns_rtt_sum_us / timed, a, sizeof(a)), ms_lower(d->dns_rtt_max_us, c, sizeof(c)));
            if (d->dns_rtt_max_seq) {
                l->go = PSK_GO_OPEN;
                l->seq = d->dns_rtt_max_seq;
            }
        } else {
            snprintf(l->why, sizeof(l->why), "No answer has been matched to its query yet.");
        }
    }
    if (d->dns_nxdomain && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s NAME NOT FOUND (NXDOMAIN)", psk_fmt_count(d->dns_nxdomain, a, sizeof(a)));
        snprintf(l->why, sizeof(l->why),
                 "The server answered that the name does not exist. A confirmed answer, not a network fault: a "
                 "mistyped name, or a resolver trying its search domains.");
        list_of(l, PPC_SIG_DNS_FAIL, 0);
    }
    if (d->dns_servfail && (l = add_line(out, &n, max, PSK_SEV_WARN)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s SERVER FAILURE (SERVFAIL)", psk_fmt_count(d->dns_servfail, a, sizeof(a)));
        snprintf(l->why, sizeof(l->why),
                 "The server could not answer: its own upstream servers or its DNSSEC check failed. Not caused by "
                 "this device.");
        list_of(l, PPC_SIG_DNS_FAIL, 0);
    }
    if (d->dns_refused + d->dns_other && (l = add_line(out, &n, max, PSK_SEV_WARN)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s REFUSED OR NOT HANDLED",
                 psk_fmt_count(d->dns_refused + d->dns_other, a, sizeof(a)));
        snprintf(l->why, sizeof(l->why),
                 "The server refused this client or could not handle the query: it may not serve this network.");
        list_of(l, PPC_SIG_DNS_FAIL, 0);
    }
    if (d->dns_noreply && (l = add_line(out, &n, max, PSK_SEV_WARN)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s NOT ANSWERED WITHIN 5 S", psk_fmt_count(d->dns_noreply, a, sizeof(a)));
        snprintf(l->why, sizeof(l->why),
                 "No answer seen. The query or its answer may have been lost, the answer may have come in on another "
                 "interface, or the server is down. Not proof of a DNS failure.%s%s%s",
                 d->dns_late ? " " : "", d->dns_late ? psk_fmt_count(d->dns_late, b, sizeof(b)) : "",
                 d->dns_late ? " answered later." : "");
        list_of(l, PPC_SIG_DNS_NOREPLY, 0);
    }
    if (d->dns_retries && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s %s ASKED AGAIN", psk_fmt_count(d->dns_retries, a, sizeof(a)),
                 d->dns_retries == 1 ? "QUERY" : "QUERIES");
        snprintf(l->why, sizeof(l->why),
                 "The resolver sent a query again before it had an answer: the first copy or its answer was slow or "
                 "lost.");
        list_of(l, 0, PPC_DIAG_DNS_RETRY);
    }
    if (d->dns_orphan && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s %s", psk_fmt_count(d->dns_orphan, a, sizeof(a)),
                 d->dns_orphan == 1 ? "ANSWER WITHOUT ITS QUERY" : "ANSWERS WITHOUT THEIR QUERY");
        snprintf(l->why, sizeof(l->why),
                 "Their query is not in this capture: sent before START, while held, or on another interface. Not "
                 "timed or judged.");
        list_of(l, 0, PPC_DIAG_DNS_ORPHAN);
    }
    if (d->dns_waiting && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        bool live = m->state == PPC_STATE_CAPTURING;

        snprintf(l->text, sizeof(l->text), "%s %s", psk_fmt_count(d->dns_waiting, a, sizeof(a)),
                 live ? "WAITING FOR AN ANSWER" : "NOT ANSWERED WHEN IT STOPPED");
        snprintf(l->why, sizeof(l->why), "%s",
                 live ? "Asked less than 5 s ago." : "Still waiting when the capture stopped or was held: not judged.");
    }
    return n;
}

static unsigned tcp_lines(const struct psk_model *m, struct psk_diag_line *out, unsigned max)
{
    const struct ppc_diag *d = &m->d;
    struct psk_diag_line *l;
    unsigned n = 0;
    char a[24];
    char b[24];
    char c[24];

    l = add_line(out, &n, max, PSK_SEV_OK);
    if (l && d->tcp_attempts == 0 && d->tcp_connected == 0) {
        snprintf(l->text, sizeof(l->text), "NO CONNECTION ATTEMPT SEEN");
        snprintf(l->why, sizeof(l->why),
                 "No SYN has passed this interface. Connections already open are followed from their first segment.");
    } else if (l) {
        snprintf(l->text, sizeof(l->text), "%s OF %s CONNECTIONS OPENED", psk_fmt_count(d->tcp_connected, a, sizeof(a)),
                 psk_fmt_count(d->tcp_attempts, b, sizeof(b)));
        if (d->tcp_timed) {
            snprintf(l->why, sizeof(l->why),
                     "Handshakes took %s on average and %s at most, from the first SYN to the segment that completed "
                     "it.",
                     ms_lower(d->tcp_hs_sum_us / d->tcp_timed, a, sizeof(a)), ms_lower(d->tcp_hs_max_us, c, sizeof(c)));
            if (d->tcp_hs_max_seq) {
                l->go = PSK_GO_OPEN;
                l->seq = d->tcp_hs_max_seq;
            }
        } else {
            snprintf(l->why, sizeof(l->why), "No handshake has been seen whole yet, so none is timed.");
        }
    }
    if (m->c.sig[4] && (l = add_line(out, &n, max, PSK_SEV_WARN)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s SYN NOT ANSWERED WITHIN 3 S", psk_fmt_count(m->c.sig[4], a, sizeof(a)));
        snprintf(l->why, sizeof(l->why),
                 "Nothing came back: the host may be down, a firewall may drop the SYN, or the answer took another "
                 "path.");
        list_of(l, PPC_SIG_SYN_UNANSWERED, 0);
    }
    if (d->tcp_refused && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s REFUSED", psk_fmt_count(d->tcp_refused, a, sizeof(a)));
        snprintf(l->why, sizeof(l->why),
                 "A reset answered the SYN: the host is reachable, but nothing listens on that port or a firewall "
                 "rejects it. Not a network fault.");
        list_of(l, 0, PPC_DIAG_TCP_REFUSED);
    }
    if (d->tcp_aborted && (l = add_line(out, &n, max, PSK_SEV_WARN)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s %s", psk_fmt_count(d->tcp_aborted, a, sizeof(a)),
                 d->tcp_aborted == 1 ? "RESET OF AN OPEN CONNECTION" : "RESETS OF OPEN CONNECTIONS");
        snprintf(l->why, sizeof(l->why),
                 "One end aborted a connection that was open or opening. Applications do this too; many of them point "
                 "to a failing service or a middlebox.");
        list_of(l, 0, PPC_DIAG_TCP_ABORTED);
    }
    if (d->tcp_rst_closed && (l = add_line(out, &n, max, PSK_SEV_OK)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s %s AFTER CLOSE", psk_fmt_count(d->tcp_rst_closed, a, sizeof(a)),
                 d->tcp_rst_closed == 1 ? "RESET" : "RESETS");
        snprintf(l->why, sizeof(l->why), "Sent while a connection was closing or already reset: usually normal.");
        list_of(l, 0, PPC_DIAG_TCP_RST_CLOSED);
    }
    if (d->tcp_rst_unseen && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s %s", psk_fmt_count(d->tcp_rst_unseen, a, sizeof(a)),
                 d->tcp_rst_unseen == 1 ? "RESET OF A CONNECTION NOT SEEN" : "RESETS OF CONNECTIONS NOT SEEN");
        snprintf(l->why, sizeof(l->why), "Their connection began before the capture, or was not tracked.");
        list_of(l, 0, PPC_DIAG_TCP_RST_UNSEEN);
    }
    if (tcp_retrans(d) && (l = add_line(out, &n, max, retrans_high(d) ? PSK_SEV_WARN : PSK_SEV_NOTE)) != NULL) {
        uint32_t r = tcp_retrans(d);
        uint64_t pm = d->tcp_segments ? (uint64_t)r * 1000u / d->tcp_segments : 0;

        /* A share means little over a few segments: then the count. */
        if (d->tcp_segments >= 100u) {
            snprintf(l->text, sizeof(l->text), "%s %s" DOT "%llu.%llu%% OF SEGMENTS", psk_fmt_count(r, a, sizeof(a)),
                     r == 1 ? "RETRANSMISSION" : "RETRANSMISSIONS", (unsigned long long)(pm / 10u),
                     (unsigned long long)(pm % 10u));
        } else {
            snprintf(l->text, sizeof(l->text), "%s %s IN %s SEGMENTS", psk_fmt_count(r, a, sizeof(a)),
                     r == 1 ? "RETRANSMISSION" : "RETRANSMISSIONS", psk_fmt_count(d->tcp_segments, b, sizeof(b)));
        }
        {
            char kinds[128];
            size_t k = 0;

            /* Only the kinds there are: what passed here before, what
             * never came by, what was already acknowledged. */
            kinds[0] = '\0';
            if (d->tcp_retrans) {
                k += (size_t)snprintf(kinds + k, sizeof(kinds) - k, "%u had passed here before",
                                      (unsigned)d->tcp_retrans);
            }
            if (d->tcp_retrans_unseen && k < sizeof(kinds)) {
                k += (size_t)snprintf(kinds + k, sizeof(kinds) - k, "%s%u never reached this interface", k ? ", " : "",
                                      (unsigned)d->tcp_retrans_unseen);
            }
            if (d->tcp_retrans_acked && k < sizeof(kinds)) {
                snprintf(kinds + k, sizeof(kinds) - k, "%s%u were already acknowledged (spurious)", k ? ", " : "",
                         (unsigned)d->tcp_retrans_acked);
            }
            if (kinds[0] >= 'a' && kinds[0] <= 'z') {
                kinds[0] = (char)(kinds[0] - 'a' + 'A');
            }
            snprintf(l->why, sizeof(l->why), "%s. A few are normal; more than one in a hundred slows connections.",
                     kinds);
        }
        list_of(l, PPC_SIG_RETRANS, 0);
    }
    if (d->tcp_syn_again && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s %s SENT AGAIN", psk_fmt_count(d->tcp_syn_again, a, sizeof(a)),
                 d->tcp_syn_again == 1 ? "HANDSHAKE STEP" : "HANDSHAKE STEPS");
        snprintf(l->why, sizeof(l->why), "A SYN or SYN-ACK was repeated: the first copy or its answer was slow or lost.");
        list_of(l, 0, PPC_DIAG_TCP_SYN_AGAIN);
    }
    if (d->tcp_ooo && (l = add_line(out, &n, max, PSK_SEV_OK)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s OUT OF ORDER", psk_fmt_count(d->tcp_ooo, a, sizeof(a)));
        snprintf(l->why, sizeof(l->why),
                 "Arrived after later data, within a round trip: reordering, not loss. Not counted as "
                 "retransmissions.");
        list_of(l, 0, PPC_DIAG_TCP_OUT_OF_ORDER);
    }
    if ((d->tcp_midstream || d->tcp_oneway) && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s WITHOUT HANDSHAKE" DOT "%s ONE WAY",
                 psk_fmt_count(d->tcp_midstream, a, sizeof(a)), psk_fmt_count(d->tcp_oneway, b, sizeof(b)));
        snprintf(l->why, sizeof(l->why),
                 "Their start was not seen, so they are not timed. One way: only one direction reached this "
                 "interface, as with asymmetric routing, or the other passed unseen.");
        list_of(l, 0, PPC_DIAG_TCP_MIDSTREAM);
    }
    return n;
}

unsigned psk_diag_lines(const struct psk_model *m, unsigned kind, struct psk_diag_line *out, unsigned max)
{
    if (kind >= PSK_DIAG_KINDS) {
        return psk_net_lines(m, kind, out, max);
    }
    return kind == PSK_DIAG_DNS ? dns_lines(m, out, max) : tcp_lines(m, out, max);
}

const char *psk_diag_confidence(const struct psk_model *m, unsigned kind, char *buf, size_t buflen)
{
    const struct ppc_diag *d = &m->d;
    uint64_t drops = m->c.kdrop + m->c.ifdrop;
    char n[24];
    char t[24];
    char iface[PPC_IFACE_MAX];
    size_t o;
    size_t i;

    if (kind >= PSK_DIAG_KINDS) {
        return psk_net_confidence(m, kind, buf, buflen);
    }
    snprintf(iface, sizeof(iface), "%s", m->iface[0] ? m->iface : "THE INTERFACE");
    for (i = 0; iface[i]; i++) {
        if (iface[i] >= 'a' && iface[i] <= 'z') {
            iface[i] = (char)(iface[i] - 'a' + 'A');
        }
    }
    o = (size_t)snprintf(buf, buflen, "What %s saw in %s, judged as it passed: heuristics, not proof.", iface,
                         psk_fmt_elapsed(m->c.elapsed_ms, t, sizeof(t)));
    if (drops && o < buflen) {
        o += (size_t)snprintf(buf + o, buflen - o, " The kernel dropped %s %s: a missing answer may be one of them.",
                              psk_fmt_count(drops, n, sizeof(n)), drops == 1 ? "packet" : "packets");
    }
    if (d->blinded && o < buflen) {
        o += (size_t)snprintf(buf + o, buflen - o,
                              " Packets passed unjudged %u time%s (HOLD, or too fast to store): what was waiting then "
                              "was let go.",
                              (unsigned)d->blinded, d->blinded == 1 ? "" : "s");
    }
    if (kind == PSK_DIAG_DNS && d->dns_unjudged && o < buflen) {
        o += (size_t)snprintf(buf + o, buflen - o, " %s %s let go unjudged.", psk_fmt_count(d->dns_unjudged, n, sizeof(n)),
                              d->dns_unjudged == 1 ? "query was" : "queries were");
    }
    if (kind == PSK_DIAG_TCP && d->tcp_replaced && o < buflen) {
        o += (size_t)snprintf(buf + o, buflen - o, " The connection table was full: %s %s forgotten.",
                              psk_fmt_count(d->tcp_replaced, n, sizeof(n)),
                              d->tcp_replaced == 1 ? "connection was" : "connections were");
    }
    if (kind == PSK_DIAG_TCP && o < buflen) {
        snprintf(buf + o, buflen - o, " %s %s followed now.", psk_fmt_count(d->tcp_flows, n, sizeof(n)),
                 d->tcp_flows == 1 ? "connection" : "connections");
    }
    return buf;
}

const char *psk_diag_plural(unsigned diag)
{
    static const char *const words[PPC_DIAG_COUNT] = {
        "ANY PACKET",
        "DNS QUERIES",
        "DNS QUERIES ASKED AGAIN",
        "DNS ANSWERS",
        "LATE DNS ANSWERS",
        "ANSWERS WITHOUT THEIR QUERY",
        "CONNECTION ATTEMPTS",
        "HANDSHAKE STEPS SENT AGAIN",
        "SYN-ACKS",
        "COMPLETED HANDSHAKES",
        "CONNECTIONS WITHOUT HANDSHAKE",
        "REFUSED CONNECTIONS",
        "RESETS OF OPEN CONNECTIONS",
        "RESETS AFTER CLOSE",
        "RESETS OF UNSEEN CONNECTIONS",
        "RETRANSMISSIONS SEEN BEFORE",
        "SPURIOUS RETRANSMISSIONS",
        "RETRANSMISSIONS OF UNSEEN DATA",
        "OUT-OF-ORDER SEGMENTS",
    };

    return diag < PPC_DIAG_COUNT ? words[diag] : "?";
}
