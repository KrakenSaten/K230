/*
 * Time zones. See tz_zones.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "tz_zones.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* UTC first (the default), then west to east. One row per rule and region,
 * named by the places people look for. */
static const struct tz_zone zones[] = {
    { "UTC", "UTC", "UTC+00:00", "UTC0" },
    { "Pacific/Honolulu", "Honolulu", "UTC-10:00", "HST10" },
    { "America/Anchorage", "Anchorage", "UTC-09:00, summer time", "AKST9AKDT,M3.2.0,M11.1.0" },
    { "America/Los_Angeles", "Los Angeles, Vancouver", "UTC-08:00, summer time", "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Denver", "Denver, Calgary", "UTC-07:00, summer time", "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Phoenix", "Phoenix", "UTC-07:00", "MST7" },
    { "America/Chicago", "Chicago, Winnipeg", "UTC-06:00, summer time", "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Mexico_City", "Mexico City", "UTC-06:00", "CST6" },
    { "America/New_York", "New York, Toronto", "UTC-05:00, summer time", "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Halifax", "Halifax", "UTC-04:00, summer time", "AST4ADT,M3.2.0,M11.1.0" },
    { "America/Sao_Paulo", "Sao Paulo, Buenos Aires", "UTC-03:00", "<-03>3" },
    { "Atlantic/Reykjavik", "Reykjavik", "UTC+00:00", "GMT0" },
    { "Europe/London", "London, Dublin, Lisbon", "UTC+00:00, summer time", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Africa/Lagos", "Lagos", "UTC+01:00", "WAT-1" },
    { "Europe/Oslo", "Oslo, Stockholm, Berlin, Paris", "UTC+01:00, summer time", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Africa/Johannesburg", "Johannesburg", "UTC+02:00", "SAST-2" },
    { "Europe/Helsinki", "Helsinki, Kyiv, Athens", "UTC+02:00, summer time", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Istanbul", "Istanbul", "UTC+03:00", "<+03>-3" },
    { "Europe/Moscow", "Moscow", "UTC+03:00", "MSK-3" },
    { "Africa/Nairobi", "Nairobi", "UTC+03:00", "EAT-3" },
    { "Asia/Dubai", "Dubai", "UTC+04:00", "<+04>-4" },
    { "Asia/Karachi", "Karachi", "UTC+05:00", "PKT-5" },
    { "Asia/Kolkata", "India", "UTC+05:30", "IST-5:30" },
    { "Asia/Dhaka", "Dhaka", "UTC+06:00", "<+06>-6" },
    { "Asia/Bangkok", "Bangkok, Jakarta", "UTC+07:00", "<+07>-7" },
    { "Asia/Shanghai", "Beijing, Shanghai", "UTC+08:00", "CST-8" },
    { "Asia/Singapore", "Singapore", "UTC+08:00", "<+08>-8" },
    { "Australia/Perth", "Perth", "UTC+08:00", "AWST-8" },
    { "Asia/Tokyo", "Tokyo", "UTC+09:00", "JST-9" },
    { "Asia/Seoul", "Seoul", "UTC+09:00", "KST-9" },
    { "Australia/Sydney", "Sydney, Melbourne", "UTC+10:00, summer time", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Pacific/Auckland", "Auckland", "UTC+12:00, summer time", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};

int tz_zone_count(void)
{
    return (int)(sizeof(zones) / sizeof(zones[0]));
}

const struct tz_zone *tz_zone_at(int i)
{
    return i >= 0 && i < tz_zone_count() ? &zones[i] : NULL;
}

int tz_zone_index(const char *id)
{
    int i;

    for (i = 0; id && i < tz_zone_count(); i++) {
        if (strcmp(zones[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

const struct tz_zone *tz_zone_find(const char *id)
{
    return tz_zone_at(tz_zone_index(id));
}

int tz_zone_apply(const struct tz_zone *zone)
{
    if (!zone || setenv("TZ", zone->posix, 1) != 0) {
        return -1;
    }
    /* localtime_r() does not look at TZ again by itself (glibc reads it once
     * and on tzset()), so this is what makes the change take effect. */
    tzset();
    return 0;
}
