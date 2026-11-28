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
 *       shell dies. Packets live in its memory only (DESIGN_DECISIONS D10,
 *       accepted): one file is ever written, by `save` on a stopped capture,
 *       0600 in the 0700 captures folder (ADR-016 rule 9, M8).
 *       SPEC: an interface (the default: the one `start` names), file:PATH,
 *       or synth:PPS[:SEED][:crash=N][:step=N,S] (ppc_source.h).
 *       $POCKETOS_CAPTURE_SOURCE is the default for SPEC.
 *   pos-capture decode FILE [--keep headers|full]
 *       One tab-separated row per frame of a .pcap, as the app would show it,
 *       built only from the bytes the stored-bytes rule keeps.
 *   pos-capture bench [FRAMES]
 *       Nanoseconds per frame for decode, decode+store and decode+store+
 *       account on synthetic frames, and per formatted row; the list's view
 *       over a full store (a filter set, the length order built) and one
 *       packet dissected for the inspector. No network.
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
#include "pocketpcap/ppc_evid.h"
#include "pocketpcap/ppc_format.h"
#include "pocketpcap/ppc_pcapfile.h"
#include "pocketpcap/ppc_proto.h"
#include "pocketpcap/ppc_net.h"
#include "pocketpcap/ppc_signals.h"
#include "pocketpcap/ppc_source.h"
#include "pocketpcap/ppc_stats.h"
#include "pocketpcap/ppc_store.h"
#include "pocketpcap/ppc_view.h"
#include "capture_priv.h"

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
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* A build for the tests alone (tests/pos-capture-testhooks) can be told to
 * run without an analyser, as a capture whose table could not be allocated
 * does: POS_CAPTURE_TEST_NO_ANALYZER=dns, net or dns,net; and to fail every
 * reading of the drop counters from <from> ms after START until <to> ms (0:
 * to the end), as a source that cannot be read does, the readings after it
 * <lost> higher, as if that many frames had been dropped meanwhile:
 * POS_CAPTURE_TEST_STATS_FAIL=<from>,<to>[,<lost>]. The shipped helper has
 * neither switch. */
#ifndef POS_CAPTURE_TEST_HOOKS
#define POS_CAPTURE_TEST_HOOKS 0
#endif

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
#define SIGNAL_EVERY_MS 500
#define DIAG_EVERY_MS 1000
#define ORIGINAL_LOOKBACK 8192u
#define SAVE_BATCH 1024u
#define SAVE_SNAPLEN 65535u
#define SAVE_MARGIN (4u * 1024u * 1024u)

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
    /* findings (M7) */
    struct ppc_signals sig;
    int sig_ready;
    int view_stale;           /* a finding was added to a stored packet: the view is rebuilt */
    uint64_t next_signal_ms;
    uint64_t next_diag_ms;    /* the DNS and TCP diagnosis (M11.1) */
    /* the network intelligence (M12.1): devices, ARP, DHCP, events */
    struct ppc_net net;
    int net_ready;
    uint64_t next_net_ms;
    /* marks and their evidence (N1.0): the history, the session clock it
     * runs on, and what tells this device's own frames from the rest */
    struct ppc_evid ev;
    int ev_ready;
    int ev_lead_set;
    uint32_t ev_lead_ms;      /* the session time of the first packet */
    uint32_t ev_last_ms;      /* the session time last told to the history */
    uint64_t next_ev_ms;
#if POS_CAPTURE_TEST_HOOKS
    uint64_t test_fail_from_ms; /* POS_CAPTURE_TEST_STATS_FAIL */
    uint64_t test_fail_to_ms;
    uint64_t test_fail_lost;
    int test_fail;
#endif
    uint8_t self_mac[6];
    int self_known;
    /* SAVE (M8): one file, written a batch a turn so the helper keeps answering */
    int save_fd;              /* -1: none being written */
    char save_name[PPC_SAVE_NAME];
    char save_part[PPC_SAVE_NAME + 8]; /* names inside the captures folder (cap_dirfd) */
    char save_path[PPC_SAVE_NAME + 8];
    int cap_dirfd;            /* the captures folder, opened before the privilege drop; -1: none */
    pid_t parent;             /* the shell that started this helper */
    uint32_t save_next;       /* the next seq to write */
    uint32_t save_last;
    uint32_t save_done;
    uint32_t save_total;
    uint64_t save_bytes;
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

/* The wall clock in seconds, 0 while it is not set. */
static uint64_t wall_s_now(void)
{
    return clock_set_now() ? clock_ns(CLOCK_REALTIME) / 1000000000u : 0;
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

static void view_refresh(struct helper *h);

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
            view_refresh(h);
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

/* After the capture is open and before a frame is read (ADR-016 rule 17,
 * amended): leave root for the capture identity, empty every capability
 * set, set no_new_privs, and prove it (capture_priv.c). The open socket
 * keeps working: capabilities and identity are checked when a socket is
 * made, not when it is read. Changing the identity clears the parent-death
 * signal, so it is set again here: the helper still dies with the shell.
 * 0, or -1 with why: the caller refuses the capture (fail closed). */
static int drop_privileges(struct helper *h, char *why, size_t whylen)
{
    struct capture_priv p;
    char note[96];

    pocketlog_keep(); /* the log's descriptor stays; it is never reopened or rotated */
    if (capture_drop(&p) != 0) {
        snprintf(why, whylen, "%s", p.why);
        LOG_ERROR("capture: privilege reduction failed (%s): capture refused", p.why);
        return -1;
    }
    prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
    if (getppid() != h->parent) {
        snprintf(why, whylen, "the shell went away");
        return -1;
    }
    snprintf(note, sizeof(note), "dropped bnd=%s", p.cap_bnd == 0 ? "empty" : "kept");
    LOG_INFO("capture: capabilities %s", note);
    say_note(h, "caps", note);
    snprintf(note, sizeof(note), "%s uid=%u gid=%u groups=%d%s", p.was_root ? "switched" : "unprivileged",
             (unsigned)p.euid, (unsigned)p.egid, p.ngroups, p.was_root ? " netns=own" : "");
    LOG_INFO("capture: identity %s", note);
    say_note(h, "identity", note);
    return 0;
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

static void count_signals(struct helper *h, uint16_t bits)
{
    int i;

    for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
        if (bits & (1u << i)) {
            h->st.signals[i]++;
        }
    }
}

/* A SYN or a DNS query that waited too long: the finding goes on the packet
 * it was, if the buffer still holds it, and is counted either way. */
static void on_late_signal(void *ctx, uint32_t seq, uint16_t bit)
{
    struct helper *h = ctx;

    count_signals(h, bit);
    if (ppc_store_mark(&h->store, seq, bit) == 0 && h->view.f.signals) {
        h->view_stale = 1;
    }
}

/* The DNS and TCP diagnosis (M11.1). A snapshot: when the shell has not
 * read what went before, it waits for the next turn. */
static int say_diag(struct helper *h, int snapshot)
{
    char line[PPC_LINE_MAX];
    struct ppc_diag d;

    if (!h->sig_ready) {
        return 0;
    }
    ppc_signals_diag(&h->sig, &d);
    return out_line(h, line, ppc_evt_format_diag(line, sizeof(line), &d), snapshot);
}

/* The network intelligence's totals (M12.1). A snapshot like the diagnosis. */
static int say_net(struct helper *h, int snapshot)
{
    char line[PPC_LINE_MAX];
    struct ppc_net_totals t;

    if (!h->net_ready) {
        return 0;
    }
    ppc_net_totals(&h->net, &t);
    return out_line(h, line, ppc_evt_format_net(line, sizeof(line), &t), snapshot);
}

/* Both once a second, as one snapshot: two snapshots in a turn would have
 * the second dropped as unread behind the first. */
static int say_figures(struct helper *h)
{
    char line[2 * PPC_LINE_MAX];
    struct ppc_diag d;
    struct ppc_net_totals t;
    int len = 0;
    int k;

    if (h->sig_ready) {
        ppc_signals_diag(&h->sig, &d);
        k = ppc_evt_format_diag(line, PPC_LINE_MAX, &d);
        len = k > 0 ? k : 0;
    }
    if (h->net_ready) {
        ppc_net_totals(&h->net, &t);
        k = ppc_evt_format_net(line + len, PPC_LINE_MAX, &t);
        len += k > 0 ? k : 0;
    }
    return len ? out_line(h, line, len, 1) : 0;
}

/* The view as it is now. A finding added to a packet the view had already
 * passed (above) makes a view that filters on findings rebuild itself. */
static void view_refresh(struct helper *h)
{
    if (!h->view_ready) {
        return;
    }
    if (h->view_stale) {
        struct ppc_filter f = h->view.f;

        h->view_stale = 0;
        ppc_view_set(&h->view, &h->store, h->view.id, h->view.sort, &f);
    }
    ppc_view_update(&h->view, &h->store);
}

/* ---- marks and their evidence (N1.0) ---------------------------------------------------- */

/* The session's clock for the evidence history: milliseconds since START.
 * Before the first packet a live capture counts on the monotonic clock;
 * from the first packet on, on the clock the packets' relative times are
 * told by (step-corrected), so a mark and a packet compare. A replayed file
 * has no clock but its frames': time stands where the newest frame put it.
 * A stopped capture's clock stands still. */
static uint32_t ev_now(struct helper *h)
{
    uint64_t wall;

    if (!h->src_open || (h->state != PPC_STATE_CAPTURING && h->state != PPC_STATE_HOLD)) {
        return h->ev_last_ms;
    }
    if (!h->tb.started) {
        return h->src.live ? (uint32_t)(mono_ms() - h->start_ms) : 0;
    }
    if (!h->src.live && !h->src.ops->wall_ns) {
        return h->ev_last_ms;
    }
    wall = h->src.ops->wall_ns ? h->src.ops->wall_ns(&h->src) : clock_ns(CLOCK_REALTIME);
    return h->ev_lead_ms + (uint32_t)(ppc_timebase_rel(&h->tb, wall) / 1000000u);
}

/* Tell the history the time and what the capture is doing: watching, on
 * HOLD, or counting without judging. Returns the time, which never goes
 * back. */
static uint32_t ev_tick(struct helper *h)
{
    uint32_t t = ev_now(h);

    if (t < h->ev_last_ms) {
        t = h->ev_last_ms;
    }
    h->ev_last_ms = t;
    if (h->ev_ready && (h->state == PPC_STATE_CAPTURING || h->state == PPC_STATE_HOLD)) {
        ppc_evid_tick(&h->ev, t, h->state == PPC_STATE_HOLD ? PPC_EVS_HELD : h->shed ? PPC_EVS_SHED : PPC_EVS_WATCHED);
    }
    return t;
}

/* Read the source's drop counters, and tell the history whether that could
 * be done (N1.0): every reading, wherever it is made - the periodic one, at
 * a MARK, for an answer to `evid`, at STOP - so that what was lost is
 * placed by the readings that actually happened. 1 when read. */
static int read_drops(struct helper *h)
{
    uint64_t kd = 0;
    uint64_t ifd = 0;
    int ok = h->src_open && h->src.ops->stats && h->src.ops->stats(&h->src, &kd, &ifd) == 0;

#if POS_CAPTURE_TEST_HOOKS
    if (ok && h->test_fail) {
        uint64_t el = mono_ms() - h->start_ms;

        if (el >= h->test_fail_from_ms && (h->test_fail_to_ms == 0 || el < h->test_fail_to_ms)) {
            ok = 0;
        } else if (h->test_fail_to_ms && el >= h->test_fail_to_ms) {
            kd += h->test_fail_lost;
        }
    }
#endif
    if (ok) {
        h->st.kdrop = kd;
        h->st.ifdrop = ifd;
    }
    if (h->ev_ready && (h->state == PPC_STATE_CAPTURING || h->state == PPC_STATE_HOLD)) {
        ppc_evid_counters(&h->ev, ev_tick(h), ok, h->st.kdrop + h->st.ifdrop);
    }
    return ok;
}

/* What the network intelligence says of an ARP or DHCP frame, as it says
 * it: an observation, when the frame is one. */
static void on_said(void *ctx, const struct ppc_net_said *s)
{
    struct helper *h = ctx;
    struct ppc_ev_obs o;

    if (h->ev_ready && ppc_evid_from_net(s, h->ev_lead_ms, h->self_known ? h->self_mac : NULL, &o)) {
        ppc_evid_note(&h->ev, &o);
    }
}

static void on_frame(void *ctx, const struct ppc_frame *f)
{
    struct helper *h = ctx;
    struct ppc_record r;
    uint32_t pay;
    uint32_t t_ms = 0;

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
    if (h->ev_ready) {
        if (!h->ev_lead_set) {
            /* The first packet: where it stands on the session's clock. */
            h->ev_lead_set = 1;
            h->ev_lead_ms = h->src.live ? (uint32_t)(mono_ms() - h->start_ms) : 0;
        }
        t_ms = h->ev_lead_ms + (uint32_t)(r.rel_ns / 1000000u);
        if (t_ms > h->ev_last_ms) {
            h->ev_last_ms = t_ms;
        }
        if ((r.flags & PPC_F_OUT) && !h->self_known && f->cap_len >= 12) {
            memcpy(h->self_mac, f->data + 6, 6);
            h->self_known = 1;
        }
    }
    if (h->shed) {
        /* Counted, not analysed: its second was not watched whole, however
         * short the burst that left it so (N1.0). */
        if (h->ev_ready) {
            ppc_evid_bypass(&h->ev, t_ms);
        }
        ppc_stats_frame(&h->st, r.cls, r.wire_len, PPC_FATE_SHED);
        return;
    }
    if (h->sig_ready) {
        r.signal = ppc_signals_frame(&h->sig, &r,
                                     r.info == PPC_INFO_TCP ? ppc_tcp_payload_len(f->data, f->cap_len, pay) : 0,
                                     h->store.last_seq + 1u);
        if (r.signal) {
            count_signals(h, r.signal);
        }
        if (h->ev_ready && r.info == PPC_INFO_DNS) {
            struct ppc_ev_obs o;

            if (ppc_evid_from_dns(&r, ppc_sig_diag(r.signal), h->sig.dns_last_us, f->pkttype == PPC_PKT_HOST,
                                  h->ev_lead_ms, h->store.last_seq + 1u, &o)) {
                ppc_evid_note(&h->ev, &o);
            }
        }
    }
    if (h->net_ready) {
        ppc_net_frame(&h->net, &r, f->data, f->cap_len, pay, h->store.last_seq + 1u);
    }
    ppc_store_add(&h->store, &r, f->data, ppc_store_len(&r, pay, f->cap_len, h->keep));
    ppc_stats_frame(&h->st, r.cls, r.wire_len, PPC_FATE_STORED);
}

static void stop_capture(struct helper *h, enum ppc_reason why, const char *detail)
{
    if (h->src_open) {
        read_drops(h); /* the last reading, or the last that could not be made */
        /* The history's windows close where the capture stops. */
        if (h->ev_ready && (h->state == PPC_STATE_CAPTURING || h->state == PPC_STATE_HOLD)) {
            ppc_evid_stop(&h->ev, ev_tick(h), wall_s_now());
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
        say_diag(h, 0);
        say_net(h, 0);
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
                ev_tick(h); /* up to here the capture only counted */
                h->shed = 0;
                ev_tick(h); /* and from here it watches again */
                LOG_INFO("capture: storing again after shedding %llu", (unsigned long long)h->st.shed);
                say_note(h, "shed", "0");
            }
            return;
        }
        if (++h->full_batches >= SHED_AFTER_FULL_BATCHES && !h->shed) {
            /* The kernel ring is not being emptied: classify and count,
             * stop storing, until a dispatch comes back short (§12.3). */
            ev_tick(h); /* up to here the capture was watching */
            h->shed = 1;
            ev_tick(h); /* and from here it only counts */
            if (h->sig_ready) {
                ppc_signals_blind(&h->sig);
            }
            if (h->net_ready) {
                ppc_net_blind(&h->net);
            }
            LOG_WARN("capture: falling behind, counting without storing");
            say_note(h, "shed", "1");
        }
    }
}

/* Give back what a refused start had taken: tables, the source, the lock. */
static void start_undo(struct helper *h)
{
    if (h->src_open) {
        ppc_source_close(&h->src);
        h->src_open = 0;
    }
    if (h->cap_dirfd >= 0) {
        close(h->cap_dirfd);
        h->cap_dirfd = -1;
    }
    ppc_view_free(&h->view);
    h->view_ready = 0;
    if (h->sig_ready) {
        ppc_signals_free(&h->sig);
        h->sig_ready = 0;
    }
    if (h->net_ready) {
        ppc_net_free(&h->net);
        h->net_ready = 0;
    }
    if (h->ev_ready) {
        ppc_evid_free(&h->ev);
        h->ev_ready = 0;
    }
    ppc_store_free(&h->store);
    h->store_ready = 0;
    release_lock(h);
}

static void cmd_start(struct helper *h, const struct ppc_cmd *c)
{
    struct ppc_open_opts o;
    enum ppc_open_err e;
    const char *spec;
    char dirbuf[POCKETOS_PATH_MAX];
    char why[96];

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
    /* Findings need their table; a capture without it still captures. */
    h->sig_ready = ppc_signals_init(&h->sig, PPC_SIG_FLOWS) == 0;
    /* The same of the network intelligence and its tables. */
    h->net_ready = ppc_net_init(&h->net) == 0;
#if POS_CAPTURE_TEST_HOOKS
    {
        const char *fail = getenv("POS_CAPTURE_TEST_STATS_FAIL");
        char *end = NULL;

        if (fail && *fail) {
            h->test_fail_from_ms = strtoull(fail, &end, 10);
            h->test_fail_to_ms = end && *end == ',' ? strtoull(end + 1, &end, 10) : 0;
            h->test_fail_lost = end && *end == ',' ? strtoull(end + 1, NULL, 10) : 0;
            h->test_fail = 1;
        }
    }
    {
        const char *drop = getenv("POS_CAPTURE_TEST_DROP_FAIL");

        capture_priv_test_fail(drop && *drop == '1');
    }
    {
        const char *no = getenv("POS_CAPTURE_TEST_NO_ANALYZER");

        if (no && strstr(no, "dns") && h->sig_ready) {
            ppc_signals_free(&h->sig);
            h->sig_ready = 0;
        }
        if (no && strstr(no, "net") && h->net_ready) {
            ppc_net_free(&h->net);
            h->net_ready = 0;
        }
    }
#endif
    /* Marks and their evidence (N1.0), fed by the two above. */
    h->ev_ready = ppc_evid_init(&h->ev) == 0;
    if (h->ev_ready && h->net_ready) {
        ppc_net_listen(&h->net, on_said, h);
    }
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
        start_undo(h);
        return;
    }
    h->src_open = 1;
    h->used = 1;
    /* The captures folder, while the helper may still reach it: SAVE writes
     * through this descriptor after the drop (ADR-016 rule 9, amended). */
    h->cap_dirfd = capture_captures_dir(ppc_captures_dir(dirbuf, sizeof(dirbuf)));
    /* Least privilege before the first frame is read; fail closed. A
     * failed reduction may have left this process partly privileged, so it
     * says why and ends: the next START is a new helper. */
    if (drop_privileges(h, why, sizeof(why)) != 0) {
        start_undo(h);
        say_error(h, "privilege", why);
        out_flush(h);
        _exit(3);
    }
    if (h->ev_ready) {
        /* What the evidence can observe: the capture filter only a live
         * source applies, and the analyses that could not start. */
        ppc_evid_coverage(&h->ev, h->src.live ? c->cfilter : PPC_CFILTER_ALL,
                          ppc_evid_unanalyzed(h->sig_ready, h->net_ready));
    }
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
    h->next_signal_ms = h->start_ms + SIGNAL_EVERY_MS;
    h->next_diag_ms = h->start_ms + DIAG_EVERY_MS;
    h->next_net_ms = h->start_ms + DIAG_EVERY_MS;
    h->next_ev_ms = h->start_ms;
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
        ev_tick(h); /* up to here the capture was watching */
        h->state = PPC_STATE_HOLD;
        if (h->sig_ready) {
            ppc_signals_blind(&h->sig); /* answers pass unseen from here */
        }
        if (h->net_ready) {
            ppc_net_blind(&h->net);
        }
    } else if (!on && h->state == PPC_STATE_HOLD) {
        ev_tick(h);
        h->state = PPC_STATE_CAPTURING;
    } else if (!((on && h->state == PPC_STATE_HOLD) || (!on && h->state == PPC_STATE_CAPTURING))) {
        say_error(h, "state", "nothing to hold or resume");
        return;
    }
    ev_tick(h); /* and from here on it is what it now is */
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
    f.signals = c->signals;
    f.diag = c->diag;
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
        h->view_stale = 0;
        ppc_view_set(&h->view, &h->store, c->view_id, (enum ppc_sort)c->sort, &f);
    }
}

/* Where the view's findings are (M7): the view cut into `buckets` equal
 * parts, a bit for each part that holds a packet with a finding. A view
 * that shows only findings has no marks to add. */
static void cmd_marks(struct helper *h, const struct ppc_cmd *c)
{
    char line[PPC_LINE_MAX];
    struct ppc_view *v = &h->view;
    uint64_t mask = 0;
    uint32_t count = 0;
    uint32_t rank;

    if (h->view_ready) {
        view_refresh(h);
        ppc_view_prepare(v, &h->store, mono_ms());
        count = ppc_view_count(v);
    }
    for (rank = 0; rank < count && !v->f.signals; rank++) {
        uint32_t seq = ppc_view_at(v, rank);
        const struct ppc_record *r = seq ? ppc_view_record(&h->store, seq, NULL) : NULL;

        if (r && ppc_sig_findings(r->signal)) {
            mask |= 1ull << (uint32_t)((uint64_t)rank * c->buckets / count);
        }
    }
    out_line(h, line,
             ppc_evt_format_marks(line, sizeof(line), c->req, h->view_ready ? v->id : h->pend_id, count, c->buckets, mask),
             0);
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
        view_refresh(h);
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
        row.signal = (uint16_t)ppc_sig_findings(r->signal);
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
    int layers;      /* layers seen */
    int tcp_layer;   /* -1: none */
    int icmp_layer;
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
        p->layers = it->layer + 1;
        if (strcmp(it->name, "TCP") == 0) {
            p->tcp_layer = it->layer;
        } else if (strncmp(it->name, "ICMP", 4) == 0) {
            p->icmp_layer = it->layer;
        }
        pkt_add(p, line, ppc_evt_format_layer(line, sizeof(line), (uint32_t)it->layer, it->cls, it->name, it->text));
    } else {
        pkt_add(p, line,
                ppc_evt_format_field(line, sizeof(line), (uint32_t)it->layer, it->name, it->text, it->off, it->len,
                                     it->filter));
    }
}

/* The packet a retransmission repeats: the nearest earlier one in the buffer
 * going the same way on the same connection with the same sequence number,
 * no further back than ORIGINAL_LOOKBACK packets. NULL when it has gone. */
static const struct ppc_record *original_of(struct helper *h, const struct ppc_record *r)
{
    uint32_t seq;
    uint32_t looked = 0;

    for (seq = r->seq - 1u; seq != 0 && seq >= h->store.first_seq && looked < ORIGINAL_LOOKBACK; seq--, looked++) {
        const struct ppc_record *o = ppc_store_get(&h->store, seq);

        if (o && o->info == PPC_INFO_TCP && o->a == r->a && o->sport == r->sport && o->dport == r->dport &&
            o->l3 == r->l3 && memcmp(o->src, r->src, 16) == 0 && memcmp(o->dst, r->dst, 16) == 0 &&
            !(o->flags & PPC_F_NONFIRST)) {
            return o;
        }
    }
    return NULL;
}

/* The packet's findings as lines for the inspector (M7), each under the
 * layer it is about. */
static void add_findings(struct helper *h, struct pkt_text *p, const struct ppc_record *r)
{
    char line[PPC_LINE_MAX];
    char text[PPC_FIELD_TEXT];
    int deepest = p->layers > 0 ? p->layers - 1 : 0;
    int tcp = p->tcp_layer >= 0 ? p->tcp_layer : deepest;

    if (r->signal & PPC_SIG_RETRANS) {
        const struct ppc_record *o =
            ppc_sig_diag(r->signal) == PPC_DIAG_TCP_RETRANS_UNSEEN ? NULL : original_of(h, r);

        if (ppc_sig_diag(r->signal) == PPC_DIAG_TCP_RETRANS_UNSEEN) {
            snprintf(text, sizeof(text), "RETRANSMISSION \xc2\xb7 ITS ORIGINAL NEVER CAME BY THIS INTERFACE");
        } else if (o && r->rel_ns >= o->rel_ns) {
            uint64_t us = (r->rel_ns - o->rel_ns) / 1000u;

            snprintf(text, sizeof(text), "RETRANSMISSION OF #%u \xc2\xb7 %llu.%01llu MS EARLIER", (unsigned)o->seq,
                     (unsigned long long)(us / 1000u), (unsigned long long)(us % 1000u / 100u));
        } else {
            snprintf(text, sizeof(text), "RETRANSMISSION \xc2\xb7 THIS SEQUENCE WAS SEEN BEFORE");
        }
        pkt_add(p, line, ppc_evt_format_finding(line, sizeof(line), (uint32_t)tcp, text));
    }
    if (r->signal & PPC_SIG_RST) {
        pkt_add(p, line, ppc_evt_format_finding(line, sizeof(line), (uint32_t)tcp, "CONNECTION RESET"));
    }
    if (r->signal & PPC_SIG_SYN_UNANSWERED) {
        pkt_add(p, line, ppc_evt_format_finding(line, sizeof(line), (uint32_t)tcp, "SYN NOT ANSWERED WITHIN 3 S"));
    }
    if (r->signal & PPC_SIG_DNS_FAIL) {
        const char *rc = ppc_dns_rcode_text(r->a & 0xfu);
        size_t i;

        snprintf(text, sizeof(text), "DNS FAILURE \xc2\xb7 %s", rc ? rc : "ERROR");
        for (i = 0; text[i]; i++) {
            if (text[i] >= 'a' && text[i] <= 'z') {
                text[i] = (char)(text[i] - 'a' + 'A');
            }
        }
        pkt_add(p, line, ppc_evt_format_finding(line, sizeof(line), (uint32_t)deepest, text));
    }
    if (r->signal & PPC_SIG_UNREACH) {
        pkt_add(p, line,
                ppc_evt_format_finding(line, sizeof(line), (uint32_t)(p->icmp_layer >= 0 ? p->icmp_layer : deepest),
                                       "DESTINATION UNREACHABLE"));
    }
    if (r->signal & PPC_SIG_DNS_NOREPLY) {
        pkt_add(p, line,
                ppc_evt_format_finding(line, sizeof(line), (uint32_t)deepest, "DNS QUERY NOT ANSWERED WITHIN 5 S"));
    }
}

/* ---- the diagnosis of one packet (M11.1) ---------------------------------------------- */

/* A duration as the inspector says it: 0.4 MS, 12.3 MS, 6.21 S. */
static const char *dur_text(uint64_t ns, char *b, size_t n)
{
    unsigned long long us = (unsigned long long)(ns / 1000u);

    if (us < 1000000ull) {
        snprintf(b, n, "%llu.%01llu MS", us / 1000u, us % 1000u / 100u);
    } else {
        snprintf(b, n, "%llu.%02llu S", us / 1000000u, us % 1000000u / 10000u);
    }
    return b;
}

static int same_ends(const struct ppc_record *o, const struct ppc_record *r, int swapped)
{
    if (o->l3 != r->l3 || o->l4 != r->l4) {
        return 0;
    }
    if (swapped) {
        return o->sport == r->dport && o->dport == r->sport && memcmp(o->src, r->dst, 16) == 0 &&
               memcmp(o->dst, r->src, 16) == 0;
    }
    return o->sport == r->sport && o->dport == r->dport && memcmp(o->src, r->src, 16) == 0 &&
           memcmp(o->dst, r->dst, 16) == 0;
}

typedef int (*want_fn)(const struct ppc_record *o, const struct ppc_record *r);

/* The nearest packet `want` accepts, walking the buffer from r back (dir
 * -1) or forward (+1), no further than ORIGINAL_LOOKBACK packets. */
static const struct ppc_record *walk(struct helper *h, const struct ppc_record *r, int dir, want_fn want)
{
    uint32_t seq = r->seq;
    uint32_t looked;

    for (looked = 0; looked < ORIGINAL_LOOKBACK; looked++) {
        const struct ppc_record *o;

        if (dir < 0) {
            if (seq <= h->store.first_seq) {
                break;
            }
            seq--;
        } else {
            if (seq >= h->store.last_seq) {
                break;
            }
            seq++;
        }
        o = ppc_store_get(&h->store, seq);
        if (o && !(o->flags & (PPC_F_NONFIRST | PPC_F_MALFORMED)) && want(o, r)) {
            return o;
        }
    }
    return NULL;
}

#define DNS_ID(rec) ((rec)->a >> 16)
#define DNS_IS_ANSWER(rec) (((rec)->a & 0x8000u) != 0)

static int is_dns(const struct ppc_record *o)
{
    return o->info == PPC_INFO_DNS && o->app == PPC_APP_DNS;
}

static int want_dns_query(const struct ppc_record *o, const struct ppc_record *r)
{
    return is_dns(o) && !DNS_IS_ANSWER(o) && DNS_ID(o) == DNS_ID(r) && same_ends(o, r, 1);
}

static int want_dns_answer(const struct ppc_record *o, const struct ppc_record *r)
{
    return is_dns(o) && DNS_IS_ANSWER(o) && DNS_ID(o) == DNS_ID(r) && same_ends(o, r, 1);
}

static int want_dns_copy(const struct ppc_record *o, const struct ppc_record *r)
{
    return is_dns(o) && !DNS_IS_ANSWER(o) && DNS_ID(o) == DNS_ID(r) && same_ends(o, r, 0);
}

static int is_syn(const struct ppc_record *o)
{
    return o->info == PPC_INFO_TCP && (o->tcp_flags & (PPC_TCP_SYN | PPC_TCP_ACK | PPC_TCP_RST)) == PPC_TCP_SYN;
}

/* What answered a SYN: its SYN-ACK, or a reset. */
static int want_syn_answer(const struct ppc_record *o, const struct ppc_record *r)
{
    return o->info == PPC_INFO_TCP && same_ends(o, r, 1) &&
           (((o->tcp_flags & (PPC_TCP_SYN | PPC_TCP_ACK)) == (PPC_TCP_SYN | PPC_TCP_ACK) && o->b == r->a + 1u) ||
            (o->tcp_flags & PPC_TCP_RST));
}

/* The SYN of a SYN-ACK, or of a reset that refused it. */
static int want_syn_back(const struct ppc_record *o, const struct ppc_record *r)
{
    return is_syn(o) && same_ends(o, r, 1) && (!(r->tcp_flags & PPC_TCP_SYN) || o->a + 1u == r->b);
}

/* The SYN of the segment that completed the handshake: the same way. */
static int want_syn_same(const struct ppc_record *o, const struct ppc_record *r)
{
    return is_syn(o) && same_ends(o, r, 0);
}

static int want_syn_copy(const struct ppc_record *o, const struct ppc_record *r)
{
    return is_syn(o) && same_ends(o, r, 0) && o->a == r->a;
}

/* The first copy of a SYN or query: earlier copies, a few at most. */
static const struct ppc_record *first_copy(struct helper *h, const struct ppc_record *r, want_fn want, unsigned *copies)
{
    const struct ppc_record *first = r;
    unsigned i;

    *copies = 1;
    for (i = 0; i < 8; i++) {
        const struct ppc_record *o = walk(h, first, -1, want);

        if (!o) {
            break;
        }
        first = o;
        (*copies)++;
    }
    return first;
}

static void detail(struct pkt_text *p, int layer, const char *text)
{
    char line[PPC_LINE_MAX];

    pkt_add(p, line, ppc_evt_format_detail(line, sizeof(line), (uint32_t)layer, text));
}

static uint64_t rel_now(struct helper *h)
{
    uint64_t wall = h->src_open && h->src.ops->wall_ns ? h->src.ops->wall_ns(&h->src) : clock_ns(CLOCK_REALTIME);

    return ppc_timebase_rel(&h->tb, wall);
}

static void upper(char *s)
{
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') {
            *s = (char)(*s - 'a' + 'A');
        }
    }
}

/* What a response code means, for the one who reads it. */
static const char *rcode_meaning(unsigned rcode)
{
    switch (rcode) {
    case 3:
        return "THE SERVER SAYS THE NAME DOES NOT EXIST: A CONFIRMED ANSWER, NOT A NETWORK FAULT";
    case 2:
        return "THE SERVER COULD NOT ANSWER: ITS UPSTREAM OR ITS DNSSEC CHECK FAILED";
    case 5:
        return "THE SERVER REFUSES TO ANSWER THIS CLIENT";
    default:
        return "THE SERVER COULD NOT HANDLE THE QUERY";
    }
}

static void dns_details(struct helper *h, struct pkt_text *p, const struct ppc_record *r, int layer)
{
    unsigned dg = ppc_sig_diag(r->signal);
    char text[PPC_FIELD_TEXT];
    char t[24];
    const struct ppc_record *o;
    unsigned copies;

    if (dg == PPC_DIAG_DNS_QUERY || dg == PPC_DIAG_DNS_RETRY) {
        if (dg == PPC_DIAG_DNS_RETRY) {
            detail(p, layer, "THE SAME QUERY AGAIN: THE RESOLVER HAD NO ANSWER YET");
        }
        o = walk(h, r, 1, want_dns_answer);
        if (o && o->rel_ns >= r->rel_ns) {
            const char *rc = ppc_dns_rcode_text(o->a & 0xfu);
            char code[16];

            snprintf(code, sizeof(code), "%s", rc ? rc : "ERROR");
            upper(code);
            snprintf(text, sizeof(text), "%s BY #%u AFTER %s \xc2\xb7 %s",
                     ppc_sig_diag(o->signal) == PPC_DIAG_DNS_LATE ? "ANSWERED LATE" : "ANSWERED", (unsigned)o->seq,
                     dur_text(o->rel_ns - r->rel_ns, t, sizeof(t)), code);
            detail(p, layer, text);
        } else if (r->signal & PPC_SIG_DNS_NOREPLY) {
            detail(p, layer, "NO REPLY SEEN: LOST, SENT ON ANOTHER INTERFACE, OR MISSED BY THIS CAPTURE");
            detail(p, layer, "NOT PROOF OF A DNS FAILURE");
        } else if (h->state == PPC_STATE_CAPTURING && rel_now(h) < r->rel_ns + PPC_DNS_WAIT_NS) {
            detail(p, layer, "WAITING FOR ITS ANSWER");
        } else {
            detail(p, layer, "NO ANSWER IN THIS CAPTURE \xc2\xb7 NOT JUDGED: THE CAPTURE STOPPED, HELD OR LOST TRACK");
        }
    } else if (dg == PPC_DIAG_DNS_ANSWER || dg == PPC_DIAG_DNS_LATE) {
        o = walk(h, r, -1, want_dns_query);
        if (o && r->rel_ns >= o->rel_ns) {
            const struct ppc_record *first = first_copy(h, o, want_dns_copy, &copies);
            char more[48] = "";

            if (copies > 1 && r->rel_ns >= first->rel_ns) {
                char t2[24];

                snprintf(more, sizeof(more), " \xc2\xb7 ASKED %u TIMES OVER %s", copies,
                         dur_text(r->rel_ns - first->rel_ns, t2, sizeof(t2)));
            }
            snprintf(text, sizeof(text), "%s TO #%u \xc2\xb7 %s AFTER IT%s",
                     dg == PPC_DIAG_DNS_LATE ? "LATE ANSWER" : "ANSWER", (unsigned)o->seq,
                     dur_text(r->rel_ns - o->rel_ns, t, sizeof(t)), more);
        } else {
            snprintf(text, sizeof(text), "%s TO A QUERY THAT HAS LEFT THE BUFFER",
                     dg == PPC_DIAG_DNS_LATE ? "LATE ANSWER" : "ANSWER");
        }
        detail(p, layer, text);
    } else if (dg == PPC_DIAG_DNS_ORPHAN) {
        detail(p, layer, "ITS QUERY IS NOT IN THIS CAPTURE: SENT BEFORE START, WHILE HELD, OR ON ANOTHER INTERFACE");
    }
    if (r->signal & PPC_SIG_DNS_FAIL) {
        detail(p, layer, rcode_meaning(r->a & 0xfu));
    }
}

static void tcp_details(struct helper *h, struct pkt_text *p, const struct ppc_record *r, int layer)
{
    char text[PPC_FIELD_TEXT];
    char t[24];
    const struct ppc_record *o;
    unsigned copies;

    switch (ppc_sig_diag(r->signal)) {
    case PPC_DIAG_TCP_SYN:
        o = walk(h, r, 1, want_syn_answer);
        if (o && o->rel_ns >= r->rel_ns) {
            snprintf(text, sizeof(text), "%s #%u AFTER %s", (o->tcp_flags & PPC_TCP_RST) ? "REFUSED BY" : "ANSWERED BY",
                     (unsigned)o->seq, dur_text(o->rel_ns - r->rel_ns, t, sizeof(t)));
            detail(p, layer, text);
        } else if (r->signal & PPC_SIG_SYN_UNANSWERED) {
            detail(p, layer, "NOTHING CAME BACK: HOST DOWN, SYN DROPPED BY A FIREWALL, OR THE ANSWER TOOK ANOTHER PATH");
        } else {
            detail(p, layer, "A CONNECTION ATTEMPT \xc2\xb7 NO ANSWER IN THIS CAPTURE YET");
        }
        break;
    case PPC_DIAG_TCP_SYN_AGAIN:
        detail(p, layer, "SENT AGAIN: THE FIRST GOT NO ANSWER IN TIME");
        break;
    case PPC_DIAG_TCP_SYNACK:
        o = walk(h, r, -1, want_syn_back);
        if (o && r->rel_ns >= o->rel_ns) {
            snprintf(text, sizeof(text), "ANSWERS SYN #%u \xc2\xb7 %s AFTER IT", (unsigned)o->seq,
                     dur_text(r->rel_ns - o->rel_ns, t, sizeof(t)));
            detail(p, layer, text);
        } else {
            detail(p, layer, "ANSWERS A SYN THAT HAS LEFT THE BUFFER");
        }
        break;
    case PPC_DIAG_TCP_CONNECTED:
        o = walk(h, r, -1, want_syn_same);
        if (o) {
            const struct ppc_record *first = first_copy(h, o, want_syn_copy, &copies);

            if (r->rel_ns >= first->rel_ns) {
                snprintf(text, sizeof(text), "HANDSHAKE COMPLETE \xc2\xb7 CONNECTED %s AFTER SYN #%u%s",
                         dur_text(r->rel_ns - first->rel_ns, t, sizeof(t)), (unsigned)first->seq,
                         copies > 1 ? " (SENT AGAIN)" : "");
                detail(p, layer, text);
                break;
            }
        }
        detail(p, layer, "HANDSHAKE COMPLETE");
        break;
    case PPC_DIAG_TCP_MIDSTREAM:
        detail(p, layer, "FIRST SEEN WITHOUT ITS HANDSHAKE: IT BEGAN BEFORE THE CAPTURE OR ITS START PASSED UNSEEN");
        break;
    case PPC_DIAG_TCP_REFUSED:
        o = walk(h, r, -1, want_syn_back);
        if (o && r->rel_ns >= o->rel_ns) {
            snprintf(text, sizeof(text), "CONNECTION REFUSED \xc2\xb7 THE ANSWER TO SYN #%u, %s AFTER IT", (unsigned)o->seq,
                     dur_text(r->rel_ns - o->rel_ns, t, sizeof(t)));
            detail(p, layer, text);
        } else {
            detail(p, layer, "CONNECTION REFUSED \xc2\xb7 THE ANSWER TO A SYN");
        }
        detail(p, layer, "THE HOST IS REACHABLE: NOTHING LISTENS ON THE PORT, OR A FIREWALL REJECTS IT");
        detail(p, layer, "NOT A NETWORK FAULT");
        break;
    case PPC_DIAG_TCP_ABORTED:
        detail(p, layer, "RESET OF A CONNECTION THAT WAS OPEN OR OPENING: ONE END ABORTED IT");
        break;
    case PPC_DIAG_TCP_RST_CLOSED:
        detail(p, layer, "RESET AFTER THE CONNECTION WAS CLOSING OR RESET \xc2\xb7 USUALLY NORMAL");
        break;
    case PPC_DIAG_TCP_RST_UNSEEN:
        detail(p, layer, "RESET OF A CONNECTION THIS CAPTURE NEVER SAW");
        break;
    case PPC_DIAG_TCP_RETRANS:
        detail(p, layer, "ITS ORIGINAL PASSED HERE: LOST FURTHER ON, OR ITS ACKNOWLEDGEMENT WAS");
        break;
    case PPC_DIAG_TCP_RETRANS_ACKED:
        detail(p, layer, "THE RECEIVER HAD ALREADY ACKNOWLEDGED THESE BYTES: A SPURIOUS RETRANSMISSION");
        break;
    case PPC_DIAG_TCP_RETRANS_UNSEEN:
        detail(p, layer, "IT FILLS A GAP MORE THAN A ROUND TRIP LATE: LOST BEFORE IT REACHED THIS INTERFACE");
        break;
    case PPC_DIAG_TCP_OUT_OF_ORDER:
        detail(p, layer, "OUT OF ORDER: ARRIVED AFTER LATER DATA, WITHIN A ROUND TRIP");
        detail(p, layer, "REORDERING, NOT LOSS \xc2\xb7 NOT COUNTED AS A RETRANSMISSION");
        break;
    default:
        break;
    }
}

/* What the DNS and TCP tables made of the packet, under its layer. */
static void add_details(struct helper *h, struct pkt_text *p, const struct ppc_record *r)
{
    int deepest = p->layers > 0 ? p->layers - 1 : 0;

    if (!h->store_ready) {
        return;
    }
    if (r->info == PPC_INFO_DNS) {
        dns_details(h, p, r, deepest);
    } else if (r->info == PPC_INFO_TCP) {
        tcp_details(h, p, r, p->tcp_layer >= 0 ? p->tcp_layer : deepest);
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
    view_refresh(h);
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
    head.signal = (uint16_t)ppc_sig_findings(r->signal);
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
    p.layers = 0;
    p.tcp_layer = -1;
    p.icmp_layer = -1;
    pkt_add(&p, line, ppc_evt_format_pkt_begin(line, sizeof(line), &head));
    pkt_add(&p, line, ppc_evt_format_pkt_ends(line, sizeof(line), &src, &dst));
    ppc_dissect(r, bytes, n, on_item, &p);
    if (ppc_sig_findings(r->signal)) {
        add_findings(h, &p, r);
    }
    add_details(h, &p, r);
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

/* ---- SAVE (M8; ADR-016 rules 9 and 16) ----------------------------------------------- */

static void say_saved(struct helper *h, enum ppc_save_phase phase, const char *name, int snapshot)
{
    char line[PPC_LINE_MAX];

    out_line(h, line,
             ppc_evt_format_saved(line, sizeof(line), phase, h->save_done, h->save_total, h->save_bytes, name),
             snapshot);
}

/* Give up the file being written: nothing half-written is left behind. */
static void save_abort(struct helper *h, const char *code)
{
    if (h->save_fd >= 0) {
        close(h->save_fd);
        h->save_fd = -1;
        unlinkat(h->cap_dirfd, h->save_part, 0);
    }
    if (code) {
        LOG_WARN("capture: save failed (%s)", code);
        h->save_done = 0;
        h->save_bytes = 0;
        say_saved(h, PPC_SAVE_FAIL, code, 0);
    }
    if (h->state == PPC_STATE_SAVING) {
        h->state = PPC_STATE_STOPPED;
        say_state(h);
    }
}

/* A name no file has: the UTC time when the clock is set, else a number.
 * Names only: the folder is the descriptor opened before the drop. */
static int save_pick_name(struct helper *h)
{
    char stem[32];
    struct stat sb;
    int i;

    if (clock_set_now()) {
        time_t now = time(NULL);
        struct tm tm;

        if (!gmtime_r(&now, &tm) || strftime(stem, sizeof(stem), "capture-%Y%m%d-%H%M%S", &tm) == 0) {
            return -1;
        }
    } else {
        snprintf(stem, sizeof(stem), "capture");
    }
    for (i = clock_set_now() ? 0 : 1; i < 10000; i++) {
        if (i == 0) {
            snprintf(h->save_name, sizeof(h->save_name), "%s.pcap", stem);
        } else if (clock_set_now()) {
            snprintf(h->save_name, sizeof(h->save_name), "%s-%d.pcap", stem, i + 1);
        } else {
            snprintf(h->save_name, sizeof(h->save_name), "%s-%04d.pcap", stem, i);
        }
        if (snprintf(h->save_path, sizeof(h->save_path), "%s", h->save_name) >= (int)sizeof(h->save_path) ||
            snprintf(h->save_part, sizeof(h->save_part), ".%s.part", h->save_name) >= (int)sizeof(h->save_part)) {
            return -1;
        }
        if (fstatat(h->cap_dirfd, h->save_path, &sb, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT) {
            return ppc_save_name_ok(h->save_name) ? 0 : -1;
        }
    }
    return -1;
}

/* `save`: the stopped capture's packets, as they are in the buffer, to one
 * new .pcap in the captures folder: 0700 folder, 0600 file, written under
 * another name and renamed when whole, never over a file that is there. */
static void cmd_save(struct helper *h)
{
    struct statvfs vfs;
    uint64_t need = PPC_PCAP_HEADER_LEN;
    uint32_t seq;

    if (h->state != PPC_STATE_STOPPED || !h->store_ready || h->save_fd >= 0) {
        say_error(h, "state", "only a stopped capture can be saved");
        return;
    }
    h->save_done = 0;
    h->save_total = 0;
    h->save_bytes = 0;
    h->save_name[0] = '\0';
    for (seq = h->store.first_seq; seq != 0 && seq <= h->store.last_seq; seq++) {
        const struct ppc_record *r = ppc_store_get(&h->store, seq);

        if (r && r->cap_len > 0 && ppc_store_has_bytes(&h->store, r)) {
            h->save_total++;
            need += PPC_PCAP_RECORD_HEADER_LEN + r->cap_len;
        }
    }
    if (h->save_total == 0) {
        say_saved(h, PPC_SAVE_FAIL, "empty", 0);
        return;
    }
    if (h->cap_dirfd < 0) {
        LOG_WARN("capture: the captures folder cannot be used");
        say_saved(h, PPC_SAVE_FAIL, "nodir", 0);
        return;
    }
    if (fstatvfs(h->cap_dirfd, &vfs) == 0 && (uint64_t)vfs.f_bavail * vfs.f_frsize < need + SAVE_MARGIN) {
        say_saved(h, PPC_SAVE_FAIL, "nospace", 0);
        return;
    }
    if (save_pick_name(h) != 0) {
        say_saved(h, PPC_SAVE_FAIL, "name", 0);
        return;
    }
    h->save_fd = openat(h->cap_dirfd, h->save_part, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (h->save_fd < 0 || ppc_pcap_write_header(h->save_fd, SAVE_SNAPLEN) != 0) {
        save_abort(h, "io");
        return;
    }
    h->save_bytes = PPC_PCAP_HEADER_LEN;
    h->save_next = h->store.first_seq;
    h->save_last = h->store.last_seq;
    h->state = PPC_STATE_SAVING;
    LOG_INFO("capture: saving %u packets", (unsigned)h->save_total);
    say_state(h);
    say_saved(h, PPC_SAVE_RUN, h->save_name, 0);
}

/* One batch of the file. The loop calls this each turn while a save runs. */
static void save_step(struct helper *h)
{
    static uint8_t bytes[65536];
    uint32_t n = 0;

    while (h->save_next != 0 && h->save_next <= h->save_last && n < SAVE_BATCH) {
        const struct ppc_record *r = ppc_store_get(&h->store, h->save_next++);
        uint32_t len;

        n++;
        if (!r || r->cap_len == 0) {
            continue;
        }
        len = ppc_store_copy(&h->store, r, bytes, sizeof(bytes));
        if (len == 0) {
            continue;
        }
        if (ppc_pcap_write_record(h->save_fd, ppc_timebase_wall(&h->tb, r->rel_ns), bytes, len, r->wire_len) != 0) {
            save_abort(h, errno == ENOSPC ? "nospace" : "io");
            return;
        }
        h->save_done++;
        h->save_bytes += PPC_PCAP_RECORD_HEADER_LEN + len;
    }
    if (h->save_next != 0 && h->save_next <= h->save_last) {
        say_saved(h, PPC_SAVE_RUN, h->save_name, 1);
        return;
    }
    if (fsync(h->save_fd) != 0 || close(h->save_fd) != 0) {
        h->save_fd = -1;
        unlinkat(h->cap_dirfd, h->save_part, 0);
        save_abort(h, "io");
        return;
    }
    h->save_fd = -1;
    if (renameat(h->cap_dirfd, h->save_part, h->cap_dirfd, h->save_path) != 0) {
        unlinkat(h->cap_dirfd, h->save_part, 0);
        save_abort(h, "io");
        return;
    }
    h->state = PPC_STATE_STOPPED;
    {
        unsigned long long size = h->save_bytes;

        LOG_INFO("capture: saved %u packets in a file of %llu B", (unsigned)h->save_done, size);
    }
    say_state(h);
    say_saved(h, PPC_SAVE_DONE, h->save_name, 0);
}

/* ---- the network intelligence's answer (M12.1) ------------------------------------------ */

static const uint8_t no_addr[6];

/* An address as text, or "" when there is none (all zeros). */
static const char *ip_text(const uint8_t *ip, char *b, size_t n)
{
    if (memcmp(ip, no_addr, 4) == 0) {
        b[0] = '\0';
    } else {
        snprintf(b, n, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    return b;
}

static const char *mac_text(const uint8_t *m, char *b, size_t n)
{
    if (memcmp(m, no_addr, 6) == 0) {
        b[0] = '\0';
    } else {
        snprintf(b, n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    return b;
}

/* The devices, servers, leases and events as they stand, after the totals:
 * built first and queued whole, like the inspector's packet. An answer that
 * does not fit now is not sent; the app asks again within a second. */
static void cmd_net(struct helper *h, const struct ppc_cmd *c)
{
    static char buf[8 * 1024];
    const struct ppc_net_host *hosts[PPC_NET_WIRE_HOSTS];
    const struct ppc_net_server *servers[PPC_NET_WIRE_SERVERS];
    const struct ppc_net_lease *leases[PPC_NET_WIRE_LEASES];
    const struct ppc_net_event *events[PPC_NET_WIRE_EVENTS];
    char line[PPC_LINE_MAX];
    size_t len = 0;
    unsigned n;
    unsigned i;
    int k;

    if (!h->net_ready) {
        say_error(h, "state", "nothing has been captured");
        return;
    }
#define NET_ADD(expr)                                                                                                  \
    do {                                                                                                               \
        k = (expr);                                                                                                    \
        if (k > 0 && len + (size_t)k <= sizeof(buf)) {                                                                 \
            memcpy(buf + len, line, (size_t)k);                                                                        \
            len += (size_t)k;                                                                                          \
        }                                                                                                              \
    } while (0)
    NET_ADD(ppc_evt_format_net_begin(line, sizeof(line), c->req));
    n = ppc_net_top(&h->net, hosts, PPC_NET_WIRE_HOSTS);
    for (i = 0; i < n; i++) {
        struct ppc_evt_net_host e;

        memset(&e, 0, sizeof(e));
        mac_text(hosts[i]->mac, e.mac, sizeof(e.mac));
        if (hosts[i]->flags & PPC_NH_IP) {
            ip_text(hosts[i]->ip, e.ip, sizeof(e.ip));
        }
        e.flags = hosts[i]->flags;
        e.pkts = hosts[i]->pkts;
        e.bytes = hosts[i]->bytes;
        snprintf(e.name, sizeof(e.name), "%s", hosts[i]->name);
        NET_ADD(ppc_evt_format_net_host(line, sizeof(line), &e));
    }
    n = ppc_net_servers(&h->net, servers, PPC_NET_WIRE_SERVERS);
    for (i = 0; i < n; i++) {
        struct ppc_evt_net_srv e;

        memset(&e, 0, sizeof(e));
        ip_text(servers[i]->ip, e.ip, sizeof(e.ip));
        mac_text(servers[i]->mac, e.mac, sizeof(e.mac));
        e.relayed = servers[i]->relayed;
        e.offers = servers[i]->offers;
        e.acks = servers[i]->acks;
        e.naks = servers[i]->naks;
        e.seq = servers[i]->seq;
        e.lease_s = servers[i]->lease_s;
        ip_text(servers[i]->router, e.router, sizeof(e.router));
        ip_text(servers[i]->dns, e.dns, sizeof(e.dns));
        ip_text(servers[i]->mask, e.mask, sizeof(e.mask));
        NET_ADD(ppc_evt_format_net_srv(line, sizeof(line), &e));
    }
    n = ppc_net_leases(&h->net, leases, PPC_NET_WIRE_LEASES);
    for (i = 0; i < n; i++) {
        struct ppc_evt_net_lease e;

        memset(&e, 0, sizeof(e));
        ip_text(leases[i]->ip, e.ip, sizeof(e.ip));
        mac_text(leases[i]->mac, e.mac, sizeof(e.mac));
        ip_text(leases[i]->server, e.server, sizeof(e.server));
        e.lease_s = leases[i]->lease_s;
        e.rel_us = leases[i]->ns / 1000u;
        e.seq = leases[i]->seq;
        snprintf(e.name, sizeof(e.name), "%s", leases[i]->name);
        NET_ADD(ppc_evt_format_net_lease(line, sizeof(line), &e));
    }
    n = ppc_net_events(&h->net, events, PPC_NET_WIRE_EVENTS);
    for (i = 0; i < n; i++) {
        struct ppc_evt_net_event e;

        memset(&e, 0, sizeof(e));
        e.kind = events[i]->kind;
        e.sev = events[i]->sev;
        e.count = events[i]->count;
        e.seq = events[i]->seq;
        e.first_us = events[i]->first_ns / 1000u;
        e.last_us = events[i]->last_ns / 1000u;
        ip_text(events[i]->ip, e.ip, sizeof(e.ip));
        mac_text(events[i]->mac, e.mac, sizeof(e.mac));
        ip_text(events[i]->ip2, e.ip2, sizeof(e.ip2));
        mac_text(events[i]->mac2, e.mac2, sizeof(e.mac2));
        NET_ADD(ppc_evt_format_net_evt(line, sizeof(line), &e));
    }
    NET_ADD(ppc_evt_format_net_end(line, sizeof(line), c->req));
#undef NET_ADD
    if (sizeof(h->out) - h->out_len < len + PPC_LINE_MAX) {
        return;
    }
    say_net(h, 0);
    memcpy(h->out + h->out_len, buf, len);
    h->out_len += len;
    out_flush(h);
}

/* MARK (N1.0): this moment, on the session's clock. Answered at once, and
 * queued like a refusal is: never dropped as a snapshot would be. A mark on
 * a stopped capture stands where it stopped, on both clocks. The counters
 * of lost frames are read at the mark, so that what was lost before it and
 * what after are told apart (ppc_evid_mark). */
static void cmd_mark(struct helper *h)
{
    char line[PPC_LINE_MAX];
    const struct ppc_ev_mark *m;
    struct ppc_evt_mark e;
    uint32_t first;
    uint32_t last;

    if (!h->ev_ready || h->state == PPC_STATE_IDLE) {
        say_error(h, "state", "nothing has been captured");
        return;
    }
    if (h->src_open && (h->state == PPC_STATE_CAPTURING || h->state == PPC_STATE_HOLD)) {
        read_drops(h);
    }
    m = ppc_evid_mark(&h->ev, ev_tick(h), h->state, h->store_ready ? h->store.last_seq : 0, wall_s_now());
    if (!m) {
        say_error(h, "state", "nothing has been captured");
        return;
    }
    ppc_evid_ids(&h->ev, &first, &last);
    memset(&e, 0, sizeof(e));
    e.id = m->id;
    e.first = first;
    e.at_ms = m->at_ms;
    e.state = m->state;
    e.seq = m->seq;
    e.wall_s = m->wall_s;
    out_line(h, line, ppc_evt_format_mark(line, sizeof(line), &e), 0);
    LOG_INFO("capture: mark %u at %u ms", (unsigned)m->id, (unsigned)m->at_ms);
}

/* What the buffer still holds of a packet: all of it, its row without its
 * bytes, or nothing. */
static unsigned ev_alive(struct helper *h, uint32_t seq)
{
    const struct ppc_pin *pin = NULL;
    const struct ppc_record *r;
    uint8_t one[1];

    if (!h->store_ready) {
        return PPC_EV_GONE;
    }
    r = ppc_view_record(&h->store, seq, &pin);
    if (!r) {
        return PPC_EV_GONE;
    }
    if (pin || r->cap_len == 0 || ppc_store_copy(&h->store, r, one, sizeof(one)) > 0) {
        return PPC_EV_WHOLE;
    }
    return PPC_EV_SUMMARY;
}

/* A mark's evidence as it stands: built first and queued whole, like the
 * network intelligence's lists. An answer that does not fit now is not
 * sent; the app asks again within a second. */
static void cmd_evid(struct helper *h, const struct ppc_cmd *c)
{
    static char buf[8 * 1024];
    char line[PPC_LINE_MAX];
    const struct ppc_ev_mark *m;
    struct ppc_evt_ev_head hd;
    uint32_t first;
    uint32_t last;
    size_t len = 0;
    unsigned w;
    unsigned i;
    int k;

    if (!h->ev_ready) {
        say_error(h, "state", "nothing has been captured");
        return;
    }
    memset(&hd, 0, sizeof(hd));
    /* Read now, so that an open window's losses are told up to now, or are
     * said not to be known. */
    if (h->src_open && (h->state == PPC_STATE_CAPTURING || h->state == PPC_STATE_HOLD)) {
        read_drops(h);
    }
    hd.now_ms = ev_tick(h); /* a window still filling is told up to now */
    m = ppc_evid_get(&h->ev, c->mark_id);
    if (!m) {
        say_error(h, "nomark", "that mark is not held");
        return;
    }
    ppc_evid_ids(&h->ev, &first, &last);
    hd.req = c->req;
    hd.id = m->id;
    hd.first = first;
    hd.last = last;
    hd.at_ms = m->at_ms;
    hd.state = m->state;
    hd.seq = m->seq;
    hd.wall_s = m->wall_s;
    hd.cfilter = h->ev.cfilter;
    hd.unanalyzed = h->ev.unanalyzed;
    hd.edge = m->edge;
    hd.edge_from_ms = m->edge_from_ms;
    hd.edge_to_ms = m->edge_to_ms;
    hd.edge_lost = m->edge_lost;
#define EV_ADD(expr)                                                                                                   \
    do {                                                                                                               \
        k = (expr);                                                                                                    \
        if (k > 0 && len + (size_t)k <= sizeof(buf)) {                                                                 \
            memcpy(buf + len, line, (size_t)k);                                                                        \
            len += (size_t)k;                                                                                          \
        }                                                                                                              \
    } while (0)
    EV_ADD(ppc_evt_format_ev_begin(line, sizeof(line), &hd));
    for (w = 0; w < 2; w++) {
        const struct ppc_ev_side *s = &m->side[w];
        struct ppc_evt_ev_win e;

        memset(&e, 0, sizeof(e));
        e.side = (uint8_t)w;
        e.flags = s->flags;
        e.span_ms = s->span_ms;
        e.watched_s = s->watched_s;
        e.held_s = s->held_s;
        e.shed_s = s->shed_s;
        e.lost = s->lost;
        e.kept = s->nkept;
        e.more = s->more;
        memcpy(e.count, s->count, sizeof(e.count));
        e.lost_near = s->lost_near;
        e.unknown = s->unknown;
        e.unknown_from_ms = s->unknown_from_ms;
        /* The window before, of a mark set while no reading could be made:
         * unknown from the last good one (its window after says it too). */
        if (w == 0 && m->edge == PPC_EV_EDGE_UNKNOWN) {
            e.unknown = 1;
            e.unknown_from_ms = m->edge_from_ms;
        }
        EV_ADD(ppc_evt_format_ev_win(line, sizeof(line), &e));
        for (i = 0; i < s->nkept; i++) {
            const struct ppc_ev_obs *o = &s->kept[i];
            struct ppc_evt_ev_obs r;

            memset(&r, 0, sizeof(r));
            r.side = (uint8_t)w;
            r.kind = o->kind;
            r.flags = o->flags;
            r.alive = (uint8_t)ev_alive(h, o->seq);
            r.t_ms = o->t_ms;
            r.seq = o->seq;
            mac_text(o->mac, r.mac, sizeof(r.mac));
            ip_text(o->ip, r.ip, sizeof(r.ip));
            ip_text(o->ip2, r.ip2, sizeof(r.ip2));
            r.a = o->a;
            r.b = o->b;
            EV_ADD(ppc_evt_format_ev_obs(line, sizeof(line), &r));
        }
    }
    EV_ADD(ppc_evt_format_ev_end(line, sizeof(line), c->req));
#undef EV_ADD
    if (sizeof(h->out) - h->out_len < len + PPC_LINE_MAX) {
        return;
    }
    memcpy(h->out + h->out_len, buf, len);
    h->out_len += len;
    out_flush(h);
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
        say_diag(h, 0);
        say_net(h, 0);
        return 0;
    case PPC_CMD_NET:
        cmd_net(h, &c);
        return 0;
    case PPC_CMD_MARK:
        cmd_mark(h);
        return 0;
    case PPC_CMD_EVID:
        cmd_evid(h, &c);
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
    case PPC_CMD_MARKS:
        cmd_marks(h, &c);
        return 0;
    case PPC_CMD_SAVE:
        cmd_save(h);
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
        if (now >= h->next_ev_ms) {
            h->next_ev_ms = now + STATS_EVERY_MS;
            ev_tick(h);
        }
        if (h->limit_s && el >= (uint64_t)h->limit_s * 1000u) {
            stop_capture(h, PPC_REASON_LIMIT, NULL);
        }
    }
    if (h->sig_ready && ppc_signals_pending(&h->sig) && h->tb.started && now >= h->next_signal_ms &&
        h->state == PPC_STATE_CAPTURING && !h->shed) {
        uint64_t wall = h->src_open && h->src.ops->wall_ns ? h->src.ops->wall_ns(&h->src) : clock_ns(CLOCK_REALTIME);

        h->next_signal_ms = now + SIGNAL_EVERY_MS;
        ppc_signals_expire(&h->sig, ppc_timebase_rel(&h->tb, wall), on_late_signal, h);
    }
    /* The network intelligence's late decisions, by the same rule: only
     * while every frame is seen. Twice a second while something waits. */
    if (h->net_ready && ppc_net_pending(&h->net) && h->tb.started && now >= h->next_net_ms &&
        h->state == PPC_STATE_CAPTURING && !h->shed) {
        uint64_t wall = h->src_open && h->src.ops->wall_ns ? h->src.ops->wall_ns(&h->src) : clock_ns(CLOCK_REALTIME);

        h->next_net_ms = now + SIGNAL_EVERY_MS;
        ppc_net_expire(&h->net, ppc_timebase_rel(&h->tb, wall));
    }
    if ((h->sig_ready || h->net_ready) && (h->state == PPC_STATE_CAPTURING || h->state == PPC_STATE_HOLD) &&
        now >= h->next_diag_ms && say_figures(h) == 0) {
        h->next_diag_ms = now + DIAG_EVERY_MS;
    }
    if (h->src_open && h->src.live && now >= h->next_link_ms) {
        h->next_link_ms = now + LINK_EVERY_MS;
        if (carrier(h->iface) != 1) {
            stop_capture(h, PPC_REASON_LINK, "carrier lost");
        }
    }
    if (now >= h->next_stats_ms) {
        h->next_stats_ms = now + STATS_EVERY_MS;
        if (h->src_open) {
            read_drops(h);
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
    h.save_fd = -1;
    h.cap_dirfd = -1;
    h.parent = getppid();
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

        if (h.save_fd >= 0) {
            wait = 0; /* a save in hand: its next batch, between the app's lines */
        } else if (h.state == PPC_STATE_IDLE || h.state == PPC_STATE_STOPPED) {
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
        if (h.save_fd >= 0 && !leave) {
            save_step(&h);
        }
        tick(&h, mono_ms());
    }
    save_abort(&h, NULL); /* a save cut short leaves no part file */
    if (h.src_open) {
        ppc_source_close(&h.src);
        h.src_open = 0;
    }
    release_lock(&h);
    if (h.view_ready) {
        ppc_view_free(&h.view);
        h.view_ready = 0;
    }
    if (h.sig_ready) {
        ppc_signals_free(&h.sig);
        h.sig_ready = 0;
    }
    if (h.net_ready) {
        ppc_net_free(&h.net);
        h.net_ready = 0;
    }
    if (h.ev_ready) {
        ppc_evid_free(&h.ev);
        h.ev_ready = 0;
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

static void bench_item(void *ctx, const struct ppc_ds_item *it)
{
    (*(uint64_t *)ctx) += (uint64_t)it->text[0];
}

/* The list's view and the inspector's dissector on what the store holds
 * (M5, M6): what a filter change, a length order and an inspected packet
 * cost, the helper's work behind a window or an inspect. */
static void bench_view(struct ppc_store *store)
{
    static uint8_t b[65536];
    struct ppc_view v;
    struct ppc_filter f;
    uint64_t t0;
    uint64_t sink = 0;
    double ms_all;
    double ms_tcp;
    double ms_len;
    double us_dissect;
    uint32_t i;
    uint32_t n;

    if (ppc_view_init(&v, PPC_STORE_SUMMARIES) != 0) {
        printf("bench view: no memory\n");
        return;
    }
    memset(&f, 0, sizeof(f));
    f.classes = PPC_CLASSES_ALL;
    t0 = clock_ns(CLOCK_MONOTONIC);
    ppc_view_set(&v, store, 1, PPC_SORT_TIME, &f);
    ms_all = (double)(clock_ns(CLOCK_MONOTONIC) - t0) / 1e6;
    f.classes = 1u << PPC_CLASS_TCP;
    t0 = clock_ns(CLOCK_MONOTONIC);
    ppc_view_set(&v, store, 2, PPC_SORT_LEN, &f);
    ms_tcp = (double)(clock_ns(CLOCK_MONOTONIC) - t0) / 1e6;
    t0 = clock_ns(CLOCK_MONOTONIC);
    ppc_view_prepare(&v, store, 0);
    ms_len = (double)(clock_ns(CLOCK_MONOTONIC) - t0) / 1e6;
    t0 = clock_ns(CLOCK_MONOTONIC);
    for (i = 0; i < 1000; i++) {
        const struct ppc_record *r = ppc_store_get(store, store->last_seq - i);

        n = r ? ppc_store_copy(store, r, b, sizeof(b)) : 0;
        if (r) {
            ppc_dissect(r, b, n, bench_item, &sink);
        }
    }
    us_dissect = (double)(clock_ns(CLOCK_MONOTONIC) - t0) / 1000.0 / 1000.0;
    printf("bench view_all_ms=%.2f view_tcp_ms=%.2f (%u of %u) len_order_ms=%.2f dissect_us=%.1f sink=%llu\n",
           ms_all, ms_tcp, ppc_view_count(&v), ppc_store_count(store), ms_len, us_dissect,
           (unsigned long long)(sink & 1u));
    ppc_view_free(&v);
}

struct bench_evid {
    struct ppc_evid ev;
};

static void bench_said(void *ctx, const struct ppc_net_said *s)
{
    struct bench_evid *b = ctx;
    struct ppc_ev_obs o;

    if (ppc_evid_from_net(s, 0, NULL, &o)) {
        ppc_evid_note(&b->ev, &o);
    }
}

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
    double ns_net = 0;
    double ns_evid = 0;
    struct ppc_net net;
    struct bench_evid be;
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

    /* The network intelligence on every frame (M12.1), beside the decoder
     * it follows: its cost is the difference to the first figure. */
    if (ppc_net_init(&net) == 0) {
        t0 = clock_ns(CLOCK_MONOTONIC);
        for (i = 0; i < frames; i++) {
            uint32_t pay;

            f.data = tpl[i % BENCH_TEMPLATES];
            f.cap_len = f.wire_len = tpl_len[i % BENCH_TEMPLATES];
            pay = ppc_decode(&f, &r);
            r.rel_ns = i * 500000u;
            ppc_net_frame(&net, &r, f.data, f.cap_len, pay, (uint32_t)i + 1u);
            if ((i & 1023u) == 0) {
                ppc_net_expire(&net, r.rel_ns);
            }
        }
        ns_net = (double)(clock_ns(CLOCK_MONOTONIC) - t0) / (double)frames;
        sink += net.st.hosts;
        ppc_net_free(&net);
    }
    /* The same with the evidence history listening (N1.0), a mark set early
     * so one window is always filling: its cost is the difference to the
     * figure above. */
    memset(&be, 0, sizeof(be));
    if (ppc_net_init(&net) == 0 && ppc_evid_init(&be.ev) == 0) {
        ppc_net_listen(&net, bench_said, &be);
        t0 = clock_ns(CLOCK_MONOTONIC);
        for (i = 0; i < frames; i++) {
            uint32_t pay;

            f.data = tpl[i % BENCH_TEMPLATES];
            f.cap_len = f.wire_len = tpl_len[i % BENCH_TEMPLATES];
            pay = ppc_decode(&f, &r);
            r.rel_ns = i * 500000u;
            ppc_net_frame(&net, &r, f.data, f.cap_len, pay, (uint32_t)i + 1u);
            if ((i & 1023u) == 0) {
                ppc_net_expire(&net, r.rel_ns);
                ppc_evid_tick(&be.ev, (uint32_t)(r.rel_ns / 1000000u), PPC_EVS_WATCHED);
                if (i == 1024u || (i & 0xfffffu) == 0) {
                    ppc_evid_mark(&be.ev, (uint32_t)(r.rel_ns / 1000000u), PPC_STATE_CAPTURING, (uint32_t)i + 1u, 0);
                }
            }
        }
        ns_evid = (double)(clock_ns(CLOCK_MONOTONIC) - t0) / (double)frames;
        sink += be.ev.total;
        ppc_evid_free(&be.ev);
        ppc_net_free(&net);
    }

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
    printf("bench decode_net_ns_per_frame=%.0f\n", ns_net);
    printf("bench net_tables_bytes=%u\n",
           (unsigned)(PPC_NET_HOSTS * sizeof(struct ppc_net_host) + PPC_NET_BINDS * sizeof(struct ppc_net_bind) +
                      PPC_NET_DHCP * sizeof(struct ppc_net_dhcp) + sizeof(struct ppc_net)));
    printf("bench decode_net_evid_ns_per_frame=%.0f\n", ns_evid);
    printf("bench evid_bytes=%u\n", (unsigned)(ppc_evid_bytes() + sizeof(struct ppc_evid)));
    printf("bench store_fill_permille=%u held=%u sink=%llu\n", ppc_store_fill_permille(&store),
           ppc_store_count(&store), (unsigned long long)(sink & 1u));
    bench_view(&store);
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

/* Every descriptor above stderr, however high: close_range where the
 * kernel has it (5.9), else every number up to the descriptor limit. */
static void close_inherited(void)
{
    long max;
    long fd;

#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0) {
        return;
    }
#endif
    max = sysconf(_SC_OPEN_MAX);
    if (max < 1024 || max > 1048576) {
        max = max < 1024 ? 1024 : 1048576;
    }
    for (fd = 3; fd < max; fd++) {
        close((int)fd);
    }
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
        /* Nothing inherited reaches a capture: the session keeps stdin,
         * stdout and stderr (the shell's socketpair and /dev/null) and
         * closes every other descriptor before it opens its own. */
        close_inherited();
        pocketlog_init("pos-capture");
        return run_session(spec);
    }
    return usage();
}
