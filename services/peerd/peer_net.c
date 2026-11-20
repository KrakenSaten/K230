/*
 * peerd's sockets (peer_net.h).
 *
 * PEERD_TEST_HOOKS builds (tests/peerd-testhooks only; the shipped peerd
 * does not contain the names, which tests/peerd_test.sh checks) read
 * PEERD_TEST_BIND, an address to bind instead of every address, so two
 * peerds can share one machine on two loopback addresses.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "peer_net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef PEERD_TEST_HOOKS
#define PEERD_TEST_HOOKS 0
#endif

/* The address to bind: every address, unless a test build was told one. */
static uint32_t bind_addr(void)
{
#if PEERD_TEST_HOOKS
    const char *v = getenv("PEERD_TEST_BIND");
    struct in_addr a;

    if (v && inet_pton(AF_INET, v, &a) == 1) {
        return ntohl(a.s_addr);
    }
#endif
    return 0;
}

int pnet_open(uint16_t port)
{
    struct sockaddr_in sa;
    int one = 1;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(bind_addr());
    /* No SO_REUSEADDR: nothing else on this device may share the port, and
     * a second peerd must fail to bind rather than run beside the first. */
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) != 0 ||
        bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void pnet_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

int pnet_ifaces(struct pcore_iface *out, int max)
{
    struct ifaddrs *list = NULL;
    struct ifaddrs *it;
    int n = 0;

    if (bind_addr() != 0 || getifaddrs(&list) != 0) {
        return 0;       /* a test bound to one address announces on none */
    }
    for (it = list; it && n < max; it = it->ifa_next) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)it->ifa_addr;
        const struct sockaddr_in *b = (const struct sockaddr_in *)it->ifa_broadaddr;

        if (!a || a->sin_family != AF_INET || !(it->ifa_flags & IFF_UP) ||
            (it->ifa_flags & IFF_LOOPBACK) || !(it->ifa_flags & IFF_BROADCAST) || !b ||
            b->sin_family != AF_INET) {
            continue;
        }
        out[n].addr = ntohl(a->sin_addr.s_addr);
        out[n].bcast = ntohl(b->sin_addr.s_addr);
        if (out[n].addr != 0 && out[n].bcast != 0) {
            n++;
        }
    }
    freeifaddrs(list);
    return n;
}

int pnet_send(int fd, uint32_t dst, uint16_t port, const uint8_t *buf, size_t n, int broadcast)
{
    struct sockaddr_in sa;
    struct msghdr msg;
    struct iovec iov;
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } ctl;
    ssize_t rc;

    if (fd < 0) {
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(dst);
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void *)buf;
    iov.iov_len = n;
    msg.msg_name = &sa;
    msg.msg_namelen = sizeof(sa);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (broadcast) {
        /* Announcements are for this link only: they are not to cross a router. */
        struct cmsghdr *cm;
        int ttl = 1;

        memset(&ctl, 0, sizeof(ctl));
        msg.msg_control = ctl.buf;
        msg.msg_controllen = sizeof(ctl.buf);
        cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = IPPROTO_IP;
        cm->cmsg_type = IP_TTL;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &ttl, sizeof(ttl));
    }
    do {
        rc = sendmsg(fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (rc < 0 && errno == EINTR);
    return rc == (ssize_t)n ? 0 : -1;
}

uint32_t pnet_source_for(uint32_t dst, uint16_t port)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    uint32_t bound = bind_addr();
    uint32_t src = 0;
    int fd;

    if (bound != 0) {
        return bound;
    }
    /* Connecting a UDP socket sends nothing: it only asks the kernel which
     * route, and so which of our addresses, a datagram to dst would take. */
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 0;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port ? port : 9);
    sa.sin_addr.s_addr = htonl(dst);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
        getsockname(fd, (struct sockaddr *)&sa, &len) == 0 && sa.sin_family == AF_INET) {
        src = ntohl(sa.sin_addr.s_addr);
    }
    close(fd);
    return src;
}

long pnet_recv(int fd, uint32_t *src, uint16_t *src_port, uint8_t *buf, size_t max)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    ssize_t n;

    do {
        n = recvfrom(fd, buf, max, MSG_DONTWAIT | MSG_TRUNC, (struct sockaddr *)&sa, &len);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    }
    if (len < sizeof(sa) || sa.sin_family != AF_INET) {
        return 0;
    }
    *src = ntohl(sa.sin_addr.s_addr);
    *src_port = ntohs(sa.sin_port);
    /* MSG_TRUNC gives the datagram's real length, which may exceed max: the
     * caller treats anything over its buffer as too long, without reading
     * past what was written. */
    return (long)n;
}
