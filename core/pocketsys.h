/*
 * pocketsys: the system facts behind the system.* API (docs/api/system.md).
 *
 * Pure C on cJSON. Reads /proc, /sys, /etc and the PocketOS runtime
 * directory; opens no device node and changes nothing. Every value that a
 * board may lack is reported as JSON null rather than guessed, so a missing
 * thermal zone, release file or power supply shows up as exactly that.
 *
 * Used by sysd (services/sysd) and unit-tested natively against a fake root.
 * The fake root is a build option, not an environment switch: an object
 * compiled with -DPOCKETSYS_TEST_HOOKS=1 prepends $POCKETSYS_ROOT to every
 * absolute path this module reads (/proc/..., /sys/..., /etc/...), and one
 * compiled without it cannot be redirected at all. Only tests/pocketsys_test
 * is built with the hook; sysd is not, so a service whose whole job is to
 * report what the system is cannot be told to report something else by
 * whoever sets its environment. The supervised-service table comes from
 * $POCKETOS_RUNTIME_DIR (pocketpaths.h), which is a production override and
 * stays one.
 *
 * The fake root covers path reads and nothing else: uname(), sysconf(),
 * time() and the SIOCGIFADDR ioctl behind ipv4 always answer for the running
 * kernel (docs/api/system.md, Test hooks).
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
