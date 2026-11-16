/*
 * PacketShark's counters, bins and time base (core/pocketpcap/ppc_stats.h).
 *
 * What is checked: seen = stored + held + shed for any mix (PRODUCT_SPEC
 * T1 at the library level), per-class totals, the per-second bins and the
 * rate of the last full second, quiet seconds emitted as empty bins, a long
 * gap bounded to the ring's length; and the time base: relative time from
 * the first packet, a clock step of 56 years forward (1970 to 2026, what NTP
 * does to this unit) and one backward leave no gap and no jump in relative
 * time (T3), small drift is not a step, and the wall time of a relative time
 * uses the corrected origin.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketpcap/ppc_stats.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

static unsigned emitted;
static uint64_t last_second;
static uint32_t last_total;

static void on_bin(void *ctx, uint64_t second, const uint32_t cls[PPC_CLASS_COUNT], uint32_t held)
{
    unsigned i;

    (void)ctx;
    emitted++;
    last_second = second;
    last_total = held;
    for (i = 0; i < PPC_CLASS_COUNT; i++) {
        last_total += cls[i];
    }
}

static void test_counters(void)
{
    struct ppc_stats st;
    uint32_t c[PPC_CLASS_COUNT];
    uint32_t held;
    unsigned i;

    ppc_stats_init(&st);
    for (i = 0; i < 1000; i++) {
        ppc_stats_frame(&st, i % PPC_CLASS_COUNT, 100, i % 10 == 0 ? PPC_FATE_SHED : PPC_FATE_STORED);
    }
    for (i = 0; i < 250; i++) {
        ppc_stats_held(&st, 60);
    }
    ppc_stats_frame(&st, 99, 10, PPC_FATE_STORED); /* out of range: OTHER */
    check("seen = stored + held + shed (T1)", st.seen == st.stored + st.held + st.shed && st.seen == 1251 &&
                                                  st.held == 250 && st.shed == 100);
    check("bytes of everything seen", st.bytes == 1000u * 100u + 250u * 60u + 10u);
    check("per-class totals; an unknown class counts as OTHER",
          st.cls_pkts[PPC_CLASS_TCP] == 125 && st.cls_pkts[PPC_CLASS_OTHER] == 126);
    emitted = 0;
    ppc_stats_roll(&st, 1, on_bin, NULL);
    check("rolling to second 1 completes second 0", emitted == 1 && last_second == 0 && last_total == 1251);
    check("the rate of the last full second", st.last_pps == 1251 && st.last_bps == st.bytes * 8u);
    check("its bin is readable", ppc_stats_bin(&st, 0, c, &held) == 0 && held == 250 && c[PPC_CLASS_TCP] == 125);
    check("the current second is not", ppc_stats_bin(&st, 1, c, &held) == -1);
    ppc_stats_roll(&st, 4, on_bin, NULL);
    check("quiet seconds are empty bins, emitted", emitted == 4 && last_second == 3 && last_total == 0 &&
                                                        st.last_pps == 0);
    ppc_stats_roll(&st, 2, on_bin, NULL);
    check("rolling back changes nothing", emitted == 4);
    emitted = 0;
    ppc_stats_roll(&st, 100000, on_bin, NULL);
    check("a long stall emits at most a ring's worth", emitted == PPC_BINS && st.cur_sec == 100000);
    check("a bin older than the ring is gone", ppc_stats_bin(&st, 10, c, &held) == -1);
}

#define S 1000000000ull

static void test_time(void)
{
    struct ppc_timebase tb;
    uint64_t mono = 5 * S;
    uint64_t wall = 1 * S; /* 1970: no RTC, NTP not yet */
    uint64_t r1;
    uint64_t r2;
    uint64_t r3;
    uint64_t r4;
    const uint64_t now2026 = 1795000000ull * S;

    ppc_timebase_init(&tb);
    check("1970 is not a set clock", !ppc_clock_is_set(wall));
    ppc_timebase_sample(&tb, wall, mono);
    r1 = ppc_timebase_rel(&tb, wall);
    check("the first packet is time 0", r1 == 0);
    mono += S / 2;
    wall += S / 2;
    ppc_timebase_sample(&tb, wall, mono);
    r2 = ppc_timebase_rel(&tb, wall);
    check("half a second later", r2 == S / 2);
    /* NTP steps the wall clock to 2026; 0.1 s passes */
    mono += S / 10;
    wall = now2026;
    check("a 56-year step is found", ppc_timebase_sample(&tb, wall, mono) == 1 && tb.steps == 1);
    r3 = ppc_timebase_rel(&tb, wall);
    check("and relative time goes on by 0.1 s, not 56 years (T3)", r3 == r2 + S / 10);
    check("the clock now counts as set", ppc_clock_is_set(wall));
    check("wall time of a relative time uses the corrected origin", ppc_timebase_wall(&tb, r3) == wall);
    /* drift of 50 ms is not a step */
    mono += S;
    wall += S + S / 20;
    check("50 ms of drift is not a step", ppc_timebase_sample(&tb, wall, mono) == 0);
    /* a step back of an hour */
    mono += S;
    wall = wall + S - 3600 * S;
    check("a step back is found", ppc_timebase_sample(&tb, wall, mono) == 1 && tb.last_step_ns < 0);
    r4 = ppc_timebase_rel(&tb, wall);
    check("and relative time still moves forward", r4 > r3 && r4 - r3 < 3 * S);
    check("a stamp before the origin is 0, not a wrap", ppc_timebase_rel(&tb, 1) == 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_counters();
    test_time();
    printf("ppc_stats_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
