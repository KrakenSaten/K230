/*
 * Host tests for the intelligence engine's first slice
 * (core/pocketpcap/ppc_intel.h, N1.1).
 *
 * The six scenes of tests/ppc_intel_fixtures.h are played on a virtual
 * clock through the real decoder, the DNS diagnosis, the network
 * intelligence, the N1.0 evidence history and the engine, as the helper
 * would feed them. Checked:
 *   - each scene's ledger against its expected ledger
 *     (tests/data/packetshark/intel/<scene>.ledger), byte for byte, and the
 *     verdicts that matter named again here, so an expected file cannot
 *     bless a wrong answer by itself;
 *   - determinism: each scene twice, and with the clock ticked every 1 s,
 *     250 ms and 50 ms, gives the same ledger;
 *   - replay parity on the host: the frames that reached the capture,
 *     written as a pcap file with the readings, marks and HOLD beside it,
 *     read back and replayed, give the same ledger;
 *   - what the loss signal prevents: loss-healthy played with counters
 *     that report nothing lost claims fewer answers - a false claim;
 *   - the statistics against known values; the loss bound's placement;
 *   - the bounds: a long, busy capture wraps every ring without a fault,
 *     ids are never reused, and a window the rings no longer hold abstains
 *     with "history";
 *   - the cost per frame on this host (figures only).
 *
 * Usage: ppc_intel_test [-v] [-w DIR]   (-w: write each scene's actual
 * ledger, pcap and .vis to DIR, to review or regenerate the expected files)
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketpcap/ppc_decode.h"
#include "pocketpcap/ppc_evid.h"
#include "pocketpcap/ppc_intel.h"
#include "pocketpcap/ppc_net.h"
#include "pocketpcap/ppc_pcapfile.h"
#include "pocketpcap/ppc_proto.h"
#include "pocketpcap/ppc_signals.h"
#include "pocketpcap/ppc_store.h"
#include "ppc_intel_fixtures.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures;
static int verbose;
static const char *write_dir;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

/* ---- readings ------------------------------------------------------------------------------ */

struct reading {
    uint32_t t_ms;
    uint8_t ok;
    uint64_t lost;
};

struct readings {
    struct reading *r;
    uint32_t n;
};

/* Every 250 ms, as the helper reads: what was lost before that moment. */
static void readings_of(const struct ix *x, struct readings *rd, int zero_loss)
{
    uint32_t i = 0;
    uint32_t t;
    uint64_t lost = 0;

    rd->n = 0;
    rd->r = calloc(IX_STOP_MS / IX_READ_MS + 2u, sizeof(*rd->r));
    if (!rd->r) {
        abort();
    }
    for (t = 0; t <= IX_STOP_MS; t += IX_READ_MS) {
        while (i < x->n && x->step[i].t_ms < t) {
            lost += x->step[i].lost;
            i++;
        }
        rd->r[rd->n].t_ms = t;
        rd->r[rd->n].ok = !(t >= x->fail_from && t < x->fail_to);
        rd->r[rd->n].lost = zero_loss ? 0 : lost;
        rd->n++;
    }
}

/* ---- the player ---------------------------------------------------------------------------- */

struct iplay {
    struct ppc_signals sig;
    struct ppc_net net;
    struct ppc_evid ev;
    struct ppc_intel in;
    struct ppc_store store;
    uint32_t lead_ms;
    int have_lead;
    uint8_t self[6];
    int have_self;
    unsigned state; /* PPC_EVS_* */
    uint32_t last_seq;
};

static void on_said(void *ctx, const struct ppc_net_said *s)
{
    struct iplay *p = ctx;
    struct ppc_ev_obs o;

    if (ppc_evid_from_net(s, p->lead_ms, p->have_self ? p->self : NULL, &o)) {
        ppc_evid_note(&p->ev, &o);
        ppc_intel_note(&p->in, &o);
    }
}

static void late_none(void *ctx, uint32_t seq, uint16_t bit)
{
    (void)ctx;
    (void)seq;
    (void)bit;
}

static void play_frame(struct iplay *p, uint32_t t_ms, unsigned dir, const uint8_t *bytes, uint32_t len)
{
    struct ppc_frame f;
    struct ppc_record r;
    struct ppc_ev_obs o;
    uint32_t pay;
    uint32_t seq;

    memset(&f, 0, sizeof(f));
    f.data = bytes;
    f.cap_len = len;
    f.wire_len = len;
    f.pkttype = (uint8_t)dir;
    pay = ppc_decode(&f, &r);
    if (!p->have_lead) {
        p->have_lead = 1;
        p->lead_ms = t_ms;
    }
    r.rel_ns = (uint64_t)(t_ms - p->lead_ms) * 1000000u;
    seq = p->store.last_seq + 1u;
    if (dir == PPC_PKT_OUTGOING && !p->have_self) {
        memcpy(p->self, bytes + 6, 6);
        p->have_self = 1;
    }
    ppc_intel_frame(&p->in, t_ms, dir, 0);
    r.signal = ppc_signals_frame(&p->sig, &r, 0, seq);
    if (ppc_evid_from_dns(&r, ppc_sig_diag(r.signal), p->sig.dns_last_us, dir == PPC_PKT_HOST, p->lead_ms, seq, &o)) {
        ppc_evid_note(&p->ev, &o);
        ppc_intel_note(&p->in, &o);
    }
    ppc_net_frame(&p->net, &r, bytes, len, pay, seq);
    ppc_store_add(&p->store, &r, bytes, ppc_store_len(&r, pay, len, PPC_KEEP_HEADERS));
    p->last_seq = p->store.last_seq;
}

static void both_tick(struct iplay *p, uint32_t t_ms)
{
    ppc_evid_tick(&p->ev, t_ms, p->state);
    ppc_intel_tick(&p->in, t_ms, p->state);
    if (p->have_lead && t_ms >= p->lead_ms) {
        ppc_signals_expire(&p->sig, (uint64_t)(t_ms - p->lead_ms) * 1000000u, late_none, NULL);
        ppc_net_expire(&p->net, (uint64_t)(t_ms - p->lead_ms) * 1000000u);
    }
}

static void both_read(struct iplay *p, const struct reading *r)
{
    ppc_evid_counters(&p->ev, r->t_ms, r->ok, r->lost);
    ppc_intel_counters(&p->in, r->t_ms, r->ok, r->lost);
}

/* A frame step as it is played: from the scene, or read back from a file. */
struct pstep {
    uint32_t t_ms;
    uint8_t op;
    uint8_t dir;
    uint8_t lost;
    uint16_t len;
    const uint8_t *frame;
};

static void play_steps(struct iplay *p, const struct pstep *st, uint32_t n, const struct readings *rd,
                       uint32_t cadence_ms, uint32_t summaries, uint64_t bytes)
{
    uint32_t next_tick = 0;
    uint32_t ri = 0;
    uint32_t i;

    memset(p, 0, sizeof(*p));
    if (ppc_signals_init(&p->sig, PPC_SIG_FLOWS) != 0 || ppc_net_init(&p->net) != 0 || ppc_evid_init(&p->ev) != 0 ||
        ppc_intel_init(&p->in) != 0 ||
        ppc_store_init(&p->store, summaries ? summaries : PPC_STORE_SUMMARIES, bytes ? bytes : PPC_STORE_RING_BYTES) !=
            0) {
        printf("FAIL out of memory\n");
        exit(2);
    }
    ppc_net_listen(&p->net, on_said, p);
    ppc_evid_coverage(&p->ev, PPC_CFILTER_ALL, 0);
    ppc_intel_coverage(&p->in, PPC_CFILTER_ALL, 0);
    p->state = PPC_EVS_WATCHED;
    for (i = 0; i <= n; i++) {
        uint32_t t = i < n ? st[i].t_ms : 0xffffffffu;

        /* readings and ticks due before this step, in time order (a reading first on a tie) */
        for (;;) {
            uint32_t tr = ri < rd->n ? rd->r[ri].t_ms : 0xffffffffu;
            uint32_t tt = next_tick;

            if (tr <= tt && tr <= t && tr != 0xffffffffu) {
                both_read(p, &rd->r[ri++]);
            } else if (tt < tr && tt <= t && tt <= IX_STOP_MS) {
                both_tick(p, tt);
                next_tick += cadence_ms;
            } else {
                break;
            }
        }
        if (i == n) {
            break;
        }
        switch (st[i].op) {
        case IX_FRAME:
            if (!st[i].lost && p->state == PPC_EVS_WATCHED) {
                play_frame(p, st[i].t_ms, st[i].dir, st[i].frame, st[i].len);
            }
            break;
        case IX_HOLD_ON:
        case IX_HOLD_OFF:
            both_tick(p, t);
            p->state = st[i].op == IX_HOLD_ON ? PPC_EVS_HELD : PPC_EVS_WATCHED;
            both_tick(p, t);
            if (st[i].op == IX_HOLD_ON) {
                ppc_signals_blind(&p->sig);
                ppc_net_blind(&p->net);
            }
            break;
        case IX_MARK: {
            const struct ppc_ev_mark *m;

            both_tick(p, t);
            m = ppc_evid_mark(&p->ev, t, p->state == PPC_EVS_HELD ? PPC_STATE_HOLD : PPC_STATE_CAPTURING, p->last_seq,
                              0);
            ppc_intel_mark(&p->in, m->id, m->at_ms);
            break;
        }
        case IX_STOP:
            both_tick(p, t);
            ppc_evid_stop(&p->ev, t, 0);
            ppc_intel_stop(&p->in, t);
            break;
        default:
            break;
        }
    }
}

static void play_end(struct iplay *p)
{
    ppc_store_free(&p->store);
    ppc_intel_free(&p->in);
    ppc_evid_free(&p->ev);
    ppc_net_free(&p->net);
    ppc_signals_free(&p->sig);
}

static struct pstep *steps_of(const struct ix *x)
{
    struct pstep *st = calloc(x->n ? x->n : 1u, sizeof(*st));
    uint32_t i;

    if (!st) {
        abort();
    }
    for (i = 0; i < x->n; i++) {
        st[i].t_ms = x->step[i].t_ms;
        st[i].op = x->step[i].op;
        st[i].dir = x->step[i].dir;
        st[i].lost = x->step[i].lost;
        st[i].len = x->step[i].len;
        st[i].frame = x->step[i].frame;
    }
    return st;
}

static void play_scene(struct iplay *p, const struct ix *x, uint32_t cadence_ms, int zero_loss)
{
    struct readings rd;
    struct pstep *st = steps_of(x);

    readings_of(x, &rd, zero_loss);
    play_steps(p, st, x->n, &rd, cadence_ms, x->store_summaries, x->store_bytes);
    free(rd.r);
    free(st);
}

/* ---- the ledger as text -------------------------------------------------------------------- */

static size_t put(char *buf, size_t cap, size_t at, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static size_t put(char *buf, size_t cap, size_t at, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (at >= cap) {
        return at;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf + at, cap - at, fmt, ap);
    va_end(ap);
    return n > 0 ? at + (size_t)n : at;
}

static const char *notes_text(unsigned notes, char *b)
{
    static const char *const w[] = { "unwatched", "loss", "undecided", "retries", "stopped", "unseen" };
    unsigned i;

    b[0] = 0;
    for (i = 0; i < 6; i++) {
        if (notes & (1u << i)) {
            if (b[0]) {
                strcat(b, ",");
            }
            strcat(b, w[i]);
        }
    }
    if (!b[0]) {
        strcpy(b, "-");
    }
    return b;
}

static size_t vis_text(char *buf, size_t cap, size_t at, const struct ppc_in_vis *v)
{
    return put(buf, cap, at, "[%u-%u w%u h%u s%u g%u fr%u lost%u..%u%s in%u out%u]", v->from_ms, v->to_ms,
               v->watched_s, v->held_s, v->shed_s, v->gap_s, v->frames, v->lost_lo, v->lost_hi,
               v->loss_known ? "" : "?", v->own_in, v->own_out);
}

static size_t cmp_text(char *buf, size_t cap, size_t at, const struct ppc_in_cmp *c, const struct ppc_store *s)
{
    static const char alive[] = { 'x', 's', 'w' }; /* gone, summary only, whole */
    char nb[64];
    unsigned i;

    at = put(buf, cap, at, "cmp %u mark %u %s %-13s %-9s q=%s why=%s notes=%s n=%u|%u k=%u|%u stat=%u|%u und=%u|%u p=%u refs=",
             c->id, c->mark_id, c->pair ? "B" : "A", ppc_in_metric_id(c->metric), ppc_in_verdict_id(c->verdict),
             ppc_in_quality_id(c->quality), ppc_in_reason_id(c->reason), notes_text(c->notes, nb), c->n[0], c->n[1],
             c->k[0], c->k[1], c->stat[0], c->stat[1], c->undecided[0], c->undecided[1], c->p_e6);
    for (i = 0; i < PPC_IN_REFS && c->refs[i]; i++) {
        at = put(buf, cap, at, "%s%u%c", i ? "," : "", c->refs[i], alive[ppc_intel_alive(s, c->refs[i])]);
    }
    if (!c->refs[0]) {
        at = put(buf, cap, at, "-");
    }
    at = put(buf, cap, at, " base=");
    at = vis_text(buf, cap, at, &c->vis[0]);
    at = put(buf, cap, at, " win=");
    at = vis_text(buf, cap, at, &c->vis[1]);
    return put(buf, cap, at, "\n");
}

#define LEDGER_TEXT 65536u

static void ledger_text(const struct iplay *p, const char *name, char *buf)
{
    size_t at = 0;
    uint32_t i;

    at = put(buf, LEDGER_TEXT, at, "scene %s mark %u stop %u packets %u\n", name, IX_MARK_MS, IX_STOP_MS, p->last_seq);
    for (i = 0; i < ppc_intel_ledger_count(&p->in); i++) {
        at = cmp_text(buf, LEDGER_TEXT, at, ppc_intel_ledger(&p->in, i), &p->store);
    }
}

static const struct ppc_in_cmp *find(const struct iplay *p, unsigned pair, unsigned metric)
{
    uint32_t i;

    for (i = 0; i < ppc_intel_ledger_count(&p->in); i++) {
        const struct ppc_in_cmp *c = ppc_intel_ledger(&p->in, i);

        if (c->pair == pair && c->metric == metric) {
            return c;
        }
    }
    return NULL;
}

static int is(const struct iplay *p, unsigned pair, unsigned metric, unsigned verdict, unsigned reason)
{
    const struct ppc_in_cmp *c = find(p, pair, metric);

    return c && c->verdict == verdict && c->reason == reason;
}

/* ---- files --------------------------------------------------------------------------------- */

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;

    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = calloc(1, (size_t)n + 1u);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

static void spill(const char *dir, const char *name, const char *ext, const char *text)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s%s", dir, name, ext);
    f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

#define TS_BASE_NS 1700000000000000000ull

/* What a saved capture of the scene would hold (the frames that reached the
 * store), as a pcap file, and beside it what a pcap cannot carry: the
 * readings, HOLD, the mark, STOP, and the session time of the first frame. */
static int write_replay(const struct ix *x, const char *dir)
{
    char path[512];
    struct readings rd;
    FILE *vis;
    int fd;
    uint32_t i;
    int held = 0;
    int first = 1;

    snprintf(path, sizeof(path), "%s/%s.pcap", dir, x->name);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || ppc_pcap_write_header(fd, 65535) != 0) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s.vis", dir, x->name);
    vis = fopen(path, "w");
    if (!vis) {
        close(fd);
        return -1;
    }
    for (i = 0; i < x->n; i++) {
        const struct ix_step *s = &x->step[i];

        switch (s->op) {
        case IX_FRAME:
            if (!s->lost && !held) {
                if (first) {
                    fprintf(vis, "lead %u\n", s->t_ms);
                    first = 0;
                }
                ppc_pcap_write_record(fd, TS_BASE_NS + (uint64_t)s->t_ms * 1000000u, s->frame, s->len, s->len);
            }
            break;
        case IX_HOLD_ON:
            held = 1;
            fprintf(vis, "hold %u 1\n", s->t_ms);
            break;
        case IX_HOLD_OFF:
            held = 0;
            fprintf(vis, "hold %u 0\n", s->t_ms);
            break;
        case IX_MARK:
            fprintf(vis, "mark %u\n", s->t_ms);
            break;
        case IX_STOP:
            fprintf(vis, "stop %u\n", s->t_ms);
            break;
        default:
            break;
        }
    }
    readings_of(x, &rd, 0);
    for (i = 0; i < rd.n; i++) {
        fprintf(vis, "read %u %u %llu\n", rd.r[i].t_ms, rd.r[i].ok, (unsigned long long)rd.r[i].lost);
    }
    fprintf(vis, "store %u %llu\n", x->store_summaries, (unsigned long long)x->store_bytes);
    free(rd.r);
    fclose(vis);
    close(fd);
    return 0;
}

static unsigned dir_of(const uint8_t *frame, uint32_t len, const uint8_t *self)
{
    if (len < 12) {
        return PPC_PKT_UNKNOWN;
    }
    if (!memcmp(frame + 6, self, 6)) {
        return PPC_PKT_OUTGOING;
    }
    if (!memcmp(frame, self, 6)) {
        return PPC_PKT_HOST;
    }
    if (frame[0] == 0xff) {
        return PPC_PKT_BROADCAST;
    }
    return frame[0] & 1 ? PPC_PKT_MULTICAST : PPC_PKT_OTHERHOST;
}

static int cmp_pstep(const void *a, const void *b)
{
    const struct pstep *x = a;
    const struct pstep *y = b;

    return x->t_ms < y->t_ms ? -1 : x->t_ms > y->t_ms; /* qsort is not stable: frames carry their file order below */
}

/* Read the pcap and the .vis back and play them on the virtual clock. The
 * direction comes from this device's MAC, as POCKETOS_CAPTURE_SELF_MAC
 * gives it to the helper for a replay. */
static int replay_file(const char *dir, const char *name, uint32_t cadence_ms, struct iplay *p)
{
    char path[512];
    struct ppc_pcap_reader rdr;
    struct ppc_frame f;
    struct readings rd = { NULL, 0 };
    struct pstep *st = NULL;
    uint8_t **bytes = NULL;
    uint32_t n = 0, cap = 0, i;
    uint32_t lead = 0;
    uint32_t summaries = 0;
    unsigned long long sbytes = 0;
    uint64_t first_ts = 0;
    uint8_t self[6];
    FILE *vis;
    char line[128];
    int rc;

    fx_mac(self, FX_SELF);
    snprintf(path, sizeof(path), "%s/%s.vis", dir, name);
    vis = fopen(path, "r");
    if (!vis) {
        return -1;
    }
    rd.r = calloc(IX_STOP_MS / IX_READ_MS + 2u, sizeof(*rd.r));
    st = calloc(8, sizeof(*st));
    cap = 8;
    if (!rd.r || !st) {
        abort();
    }
    while (fgets(line, sizeof(line), vis)) {
        unsigned a, b;
        unsigned long long c;
        struct pstep s;

        memset(&s, 0, sizeof(s));
        if (sscanf(line, "lead %u", &a) == 1) {
            lead = a;
            continue;
        } else if (sscanf(line, "read %u %u %llu", &a, &b, &c) == 3) {
            rd.r[rd.n].t_ms = a;
            rd.r[rd.n].ok = (uint8_t)b;
            rd.r[rd.n].lost = c;
            rd.n++;
            continue;
        } else if (sscanf(line, "store %u %llu", &a, &c) == 2) {
            summaries = a;
            sbytes = c;
            continue;
        } else if (sscanf(line, "hold %u %u", &a, &b) == 2) {
            s.t_ms = a;
            s.op = b ? IX_HOLD_ON : IX_HOLD_OFF;
        } else if (sscanf(line, "mark %u", &a) == 1) {
            s.t_ms = a;
            s.op = IX_MARK;
        } else if (sscanf(line, "stop %u", &a) == 1) {
            s.t_ms = a;
            s.op = IX_STOP;
        } else {
            continue;
        }
        if (n == cap) {
            cap *= 2;
            st = realloc(st, cap * sizeof(*st));
        }
        st[n++] = s;
    }
    fclose(vis);
    snprintf(path, sizeof(path), "%s/%s.pcap", dir, name);
    if (ppc_pcap_open(&rdr, path) != 0) {
        return -1;
    }
    bytes = calloc(1, sizeof(*bytes));
    while ((rc = ppc_pcap_next(&rdr, &f)) == 1) {
        struct pstep s;

        if (!first_ts) {
            first_ts = f.ts_ns;
        }
        memset(&s, 0, sizeof(s));
        s.t_ms = lead + (uint32_t)((f.ts_ns - first_ts) / 1000000u);
        s.op = IX_FRAME;
        s.len = (uint16_t)f.cap_len;
        s.dir = (uint8_t)dir_of(f.data, f.cap_len, self);
        if (n == cap) {
            cap *= 2;
            st = realloc(st, cap * sizeof(*st));
        }
        bytes = realloc(bytes, (n + 1u) * sizeof(*bytes));
        bytes[n] = malloc(f.cap_len);
        memcpy(bytes[n], f.data, f.cap_len);
        s.frame = bytes[n];
        st[n++] = s;
    }
    ppc_pcap_close(&rdr);
    /* frames before ops of the same millisecond (as the scene made them),
     * frames in file order: a
     * stable merge by time (insertion sort; the steps are nearly sorted) */
    for (i = 1; i < n; i++) {
        struct pstep v = st[i];
        uint32_t j = i;

        while (j > 0 && (st[j - 1].t_ms > v.t_ms || (st[j - 1].t_ms == v.t_ms && st[j - 1].op != IX_FRAME &&
                                                        v.op == IX_FRAME))) {
            st[j] = st[j - 1];
            j--;
        }
        st[j] = v;
    }
    (void)cmp_pstep;
    play_steps(p, st, n, &rd, cadence_ms, summaries, sbytes);
    for (i = 0; i < n; i++) {
        if (st[i].op == IX_FRAME) {
            free((void *)st[i].frame);
        }
    }
    free(bytes);
    free(st);
    free(rd.r);
    return 0;
}

/* ---- the scenes ---------------------------------------------------------------------------- */

static void scene_checks(const struct iplay *p, const char *name)
{
    char what[300];

#define CHECK(text, cond)                                                                                              \
    do {                                                                                                               \
        snprintf(what, sizeof(what), "%s: %s", name, text);                                                            \
        check(what, cond);                                                                                             \
    } while (0)

    CHECK("ten comparisons made final, five a pair", ppc_intel_ledger_count(&p->in) == 10u);
    if (!strcmp(name, "slow-dns")) {
        CHECK("before the mark nothing changed", is(p, 0, PPC_INM_DNS_TIME, PPC_INV_NO_CHANGE, 0) &&
                                                     is(p, 0, PPC_INM_DNS_ANSWERED, PPC_INV_NO_CHANGE, 0));
        CHECK("after it the answers are slower, on good evidence",
              is(p, 1, PPC_INM_DNS_TIME, PPC_INV_WORSE, 0) && find(p, 1, PPC_INM_DNS_TIME)->quality == PPC_INQ_GOOD);
        CHECK("every question is still answered", is(p, 1, PPC_INM_DNS_ANSWERED, PPC_INV_NO_CHANGE, 0));
        CHECK("its references are the slowest answers, alive",
              find(p, 1, PPC_INM_DNS_TIME)->refs[3] != 0 &&
                  ppc_intel_alive(&p->store, find(p, 1, PPC_INM_DNS_TIME)->refs[0]) == PPC_EV_WHOLE);
    } else if (!strcmp(name, "one-sided")) {
        CHECK("no claim on DNS before or after: one-sided visibility",
              is(p, 0, PPC_INM_DNS_ANSWERED, PPC_INV_CANT_TELL, PPC_INR_ONE_WAY) &&
                  is(p, 1, PPC_INM_DNS_ANSWERED, PPC_INV_CANT_TELL, PPC_INR_ONE_WAY) &&
                  is(p, 0, PPC_INM_DNS_TIME, PPC_INV_CANT_TELL, PPC_INR_ONE_WAY));
        /* The broadcasts it still sees are never a claim: judged no change,
         * or one-way where its inbound gap covers most of the window (the
         * dense inbound of the base makes the window's quiet implausible,
         * and broadcasts may be missing too: G0.md §8). */
        CHECK("the broadcast it still sees makes no claim: no change, or one-way over the gap",
              is(p, 0, PPC_INM_ARP_REQUEST, PPC_INV_NO_CHANGE, 0) ||
                  is(p, 0, PPC_INM_ARP_REQUEST, PPC_INV_CANT_TELL, PPC_INR_ONE_WAY));
    } else if (!strcmp(name, "hold-recovery")) {
        CHECK("before the mark: fewer answered, on good evidence",
              is(p, 0, PPC_INM_DNS_ANSWERED, PPC_INV_WORSE, 0) &&
                  find(p, 0, PPC_INM_DNS_ANSWERED)->quality == PPC_INQ_GOOD);
        CHECK("after it: better, degraded, the recovery not watched",
              is(p, 1, PPC_INM_DNS_ANSWERED, PPC_INV_BETTER, 0) &&
                  find(p, 1, PPC_INM_DNS_ANSWERED)->quality == PPC_INQ_DEGRADED &&
                  (find(p, 1, PPC_INM_DNS_ANSWERED)->notes & PPC_INN_UNWATCHED));
        CHECK("questions whose wait met HOLD are left out, not called unanswered",
              find(p, 1, PPC_INM_DNS_ANSWERED)->undecided[1] > 0);
    } else if (!strcmp(name, "loss-healthy")) {
        CHECK("fewer answers seen, and the loss could account for it: can't tell",
              is(p, 1, PPC_INM_DNS_ANSWERED, PPC_INV_CANT_TELL, PPC_INR_LOSS_EXPLAINS));
        CHECK("the answer time, which loss does not bias, is unchanged",
              is(p, 1, PPC_INM_DNS_TIME, PPC_INV_NO_CHANGE, 0));
        CHECK("no false claim anywhere", !is(p, 1, PPC_INM_DNS_ANSWERED, PPC_INV_WORSE, 0) &&
                                              !is(p, 1, PPC_INM_ARP_REQUEST, PPC_INV_WORSE, 0) &&
                                              !is(p, 1, PPC_INM_DNS_FAILED, PPC_INV_WORSE, 0));
    } else if (!strcmp(name, "fault-under-loss")) {
        CHECK("a real fault under the same loss is still claimed",
              is(p, 1, PPC_INM_DNS_ANSWERED, PPC_INV_WORSE, 0) &&
                  find(p, 1, PPC_INM_DNS_ANSWERED)->quality == PPC_INQ_DEGRADED &&
                  (find(p, 1, PPC_INM_DNS_ANSWERED)->notes & PPC_INN_LOSS));
        CHECK("its references are questions not answered", find(p, 1, PPC_INM_DNS_ANSWERED)->refs[0] != 0);
    } else if (!strcmp(name, "loss-unknown")) {
        CHECK("pair A, final before the counters failed, is untouched",
              is(p, 0, PPC_INM_DNS_ANSWERED, PPC_INV_NO_CHANGE, 0));
        CHECK("the slower answers are still claimed: loss does not bias a time",
              is(p, 1, PPC_INM_DNS_TIME, PPC_INV_WORSE, 0) && find(p, 1, PPC_INM_DNS_TIME)->quality == PPC_INQ_DEGRADED);
        CHECK("the rates abstain: losses not known could hide a rise",
              is(p, 1, PPC_INM_ARP_REQUEST, PPC_INV_CANT_TELL, PPC_INR_LOSS_UNKNOWN) &&
                  is(p, 1, PPC_INM_DHCP_DISCOVER, PPC_INV_CANT_TELL, PPC_INR_LOSS_UNKNOWN));
        CHECK("every question answered stays no change: lost frames cannot hide fewer answers",
              is(p, 1, PPC_INM_DNS_ANSWERED, PPC_INV_NO_CHANGE, 0) &&
                  find(p, 1, PPC_INM_DNS_ANSWERED)->quality == PPC_INQ_DEGRADED);
        {
            const struct ppc_in_cmp *c = find(p, 1, PPC_INM_DNS_TIME);
            unsigned i;
            int gone = 0;

            for (i = 0; i < PPC_IN_REFS; i++) {
                gone += c->refs[i] && ppc_intel_alive(&p->store, c->refs[i]) == PPC_EV_GONE;
            }
            CHECK("a reference whose packet has left the buffer keeps its number and says so", gone > 0);
        }
    }
#undef CHECK
}

static void scenes(void)
{
    static char got[LEDGER_TEXT], again[LEDGER_TEXT];
    char path[512];
    char what[700];
    unsigned i;
    const char *dir = write_dir ? write_dir : "out/packetshark-intel";

    mkdir("out", 0755);
    mkdir(dir, 0755);
    for (i = 0; i < IX_COUNT; i++) {
        struct ix x;
        struct iplay p;
        char *want;
        static const uint32_t cadence[3] = { 1000, 250, 50 };
        unsigned c;
        int same = 1;

        ix_build(i, &x);
        play_scene(&p, &x, 250, 0);
        ledger_text(&p, x.name, got);
        if (verbose) {
            fputs(got, stdout);
        }
        scene_checks(&p, x.name);
        play_end(&p);
        snprintf(path, sizeof(path), "tests/data/packetshark/intel/%s.ledger", x.name);
        want = slurp(path);
        snprintf(what, sizeof(what), "%s: the ledger is the expected one (%s)", x.name, path);
        check(what, want && !strcmp(want, got));
        if (want && strcmp(want, got) && !verbose) {
            fputs(got, stdout);
        }
        free(want);
        spill(dir, x.name, ".ledger", got);
        for (c = 0; c < 3; c++) {
            play_scene(&p, &x, cadence[c], 0);
            ledger_text(&p, x.name, again);
            same &= !strcmp(got, again);
            play_end(&p);
        }
        snprintf(what, sizeof(what), "%s: the same ledger again, and ticked every 1 s, 250 ms and 50 ms", x.name);
        check(what, same);
        same = write_replay(&x, dir) == 0 && replay_file(dir, x.name, 1000, &p) == 0;
        if (same) {
            ledger_text(&p, x.name, again);
            same = !strcmp(got, again);
            if (!same && verbose) {
                fputs(again, stdout);
            }
            play_end(&p);
        }
        snprintf(what, sizeof(what), "%s: replayed from its pcap file and readings, the same ledger", x.name);
        check(what, same);
        ix_free(&x);
    }
}

/* Without the loss signal the engine would make the false claim the
 * signal prevents. */
static void without_loss_signal(void)
{
    struct ix x;
    struct iplay p;

    ix_build(3, &x);
    play_scene(&p, &x, 250, 1);
    check("loss-healthy with counters that report nothing lost: fewer answered is claimed (the false claim the "
          "loss bound prevents)",
          is(&p, 1, PPC_INM_DNS_ANSWERED, PPC_INV_WORSE, 0));
    play_end(&p);
    /* every reading made: the loss is known to the reading, and it accounts
     * for the answers not seen - the corrected comparison is informative */
    x.fail_from = x.fail_to = 0;
    play_scene(&p, &x, 250, 0);
    check("loss-healthy with every reading made: no change, on degraded evidence, the loss noted",
          is(&p, 1, PPC_INM_DNS_ANSWERED, PPC_INV_NO_CHANGE, 0) &&
              find(&p, 1, PPC_INM_DNS_ANSWERED)->quality == PPC_INQ_DEGRADED &&
              (find(&p, 1, PPC_INM_DNS_ANSWERED)->notes & PPC_INN_LOSS));
    play_end(&p);
    ix_free(&x);
}

/* ---- the statistics and the loss bound ---------------------------------------------------- */

static int near(uint32_t got, uint32_t want, uint32_t tol)
{
    return got + tol >= want && got <= want + tol;
}

static void statistics(void)
{
    check("binomial: P(K <= 0 | 10, 0.5) = 0.000977", near(ppc_in_binom_le_e6(0, 10, 0.5), 977, 1));
    check("binomial: P(K <= 5 | 10, 0.5) = 0.623047", near(ppc_in_binom_le_e6(5, 10, 0.5), 623047, 1));
    check("binomial: P(K >= 9 | 10, 0.5) = 0.010742", near(ppc_in_binom_ge_e6(9, 10, 0.5), 10742, 1));
    check("binomial: P(K <= 30 | 40, 0.912) = 0.001995", near(ppc_in_binom_le_e6(30, 40, 0.912), 1995, 1));
    check("binomial: the edges (k = n, p = 0, p = 1)",
          ppc_in_binom_le_e6(10, 10, 0.3) == 1000000u && ppc_in_binom_le_e6(3, 10, 0.0) == 1000000u &&
              ppc_in_binom_le_e6(3, 10, 1.0) == 0 && ppc_in_binom_ge_e6(0, 10, 0.3) == 1000000u);
    check("Poisson: P(C >= 3 | 1.5) = 0.191153", near(ppc_in_poisson_ge_e6(3, 1.5), 191153, 2));
    check("Poisson: P(C <= 2 | 6) = 0.061969", near(ppc_in_poisson_le_e6(2, 6.0), 61969, 2));
    check("Poisson: P(C >= 1000 | 500) is 0, with a bounded loop", ppc_in_poisson_ge_e6(1000, 500.0) == 0);
    check("Poisson: an unbounded expectation claims nothing", ppc_in_poisson_ge_e6(5, 1.0 / 0.0) == 1000000u);
}

static void loss_bound(void)
{
    struct ppc_intel in;
    struct ppc_in_cmp c;
    uint32_t t;

    if (ppc_intel_init(&in) != 0) {
        exit(2);
    }
    /* readings every 250 ms; 40 frames reported lost by the reading at
     * 200.25 s (interval 200.0-200.25: the window after's), 30 by the one at
     * 199.75 s that follows a failure at 199.5 s (interval 199.25-199.75:
     * the window before's), and 20 by a reading at 260.5 s after failures
     * from 259.75 s (interval 259.5-260.5: straddles the window after's end) */
    for (t = 0; t <= 300000; t += 250) {
        uint64_t lost = (t >= 199750 ? 30u : 0u) + (t >= 200250 ? 40u : 0u) + (t >= 260500 ? 20u : 0u);
        int ok = !(t == 199500 || (t >= 259750 && t < 260500));

        ppc_intel_tick(&in, t, PPC_EVS_WATCHED);
        if (t % 1000u == 0) {
            ppc_intel_frame(&in, t, PPC_PKT_BROADCAST, 0);
        }
        if (t == 200000) {
            ppc_intel_mark(&in, 1, 200000);
        }
        ppc_intel_counters(&in, t, ok, lost);
    }
    ppc_intel_compare(&in, 1, 1, PPC_INM_ARP_REQUEST, &c);
    check("loss bound: a reading inside a window is surely its own (lo), on either side of the mark",
          c.vis[0].lost_lo == 30 && c.vis[1].lost_lo == 40);
    check("loss bound: one straddling a window's end may be its own (hi only)",
          c.vis[1].lost_hi == 60 && c.vis[0].lost_hi == 30);
    check("loss bound: both windows' losses are known once a good reading is past their end",
          c.vis[0].loss_known && c.vis[1].loss_known);
    check("loss bound: only intervals with losses are kept", in.nread == 3u);
    ppc_intel_free(&in);
}

/* ---- selective loss in answer times ------------------------------------------------------- */

/* A question at t; answered rtt_ms later, or never (its answer lost, or
 * slower than the wait). Other inbound frames keep the window two-sided. */
static void ask(struct ppc_intel *in, uint32_t t, uint32_t k, int answered, uint32_t rtt_ms)
{
    struct ppc_ev_obs o;

    memset(&o, 0, sizeof(o));
    o.kind = PPC_EV_DNS_QUERY;
    o.flags = PPC_EVF_OWN | PPC_EVF_REPLY_HERE;
    o.t_ms = t;
    o.seq = 2u * k + 1u;
    o.a = k & 0xffffu;
    ppc_intel_note(in, &o);
    ppc_intel_frame(in, t, PPC_PKT_OUTGOING, 0);
    ppc_intel_frame(in, t + 100u, PPC_PKT_HOST, 0);
    if (answered) {
        o.kind = PPC_EV_DNS_ANSWER;
        o.flags = PPC_EVF_OWN;
        o.t_ms = t + rtt_ms;
        o.seq = 2u * k + 2u;
        o.b = rtt_ms * 1000u;
        ppc_intel_frame(in, o.t_ms, PPC_PKT_HOST, 0);
        ppc_intel_note(in, &o);
    }
}

static unsigned selective_case(int base_missing)
{
    struct ppc_intel in;
    struct ppc_in_cmp c;
    uint32_t t;
    unsigned v;

    if (ppc_intel_init(&in) != 0) {
        exit(2);
    }
    for (t = 0; t <= 140000; t += 250) {
        ppc_intel_tick(&in, t, PPC_EVS_WATCHED);
        ppc_intel_counters(&in, t, 1, 0);
        if (t % 1000u == 0 && t >= 1000u && t < 120000u) {
            uint32_t k = t / 1000u;
            int base = t < 61000u;

            if (base_missing) {
                /* base: every other answer never seen; window: all seen, twice as slow */
                ask(&in, t, k, base ? (int)(k % 2u) : 1, base ? 30u : 60u);
            } else {
                /* base: all seen at 30 ms; window: half never seen, the other half as before */
                ask(&in, t, k, base ? 1 : (int)(k % 2u), 30u);
            }
        }
        if (t == 61000u) {
            ppc_intel_mark(&in, 1, 61000u);
        }
    }
    ppc_intel_compare(&in, 1, 1, PPC_INM_DNS_TIME, &c);
    v = (unsigned)c.verdict << 8 | c.reason;
    printf("     selective %d: %s %s n=%u|%u stat=%u|%u und=%u|%u\n", base_missing, ppc_in_verdict_id(c.verdict),
           ppc_in_reason_id(c.reason), c.n[0], c.n[1], c.stat[0], c.stat[1], c.undecided[0], c.undecided[1]);
    ppc_intel_free(&in);
    return v;
}

static void selective(void)
{
    check("selective loss: half the window's answers never seen - not 'no change' (they count as slow)",
          selective_case(0) == ((unsigned)PPC_INV_CANT_TELL << 8 | PPC_INR_WEAK));
    check("selective loss: seen answers twice as slow, half the base's never seen - can't tell, 'unseen'",
          selective_case(1) == ((unsigned)PPC_INV_CANT_TELL << 8 | PPC_INR_UNSEEN));
}

/* ---- the bounds ---------------------------------------------------------------------------- */

static void bounds(void)
{
    struct ppc_intel in;
    struct ppc_in_cmp c;
    struct ppc_ev_obs o;
    uint32_t t;
    uint32_t id = 0;
    uint32_t prev = 0;
    uint32_t i;
    int ids_ok = 1;
    int rc;

    if (ppc_intel_init(&in) != 0) {
        exit(2);
    }
    /* 40 minutes: a question every 200 ms (answered 30 ms later), a mark
     * every 30 s, a reading every 250 ms that reports a frame lost */
    memset(&o, 0, sizeof(o));
    for (t = 0; t <= 2400000; t += 50) {
        ppc_intel_tick(&in, t, PPC_EVS_WATCHED);
        ppc_intel_frame(&in, t, t % 100u ? PPC_PKT_HOST : PPC_PKT_OUTGOING, 0);
        if (t % 200u == 0) {
            o.kind = PPC_EV_DNS_QUERY;
            o.flags = PPC_EVF_OWN | PPC_EVF_REPLY_HERE;
            o.t_ms = t;
            o.seq = t / 50u + 1u;
            o.a = (t / 200u) & 0xffffu;
            ppc_intel_note(&in, &o);
        } else if (t % 200u == 50) {
            o.kind = PPC_EV_DNS_ANSWER;
            o.flags = PPC_EVF_OWN;
            o.t_ms = t;
            o.seq = t / 50u + 1u;
            o.a = ((t - 50u) / 200u) & 0xffffu;
            o.b = 30000;
            ppc_intel_note(&in, &o);
        }
        if (t % 250u == 0) {
            ppc_intel_counters(&in, t, 1, t / 250u);
        }
        if (t % 30000u == 0 && t) {
            ppc_intel_mark(&in, ++id, t);
        }
    }
    for (i = 0; i < ppc_intel_ledger_count(&in); i++) {
        const struct ppc_in_cmp *x = ppc_intel_ledger(&in, i);

        ids_ok &= x->id > prev;
        prev = x->id;
    }
    check("bounds: the ledger holds its last 128 comparisons, ids rising, never reused",
          ppc_intel_ledger_count(&in) == PPC_IN_LEDGER && ids_ok && prev > PPC_IN_LEDGER);
    check("bounds: a mark older than the eight held is not answered", ppc_intel_compare(&in, 1, 0, 0, &c) == -1);
    rc = ppc_intel_compare(&in, id, 0, PPC_INM_DNS_ANSWERED, &c);
    check("bounds: five questions a second fill the question ring: its windows abstain with 'history'",
          rc == 0 && c.verdict == PPC_INV_CANT_TELL && c.reason == PPC_INR_HISTORY);
    rc = ppc_intel_compare(&in, id, 0, PPC_INM_ARP_REQUEST, &c);
    check("bounds: a loss at every reading overflows the readings ring: a window older than it abstains",
          rc == 0 && c.reason == PPC_INR_HISTORY && in.reads_from_ms > 0);
    check("bounds: one allocation of under 64 KiB", ppc_intel_bytes() < 65536u);
    printf("     engine allocation: %u bytes (+%zu in its struct)\n", ppc_intel_bytes(), sizeof(struct ppc_intel));
    ppc_intel_free(&in);
}

/* ---- cost ---------------------------------------------------------------------------------- */

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* The engine's own part of a frame's cost: ppc_intel_frame on every frame,
 * an observation on one frame in ten, a tick and a reading every 250 ms of
 * packet time, at 8,000 frames a second of packet time. Figures only. */
static void cost(void)
{
    struct ppc_intel in;
    struct ppc_ev_obs o;
    const uint32_t frames = 4000000u;
    double best = 1e9;
    int rep;

    for (rep = 0; rep < 5; rep++) {
        double t0, dt;
        uint32_t i;

        if (ppc_intel_init(&in) != 0) {
            exit(2);
        }
        memset(&o, 0, sizeof(o));
        t0 = now_s();
        for (i = 0; i < frames; i++) {
            uint32_t t = i / 8u; /* 8 frames a millisecond */

            ppc_intel_frame(&in, t, i & 1u ? PPC_PKT_HOST : PPC_PKT_OUTGOING, 0);
            if (i % 10u == 0) {
                o.kind = i % 20u ? PPC_EV_ARP_REQUEST : PPC_EV_DNS_QUERY;
                o.flags = PPC_EVF_OWN;
                o.t_ms = t;
                o.seq = i + 1u;
                o.a = i & 0xffffu;
                ppc_intel_note(&in, &o);
            }
            if (i % 2000u == 0) {
                ppc_intel_tick(&in, t, PPC_EVS_WATCHED);
                ppc_intel_counters(&in, t, 1, 0);
                if (t % 60000u == 0) {
                    ppc_intel_mark(&in, t / 60000u + 1u, t);
                }
            }
        }
        dt = now_s() - t0;
        if (dt < best) {
            best = dt;
        }
        ppc_intel_free(&in);
    }
    printf("     cost: %.1f ns a frame (best of 5, %u frames, 8,000 a second of packet time, this host)\n",
           best * 1e9 / frames, frames);
    check("cost: measured (figures only; the device budget is G1's)", best > 0.0);
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) {
            verbose = 1;
        } else if (!strcmp(argv[i], "-w") && i + 1 < argc) {
            write_dir = argv[++i];
        }
    }
    statistics();
    loss_bound();
    scenes();
    without_loss_signal();
    selective();
    bounds();
    cost();
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
