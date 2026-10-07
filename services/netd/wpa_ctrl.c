/*
 * wpa_supplicant control-interface client. See wpa_ctrl.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "wpa_ctrl.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static long mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int wpa_ctrl_open(struct wpa_ctrl *c, const char *ctrl_dir, const char *iface,
                  const char *local_dir)
{
    static unsigned counter;
    struct sockaddr_un local;
    struct sockaddr_un remote;
    int saved;

    memset(c, 0, sizeof(*c));
    c->fd = -1;
    if (snprintf(c->remote, sizeof(c->remote), "%s/%s", ctrl_dir, iface) >= (int)sizeof(c->remote) ||
        snprintf(c->local, sizeof(c->local), "%s/netd-ctrl-%d-%u", local_dir, (int)getpid(),
                 counter++) >= (int)sizeof(c->local)) {
        errno = ENAMETOOLONG;
        c->local[0] = '\0';
        return -1;
    }
    c->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (c->fd < 0) {
        c->local[0] = '\0';
        return -1;
    }
    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    memcpy(local.sun_path, c->local, strlen(c->local) + 1);
    unlink(c->local);
    if (bind(c->fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        goto fail;
    }
    memset(&remote, 0, sizeof(remote));
    remote.sun_family = AF_UNIX;
    memcpy(remote.sun_path, c->remote, strlen(c->remote) + 1);
    if (connect(c->fd, (struct sockaddr *)&remote, sizeof(remote)) != 0) {
        goto fail;
    }
    return 0;
fail:
    saved = errno;
    wpa_ctrl_close(c);
    errno = saved;
    return -1;
}

void wpa_ctrl_close(struct wpa_ctrl *c)
{
    if (c->fd >= 0) {
        close(c->fd);
    }
    c->fd = -1;
    if (c->local[0]) {
        unlink(c->local);
    }
    c->local[0] = '\0';
}

/* The same connection again, from a new local address. A reply carries no
 * request id, so the only way to be sure a late one is never read as the
 * answer to something else is to not be at the address it is sent to: the
 * supplicant's sendto() to the old, unlinked path fails and the reply is
 * gone. The new socket is made first and swapped in only once it is up, so
 * a supplicant that has gone away leaves the old connection exactly as it
 * was - a failure the caller already counts, not a new state. Returns 0 when
 * the connection was renewed. */
static int renew(struct wpa_ctrl *c)
{
    char ctrl_dir[sizeof(c->remote)];
    char local_dir[sizeof(c->local)];
    char *iface;
    char *slash;
    struct wpa_ctrl fresh;

    memcpy(ctrl_dir, c->remote, sizeof(ctrl_dir));
    memcpy(local_dir, c->local, sizeof(local_dir));
    iface = strrchr(ctrl_dir, '/');
    slash = strrchr(local_dir, '/');
    if (!iface || !slash) {
        return -1;
    }
    *iface++ = '\0';
    *slash = '\0';
    if (wpa_ctrl_open(&fresh, ctrl_dir, iface, local_dir) != 0) {
        return -1;
    }
    wpa_ctrl_close(c);
    *c = fresh;
    return 0;
}

int wpa_ctrl_request(struct wpa_ctrl *c, const char *cmd, char *reply, size_t n, int timeout_ms)
{
    size_t len = strlen(cmd);
    long deadline = mono_ms() + timeout_ms;
    char drain[256];

    if (c->fd < 0 || n == 0) {
        errno = EBADF;
        return -1;
    }
    /* Anything already queued belongs to an earlier exchange; it must not be
     * taken as this command's reply. This only catches a reply that arrived
     * before this send - one that arrives after it is what the renewal on a
     * timeout below is for. */
    while (recv(c->fd, drain, sizeof(drain), 0) > 0) {
    }
    if (send(c->fd, cmd, len, 0) != (ssize_t)len) {
        return -1;
    }
    for (;;) {
        struct pollfd p = { .fd = c->fd, .events = POLLIN };
        long left = deadline - mono_ms();
        ssize_t r;

        if (left <= 0) {
            /* The answer may still come - after the next command has been
             * sent, where it would be read as that command's (a scan list
             * taken for STATUS reads as "not associated", and netd then
             * drops a working lease). So it must not find this socket. */
            renew(c);
            errno = ETIMEDOUT;
            return -1;
        }
        r = poll(&p, 1, (int)left);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            continue;
        }
        r = recv(c->fd, reply, n - 1, 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            return -1;
        }
        reply[r] = '\0';
        /* An unsolicited message on a request connection (it was never
         * attached, but be safe) starts with "<digit>". */
        if (r > 0 && reply[0] == '<' && r > 2 && reply[2] == '>') {
            continue;
        }
        return (int)r;
    }
}

int wpa_ctrl_attach(struct wpa_ctrl *c, int timeout_ms)
{
    char reply[32];

    if (wpa_ctrl_request(c, "ATTACH", reply, sizeof(reply), timeout_ms) < 0) {
        return -1;
    }
    if (strncmp(reply, "OK", 2) != 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

int wpa_ctrl_recv(struct wpa_ctrl *c, char *buf, size_t n)
{
    ssize_t r;

    if (c->fd < 0 || n == 0) {
        errno = EBADF;
        return -1;
    }
    r = recv(c->fd, buf, n - 1, 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    buf[r] = '\0';
    return (int)r;
}
