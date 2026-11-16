/*
 * pocketpcap statistics and time base. See ppc_stats.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_stats.h"

#include <string.h>

/* 2025-01-01T00:00:00Z */
#define CLOCK_SET_AFTER_S 1735689600ull

void ppc_stats_init(struct ppc_stats *st)
{
    memset(st, 0, sizeof(*st));
}

static void complete(struct ppc_stats *st, ppc_bin_fn emit, void *ctx)
{
    unsigned i = (unsigned)(st->cur_sec % PPC_BINS);

    memcpy(st->bins[i], st->cur, sizeof(st->cur));
    st->held_bins[i] = st->cur_held;
    st->last_pps = st->cur_pkts;
    st->last_bps = st->cur_bytes * 8u;
    if (emit) {
        emit(ctx, st->cur_sec, st->cur, st->cur_held);
    }
    memset(st->cur, 0, sizeof(st->cur));
    st->cur_held = 0;
    st->cur_pkts = 0;
    st->cur_bytes = 0;
}

void ppc_stats_roll(struct ppc_stats *st, uint64_t sec, ppc_bin_fn emit, void *ctx)
{
    unsigned n = 0;

    if (sec <= st->cur_sec) {
        return;
    }
    /* A long stall (a stopped process) would otherwise emit one line per
     * missed second; past the ring's length they would only overwrite each
     * other, so the gap is skipped to its last PPC_BINS seconds. */
    while (st->cur_sec < sec) {
        complete(st, emit, ctx);
        st->cur_sec++;
        if (++n == PPC_BINS && st->cur_sec < sec) {
            st->cur_sec = sec;
        }
    }
}

void ppc_stats_frame(struct ppc_stats *st, unsigned cls, uint32_t wire_len, enum ppc_fate fate)
{
    if (cls >= PPC_CLASS_COUNT) {
        cls = PPC_CLASS_OTHER;
    }
    st->seen++;
    st->bytes += wire_len;
    st->cls_pkts[cls]++;
    st->cls_bytes[cls] += wire_len;
    st->cur[cls]++;
    st->cur_pkts++;
    st->cur_bytes += wire_len;
    if (fate == PPC_FATE_SHED) {
        st->shed++;
    } else {
        st->stored++;
    }
}

void ppc_stats_held(struct ppc_stats *st, uint32_t wire_len)
{
    st->seen++;
    st->held++;
    st->bytes += wire_len;
    st->cur_held++;
    st->cur_pkts++;
    st->cur_bytes += wire_len;
}

int ppc_stats_bin(const struct ppc_stats *st, uint64_t sec, uint32_t cls[PPC_CLASS_COUNT],
                  uint32_t *held)
{
    unsigned i;

    if (sec >= st->cur_sec || st->cur_sec - sec > PPC_BINS) {
        return -1;
    }
    i = (unsigned)(sec % PPC_BINS);
    memcpy(cls, st->bins[i], sizeof(st->bins[i]));
    if (held) {
        *held = st->held_bins[i];
    }
    return 0;
}

/* ---- time ------------------------------------------------------------------------- */

void ppc_timebase_init(struct ppc_timebase *tb)
{
    memset(tb, 0, sizeof(*tb));
}

int ppc_timebase_sample(struct ppc_timebase *tb, uint64_t wall_ns, uint64_t mono_ns)
{
    int64_t off = (int64_t)(wall_ns - mono_ns);
    int64_t d;

    if (!tb->have_offset) {
        tb->have_offset = 1;
        tb->offset_ns = off;
        return 0;
    }
    d = off - tb->offset_ns;
    tb->offset_ns = off;
    if (d > PPC_STEP_NS || d < -PPC_STEP_NS) {
        if (tb->started) {
            tb->origin_ns = (uint64_t)((int64_t)tb->origin_ns + d);
        }
        tb->steps++;
        tb->last_step_ns = d;
        return 1;
    }
    return 0;
}

uint64_t ppc_timebase_rel(struct ppc_timebase *tb, uint64_t ts_ns)
{
    if (!tb->started) {
        tb->started = 1;
        tb->origin_ns = ts_ns;
    }
    return ts_ns > tb->origin_ns ? ts_ns - tb->origin_ns : 0;
}

uint64_t ppc_timebase_wall(const struct ppc_timebase *tb, uint64_t rel_ns)
{
    return tb->origin_ns + rel_ns;
}

int ppc_clock_is_set(uint64_t wall_ns)
{
    return wall_ns / 1000000000ull >= CLOCK_SET_AFTER_S;
}
