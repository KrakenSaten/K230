/*
 * The time zones Settings offers (ui/shell/tz_zones.h): the table is sound,
 * and every rule, applied the way the shell applies it, gives the local time
 * it claims - in January and in July, so summer time is checked both sides
 * of the equator. No zone database is needed or used: the rules are POSIX
 * TZ strings, which is the point.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "tz_zones.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

#define JAN_15_NOON 1768478400L /* 2026-01-15T12:00:00Z */
#define JUL_01_NOON 1782907200L /* 2026-07-01T12:00:00Z */

/* Local "HH:MM" at t in the zone id, as the shell would show it. */
static void local_hm(const char *id, long t, char *out, size_t n)
{
    time_t tt = (time_t)t;
    struct tm tm;

    tz_zone_apply(tz_zone_find(id));
    localtime_r(&tt, &tm);
    snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static int at(const char *id, long t, const char *want)
{
    char hm[8];

    local_hm(id, t, hm, sizeof(hm));
    if (strcmp(hm, want) != 0) {
        printf("     %s: %s, want %s\n", id, hm, want);
    }
    return strcmp(hm, want) == 0;
}

/* "UTC+05:30..." as seconds east. */
static long offset_of(const char *text)
{
    int h = 0;
    int m = 0;
    char sign = '+';

    if (sscanf(text, "UTC%c%d:%d", &sign, &h, &m) != 3) {
        return -99999;
    }
    return (sign == '-' ? -1L : 1L) * (h * 3600L + m * 60L);
}

static long gmtoff(const struct tz_zone *z, long t)
{
    time_t tt = (time_t)t;
    struct tm tm;

    tz_zone_apply(z);
    localtime_r(&tt, &tm);
    return tm.tm_gmtoff;
}

int main(void)
{
    int i;
    int j;
    int unique = 1;
    int complete = 1;
    int rules_ok = 1;

    check("there are zones to choose from", tz_zone_count() >= 20);
    check("UTC is first and is the default", tz_zone_at(0) && strcmp(tz_zone_at(0)->id, TZ_DEFAULT_ID) == 0);
    for (i = 0; i < tz_zone_count(); i++) {
        const struct tz_zone *z = tz_zone_at(i);

        if (!z->id[0] || !z->place[0] || !z->offset[0] || !z->posix[0] || strlen(z->id) >= 64 ||
            strlen(z->place) >= 48) {
            complete = 0;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(tz_zone_at(j)->id, z->id) == 0) {
                unique = 0;
            }
        }
    }
    check("every zone has a name, places, an offset and a rule, of a size a row holds", complete);
    check("no name twice", unique);
    check("find and index agree", tz_zone_find("Europe/Oslo") == tz_zone_at(tz_zone_index("Europe/Oslo")));
    check("an unknown name is not found", !tz_zone_find("Mars/Olympus") && tz_zone_index("Mars/Olympus") < 0);
    check("NULL is not found", !tz_zone_find(NULL));
    check("applying nothing fails and changes nothing",
          setenv("TZ", "UTC0", 1) == 0 && tz_zone_apply(NULL) < 0 && strcmp(getenv("TZ"), "UTC0") == 0);

    /* Every rule: its standard offset is what the row says, and it has
     * summer time exactly when the row says so. Standard time is the smaller
     * of January's and July's offsets, whichever hemisphere. */
    for (i = 0; i < tz_zone_count(); i++) {
        const struct tz_zone *z = tz_zone_at(i);
        long jan = gmtoff(z, JAN_15_NOON);
        long jul = gmtoff(z, JUL_01_NOON);
        long std = jan < jul ? jan : jul;
        int summer = strstr(z->offset, "summer time") != NULL;

        if (std != offset_of(z->offset) || summer != (jan != jul) || (summer && labs(jan - jul) != 3600)) {
            printf("     %s: jan %ld jul %ld, row says %s\n", z->id, jan, jul, z->offset);
            rules_ok = 0;
        }
    }
    check("every rule gives the offset and the summer time its row shows", rules_ok);

    check("applying sets TZ to the zone's rule",
          tz_zone_apply(tz_zone_find("Europe/Oslo")) == 0 && strcmp(getenv("TZ"), "CET-1CEST,M3.5.0,M10.5.0/3") == 0);
    check("noon UTC in July: Oslo 14:00", at("Europe/Oslo", JUL_01_NOON, "14:00"));
    check("in January: Oslo 13:00", at("Europe/Oslo", JAN_15_NOON, "13:00"));
    check("London 13:00 in July, 12:00 in January",
          at("Europe/London", JUL_01_NOON, "13:00") && at("Europe/London", JAN_15_NOON, "12:00"));
    check("New York 08:00 in July, 07:00 in January",
          at("America/New_York", JUL_01_NOON, "08:00") && at("America/New_York", JAN_15_NOON, "07:00"));
    check("Los Angeles 05:00 in July", at("America/Los_Angeles", JUL_01_NOON, "05:00"));
    check("India 17:30, all year",
          at("Asia/Kolkata", JUL_01_NOON, "17:30") && at("Asia/Kolkata", JAN_15_NOON, "17:30"));
    check("Sydney 22:00 in July (winter), 23:00 in January (summer)",
          at("Australia/Sydney", JUL_01_NOON, "22:00") && at("Australia/Sydney", JAN_15_NOON, "23:00"));
    check("Auckland past midnight both ways",
          at("Pacific/Auckland", JUL_01_NOON, "00:00") && at("Pacific/Auckland", JAN_15_NOON, "01:00"));
    check("Sao Paulo 09:00, no summer time", at("America/Sao_Paulo", JUL_01_NOON, "09:00") &&
                                                 at("America/Sao_Paulo", JAN_15_NOON, "09:00"));
    check("UTC 12:00", at("UTC", JUL_01_NOON, "12:00"));
    /* The change of rule takes effect on the next reading, without anything
     * but the apply in between - the shell's live switch. */
    check("switching back and forth takes effect at once",
          at("Asia/Tokyo", JUL_01_NOON, "21:00") && at("UTC", JUL_01_NOON, "12:00") &&
              at("Asia/Tokyo", JUL_01_NOON, "21:00"));

    printf("tz_zones_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
