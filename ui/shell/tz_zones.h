/*
 * Time zones: the ones Settings offers, and how the shell applies one.
 *
 * The image carries no time zone database (no /usr/share/zoneinfo, no
 * /etc/localtime: VERIFIED on unit B 2026-10-02), so the system's local time
 * is UTC. glibc does not need a database for a zone given as a POSIX TZ rule
 * ("CET-1CEST,M3.5.0,M10.5.0/3"), so each zone here carries its rule, and the
 * shell applies the chosen one the way libc expects - TZ in the environment,
 * then tzset() - which takes effect at once for every localtime_r() in the
 * shell (the status clock, the lock, the launcher, Clock and its alarms,
 * every app) and for every helper the shell starts afterwards. Nothing is
 * written outside settings.conf: services keep logging in UTC, which their
 * logs say they do. The rules are the ones in force in 2026; a zone whose
 * rules change needs this table changed.
 *
 * The stored value is the zone's IANA name (settings.conf `timezone`), so a
 * future image with a zone database can keep it. An unknown or absent name
 * leaves the environment alone: UTC, as every unit has always run.
 *
 * Pure C, no LVGL: tests/tz_zones_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_TZ_ZONES_H
#define DOORS_TZ_ZONES_H

#define TZ_SETTING "timezone"
#define TZ_DEFAULT_ID "UTC"

struct tz_zone {
    const char *id;     /* IANA name, the stored value */
    const char *place;  /* what a person looks for: "Oslo, Stockholm, Berlin, Paris" */
    const char *offset; /* standard time: "UTC+01:00", and whether there is summer time */
    const char *posix;  /* the TZ rule glibc applies */
};

int tz_zone_count(void);
const struct tz_zone *tz_zone_at(int i);
/* The zone stored under id, or NULL. */
const struct tz_zone *tz_zone_find(const char *id);
/* Its index in the list, or -1. */
int tz_zone_index(const char *id);

/* Make zone the process's local time: TZ=<rule>, tzset(). Returns 0, or -1
 * when zone is NULL or the environment could not be set (nothing changed). */
int tz_zone_apply(const struct tz_zone *zone);

#endif
