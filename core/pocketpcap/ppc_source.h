/*
 * pocketpcap capture sources: where frames come from.
 *
 *   <iface>              a live interface through libpcap (ppc_source_pcap.c;
 *                        ppc_source_none.c in a build without libpcap)
 *   file:<path>[:<pps>]  a .pcap replayed through the same pipeline, as fast
 *                        as the loop takes it, or at pps frames a second
 *   synth:<pps>[:<seed>][:crash=<n>][:step=<n>,<s>][:burst=<n>,<ms>]
 *                        a deterministic generator: benchmarks, the simulator,
 *                        UI tests. crash=n makes the helper die by SIGSEGV
 *                        after n frames, step=n,s steps the wall clock by s
 *                        seconds after n frames, burst=n,ms hands over n frames
 *                        more ms after the start, all stamped then and all due
 *                        at once, so the reader falls behind briefly (tests
 *                        only)
 *
 * Only a live source applies a capture filter (enum ppc_cfilter): a replay
 * and the generator pass every frame they have.
 *
 * Every source is receive-only: none of them has a way to send a frame
 * (tests/packetshark_lint.sh). Each one gives the helper a descriptor to
 * poll and a non-blocking dispatch that hands over at most `max` frames.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_SOURCE_H
#define PPC_SOURCE_H

#include "ppc_proto.h"
#include "ppc_record.h"

#include <stdint.h>

#define PPC_SRC_EOF (-1)   /* a replay ended */
#define PPC_SRC_ERR (-2)   /* the source failed: the interface went away or down */

/* Why an open failed: the `error` code the helper sends. */
enum ppc_open_err {
    PPC_OPEN_OK = 0,
    PPC_OPEN_NODEV,     /* no such interface */
    PPC_OPEN_DOWN,      /* the interface is down */
    PPC_OPEN_PERM,      /* not permitted (not root) */
    PPC_OPEN_NOTETHER,  /* not an Ethernet-framed link */
    PPC_OPEN_NOBACKEND, /* this build has no capture backend */
    PPC_OPEN_FILTER,    /* the capture filter did not compile */
    PPC_OPEN_BADARG,    /* a source spec that does not parse */
    PPC_OPEN_NOMEM,
    PPC_OPEN_OTHER,
};

const char *ppc_open_err_code(enum ppc_open_err e);

struct ppc_open_opts {
    const char *iface;
    enum ppc_keep keep;
    int promisc;
    enum ppc_cfilter cfilter;
};

typedef void (*ppc_frame_fn)(void *ctx, const struct ppc_frame *f);

struct ppc_source;

struct ppc_source_ops {
    const char *name;
    int (*fd)(struct ppc_source *s);
    /* Hand at most max frames to fn. Returns how many (0 when none were
     * ready), PPC_SRC_EOF or PPC_SRC_ERR (s->err says why). Never blocks. */
    int (*dispatch)(struct ppc_source *s, int max, ppc_frame_fn fn, void *ctx);
    /* The kernel's drop counters, cumulative. 0, or -1 when unknown. */
    int (*stats)(struct ppc_source *s, uint64_t *kdrop, uint64_t *ifdrop);
    /* The wall clock as this source's stamps see it, in nanoseconds; NULL
     * means CLOCK_REALTIME. The synthetic source steps it on request. */
    uint64_t (*wall_ns)(struct ppc_source *s);
    void (*close)(struct ppc_source *s);
};

struct ppc_source {
    const struct ppc_source_ops *ops;
    void *impl;
    int live;            /* a real interface (carrier and speed apply) */
    char err[128];
};

/* The backend this build captures live interfaces with: "pcap" or "none". */
const char *ppc_source_backend(void);

/* Open spec (see above; a bare interface name is live). 0, or a
 * PPC_OPEN_* with s->err set and nothing to close. */
enum ppc_open_err ppc_source_open(struct ppc_source *s, const char *spec,
                                  const struct ppc_open_opts *o);
void ppc_source_close(struct ppc_source *s);

/* The individual openers, for ppc_source_open(). */
enum ppc_open_err ppc_source_open_live(struct ppc_source *s, const struct ppc_open_opts *o);
enum ppc_open_err ppc_source_open_file(struct ppc_source *s, const char *arg);
enum ppc_open_err ppc_source_open_synth(struct ppc_source *s, const char *arg);

/* The synthetic generator's frame n (deterministic for a seed), into buf of
 * at least 1600 bytes. Returns its length. For the bench and the tests. */
uint32_t ppc_synth_frame(uint64_t n, uint32_t seed, uint8_t *buf);

#endif
