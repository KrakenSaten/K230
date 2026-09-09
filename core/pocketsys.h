/*
 * pocketsys: the system facts behind the system.* API (docs/api/system.md).
 *
 * Pure C on cJSON. Reads /proc, /sys, /etc and the PocketOS runtime
 * directory; opens no device node and changes nothing. Every value that a
 * board may lack is reported as JSON null rather than guessed, so a missing
 * thermal zone, release file or power supply shows up as exactly that.
 *
 * Used by sysd (services/sysd) and unit-tested natively against a fake
 * root: when $POCKETSYS_ROOT is set, it is prepended to every absolute path
 * this module reads (/proc/..., /sys/..., /etc/...). Production leaves it
 * unset. The supervised-service table comes from $POCKETOS_RUNTIME_DIR
 * (pocketpaths.h), which the tests already override.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETSYS_H
#define POCKETSYS_H

#include <cjson/cJSON.h>

/* CPU utilisation is a rate and needs two samples of /proc/stat. The owner
 * keeps one of these, samples it on a timer (sysd: once a second) and passes
 * it to pocketsys_status(), which reports the share of the last interval. */
struct pocketsys_cpu {
    unsigned long long busy;
    unsigned long long total;
    int have_sample;
    double percent;
    int have_percent;
};

void pocketsys_cpu_init(struct pocketsys_cpu *cpu);
/* Read /proc/stat. With a previous sample, compute the busy share of the
 * interval since it (0..100). Returns 0 when the file was read, -1 (errno)
 * when it could not be; the previous percent is kept on failure. */
int pocketsys_cpu_sample(struct pocketsys_cpu *cpu);

/* system.info: identity that does not change while the system runs.
 * version and build name the running binary; both may be NULL ("unknown"). */
cJSON *pocketsys_info(const char *version, const char *build);

/* system.status: the live view. cpu may be NULL (cpu_percent is then null). */
cJSON *pocketsys_status(const struct pocketsys_cpu *cpu);

#endif
