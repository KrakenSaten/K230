/*
 * companiond: a MeshCore companion-protocol endpoint on the local network,
 * for one MeshCore app, in front of meshcored (docs/services/COMPANIOND.md).
 *
 * The sockets live here and nothing else: the protocol is cmp_wire.c and
 * the translation cmp_bridge.c. One loop, poll() on at most three
 * descriptors, nothing that blocks for longer than pocketipc's documented
 * 200 ms write window:
 *
 *   - the listening TCP socket, bound to the one address the operator named;
 *   - the one app connection. A second connection from an allowed address
 *     replaces it, as upstream's SerialWifiInterface does - a phone that
 *     changed networks leaves a dead socket behind, and refusing the new one
 *     would lock its owner out until that one timed out;
 *   - one pocketipc connection to meshcored, subscribed, read continuously
 *     so meshcored never has to drop this client for not reading
 *     (docs/api/pocketipc.md, "Backpressure"). A subscriber only receives:
 *     whatever RIFT is sent, RIFT is still sent.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE /* accept4 */

#include "cmp_bridge.h"
#include "cmp_wire.h"

#include "pocketipc/pocketipc.h"
#include "pocketlog/pocketlog.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SERVICE "meshcored"
#define MAX_ALLOW 8
/* Output held for the app. A contact listing is written as it drains, so
 * this holds a handful of frames in practice; an app that stops reading
 * long enough to fill it, or makes no progress for OUT_STALL_MS, is
 * dropped rather than allowed to hold memory or the loop. */
#define OUT_CAP (64 * 1024)
#define OUT_LOW (4 * 1024)
#define OUT_STALL_MS 10000
#define IN_CAP 4096
#define MESH_CONNECT_TIMEOUT_MS 200
#define MESH_BACKOFF_MIN_MS 500
#define MESH_BACKOFF_MAX_MS 30000

struct cidr {
    uint32_t net, mask;
};

struct app {
    int fd;
    char peer[INET_ADDRSTRLEN];
    struct cmp_decoder dec;
    uint8_t in[IN_CAP];
    size_t in_len, in_at;
    uint8_t *out;
    size_t out_len;
    uint64_t out_progress_ms;
    bool drop;
};

struct ctx {
    struct cmp_bridge bridge;
    struct app app;
    int mesh_fd;
    struct pocketipc_reader reader;
    uint64_t mesh_next_ms;
    int mesh_backoff_ms;
    struct cidr allow[MAX_ALLOW];
    int n_allow;
};

static volatile sig_atomic_t stopping;

static void on_signal(int sig)
{
    (void)sig;
    stopping = 1;
}

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t io_mono(void *user)
{
    (void)user;
    return now_ms();
}

static uint32_t io_wall(void *user)
{
    (void)user;
    return (uint32_t)time(NULL);
}

static int io_to_app(void *user, const uint8_t *p, size_t len)
{
    struct ctx *c = user;
    struct app *a = &c->app;
    size_t n;

    if (a->fd < 0 || a->drop) {
        return -1;
    }
    if (a->out_len + len + CMP_HDR_LEN > OUT_CAP) {
        LOG_WARN("app %s is not reading (%zu bytes waiting): dropping it", a->peer, a->out_len);
        a->drop = true;
        return -1;
    }
    n = cmp_frame_encode(p, len, a->out + a->out_len, OUT_CAP - a->out_len);
    if (n == 0) {
        return -1;
    }
    if (a->out_len == 0) {
        a->out_progress_ms = now_ms();
    }
    a->out_len += n;
    return 0;
}

static int io_mesh_call(void *user, uint32_t id, const char *method, cJSON *params)
{
    struct ctx *c = user;
    cJSON *req = cJSON_CreateObject();
    int rc;

    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", method);
    if (params) {
        cJSON_AddItemToObject(req, "params", params);
    }
    rc = c->mesh_fd >= 0 ? pocketipc_send(c->mesh_fd, req) : -1;
    cJSON_Delete(req);
    return rc;
}

/* ---- addresses ------------------------------------------------------------ */

static int parse_cidr(const char *s, struct cidr *out)
{
    char buf[32];
    char *slash;
    int bits = 32;
    struct in_addr a;

    if (strlen(s) >= sizeof(buf)) {
        return -1;
    }
    strcpy(buf, s);
    slash = strchr(buf, '/');
    if (slash) {
        char *end;
        long v;

        *slash = '\0';
        v = strtol(slash + 1, &end, 10);
        if (*end || v < 8 || v > 32) {
            return -1;
        }
        bits = (int)v;
    }
    if (inet_pton(AF_INET, buf, &a) != 1) {
        return -1;
    }
    out->mask = bits == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> bits);
    out->net = ntohl(a.s_addr) & out->mask;
    return 0;
}

/* Loopback, RFC 1918 and link-local: the only sources a local-network
 * service answers, whatever the allow list says, so a port forward or a
 * mistyped list cannot put it on the internet. */
static bool local_address(uint32_t ip)
{
    return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip & 0xFFF00000u) == 0xAC100000u ||
           (ip & 0xFFFF0000u) == 0xC0A80000u || (ip & 0xFFFF0000u) == 0xA9FE0000u;
}

static bool allowed(const struct ctx *c, uint32_t ip)
{
    int i;

    if (!local_address(ip)) {
        return false;
    }
    for (i = 0; i < c->n_allow; i++) {
        if ((ip & c->allow[i].mask) == c->allow[i].net) {
            return true;
        }
    }
    return false;
}

static int listen_on(const char *spec)
{
    char host[INET_ADDRSTRLEN];
    const char *colon = strrchr(spec, ':');
    struct sockaddr_in sa;
    char *end;
    long port;
    int fd;
    int one = 1;

    if (!colon || (size_t)(colon - spec) >= sizeof(host)) {
        return -1;
    }
    memcpy(host, spec, (size_t)(colon - spec));
    host[colon - spec] = '\0';
    port = strtol(colon + 1, &end, 10);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    if (*end || port < 1 || port > 65535 || inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        return -1;
    }
    /* A named address, never the wildcard: the service is on the one
     * network the operator chose, not on every interface the unit has or
     * will get. And never a public one. */
    if (sa.sin_addr.s_addr == htonl(INADDR_ANY) || !local_address(ntohl(sa.sin_addr.s_addr))) {
        errno = EINVAL;
        return -2;
    }
    sa.sin_port = htons((uint16_t)port);
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 2) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- the app -------------------------------------------------------------- */

static void app_close(struct ctx *c, const char *why)
{
    struct app *a = &c->app;

    if (a->fd < 0) {
        return;
    }
    LOG_INFO("app %s gone: %s (frames %u, skipped %u wrong type, %u oversized)", a->peer, why,
             a->dec.frames, a->dec.skipped_type, a->dec.skipped_size);
    close(a->fd);
    a->fd = -1;
    a->out_len = 0;
    a->in_len = a->in_at = 0;
    a->drop = false;
    cmp_bridge_app_down(&c->bridge);
}

static void app_accept(struct ctx *c, int lfd)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int fd = accept4(lfd, (struct sockaddr *)&sa, &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
    char peer[INET_ADDRSTRLEN];
    int one = 1;
    int idle = 60, intvl = 10, cnt = 3;

    if (fd < 0) {
        return;
    }
    inet_ntop(AF_INET, &sa.sin_addr, peer, sizeof(peer));
    if (sa.sin_family != AF_INET || !allowed(c, ntohl(sa.sin_addr.s_addr))) {
        LOG_WARN("refused a connection from %s: not in the allowed addresses", peer);
        close(fd);
        return;
    }
    if (c->app.fd >= 0) {
        app_close(c, "replaced by a new connection");
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* A phone that walks out of range sends no FIN. */
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    c->app.fd = fd;
    snprintf(c->app.peer, sizeof(c->app.peer), "%s", peer);
    cmp_decoder_init(&c->app.dec);
    c->app.in_len = c->app.in_at = 0;
    c->app.out_len = 0;
    c->app.drop = false;
    LOG_INFO("app connected from %s", peer);
    cmp_bridge_app_up(&c->bridge);
}

/* Hand over buffered bytes, one command at a time, while the bridge is not
 * waiting for meshcored. What it does not take yet stays here; while this
 * buffer is full the socket is not read, and TCP holds the rest. */
static void app_consume(struct ctx *c)
{
    struct app *a = &c->app;

    while (a->fd >= 0 && !a->drop && a->in_at < a->in_len && !cmp_bridge_busy(&c->bridge)) {
        size_t frame = 0;
        size_t used = cmp_decoder_feed(&a->dec, a->in + a->in_at, a->in_len - a->in_at, &frame);

        a->in_at += used;
        if (frame) {
            cmp_bridge_app_frame(&c->bridge, a->dec.buf, frame);
        }
    }
    if (a->in_at == a->in_len) {
        a->in_at = a->in_len = 0;
    }
}

static void app_read(struct ctx *c)
{
    struct app *a = &c->app;
    ssize_t n;

    if (a->in_at > 0) {
        memmove(a->in, a->in + a->in_at, a->in_len - a->in_at);
        a->in_len -= a->in_at;
        a->in_at = 0;
    }
    if (a->in_len == IN_CAP) {
        return;
    }
    n = recv(a->fd, a->in + a->in_len, IN_CAP - a->in_len, 0);
    if (n == 0) {
        app_close(c, "closed by the app");
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            app_close(c, strerror(errno));
        }
        return;
    }
    a->in_len += (size_t)n;
    app_consume(c);
}

static void app_write(struct ctx *c)
{
    struct app *a = &c->app;
    ssize_t n;

    if (a->fd < 0 || a->out_len == 0) {
        return;
    }
    n = send(a->fd, a->out, a->out_len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            app_close(c, strerror(errno));
        }
        return;
    }
    memmove(a->out, a->out + n, a->out_len - (size_t)n);
    a->out_len -= (size_t)n;
    a->out_progress_ms = now_ms();
}

/* ---- meshcored ------------------------------------------------------------ */

static void mesh_close(struct ctx *c, const char *why)
{
    if (c->mesh_fd < 0) {
        return;
    }
    LOG_WARN("meshcored connection closed: %s", why);
    close(c->mesh_fd);
    c->mesh_fd = -1;
    pocketipc_reader_free(&c->reader);
    pocketipc_reader_init(&c->reader);
    cmp_bridge_mesh_down(&c->bridge);
    c->mesh_next_ms = now_ms() + (uint64_t)c->mesh_backoff_ms;
}

static void mesh_try_connect(struct ctx *c)
{
    int fd;

    if (c->mesh_fd >= 0 || now_ms() < c->mesh_next_ms) {
        return;
    }
    fd = pocketipc_connect_timeout(SERVICE, MESH_CONNECT_TIMEOUT_MS);
    if (fd < 0) {
        c->mesh_next_ms = now_ms() + (uint64_t)c->mesh_backoff_ms;
        if (c->mesh_backoff_ms < MESH_BACKOFF_MAX_MS) {
            c->mesh_backoff_ms *= 2;
            if (c->mesh_backoff_ms > MESH_BACKOFF_MAX_MS) {
                c->mesh_backoff_ms = MESH_BACKOFF_MAX_MS;
            }
        }
        return;
    }
    c->mesh_fd = fd;
    c->mesh_backoff_ms = MESH_BACKOFF_MIN_MS;
    LOG_INFO("connected to meshcored");
    cmp_bridge_mesh_up(&c->bridge);
}

static void mesh_read(struct ctx *c)
{
    uint8_t buf[16384];
    ssize_t n = recv(c->mesh_fd, buf, sizeof(buf), 0);
    cJSON *msg;
    int bad = 0;

    if (n == 0) {
        mesh_close(c, "meshcored went away");
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            mesh_close(c, strerror(errno));
        }
        return;
    }
    if (pocketipc_reader_feed(&c->reader, buf, (size_t)n) != 0) {
        mesh_close(c, "oversized frame");
        return;
    }
    while ((msg = pocketipc_reader_next(&c->reader, &bad)) != NULL) {
        cmp_bridge_mesh_frame(&c->bridge, msg);
        cJSON_Delete(msg);
        if (c->mesh_fd < 0) {
            return;
        }
    }
    if (bad) {
        mesh_close(c, "invalid frame");
    }
}

/* ---- main ----------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
            "usage: companiond --listen ADDR:PORT --allow CIDR [--allow CIDR ...] [--verbose]\n"
            "  ADDR is one local address of this unit (not 0.0.0.0); upstream's port is 5000.\n"
            "  Connections are taken only from the --allow networks, and only from\n"
            "  loopback, RFC 1918 or link-local addresses whatever the list says.\n");
}

int main(int argc, char **argv)
{
    static struct ctx c;
    struct cmp_io io;
    const char *listen_spec = NULL;
    int lfd;
    int i;
    uint32_t seed;

    memset(&c, 0, sizeof(c));
    c.app.fd = -1;
    c.mesh_fd = -1;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen_spec = argv[++i];
        } else if (strcmp(argv[i], "--allow") == 0 && i + 1 < argc) {
            if (c.n_allow == MAX_ALLOW || parse_cidr(argv[++i], &c.allow[c.n_allow]) != 0) {
                fprintf(stderr, "companiond: bad --allow %s (IPv4 CIDR, /8 to /32, at most %d)\n",
                        argv[i], MAX_ALLOW);
                return 2;
            }
            c.n_allow++;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            pocketlog_set_level(POCKETLOG_DEBUG);
        } else {
            usage();
            return 2;
        }
    }
    if (!listen_spec || c.n_allow == 0) {
        usage();
        return 2;
    }
    pocketlog_init("companiond");
    pocketlog_install_crash_handler();
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    lfd = listen_on(listen_spec);
    if (lfd < 0) {
        fprintf(stderr, "companiond: cannot listen on %s: %s\n", listen_spec,
                lfd == -2 ? "give one local (loopback, RFC 1918 or link-local) address, "
                            "not 0.0.0.0"
                          : strerror(errno));
        LOG_ERROR("cannot listen on %s", listen_spec);
        return 1;
    }
    c.app.out = malloc(OUT_CAP);
    if (!c.app.out) {
        return 1;
    }
    pocketipc_reader_init(&c.reader);
    c.mesh_backoff_ms = MESH_BACKOFF_MIN_MS;
    io.user = &c;
    io.to_app = io_to_app;
    io.mesh_call = io_mesh_call;
    io.mono_ms = io_mono;
    io.wall_s = io_wall;
    /* The tags handed to the app for its sends are opaque to it; starting
     * them somewhere new on each run keeps one run's from matching another's. */
    seed = (uint32_t)now_ms() ^ ((uint32_t)getpid() << 16);
    cmp_bridge_init(&c.bridge, &io, seed);
    LOG_INFO("companiond %s listening on %s for one MeshCore app (%d allowed network(s))",
             POCKETOS_VERSION, listen_spec, c.n_allow);

    while (!stopping) {
        struct pollfd p[3];
        int np = 0;
        int ia = -1, im = -1;

        mesh_try_connect(&c);
        p[np].fd = lfd;
        p[np].events = POLLIN;
        np++;
        if (c.app.fd >= 0) {
            ia = np;
            p[np].fd = c.app.fd;
            p[np].events = (c.app.in_len < IN_CAP ? POLLIN : 0) | (c.app.out_len ? POLLOUT : 0);
            np++;
        }
        if (c.mesh_fd >= 0) {
            im = np;
            p[np].fd = c.mesh_fd;
            p[np].events = POLLIN;
            np++;
        }
        if (poll(p, (nfds_t)np, 100) < 0 && errno != EINTR) {
            LOG_ERROR("poll: %s", strerror(errno));
            break;
        }
        /* meshcored first: it is the peer that must never wait for us. */
        if (im >= 0 && (p[im].revents & (POLLIN | POLLHUP | POLLERR))) {
            mesh_read(&c);
        }
        if (p[0].revents & POLLIN) {
            app_accept(&c, lfd);
            ia = -1; /* the descriptor may have changed under the index */
        }
        if (ia >= 0 && c.app.fd >= 0) {
            if (p[ia].revents & (POLLIN | POLLHUP | POLLERR)) {
                app_read(&c);
            }
            if (c.app.fd >= 0 && (p[ia].revents & POLLOUT)) {
                app_write(&c);
            }
        }
        /* A reply from meshcored may have freed the bridge to take the next
         * buffered command. */
        app_consume(&c);
        cmp_bridge_tick(&c.bridge, c.app.out_len < OUT_LOW ? 8 : 0);
        if (c.app.fd >= 0 && c.app.out_len) {
            app_write(&c);
        }
        if (c.app.fd >= 0 && c.app.drop) {
            app_close(&c, "output over the limit");
        } else if (c.app.fd >= 0 && c.app.out_len &&
                   now_ms() - c.app.out_progress_ms > OUT_STALL_MS) {
            app_close(&c, "not reading");
        }
        if (c.bridge.mesh_reset_wanted && c.mesh_fd >= 0) {
            c.bridge.mesh_reset_wanted = false;
            mesh_close(&c, "reset after a failed or late request");
        }
    }
    LOG_INFO("companiond stopping (queued %u, delivered %u, evicted %u; sent %u direct, "
             "%u channel, %u resent, %u duplicates answered, %u confirmed)",
             c.bridge.n_queued, c.bridge.n_synced, c.bridge.n_evicted, c.bridge.n_sent_dm,
             c.bridge.n_sent_chan, c.bridge.n_resent, c.bridge.n_dup_suppressed,
             c.bridge.n_confirmed);
    app_close(&c, "service stopping");
    if (c.mesh_fd >= 0) {
        close(c.mesh_fd);
    }
    close(lfd);
    cmp_bridge_free(&c.bridge);
    pocketipc_reader_free(&c.reader);
    free(c.app.out);
    pocketlog_close();
    return 0;
}
