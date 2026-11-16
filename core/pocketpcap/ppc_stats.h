/*
 * pocketpcap statistics: a session's counters and its per-second class bins,
 * and the time base that keeps relative time continuous across clock steps.
 *
 * Counters (ARCHITECTURE §6):
 *   seen   = stored + held + shed    every frame the helper took from the kernel
 *   stored   decoded and kept
 *   held     drained while in HOLD (or SAVING): counted, not decoded, not kept
 *   shed     decoded and counted, not kept, because ingest fell behind
 *   kdrop, ifdrop   the kernel's and the interface's own drops; not in seen
 *
 * Bins: one per elapsed second of the session (monotonic), eight class
 * counts and a held count, the last PPC_BINS seconds. A completed second is
 * handed to a callback once (the `bin` line).
 *
 * Signals are a later milestone (M7); their counters are carried as zeros
 * so the line format does not change.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_STATS_H
#define PPC_STATS_H

#include "ppc_record.h"

#include <stdint.h>

#define PPC_BINS 120

struct ppc_stats {
    uint64_t seen;
    uint64_t stored;
    uint64_t held;
    uint64_t shed;
    uint64_t bytes;          /* wire bytes of everything seen */
    uint64_t kdrop;
    uint64_t ifdrop;
    uint64_t ui_skipped;     /* snapshot lines not written because the shell was behind */
    uint64_t cls_pkts[PPC_CLASS_COUNT];
    uint64_t cls_bytes[PPC_CLASS_COUNT];
    uint32_t signals[PPC_SIGNAL_COUNT];
    /* the second being filled, and the ring of completed ones */
    uint64_t cur_sec;
    uint32_t cur[PPC_CLASS_COUNT];
    uint32_t cur_held;
    uint64_t cur_pkts;
    uint64_t cur_bytes;
    uint32_t bins[PPC_BINS][PPC_CLASS_COUNT];
    uint32_t held_bins[PPC_BINS];
    /* the last completed second */
    uint64_t last_pps;
    uint64_t last_bps;
};

typedef void (*ppc_bin_fn)(void *ctx, uint64_t second, const uint32_t cls[PPC_CLASS_COUNT],
                           uint32_t held);

void ppc_stats_init(struct ppc_stats *st);

/* Move to elapsed second `sec`, completing every second before it (each
 * handed to emit, which may be NULL; seconds with nothing in them included,
 * at most PPC_BINS of them). A `sec` before the current one changes nothing. */
void ppc_stats_roll(struct ppc_stats *st, uint64_t sec, ppc_bin_fn emit, void *ctx);

enum ppc_fate {
    PPC_FATE_STORED = 0,
    PPC_FATE_SHED,
};

/* A decoded frame. */
void ppc_stats_frame(struct ppc_stats *st, unsigned cls, uint32_t wire_len, enum ppc_fate fate);
/* A frame drained in HOLD. */
void ppc_stats_held(struct ppc_stats *st, uint32_t wire_len);

/* The bin of an elapsed second still in the ring: 0, or -1 when it is not. */
int ppc_stats_bin(const struct ppc_stats *st, uint64_t sec, uint32_t cls[PPC_CLASS_COUNT],
                  uint32_t *held);

/* ---- time ------------------------------------------------------------------------- *
 *
 * The unit has no RTC and reads 1970 until NTP steps the clock (VERIFIED,
 * feasibility report §2.1). Capture stamps are wall time. The helper samples
 * the wall and monotonic clocks together once per loop turn; when their
 * difference jumps by more than PPC_STEP_NS the jump is added to the origin,
 * so relative times stay continuous across the step (DESIGN_DECISIONS D12).
 */
#define PPC_STEP_NS 1000000000LL

struct ppc_timebase {
    int started;           /* origin set by the first packet */
    int have_offset;
    uint64_t origin_ns;    /* wall time of the first packet, step-corrected */
    int64_t offset_ns;     /* wall - monotonic at the last sample */
    uint32_t steps;
    int64_t last_step_ns;  /* size of the last step */
};

void ppc_timebase_init(struct ppc_timebase *tb);
/* Returns 1 when this sample found a step (and corrected the origin). */
int ppc_timebase_sample(struct ppc_timebase *tb, uint64_t wall_ns, uint64_t mono_ns);
/* Time since the first packet; the first call sets the origin. A stamp
 * before the origin (reordering inside a batch) is 0. */
uint64_t ppc_timebase_rel(struct ppc_timebase *tb, uint64_t ts_ns);
/* Wall time of a relative time, with the origin as it is now. */
uint64_t ppc_timebase_wall(const struct ppc_timebase *tb, uint64_t rel_ns);

/* The wall clock counts as set from 2025-01-01 (sysd's rule, docs/api/system.md). */
int ppc_clock_is_set(uint64_t wall_ns);

#endif
