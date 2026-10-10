/*
 * The battery report (core/battery_report.h): the text round-trips, damaged
 * text is refused rather than half-read, and the charging words follow the
 * documented REG0B and BatteryStatus() bits.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "battery_report.h"

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
    }
}

static int words_are(const struct battery_report *r, const char *want)
{
    const char *w = battery_report_status_word(r);

    return want ? (w && strcmp(w, want) == 0) : w == NULL;
}

int main(void)
{
    struct battery_report r;
    struct battery_report back;
    char text[512];

    /* ---- the unit A reading of 2026-10-10, round trip ------------------- */
    memset(&r, 0, sizeof(r));
    r.monotonic_ms = 123456789ULL;
    r.state = BATTERY_REPORT_OK;
    r.has_voltage = true;
    r.voltage_mv = 3785;
    r.has_current = true;
    r.current_ma = -570;
    r.has_battery_status = true;
    r.battery_status = 0x4029;
    r.has_soc = true;
    r.soc_percent = 29;
    r.has_fcc = true;
    r.fcc_mah = 3512;
    r.has_design = true;
    r.design_mah = 3000;
    r.has_opstatus = true;
    r.opstatus = 0x00b4;
    r.has_charger = true;
    r.charger_status = 0x02;
    check("a full reading formats", battery_report_format(&r, text, sizeof(text)) == 0);
    check("and parses back", battery_report_parse(text, &back) == 0);
    check("to the same values", back.monotonic_ms == r.monotonic_ms && back.state == r.state &&
                                    back.voltage_mv == 3785 && back.current_ma == -570 &&
                                    back.battery_status == 0x4029 && back.soc_percent == 29 &&
                                    back.fcc_mah == 3512 && back.design_mah == 3000 &&
                                    back.opstatus == 0x00b4 && back.charger_status == 0x02);
    check("every value marked present", back.has_voltage && back.has_current &&
                                            back.has_battery_status && back.has_soc &&
                                            back.has_fcc && back.has_design && back.has_opstatus &&
                                            back.has_charger);
    check("on battery, no input: discharging", words_are(&back, "discharging"));
    check("and the charger says no external power", battery_report_external(&back) == 0);

    /* ---- what was not read is not invented ------------------------------- */
    memset(&r, 0, sizeof(r));
    r.monotonic_ms = 5;
    r.state = BATTERY_REPORT_NO_ANSWER;
    battery_report_format(&r, text, sizeof(text));
    check("a bare report parses", battery_report_parse(text, &back) == 0);
    check("with nothing in it", back.state == BATTERY_REPORT_NO_ANSWER && !back.has_voltage &&
                                    !back.has_current && !back.has_soc && !back.has_charger);
    check("and says nothing about charging", words_are(&back, NULL) &&
                                                 battery_report_external(&back) == -1);

    /* ---- damaged text ------------------------------------------------------ */
    check("no header, no report", battery_report_parse("monotonic_ms 5\nstate ok\n", &back) != 0);
    check("no time, no report",
          battery_report_parse("doors-battery-report 1\nstate ok\n", &back) != 0);
    check("no state, no report",
          battery_report_parse("doors-battery-report 1\nmonotonic_ms 5\n", &back) != 0);
    check("an unknown state is refused",
          battery_report_parse("doors-battery-report 1\nmonotonic_ms 5\nstate maybe\n", &back) != 0);
    check("a voltage beyond Voltage()'s range is refused",
          battery_report_parse("doors-battery-report 1\nmonotonic_ms 5\nstate ok\nvoltage_mv 7000\n",
                               &back) != 0);
    check("an SOC above 100 is refused",
          battery_report_parse("doors-battery-report 1\nmonotonic_ms 5\nstate ok\nsoc_percent 101\n",
                               &back) != 0);
    check("a value with junk after it is refused",
          battery_report_parse("doors-battery-report 1\nmonotonic_ms 5\nstate ok\ncurrent_ma 5x\n",
                               &back) != 0);
    check("a line it does not know is ignored",
          battery_report_parse("doors-battery-report 1\nmonotonic_ms 5\nstate ok\nfuture 1\n",
                               &back) == 0 && back.state == BATTERY_REPORT_OK);
    check("a truncated buffer does not format", battery_report_format(&r, text, 10) != 0);

    /* ---- charging words, from SLUSC76C Table 17 ----------------------------- */
    memset(&r, 0, sizeof(r));
    r.state = BATTERY_REPORT_OK;
    r.has_charger = true;
    r.charger_status = 0x20 | 0x10 | 0x04 | 0x02; /* SDP, fast charging, power good */
    r.has_battery_status = true;
    r.battery_status = 0x0008;
    check("fast charging is charging", words_are(&r, "charging"));
    check("with external power", battery_report_external(&r) == 1);
    r.charger_status = 0x20 | 0x08 | 0x04 | 0x02; /* pre-charge */
    check("pre-charge is charging", words_are(&r, "charging"));
    r.charger_status = 0x20 | 0x18 | 0x04 | 0x02; /* termination done */
    check("termination done is full", words_are(&r, "full"));
    r.charger_status = 0x20 | 0x04 | 0x02; /* power good, not charging */
    check("power good, not charging, not discharging: not charging",
          words_are(&r, "not_charging"));
    r.battery_status = 0x0009; /* DSG */
    check("DSG wins over a charger that is not charging", words_are(&r, "discharging"));
    r.has_charger = false;
    r.battery_status = 0x0008;
    check("no charger and no DSG: no word", words_are(&r, NULL));
    r.state = BATTERY_REPORT_STOPPED;
    r.has_charger = true;
    r.charger_status = 0x10;
    check("a report that is not OK says nothing", words_are(&r, NULL) &&
                                                       battery_report_external(&r) == -1);

    printf("battery_report_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
