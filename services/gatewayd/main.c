/*
 * gatewayd: the authenticated Companion Gateway (docs/decisions/ADR-019,
 * accepted 2026-11-21; docs/api/gateway.md; docs/services/GATEWAYD.md).
 *
 * One thread, one loop. Two surfaces:
 *
 *   gatewayd.sock   pocketipc, root only: the unit's own control of the
 *                   gateway (open a pairing window, decide about a
 *                   candidate, list, re-scope and revoke clients). The
 *                   physical approval of ADR-019 rule 22 arrives here: the
 *                   Settings page is its client, and until that page exists
 *                   `doors call` on a root console is.
 *   TCP 47231       TLS 1.3, both sides pinned (gw_tls.c). Paired clients
 *                   call the allowlist of gw_policy.c and nothing else.
 *
 * Closed until asked (rule 5): no TCP socket exists unless a client is
 * paired or a pairing window is open, the unit has a device password
 * (rule 9b), and the identity and the paired list are intact. Each of those
 * is re-evaluated as it changes, and losing any of them closes the listener
 * and every session.
 *
 * Milestone M2: the session group and device status, by synchronous upstream
 * calls with a deadline (GW_UPSTREAM_CALL_MS).
 *
 * Milestone M4.1: reading the mesh. While a paired client holds mesh_read
 * the gateway keeps one connection to meshcored that never blocks the loop
 * (gw_uplink.c): it subscribes without the receive log, reads the events as
 * they come, keeps the last 512 messages in memory (gw_journal.c), and
 * passes the mesh_read methods and a bounded event stream (gw_feed.c) to
 * the clients that subscribed. Nothing here sends a message, changes the
 * node or holds a mesh key; the capture group has its own milestone.
 *
 * Vision (ADR-019 amendment 1): on the shell's socket, beside the capture's
 * requests, the gateway asks the Vision app for its state and, for one
 * viewer at a time, for one picture at a time. See the Vision section below
 * and gw_vision.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE

#include "gw.h"
#include "gw_capture.h"
#include "gw_feed.h"
#include "gw_frame.h"
#include "gw_gate.h"
#include "gw_journal.h"
#include "gw_pair.h"
#include "gw_policy.h"
#include "gw_send.h"
#include "gw_store.h"
#include "gw_tls.h"
#include "gw_uplink.h"
#include "gw_util.h"
#include "gw_vision.h"

#include "pocketipc/pocketipc.h"
#include "pocketipc/server.h"
#include "pocketlog/pocketlog.h"
#include "pocketpaths.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/capability.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define SESS_SLOTS (GW_MAX_SESSIONS + GW_MAX_PENDING + 1)
#define LOOP_MS 100
#define RECHECK_MS 5000
#define CLOSING_MS 2000          /* how long an ending connection may take to drain */
#define UPSTREAM_BACKOFF_MS 1000 /* after a service failed to answer */
#define WALL_SET_AFTER 1704067200LL /* 2024-01-01: an unset clock reads 1970 */

enum sess_state { S_FREE = 0, S_HANDSHAKE, S_OPEN };

struct sess {
    enum sess_state st;
    uint64_t id;
    int fd;
    SSL *ssl;
    bool admitted;          /* the verification callback said yes */
    enum gw_who who;
    uint8_t fp[GW_FP_LEN];
    bool hello;
    bool closing;           /* close once the output is written */
    bool pair_waiting;      /* gateway.pair is waiting for the person */
    double pair_id;
    int64_t deadline_ms;    /* the handshake's, then gateway.hello's; 0: none */
    int64_t last_rx_ms;
    struct gw_frame in;
    uint8_t *out;
    size_t out_len, out_off, out_cap;
    struct gw_bucket rate;
    uint32_t events;
    unsigned topics;        /* GW_T_*: what gateway.subscribe left it holding */
    int inflight;           /* requests waiting for meshcored's answer */
    bool cap_new;           /* it began to listen to the capture and has not been told its state */
    bool resume;            /* one of them completed: read on */
    char peer[INET_ADDRSTRLEN];
};

struct upstream {
    const char *service;
    int fd;
    int64_t retry_ms;   /* not asked again before this */
};

/* A client's request that meshcored has been asked to answer. */
struct mesh_req {
    bool used;
    uint64_t tag;
    uint64_t sid;               /* the session; it may be gone when the answer comes */
    double rid;                 /* the client's request id */
    const struct gw_method *m;
    int offset, limit;          /* GW_K_NODES */
};

#define MESH_EPOLL_ID UINT64_MAX
#define SHELL_EPOLL_ID (UINT64_MAX - 1)
/* The gateway's own requests to meshcored; a client's start at TAG_CLIENT. */
#define TAG_STATUS 1
#define TAG_SUBSCRIBE 2
#define TAG_MESSAGES 3
#define TAG_NODES 4
/* The gateway's own requests to PacketShark on the shell's socket. */
#define TAG_CAP_POLL 6
#define TAG_CAP_CANCEL 7
#define TAG_CAP_LATE 8
#define CAP_SAY_MAX 4
#define CAP_STOP_FLUSH_MS 500      /* on stopping: a cancel on its way gets this long to be answered */
/* The gateway's own requests to the Vision app on the shell's socket. */
#define TAG_VIS_POLL 9
#define TAG_VIS_PULL 10
#define TAG_VIS_OFF 11
#define TAG_CLIENT 16

static struct {
    struct pocketipc_server *ctl;
    int epfd;
    int lfd;                        /* the TCP listener, or -1 */
    uint32_t laddr;                 /* what it is bound to, network order */
    uint16_t port;
    const char *iface;
    const char *listen_override;    /* test hooks only */
    const char *shadow;
    const char *passwd;
    bool store_dirty;               /* the paired list in memory is not the one on storage */
    SSL_CTX *ctx;
    EVP_PKEY *key;
    uint8_t device_fp[GW_FP_LEN];
    char state_dir[GW_STORE_PATH_MAX];
    struct gw_store store;
    struct gw_pair pair;
    /* Why the last pairing window closed ("expired", "paired", ...), for
     * gateway.status: Settings asks once a second and holds no
     * subscription, so the gateway.pairing event alone would not reach it.
     * Empty while a window is open and before the first one. */
    char pair_closed[24];
    struct sess s[SESS_SLOTS];
    uint64_t next_id;
    struct gw_bucket handshakes;
    struct upstream up[4];
    bool identity_fault, state_fault, password_set;
    bool relisten;                  /* the listener's conditions changed: re-evaluate */
    int64_t recheck_ms;
    char boot_id[40];
    int ssl_idx;
    /* The mesh (M4.1). */
    struct gw_uplink *mesh;
    bool mesh_ready;                /* subscribed, and the journal holds this run */
    bool mesh_subscribed;           /* this connection's run is known and it sends events */
    bool resume_soon;               /* a session stopped at its share: do not wait in poll */
    bool mesh_fault_said;           /* the refusal to work with it was logged once */
    bool journal_filled;            /* mesh.messages of the run held was taken */
    struct gw_journal journal;
    struct gw_feed feed;
    cJSON *nodes;                   /* the last mesh.nodes, filtered, for the pages */
    int64_t nodes_ms;
    bool nodes_fetching;
    uint64_t next_tag;
    struct mesh_req req[GW_MESH_PENDING];
    /* Sending (M4.2). */
    struct gw_send send;
    double mesh_max_id;             /* the highest message id of this run seen so far */
    uint64_t mesh_conn;             /* counts the connections to meshcored */
    /* PacketShark's capture (ADR-016 amendment 1). */
    struct gw_uplink *shell;        /* to the shell's socket: capture.* of the PacketShark app only */
    bool cap_up;                    /* that socket is connected */
    int cap_seen;                   /* 1: the shell answered capture.status; -1: it knows no such method
                                     * (no PacketShark in this image); 0: not asked yet on this connection */
    int64_t cap_close_ms;           /* the link is no longer wanted and goes by then at the latest */
    bool cap_polling;               /* the gateway's own capture.status waits for its answer */
    int64_t cap_poll_ms;            /* when it was last asked, or tried */
    int64_t cap_poll_sent_ms;       /* when the one that waits was sent */
    bool cap_poll_due;              /* ask now: somebody began to listen */
    const char *cap_say[CAP_SAY_MAX]; /* what to tell clients when the unit confirms each cancel on its way */
    int cap_say_at, cap_say_n;
    char cap_late[GW_CAPTURE_TOKEN]; /* a request to take back once the shell is reached again */
    int64_t cap_late_until;
    int64_t cap_late_ms;            /* when it was last tried */
    uint64_t cap_ask_tag;           /* the capture.request_start that is on its way */
    struct gw_capture cap;
    /* Vision (ADR-019 amendment 1), on the same connection to the shell. */
    int vis_seen;                   /* 1: the shell answered vision.status; -1: it knows no such name (an image
                                     * without Vision's part); 0: not asked yet on this connection */
    bool vis_polling;               /* the gateway's own vision.status waits for its answer */
    int64_t vis_poll_ms;            /* when it was last asked, or tried */
    bool vis_poll_due;              /* ask now: somebody began to listen, or a preview began or ended */
    uint64_t vis_order;             /* counts the requests to the unit whose order matters */
    uint64_t vis_poll_order;        /* the place of the vision.status that waits */
    bool vis_pulling;               /* the gateway's own vision.pull waits for its answer */
    uint64_t vis_pull_for;          /* the preview it was asked for (gw_vision.began) */
    uint64_t vis_share_tag;         /* a client's vision.share {on: true} that is on its way, or 0 */
    uint64_t vis_share_sid;         /* whose it is */
    uint64_t vis_share_order;       /* and its place */
    bool vis_share_void;            /* that client has asked for off since: the answer makes no viewer */
    int vis_offs;                   /* the gateway's own vision.share {on: false} waiting for their answers */
    bool vis_say;                   /* vision.state is owed to the clients that listen */
    bool vis_biometric;             /* the unit's mode, as last answered, is never shared */
    struct gw_vision vis;
} G;

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t wall_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

static void fp_prefix(const uint8_t fp[GW_FP_LEN], char out[17])
{
    gw_hex(fp, 8, out);
}

/* ---- privilege ---------------------------------------------------------- */

/* The gateway needs no capability: its port is above 1024 and everything it
 * opens is root's own. Dropped before the first connection is accepted
 * (ADR-019 rule 8); it stays uid 0 until Doors has per-service users. */
static bool drop_capabilities(void)
{
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[2];
    int cap;

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        LOG_ERROR("no_new_privs refused: %s", strerror(errno));
        return false;
    }
    for (cap = 0; cap < 64; cap++) {
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) != 0) {
            if (errno == EINVAL) {
                break; /* past the kernel's last capability */
            }
            /* EPERM is what a process that is not root gets (the host
             * suites): it has no capability to lose. For root it is a
             * refusal, and the service does not run on. */
            if (geteuid() == 0) {
                LOG_ERROR("bounding set: capability %d was not dropped: %s", cap, strerror(errno));
                return false;
            }
        }
    }
    memset(&hdr, 0, sizeof(hdr));
    memset(data, 0, sizeof(data));
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    if (syscall(SYS_capset, &hdr, data) != 0) {
        LOG_ERROR("capset refused: %s", strerror(errno));
        return false;
    }
    /* Read back: what the kernel says is held now, not what was asked. */
    memset(&hdr, 0, sizeof(hdr));
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    memset(data, 0xff, sizeof(data));
    if (syscall(SYS_capget, &hdr, data) != 0 || data[0].effective != 0 || data[0].permitted != 0 ||
        data[0].inheritable != 0 || data[1].effective != 0 || data[1].permitted != 0 ||
        data[1].inheritable != 0) {
        LOG_ERROR("capabilities are still held after the drop");
        return false;
    }
    LOG_INFO("capabilities dropped");
    return true;
}

/* ---- sessions ----------------------------------------------------------- */

static struct sess *sess_by_id(uint64_t id)
{
    int i;

    for (i = 0; id != 0 && i < SESS_SLOTS; i++) {
        if (G.s[i].st != S_FREE && G.s[i].id == id) {
            return &G.s[i];
        }
    }
    return NULL;
}

static void ctl_event(const char *name, cJSON *data)
{
    pocketipc_server_broadcast(G.ctl, pocketipc_event(name, data));
}

static void pairing_closed_event(const char *reason)
{
    cJSON *d = cJSON_CreateObject();

    cJSON_AddStringToObject(d, "state", "closed");
    cJSON_AddStringToObject(d, "reason", reason);
    ctl_event("gateway.pairing", d);
    snprintf(G.pair_closed, sizeof(G.pair_closed), "%s", reason);
    /* A window that closed may have been the only reason to listen. Done
     * from the loop, not from here: this runs inside a session's own
     * handling, and closing the listener closes sessions. */
    G.relisten = true;
}

static void vis_session_gone(uint64_t sid);

static void sess_close(struct sess *s, const char *why)
{
    bool was_open_window = gw_pair_open(&G.pair);
    int i;

    if (s->st == S_FREE) {
        return;
    }
    /* A preview does not outlive its viewer's connection. */
    vis_session_gone(s->id);
    /* What it asked meshcored is no longer owed to anyone; the answer is
     * ignored when it comes. */
    for (i = 0; i < GW_MESH_PENDING; i++) {
        if (G.req[i].used && G.req[i].sid == s->id) {
            G.req[i].used = false;
        }
    }
    if (s->admitted) {
        char p[17];

        fp_prefix(s->fp, p);
        LOG_INFO("session %llu (%s) closed: %s", (unsigned long long)s->id, p, why);
    }
    gw_pair_gone(&G.pair, s->id);
    if (was_open_window && !gw_pair_open(&G.pair)) {
        pairing_closed_event("candidate_left");
    }
    if (s->ssl != NULL) {
        /* One attempt at a close_notify; a peer that is not reading does
         * not get to hold the slot. */
        (void)SSL_shutdown(s->ssl);
        SSL_free(s->ssl);
    }
    if (s->fd >= 0) {
        (void)epoll_ctl(G.epfd, EPOLL_CTL_DEL, s->fd, NULL);
        close(s->fd);
    }
    gw_frame_free(&s->in);
    free(s->out);
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    ERR_clear_error();
}

static void close_all_sessions(const char *why)
{
    int i;

    for (i = 0; i < SESS_SLOTS; i++) {
        sess_close(&G.s[i], why);
    }
}

static void sess_events(struct sess *s, uint32_t ev)
{
    struct epoll_event e;

    if (s->events == ev) {
        return;
    }
    memset(&e, 0, sizeof(e));
    e.events = ev;
    e.data.u64 = s->id;
    if (epoll_ctl(G.epfd, EPOLL_CTL_MOD, s->fd, &e) == 0) {
        s->events = ev;
    }
}

/* The connection ends as soon as what is queued for it is written. From
 * here nothing more is read from it, so it is taken off the read set (an
 * unread byte would otherwise wake the loop for ever) and given a short
 * time to drain; a peer that does not read does not keep the slot. */
static void sess_end(struct sess *s)
{
    s->closing = true;
    s->deadline_ms = mono_ms() + CLOSING_MS;
}

/* Writes what is queued. False when the session was closed. */
static bool sess_flush(struct sess *s)
{
    while (s->out_off < s->out_len) {
        size_t n = s->out_len - s->out_off;
        int w, err;

        if (n > 16384) {
            n = 16384;
        }
        ERR_clear_error();
        w = SSL_write(s->ssl, s->out + s->out_off, (int)n);
        if (w > 0) {
            s->out_off += (size_t)w;
            continue;
        }
        err = SSL_get_error(s->ssl, w);
        if (err == SSL_ERROR_WANT_WRITE) {
            sess_events(s, s->closing ? EPOLLOUT : (EPOLLIN | EPOLLOUT));
            return true;
        }
        if (err == SSL_ERROR_WANT_READ && !s->closing) {
            sess_events(s, EPOLLIN);
            return true;
        }
        sess_close(s, "write failed");
        return false;
    }
    s->out_len = s->out_off = 0;
    sess_events(s, EPOLLIN);
    s->resume = true; /* it may have been held back until its output was written */
    if (s->closing) {
        sess_close(s, "done");
        return false;
    }
    return true;
}

/* Queues one message (taken and freed). False when the session was closed. */
static bool sess_send(struct sess *s, cJSON *msg)
{
    char *text;
    size_t len, pending;

    if (msg == NULL) {
        sess_close(s, "out of memory");
        return false;
    }
    /* Every response and event carries the unit's monotonic clock
     * (docs/api/gateway.md §1). */
    cJSON_AddNumberToObject(msg, "mono_ms", (double)mono_ms());
    text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (text == NULL) {
        sess_close(s, "out of memory");
        return false;
    }
    len = strlen(text);
    if (s->out_off > 0) {
        memmove(s->out, s->out + s->out_off, s->out_len - s->out_off);
        s->out_len -= s->out_off;
        s->out_off = 0;
    }
    pending = s->out_len;
    /* One frame of any allowed size always fits; behind unread output the
     * queue's limit holds (ADR-019 "Limits"). */
    if (len > GW_FRAME_OUT_MAX || (pending != 0 && pending + 4 + len > GW_OUTQ_MAX)) {
        free(text);
        sess_close(s, "output limit");
        return false;
    }
    if (pending + 4 + len > s->out_cap) {
        size_t cap = pending + 4 + len;
        uint8_t *grown = realloc(s->out, cap);

        if (grown == NULL) {
            free(text);
            sess_close(s, "out of memory");
            return false;
        }
        s->out = grown;
        s->out_cap = cap;
    }
    s->out[pending] = (uint8_t)(len >> 24);
    s->out[pending + 1] = (uint8_t)(len >> 16);
    s->out[pending + 2] = (uint8_t)(len >> 8);
    s->out[pending + 3] = (uint8_t)len;
    memcpy(s->out + pending + 4, text, len);
    s->out_len = pending + 4 + len;
    free(text);
    return sess_flush(s);
}

static bool reply_error(struct sess *s, const cJSON *id, int code, const char *message)
{
    return sess_send(s, pocketipc_error_response(id, code, message));
}

/* An error, then the connection ends once it is written. */
static bool reply_fatal(struct sess *s, const cJSON *id, int code, const char *message)
{
    sess_end(s);
    return reply_error(s, id, code, message);
}

/* ---- the listener ------------------------------------------------------- */

static bool addr_private(uint32_t net_order)
{
    uint32_t a = ntohl(net_order);

    return (a >> 24) == 10 || (a >> 24) == 127 || (a >> 20) == ((172u << 4) | 1u) ||
           (a >> 16) == ((192u << 8) | 168u) || (a >> 16) == ((169u << 8) | 254u);
}

/* The address to listen on: the interface's first IPv4 address. 0 for none. */
static uint32_t wanted_addr(void)
{
    struct ifaddrs *list, *it;
    struct in_addr a;
    uint32_t found = 0;

    if (G.listen_override != NULL) {
        return inet_pton(AF_INET, G.listen_override, &a) == 1 ? a.s_addr : 0;
    }
    if (getifaddrs(&list) != 0) {
        return 0;
    }
    for (it = list; it != NULL; it = it->ifa_next) {
        if (it->ifa_addr != NULL && it->ifa_addr->sa_family == AF_INET && (it->ifa_flags & IFF_UP) != 0 &&
            strcmp(it->ifa_name, G.iface) == 0) {
            found = ((struct sockaddr_in *)(void *)it->ifa_addr)->sin_addr.s_addr;
            break;
        }
    }
    freeifaddrs(list);
    return found;
}

static void listener_close(const char *why)
{
    if (G.lfd < 0) {
        return;
    }
    (void)epoll_ctl(G.epfd, EPOLL_CTL_DEL, G.lfd, NULL);
    close(G.lfd);
    G.lfd = -1;
    G.laddr = 0;
    LOG_INFO("listener closed: %s", why);
    close_all_sessions("listener closed");
}

/* Why the listener is closed, or NULL when nothing forbids it. */
static const char *closed_reason(uint32_t addr)
{
    if (G.identity_fault) {
        return "identity_fault";
    }
    if (G.state_fault) {
        return "state_fault";
    }
    if (!G.password_set) {
        return "password_required";
    }
    if (G.store.n == 0 && !gw_pair_open(&G.pair)) {
        return "no_clients";
    }
    if (addr == 0) {
        return "no_address";
    }
    if (!addr_private(addr)) {
        return "public_address";
    }
    return NULL;
}

/* Brings the listener to what the state says it should be. Called after
 * anything that state depends on changed, and every RECHECK_MS. */
static void listener_update(void)
{
    uint32_t addr = wanted_addr();
    const char *why = closed_reason(addr);
    struct sockaddr_in sa;
    struct epoll_event e;
    char text[INET_ADDRSTRLEN];
    int fd, one = 1;

    if (why != NULL) {
        listener_close(why);
        return;
    }
    if (G.lfd >= 0 && G.laddr == addr) {
        return;
    }
    listener_close("address changed");
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(G.port);
    sa.sin_addr.s_addr = addr; /* one address, never the wildcard */
    memset(&e, 0, sizeof(e));
    e.events = EPOLLIN;
    e.data.u64 = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, GW_MAX_PENDING) != 0 ||
        epoll_ctl(G.epfd, EPOLL_CTL_ADD, fd, &e) != 0) {
        LOG_WARN("listen failed: %s", strerror(errno));
        close(fd);
        return;
    }
    G.lfd = fd;
    G.laddr = addr;
    LOG_INFO("listening on %s:%u", inet_ntop(AF_INET, &sa.sin_addr, text, sizeof(text)), (unsigned)G.port);
}

static int count_state(enum sess_state st, bool paired_only)
{
    int i, n = 0;

    for (i = 0; i < SESS_SLOTS; i++) {
        if (G.s[i].st == st && (!paired_only || G.s[i].who == GW_W_PAIRED)) {
            n++;
        }
    }
    return n;
}

/* The trust decision of one handshake (gw_tls.c calls it once). */
static int admit(void *user, SSL *ssl, const uint8_t fp[GW_FP_LEN])
{
    struct sess *s = SSL_get_ex_data(ssl, G.ssl_idx);
    int64_t now = mono_ms();

    (void)user;
    if (s == NULL || s->st != S_HANDSHAKE || s->admitted) {
        return 0;
    }
    if (gw_store_find(&G.store, fp) >= 0) {
        s->who = GW_W_PAIRED;
    } else if (gw_pair_admits(&G.pair, now)) {
        /* Rule 14: an unknown key gets through only while a window is open
         * and nobody else is at the door, and then only to gateway.pair. */
        gw_pair_candidate(&G.pair, s->id, fp, now);
        s->who = GW_W_CANDIDATE;
    } else {
        return 0;
    }
    memcpy(s->fp, fp, GW_FP_LEN);
    s->admitted = true;
    return 1;
}

static void sess_handshake(struct sess *s)
{
    char p[17];
    int r, err, i;

    ERR_clear_error();
    r = SSL_accept(s->ssl);
    if (r != 1) {
        err = SSL_get_error(s->ssl, r);
        if (err == SSL_ERROR_WANT_READ) {
            sess_events(s, EPOLLIN);
        } else if (err == SSL_ERROR_WANT_WRITE) {
            sess_events(s, EPOLLIN | EPOLLOUT);
        } else {
            /* Not a line a stranger can write to storage four times a
             * second: debug level only. */
            if (!s->admitted) {
                LOG_DEBUG("handshake from %s refused", s->peer);
            }
            sess_close(s, "handshake failed");
        }
        return;
    }
    /* The callback decided; the finished connection is checked again
     * against what was decided (TLS 1.3, not resumed, that key). */
    if (!s->admitted || !gw_tls_established_ok(s->ssl, s->fp)) {
        LOG_WARN("handshake from %s ended without an admitted key", s->peer);
        sess_close(s, "not admitted");
        return;
    }
    fp_prefix(s->fp, p);
    if (s->who == GW_W_PAIRED) {
        /* A second connection of the same client replaces the first: a
         * phone that changed networks leaves a dead socket behind. */
        for (i = 0; i < SESS_SLOTS; i++) {
            if (&G.s[i] != s && G.s[i].st == S_OPEN && G.s[i].who == GW_W_PAIRED &&
                gw_ct_eq(G.s[i].fp, s->fp, GW_FP_LEN)) {
                sess_close(&G.s[i], "replaced");
            }
        }
        if (count_state(S_OPEN, true) >= GW_MAX_SESSIONS) {
            sess_close(s, "session limit");
            return;
        }
    }
    s->st = S_OPEN;
    s->deadline_ms = mono_ms() + GW_HELLO_MS;
    s->last_rx_ms = mono_ms();
    sess_events(s, EPOLLIN);
    LOG_INFO("session %llu (%s) %s from %s", (unsigned long long)s->id, p,
             s->who == GW_W_PAIRED ? "paired" : "pairing candidate", s->peer);
}

static void listener_accept(void)
{
    for (;;) {
        struct sockaddr_in sa;
        socklen_t slen = sizeof(sa);
        struct epoll_event e;
        struct sess *s = NULL;
        int64_t now = mono_ms();
        int fd, i, one = 1;

        fd = accept4(G.lfd, (struct sockaddr *)&sa, &slen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            return;
        }
        for (i = 0; i < SESS_SLOTS; i++) {
            if (G.s[i].st == S_FREE) {
                s = &G.s[i];
                break;
            }
        }
        /* Refusals cost a close and nothing else: no TLS, no log line a
         * flood could fill the storage with. */
        if (s == NULL || sa.sin_family != AF_INET || !addr_private(sa.sin_addr.s_addr) ||
            count_state(S_HANDSHAKE, false) >= GW_MAX_PENDING || !gw_bucket_take(&G.handshakes, now)) {
            close(fd);
            continue;
        }
        memset(s, 0, sizeof(*s));
        s->fd = fd;
        s->id = ++G.next_id;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        inet_ntop(AF_INET, &sa.sin_addr, s->peer, sizeof(s->peer));
        s->ssl = SSL_new(G.ctx);
        memset(&e, 0, sizeof(e));
        e.events = EPOLLIN;
        e.data.u64 = s->id;
        if (s->ssl == NULL || gw_frame_init(&s->in, GW_FRAME_IN_MAX) != 0 || SSL_set_fd(s->ssl, fd) != 1 ||
            SSL_set_ex_data(s->ssl, G.ssl_idx, s) != 1 || epoll_ctl(G.epfd, EPOLL_CTL_ADD, fd, &e) != 0) {
            s->st = S_HANDSHAKE;
            sess_close(s, "setup failed");
            continue;
        }
        SSL_set_accept_state(s->ssl);
        s->events = EPOLLIN;
        s->st = S_HANDSHAKE;
        s->deadline_ms = now + GW_HANDSHAKE_MS;
        gw_bucket_init(&s->rate, GW_REQ_BURST, GW_REQ_PER_S, now);
        sess_handshake(s);
    }
}

/* ---- upstream ----------------------------------------------------------- */

/* Calls one allowlisted method of a service, with a deadline on the connect
 * and on the call. NULL with *code: an upstream error code, or
 * GW_ERR_UPSTREAM when the service is not there or did not answer. */
static cJSON *upstream_call(const struct gw_method *m, int *code)
{
    struct upstream *u = NULL;
    char err[160];
    cJSON *res;
    size_t i;
    int c = 0;

    for (i = 0; i < sizeof(G.up) / sizeof(G.up[0]); i++) {
        if (strcmp(G.up[i].service, m->service) == 0) {
            u = &G.up[i];
        }
    }
    *code = GW_ERR_UPSTREAM;
    if (u == NULL) {
        return NULL;
    }
    /* A service that just failed to answer is not asked again at once:
     * every call blocks this loop for its deadline, and a client that
     * repeats the request must not be able to turn that into a stall. */
    if (mono_ms() < u->retry_ms) {
        return NULL;
    }
    if (u->fd < 0) {
        u->fd = pocketipc_connect_timeout(u->service, GW_UPSTREAM_CONNECT_MS);
        if (u->fd < 0) {
            u->retry_ms = mono_ms() + UPSTREAM_BACKOFF_MS;
            return NULL;
        }
    }
    res = pocketipc_call_timeout(u->fd, m->name, NULL, GW_UPSTREAM_CALL_MS, &c, err, sizeof(err));
    if (res == NULL) {
        if (c == 0) {
            /* A transport failure or a timeout: the connection can no
             * longer be interpreted (docs/api/pocketipc.md). */
            close(u->fd);
            u->fd = -1;
            u->retry_ms = mono_ms() + UPSTREAM_BACKOFF_MS;
        } else if (c >= 1 && c <= 6) {
            *code = c;
        }
    }
    return res;
}

/* ---- the mesh: meshcored's events, the journal, the read methods -------- */

/* The scopes a session's client holds now: looked up, never remembered. */
static unsigned sess_scopes(const struct sess *s)
{
    int idx = gw_store_find(&G.store, s->fp);

    return idx < 0 ? 0u : G.store.c[idx].scopes;
}

/* The mesh is read only while somebody may read it: with no paired client
 * holding mesh_read the gateway holds no connection to meshcored and no
 * message in memory. */
static bool mesh_wanted(void)
{
    int i;

    if (G.identity_fault || G.state_fault) {
        return false;
    }
    for (i = 0; i < G.store.n; i++) {
        if ((G.store.c[i].scopes & GW_S_MESH_READ) != 0) {
            return true;
        }
    }
    return false;
}

static bool topic_held(unsigned topics)
{
    int i;

    for (i = 0; i < SESS_SLOTS; i++) {
        if (G.s[i].st == S_OPEN && G.s[i].hello && !G.s[i].closing && (G.s[i].topics & topics) != 0) {
            return true;
        }
    }
    return false;
}

/* One event (data taken) to every session that holds one of the topics and
 * whose client's scopes still allow it. A snapshot event - one a later
 * event or a request replaces - is skipped for a session that is behind on
 * reading; any other event that does not fit ends that session (sess_send),
 * and its client syncs when it returns (ADR-019 "Limits"). */
static void mesh_emit(unsigned topics, const char *name, cJSON *data, bool snapshot)
{
    int i;

    for (i = 0; data != NULL && i < SESS_SLOTS; i++) {
        struct sess *s = &G.s[i];
        cJSON *copy;

        if (s->st != S_OPEN || !s->hello || s->closing || s->who != GW_W_PAIRED || s->topics == 0) {
            continue;
        }
        s->topics = gw_topics_allowed(s->topics, sess_scopes(s));
        if ((s->topics & topics) == 0 || (snapshot && s->out_len - s->out_off > GW_EVENT_SOFT_MAX)) {
            continue;
        }
        copy = cJSON_Duplicate(data, 1);
        if (copy != NULL) {
            (void)sess_send(s, pocketipc_event(name, copy));
        }
    }
    cJSON_Delete(data);
}

static void mesh_upstream_event(bool connected)
{
    cJSON *d = cJSON_CreateObject();

    cJSON_AddStringToObject(d, "service", "meshcored");
    cJSON_AddBoolToObject(d, "connected", connected);
    if (connected) {
        cJSON_AddStringToObject(d, "run_id", G.journal.run_id);
        cJSON_AddStringToObject(d, "journal_id", G.journal.id);
    }
    mesh_emit(GW_T_MESH_ALL, "gateway.upstream", d, false);
}

/* One message as meshcored reports it, from an event or from mesh.messages:
 * into the journal, and - when that was news - to the clients. */
static void mesh_message(const cJSON *message)
{
    const cJSON *kept = NULL, *mid = cJSON_GetObjectItemCaseSensitive(message, "id");
    uint64_t seq = gw_journal_put(&G.journal, gw_filter_message(message), &kept);
    cJSON *d, *copy;

    if (cJSON_IsNumber(mid) && mid->valuedouble > G.mesh_max_id && mid->valuedouble <= GW_MSG_ID_MAX) {
        G.mesh_max_id = mid->valuedouble; /* what a send that follows is told apart from */
    }
    if (seq == 0 || !topic_held(GW_T_MESH)) {
        return;
    }
    d = cJSON_CreateObject();
    copy = cJSON_Duplicate(kept, 1);
    if (d == NULL || copy == NULL) {
        cJSON_Delete(d);
        cJSON_Delete(copy);
        return;
    }
    cJSON_AddNumberToObject(d, "seq", (double)seq);
    cJSON_AddStringToObject(d, "run_id", G.journal.run_id);
    cJSON_AddStringToObject(d, "journal_id", G.journal.id);
    cJSON_AddItemToObject(d, "message", copy);
    mesh_emit(GW_T_MESH, "mesh.message", d, false);
}

static void nodes_forget(void)
{
    cJSON_Delete(G.nodes);
    G.nodes = NULL;
}

static void nodes_drop(void);

static void mesh_on_event(void *user, const char *name, const cJSON *data)
{
    int64_t now = mono_ms();

    (void)user;
    if (!G.mesh_subscribed) {
        return; /* nothing is taken from a connection whose run is not known yet */
    }
    if (strcmp(name, "mesh.message") == 0) {
        mesh_message(cJSON_GetObjectItemCaseSensitive(data, "message"));
    } else if (strcmp(name, "mesh.activity") == 0) {
        gw_feed_activity(&G.feed, data, now);
    } else if (strcmp(name, "mesh.node") == 0) {
        if (topic_held(GW_T_MESH)) {
            const cJSON *why = cJSON_GetObjectItemCaseSensitive(data, "reason");
            /* A removal is not a snapshot a later event replaces: it is
             * never skipped for a client that is behind. */
            bool removed = cJSON_IsString(why) && strcmp(why->valuestring, "removed") == 0;

            mesh_emit(GW_T_MESH, name, gw_feed_node(&G.feed, gw_filter_event(name, data), now), !removed);
        }
    } else if (strcmp(name, "mesh.channel") == 0 || strcmp(name, "mesh.state") == 0) {
        mesh_emit(GW_T_MESH, name, gw_filter_event(name, data), false);
    }
    /* Everything else meshcored raises - app datagrams, the repeater
     * session, discovery, the receive log - is not passed on. */
}

/* meshcored answered something the gateway cannot work with, or not at
 * all: the connection is given up and made again after the backoff. */
static void mesh_refuse(void)
{
    if (!gw_uplink_connected(G.mesh)) {
        return; /* it went away by itself, which is not a fault of its answers */
    }
    if (!G.mesh_fault_said) {
        G.mesh_fault_said = true;
        LOG_WARN("meshcored did not answer as its API says; the mesh group stays unavailable");
    }
    gw_uplink_drop(G.mesh, mono_ms());
}

static void mesh_on_state(void *user, bool up)
{
    struct epoll_event e;

    (void)user;
    if (up) {
        G.mesh_conn++;
        memset(&e, 0, sizeof(e));
        e.events = EPOLLIN;
        e.data.u64 = MESH_EPOLL_ID;
        /* First who it is: the run decides whether the journal still holds. */
        if (epoll_ctl(G.epfd, EPOLL_CTL_ADD, gw_uplink_fd(G.mesh), &e) != 0 ||
            gw_uplink_call(G.mesh, "mesh.status", NULL, TAG_STATUS, mono_ms(), GW_MESH_CALL_MS) != 0) {
            mesh_refuse();
        }
        return;
    }
    /* The closed socket left the epoll set by being closed. */
    G.mesh_subscribed = false;
    G.nodes_fetching = false;
    nodes_drop();
    if (G.mesh_ready) {
        G.mesh_ready = false;
        LOG_INFO("meshcored connection lost");
        mesh_upstream_event(false);
    }
}

/* Answers one request a client is waiting for; result is taken. */
static void req_done(struct mesh_req *q, cJSON *result, int code)
{
    struct sess *s = sess_by_id(q->sid);
    const struct gw_method *m = q->m;
    cJSON *idn;

    q->used = false;
    if (s == NULL || s->st != S_OPEN) {
        cJSON_Delete(result);
        return;
    }
    if (s->inflight > 0) {
        s->inflight--;
    }
    s->resume = true;
    idn = cJSON_CreateNumber(q->rid);
    if (s->closing || idn == NULL) {
        cJSON_Delete(result);
    } else if ((sess_scopes(s) & m->scope) != m->scope) {
        /* The scope went while meshcored was answering. */
        cJSON_Delete(result);
        (void)reply_error(s, idn, GW_ERR_FORBIDDEN, "not permitted");
    } else if (result != NULL) {
        (void)sess_send(s, pocketipc_response(idn, result));
    } else {
        /* The upstream's own words stay on the unit. */
        (void)reply_error(s, idn, code, code == GW_ERR_UPSTREAM ? "upstream unavailable" : "upstream error");
    }
    cJSON_Delete(idn);
}

/* Answers the mesh.nodes requests that wait: each with its page of the
 * list held, or all with `code` when there is none. A client that has not
 * yet taken what it was sent waits for its page (mesh_tick comes back to
 * it): eight pages at once would be more than its queue may hold. Returns
 * how many still wait. */
static int nodes_serve(int code)
{
    int i, waiting = 0;

    for (i = 0; i < GW_MESH_PENDING; i++) {
        struct mesh_req *q = &G.req[i];
        struct sess *s;
        cJSON *page;

        if (!q->used || q->m->kind != GW_K_NODES) {
            continue;
        }
        s = sess_by_id(q->sid);
        if (G.nodes != NULL && s != NULL && s->out_len - s->out_off > GW_REQ_SOFT_MAX) {
            waiting++;
            continue;
        }
        page = G.nodes != NULL ? gw_nodes_page(G.nodes, q->offset, q->limit, G.journal.run_id) : NULL;
        req_done(q, page, page == NULL && G.nodes != NULL ? POCKETIPC_ERR_BACKEND : code);
    }
    return waiting;
}

/* The list is no longer good for anything: whoever waits for a page of it
 * is told, and it is forgotten. */
static void nodes_drop(void)
{
    nodes_forget();
    if (!G.nodes_fetching) {
        (void)nodes_serve(GW_ERR_UPSTREAM);
    }
}

static void nodes_done(const cJSON *result, int code)
{

    G.nodes_fetching = false;
    nodes_forget();
    if (result != NULL) {
        G.nodes = gw_filter_result(gw_method_find("mesh.nodes"), result);
        G.nodes_ms = mono_ms();
        if (G.nodes == NULL) {
            code = POCKETIPC_ERR_BACKEND;
        }
    }
    (void)nodes_serve(code);
}

static bool send_on_reply(uint64_t tag, const cJSON *result, int code);

static void mesh_on_reply(void *user, uint64_t tag, const cJSON *result, int code)
{
    int64_t now = mono_ms();
    const cJSON *v, *el;
    int i, changed;

    (void)user;
    switch (tag) {
    case TAG_STATUS:
        v = cJSON_GetObjectItemCaseSensitive(result, "run_id");
        if (!cJSON_IsString(v)) {
            mesh_refuse();
            return;
        }
        i = G.journal.run_id[0] != '\0';
        changed = gw_journal_set_run(&G.journal, v->valuestring);
        if (changed == 1) {
            /* Another run of meshcored: its message ids start again. */
            G.journal_filled = false;
            G.mesh_max_id = 0;
            nodes_drop();
            if (i) {
                LOG_INFO("meshcored started again: the journal starts again with it");
            }
        }
        if (changed < 0 ||
            gw_uplink_call(G.mesh, "mesh.subscribe", NULL, TAG_SUBSCRIBE, now, GW_MESH_CALL_MS) != 0) {
            mesh_refuse();
        }
        return;
    case TAG_SUBSCRIBE:
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "subscribed")) ||
            gw_uplink_call(G.mesh, "mesh.messages", NULL, TAG_MESSAGES, now, GW_MESH_CALL_MS) != 0) {
            mesh_refuse();
            return;
        }
        G.mesh_subscribed = true; /* from here its events are this run's */
        return;
    case TAG_MESSAGES:
        v = cJSON_GetObjectItemCaseSensitive(result, "messages");
        if (!cJSON_IsArray(v)) {
            mesh_refuse();
            return;
        }
        /* What meshcored still holds of this run and the journal does not:
         * all of it on a new journal, what was missed after a lost
         * connection. A message the journal has in this state already is
         * not journaled twice. */
        cJSON_ArrayForEach(el, v) {
            mesh_message(el);
        }
        G.journal_filled = true;
        G.mesh_ready = true;
        G.mesh_fault_said = false;
        LOG_INFO("meshcored connected");
        mesh_upstream_event(true);
        return;
    case TAG_NODES:
        nodes_done(result, code);
        return;
    default:
        break;
    }
    if (send_on_reply(tag, result, code)) {
        return;
    }
    for (i = 0; i < GW_MESH_PENDING; i++) {
        struct mesh_req *q = &G.req[i];

        if (q->used && q->tag == tag) {
            cJSON *filtered = result != NULL ? gw_filter_result(q->m, result) : NULL;

            req_done(q, filtered, result != NULL ? POCKETIPC_ERR_BACKEND : code);
            return;
        }
    }
    /* The session that asked is gone. */
}

/* Forgets the mesh: the connection, the journal's messages, what is held
 * for clients. Called when nobody may read it any more. */
static void mesh_stop(void)
{
    char id[GW_JOURNAL_ID_MAX + 1];
    uint64_t next = G.journal.next_seq;

    if (gw_uplink_connected(G.mesh)) {
        gw_uplink_drop(G.mesh, mono_ms());
    }
    if (G.journal.count > 0 || G.journal.run_id[0] != '\0') {
        memcpy(id, G.journal.id, sizeof(id));
        gw_journal_free(&G.journal);
        (void)gw_journal_init(&G.journal, id);
        /* A seq is never reused within a journal_id, and one is left out:
         * every cursor from before lies below what will be held, so a
         * client that returns is told that it may have missed something. */
        G.journal.next_seq = next + 1;
        LOG_INFO("no client may read the mesh: the journal was emptied");
    }
    G.journal_filled = false;
    gw_feed_reset(&G.feed);
    nodes_drop();
}

static void mesh_tick(int64_t now)
{
    cJSON *d;

    if (!mesh_wanted()) {
        mesh_stop();
        return;
    }
    gw_uplink_tick(G.mesh, now);
    while ((d = gw_feed_node_due(&G.feed, now)) != NULL) {
        mesh_emit(GW_T_MESH, "mesh.node", d, true);
    }
    d = gw_feed_activity_due(&G.feed, now);
    if (d != NULL) {
        mesh_emit(GW_T_MESH_ACTIVITY, "mesh.activity_bin", d, true);
    }
    if (G.nodes != NULL && !G.nodes_fetching && nodes_serve(0) == 0 && now - G.nodes_ms >= GW_NODES_CACHE_MS) {
        nodes_forget();
    }
}

static struct mesh_req *req_new(struct sess *s, const cJSON *id, const struct gw_method *m)
{
    int i;

    for (i = 0; i < GW_MESH_PENDING; i++) {
        struct mesh_req *q = &G.req[i];

        if (!q->used) {
            memset(q, 0, sizeof(*q));
            q->used = true;
            q->tag = TAG_CLIENT + G.next_tag++;
            q->sid = s->id;
            q->rid = id->valuedouble;
            q->m = m;
            s->inflight++;
            return q;
        }
    }
    return NULL;
}

static void req_cancel(struct sess *s, struct mesh_req *q)
{
    q->used = false;
    s->inflight--;
}

/* Vision shares the connection to the shell's socket (its section is
 * further down). */
static bool vis_holds_link(void);
static bool vis_ready(void);
static void vis_end(const char *why, bool tell);

/* ---- PacketShark's capture (ADR-016 amendment 1; ADR-019 rules 6 and 6a; gw_capture.h) ----
 *
 * A second connection that never blocks, to the shell's socket, held only
 * while some paired client may read a capture or holds vision. On it the
 * gateway calls five methods of the PacketShark app (and six of the Vision
 * app, in the Vision section) and nothing else - no shell.* method, and
 * no subscription: what clients are told is made from the gateway's own
 * capture.status, asked once a second while a client holds the topic.
 *
 * A client's scopes are looked up when it asks, again when the unit's
 * answer comes (req_done), and again for every event (mesh_emit). */

static bool cap_wanted(void)
{
    int i;

    if (G.identity_fault || G.state_fault) {
        return false;
    }
    for (i = 0; i < G.store.n; i++) {
        if ((G.store.c[i].scopes & GW_S_CAPTURE_VIEW) != 0) {
            return true;
        }
    }
    return false;
}

/* The shell is connected and has not said that it knows no capture.status.
 * The gateway asks nothing on connecting to find out: the first question is
 * a client's, or the gateway's own once a client listens. So nothing reads
 * a capture, and the unit's screen says nothing of a reader, until a client
 * does. */
static bool cap_ready(void)
{
    return G.cap_up && G.cap_seen >= 0;
}

static void cap_upstream_event(bool connected)
{
    cJSON *d = cJSON_CreateObject();

    if (d != NULL) {
        cJSON_AddStringToObject(d, "service", "shell");
        cJSON_AddBoolToObject(d, "connected", connected);
    }
    mesh_emit(GW_T_CAPTURE, "gateway.upstream", d, false);
}

/* capture.request_cancel for the request of this name: the gateway's own
 * method, which no client can call (ADR-019 rule 6a). The unit takes down
 * the sheet of that request and no other - not a later request's, and not a
 * capture that a tap has started, which is no request any more. `say` is
 * what clients are told if the unit answers that it took the sheet down
 * ("refused", "timeout"), or NULL for nothing. `tag` is TAG_CAP_CANCEL, or
 * TAG_CAP_LATE for one that is owed from before (cap_cancel_later). True
 * when it is on its way. */
static bool cap_send_cancel(const char *token, const char *say, uint64_t tag)
{
    cJSON *p = cJSON_CreateObject();

    if (p == NULL || cJSON_AddStringToObject(p, "request", token) == NULL || !gw_uplink_connected(G.shell)) {
        cJSON_Delete(p);
        return false;
    }
    if (gw_uplink_call(G.shell, "capture.request_cancel", p, tag, mono_ms(), GW_CAPTURE_CALL_MS) != 0) {
        return false;
    }
    /* The answers come in the order of the cancels. */
    if (tag == TAG_CAP_CANCEL && G.cap_say_n < CAP_SAY_MAX) {
        G.cap_say[(G.cap_say_at + G.cap_say_n++) % CAP_SAY_MAX] = say;
    }
    return true;
}

/* A request that could not be taken back for certain, because the shell
 * could not be reached or did not answer: its sheet may be up, or may still
 * come up when a slow shell gets to the request. It is taken back by name
 * once a second (cap_tick) until the unit says it took a sheet down, or
 * until no sheet of it can be up any more. Under all of this the unit's own
 * 60 seconds end the sheet. */
static void cap_cancel_later(const char *token)
{
    snprintf(G.cap_late, sizeof(G.cap_late), "%s", token);
    G.cap_late_until = mono_ms() + GW_CAPTURE_ASK_MS;
    G.cap_late_ms = 0;
}

/* The waiting start request is the gateway's no longer: taken back on the
 * unit by its own name, and forgotten here. What clients are told waits for
 * the unit's answer: if START was tapped a moment ago there was no sheet to
 * take down, the capture runs on, and nothing is said of a refusal. */
static void cap_cancel(const char *why, const char *say)
{
    if (!G.cap.asking) {
        return;
    }
    if (cap_send_cancel(G.cap.token, say, TAG_CAP_CANCEL)) {
        LOG_INFO("capture: a start request was withdrawn (%s)", why);
    } else {
        cap_cancel_later(G.cap.token);
        LOG_INFO("capture: a start request could not be withdrawn yet (%s); the unit ends it by itself", why);
    }
    gw_capture_ask_end(&G.cap);
}

static void cap_on_state(void *user, bool up)
{
    struct epoll_event e;
    cJSON *state = NULL;

    (void)user;
    if (up) {
        memset(&e, 0, sizeof(e));
        e.events = EPOLLIN;
        e.data.u64 = SHELL_EPOLL_ID;
        G.cap_up = true;
        G.cap_seen = 0;
        G.cap_polling = false;
        G.cap_poll_due = true;
        if (epoll_ctl(G.epfd, EPOLL_CTL_ADD, gw_uplink_fd(G.shell), &e) != 0) {
            gw_uplink_drop(G.shell, mono_ms());
            return;
        }
        cap_upstream_event(true);
        return;
    }
    /* The closed socket left the epoll set by being closed. Requests that
     * waited were answered "upstream unavailable" before this. */
    if (cap_ready()) {
        if (G.cap_seen > 0) {
            LOG_INFO("PacketShark on the shell's socket is out of reach");
        }
        cap_upstream_event(false);
    }
    G.cap_up = false;
    G.cap_seen = 0;
    G.cap_close_ms = 0;
    G.cap_polling = false;
    G.cap_say_n = 0;
    gw_capture_lost(&G.cap, &state);
    if (state != NULL) {
        mesh_emit(GW_T_CAPTURE, "capture.state", state, false);
    }
    if (G.cap.asking) {
        /* Its sheet may be up and cannot be taken down now. */
        cap_cancel_later(G.cap.token);
        gw_capture_ask_end(&G.cap);
    }
}

/* The shell answered "unknown method" to a capture.* name: this unit has no
 * PacketShark. Learned from the first question, whoever's it was; not asked
 * again on this connection, and its methods are unavailable (error 9). */
static void cap_absent(void)
{
    if (G.cap_seen >= 0) {
        G.cap_seen = -1;
        LOG_INFO("the shell knows no capture.status: no PacketShark in this image");
        cap_upstream_event(false);
    }
}

/* The gateway asked for no events and takes none. */
static void cap_on_event(void *user, const char *name, const cJSON *data)
{
    (void)user;
    (void)name;
    (void)data;
}

/* The unit's answer to the waiting start request, whoever still waits for
 * it here. */
static void cap_ask_answered(const cJSON *result, int code, int64_t now)
{
    if (result != NULL && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "shown"))) {
        gw_capture_ask_shown(&G.cap, now);
    } else if (result == NULL && code >= POCKETIPC_ERR_UNKNOWN_METHOD && code <= POCKETIPC_ERR_UNSUPPORTED) {
        gw_capture_ask_end(&G.cap); /* the unit refused it: no sheet is up */
    } else {
        /* No answer in time, or one that does not say: the request may
         * still reach the unit and raise its sheet, now or when a slow
         * shell gets to it. Taken back by name, and again until it is. */
        cap_cancel_later(G.cap.token);
        cap_cancel("its answer did not come", NULL);
    }
}

static void cap_on_reply(void *user, uint64_t tag, const cJSON *result, int code)
{
    cJSON *state = NULL, *stats = NULL;
    const char *outcome = NULL;
    int64_t now = mono_ms();
    int i;

    (void)user;
    if (tag == TAG_CAP_CANCEL) {
        const char *say = NULL;

        if (G.cap_say_n > 0) {
            say = G.cap_say[G.cap_say_at];
            G.cap_say_at = (G.cap_say_at + 1) % CAP_SAY_MAX;
            G.cap_say_n--;
        }
        /* Only a sheet the unit says it took down was refused by this.
         * Anything else - no request of that name was up: answered on the
         * unit a moment before, or START was tapped - is not ours to call. */
        if (say != NULL && result != NULL && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "shown"))) {
            mesh_emit(GW_T_CAPTURE, "capture.state", gw_capture_outcome(say), false);
        }
        return;
    }
    if (tag == TAG_CAP_LATE) {
        if (result != NULL && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "shown"))) {
            G.cap_late[0] = '\0'; /* the sheet that came up after all is down */
        }
        return;
    }
    if (tag == TAG_CAP_POLL) {
        bool said;

        G.cap_polling = false;
        if (result == NULL) {
            if (code == POCKETIPC_ERR_UNKNOWN_METHOD) {
                cap_absent();
            }
            return; /* a shell that does not answer in time loses the connection (gw_uplink), which is made again */
        }
        if (G.cap_seen <= 0) {
            G.cap_seen = 1;
            LOG_INFO("PacketShark answers on the shell's socket");
        }
        gw_capture_step(&G.cap, result, G.cap_poll_sent_ms, &state, &stats, &outcome);
        said = state != NULL;
        if (state != NULL) {
            mesh_emit(GW_T_CAPTURE, "capture.state", state, false);
        }
        /* A client that has just begun to listen is told how things are,
         * it alone: the others were told when it changed. */
        for (i = 0; i < SESS_SLOTS; i++) {
            struct sess *s = &G.s[i];
            cJSON *again;

            if (s->st != S_OPEN || !s->cap_new || !G.cap.told) {
                continue;
            }
            s->cap_new = false;
            if (said || !s->hello || s->closing || s->who != GW_W_PAIRED ||
                (gw_topics_allowed(s->topics, sess_scopes(s)) & GW_T_CAPTURE) == 0) {
                continue;
            }
            again = gw_capture_told(&G.cap);
            if (again != NULL) {
                (void)sess_send(s, pocketipc_event("capture.state", again));
            }
        }
        if (stats != NULL) {
            /* Figures are replaced by the next: skipped for a client that is behind. */
            mesh_emit(GW_T_CAPTURE, "capture.stats", stats, true);
        }
        if (outcome != NULL) {
            gw_capture_ask_end(&G.cap);
            mesh_emit(GW_T_CAPTURE, "capture.state", gw_capture_outcome(outcome), false);
        }
        return;
    }
    for (i = 0; i < GW_MESH_PENDING; i++) {
        struct mesh_req *q = &G.req[i];

        if (q->used && q->tag == tag) {
            cJSON *filtered = result != NULL ? gw_filter_result(q->m, result) : NULL;

            if (tag == G.cap_ask_tag && G.cap.asking) {
                cap_ask_answered(result, code, now);
            }
            if (result == NULL && code == POCKETIPC_ERR_UNKNOWN_METHOD) {
                /* A client's own question was the first: no PacketShark
                 * here, which to the client is "unavailable", as it is to
                 * every question after this one. */
                cap_absent();
                code = GW_ERR_UPSTREAM;
            }
            req_done(q, filtered, result != NULL ? POCKETIPC_ERR_BACKEND : code);
            return;
        }
    }
    /* The session that asked is gone; its request may still have raised a
     * sheet. (cap_tick takes back the request of a client that went away.) */
    if (tag == G.cap_ask_tag && G.cap.asking) {
        cap_ask_answered(result, code, now);
    }
}

static void cap_tick(int64_t now)
{
    if (!cap_wanted()) {
        /* Nobody may read a capture any more: the connection goes. But a
         * request that waits on the unit is taken back first, and the link
         * stays until the unit has answered that (or its time is up): a
         * sheet is not left asking for a client that was just revoked. */
        if (gw_uplink_connected(G.shell)) {
            cap_cancel("no client may read a capture", NULL);
            if (G.cap_close_ms == 0) {
                G.cap_close_ms = now + GW_CAPTURE_CALL_MS;
            }
            if (G.cap_say_n > 0 && now < G.cap_close_ms) {
                gw_uplink_tick(G.shell, now);
                return;
            }
            if (!vis_holds_link()) {
                gw_uplink_drop(G.shell, now);
            }
        }
        /* (A cancel that is still owed, cap_cancel_later, is kept: it is
         * sent if a client may read again within the sheet's lifetime, and
         * expires by itself otherwise.) */
        G.cap_close_ms = 0;
        if (vis_holds_link()) {
            /* The link is Vision's alone: kept, or made, for it. Nothing of
             * a capture is asked on it. */
            gw_uplink_tick(G.shell, now);
        }
        return;
    }
    G.cap_close_ms = 0;
    gw_uplink_tick(G.shell, now);
    if (G.cap.asking) {
        /* Whose request it is, looked up now: the session that asked must
         * still be there, be that client, and that client must still be
         * paired and allowed to ask. */
        const struct sess *s = sess_by_id(G.cap.sid);
        int idx = gw_store_find(&G.store, G.cap.fp);

        if (s == NULL || s->st != S_OPEN || s->closing || memcmp(s->fp, G.cap.fp, GW_FP_LEN) != 0) {
            cap_cancel("its client went away", "refused");
        } else if (idx < 0 || (G.store.c[idx].scopes & GW_S_CAPTURE_CONTROL) == 0) {
            cap_cancel("its client may no longer ask", "refused");
        } else if (now - G.cap.asked_ms >= GW_CAPTURE_ASK_MS) {
            cap_cancel("nobody answered it", "timeout");
        }
    }
    if (G.cap_late[0] != '\0') {
        /* A request owed a cancel (cap_cancel_later). */
        if (now >= G.cap_late_until) {
            G.cap_late[0] = '\0';
        } else if (cap_ready() && now - G.cap_late_ms >= GW_CAPTURE_POLL_MS) {
            (void)cap_send_cancel(G.cap_late, NULL, TAG_CAP_LATE);
            G.cap_late_ms = now;
        }
    }
    /* Asked only while somebody is told the answer. */
    if (cap_ready() && !G.cap_polling && (topic_held(GW_T_CAPTURE) || G.cap.asking) &&
        (G.cap_poll_due || now - G.cap_poll_ms >= GW_CAPTURE_POLL_MS)) {
        if (gw_uplink_call(G.shell, "capture.status", NULL, TAG_CAP_POLL, now, GW_CAPTURE_CALL_MS) == 0) {
            G.cap_polling = true;
            G.cap_poll_sent_ms = now;
        }
        G.cap_poll_ms = now;
        G.cap_poll_due = false;
    }
}

/* capture.status, capture.hold, capture.stop, capture.request_start. The
 * row has said who may call it; the parameters are checked and written
 * again before anything is sent; the answer comes later (cap_on_reply). */
static bool do_capture(struct sess *s, const cJSON *id, const struct gw_method *m, const cJSON *params, int idx)
{
    bool ask = strcmp(m->name, "capture.request_start") == 0;
    char token[GW_CAPTURE_TOKEN] = "";
    int64_t now = mono_ms();
    struct mesh_req *q;
    cJSON *up = NULL;

    if (m->params != NULL ? m->params(params, &up) != 0 : !gw_params_empty(params)) {
        return reply_error(s, id, POCKETIPC_ERR_INVALID_PARAMS, "invalid params");
    }
    if (!cap_ready() || idx < 0) {
        cJSON_Delete(up);
        return reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
    }
    if (ask) {
        uint8_t rnd[(GW_CAPTURE_TOKEN - 1) / 2];

        /* How often, counted here and not on the unit: a request opens
         * PacketShark there even when it is then refused. */
        switch (gw_capture_may_ask(&G.cap, s->fp, now)) {
        case GW_ASK_BUSY:
            cJSON_Delete(up);
            return reply_error(s, id, POCKETIPC_ERR_BUSY, "busy");
        case GW_ASK_RATE:
            cJSON_Delete(up);
            return reply_error(s, id, GW_ERR_RATE, "rate limited");
        default:
            break;
        }
        /* Who asks is the name this unit gave the client at pairing, never
         * a word of the client's; what the request is called is the
         * gateway's own, so that only the gateway can take it back. */
        if (gw_tls_random(rnd, sizeof(rnd)) != 0 || up == NULL) {
            cJSON_Delete(up);
            return reply_error(s, id, POCKETIPC_ERR_BACKEND, "upstream error");
        }
        gw_hex(rnd, sizeof(rnd), token);
        if (cJSON_AddStringToObject(up, "client", G.store.c[idx].name) == NULL ||
            cJSON_AddStringToObject(up, "request", token) == NULL) {
            cJSON_Delete(up);
            return reply_error(s, id, POCKETIPC_ERR_BACKEND, "upstream error");
        }
    }
    q = req_new(s, id, m);
    if (q == NULL) {
        cJSON_Delete(up);
        return reply_error(s, id, POCKETIPC_ERR_BUSY, "busy");
    }
    if (gw_uplink_call(G.shell, m->name, up, q->tag, now, GW_CAPTURE_CALL_MS) != 0) {
        req_cancel(s, q);
        return reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
    }
    if (ask) {
        gw_capture_ask(&G.cap, s->fp, s->id, token, now);
        G.cap_ask_tag = q->tag;
    }
    return true;
}

static bool do_subscribe(struct sess *s, const cJSON *id, const cJSON *params, unsigned scopes)
{
    unsigned mask = 0;
    bool vis_new;
    cJSON *r, *up;

    if (gw_topics_parse(params, scopes, &mask) != 0) {
        return reply_error(s, id, POCKETIPC_ERR_INVALID_PARAMS, "invalid params");
    }
    vis_new = (mask & GW_T_VISION) != 0 && (s->topics & GW_T_VISION) == 0;
    s->topics = mask; /* replaces what was held */
    if ((mask & GW_T_VISION) == 0 && gw_vision_view_of(&G.vis, s->id) == GW_VV_SELF) {
        /* The viewer gave the topic back: its preview goes with it. */
        vis_end("its client no longer listens", true);
    }
    r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "topics", gw_topics_json(mask));
    if (mask != 0) {
        /* gateway.upstream says when this changes; this is how it is now. */
        up = cJSON_AddObjectToObject(r, "upstream");
        cJSON_AddBoolToObject(up, "meshcored", G.mesh_ready);
        if ((mask & GW_T_CAPTURE) != 0) {
            cJSON_AddBoolToObject(up, "shell", cap_ready());
        } else if ((mask & GW_T_VISION) != 0) {
            cJSON_AddBoolToObject(up, "shell", vis_ready());
        }
    }
    /* A new listener is told how the capture is, with the next answer. */
    s->cap_new = (mask & GW_T_CAPTURE) != 0;
    if (s->cap_new) {
        G.cap_poll_due = true;
    }
    if (!sess_send(s, pocketipc_response(id, r))) {
        return false;
    }
    if (vis_new) {
        /* A new listener to Vision is told to ask how it is, once. */
        G.vis_poll_due = true;
        return sess_send(s, pocketipc_event("vision.state", cJSON_CreateObject()));
    }
    return true;
}

/* A mesh_read method meshcored answers. The answer comes later
 * (mesh_on_reply); the loop does not wait for it. */
static bool do_mesh(struct sess *s, const cJSON *id, const struct gw_method *m, const cJSON *params)
{
    struct mesh_req *q;
    cJSON *up = NULL;

    /* What goes upstream is written by the row's own check, from nothing. */
    if (m->params != NULL ? m->params(params, &up) != 0 : !gw_params_empty(params)) {
        return reply_error(s, id, POCKETIPC_ERR_INVALID_PARAMS, "invalid params");
    }
    if (!G.mesh_ready) {
        cJSON_Delete(up);
        return reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
    }
    q = req_new(s, id, m);
    if (q == NULL) {
        cJSON_Delete(up);
        return reply_error(s, id, POCKETIPC_ERR_BUSY, "busy");
    }
    if (gw_uplink_call(G.mesh, m->name, up, q->tag, mono_ms(), GW_MESH_CALL_MS) != 0) {
        req_cancel(s, q);
        return reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
    }
    return true;
}

/* mesh.nodes in pages: meshcored's list can be larger than a frame to a
 * client may be. One list is asked for and serves every page wanted while
 * it is fresh, so the pages of one reading belong together. */
static bool do_nodes(struct sess *s, const cJSON *id, const struct gw_method *m, const cJSON *params)
{
    struct mesh_req *q;
    int offset, limit;

    if (gw_page_params(params, &offset, &limit) != 0) {
        return reply_error(s, id, POCKETIPC_ERR_INVALID_PARAMS, "invalid params");
    }
    if (!G.mesh_ready) {
        return reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
    }
    if (G.nodes != NULL && !G.nodes_fetching && mono_ms() - G.nodes_ms < GW_NODES_CACHE_MS) {
        cJSON *page = gw_nodes_page(G.nodes, offset, limit, G.journal.run_id);

        return page != NULL ? sess_send(s, pocketipc_response(id, page))
                            : reply_error(s, id, POCKETIPC_ERR_BACKEND, "upstream error");
    }
    q = req_new(s, id, m);
    if (q == NULL) {
        return reply_error(s, id, POCKETIPC_ERR_BUSY, "busy");
    }
    q->offset = offset;
    q->limit = limit;
    if (!G.nodes_fetching) {
        if (gw_uplink_call(G.mesh, m->name, NULL, TAG_NODES, mono_ms(), GW_MESH_CALL_MS) != 0) {
            req_cancel(s, q);
            return reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
        }
        G.nodes_fetching = true;
    }
    return true;
}

/* mesh.sync: from the journal, whether or not meshcored is there now. */
static bool do_sync(struct sess *s, const cJSON *id, const cJSON *params)
{
    bool have_after;
    uint64_t after;
    cJSON *r;
    int limit;

    if (gw_sync_params(params, &have_after, &after, &limit) != 0) {
        return reply_error(s, id, POCKETIPC_ERR_INVALID_PARAMS, "invalid params");
    }
    if (!G.journal_filled) {
        /* Not yet filled from meshcored for the run it is in. */
        return reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
    }
    r = gw_journal_sync(&G.journal, have_after, after, limit);
    if (r == NULL) {
        return reply_error(s, id, POCKETIPC_ERR_BACKEND, "upstream error");
    }
    return sess_send(s, pocketipc_response(id, r));
}

/* ---- the mesh: sending (docs/api/gateway.md §8; gw_send.h) -------------- */

static bool send_paired(const uint8_t fp[GW_FP_LEN])
{
    return gw_store_find(&G.store, fp) >= 0;
}

/* Answers the request that waits for a send, if its session is still
 * there; result is taken. The record has the outcome either way. */
static void send_answer(uint64_t sid, double rid, cJSON *result, int code, const char *message)
{
    struct sess *s = sess_by_id(sid);
    cJSON *idn;

    if (s == NULL || s->st != S_OPEN) {
        cJSON_Delete(result);
        return;
    }
    if (s->inflight > 0) {
        s->inflight--;
    }
    s->resume = true;
    idn = cJSON_CreateNumber(rid);
    if (s->closing || idn == NULL) {
        cJSON_Delete(result);
    } else if (result != NULL) {
        (void)sess_send(s, pocketipc_response(idn, result));
    } else {
        (void)reply_error(s, idn, code, message);
    }
    cJSON_Delete(idn);
}

/* Passes one record's message to meshcored; up is taken. False when it did
 * not leave the gateway. */
static bool send_forward(struct gw_send_rec *r, cJSON *up)
{
    r->tag = TAG_CLIENT + G.next_tag++;
    r->conn = G.mesh_conn;
    return up != NULL && gw_uplink_call(G.mesh, "mesh.send", up, r->tag, mono_ms(), GW_MESH_CALL_MS) == 0;
}

/* meshcored's answer to a request made for a send: to the send itself, or
 * to the list asked for because an earlier answer never came. False when
 * the tag is not a send's. */
static bool send_on_reply(uint64_t tag, const cJSON *result, int code)
{
    struct gw_send_client *c = NULL;
    struct gw_send_rec *r = gw_send_by_tag(&G.send, tag, &c);
    uint64_t sid;
    double rid;
    bool dup;
    cJSON *up;
    int idx;

    if (r == NULL) {
        return false;
    }
    sid = r->sid;
    rid = r->rid;
    dup = r->dup;
    r->sid = 0;
    if (r->state == GW_SS_PENDING) {
        switch (gw_send_answered(r, result, code)) {
        case GW_SO_DONE:
            /* Without memory for the answer: not a code that says "refused". */
            send_answer(sid, rid, gw_send_result(r, dup), GW_ERR_UPSTREAM, "upstream unavailable");
            break;
        case GW_SO_REFUSED:
            /* The upstream's own words stay on the unit. */
            send_answer(sid, rid, NULL, code, "upstream error");
            break;
        case GW_SO_UNCERTAIN:
            LOG_WARN("a send got no answer from meshcored: its outcome is not known");
            send_answer(sid, rid, NULL, GW_ERR_UPSTREAM, "upstream unavailable");
            break;
        default:
            LOG_WARN("meshcored answered a send in a way that says nothing: it is not sent again");
            send_answer(sid, rid, NULL, POCKETIPC_ERR_POLICY, "outcome unknown");
            break;
        }
        return true;
    }
    /* RESOLVING: the list that says whether the earlier attempt was taken. */
    if (result == NULL) {
        gw_send_unresolved(r);
        send_answer(sid, rid, NULL, GW_ERR_UPSTREAM, "upstream unavailable");
        return true;
    }
    switch (gw_send_looked(&G.send, r, cJSON_GetObjectItemCaseSensitive(result, "messages"))) {
    case GW_SL_FOUND:
        send_answer(sid, rid, gw_send_result(r, true), GW_ERR_UPSTREAM, "upstream unavailable");
        return true;
    case GW_SL_UNKNOWN:
        send_answer(sid, rid, NULL, POCKETIPC_ERR_POLICY, "outcome unknown");
        return true;
    default:
        break;
    }
    /* Not in the list. That is proof only when the list came over the very
     * connection the message was passed on: there meshcored reads the
     * request before the question. On a connection made since, a request
     * still unread in the old one could be taken after this answer. */
    if (r->conn != G.mesh_conn) {
        r->state = GW_SS_UNKNOWN;
        cJSON_Delete(r->up);
        r->up = NULL;
        LOG_WARN("a send got no answer and the connection to meshcored is another since: it is not sent again");
        send_answer(sid, rid, NULL, POCKETIPC_ERR_POLICY, "outcome unknown");
        return true;
    }
    /* Proven not taken, so it is sent now - if the client may still send
     * and meshcored is still the run that was asked. */
    idx = gw_store_find(&G.store, c->fp);
    if (idx < 0 || (G.store.c[idx].scopes & GW_S_MESH_SEND) == 0) {
        gw_send_unresolved(r);
        send_answer(sid, rid, NULL, GW_ERR_FORBIDDEN, "not permitted");
        return true;
    }
    up = r->up;
    r->up = NULL;
    if (!G.mesh_ready || strcmp(r->run_id, G.journal.run_id) != 0) {
        cJSON_Delete(up);
        up = NULL;
    }
    r->state = GW_SS_PENDING;
    r->id_before = G.mesh_max_id;
    if (up == NULL || !send_forward(r, up)) {
        gw_send_unresolved(r);
        send_answer(sid, rid, NULL, GW_ERR_UPSTREAM, "upstream unavailable");
        return true;
    }
    LOG_INFO("a send whose answer never came was not with meshcored: passed on again");
    r->sid = sid;
    r->rid = rid;
    r->dup = false;
    return true;
}

/* mesh.send and mesh.resend (scope mesh_send). One client_msg_id is passed
 * to meshcored at most once unless meshcored's own list proves that it did
 * not take it. */
static bool do_send(struct sess *s, const cJSON *id, const struct gw_method *m, const cJSON *params)
{
    struct gw_send_client *c;
    struct gw_send_rec *r = NULL;
    struct gw_send_req q;
    const cJSON *att;
    struct sess *w;
    cJSON *res, *up;

    if (gw_send_parse(params, m->kind == GW_K_RESEND, &q) != 0) {
        return reply_error(s, id, POCKETIPC_ERR_INVALID_PARAMS, "invalid params");
    }
    c = gw_send_client(&G.send, s->fp, send_paired);
    if (c == NULL) {
        return reply_error(s, id, POCKETIPC_ERR_BUSY, "busy");
    }
    switch (gw_send_begin(c, &q, G.mesh_ready ? G.journal.run_id : NULL, G.mesh_max_id, mono_ms(), &r)) {
    case GW_SA_REPLY:
        /* Answered from the record, also while meshcored is away. */
        res = gw_send_result(r, true);
        return res != NULL ? sess_send(s, pocketipc_response(id, res))
                           : reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
    case GW_SA_MISMATCH:
        return reply_error(s, id, POCKETIPC_ERR_INVALID_PARAMS, "client_msg_id was used for another message");
    case GW_SA_UNKNOWN:
        return reply_error(s, id, POCKETIPC_ERR_POLICY, "outcome unknown");
    case GW_SA_STALE_RUN:
        return reply_error(s, id, POCKETIPC_ERR_POLICY, "not a message of this run");
    case GW_SA_RATE:
        return reply_error(s, id, GW_ERR_RATE, "rate limited");
    case GW_SA_BUSY:
        return reply_error(s, id, POCKETIPC_ERR_BUSY, "busy");
    case GW_SA_UNAVAILABLE:
        return reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
    case GW_SA_IN_PROGRESS:
        w = sess_by_id(r->sid);
        if (w != NULL && w->st == S_OPEN && !w->closing) {
            return reply_error(s, id, POCKETIPC_ERR_BUSY, "busy");
        }
        /* The connection that asked is gone (PAIRING.md §7): this request
         * takes its place and gets the answer when it comes. */
        r->sid = s->id;
        r->rid = id->valuedouble;
        r->dup = true;
        s->inflight++;
        return true;
    case GW_SA_LOOKUP:
        up = gw_send_upstream(&q, params);
        r->tag = TAG_CLIENT + G.next_tag++;
        if (up == NULL || gw_uplink_call(G.mesh, "mesh.messages", NULL, r->tag, mono_ms(), GW_MESH_CALL_MS) != 0) {
            cJSON_Delete(up);
            gw_send_unresolved(r);
            return reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
        }
        r->up = up;
        break;
    default: /* GW_SA_FORWARD */
        if (q.resend) {
            att = cJSON_GetObjectItemCaseSensitive(gw_journal_find(&G.journal, q.message_id), "attempts");
            if (cJSON_IsNumber(att) && att->valuedouble >= 0) {
                r->attempts_before = att->valuedouble;
            }
        }
        if (!send_forward(r, gw_send_upstream(&q, params))) {
            gw_send_release(r); /* it never left: the id is free to be tried again */
            return reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
        }
        break;
    }
    r->sid = s->id;
    r->rid = id->valuedouble;
    r->dup = false;
    s->inflight++;
    return true;
}

/* ---- Vision (ADR-019 amendment 1; ADR-006 amendment 1; gw_vision.h) ------
 *
 * On the connection to the shell's socket, which is held while some paired
 * client may read a capture or holds vision, the gateway calls six methods
 * of the Vision app and nothing else: vision.status, vision.share,
 * vision.mode, vision.set and vision.reset when a client with the scope
 * asks, and vision.pull for itself alone. No shell.* method and no
 * subscription: vision.state and vision.stats are made from the gateway's
 * own vision.status, asked once a second while a client holds the topic.
 *
 * One viewer. A client that asks for the preview while Vision is in front
 * gets it at once (the scope is the authorization); a second is told busy.
 * A picture is pulled only when the viewer has acknowledged the last one
 * and the interval has passed, is sent to the viewer's session alone, and
 * is held here only from the unit's answer to the moment it is queued: no
 * queue, no copy, nothing of it in a log.
 *
 * The viewer's scope and topic are looked up when it asks, when the unit's
 * answer comes, before every picture, and on every turn of the loop; a
 * preview whose viewer went away, gave the topic back, was revoked, lost
 * the scope or stopped acknowledging is ended here, and the unit is told. */

static bool vis_wanted(void)
{
    int i;

    if (G.identity_fault || G.state_fault) {
        return false;
    }
    for (i = 0; i < G.store.n; i++) {
        if ((G.store.c[i].scopes & GW_S_VISION) != 0) {
            return true;
        }
    }
    return false;
}

/* The connection is Vision's to keep: a client holds the scope, or the unit
 * has not yet answered that a preview is over. */
static bool vis_holds_link(void)
{
    return vis_wanted() || G.vis_offs > 0;
}

/* The shell is connected and has not said that it knows no vision.status. */
static bool vis_ready(void)
{
    return G.cap_up && G.vis_seen >= 0;
}

/* gateway.upstream for the clients that listen to Vision and not to a
 * capture (those are told by the capture's own event, of PacketShark). */
static void vis_upstream_event(bool connected)
{
    int i;

    for (i = 0; i < SESS_SLOTS; i++) {
        struct sess *s = &G.s[i];
        cJSON *d;

        if (s->st != S_OPEN || !s->hello || s->closing || s->who != GW_W_PAIRED || s->topics == 0) {
            continue;
        }
        s->topics = gw_topics_allowed(s->topics, sess_scopes(s));
        if ((s->topics & GW_T_VISION) == 0 || (s->topics & GW_T_CAPTURE) != 0) {
            continue;
        }
        d = cJSON_CreateObject();
        if (d != NULL) {
            cJSON_AddStringToObject(d, "service", "shell");
            cJSON_AddBoolToObject(d, "connected", connected);
            (void)sess_send(s, pocketipc_event("gateway.upstream", d));
        }
    }
}

/* vision.share {on: false}, the gateway's own: the unit is told that a
 * preview is over whenever it did not say so itself. */
static void vis_send_off(void)
{
    cJSON *p = cJSON_CreateObject();

    if (p == NULL || cJSON_AddBoolToObject(p, "on", false) == NULL || !gw_uplink_connected(G.shell)) {
        cJSON_Delete(p);
        return;
    }
    G.vis_order++;
    if (gw_uplink_call(G.shell, "vision.share", p, TAG_VIS_OFF, mono_ms(), GW_VISION_CALL_MS) == 0) {
        G.vis_offs++;
    }
}

/* The preview is over. `tell`: the unit does not know it yet. What clients
 * are told (vision.state) is said from the loop (vis_tick), never from
 * inside another session's handling. */
static void vis_end(const char *why, bool tell)
{
    char p[17];

    if (!G.vis.viewing) {
        return;
    }
    fp_prefix(G.vis.fp, p);
    LOG_INFO("vision: the preview of %s ended (%s)", p, why);
    gw_vision_viewer_end(&G.vis);
    if (tell) {
        vis_send_off();
    }
    G.vis_say = true;
    G.vis_poll_due = true;
}

static void vis_session_gone(uint64_t sid)
{
    if (gw_vision_view_of(&G.vis, sid) == GW_VV_SELF) {
        vis_end("its client went away", true);
    }
}

/* The viewer's session, if it is still one a picture may go to: there, that
 * client, paired, holding the scope and the topic. NULL with the reason. */
static struct sess *vis_viewer(const char **why)
{
    struct sess *s = sess_by_id(G.vis.sid);
    unsigned scopes;

    if (s == NULL || s->st != S_OPEN || s->closing || s->who != GW_W_PAIRED ||
        memcmp(s->fp, G.vis.fp, GW_FP_LEN) != 0) {
        *why = "its client went away";
        return NULL;
    }
    scopes = sess_scopes(s);
    if ((scopes & GW_S_VISION) == 0) {
        *why = "its client may no longer view";
        return NULL;
    }
    s->topics = gw_topics_allowed(s->topics, scopes);
    if ((s->topics & GW_T_VISION) == 0) {
        *why = "its client no longer listens";
        return NULL;
    }
    return s;
}

/* The shell answered "unknown method" to a vision.* name: this image has no
 * Vision that serves them. Not asked again on this connection, and its
 * methods are unavailable (error 9). */
static void vis_absent(void)
{
    if (G.vis_seen >= 0) {
        G.vis_seen = -1;
        LOG_INFO("the shell knows no vision.status: this image serves no Vision preview");
        vis_upstream_event(false);
        vis_end("the unit serves no Vision", false);
    }
}

static void vis_on_state(bool up)
{
    if (up) {
        G.vis_seen = 0;
        G.vis_polling = false;
        G.vis_pulling = false;
        G.vis_poll_due = true;
        G.vis_offs = 0;
        G.vis_share_tag = 0;
        vis_upstream_event(true);
        return;
    }
    /* Requests that waited were answered "upstream unavailable" before
     * this. Called while G.cap_up still says how it was. */
    if (vis_ready()) {
        if (G.vis_seen > 0) {
            LOG_INFO("Vision on the shell's socket is out of reach");
        }
        vis_upstream_event(false);
    }
    G.vis_seen = 0;
    G.vis_polling = false;
    G.vis_pulling = false;
    G.vis_offs = 0;
    G.vis_share_tag = 0;
    G.vis_biometric = false;
    vis_end("the shell went away", false);
    if (gw_vision_lost(&G.vis)) {
        G.vis_say = true;
    }
}

static void vis_say_state(void)
{
    if (G.vis_say) {
        G.vis_say = false;
        /* Empty: a client that is told asks vision.status, which answers
         * for that client (live, or busy). */
        mesh_emit(GW_T_VISION, "vision.state", cJSON_CreateObject(), false);
    }
}

/* One answer to the gateway's own vision.status. */
static void vis_polled(const cJSON *result, int code)
{
    struct gw_vision_seen seen;
    cJSON *stats = NULL;
    bool changed = false;
    char p[17];

    G.vis_polling = false;
    if (result == NULL) {
        if (code == POCKETIPC_ERR_UNKNOWN_METHOD) {
            vis_absent();
        }
        return; /* a shell that does not answer in time loses the connection (gw_uplink), which is made again */
    }
    if (G.vis_seen <= 0) {
        G.vis_seen = 1;
        LOG_INFO("Vision answers on the shell's socket");
    }
    gw_vision_step(&G.vis, result, &changed, &stats, &seen);
    if (changed) {
        G.vis_say = true;
    }
    G.vis_biometric = seen.available && seen.biometric;
    if (G.vis.viewing) {
        /* Only an answer asked for after the preview began speaks of it. */
        if (G.vis_poll_order > G.vis.began) {
            fp_prefix(G.vis.fp, p);
            if (!seen.live) {
                vis_end("the unit stopped sharing", false);
            } else if (seen.biometric) {
                vis_end("a mode that is never shared", true);
            } else if (seen.client[0] != '\0' && strcmp(seen.client, p) != 0) {
                vis_end("the unit shares with another", true);
            }
        }
    } else if (seen.live && G.vis_share_tag == 0 && G.vis_offs == 0) {
        /* The unit shares and the gateway has no viewer (it was started
         * again, or an answer was lost): nobody is there to see it. */
        LOG_INFO("vision: the unit shares and nobody views: it is told to stop");
        vis_send_off();
    }
    vis_say_state();
    if (stats != NULL) {
        /* Figures are replaced by the next: skipped for a client that is behind. */
        mesh_emit(GW_T_VISION, "vision.stats", stats, true);
    }
}

/* One answer to the gateway's own vision.pull. The picture in it, if it is
 * one, exists here from this call to the end of it. */
static void vis_pulled(const cJSON *result, int code, int64_t now)
{
    const char *why = NULL;
    struct sess *s;
    cJSON *data = NULL;
    double seq = 0;

    G.vis_pulling = false;
    if (!G.vis.viewing || G.vis_pull_for != G.vis.began) {
        return; /* asked for a preview that is over */
    }
    if (result == NULL) {
        if (code == POCKETIPC_ERR_UNKNOWN_METHOD) {
            vis_absent();
        } else if (code == POCKETIPC_ERR_POLICY) {
            /* STOP on the unit, another mode, or Vision was left. */
            vis_end("the unit stopped sharing", false);
        } else if (code == GW_ERR_UPSTREAM) {
            gw_vision_skipped(&G.vis, 0, now); /* no answer in time: asked again while the link holds */
        } else {
            vis_end("the unit refused a picture", true);
        }
        return;
    }
    s = vis_viewer(&why);
    if (s == NULL) {
        vis_end(why, true);
        return;
    }
    if (G.vis_biometric) {
        vis_end("a mode that is never shared", true);
        return;
    }
    switch (gw_vision_frame(result, G.vis.last_seq, &data, &seq)) {
    case GW_VF_NONE:
        gw_vision_none(&G.vis, now);
        return;
    case GW_VF_BAD:
        if (G.vis.bad++ == 0) {
            LOG_WARN("vision: an answer to vision.pull was not as the contract says and was dropped (said once)");
        }
        gw_vision_skipped(&G.vis, 0, now);
        return;
    default:
        break;
    }
    if (s->out_len - s->out_off > GW_EVENT_SOFT_MAX) {
        /* The viewer is behind on reading: this picture is not for it, and
         * nothing is kept for later. */
        cJSON_Delete(data);
        gw_vision_skipped(&G.vis, seq, now);
        return;
    }
    gw_vision_sent(&G.vis, seq, now);
    (void)sess_send(s, pocketipc_event("vision.frame", data));
}

/* An answer on the shell's socket that is Vision's. False when it is not. */
static bool vis_on_reply(uint64_t tag, const cJSON *result, int code)
{
    int64_t now = mono_ms();
    int i;

    if (tag == TAG_VIS_OFF) {
        if (G.vis_offs > 0) {
            G.vis_offs--;
        }
        return true;
    }
    if (tag == TAG_VIS_POLL) {
        vis_polled(result, code);
        return true;
    }
    if (tag == TAG_VIS_PULL) {
        vis_pulled(result, code, now);
        return true;
    }
    for (i = 0; i < GW_MESH_PENDING; i++) {
        struct mesh_req *q = &G.req[i];
        struct sess *s;
        cJSON *filtered = NULL;
        bool share;
        int err;

        if (!q->used || q->tag != tag) {
            continue;
        }
        if (q->m->kind != GW_K_VISION) {
            return false;
        }
        share = tag == G.vis_share_tag;
        err = result != NULL ? POCKETIPC_ERR_BACKEND : code;
        if (result == NULL && code == POCKETIPC_ERR_UNKNOWN_METHOD) {
            /* A client's own question was the first: no Vision here, which
             * to the client is "unavailable". */
            vis_absent();
            err = GW_ERR_UPSTREAM;
        }
        if (result != NULL && strcmp(q->m->name, "vision.status") == 0) {
            if (G.vis_seen <= 0) {
                G.vis_seen = 1;
                LOG_INFO("Vision answers on the shell's socket");
            }
            /* For the client that asked: live is the viewer's word alone. */
            filtered = gw_vision_status(result, gw_vision_view_of(&G.vis, q->sid));
        } else if (result != NULL) {
            filtered = gw_filter_result(q->m, result);
        }
        if (share) {
            const cJSON *st = cJSON_GetObjectItemCaseSensitive(filtered, "state");

            G.vis_share_tag = 0;
            if (cJSON_IsString(st) && strcmp(st->valuestring, "live") == 0) {
                unsigned scopes;

                /* The unit shares. Whether for this client, is looked up
                 * now, not remembered from when it asked. */
                s = sess_by_id(q->sid);
                scopes = s != NULL ? sess_scopes(s) : 0;
                if (s != NULL && s->st == S_OPEN && !s->closing && !G.vis_share_void && !G.vis.viewing &&
                    (scopes & GW_S_VISION) != 0 && (gw_topics_allowed(s->topics, scopes) & GW_T_VISION) != 0) {
                    char p[17];

                    gw_vision_viewer(&G.vis, s->fp, s->id, G.vis_share_order, now);
                    fp_prefix(s->fp, p);
                    LOG_INFO("vision: the preview of %s began", p);
                    G.vis_say = true;
                    G.vis_poll_due = true;
                } else {
                    vis_send_off();
                    cJSON_Delete(filtered);
                    filtered = NULL;
                    err = POCKETIPC_ERR_POLICY;
                }
            }
        }
        req_done(q, filtered, err);
        return true;
    }
    if (tag == G.vis_share_tag) {
        /* The session that asked is gone; the unit may share for it now. */
        G.vis_share_tag = 0;
        if (result != NULL) {
            vis_send_off();
        }
        return true;
    }
    return false;
}

static void vis_tick(int64_t now)
{
    const char *why = NULL;

    if (G.vis.viewing) {
        if (vis_viewer(&why) == NULL) {
            vis_end(why, true);
        } else if (gw_vision_ack_late(&G.vis, now)) {
            vis_end("no acknowledgement", true);
        }
    }
    /* Asked only while somebody is told the answer. Before the picture: the
     * unit answers in order, so the first picture of a preview is read
     * knowing the mode the unit was in when the preview began. */
    if (vis_ready() && !G.vis_polling && (topic_held(GW_T_VISION) || G.vis.viewing) &&
        (G.vis_poll_due || now - G.vis_poll_ms >= GW_VISION_POLL_MS)) {
        if (gw_uplink_call(G.shell, "vision.status", NULL, TAG_VIS_POLL, now, GW_VISION_CALL_MS) == 0) {
            G.vis_polling = true;
            G.vis_poll_order = ++G.vis_order;
        }
        G.vis_poll_ms = now;
        G.vis_poll_due = false;
    }
    /* The next picture: only for a viewer that has taken the last. */
    if (G.vis.viewing && vis_ready() && !G.vis_pulling && gw_vision_pull_due(&G.vis, now)) {
        cJSON *p = cJSON_CreateObject();

        if (p == NULL || cJSON_AddNumberToObject(p, "after_seq", G.vis.last_seq) == NULL) {
            cJSON_Delete(p);
            gw_vision_none(&G.vis, now);
        } else if (gw_uplink_call(G.shell, "vision.pull", p, TAG_VIS_PULL, now, GW_VISION_CALL_MS) == 0) {
            G.vis_pulling = true;
            G.vis_pull_for = G.vis.began;
        } else {
            gw_vision_none(&G.vis, now);
        }
    }
    vis_say_state();
}

/* How long the loop may sleep: its usual turn, or less when the next
 * picture may be asked for before that. */
static int vis_wait_ms(int64_t now)
{
    int64_t t = G.vis.next_ms - now;

    if (G.vis.viewing && !G.vis_pulling && !(G.vis.sent && G.vis.waiting) && t > 0 && t < LOOP_MS) {
        return (int)t;
    }
    return LOOP_MS;
}

/* vision.status, vision.share, vision.ack, vision.mode, vision.set,
 * vision.reset. The row has said who may call it; the parameters are
 * checked and written again before anything is sent; the unit's answer
 * comes later (vis_on_reply). */
static bool do_vision(struct sess *s, const cJSON *id, const struct gw_method *m, const cJSON *params, int idx)
{
    bool share = strcmp(m->name, "vision.share") == 0;
    int64_t now = mono_ms();
    struct mesh_req *q;
    cJSON *up = NULL, *r;
    char p[17];

    if (m->params != NULL ? m->params(params, &up) != 0 : !gw_params_empty(params)) {
        return reply_error(s, id, POCKETIPC_ERR_INVALID_PARAMS, "invalid params");
    }
    if (idx < 0) {
        cJSON_Delete(up);
        return reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
    }
    if (strcmp(m->name, "vision.ack") == 0) {
        /* The gateway's own: it reaches nothing. It counts from the viewer
         * and for the picture last sent; anything else is ignored. */
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(up, "seq");

        (void)gw_vision_ack(&G.vis, s->id, cJSON_IsNumber(seq) ? seq->valuedouble : -1);
        cJSON_Delete(up);
        return sess_send(s, pocketipc_response(id, NULL));
    }
    if (share && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(up, "on"))) {
        /* Off always succeeds. The unit is told only by the viewer's. */
        cJSON_Delete(up);
        if (G.vis_share_tag != 0 && G.vis_share_sid == s->id) {
            G.vis_share_void = true; /* its own request to view is still on its way */
        }
        if (gw_vision_view_of(&G.vis, s->id) == GW_VV_SELF) {
            vis_end("its client asked", true);
        }
        r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "state", "off");
        return sess_send(s, pocketipc_response(id, r));
    }
    /* How often, counted here: each of these wakes the Vision app on the
     * unit's one hart. Reading the state is under the general limit only. */
    if (strcmp(m->name, "vision.status") != 0 && !gw_vision_rate(&G.vis, s->fp, now)) {
        cJSON_Delete(up);
        return reply_error(s, id, GW_ERR_RATE, "rate limited");
    }
    if (!vis_ready()) {
        cJSON_Delete(up);
        return reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
    }
    if (share) {
        if (gw_vision_view_of(&G.vis, s->id) == GW_VV_SELF) {
            cJSON_Delete(up);
            r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "state", "live");
            return sess_send(s, pocketipc_response(id, r));
        }
        if (G.vis.viewing || G.vis_share_tag != 0) {
            cJSON_Delete(up);
            return reply_error(s, id, POCKETIPC_ERR_BUSY, "busy");
        }
        if ((s->topics & GW_T_VISION) == 0) {
            /* A picture is an event of the topic: without it there is
             * nobody to send one to. */
            cJSON_Delete(up);
            return reply_error(s, id, POCKETIPC_ERR_POLICY, "the topic is not held");
        }
        /* (Whether the unit's mode is one that may be shared is the unit's
         * to say, now: what the gateway last heard of it may be a second
         * old. The gateway's own check is on every picture, vis_pulled.) */
        /* Who views is this unit's own word for the client: the start of
         * its fingerprint, and the name it was given at pairing. */
        fp_prefix(s->fp, p);
        if (up == NULL || cJSON_AddStringToObject(up, "client", p) == NULL ||
            cJSON_AddStringToObject(up, "name", G.store.c[idx].name) == NULL) {
            cJSON_Delete(up);
            return reply_error(s, id, POCKETIPC_ERR_BACKEND, "upstream error");
        }
    }
    q = req_new(s, id, m);
    if (q == NULL) {
        cJSON_Delete(up);
        return reply_error(s, id, POCKETIPC_ERR_BUSY, "busy");
    }
    if (gw_uplink_call(G.shell, m->name, up, q->tag, now, GW_VISION_CALL_MS) != 0) {
        req_cancel(s, q);
        return reply_error(s, id, GW_ERR_UPSTREAM, "upstream unavailable");
    }
    if (share) {
        G.vis_share_tag = q->tag;
        G.vis_share_sid = s->id;
        G.vis_share_order = ++G.vis_order;
        G.vis_share_void = false;
    }
    return true;
}

/* The shell's socket serves two apps: each answer and each change of the
 * link goes to the part it belongs to. */
static void shell_on_reply(void *user, uint64_t tag, const cJSON *result, int code)
{
    if (!vis_on_reply(tag, result, code)) {
        cap_on_reply(user, tag, result, code);
    }
}

static void shell_on_state(void *user, bool up)
{
    if (!up) {
        vis_on_state(false);
    }
    cap_on_state(user, up);
    if (up && G.cap_up) {
        vis_on_state(true);
    }
}

/* ---- requests from clients ---------------------------------------------- */

static cJSON *hello_result(const struct sess *s, unsigned scopes)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *limits = cJSON_CreateObject();
    cJSON *caps = cJSON_CreateArray();
    char id[GW_FP_HEX + 1], host[65];
    int64_t w = wall_s();

    gw_hex(G.device_fp, GW_FP_LEN, id);
    if (gethostname(host, sizeof(host)) != 0) {
        host[0] = '\0';
    }
    host[sizeof(host) - 1] = '\0';
    cJSON_AddNumberToObject(r, "proto", GW_PROTO);
    cJSON_AddStringToObject(r, "device_id", id);
    cJSON_AddStringToObject(r, "name", host);
    cJSON_AddBoolToObject(r, "paired", s->who == GW_W_PAIRED);
    if (s->who != GW_W_PAIRED) {
        /* A pairing candidate learns who it is talking to and nothing else. */
        cJSON_Delete(limits);
        cJSON_Delete(caps);
        return r;
    }
    cJSON_AddStringToObject(r, "version", pocketlog_version());
    cJSON_AddStringToObject(r, "build", pocketlog_build_id());
    cJSON_AddItemToObject(r, "scopes", gw_scopes_json(scopes));
    /* What this build serves. A client shows a feature only when its
     * capability is here (and its scope allows it); capture arrives with
     * its milestone. */
    cJSON_AddItemToArray(caps, cJSON_CreateString("status"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("mesh"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("mesh.activity"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("mesh.send"));
    if ((scopes & GW_S_CAPTURE_VIEW) != 0) {
        /* Offered to a client that may read a capture, and to no other: the
         * answer to a client without the scope is what it always was.
         * Whether PacketShark answers now is gateway.subscribe's
         * upstream.shell and the gateway.upstream event. */
        cJSON_AddItemToArray(caps, cJSON_CreateString("capture"));
    }
    if ((scopes & GW_S_VISION) != 0) {
        /* The same rule: offered to a client that holds vision, and to no
         * other. Whether Vision is in front now is vision.status. */
        cJSON_AddItemToArray(caps, cJSON_CreateString("vision"));
    }
    cJSON_AddItemToObject(r, "capabilities", caps);
    cJSON_AddNumberToObject(r, "wall_ms", (double)w * 1000.0);
    cJSON_AddBoolToObject(r, "clock_set", w > WALL_SET_AFTER);
    cJSON_AddStringToObject(r, "boot_id", G.boot_id);
    cJSON_AddNumberToObject(limits, "frame_in", GW_FRAME_IN_MAX);
    cJSON_AddNumberToObject(limits, "frame_out", GW_FRAME_OUT_MAX);
    cJSON_AddNumberToObject(limits, "send_per_min", GW_SEND_PER_MIN);
    cJSON_AddItemToObject(r, "limits", limits);
    return r;
}

static bool int_param(const cJSON *params, const char *name, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, name);

    /* The range first: converting a number outside it is not defined. */
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= 65535) ||
        v->valuedouble != (double)(int)v->valuedouble) {
        return false;
    }
    *out = (int)v->valuedouble;
    return true;
}

static bool short_string(const cJSON *params, const char *name, size_t max)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, name);

    return cJSON_IsString(v) && strlen(v->valuestring) <= max;
}

static bool do_hello(struct sess *s, const cJSON *id, const cJSON *params, unsigned scopes)
{
    int lo, hi;

    if (s->hello) {
        return reply_fatal(s, id, POCKETIPC_ERR_POLICY, "gateway.hello was already sent");
    }
    if (params == NULL || !int_param(params, "proto_min", &lo) || !int_param(params, "proto_max", &hi) ||
        lo > hi || !short_string(params, "app", 64) || !short_string(params, "app_version", 32)) {
        return reply_fatal(s, id, POCKETIPC_ERR_INVALID_PARAMS, "invalid params");
    }
    if (GW_PROTO < lo || GW_PROTO > hi) {
        return reply_fatal(s, id, GW_ERR_NO_PROTO, "no common protocol version");
    }
    s->hello = true;
    s->deadline_ms = 0;
    return sess_send(s, pocketipc_response(id, hello_result(s, scopes)));
}

static bool do_pair(struct sess *s, const cJSON *id, const cJSON *params)
{
    const cJSON *tok = params != NULL ? cJSON_GetObjectItemCaseSensitive(params, "token") : NULL;
    const cJSON *name = params != NULL ? cJSON_GetObjectItemCaseSensitive(params, "name") : NULL;
    uint8_t token[GW_TOKEN_LEN];
    bool have = cJSON_IsString(tok) && gw_unb64url(tok->valuestring, token, sizeof(token)) == 0;
    enum gw_pair_token_result r;
    char fp[GW_FP_HEX + 1];
    cJSON *d;

    if (s->pair_waiting) {
        return reply_fatal(s, id, POCKETIPC_ERR_POLICY, "refused");
    }
    r = gw_pair_token(&G.pair, s->id, have ? token : NULL, have ? sizeof(token) : 0,
                      cJSON_IsString(name) ? name->valuestring : NULL, mono_ms());
    memset(token, 0, sizeof(token));
    if (r != GW_PAIR_TOKEN_OK) {
        LOG_WARN("pairing: a candidate from %s was refused", s->peer);
        if (!gw_pair_open(&G.pair)) {
            pairing_closed_event("failed");
        }
        /* One answer for every reason: a wrong token, a bad name, no window. */
        return reply_fatal(s, id, POCKETIPC_ERR_POLICY, "refused");
    }
    /* The token was right. Nobody is paired: a person decides, on the unit
     * (gateway.pair_decide on the local socket). The answer waits for that. */
    s->pair_waiting = true;
    s->pair_id = id->valuedouble;
    gw_hex(s->fp, GW_FP_LEN, fp);
    d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "name", G.pair.name);
    cJSON_AddStringToObject(d, "fingerprint", fp);
    ctl_event("gateway.candidate", d);
    LOG_INFO("pairing: a candidate passed; waiting for the decision on the unit");
    return true;
}

static bool do_unpair(struct sess *s, const cJSON *id, int idx)
{
    gw_store_remove(&G.store, idx);
    G.relisten = true; /* it may have been the last client */
    if (gw_store_save(&G.store) != 0) {
        G.store_dirty = true;
        LOG_ERROR("paired list could not be written");
        /* Out of memory's list either way; the client is told it is not
         * durable, and the connection ends. */
        return reply_fatal(s, id, POCKETIPC_ERR_BACKEND, "not durable");
    }
    LOG_INFO("a client removed itself");
    sess_end(s);
    return sess_send(s, pocketipc_response(id, NULL));
}

static bool do_upstream(struct sess *s, const cJSON *id, const struct gw_method *m, const cJSON *params)
{
    cJSON *res, *filtered;
    int code = 0;

    /* Every M2 upstream method takes no parameters, and nothing a client
     * sends is ever passed on as one. */
    if (!gw_params_empty(params)) {
        return reply_error(s, id, POCKETIPC_ERR_INVALID_PARAMS, "invalid params");
    }
    res = upstream_call(m, &code);
    if (res == NULL) {
        /* The upstream's own words stay on the unit. */
        return reply_error(s, id, code, code == GW_ERR_UPSTREAM ? "upstream unavailable" : "upstream error");
    }
    filtered = gw_filter_result(m, res);
    cJSON_Delete(res);
    if (filtered == NULL) {
        return reply_error(s, id, POCKETIPC_ERR_BACKEND, "upstream error");
    }
    return sess_send(s, pocketipc_response(id, filtered));
}

/* One frame from a client. False when the session was closed. */
static bool sess_request(struct sess *s, const uint8_t *p, size_t n)
{
    const cJSON *id = NULL, *params = NULL;
    const char *method = NULL;
    const struct gw_method *m;
    cJSON *root = NULL;
    unsigned scopes = 0;
    int idx = -1;
    bool alive;

    if (gw_request_parse(p, n, &root, &id, &method, &params) != 0) {
        sess_close(s, "malformed frame");
        return false;
    }
    if (s->who == GW_W_PAIRED) {
        /* Looked up on every request, not remembered from the handshake: a
         * revocation or a reduced scope holds from the next request. */
        idx = gw_store_find(&G.store, s->fp);
        if (idx < 0) {
            cJSON_Delete(root);
            sess_close(s, "no longer paired");
            return false;
        }
        scopes = G.store.c[idx].scopes;
    }
    m = gw_method_find(method);
    if (!gw_bucket_take(&s->rate, mono_ms())) {
        alive = reply_error(s, id, GW_ERR_RATE, "rate limited");
    } else if (!s->hello && (m == NULL || m->kind != GW_K_HELLO)) {
        alive = reply_fatal(s, id, POCKETIPC_ERR_POLICY, "gateway.hello first");
    } else {
        switch (gw_policy_check(m, s->who, scopes)) {
        case GW_V_UNKNOWN:
            alive = reply_error(s, id, POCKETIPC_ERR_UNKNOWN_METHOD, "unknown method");
            break;
        case GW_V_FORBIDDEN:
            alive = reply_error(s, id, GW_ERR_FORBIDDEN, "not permitted");
            break;
        default:
            switch (m->kind) {
            case GW_K_HELLO:
                alive = do_hello(s, id, params, scopes);
                break;
            case GW_K_PING:
                alive = sess_send(s, pocketipc_response(id, NULL));
                break;
            case GW_K_PAIR:
                alive = do_pair(s, id, params);
                break;
            case GW_K_UNPAIR:
                alive = do_unpair(s, id, idx);
                break;
            case GW_K_UPSTREAM:
                alive = do_upstream(s, id, m, params);
                break;
            case GW_K_SUBSCRIBE:
                alive = do_subscribe(s, id, params, scopes);
                break;
            case GW_K_MESH:
                alive = do_mesh(s, id, m, params);
                break;
            case GW_K_CAPTURE:
                alive = do_capture(s, id, m, params, idx);
                break;
            case GW_K_VISION:
                alive = do_vision(s, id, m, params, idx);
                break;
            case GW_K_NODES:
                alive = do_nodes(s, id, m, params);
                break;
            case GW_K_SYNC:
                alive = do_sync(s, id, params);
                break;
            case GW_K_SEND:
            case GW_K_RESEND:
                alive = do_send(s, id, m, params);
                break;
            default:
                alive = reply_error(s, id, POCKETIPC_ERR_UNKNOWN_METHOD, "unknown method");
                break;
            }
            break;
        }
    }
    cJSON_Delete(root);
    return alive;
}

static void sess_read(struct sess *s)
{
    int taken = 0;

    for (;;) {
        const uint8_t *body;
        size_t n, room;
        uint8_t *space;
        int r, err;

        if (s->closing) {
            return; /* nothing more is read from a connection that is ending */
        }
        if (s->out_len - s->out_off > GW_REQ_SOFT_MAX) {
            /* It has answers it has not taken yet: no further request is
             * read until they are written, so that pipelined requests for
             * large answers wait instead of running into the output limit. */
            sess_events(s, EPOLLOUT);
            return;
        }
        if (taken >= GW_READ_BATCH) {
            /* One session's share of a turn of the loop: the rest is read
             * in the next, after everybody else was served. */
            s->resume = true;
            G.resume_soon = true;
            return;
        }
        if (s->inflight >= GW_MAX_INFLIGHT) {
            /* docs/api/gateway.md §1: at most 8 at a time, and nothing
             * further is read until one completes. Off the read set
             * meanwhile, or the unread bytes would wake the loop for ever. */
            sess_events(s, s->out_off < s->out_len ? EPOLLOUT : 0);
            return;
        }
        r = gw_frame_next(&s->in, &body, &n);
        if (r < 0) {
            sess_close(s, "frame too large");
            return;
        }
        if (r == 1) {
            if (!sess_request(s, body, n)) {
                return;
            }
            gw_frame_consume(&s->in);
            taken++;
            continue;
        }
        space = gw_frame_space(&s->in, &room);
        ERR_clear_error();
        r = SSL_read(s->ssl, space, (int)room);
        if (r > 0) {
            gw_frame_added(&s->in, (size_t)r);
            s->last_rx_ms = mono_ms();
            continue;
        }
        err = SSL_get_error(s->ssl, r);
        if (err == SSL_ERROR_WANT_READ) {
            if ((s->events & EPOLLIN) == 0) {
                sess_events(s, s->events | EPOLLIN); /* it had been taken off above */
            }
            return;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            sess_events(s, EPOLLIN | EPOLLOUT);
            return;
        }
        sess_close(s, err == SSL_ERROR_ZERO_RETURN ? "peer closed" : "read failed");
        return;
    }
}

static void sess_ready(struct sess *s, uint32_t ev)
{
    uint64_t id = s->id;

    if (s->st == S_HANDSHAKE) {
        sess_handshake(s);
        if (s->st != S_OPEN || s->id != id) {
            return;
        }
    } else if ((ev & (EPOLLHUP | EPOLLERR)) != 0 && (ev & EPOLLIN) == 0) {
        sess_close(s, "connection lost");
        return;
    }
    if (s->closing) {
        /* Only its output matters now. */
        if ((ev & (EPOLLHUP | EPOLLERR)) != 0) {
            sess_close(s, "connection lost");
        } else if ((ev & EPOLLOUT) != 0) {
            (void)sess_flush(s);
        }
        return;
    }
    if ((ev & EPOLLOUT) != 0) {
        if (s->out_off < s->out_len) {
            if (!sess_flush(s)) {
                return;
            }
        } else {
            /* Asked for by a read that needed to write; the read below asks
             * again if it still does. */
            sess_events(s, EPOLLIN);
        }
    }
    if (s->st == S_OPEN && s->id == id && !s->closing) {
        sess_read(s);
    }
}

static void sessions_tick(int64_t now)
{
    bool was_open = gw_pair_open(&G.pair);
    uint64_t drop = gw_pair_tick(&G.pair, now);
    struct sess *s = sess_by_id(drop);
    int i;

    if (s != NULL) {
        if (s->pair_waiting) {
            cJSON *idn = cJSON_CreateNumber(s->pair_id);

            s->pair_waiting = false;
            (void)reply_fatal(s, idn, POCKETIPC_ERR_POLICY, "refused");
            cJSON_Delete(idn);
        } else {
            sess_close(s, "pairing timed out");
        }
    }
    if (was_open && !gw_pair_open(&G.pair)) {
        pairing_closed_event("expired");
        listener_update();
    }
    for (i = 0; i < SESS_SLOTS; i++) {
        s = &G.s[i];
        if (s->st == S_FREE) {
            continue;
        }
        if (s->deadline_ms != 0 && now >= s->deadline_ms) {
            sess_close(s, s->st == S_HANDSHAKE ? "handshake timed out"
                                              : s->closing ? "did not drain" : "no gateway.hello");
        } else if (s->st == S_OPEN && now - s->last_rx_ms >= GW_IDLE_MS) {
            sess_close(s, "idle");
        }
    }
}

/* ---- the local control surface (docs/api/gateway.md §10) ---------------- */

static void ctl_reply(struct pocketipc_client *c, const cJSON *id, cJSON *result)
{
    pocketipc_server_reply(G.ctl, c, pocketipc_response(id, result));
}

static void ctl_error(struct pocketipc_client *c, const cJSON *id, int code, const char *message)
{
    pocketipc_server_reply(G.ctl, c, pocketipc_error_response(id, code, message));
}

static bool client_connected(const uint8_t fp[GW_FP_LEN])
{
    int i;

    for (i = 0; i < SESS_SLOTS; i++) {
        if (G.s[i].st == S_OPEN && G.s[i].who == GW_W_PAIRED && gw_ct_eq(G.s[i].fp, fp, GW_FP_LEN)) {
            return true;
        }
    }
    return false;
}

static cJSON *status_json(void)
{
    cJSON *r = cJSON_CreateObject(), *mesh;
    uint32_t addr = wanted_addr();
    const char *why = closed_reason(addr);
    char text[INET_ADDRSTRLEN], id[GW_FP_HEX + 1];
    struct in_addr a;

    gw_hex(G.device_fp, GW_FP_LEN, id);
    cJSON_AddBoolToObject(r, "listening", G.lfd >= 0);
    if (G.lfd >= 0) {
        a.s_addr = G.laddr;
        cJSON_AddStringToObject(r, "address", inet_ntop(AF_INET, &a, text, sizeof(text)));
        cJSON_AddNumberToObject(r, "port", G.port);
    } else {
        cJSON_AddStringToObject(r, "closed", why != NULL ? why : "listen_failed");
    }
    cJSON_AddBoolToObject(r, "password_set", G.password_set);
    cJSON_AddBoolToObject(r, "identity_fault", G.identity_fault);
    cJSON_AddBoolToObject(r, "state_fault", G.state_fault);
    /* True while a revocation, a scope change or an unpairing is in force
     * in memory and not yet on storage. */
    cJSON_AddBoolToObject(r, "store_dirty", G.store_dirty);
    if (!G.identity_fault) {
        cJSON_AddStringToObject(r, "device_id", id);
    }
    cJSON_AddNumberToObject(r, "clients", G.store.n);
    cJSON_AddNumberToObject(r, "sessions", count_state(S_OPEN, true));
    cJSON_AddBoolToObject(r, "pairing", gw_pair_open(&G.pair));
    /* The window as the unit's own pairing page shows it (ADR-019
     * amendment 2, decision 6): its time, and the phone waiting for the
     * person's decision by the name it gave and its key's fingerprint - the
     * two values of the gateway.candidate event. Never the token. */
    if (gw_pair_open(&G.pair)) {
        cJSON *pr = cJSON_AddObjectToObject(r, "pair");
        int64_t left = G.pair.until_ms - mono_ms();

        cJSON_AddStringToObject(pr, "state", G.pair.state == GW_PAIR_WAITING     ? "waiting"
                                             : G.pair.state == GW_PAIR_CANDIDATE ? "candidate"
                                                                                 : "open");
        cJSON_AddNumberToObject(pr, "left_ms", (double)(left > 0 ? left : 0));
        if (G.pair.state == GW_PAIR_WAITING) {
            cJSON *cand = cJSON_AddObjectToObject(pr, "candidate");
            char fp[GW_FP_HEX + 1];

            gw_hex(G.pair.fp, GW_FP_LEN, fp);
            cJSON_AddStringToObject(cand, "name", G.pair.name);
            cJSON_AddStringToObject(cand, "fingerprint", fp);
        }
    } else if (G.pair_closed[0] != '\0') {
        cJSON_AddStringToObject(r, "pair_closed", G.pair_closed);
    }
    /* The mesh behind the gateway: counts and flags, nothing of a message. */
    mesh = cJSON_AddObjectToObject(r, "mesh");
    cJSON_AddBoolToObject(mesh, "wanted", mesh_wanted());
    cJSON_AddBoolToObject(mesh, "connected", G.mesh_ready);
    cJSON_AddNumberToObject(mesh, "journal", G.journal.count);
    cJSON_AddNumberToObject(mesh, "seq", (double)gw_journal_seq(&G.journal));
    return r;
}

static void url_name(const char *in, char *out, size_t cap)
{
    size_t o = 0;

    for (; *in != '\0' && o + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;

        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '.') {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, cap - o, "%%%02X", c);
        }
    }
    out[o] = '\0';
}

static void ctl_pair_begin(struct pocketipc_client *c, const cJSON *id)
{
    uint8_t token[GW_TOKEN_LEN];
    char t64[32], f64[64], host[65], name[200], addr[INET_ADDRSTRLEN], uri[512];
    struct in_addr a;
    cJSON *r;

    if (G.identity_fault || G.state_fault) {
        ctl_error(c, id, POCKETIPC_ERR_BACKEND, "gateway state fault");
        return;
    }
    if (!G.password_set) {
        ctl_error(c, id, POCKETIPC_ERR_POLICY, "password_required");
        return;
    }
    if (G.store.n >= GW_MAX_CLIENTS) {
        ctl_error(c, id, POCKETIPC_ERR_POLICY, "client list full");
        return;
    }
    if (gw_pair_open(&G.pair)) {
        ctl_error(c, id, POCKETIPC_ERR_BUSY, "a pairing window is open");
        return;
    }
    if (gw_tls_random(token, sizeof(token)) != 0 || gw_pair_begin(&G.pair, token, mono_ms()) != 0) {
        ctl_error(c, id, POCKETIPC_ERR_BACKEND, "no random source");
        return;
    }
    listener_update();
    if (G.lfd < 0) {
        const char *why = closed_reason(wanted_addr());

        (void)gw_pair_cancel(&G.pair);
        memset(token, 0, sizeof(token));
        listener_update();
        ctl_error(c, id, POCKETIPC_ERR_POLICY, why != NULL ? why : "listen_failed");
        return;
    }
    gw_b64url(token, sizeof(token), t64, sizeof(t64));
    memset(token, 0, sizeof(token));
    gw_b64url(G.device_fp, GW_FP_LEN, f64, sizeof(f64));
    if (gethostname(host, sizeof(host)) != 0) {
        host[0] = '\0';
    }
    host[sizeof(host) - 1] = '\0';
    url_name(host, name, sizeof(name));
    a.s_addr = G.laddr;
    snprintf(uri, sizeof(uri), "doors-pair:1?h=%s&p=%u&fp=%s&t=%s&n=%s",
             inet_ntop(AF_INET, &a, addr, sizeof(addr)), (unsigned)G.port, f64, t64, name);
    /* The URI holds the token. It goes to the local caller - the panel -
     * and nowhere else: not to the log, not to an event. */
    r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "uri", uri);
    cJSON_AddNumberToObject(r, "expires_in_ms", GW_PAIR_WINDOW_MS);
    ctl_reply(c, id, r);
    memset(uri, 0, sizeof(uri));
    memset(t64, 0, sizeof(t64));
    G.pair_closed[0] = '\0';
    LOG_INFO("pairing: window opened");
}

static void ctl_pair_decide(struct pocketipc_client *c, const cJSON *id, const cJSON *params)
{
    const cJSON *allow = cJSON_GetObjectItemCaseSensitive(params, "allow");
    const cJSON *sc = cJSON_GetObjectItemCaseSensitive(params, "scopes");
    unsigned scopes = GW_S_DEFAULT;
    uint64_t sid = 0;
    struct sess *s;
    cJSON *idn, *r;
    bool ok = false;
    int d;

    if (!cJSON_IsBool(allow) || (sc != NULL && gw_scopes_from_json(sc, &scopes) != 0)) {
        ctl_error(c, id, POCKETIPC_ERR_INVALID_PARAMS, "invalid params");
        return;
    }
    /* The key the person compared on the unit's screen (ADR-019 amendment
     * 2). When it is given, the decision is for that phone only: another one
     * waiting now - a window opened anew between two of the page's polls -
     * is decided nothing about, and stays waiting. */
    {
        const cJSON *fpj = cJSON_GetObjectItemCaseSensitive(params, "fingerprint");
        uint8_t want[GW_FP_LEN];

        if (fpj != NULL) {
            if (!cJSON_IsString(fpj) || gw_unhex(fpj->valuestring, want, GW_FP_LEN) != 0) {
                ctl_error(c, id, POCKETIPC_ERR_INVALID_PARAMS, "invalid params");
                return;
            }
            if (G.pair.state != GW_PAIR_WAITING || !gw_ct_eq(want, G.pair.fp, GW_FP_LEN)) {
                ctl_error(c, id, POCKETIPC_ERR_POLICY, "not the phone that was confirmed");
                return;
            }
        }
    }
    d = gw_pair_decide(&G.pair, cJSON_IsTrue(allow), mono_ms(), &sid);
    if (d < 0) {
        ctl_error(c, id, POCKETIPC_ERR_POLICY, "no candidate is waiting");
        return;
    }
    s = sess_by_id(sid);
    if (d == 1 && s != NULL && s->pair_waiting && gw_ct_eq(s->fp, G.pair.fp, GW_FP_LEN)) {
        int64_t w = wall_s();

        if (gw_store_add(&G.store, G.pair.fp, G.pair.name, scopes, w > WALL_SET_AFTER ? w : 0) == 0) {
            if (gw_store_save(&G.store) == 0) {
                ok = true;
            } else {
                /* Not on storage, so not paired: allowed means durable. The
                 * save may have got as far as the rename, so the list
                 * without this client is written again - now, or by the
                 * loop until it succeeds. */
                gw_store_remove(&G.store, G.store.n - 1);
                G.store_dirty = gw_store_save(&G.store) != 0;
                LOG_ERROR("paired list could not be written; pairing refused");
            }
        }
    }
    (void)gw_pair_cancel(&G.pair);
    if (s != NULL) {
        idn = cJSON_CreateNumber(s->pair_id);
        s->pair_waiting = false;
        sess_end(s); /* the client reconnects as a paired one */
        if (ok) {
            r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "paired", 1);
            cJSON_AddItemToObject(r, "scopes", gw_scopes_json(scopes));
            (void)sess_send(s, pocketipc_response(idn, r));
        } else {
            (void)reply_error(s, idn, POCKETIPC_ERR_POLICY, "refused");
        }
        cJSON_Delete(idn);
    }
    LOG_INFO("pairing: %s", ok ? "a client was paired" : "refused");
    pairing_closed_event(ok ? "paired" : "refused");
    listener_update();
    if (d == 1 && !ok) {
        ctl_error(c, id, POCKETIPC_ERR_BACKEND, "could not pair");
        return;
    }
    r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "paired", ok);
    ctl_reply(c, id, r);
}

/* The client a local request names by its full fingerprint, or -1. */
static int ctl_client(const cJSON *params, uint8_t fp[GW_FP_LEN])
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, "client");

    if (!cJSON_IsString(v) || gw_unhex(v->valuestring, fp, GW_FP_LEN) != 0) {
        return -1;
    }
    return gw_store_find(&G.store, fp);
}

static void ctl_clients(struct pocketipc_client *c, const cJSON *id)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "clients");
    int i;

    for (i = 0; i < G.store.n; i++) {
        cJSON *o = cJSON_CreateObject();
        char fp[GW_FP_HEX + 1];

        gw_hex(G.store.c[i].fp, GW_FP_LEN, fp);
        cJSON_AddStringToObject(o, "fingerprint", fp);
        cJSON_AddStringToObject(o, "name", G.store.c[i].name);
        cJSON_AddItemToObject(o, "scopes", gw_scopes_json(G.store.c[i].scopes));
        cJSON_AddNumberToObject(o, "paired_wall_s", (double)G.store.c[i].paired_wall_s);
        cJSON_AddBoolToObject(o, "connected", client_connected(G.store.c[i].fp));
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(r, "max", GW_MAX_CLIENTS);
    ctl_reply(c, id, r);
}

static void ctl_revoke(struct pocketipc_client *c, const cJSON *id, const cJSON *params)
{
    uint8_t fp[GW_FP_LEN];
    int idx = ctl_client(params, fp), i, saved;

    if (idx < 0) {
        ctl_error(c, id, POCKETIPC_ERR_INVALID_PARAMS, "no such client");
        return;
    }
    /* Out of the list first, then its sessions, then the storage: whatever
     * the storage says, that key opens nothing from this line on. */
    gw_store_remove(&G.store, idx);
    for (i = 0; i < SESS_SLOTS; i++) {
        struct sess *s = &G.s[i];

        if (s->st != S_FREE && s->who == GW_W_PAIRED && gw_ct_eq(s->fp, fp, GW_FP_LEN)) {
            uint64_t sid = s->id;

            if (s->st == S_OPEN && s->hello && !s->closing) {
                sess_end(s);
                (void)sess_send(s, pocketipc_event("gateway.revoked", NULL));
            }
            /* Rule 23: closed at once. The notice is sent if the socket
             * takes it now; a revoked client is not waited for. */
            if (s->st != S_FREE && s->id == sid) {
                sess_close(s, "revoked");
            }
        }
    }
    saved = gw_store_save(&G.store);
    if (saved != 0) {
        G.store_dirty = true; /* retried by the loop until it is on storage */
    }
    LOG_INFO("a client was revoked%s", saved == 0 ? "" : " (NOT durable yet)");
    listener_update();
    if (saved != 0) {
        ctl_error(c, id, POCKETIPC_ERR_BACKEND, "revoked, but not durable");
        return;
    }
    ctl_reply(c, id, NULL);
}

/* The paired client at idx holds these scopes from now: in memory first,
 * with what follows for its connection, then on storage. */
static void ctl_scopes_apply(struct pocketipc_client *c, const cJSON *id, int idx, unsigned scopes)
{
    uint8_t fp[GW_FP_LEN];
    cJSON *d;
    int i;

    memcpy(fp, G.store.c[idx].fp, GW_FP_LEN);
    G.store.c[idx].scopes = scopes;
    if ((scopes & GW_S_VISION) == 0 && G.vis.viewing && gw_ct_eq(G.vis.fp, fp, GW_FP_LEN)) {
        /* The viewer lost vision: its preview ends with this line, not with
         * the next picture. */
        vis_end("its client may no longer view", true);
    }
    for (i = 0; i < SESS_SLOTS; i++) {
        struct sess *s = &G.s[i];

        if (s->st == S_OPEN && s->hello && s->who == GW_W_PAIRED && gw_ct_eq(s->fp, fp, GW_FP_LEN)) {
            /* A topic the new scopes do not allow is no longer held. */
            s->topics = gw_topics_allowed(s->topics, scopes);
            d = cJSON_CreateObject();
            cJSON_AddItemToObject(d, "scopes", gw_scopes_json(scopes));
            (void)sess_send(s, pocketipc_event("gateway.scopes", d));
        }
    }
    if (gw_store_save(&G.store) != 0) {
        G.store_dirty = true;
        ctl_error(c, id, POCKETIPC_ERR_BACKEND, "changed, but not durable");
        return;
    }
    LOG_INFO("a client's scopes were changed");
    d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "scopes", gw_scopes_json(scopes));
    ctl_reply(c, id, d);
}

static void ctl_set_scopes(struct pocketipc_client *c, const cJSON *id, const cJSON *params)
{
    uint8_t fp[GW_FP_LEN];
    int idx = ctl_client(params, fp);
    unsigned scopes = 0;

    if (idx < 0 || gw_scopes_from_json(cJSON_GetObjectItemCaseSensitive(params, "scopes"), &scopes) != 0) {
        ctl_error(c, id, POCKETIPC_ERR_INVALID_PARAMS, "invalid params");
        return;
    }
    ctl_scopes_apply(c, id, idx, scopes);
}

/* The client a local request names by its fingerprint or by the start of
 * it: 8 to 64 hex digits that begin exactly one paired client's. -1 for
 * none, and for more than one. */
static int ctl_client_prefix(const cJSON *params)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, "client");
    char hex[GW_FP_HEX + 1], want[GW_FP_HEX + 1];
    size_t n, k;
    int i, found = -1;

    if (!cJSON_IsString(v)) {
        return -1;
    }
    n = strlen(v->valuestring);
    if (n < 8 || n > GW_FP_HEX) {
        return -1;
    }
    for (k = 0; k < n; k++) {
        char ch = v->valuestring[k];

        if (ch >= 'A' && ch <= 'F') {
            ch = (char)(ch - 'A' + 'a');
        }
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return -1;
        }
        want[k] = ch;
    }
    for (i = 0; i < G.store.n; i++) {
        gw_hex(G.store.c[i].fp, GW_FP_LEN, hex);
        if (memcmp(hex, want, n) == 0) {
            if (found >= 0) {
                return -1;
            }
            found = i;
        }
    }
    return found;
}

/* gateway.scope {client, scope, on}: one scope of one paired client, given
 * or taken. Strings and a boolean, so that the unit's own command line and
 * a switch in Settings can send it (gateway.set_scopes takes a list). What
 * results must be a set a client may hold. */
static void ctl_scope(struct pocketipc_client *c, const cJSON *id, const cJSON *params)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "scope");
    const cJSON *on = cJSON_GetObjectItemCaseSensitive(params, "on");
    const cJSON *el;
    int idx = ctl_client_prefix(params);
    unsigned bit = cJSON_IsString(name) ? gw_scope_parse(name->valuestring) : 0;
    unsigned scopes;

    cJSON_ArrayForEach(el, params) {
        if (el->string == NULL || (strcmp(el->string, "client") != 0 && strcmp(el->string, "scope") != 0 &&
                                   strcmp(el->string, "on") != 0)) {
            idx = -1;
        }
    }
    if (idx < 0 || bit == 0 || !cJSON_IsBool(on)) {
        ctl_error(c, id, POCKETIPC_ERR_INVALID_PARAMS, "invalid params");
        return;
    }
    scopes = cJSON_IsTrue(on) ? (G.store.c[idx].scopes | bit) : (G.store.c[idx].scopes & ~bit);
    if (!gw_scopes_valid(scopes)) {
        /* mesh_send without mesh_read, or capture_control without
         * capture_view: the other one first. */
        ctl_error(c, id, POCKETIPC_ERR_INVALID_PARAMS, "not a set a client may hold");
        return;
    }
    ctl_scopes_apply(c, id, idx, scopes);
}

static void ctl_handler(struct pocketipc_server *srv, struct pocketipc_client *c, cJSON *req, void *user)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(req, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    const char *method = cJSON_IsString(m) ? m->valuestring : "";
    cJSON *r;

    (void)srv;
    (void)user;
    if (strcmp(method, "gateway.info") == 0) {
        r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "service", "gatewayd");
        cJSON_AddNumberToObject(r, "api_version", GW_API_VERSION);
        cJSON_AddNumberToObject(r, "proto", GW_PROTO);
        cJSON_AddStringToObject(r, "version", pocketlog_version());
        cJSON_AddStringToObject(r, "build", pocketlog_build_id());
        ctl_reply(c, id, r);
    } else if (strcmp(method, "gateway.status") == 0) {
        ctl_reply(c, id, status_json());
    } else if (strcmp(method, "gateway.pair_begin") == 0) {
        ctl_pair_begin(c, id);
    } else if (strcmp(method, "gateway.pair_cancel") == 0) {
        bool was = gw_pair_open(&G.pair);
        struct sess *s = sess_by_id(gw_pair_cancel(&G.pair));

        if (s != NULL) {
            sess_close(s, "pairing cancelled");
        }
        if (was) {
            pairing_closed_event("cancelled");
        }
        listener_update();
        ctl_reply(c, id, NULL);
    } else if (strcmp(method, "gateway.pair_decide") == 0) {
        ctl_pair_decide(c, id, params);
    } else if (strcmp(method, "gateway.clients") == 0) {
        ctl_clients(c, id);
    } else if (strcmp(method, "gateway.revoke") == 0) {
        ctl_revoke(c, id, params);
    } else if (strcmp(method, "gateway.set_scopes") == 0) {
        ctl_set_scopes(c, id, params);
    } else if (strcmp(method, "gateway.scope") == 0) {
        ctl_scope(c, id, params);
    } else if (strcmp(method, "gateway.subscribe") == 0 || strcmp(method, "gateway.unsubscribe") == 0) {
        pocketipc_client_set_subscribed(c, method[8] == 's');
        r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "subscribed", pocketipc_client_subscribed(c));
        ctl_reply(c, id, r);
    } else {
        ctl_error(c, id, POCKETIPC_ERR_UNKNOWN_METHOD, "unknown method");
    }
}

/* ---- start and stop ----------------------------------------------------- */

static void read_boot_id(void)
{
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "re");
    uint8_t rnd[16];

    G.boot_id[0] = '\0';
    if (f != NULL) {
        if (fgets(G.boot_id, sizeof(G.boot_id), f) != NULL) {
            G.boot_id[strcspn(G.boot_id, "\n")] = '\0';
        }
        fclose(f);
    }
    if (G.boot_id[0] == '\0' && gw_tls_random(rnd, sizeof(rnd)) == 0) {
        gw_hex(rnd, sizeof(rnd), G.boot_id);
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--interface NAME]\n"
            "The Companion Gateway (docs/services/GATEWAYD.md). Listens on TCP %d on the\n"
            "interface's address, only while a client is paired or a pairing window is open.\n",
            argv0, GW_PORT);
}

int main(int argc, char **argv)
{
    char lock_path[GW_STORE_PATH_MAX];
    static const struct gw_uplink_cb mesh_cb = {mesh_on_state, mesh_on_event, mesh_on_reply};
    static const struct gw_uplink_cb shell_cb = {shell_on_state, cap_on_event, shell_on_reply};
    struct epoll_event evs[SESS_SLOTS + 3];
    enum gw_tls_identity_result idr;
    struct sigaction sa;
    uint8_t jrnd[8];
    char jid[17];
    int i, lock_fd;

    memset(&G, 0, sizeof(G));
    G.lfd = -1;
    G.port = GW_PORT;
    G.iface = "wlan0";
    G.shadow = GW_GATE_SHADOW;
    G.passwd = GW_GATE_PASSWD;
    G.up[0].service = "sysd";
    G.up[1].service = "netd";
    G.up[2].service = "radiod";
    G.up[3].service = "meshcored";
    for (i = 0; i < 4; i++) {
        G.up[i].fd = -1;
    }
    for (i = 0; i < SESS_SLOTS; i++) {
        G.s[i].fd = -1;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--interface") == 0 && i + 1 < argc) {
            G.iface = argv[++i];
#ifdef GATEWAYD_TEST_HOOKS
        /* The host suite's handles: an address that is not an interface's,
         * a port two instances can share a machine on, a shadow file that
         * is not the machine's. The shipped binary has none of them
         * (tests/gatewayd_lint.sh checks its strings). */
        } else if (strcmp(argv[i], "--test-listen") == 0 && i + 1 < argc) {
            G.listen_override = argv[++i];
        } else if (strcmp(argv[i], "--test-port") == 0 && i + 1 < argc) {
            G.port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--test-shadow") == 0 && i + 1 < argc) {
            G.shadow = argv[++i];
        } else if (strcmp(argv[i], "--test-passwd") == 0 && i + 1 < argc) {
            G.passwd = argv[++i];
#endif
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    pocketlog_init("gatewayd");
    (void)pocketlog_install_crash_handler();
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    /* Under contention the shell wins the one hart (THREAT_MODEL T4). */
    (void)setpriority(PRIO_PROCESS, 0, 5);
    /* Rule 8 is a condition of running at all, not a best effort. */
    if (!drop_capabilities()) {
        LOG_ERROR("capabilities could not be dropped; not starting");
        return 1;
    }

    if (gw_tls_init() != 0) {
        LOG_ERROR("OpenSSL 3.0 or later with TLS 1.3 is required");
        return 1;
    }
    /* One gatewayd per unit: two would share one key and one port. */
    (void)pocketos_mkdir_p(pocketos_runtime_dir(), 0770);
    snprintf(lock_path, sizeof(lock_path), "%s/gatewayd.lock", pocketos_runtime_dir());
    lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        LOG_ERROR("another gatewayd is running, or %s cannot be locked", lock_path);
        return 1;
    }

    (void)pocketos_mkdir_p(pocketos_state_dir(), 0755);
    if (snprintf(G.state_dir, sizeof(G.state_dir), "%s/gatewayd", pocketos_state_dir()) >=
            (int)sizeof(G.state_dir) ||
        gw_store_dir(G.state_dir) != 0) {
        LOG_ERROR("state directory %s is not usable", G.state_dir);
        return 1;
    }
    gw_pair_init(&G.pair);
    if (gw_store_load(&G.store, G.state_dir) != 0) {
        /* Not read as an empty list: nobody is let in, nothing is rewritten. */
        LOG_ERROR("paired list is damaged; the gateway stays closed (state_fault)");
        G.state_fault = true;
        memcpy(G.store.dir, G.state_dir, strlen(G.state_dir) + 1);
    }
    /* An identity is created only where there is no state at all. A paired
     * list without its key means the key was lost: every phone pinned it,
     * and a new one would be a different unit to all of them (rule 16). */
    idr = gw_tls_identity(G.state_dir, !gw_store_exists(G.state_dir), &G.key);
    if (idr == GW_TLS_ID_FAULT || gw_tls_fingerprint(G.key, G.device_fp) != 0) {
        LOG_ERROR("identity is missing or unusable; the gateway stays closed (identity_fault)");
        G.identity_fault = true;
    } else {
        if (idr == GW_TLS_ID_CREATED) {
            LOG_INFO("identity created");
        }
        G.ctx = gw_tls_ctx(G.key, admit, NULL);
        if (G.ctx == NULL) {
            LOG_ERROR("TLS could not be configured as required; the gateway stays closed");
            G.identity_fault = true;
        }
    }
    G.ssl_idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    read_boot_id();

    G.epfd = epoll_create1(EPOLL_CLOEXEC);
    G.ctl = pocketipc_server_new("gatewayd", ctl_handler, NULL);
    if (G.epfd < 0 || G.ctl == NULL || G.ssl_idx < 0) {
        LOG_ERROR("could not start: %s", strerror(errno));
        return 1;
    }
    /* The journal is this run's: a new journal_id tells a client that the
     * cursor it kept means nothing any more. */
    gw_feed_init(&G.feed);
    gw_send_init(&G.send);
    G.mesh = gw_uplink_new("meshcored", &mesh_cb, NULL);
    gw_capture_init(&G.cap);
    gw_vision_init(&G.vis);
    G.shell = gw_uplink_new("shell", &shell_cb, NULL);
    if (gw_tls_random(jrnd, sizeof(jrnd)) != 0) {
        LOG_ERROR("no random source");
        return 1;
    }
    gw_hex(jrnd, sizeof(jrnd), jid);
    if (G.mesh == NULL || G.shell == NULL || gw_journal_init(&G.journal, jid) != 0) {
        LOG_ERROR("could not start: out of memory");
        return 1;
    }
    gw_bucket_init(&G.handshakes, GW_HANDSHAKES_PER_S, GW_HANDSHAKES_PER_S, mono_ms());
    G.password_set = gw_gate_password_set(G.passwd, G.shadow);
    LOG_INFO("started: %d paired client(s), device password %s", G.store.n, G.password_set ? "set" : "NOT set");
    listener_update();

    while (!g_stop) {
        int64_t now;
        int ready = 0, n;

        (void)pocketipc_server_poll_fd(G.ctl, G.resume_soon ? 0 : vis_wait_ms(mono_ms()), G.epfd, &ready);
        G.resume_soon = false;
        n = ready ? epoll_wait(G.epfd, evs, SESS_SLOTS + 3, 0) : 0;
        for (i = 0; i < n; i++) {
            if (evs[i].data.u64 == 0) {
                if (G.lfd >= 0) {
                    listener_accept();
                }
            } else if (evs[i].data.u64 == MESH_EPOLL_ID) {
                if (gw_uplink_connected(G.mesh)) {
                    gw_uplink_io(G.mesh, mono_ms());
                }
            } else if (evs[i].data.u64 == SHELL_EPOLL_ID) {
                if (gw_uplink_connected(G.shell)) {
                    gw_uplink_io(G.shell, mono_ms());
                }
            } else {
                struct sess *s = sess_by_id(evs[i].data.u64);

                if (s != NULL) {
                    sess_ready(s, evs[i].events);
                }
            }
        }
        now = mono_ms();
        mesh_tick(now);
        cap_tick(now);
        vis_tick(now);
        /* A session that was at its limit of waiting requests and had one
         * answered reads on. Here and not where the answer was sent: never
         * from inside another session's handling. */
        for (i = 0; i < SESS_SLOTS; i++) {
            struct sess *s = &G.s[i];

            if (s->st == S_OPEN && s->resume) {
                s->resume = false;
                if (!s->closing) {
                    sess_read(s);
                }
            }
        }
        sessions_tick(now);
        if (G.relisten) {
            G.relisten = false;
            listener_update();
        }
        if (now - G.recheck_ms >= RECHECK_MS) {
            /* The password can be removed and the address can change while
             * the service runs; neither sends a signal. */
            G.recheck_ms = now;
            if (G.store_dirty && !G.state_fault && gw_store_save(&G.store) == 0) {
                G.store_dirty = false;
                LOG_INFO("paired list written after an earlier failure");
            }
            G.password_set = gw_gate_password_set(G.passwd, G.shadow);
            listener_update();
        }
    }

    LOG_INFO("stopping");
    listener_close("stopping");
    close_all_sessions("stopping");
    (void)gw_pair_cancel(&G.pair);
    gw_uplink_free(G.mesh);
    if (G.cap.asking && gw_uplink_connected(G.shell) && cap_send_cancel(G.cap.token, NULL, TAG_CAP_CANCEL)) {
        /* Stopping: no sheet is left asking for a gateway that is gone. The
         * unit's answer is waited for, a moment at most, so that the cancel
         * has left before the socket is closed. */
        int64_t until = mono_ms() + CAP_STOP_FLUSH_MS;

        while (G.cap_say_n > 0 && gw_uplink_connected(G.shell) && mono_ms() < until) {
            struct pollfd pf = {gw_uplink_fd(G.shell), POLLIN, 0};

            (void)poll(&pf, 1, 20);
            gw_uplink_io(G.shell, mono_ms());
            gw_uplink_tick(G.shell, mono_ms());
        }
    }
    if (G.vis_offs > 0 && gw_uplink_connected(G.shell)) {
        /* Stopping: the viewer's session was closed above, and with that
         * the unit was told that its preview is over. Its answer is waited
         * for, a moment at most, so that the word has left before the
         * socket is closed. */
        int64_t until = mono_ms() + CAP_STOP_FLUSH_MS;

        while (G.vis_offs > 0 && gw_uplink_connected(G.shell) && mono_ms() < until) {
            struct pollfd pf = {gw_uplink_fd(G.shell), POLLIN, 0};

            (void)poll(&pf, 1, 20);
            gw_uplink_io(G.shell, mono_ms());
            gw_uplink_tick(G.shell, mono_ms());
        }
    }
    gw_uplink_free(G.shell);
    gw_journal_free(&G.journal);
    gw_send_free(&G.send);
    gw_feed_reset(&G.feed);
    nodes_forget();
    pocketipc_server_free(G.ctl);
    for (i = 0; i < 4; i++) {
        if (G.up[i].fd >= 0) {
            close(G.up[i].fd);
        }
    }
    SSL_CTX_free(G.ctx);
    EVP_PKEY_free(G.key);
    close(G.epfd);
    close(lock_fd);
    pocketlog_close();
    return 0;
}
