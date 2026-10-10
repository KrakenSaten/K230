/*
 * The battery report. See battery_report.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "battery_report.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER "doors-battery-report 1\n"

static const char *const state_words[] = {
    [BATTERY_REPORT_OK] = "ok",
    [BATTERY_REPORT_NO_ANSWER] = "no-answer",
    [BATTERY_REPORT_STOPPED] = "stopped",
    [BATTERY_REPORT_BASE_ABSENT] = "base-absent",
};

const char *battery_report_state_word(enum battery_report_state s)
{
    return (unsigned)s < sizeof(state_words) / sizeof(state_words[0]) ? state_words[s] : "?";
}

/* Append, keeping track of whether everything fitted. */
static void put(char *buf, size_t len, size_t *at, bool *ok, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

static void put(char *buf, size_t len, size_t *at, bool *ok, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!*ok) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf + *at, len - *at, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= len - *at) {
        *ok = false;
        return;
    }
    *at += (size_t)n;
}

int battery_report_format(const struct battery_report *r, char *buf, size_t len)
{
    size_t at = 0;
    bool ok = true;

    if (!r || !buf || len == 0) {
        return -1;
    }
    buf[0] = '\0';
    put(buf, len, &at, &ok, HEADER "monotonic_ms %" PRIu64 "\nstate %s\n", r->monotonic_ms,
        battery_report_state_word(r->state));
    if (r->has_voltage) {
        put(buf, len, &at, &ok, "voltage_mv %u\n", (unsigned)r->voltage_mv);
    }
    if (r->has_current) {
        put(buf, len, &at, &ok, "current_ma %d\n", (int)r->current_ma);
    }
    if (r->has_battery_status) {
        put(buf, len, &at, &ok, "battery_status 0x%04x\n", (unsigned)r->battery_status);
    }
    if (r->has_soc) {
        put(buf, len, &at, &ok, "soc_percent %u\n", (unsigned)r->soc_percent);
    }
    if (r->has_fcc) {
        put(buf, len, &at, &ok, "full_charge_mah %u\n", (unsigned)r->fcc_mah);
    }
    if (r->has_design) {
        put(buf, len, &at, &ok, "design_mah %u\n", (unsigned)r->design_mah);
    }
    if (r->has_opstatus) {
        put(buf, len, &at, &ok, "operation_status 0x%04x\n", (unsigned)r->opstatus);
    }
    if (r->has_charger) {
        put(buf, len, &at, &ok, "charger_status 0x%02x\n", (unsigned)r->charger_status);
    }
    return ok ? 0 : -1;
}

/* One "key value" line's value as an integer within [lo, hi]. */
static int value(const char *v, const char *eol, long long lo, long long hi, long long *out)
{
    char tmp[32];
    char *end;
    size_t n = (size_t)(eol - v);
    long long x;

    if (n == 0 || n >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, v, n);
    tmp[n] = '\0';
    x = strtoll(tmp, &end, 0);
    if (*end != '\0' || x < lo || x > hi) {
        return -1;
    }
    *out = x;
    return 0;
}

int battery_report_parse(const char *text, struct battery_report *r)
{
    const char *p;
    bool have_time = false;
    bool have_state = false;

    if (!text || !r || strncmp(text, HEADER, strlen(HEADER)) != 0) {
        return -1;
    }
    memset(r, 0, sizeof(*r));
    p = text + strlen(HEADER);
    while (*p) {
        const char *eol = strchr(p, '\n');
        const char *sp;
        long long x = 0;

        if (!eol) {
            eol = p + strlen(p);
        }
        sp = memchr(p, ' ', (size_t)(eol - p));
        if (sp) {
            size_t k = (size_t)(sp - p);
            const char *v = sp + 1;

#define KEY(s) (k == sizeof(s) - 1 && strncmp(p, s, k) == 0)
            if (KEY("monotonic_ms")) {
                if (value(v, eol, 0, INT64_MAX, &x) != 0) {
                    return -1;
                }
                r->monotonic_ms = (uint64_t)x;
                have_time = true;
            } else if (KEY("state")) {
                size_t n = (size_t)(eol - v);
                unsigned i;

                for (i = 0; i < sizeof(state_words) / sizeof(state_words[0]); i++) {
                    if (strlen(state_words[i]) == n && strncmp(v, state_words[i], n) == 0) {
                        r->state = (enum battery_report_state)i;
                        have_state = true;
                    }
                }
                if (!have_state) {
                    return -1;
                }
            } else if (KEY("voltage_mv")) {
                if (value(v, eol, 0, 6000, &x) != 0) {
                    return -1;
                }
                r->voltage_mv = (uint16_t)x;
                r->has_voltage = true;
            } else if (KEY("current_ma")) {
                if (value(v, eol, -32768, 32767, &x) != 0) {
                    return -1;
                }
                r->current_ma = (int16_t)x;
                r->has_current = true;
            } else if (KEY("battery_status")) {
                if (value(v, eol, 0, 0xFFFF, &x) != 0) {
                    return -1;
                }
                r->battery_status = (uint16_t)x;
                r->has_battery_status = true;
            } else if (KEY("soc_percent")) {
                if (value(v, eol, 0, 100, &x) != 0) {
                    return -1;
                }
                r->soc_percent = (uint16_t)x;
                r->has_soc = true;
            } else if (KEY("full_charge_mah")) {
                if (value(v, eol, 0, 32767, &x) != 0) {
                    return -1;
                }
                r->fcc_mah = (uint16_t)x;
                r->has_fcc = true;
            } else if (KEY("design_mah")) {
                if (value(v, eol, 0, 32767, &x) != 0) {
                    return -1;
                }
                r->design_mah = (uint16_t)x;
                r->has_design = true;
            } else if (KEY("operation_status")) {
                if (value(v, eol, 0, 0xFFFF, &x) != 0) {
                    return -1;
                }
                r->opstatus = (uint16_t)x;
                r->has_opstatus = true;
            } else if (KEY("charger_status")) {
                if (value(v, eol, 0, 0xFF, &x) != 0) {
                    return -1;
                }
                r->charger_status = (uint8_t)x;
                r->has_charger = true;
            }
#undef KEY
        }
        p = *eol ? eol + 1 : eol;
    }
    return have_time && have_state ? 0 : -1;
}

const char *battery_report_status_word(const struct battery_report *r)
{
    unsigned chrg;

    if (!r || r->state != BATTERY_REPORT_OK) {
        return NULL;
    }
    if (r->has_charger) {
        chrg = (r->charger_status >> BATTERY_REPORT_CHG_STAT_SHIFT) & 0x3u;
        if (chrg == 1 || chrg == 2) {
            return "charging";
        }
        if (chrg == 3) {
            return "full";
        }
    }
    if (r->has_battery_status && (r->battery_status & BATTERY_REPORT_BS_DSG)) {
        return "discharging";
    }
    if (r->has_charger && (r->charger_status & BATTERY_REPORT_CHG_PG)) {
        return "not_charging";
    }
    return NULL;
}

int battery_report_external(const struct battery_report *r)
{
    if (!r || r->state != BATTERY_REPORT_OK || !r->has_charger) {
        return -1;
    }
    return (((r->charger_status >> BATTERY_REPORT_CHG_VBUS_SHIFT) & 0x7u) != 0 ||
            (r->charger_status & BATTERY_REPORT_CHG_PG))
               ? 1
               : 0;
}
