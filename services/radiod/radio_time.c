/*
 * The two clocks radiod reads, in one place because three kinds of caller
 * need them: the daemon, the backends (including the C++ one) and the host
 * tests that link the state machine without main.c.
 *
 * They are deliberately separate functions rather than one with a flag. The
 * wall clock and the monotonic clock answer different questions and one is
 * not a substitute for the other: a received packet's place in a sequence is
 * a monotonic question, and the time of day it is written into a log is a
 * wall-clock one. Relabelling CLOCK_REALTIME as monotonic - which costs one
 * character and passes every test that only checks the field exists - would
 * make every receive timestamp jump when NTP first sets the clock, and on
 * this board that happens on every boot: it starts in 1970.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "radio_backend.h"

#include <time.h>

/* Wall clock. Comparable with other machines, and it jumps: NTP steps it,
 * the operator sets it, and it runs backwards when either does. */
uint64_t radio_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Time since boot. Not comparable with other machines, and it never jumps or
 * runs backwards, which is the only property a receive timestamp needs.
 *
 * 64 bits the whole way. At 32 bits this wraps after 49.7 days of uptime,
 * and a wrap in a timestamp a protocol daemon subtracts gives an interval of
 * about 49 days where it should give milliseconds. Nothing on the way to the
 * wire narrows it either: it is carried as a JSON number, and a double holds
 * every integer up to 2^53, which is 285000 years of milliseconds. */
uint64_t radio_mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
