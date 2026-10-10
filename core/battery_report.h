/*
 * The battery report: what the shell's read-only battery probe last read
 * from the keyboard base's BQ27220 gauge and BQ25896 charger, handed to sysd
 * as a small text file in the runtime directory (BATTERY_PROBE.md §9).
 *
 * The shell owns the bus the gauge is on (KEYBOARD_DRIVER_DESIGN §0.2), and
 * sysd owns system.status (docs/api/system.md), so the reading crosses over
 * as a file: written whole and renamed into place by the shell, read by sysd
 * on each status call. It carries the CLOCK_MONOTONIC time of the reading,
 * which both processes share, so sysd can tell a current reading from an
 * old one and never passes an old one on as current.
 *
 * Only what was read, and read plausibly, is in it. Raw gauge words stay
 * raw; units are the gauge's (mV, signed mA, %, mAh). Nothing here decides
 * whether the gauge's percentage means anything: that is sysd's to label
 * and the UI's to withhold.
 *
 * Pure: no I/O. tests/battery_report_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_BATTERY_REPORT_H
#define POCKETOS_BATTERY_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BATTERY_REPORT_FILE "battery-report"

/* A reading older than this is not current (sysd). The shell samples every
 * 30 s and rewrites the report at least once a minute. */
#define BATTERY_REPORT_STALE_MS 150000ULL

enum battery_report_state {
    BATTERY_REPORT_OK,          /* the gauge answered in the last sample */
    BATTERY_REPORT_NO_ANSWER,   /* the keyboard base answers, the gauge did not */
    BATTERY_REPORT_STOPPED,     /* the shell stopped reading (keyboard protection) */
    BATTERY_REPORT_BASE_ABSENT, /* no keyboard base: no battery to read */
};

struct battery_report {
    uint64_t monotonic_ms; /* CLOCK_MONOTONIC when this was true */
    enum battery_report_state state;
    bool has_voltage;
    uint16_t voltage_mv;       /* Voltage() */
    bool has_current;
    int16_t current_ma;        /* Current(): negative is discharge (VERIFIED unit A) */
    bool has_battery_status;
    uint16_t battery_status;   /* BatteryStatus() */
    bool has_soc;
    uint16_t soc_percent;      /* StateOfCharge(): unvalidated */
    bool has_fcc;
    uint16_t fcc_mah;          /* FullChargeCapacity() */
    bool has_design;
    uint16_t design_mah;       /* DesignCapacity() */
    bool has_opstatus;
    uint16_t opstatus;         /* OperationStatus() */
    bool has_charger;
    uint8_t charger_status;    /* BQ25896 REG0B */
};

/* BatteryStatus() and REG0B bits used here (SLUUBD4A Table 2-6, SLUSC76C
 * Table 17). */
#define BATTERY_REPORT_BS_DSG 0x0001u
#define BATTERY_REPORT_BS_BATTPRES 0x0008u
#define BATTERY_REPORT_CHG_VBUS_SHIFT 5u
#define BATTERY_REPORT_CHG_STAT_SHIFT 3u
#define BATTERY_REPORT_CHG_PG 0x04u

/* The file's text. 0, or -1 when it did not fit. */
int battery_report_format(const struct battery_report *r, char *buf, size_t len);

/* The file's text back. Unknown lines are ignored; a missing or wrong
 * header, a missing time or state, or a malformed value is -1. */
int battery_report_parse(const char *text, struct battery_report *r);

const char *battery_report_state_word(enum battery_report_state s);

/* What the readings say about charging, in system.status's words:
 * "charging" (REG0B pre-charge or fast charge), "full" (charge termination
 * done), "discharging" (BatteryStatus()[DSG]), "not_charging" (power good,
 * not charging, not discharging), or NULL when they do not say. */
const char *battery_report_status_word(const struct battery_report *r);

/* External power from REG0B: 1 when VBUS shows an input or power is good,
 * 0 when neither, -1 when the charger was not read. */
int battery_report_external(const struct battery_report *r);

#endif
