/*
 * pos-capture: PacketShark's capture helper (docs/apps/PACKETSHARK.md,
 * ADR-016 accepted). The only Doors program that opens a capture socket.
 *
 *   pos-capture session [--source SPEC]
 *       PacketShark's helper. stdin and stdout are a socketpair to the shell;
 *       the protocol is core/pocketpcap/ppc_proto.h. One helper serves ONE
 *       capture: it opens the source on `start`, drops every capability once
 *       the socket is open (so it cannot open another), and a new capture is
 *       a new helper. It leaves on `quit`, `discard`, end of input, SIGTERM,
 *       and - through PR_SET_PDEATHSIG, set by the app before exec - when the
 *       shell dies. Packets live in its memory only: nothing is written to
 *       storage (DESIGN_DECISIONS D10, accepted).
 *       SPEC: an interface (the default: the one `start` names), file:PATH,
 *       or synth:PPS[:SEED][:crash=N][:step=N,S] (ppc_source.h).
 *       $POCKETOS_CAPTURE_SOURCE is the default for SPEC.
 *   pos-capture decode FILE [--keep headers|full]
 *       One tab-separated row per frame of a .pcap, as the app would show it,
 *       built only from the bytes the stored-bytes rule keeps.
 *   pos-capture bench [FRAMES]
 *       Nanoseconds per frame for decode, decode+store and decode+store+
 *       account on synthetic frames, and per formatted row. No network.
 *   pos-capture backend
 *       "pcap" when this build captures live interfaces, "none" when not.
 *
 * Receive only: nothing in this program or the library it links can send a
 * frame (tests/packetshark_lint.sh). It runs at nice 5 (DESIGN_DECISIONS D5,
 * accepted): under contention the shell wins the one hart and the cost
 * shows as counted kernel drops instead of a frozen panel.
 *
 * Logs are lifecycle lines only, through core/pocketlog: never an address,
 * a name or a byte of a packet (ADR-016 rule 12).
 *
 * Exit codes: 0 done, 1 failure, 2 usage.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "pocketlog/pocketlog.h"
#include "pocketpaths.h"
#include "pocketpcap/ppc_decode.h"
#include "pocketpcap/ppc_dissect.h"
#include "pocketpcap/ppc_format.h"
#include "pocketpcap/ppc_pcapfile.h"
#include "pocketpcap/ppc_proto.h"
#include "pocketpcap/ppc_source.h"
#include "pocketpcap/ppc_stats.h"
#include "pocketpcap/ppc_store.h"
#include "pocketpcap/ppc_view.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define BATCH 256
#define DISPATCH_ROUNDS 32
#define SHED_AFTER_FULL_BATCHES 50
#define STATS_EVERY_MS 250
#define LINK_EVERY_MS 1000
#define OUT_BUF (64 * 1024)
#define ROW_BYTES 2048
#define PKT_TEXT (40 * 1024)
#define HELPER_NICE 5
#define LOCK_NAME "capture.lock"

static volatile sig_atomic_t stop_flag;

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static uint64_t clock_ns(clockid_t id)
{
    struct timespec t;

    clock_gettime(id, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t mono_ms(void)
{
    return clock_ns(CLOCK_MONOTONIC) / 1000000u;
}

/* ---- the session ------------------------------------------------------------------- */

struct helper {
    const char *source_spec;  /* --source, or NULL: the interface `start` names */
    enum ppc_state state;
    enum ppc_reason reason;
    int used;                 /* a start was accepted: this helper is spent */
    char iface[PPC_IFACE_MAX];
    uint32_t speed;
    struct ppc_source src;
    int src_open;
    struct ppc_store store;
    int store_ready;
    /* the packet list's filter and sort (M5); asked for before START, it
     * waits in pend_* until the view exists */
    struct ppc_view view;
    int view_ready;
    struct ppc_filter pend_f;
    enum ppc_sort pend_sort;
    uint32_t pend_id;
    struct ppc_stats st;
    struct ppc_timebase tb;
    enum ppc_keep keep;
    uint32_t limit_s;
    uint64_t start_ms;        /* monotonic, at start */
    uint64_t frozen_ms;       /* elapsed when it stopped */
    int lock_fd;
    int shed;
    int full_batches;
    uint64_t disp_mono_ns;    /* monotonic time when the current dispatch began */
    uint64_t checked_mono_ns; /* the dispatch whose clock was already resampled */
    uint64_t next_stats_ms;
    uint64_t next_link_ms;
    /* output to the shell */
    char out[OUT_BUF];
    size_t out_len;
    int out_socket;
    int peer_gone;
    /* input from the shell */
    char in[PPC_LINE_MAX + 1];
    size_t in_len;
    int overlong;
    int in_socket;
};

static void out_flush(struct helper *h)
{
    while (h->out_len > 0 && !h->peer_gone) {
        ssize_t w;

        if (h->out_socket) {
            w = send(STDOUT_FILENO, h->out, h->out_len, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (w < 0 && errno == ENOTSOCK) {
                h->out_socket = 0;
                continue;
            }
        } else {
            w = write(STDOUT_FILENO, h->out, h->out_len);
        }
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (w <= 0) {
            h->peer_gone = 1;
            h->out_len = 0;
            break;
        }
        memmove(h->out, h->out + w, h->out_len - (size_t)w);
        h->out_len -= (size_t)w;
    }
}

/* Queue one line. A snapshot (stats, bin) is dropped and counted when the
 * shell has not yet taken what was sent before: it is replaced by the next
 * one anyway. Anything else is queued while the buffer has room. Returns 0,
 * or -1 when the line was not queued. */
static int out_line(struct helper *h, const char *line, int len, int snapshot)
{
    if (len <= 0) {
        return -1;
    }
    if (snapshot && h->out_len > 0) {
        h->st.ui_skipped++;
        return -1;
    }
    if (h->out_len + (size_t)len > sizeof(h->out)) {
        if (snapshot) {
            h->st.ui_skipped++;
        }
        return -1;
    }
    memcpy(h->out + h->out_len, line, (size_t)len);
    h->out_len += (size_t)len;
    out_flush(h);
    return 0;
}

static void say_error(struct helper *h, const char *code, const char *text)
{
    char line[PPC_LINE_MAX];

    out_line(h, line, ppc_evt_format_error(line, sizeof(line), code, text), 0);
}

static void say_note(struct helper *h, const char *key, const char *value)
{
    char line[PPC_LINE_MAX];

    out_line(h, line, ppc_evt_format_note(line, sizeof(line), key, value), 0);
}

static int clock_set_now(void)
{
    return ppc_clock_is_set(clock_ns(CLOCK_REALTIME));
}

static void say_state(struct helper *h)
{
    char line[PPC_LINE_MAX];

    out_line(h, line,
             ppc_evt_format_state(line, sizeof(line), h->state, h->reason, h->iface[0] ? h->iface : NULL,
                                  h->speed, clock_set_now()),
             0);
}

static uint64_t elapsed_ms(const struct helper *h)
{
    if (h->state == PPC_STATE_STOPPED || !h->start_ms) {
        return h->frozen_ms;
    }
    return mono_ms() - h->start_ms;
}

static void fill_counters(struct helper *h, struct ppc_counters *c)
{
    int i;

    memset(c, 0, sizeof(*c));
    c->elapsed_ms = elapsed_ms(h);
    c->seen = h->st.seen;
    c->stored = h->st.stored;
    c->held = h->st.held;
    c->kdrop = h->st.kdrop;
    c->ifdrop = h->st.ifdrop;
    c->shed = h->st.shed;
    c->bytes = h->st.bytes;
    c->pps = h->st.last_pps;
    c->bps = h->st.last_bps;
    if (h->store_ready) {
        c->first_seq = ppc_store_count(&h->store) ? h->store.first_seq : 0;
        c->last_seq = h->store.last_seq;
        c->match = ppc_store_count(&h->store);
        if (h->view_ready) {
            ppc_view_update(&h->view, &h->store);
            c->match = ppc_view_count(&h->view);
        }
        c->fill_permille = ppc_store_fill_permille(&h->store);
    }
    c->ui_skipped = h->st.ui_skipped;
    for (i = 0; i < PPC_CLASS_COUNT; i++) {
        c->cls[i] = h->st.cls_pkts[i];
    }
    for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
        c->sig[i] = h->st.signals[i];
    }
}

static void say_stats(struct helper *h, int snapshot)
{
    char line[PPC_LINE_MAX];
    struct ppc_counters c;

    fill_counters(h, &c);
    out_line(h, line, ppc_evt_format_stats(line, sizeof(line), &c), snapshot);
}

static void emit_bin(void *ctx, uint64_t second, const uint32_t cls[PPC_CLASS_COUNT], uint32_t held)
{
    struct helper *h = ctx;
    char line[PPC_LINE_MAX];

    out_line(h, line, ppc_evt_format_bin(line, sizeof(line), second, cls, held), 1);
}

/* ---- privilege ----------------------------------------------------------------------- */

/* The effective and permitted capability sets from /proc/self/status. */
static int caps_now(unsigned long long *eff, unsigned long long *prm, unsigned long long *bnd)
{
    FILE *f = fopen("/proc/self/status", "re");
    char line[128];
    int got = 0;

    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "CapEff: %llx", eff) == 1 || sscanf(line, "CapPrm: %llx", prm) == 1 ||
            sscanf(line, "CapBnd: %llx", bnd) == 1) {
            got++;
        }
    }
    fclose(f);
    return got == 3 ? 0 : -1;
}

/* After the capture is open (ADR-016 rule 17): no new privileges, the
 * bounding set cleared, and every capability set emptied. The open socket
 * keeps working: capabilities are checked when a socket is made, not when
 * it is read. Whether this works on the unit's kernel is checked, not
 * assumed: the outcome is read back from /proc and reported as a note. */
static void drop_privileges(struct helper *h)
{
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[2];
    unsigned long long eff = ~0ull;
    unsigned long long prm = ~0ull;
    unsigned long long bnd = ~0ull;
    int bounding_ok = 1;
    int cap;
    char note[64];

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        LOG_WARN("capture: no_new_privs refused: %s", strerror(errno));
    }
    for (cap = 0; cap < 64; cap++) {
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) != 0) {
            if (errno == EINVAL) {
                break; /* past the kernel's last capability */
            }
            bounding_ok = 0; /* EPERM: not root, nothing in it to lose */
        }
    }
    memset(&hdr, 0, sizeof(hdr));
    memset(data, 0, sizeof(data));
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid = 0;
    if (syscall(SYS_capset, &hdr, data) != 0) {
        LOG_WARN("capture: capset refused: %s", strerror(errno));
    }
    if (caps_now(&eff, &prm, &bnd) != 0) {
        snprintf(note, sizeof(note), "unknown");
    } else if (eff == 0 && prm == 0) {
        snprintf(note, sizeof(note), "dropped bnd=%s", bnd == 0 ? "empty" : "kept");
    } else {
        snprintf(note, sizeof(note), "kept eff=%llx", eff);
    }
    (void)bounding_ok;
    LOG_INFO("capture: capabilities %s", note);
    say_note(h, "caps", note);
}

/* ---- the lock ------------------------------------------------------------------------ */

static int take_lock(void)
{
    char path[POCKETOS_PATH_MAX];
    const char *dir = pocketos_runtime_dir();
    int fd;

    pocketos_mkdir_p(dir, 0755);
    snprintf(path, sizeof(path), "%s/%s", dir, LOCK_NAME);
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -2;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void release_lock(struct helper *h)
{
    if (h->lock_fd >= 0) {
        flock(h->lock_fd, LOCK_UN);
        close(h->lock_fd);
        h->lock_fd = -1;
    }
}

/* ---- the interface ------------------------------------------------------------------- */

/* 1 carrier, 0 none (or the interface is down: the kernel answers EINVAL),
 * -1 the interface is gone. A sysfs read: no socket, no privilege. */
static int carrier(const char *iface)
{
    char path[96];
    char buf[8] = "";
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", iface);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    return buf[0] == '1';
}

static uint32_t link_speed(const char *iface)
{
    char path[96];
    FILE *f;
    long v = 0;

    snprintf(path, sizeof(path), "/sys/class/net/%s/speed", iface);
    f = fopen(path, "re");
    if (!f) {
        return 0;
    }
    if (fscanf(f, "%ld", &v) != 1 || v < 0 || v > 400000) {
        v = 0;
    }
    fclose(f);
    return (uint32_t)v;
}

/* ---- capture ------------------------------------------------------------------------- */

/* Sample the wall and monotonic clocks together; report a step once. */
static void clock_check(struct helper *h)
{
    uint64_t wall = h->src.ops && h->src.ops->wall_ns ? h->src.ops->wall_ns(&h->src) : clock_ns(CLOCK_REALTIME);

    if (ppc_timebase_sample(&h->tb, wall, clock_ns(CLOCK_MONOTONIC))) {
        LOG_INFO("capture: clock-step of %lld s corrected", (long long)(h->tb.last_step_ns / 1000000000LL));
        say_note(h, "clock-step", h->tb.last_step_ns > 0 ? "forward" : "back");
        say_state(h); /* clock_set may have changed */
    }
}

static void on_frame(void *ctx, const struct ppc_frame *f)
{
    struct helper *h = ctx;
    struct ppc_record r;
    uint32_t pay;

    if (h->state != PPC_STATE_CAPTURING) {
        /* HOLD (and SAVING): drained from the kernel, counted, not decoded
         * or kept (ARCHITECTURE §4.4). */
        ppc_stats_held(&h->st, f->wire_len);
        return;
    }
    /* A stamp more than a second from where the wall clock was when this
     * batch began may mean the clock stepped inside the batch: resample
     * before the stamp is used (once per batch; a resample that finds no
     * step changes nothing). */
    if (h->tb.have_offset && h->checked_mono_ns != h->disp_mono_ns) {
        int64_t d = (int64_t)f->ts_ns - ((int64_t)h->disp_mono_ns + h->tb.offset_ns);

        if (d > PPC_STEP_NS || d < -PPC_STEP_NS) {
            h->checked_mono_ns = h->disp_mono_ns;
            clock_check(h);
        }
    }
    pay = ppc_decode(f, &r);
    r.rel_ns = ppc_timebase_rel(&h->tb, f->ts_ns);
    if (h->shed) {
        ppc_stats_frame(&h->st, r.cls, r.wire_len, PPC_FATE_SHED);
        return;
    }
    ppc_store_add(&h->store, &r, f->data, ppc_store_len(&r, pay, f->cap_len, h->keep));
    ppc_stats_frame(&h->st, r.cls, r.wire_len, PPC_FATE_STORED);
}

static void stop_capture(struct helper *h, enum ppc_reason why, const char *detail)
{
    uint64_t kd;
    uint64_t ifd;

    if (h->src_open) {
        if (h->src.ops->stats && h->src.ops->stats(&h->src, &kd, &ifd) == 0) {
            h->st.kdrop = kd;
            h->st.ifdrop = ifd;
        }
        ppc_source_close(&h->src);
        h->src_open = 0;
    }
    release_lock(h);
    if (h->state != PPC_STATE_STOPPED && h->state != PPC_STATE_IDLE) {
        h->frozen_ms = mono_ms() - h->start_ms;
        h->state = PPC_STATE_STOPPED;
        h->reason = why;
        LOG_INFO("capture: stopped (%s%s%s) seen=%llu stored=%llu held=%llu shed=%llu kdrop=%llu",
                 ppc_reason_name(why), detail ? ": " : "", detail ? detail : "",
                 (unsigned long long)h->st.seen, (unsigned long long)h->st.stored,
                 (unsigned long long)h->st.held, (unsigned long long)h->st.shed,
                 (unsigned long long)h->st.kdrop);
        say_state(h);
        say_stats(h, 0);
    }
}

static void ingest(struct helper *h)
{
    int round;

    clock_check(h);
    for (round = 0; round < DISPATCH_ROUNDS && h->src_open; round++) {
        int n;

        h->disp_mono_ns = clock_ns(CLOCK_MONOTONIC);
        n = h->src.ops->dispatch(&h->src, BATCH, on_frame, h);

        if (n == PPC_SRC_EOF) {
            stop_capture(h, PPC_REASON_END, NULL);
            return;
        }
        if (n == PPC_SRC_ERR) {
            /* The source error text can name the interface; nothing else.
             * A live source failing is the link going; a replay failing is
             * a damaged file. */
            stop_capture(h, h->src.live ? PPC_REASON_LINK : PPC_REASON_ERROR, h->src.err);
            return;
        }
        if (n < BATCH) {
            h->full_batches = 0;
            if (h->shed) {
                h->shed = 0;
                LOG_INFO("capture: storing again after shedding %llu", (unsigned long long)h->st.shed);
                say_note(h, "shed", "0");
            }
            return;
        }
        if (++h->full_batches >= SHED_AFTER_FULL_BATCHES && !h->shed) {
            /* The kernel ring is not being emptied: classify and count,
             * stop storing, until a dispatch comes back short (§12.3). */
            h->shed = 1;
            LOG_WARN("capture: falling behind, counting without storing");
            say_note(h, "shed", "1");
        }
    }
}

static void cmd_start(struct helper *h, const struct ppc_cmd *c)
{
    struct ppc_open_opts o;
    enum ppc_open_err e;
    const char *spec;

    if (h->used) {
        say_error(h, "state", "this helper has captured once; a new capture is a new helper");
        return;
    }
    h->lock_fd = take_lock();
    if (h->lock_fd < 0) {
        say_error(h, "busy", h->lock_fd == -1 ? "another capture is running" : "cannot open the lock");
        h->lock_fd = -1;
        return;
    }
    if (ppc_store_init(&h->store, PPC_STORE_SUMMARIES, PPC_STORE_RING_BYTES) != 0) {
        release_lock(h);
        say_error(h, "nomem", "not enough memory for the capture buffer");
        return;
    }
    h->store_ready = 1;
    if (ppc_view_init(&h->view, PPC_STORE_SUMMARIES) != 0) {
        ppc_store_free(&h->store);
        h->store_ready = 0;
        release_lock(h);
        say_error(h, "nomem", "not enough memory for the packet list");
        return;
    }
    h->view_ready = 1;
    ppc_view_set(&h->view, &h->store, h->pend_id, h->pend_sort, &h->pend_f);
    memset(&o, 0, sizeof(o));
    o.iface = c->iface;
    o.keep = (enum ppc_keep)c->keep;
    o.promisc = c->promisc;
    o.cfilter = (enum ppc_cfilter)c->cfilter;
    spec = h->source_spec && *h->source_spec ? h->source_spec : c->iface;
    e = ppc_source_open(&h->src, spec, &o);
    if (e != PPC_OPEN_OK) {
        LOG_WARN("capture: could not open the source (%s)", ppc_open_err_code(e));
        say_error(h, ppc_open_err_code(e), h->src.err);
        ppc_view_free(&h->view);
        h->view_ready = 0;
        ppc_store_free(&h->store);
        h->store_ready = 0;
        release_lock(h);
        return;
    }
    h->src_open = 1;
    h->used = 1;
    drop_privileges(h);
    snprintf(h->iface, sizeof(h->iface), "%s", c->iface);
    h->speed = h->src.live ? link_speed(c->iface) : 0;
    h->keep = (enum ppc_keep)c->keep;
    h->limit_s = c->limit_s;
    ppc_stats_init(&h->st);
    ppc_timebase_init(&h->tb);
    clock_check(h);
    h->start_ms = mono_ms();
    h->next_stats_ms = h->start_ms + STATS_EVERY_MS;
    h->next_link_ms = h->start_ms + LINK_EVERY_MS;
    h->state = PPC_STATE_CAPTURING;
    h->reason = PPC_REASON_NONE;
    LOG_INFO("capture: start %s source=%s keep=%s promisc=%d filter=%s limit=%us", c->iface,
             h->src.ops->name, h->keep == PPC_KEEP_FULL ? "full" : "headers", c->promisc ? 1 : 0,
             ppc_cfilter_id((enum ppc_cfilter)c->cfilter), (unsigned)c->limit_s);
    say_state(h);
}

static void cmd_hold(struct helper *h, int on)
{
    if (on && h->state == PPC_STATE_CAPTURING) {
        h->state = PPC_STATE_HOLD;
    } else if (!on && h->state == PPC_STATE_HOLD) {
        h->state = PPC_STATE_CAPTURING;
    } else if (!((on && h->state == PPC_STATE_HOLD) || (!on && h->state == PPC_STATE_CAPTURING))) {
        say_error(h, "state", "nothing to hold or resume");
        return;
    }
    say_state(h);
}

/* The record of seq and its stored bytes, from the store or from its pin. */
static const struct ppc_record *record_bytes(struct helper *h, uint32_t seq, uint8_t *bytes, uint32_t max,
                                             uint32_t *n)
{
    const struct ppc_pin *pin = NULL;
    const struct ppc_record *r = ppc_view_record(&h->store, seq, &pin);

    *n = 0;
    if (!r) {
        return NULL;
    }
    if (pin) {
        *n = pin->n < max ? pin->n : max;
        memcpy(bytes, pin->bytes, *n);
    } else {
        *n = ppc_store_copy(&h->store, r, bytes, max);
    }
    return r;
}

static uint8_t row_flags(struct helper *h, const struct ppc_record *r, uint32_t n)
{
    uint8_t f = 0;

    if (ppc_store_pinned(&h->store, r->seq)) {
        f |= PPC_ROWF_PINNED;
    }
    if (n == 0 && r->cap_len > 0) {
        f |= PPC_ROWF_GONE;
    }
    if (r->flags & PPC_F_TRUNCATED) {
        f |= PPC_ROWF_TRUNCATED;
    }
    if (r->flags & PPC_F_MALFORMED) {
        f |= PPC_ROWF_MALFORMED;
    }
    if (r->flags & PPC_F_GRO) {
        f |= PPC_ROWF_GRO;
    }
    return f;
}

/* The packet before seq in the view's capture order (the one a row's gap
 * is measured from): the previous row in time order, the next one when the
 * newest come first, and the previous packet captured for the length order
 * and the pins. */
static const struct ppc_record *previous_of(struct helper *h, uint32_t seq, uint32_t rank)
{
    const struct ppc_view *v = &h->view;
    uint32_t prev = 0;

    if (!v->f.pinned && v->sort == PPC_SORT_TIME) {
        prev = rank > 0 ? ppc_view_at(v, rank - 1u) : 0;
    } else if (!v->f.pinned && v->sort == PPC_SORT_NEWEST) {
        prev = ppc_view_at(v, rank + 1u);
    } else {
        prev = seq - 1u;
    }
    return prev ? ppc_view_record(&h->store, prev, NULL) : NULL;
}

static void cmd_view(struct helper *h, const struct ppc_cmd *c)
{
    struct ppc_filter f;
    int len;

    memset(&f, 0, sizeof(f));
    f.classes = c->classes ? c->classes : PPC_CLASSES_ALL;
    f.port = c->port;
    f.pinned = c->pinned;
    if (c->host[0]) {
        len = ppc_filter_host(c->host, f.host);
        if (len < 0) {
            say_error(h, "badarg", "a host this helper cannot read");
            return;
        }
        f.host_len = (uint8_t)len;
    }
    h->pend_f = f;
    h->pend_sort = (enum ppc_sort)c->sort;
    h->pend_id = c->view_id;
    if (h->view_ready) {
        ppc_view_set(&h->view, &h->store, c->view_id, (enum ppc_sort)c->sort, &f);
    }
}

/* One window of the view: at most PPC_WINDOW_MAX rows around an anchor. */
static void cmd_window(struct helper *h, const struct ppc_cmd *c)
{
    static uint8_t bytes[ROW_BYTES];
    char line[PPC_LINE_MAX];
    struct ppc_view *v = &h->view;
    uint32_t count = 0;
    uint32_t first = 0;
    uint32_t last = 0;
    uint32_t anchor = 0;
    uint32_t rank;
    size_t room;

    if (h->view_ready) {
        ppc_view_update(v, &h->store);
        ppc_view_prepare(v, &h->store, mono_ms());
        count = ppc_view_count(v);
    }
    if (count) {
        int time_order = v->sort == PPC_SORT_TIME;
        uint32_t before = c->before < PPC_WINDOW_MAX - 1u ? c->before : PPC_WINDOW_MAX - 1u;
        uint32_t after = c->after < PPC_WINDOW_MAX - 1u - before ? c->after : PPC_WINDOW_MAX - 1u - before;
        uint32_t a;

        switch (c->anchor) {
        case PPC_ANCHOR_SEQ:
            a = ppc_view_rank_of(v, c->seq, NULL);
            break;
        case PPC_ANCHOR_RANK:
            a = c->at < count ? (uint32_t)c->at : count - 1u;
            break;
        case PPC_ANCHOR_US:
            a = ppc_view_rank_of_time(v, &h->store, c->at > UINT64_MAX / 1000u ? UINT64_MAX : c->at * 1000u);
            break;
        case PPC_ANCHOR_TAIL:
        default:
            /* The newest packet's end: the bottom of a time-ordered list,
             * the top of the others. */
            if (time_order) {
                a = count - 1u;
                after = 0;
                before = c->before < PPC_WINDOW_MAX - 1u ? c->before : PPC_WINDOW_MAX - 1u;
            } else {
                a = 0;
                before = 0;
                after = c->after < PPC_WINDOW_MAX - 1u ? c->after : PPC_WINDOW_MAX - 1u;
            }
            break;
        }
        if (a >= count) {
            a = count - 1u;
        }
        first = a - (a < before ? a : before);
        last = count - 1u - a < after ? count - 1u : a + after;
        anchor = a;
    }
    /* The whole answer goes in or none of it: a half window is worse than
     * an "ask again". A row line is at most a few hundred bytes. */
    room = sizeof(h->out) - h->out_len;
    if (room < (size_t)(count ? last - first + 3u : 2u) * 400u) {
        say_error(h, "busy", "the answer does not fit now; ask again");
        return;
    }
    out_line(h, line,
             ppc_evt_format_rows_begin_at(line, sizeof(line), c->req, h->view_ready ? v->id : h->pend_id, first,
                                          count, h->store_ready && ppc_store_count(&h->store) ? h->store.first_seq : 0,
                                          h->store_ready ? h->store.last_seq : 0, anchor),
             0);
    for (rank = first; count && rank <= last; rank++) {
        uint32_t seq = ppc_view_at(v, rank);
        const struct ppc_record *r;
        const struct ppc_record *prev;
        struct ppc_row_text t;
        struct ppc_evt_row row;
        uint32_t n;

        r = seq ? record_bytes(h, seq, bytes, sizeof(bytes), &n) : NULL;
        if (!r) {
            continue; /* left the store since the length order was built */
        }
        prev = previous_of(h, seq, rank);
        ppc_format_row(r, bytes, n, &t);
        memset(&row, 0, sizeof(row));
        row.seq = r->seq;
        row.rel_us = r->rel_ns / 1000u;
        row.gap_us = prev && r->rel_ns >= prev->rel_ns ? (r->rel_ns - prev->rel_ns) / 1000u : 0;
        snprintf(row.dir, sizeof(row.dir), "%s", t.dir);
        row.cls = r->cls;
        snprintf(row.word, sizeof(row.word), "%s", t.word);
        row.len = r->wire_len;
        row.signal = r->signal;
        snprintf(row.src, sizeof(row.src), "%s", t.src);
        snprintf(row.dst, sizeof(row.dst), "%s", t.dst);
        snprintf(row.info, sizeof(row.info), "%s", t.info);
        row.flags = row_flags(h, r, n);
        out_line(h, line, ppc_evt_format_row(line, sizeof(line), &row), 0);
    }
    out_line(h, line, ppc_evt_format_rows_end(line, sizeof(line), c->req), 0);
}

/* The inspector's answer is built here first and queued whole. */
struct pkt_text {
    char buf[PKT_TEXT];
    size_t len;
    int full;
};

static void pkt_add(struct pkt_text *p, const char *line, int len)
{
    if (len <= 0 || p->full) {
        return;
    }
    if (p->len + (size_t)len > sizeof(p->buf)) {
        p->full = 1;
        return;
    }
    memcpy(p->buf + p->len, line, (size_t)len);
    p->len += (size_t)len;
}

static void on_item(void *ctx, const struct ppc_ds_item *it)
{
    struct pkt_text *p = ctx;
    char line[PPC_LINE_MAX];

    if (it->kind == PPC_DS_LAYER) {
        pkt_add(p, line, ppc_evt_format_layer(line, sizeof(line), (uint32_t)it->layer, it->cls, it->name, it->text));
    } else {
        pkt_add(p, line,
                ppc_evt_format_field(line, sizeof(line), (uint32_t)it->layer, it->name, it->text, it->off, it->len,
                                     it->filter));
    }
}

/* One packet for the inspector (M6): its head, its two ends, its layers and
 * fields, and the hex of its first PPC_HEX_MAX stored bytes. A step moves to
 * its neighbour in the list's order. */
static void cmd_inspect(struct helper *h, const struct ppc_cmd *c)
{
    static uint8_t bytes[65536];
    static struct pkt_text p;
    char line[PPC_LINE_MAX];
    struct ppc_view *v = &h->view;
    const struct ppc_record *r;
    const struct ppc_record *prev;
    struct ppc_row_text t;
    struct ppc_evt_pkt head;
    struct ppc_endpoint es;
    struct ppc_endpoint ed;
    struct ppc_evt_end src;
    struct ppc_evt_end dst;
    uint32_t seq = c->seq;
    uint32_t count = 0;
    uint32_t rank = 0;
    uint32_t n = 0;
    uint32_t off;
    int exact = 0;

    if (!h->view_ready) {
        say_error(h, "gone", "no capture holds packets");
        return;
    }
    ppc_view_update(v, &h->store);
    ppc_view_prepare(v, &h->store, mono_ms());
    count = ppc_view_count(v);
    if (c->step != 0 && count > 0) {
        rank = ppc_view_rank_of(v, seq, &exact);
        if (exact) {
            if (c->step > 0 && rank + 1u < count) {
                rank++;
            } else if (c->step < 0 && rank > 0) {
                rank--;
            }
        } else if (c->step < 0 && rank > 0 && v->sort != PPC_SORT_LEN && !v->f.pinned) {
            rank--; /* rank_of gave the next one in the order */
        }
        seq = ppc_view_at(v, rank) ? ppc_view_at(v, rank) : seq;
    }
    r = record_bytes(h, seq, bytes, sizeof(bytes), &n);
    if (!r) {
        say_error(h, "gone", "the packet has left the buffer");
        return;
    }
    rank = ppc_view_rank_of(v, seq, &exact);
    prev = ppc_view_record(&h->store, seq - 1u, NULL);
    ppc_format_row(r, bytes, n, &t);
    memset(&head, 0, sizeof(head));
    head.req = c->req;
    head.seq = seq;
    head.rank = exact ? rank : count;
    head.match = count;
    head.rel_us = r->rel_ns / 1000u;
    head.gap_us = prev && r->rel_ns >= prev->rel_ns ? (r->rel_ns - prev->rel_ns) / 1000u : 0;
    snprintf(head.dir, sizeof(head.dir), "%s", t.dir);
    head.cls = r->cls;
    snprintf(head.word, sizeof(head.word), "%s", t.word);
    head.wire_len = r->wire_len;
    head.stored = n;
    head.signal = r->signal;
    head.flags = row_flags(h, r, n);
    ppc_endpoints(r, &es, &ed);
    snprintf(src.addr, sizeof(src.addr), "%s", es.addr);
    snprintf(src.sub, sizeof(src.sub), "%s", es.sub);
    snprintf(src.filter, sizeof(src.filter), "%s", es.filter);
    snprintf(dst.addr, sizeof(dst.addr), "%s", ed.addr);
    snprintf(dst.sub, sizeof(dst.sub), "%s", ed.sub);
    snprintf(dst.filter, sizeof(dst.filter), "%s", ed.filter);

    p.len = 0;
    p.full = 0;
    pkt_add(&p, line, ppc_evt_format_pkt_begin(line, sizeof(line), &head));
    pkt_add(&p, line, ppc_evt_format_pkt_ends(line, sizeof(line), &src, &dst));
    ppc_dissect(r, bytes, n, on_item, &p);
    for (off = 0; off < n && off < PPC_HEX_MAX; off += PPC_HEX_PER_LINE) {
        char hex[PPC_HEX_TEXT];
        char ascii[PPC_ASCII_TEXT];

        ppc_hex_line(bytes, n, off, hex, sizeof(hex), ascii, sizeof(ascii));
        pkt_add(&p, line, ppc_evt_format_hex(line, sizeof(line), off, hex, ascii));
    }
    pkt_add(&p, line, ppc_evt_format_pkt_end(line, sizeof(line), c->req));
    if (p.full || sizeof(h->out) - h->out_len < p.len) {
        say_error(h, "busy", "the answer does not fit now; ask again");
        return;
    }
    memcpy(h->out + h->out_len, p.buf, p.len);
    h->out_len += p.len;
    out_flush(h);
}

static void cmd_pin(struct helper *h, const struct ppc_cmd *c)
{
    char line[PPC_LINE_MAX];
    uint32_t pins = 0;
    int rc;
    int i;

    if (!h->store_ready) {
        say_error(h, "gone", "no capture holds packets");
        return;
    }
    rc = c->on ? ppc_store_pin(&h->store, c->seq) : ppc_store_unpin(&h->store, c->seq);
    if (rc == -2) {
        say_error(h, "pins", "every pin is taken");
        return;
    }
    if (rc != 0) {
        say_error(h, "gone", c->on ? "the packet has left the buffer" : "the packet is not pinned");
        return;
    }
    for (i = 0; i < PPC_PINS; i++) {
        pins += h->store.pins[i].seq != 0;
    }
    out_line(h, line, ppc_evt_format_pinned(line, sizeof(line), c->seq, c->on, pins), 0);
}

/* One command line. Returns 0 to go on, 1 to leave. */
static int command(struct helper *h, const char *line)
{
    struct ppc_cmd c;

    switch (ppc_cmd_parse(line, &c)) {
    case PPC_CMD_START:
        cmd_start(h, &c);
        return 0;
    case PPC_CMD_HOLD:
        cmd_hold(h, c.on);
        return 0;
    case PPC_CMD_STOP:
        if (h->state == PPC_STATE_CAPTURING || h->state == PPC_STATE_HOLD) {
            stop_capture(h, PPC_REASON_USER, NULL);
        } else {
            say_error(h, "state", "nothing is capturing");
        }
        return 0;
    case PPC_CMD_STATS:
        say_stats(h, 0);
        return 0;
    case PPC_CMD_WINDOW:
        cmd_window(h, &c);
        return 0;
    case PPC_CMD_VIEW:
        cmd_view(h, &c);
        return 0;
    case PPC_CMD_INSPECT:
        cmd_inspect(h, &c);
        return 0;
    case PPC_CMD_PIN:
        cmd_pin(h, &c);
        return 0;
    case PPC_CMD_DISCARD:
        LOG_INFO("capture: discarded by the app");
        return 1;
    case PPC_CMD_QUIT:
        return 1;
    case PPC_CMD_BAD:
        say_error(h, "badarg", "a command this helper cannot read");
        return 0;
    case PPC_CMD_NONE:
    default:
        return 0; /* a word from a newer app, or an empty line */
    }
}

/* Read what the app sent, after poll() said there is something. Returns 1 at
 * end of input or when a command says to leave. stdin and stdout are one
 * socket, so the descriptor is never made non-blocking (pos_zabbix.c explains
 * why): each read is non-blocking by itself, and on a pipe or a terminal one
 * read is made, which poll() said will not block. */
static int read_commands(struct helper *h)
{
    char buf[512];
    int reads = 0;

    for (;;) {
        ssize_t n;
        ssize_t i;

        if (h->in_socket) {
            n = recv(STDIN_FILENO, buf, sizeof(buf), MSG_DONTWAIT);
            if (n < 0 && errno == ENOTSOCK) {
                h->in_socket = 0;
                continue;
            }
        } else if (reads > 0) {
            return 0;
        } else {
            n = read(STDIN_FILENO, buf, sizeof(buf));
        }
        reads++;
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        if (n <= 0) {
            return 1;
        }
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                int leave = 0;

                h->in[h->in_len] = '\0';
                if (h->overlong) {
                    say_error(h, "badarg", "line too long");
                } else {
                    leave = command(h, h->in);
                }
                h->in_len = 0;
                h->overlong = 0;
                if (leave) {
                    return 1;
                }
            } else if (h->in_len + 1 < sizeof(h->in)) {
                h->in[h->in_len++] = buf[i];
            } else {
                h->overlong = 1;
            }
        }
    }
}

static void tick(struct helper *h, uint64_t now)
{
    if (h->src_open) {
        clock_check(h);
    }
    if (h->state == PPC_STATE_CAPTURING || h->state == PPC_STATE_HOLD) {
        uint64_t el = now - h->start_ms;

        ppc_stats_roll(&h->st, el / 1000u, emit_bin, h);
        if (h->limit_s && el >= (uint64_t)h->limit_s * 1000u) {
            stop_capture(h, PPC_REASON_LIMIT, NULL);
        }
    }
    if (h->src_open && h->src.live && now >= h->next_link_ms) {
        h->next_link_ms = now + LINK_EVERY_MS;
        if (carrier(h->iface) != 1) {
            stop_capture(h, PPC_REASON_LINK, "carrier lost");
        }
    }
    if (now >= h->next_stats_ms) {
        uint64_t kd;
        uint64_t ifd;

        h->next_stats_ms = now + STATS_EVERY_MS;
        if (h->src_open && h->src.ops->stats && h->src.ops->stats(&h->src, &kd, &ifd) == 0) {
            h->st.kdrop = kd;
            h->st.ifdrop = ifd;
        }
        if (h->state != PPC_STATE_IDLE) {
            say_stats(h, 1);
        }
    }
}

static int run_session(const char *source_spec)
{
    static struct helper h;
    struct sigaction sa;
    char line[PPC_LINE_MAX];
    int leave = 0;

    memset(&h, 0, sizeof(h));
    h.source_spec = source_spec;
    h.pend_f.classes = PPC_CLASSES_ALL;
    h.lock_fd = -1;
    h.out_socket = 1;
    h.in_socket = 1;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    umask(077);
    errno = 0;
    if (nice(HELPER_NICE) == -1 && errno != 0) {
        LOG_WARN("capture: nice %d refused: %s", HELPER_NICE, strerror(errno));
    }
    pocketlog_install_crash_handler();
    LOG_INFO("capture: session open, backend %s, nice %d", ppc_source_backend(),
             getpriority(PRIO_PROCESS, 0));
    out_line(&h, line, ppc_evt_format_hello(line, sizeof(line), pocketlog_version(), ppc_source_backend()), 0);
    say_state(&h);

    while (!stop_flag && !leave && !h.peer_gone) {
        struct pollfd p[2];
        int np = 1;
        uint64_t now = mono_ms();
        int wait = (int)(h.next_stats_ms > now ? h.next_stats_ms - now : 0);
        int pr;

        if (h.state == PPC_STATE_IDLE || h.state == PPC_STATE_STOPPED) {
            wait = 1000;
        } else if (wait > STATS_EVERY_MS) {
            wait = STATS_EVERY_MS;
        }
        p[0].fd = STDIN_FILENO;
        p[0].events = POLLIN | (h.out_len ? POLLOUT : 0);
        p[0].revents = 0;
        if (h.src_open) {
            p[1].fd = h.src.ops->fd(&h.src);
            p[1].events = POLLIN;
            p[1].revents = 0;
            np = 2;
        }
        pr = poll(p, (nfds_t)np, wait);
        if (pr < 0 && errno != EINTR) {
            break;
        }
        if (pr > 0 && (p[0].revents & POLLOUT)) {
            out_flush(&h);
        }
        if (pr > 0 && np == 2 && (p[1].revents & (POLLIN | POLLERR | POLLHUP))) {
            ingest(&h);
        }
        if (pr > 0 && (p[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            leave = read_commands(&h);
        }
        tick(&h, mono_ms());
    }
    if (h.src_open) {
        ppc_source_close(&h.src);
        h.src_open = 0;
    }
    release_lock(&h);
    if (h.view_ready) {
        ppc_view_free(&h.view);
        h.view_ready = 0;
    }
    if (h.store_ready) {
        ppc_store_free(&h.store);
        h.store_ready = 0;
    }
    out_line(&h, line, ppc_evt_format_bye(line, sizeof(line)), 0);
    out_flush(&h);
    LOG_INFO("capture: session end (%s)", stop_flag ? "signal" : h.peer_gone ? "app gone" : "asked");
    return 0;
}

/* ---- decode -------------------------------------------------------------------------- */

static int run_decode(const char *path, enum ppc_keep keep)
{
    struct ppc_pcap_reader rd;
    struct ppc_frame f;
    uint64_t first = 0;
    uint32_t seq = 0;
    uint64_t stored = 0;
    uint64_t wire = 0;
    int r;

    if (ppc_pcap_open(&rd, path) != 0) {
        fprintf(stderr, "pos-capture: %s: %s\n", path, rd.err);
        return 1;
    }
    printf("# seq\trel_us\tdir\tproto\tsrc\tdst\tlen\tstored\tclass\tinfo\n");
    while ((r = ppc_pcap_next(&rd, &f)) == 1) {
        struct ppc_record rec;
        struct ppc_row_text t;
        uint32_t pay = ppc_decode(&f, &rec);
        uint32_t keepn = ppc_store_len(&rec, pay, f.cap_len, keep);

        seq++;
        if (seq == 1) {
            first = f.ts_ns;
        }
        /* The row is built from what would be kept, exactly as the app
         * sees it, not from the whole frame. */
        ppc_format_row(&rec, f.data, keepn, &t);
        printf("%u\t%llu\t%s\t%s\t%s\t%s\t%u\t%u\t%s\t%s\n", seq,
               (unsigned long long)((f.ts_ns >= first ? f.ts_ns - first : 0) / 1000u), t.dir, t.word,
               t.src, t.dst, (unsigned)rec.wire_len, (unsigned)keepn, ppc_class_name(rec.cls), t.info);
        stored += keepn;
        wire += rec.wire_len;
    }
    printf("# frames=%u wire_bytes=%llu stored_bytes=%llu keep=%s\n", seq, (unsigned long long)wire,
           (unsigned long long)stored, keep == PPC_KEEP_FULL ? "full" : "headers");
    if (r < 0) {
        fprintf(stderr, "pos-capture: %s: %s\n", path, rd.err);
    }
    ppc_pcap_close(&rd);
    return r < 0 ? 1 : 0;
}

/* ---- bench --------------------------------------------------------------------------- */

#define BENCH_TEMPLATES 4096

static int run_bench(uint64_t frames)
{
    static uint8_t tpl[BENCH_TEMPLATES][1600];
    static uint32_t tpl_len[BENCH_TEMPLATES];
    struct ppc_store store;
    struct ppc_stats st;
    struct ppc_frame f;
    struct ppc_record r;
    struct ppc_row_text t;
    uint64_t i;
    uint64_t t0;
    uint64_t sink = 0;
    double ns_decode;
    double ns_store;
    double ns_account;
    double ns_format;
    uint64_t rows;

    for (i = 0; i < BENCH_TEMPLATES; i++) {
        tpl_len[i] = ppc_synth_frame(i, 7, tpl[i]);
    }
    if (ppc_store_init(&store, PPC_STORE_SUMMARIES, PPC_STORE_RING_BYTES) != 0) {
        fprintf(stderr, "pos-capture: bench: no memory for the store\n");
        return 1;
    }
    f.pkttype = PPC_PKT_UNKNOWN;
    f.ts_ns = 0;

    t0 = clock_ns(CLOCK_MONOTONIC);
    for (i = 0; i < frames; i++) {
        f.data = tpl[i % BENCH_TEMPLATES];
        f.cap_len = f.wire_len = tpl_len[i % BENCH_TEMPLATES];
        sink += ppc_decode(&f, &r);
    }
    ns_decode = (double)(clock_ns(CLOCK_MONOTONIC) - t0) / (double)frames;

    t0 = clock_ns(CLOCK_MONOTONIC);
    for (i = 0; i < frames; i++) {
        uint32_t pay;

        f.data = tpl[i % BENCH_TEMPLATES];
        f.cap_len = f.wire_len = tpl_len[i % BENCH_TEMPLATES];
        pay = ppc_decode(&f, &r);
        ppc_store_add(&store, &r, f.data, ppc_store_len(&r, pay, f.cap_len, PPC_KEEP_HEADERS));
    }
    ns_store = (double)(clock_ns(CLOCK_MONOTONIC) - t0) / (double)frames;

    ppc_stats_init(&st);
    t0 = clock_ns(CLOCK_MONOTONIC);
    for (i = 0; i < frames; i++) {
        uint32_t pay;

        f.data = tpl[i % BENCH_TEMPLATES];
        f.cap_len = f.wire_len = tpl_len[i % BENCH_TEMPLATES];
        pay = ppc_decode(&f, &r);
        ppc_store_add(&store, &r, f.data, ppc_store_len(&r, pay, f.cap_len, PPC_KEEP_HEADERS));
        ppc_stats_frame(&st, r.cls, r.wire_len, PPC_FATE_STORED);
        if ((i & 1023u) == 0) {
            ppc_stats_roll(&st, i >> 10, NULL, NULL);
        }
    }
    ns_account = (double)(clock_ns(CLOCK_MONOTONIC) - t0) / (double)frames;

    rows = frames / 10u ? frames / 10u : 1u;
    t0 = clock_ns(CLOCK_MONOTONIC);
    for (i = 0; i < rows; i++) {
        uint8_t b[ROW_BYTES];
        const struct ppc_record *rec = ppc_store_get(&store, store.last_seq - (uint32_t)(i % 1000u));
        uint32_t n = rec ? ppc_store_copy(&store, rec, b, sizeof(b)) : 0;

        if (rec) {
            ppc_format_row(rec, b, n, &t);
            sink += (uint64_t)t.info[0];
        }
    }
    ns_format = (double)(clock_ns(CLOCK_MONOTONIC) - t0) / (double)rows;

    printf("bench frames=%llu\n", (unsigned long long)frames);
    printf("bench decode_ns_per_frame=%.0f\n", ns_decode);
    printf("bench decode_store_ns_per_frame=%.0f\n", ns_store);
    printf("bench decode_store_account_ns_per_frame=%.0f\n", ns_account);
    printf("bench format_row_ns=%.0f\n", ns_format);
    printf("bench store_fill_permille=%u held=%u sink=%llu\n", ppc_store_fill_permille(&store),
           ppc_store_count(&store), (unsigned long long)(sink & 1u));
    ppc_store_free(&store);
    return 0;
}

/* ---- main ---------------------------------------------------------------------------- */

static int usage(void)
{
    fprintf(stderr, "usage: pos-capture session [--source SPEC]\n"
                    "       pos-capture decode FILE [--keep headers|full]\n"
                    "       pos-capture bench [FRAMES]\n"
                    "       pos-capture backend\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return usage();
    }
    if (strcmp(argv[1], "backend") == 0) {
        printf("%s\n", ppc_source_backend());
        return 0;
    }
    if (strcmp(argv[1], "decode") == 0) {
        enum ppc_keep keep = PPC_KEEP_HEADERS;

        if (argc == 5 && strcmp(argv[3], "--keep") == 0) {
            if (strcmp(argv[4], "full") == 0) {
                keep = PPC_KEEP_FULL;
            } else if (strcmp(argv[4], "headers") != 0) {
                return usage();
            }
        } else if (argc != 3) {
            return usage();
        }
        return run_decode(argv[2], keep);
    }
    if (strcmp(argv[1], "bench") == 0) {
        unsigned long long n = argc > 2 ? strtoull(argv[2], NULL, 10) : 200000ull;

        if (argc > 3 || n == 0 || n > 100000000ull) {
            return usage();
        }
        return run_bench(n);
    }
    if (strcmp(argv[1], "session") == 0) {
        const char *spec = getenv("POCKETOS_CAPTURE_SOURCE");

        if (argc == 4 && strcmp(argv[2], "--source") == 0) {
            spec = argv[3];
        } else if (argc != 2) {
            return usage();
        }
        pocketlog_init("pos-capture");
        return run_session(spec);
    }
    return usage();
}
