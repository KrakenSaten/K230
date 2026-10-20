/* Host-only simulator capture fixture; never linked into DOORS.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <time.h>
#include <dlfcn.h>
/* Capture fixture only: wall time fixed, monotonic time stays real. */
int clock_gettime(clockid_t id, struct timespec *out)
{
    if (id == CLOCK_REALTIME) {
        out->tv_sec = 1792396800; out->tv_nsec = 0; return 0;
    }
    static int (*real_clock)(clockid_t, struct timespec *);
    if (!real_clock) real_clock = dlsym(RTLD_NEXT, "clock_gettime");
    return real_clock(id, out);
}
