/*
 * pocketpcap live source: libpcap (TPACKET_V3 on Linux), receive only.
 *
 * The only file in Doors that includes <pcap/pcap.h> (tests/packetshark_lint.sh).
 * It is built when the build has libpcap (PACKETSHARK_PCAP=1, set by the
 * Buildroot package; libpcap 1.10.5 is in the v0.4.0 image, VERIFIED on Unit
 * A); otherwise ppc_source_none.c is built in its place.
 *
 * What it asks of libpcap (DOCUMENTED, libpcap 1.10 man pages):
 *   pcap_create, snap length 65535 (the decoder sees whole frames; what is
 *   kept is the HEADERS rule's business, ppc_decode.h), promiscuous only when
 *   asked, a 4 MiB kernel ring, 100 ms block timeout, nanosecond stamps,
 *   pcap_activate, Ethernet framing required, non-blocking dispatch from the
 *   selectable descriptor, an optional preset filter, pcap_stats for drops.
 * Nothing is ever sent: there is no call here that can transmit.
 *
 * Direction: libpcap's Ethernet framing carries no packet type, so a frame
 * whose source MAC is the interface's own is outgoing.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "ppc_source.h"

#include <pcap/pcap.h>

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KERNEL_RING_BYTES (4 * 1024 * 1024)
#define BLOCK_TIMEOUT_MS 100
#define SNAPLEN 65535

struct pcap_src {
    pcap_t *p;
    int fd;
    int nano;
    uint8_t mac[6];
    int have_mac;
    ppc_frame_fn fn;
    void *ctx;
};

const char *ppc_source_backend(void)
{
    return "pcap";
}

static int pcap_fd(struct ppc_source *s)
{
    return ((struct pcap_src *)s->impl)->fd;
}

static void on_packet(u_char *user, const struct pcap_pkthdr *h, const u_char *bytes)
{
    struct pcap_src *ps = (struct pcap_src *)user;
    struct ppc_frame f;

    f.data = bytes;
    f.cap_len = h->caplen;
    f.wire_len = h->len < h->caplen ? h->caplen : h->len;
    f.ts_ns = (uint64_t)h->ts.tv_sec * 1000000000u +
              (ps->nano ? (uint64_t)h->ts.tv_usec : (uint64_t)h->ts.tv_usec * 1000u);
    /* The direction libpcap does not give, from the interface's address:
     * sent by this device, addressed to it, broadcast, multicast, or for
     * another host (N1.2: the comparisons count this device's frames in and
     * out, and N1.0's own DNS answers are those addressed to it). */
    f.pkttype = PPC_PKT_UNKNOWN;
    if (ps->have_mac && h->caplen >= 14) {
        static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

        if (memcmp(bytes + 6, ps->mac, 6) == 0) {
            f.pkttype = PPC_PKT_OUTGOING;
        } else if (memcmp(bytes, ps->mac, 6) == 0) {
            f.pkttype = PPC_PKT_HOST;
        } else if (memcmp(bytes, bcast, 6) == 0) {
            f.pkttype = PPC_PKT_BROADCAST;
        } else if (bytes[0] & 1u) {
            f.pkttype = PPC_PKT_MULTICAST;
        } else {
            f.pkttype = PPC_PKT_OTHERHOST;
        }
    }
    ps->fn(ps->ctx, &f);
}

static int pcap_dispatch_frames(struct ppc_source *s, int max, ppc_frame_fn fn, void *ctx)
{
    struct pcap_src *ps = s->impl;
    int n;

    ps->fn = fn;
    ps->ctx = ctx;
    n = pcap_dispatch(ps->p, max, on_packet, (u_char *)ps);
    if (n == PCAP_ERROR_BREAK) {
        return 0;
    }
    if (n < 0) {
        snprintf(s->err, sizeof(s->err), "%s", pcap_geterr(ps->p));
        return PPC_SRC_ERR;
    }
    return n;
}

static int pcap_drops(struct ppc_source *s, uint64_t *kdrop, uint64_t *ifdrop)
{
    struct pcap_src *ps = s->impl;
    struct pcap_stat st;

    if (pcap_stats(ps->p, &st) != 0) {
        return -1;
    }
    *kdrop = st.ps_drop;
    *ifdrop = st.ps_ifdrop;
    return 0;
}

static void pcap_close_src(struct ppc_source *s)
{
    struct pcap_src *ps = s->impl;

    if (ps) {
        if (ps->p) {
            /* Closing the socket ends a promiscuous membership: the kernel
             * reverts the flag itself (DOCUMENTED, packet(7)). */
            pcap_close(ps->p);
        }
        free(ps);
    }
}

static const struct ppc_source_ops pcap_ops = {
    .name = "pcap",
    .fd = pcap_fd,
    .dispatch = pcap_dispatch_frames,
    .stats = pcap_drops,
    .wall_ns = NULL,
    .close = pcap_close_src,
};

/* The interface's MAC from sysfs (a read; no socket, no privilege). */
static int read_mac(const char *iface, uint8_t mac[6])
{
    char path[96];
    unsigned v[6];
    FILE *f;
    int ok = 0;
    int i;

    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    f = fopen(path, "re");
    if (!f) {
        return 0;
    }
    if (fscanf(f, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
        for (i = 0; i < 6; i++) {
            mac[i] = (uint8_t)v[i];
        }
        ok = 1;
    }
    fclose(f);
    return ok;
}

/* This device's IPv4 address on iface, for the "not my SSH" preset. */
static int self_ipv4(const char *iface, char *out, size_t outlen)
{
    struct ifaddrs *ifa = NULL;
    struct ifaddrs *i;
    int found = 0;

    if (getifaddrs(&ifa) != 0) {
        return 0;
    }
    for (i = ifa; i && !found; i = i->ifa_next) {
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && i->ifa_name &&
            strcmp(i->ifa_name, iface) == 0) {
            const struct sockaddr_in *sin = (const struct sockaddr_in *)i->ifa_addr;

            found = inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)outlen) != NULL;
        }
    }
    freeifaddrs(ifa);
    return found;
}

static enum ppc_open_err fail(struct ppc_source *s, struct pcap_src *ps, enum ppc_open_err e,
                              const char *why)
{
    snprintf(s->err, sizeof(s->err), "%s", why && *why ? why : "capture could not start");
    if (ps) {
        if (ps->p) {
            pcap_close(ps->p);
        }
        free(ps);
    }
    return e;
}

enum ppc_open_err ppc_source_open_live(struct ppc_source *s, const struct ppc_open_opts *o)
{
    char errbuf[PCAP_ERRBUF_SIZE] = "";
    struct pcap_src *ps;
    int rc;

    ps = calloc(1, sizeof(*ps));
    if (!ps) {
        return fail(s, NULL, PPC_OPEN_NOMEM, "out of memory");
    }
    ps->fd = -1;
    ps->p = pcap_create(o->iface, errbuf);
    if (!ps->p) {
        return fail(s, ps, PPC_OPEN_OTHER, errbuf);
    }
    pcap_set_snaplen(ps->p, SNAPLEN);
    pcap_set_promisc(ps->p, o->promisc ? 1 : 0);
    pcap_set_buffer_size(ps->p, KERNEL_RING_BYTES);
    pcap_set_timeout(ps->p, BLOCK_TIMEOUT_MS);
    if (pcap_set_tstamp_precision(ps->p, PCAP_TSTAMP_PRECISION_NANO) != 0) {
        /* microseconds then; on_packet scales by what activate gave */
    }
    rc = pcap_activate(ps->p);
    if (rc < 0) {
        const char *why = pcap_geterr(ps->p);

        switch (rc) {
        case PCAP_ERROR_NO_SUCH_DEVICE:
            return fail(s, ps, PPC_OPEN_NODEV, why);
        case PCAP_ERROR_PERM_DENIED:
            return fail(s, ps, PPC_OPEN_PERM, why);
        case PCAP_ERROR_IFACE_NOT_UP:
            return fail(s, ps, PPC_OPEN_DOWN, why);
        default:
            return fail(s, ps, PPC_OPEN_OTHER, why);
        }
    }
    if (pcap_datalink(ps->p) != DLT_EN10MB) {
        return fail(s, ps, PPC_OPEN_NOTETHER, "not an Ethernet-framed interface");
    }
    ps->nano = pcap_get_tstamp_precision(ps->p) == PCAP_TSTAMP_PRECISION_NANO;
    if (o->cfilter != PPC_CFILTER_ALL) {
        char self[INET_ADDRSTRLEN] = "";
        char expr[160];
        struct bpf_program prog;

        self_ipv4(o->iface, self, sizeof(self));
        ppc_cfilter_expr(o->cfilter, self[0] ? self : NULL, expr, sizeof(expr));
        if (pcap_compile(ps->p, &prog, expr, 1, PCAP_NETMASK_UNKNOWN) != 0) {
            return fail(s, ps, PPC_OPEN_FILTER, pcap_geterr(ps->p));
        }
        rc = pcap_setfilter(ps->p, &prog);
        pcap_freecode(&prog);
        if (rc != 0) {
            return fail(s, ps, PPC_OPEN_FILTER, pcap_geterr(ps->p));
        }
    }
    if (pcap_setnonblock(ps->p, 1, errbuf) != 0) {
        return fail(s, ps, PPC_OPEN_OTHER, errbuf);
    }
    ps->fd = pcap_get_selectable_fd(ps->p);
    if (ps->fd < 0) {
        return fail(s, ps, PPC_OPEN_OTHER, "no selectable descriptor");
    }
    ps->have_mac = read_mac(o->iface, ps->mac);
    s->ops = &pcap_ops;
    s->impl = ps;
    s->live = 1;
    return PPC_OPEN_OK;
}

/* For the host test: does a preset compile? 0, or -1 with why. Uses a dead
 * handle, so no interface and no privilege are needed. */
int ppc_source_pcap_compile_check(enum ppc_cfilter f, char *why, size_t whylen);

int ppc_source_pcap_compile_check(enum ppc_cfilter f, char *why, size_t whylen)
{
    pcap_t *p = pcap_open_dead(DLT_EN10MB, SNAPLEN);
    struct bpf_program prog;
    char expr[160];
    int rc = 0;

    if (!p) {
        snprintf(why, whylen, "pcap_open_dead failed");
        return -1;
    }
    ppc_cfilter_expr(f, f == PPC_CFILTER_NOSELF ? "192.0.2.1" : NULL, expr, sizeof(expr));
    if (pcap_compile(p, &prog, expr, 1, PCAP_NETMASK_UNKNOWN) != 0) {
        snprintf(why, whylen, "%s: %s", expr, pcap_geterr(p));
        rc = -1;
    } else {
        pcap_freecode(&prog);
    }
    pcap_close(p);
    return rc;
}
