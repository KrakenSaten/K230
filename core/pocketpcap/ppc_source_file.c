/*
 * pocketpcap replay source: a .pcap through the capture pipeline, for tests,
 * the simulator and (later) opening a saved capture. See ppc_source.h.
 *
 * A pcap file of Ethernet frames carries no direction. A replay that needs
 * one (the evidence history's fixtures, N1.0) names this device's MAC in
 * POCKETOS_CAPTURE_SELF_MAC ("02:00:5e:00:00:10"): frames from it are then
 * handed over as sent by this device, frames to it as addressed to it, the
 * rest as a live interface would call them. Without it, or when it does not
 * parse, direction stays unknown as before.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "ppc_pcapfile.h"
#include "ppc_source.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define FILE_PATH_MAX 512
#define TICK_NS 10000000L

struct file_src {
    struct ppc_pcap_reader rd;
    int fd;
    uint32_t pps;       /* 0: as fast as the loop takes it */
    uint64_t start_ns;  /* monotonic, for the pace */
    uint64_t sent;
    int ended;
    int have_self;      /* POCKETOS_CAPTURE_SELF_MAC was given and parsed */
    uint8_t self[6];
};

/* "02:00:5e:00:00:10" into six bytes. 0, or -1 when it is not a MAC. */
static int mac_parse(const char *t, uint8_t *m)
{
    int i;

    for (i = 0; i < 6; i++) {
        unsigned v = 0;
        int k;

        for (k = 0; k < 2; k++) {
            char c = t[i * 3 + k];

            if (c >= '0' && c <= '9') {
                v = v * 16u + (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v = v * 16u + (unsigned)(c - 'a') + 10u;
            } else {
                return -1;
            }
        }
        if (t[i * 3 + 2] != (i < 5 ? ':' : '\0')) {
            return -1;
        }
        m[i] = (uint8_t)v;
    }
    return 0;
}

/* What a live interface would call the frame, knowing this device's MAC. */
static uint8_t direction(const struct file_src *f, const struct ppc_frame *fr)
{
    if (fr->cap_len < 12) {
        return PPC_PKT_UNKNOWN;
    }
    if (memcmp(fr->data + 6, f->self, 6) == 0) {
        return PPC_PKT_OUTGOING;
    }
    if (memcmp(fr->data, f->self, 6) == 0) {
        return PPC_PKT_HOST;
    }
    if (fr->data[0] & 1u) {
        static const uint8_t all[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

        return memcmp(fr->data, all, 6) == 0 ? PPC_PKT_BROADCAST : PPC_PKT_MULTICAST;
    }
    return PPC_PKT_OTHERHOST;
}

static uint64_t mono_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static int file_fd(struct ppc_source *s)
{
    return ((struct file_src *)s->impl)->fd;
}

static int file_dispatch(struct ppc_source *s, int max, ppc_frame_fn fn, void *ctx)
{
    struct file_src *f = s->impl;
    uint64_t allowed = (uint64_t)max;
    int n = 0;

    if (f->ended) {
        return PPC_SRC_EOF;
    }
    if (f->pps) {
        uint64_t tick;
        uint64_t due = (mono_ns() - f->start_ns) * f->pps / 1000000000u;

        if (read(f->fd, &tick, sizeof(tick)) < 0 && errno != EAGAIN) {
            /* nothing: the next tick reads it */
        }
        allowed = due > f->sent ? due - f->sent : 0;
        if (allowed > (uint64_t)max) {
            allowed = (uint64_t)max;
        }
    }
    while ((uint64_t)n < allowed) {
        struct ppc_frame fr;
        int r = ppc_pcap_next(&f->rd, &fr);

        if (r == 0) {
            f->ended = 1;
            return n > 0 ? n : PPC_SRC_EOF;
        }
        if (r < 0) {
            snprintf(s->err, sizeof(s->err), "%s", f->rd.err);
            f->ended = 1;
            return PPC_SRC_ERR;
        }
        if (f->have_self) {
            fr.pkttype = direction(f, &fr);
        }
        fn(ctx, &fr);
        f->sent++;
        n++;
    }
    return n;
}

static int file_stats(struct ppc_source *s, uint64_t *kdrop, uint64_t *ifdrop)
{
    (void)s;
    *kdrop = 0;
    *ifdrop = 0;
    return 0;
}

static void file_close(struct ppc_source *s)
{
    struct file_src *f = s->impl;

    if (f) {
        ppc_pcap_close(&f->rd);
        if (f->fd >= 0) {
            close(f->fd);
        }
        free(f);
    }
}

static const struct ppc_source_ops file_ops = {
    .name = "file",
    .fd = file_fd,
    .dispatch = file_dispatch,
    .stats = file_stats,
    .wall_ns = NULL,
    .close = file_close,
};

enum ppc_open_err ppc_source_open_file(struct ppc_source *s, const char *arg)
{
    char path[FILE_PATH_MAX];
    const char *colon = strrchr(arg, ':');
    uint32_t pps = 0;
    struct file_src *f;

    snprintf(path, sizeof(path), "%s", arg);
    if (colon && colon[1]) {
        const char *p = colon + 1;

        while (*p && isdigit((unsigned char)*p)) {
            p++;
        }
        if (!*p) {
            pps = (uint32_t)strtoul(colon + 1, NULL, 10);
            path[colon - arg] = '\0';
        }
    }
    if (!path[0] || strlen(arg) >= sizeof(path)) {
        snprintf(s->err, sizeof(s->err), "file: needs a path");
        return PPC_OPEN_BADARG;
    }
    f = calloc(1, sizeof(*f));
    if (!f) {
        snprintf(s->err, sizeof(s->err), "out of memory");
        return PPC_OPEN_NOMEM;
    }
    f->fd = -1;
    if (ppc_pcap_open(&f->rd, path) != 0) {
        snprintf(s->err, sizeof(s->err), "%s", f->rd.err);
        free(f);
        return PPC_OPEN_NODEV;
    }
    f->pps = pps;
    {
        const char *self = getenv("POCKETOS_CAPTURE_SELF_MAC");

        f->have_self = self && strlen(self) == 17 && mac_parse(self, f->self) == 0;
    }
    if (pps) {
        struct itimerspec it = { { 0, TICK_NS }, { 0, TICK_NS } };

        f->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (f->fd >= 0) {
            timerfd_settime(f->fd, 0, &it, NULL);
        }
    } else {
        /* Always readable: replay as fast as the loop takes frames. */
        f->fd = eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
    }
    if (f->fd < 0) {
        snprintf(s->err, sizeof(s->err), "timer: %s", strerror(errno));
        ppc_pcap_close(&f->rd);
        free(f);
        return PPC_OPEN_OTHER;
    }
    f->start_ns = mono_ns();
    s->ops = &file_ops;
    s->impl = f;
    s->live = 0;
    return PPC_OPEN_OK;
}
