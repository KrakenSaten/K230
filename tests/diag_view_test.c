/*
 * System > Diagnostics model test (apps/system/diag_view.c): the summary rows
 * from system.status, radio.status and mesh.status, crash reports, the log
 * list and its filters, the staged refresh, and what every "did not answer"
 * looks like.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "diag_view.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static int row_is(const struct diag_view *v, enum diag_row r, const char *want)
{
    int ok = strcmp(v->rows[r].value, want) == 0;

    if (!ok) {
        printf("     row %s: got '%s', want '%s'\n", v->rows[r].label, v->rows[r].value, want);
    }
    return ok;
}

static void apply_system(struct diag_view *d, struct system_view *sv, const char *json)
{
    cJSON *st = json ? cJSON_Parse(json) : NULL;

    system_view_apply_status(sv, st, 1000);
    diag_view_apply_system(d, sv, st);
    cJSON_Delete(st);
}

static void apply(void (*fn)(struct diag_view *, const cJSON *), struct diag_view *d,
                  const char *json)
{
    cJSON *o = json ? cJSON_Parse(json) : NULL;

    fn(d, o);
    cJSON_Delete(o);
}

#define UNIT_A_STATUS                                                                           \
    "{\"uptime_s\":7384,\"load\":[0.1,0.2,0.3],\"cpu_percent\":4,"                            \
    "\"memory\":{\"total_kb\":990544,\"available_kb\":931408,\"free_kb\":900000},"             \
    "\"temperature_c\":49.8,\"clock_set\":true,"                                               \
    "\"storage\":[{\"mount\":\"/\",\"total_bytes\":574000000,\"avail_bytes\":131000000}],"     \
    "\"network\":[],"                                                                          \
    "\"power\":{\"source\":\"external\",\"external_online\":null,\"battery\":null,\"supplies\":[]}," \
    "\"bluetooth\":{\"controllers\":[]},"                                                      \
    "\"services\":[{\"name\":\"netd\",\"pid\":1,\"running\":true,\"crashloop\":false,"        \
    "\"last_exit_code\":null,\"restarts\":0},"                                                 \
    "{\"name\":\"radiod\",\"pid\":2,\"running\":true,\"crashloop\":false,"                    \
    "\"last_exit_code\":null,\"restarts\":0},"                                                 \
    "{\"name\":\"sysd\",\"pid\":3,\"running\":true,\"crashloop\":false,"                      \
    "\"last_exit_code\":null,\"restarts\":0}]}"

int main(void)
{
    struct diag_view d;
    struct system_view sv;
    cJSON *p;
    int i;

    diag_view_init(&d);
    system_view_init(&sv);

    /* ---- before anything answered: unknown, never zero ---- */
    check("every row starts unknown", row_is(&d, DIAG_ROW_UPTIME, SYSTEM_VIEW_UNKNOWN) &&
                                          row_is(&d, DIAG_ROW_BATTERY, SYSTEM_VIEW_UNKNOWN));
    check("the log has not been read", strcmp(d.log_status, "Not read yet") == 0 && d.log_count == 0);

    /* ---- the staged refresh: one call per tick ---- */
    check("idle until asked", diag_view_step(&d) == DIAG_STEP_IDLE && !diag_view_busy(&d));
    diag_view_refresh(&d);
    check("refresh starts with system.status", diag_view_step(&d) == DIAG_STEP_STATUS);
    diag_view_step_done(&d);
    check("then radio.status", diag_view_step(&d) == DIAG_STEP_RADIO);
    diag_view_refresh(&d);
    check("a second refresh while one runs does not restart it", diag_view_step(&d) == DIAG_STEP_RADIO);
    diag_view_step_done(&d);
    check("then mesh.status", diag_view_step(&d) == DIAG_STEP_MESH);
    diag_view_step_done(&d);
    check("then system.crashes", diag_view_step(&d) == DIAG_STEP_CRASHES);
    diag_view_step_done(&d);
    check("then system.logs", diag_view_step(&d) == DIAG_STEP_LOGS);
    diag_view_step_done(&d);
    check("then idle", diag_view_step(&d) == DIAG_STEP_IDLE);
    diag_view_set_filter(&d, DIAG_FILTER_ERROR);
    check("a new filter asks for the log only", diag_view_step(&d) == DIAG_STEP_LOGS);
    p = diag_view_logs_params(&d);
    check("with level error and the page's bound",
          strcmp(cJSON_GetObjectItemCaseSensitive(p, "level")->valuestring, "error") == 0 &&
              cJSON_GetObjectItemCaseSensitive(p, "limit")->valueint == DIAG_LOG_MAX);
    cJSON_Delete(p);
    diag_view_step_done(&d);
    diag_view_set_filter(&d, (enum diag_filter)7);
    check("an unknown filter is ignored", d.filter == DIAG_FILTER_ERROR && !diag_view_busy(&d));
    check("filter labels", strcmp(diag_view_filter_label(DIAG_FILTER_ALL), "All") == 0 &&
                               strcmp(diag_view_filter_label(DIAG_FILTER_WARN), "Warnings") == 0 &&
                               strcmp(diag_view_filter_label(DIAG_FILTER_ERROR), "Errors") == 0);

    /* ---- unit A as it is: external power, no Bluetooth, three services ---- */
    {
        cJSON *info = cJSON_Parse("{\"version\":\"0.0.12\",\"build\":\"abc1234\"}");

        system_view_apply_info(&sv, info);
        cJSON_Delete(info);
    }
    apply_system(&d, &sv, UNIT_A_STATUS);
    check("version and build", strstr(d.rows[DIAG_ROW_VERSION].value, "0.0.12") &&
                                   strstr(d.rows[DIAG_ROW_VERSION].value, "abc1234"));
    check("uptime from the System screen's own formatting",
          strcmp(d.rows[DIAG_ROW_UPTIME].value, sv.vitals[4].value) == 0 &&
              strcmp(d.rows[DIAG_ROW_UPTIME].value, SYSTEM_VIEW_UNKNOWN) != 0);
    check("memory likewise", strcmp(d.rows[DIAG_ROW_MEMORY].value, sv.vitals[2].value) == 0);
    check("storage is the root file system, 77 % used is not a warning",
          strcmp(d.rows[DIAG_ROW_STORAGE].value, sv.mounts[0].detail) == 0 &&
              strcmp(d.rows[DIAG_ROW_STORAGE].value, SYSTEM_VIEW_UNKNOWN) != 0 &&
              !d.rows[DIAG_ROW_STORAGE].warn);
    check("no battery on unit A, said as such", row_is(&d, DIAG_ROW_BATTERY, "No battery, external power"));
    check("no Bluetooth controller", row_is(&d, DIAG_ROW_BLUETOOTH, "No controller"));
    check("services: all running, no warning",
          row_is(&d, DIAG_ROW_SERVICES, "3 running") && !d.rows[DIAG_ROW_SERVICES].warn);

    /* ---- services that are not well ---- */
    apply_system(&d, &sv,
                 "{\"services\":[{\"name\":\"radiod\",\"running\":true,\"crashloop\":false,\"restarts\":2},"
                 "{\"name\":\"sysd\",\"running\":true,\"crashloop\":false,\"restarts\":0}]}");
    check("restarts in the window are named, with a warning",
          row_is(&d, DIAG_ROW_SERVICES, "2 running \xC2\xB7 restarted: radiod 2x") &&
              d.rows[DIAG_ROW_SERVICES].warn);
    apply_system(&d, &sv,
                 "{\"services\":[{\"name\":\"radiod\",\"running\":false,\"crashloop\":true,\"restarts\":6},"
                 "{\"name\":\"meshcored\",\"running\":false,\"crashloop\":false,\"restarts\":0}]}");
    check("a crash loop and a stopped service lead the row",
          row_is(&d, DIAG_ROW_SERVICES, "radiod crash loop, meshcored stopped") &&
              d.rows[DIAG_ROW_SERVICES].warn);
    apply_system(&d, &sv, "{\"services\":[]}");
    check("nothing supervised", row_is(&d, DIAG_ROW_SERVICES, "None supervised"));

    /* ---- batteries ---- */
    apply_system(&d, &sv, "{\"power\":{\"source\":\"external\",\"battery\":{\"present\":true,"
                          "\"capacity_percent\":76,\"status\":\"charging\",\"voltage_v\":4.05}}}");
    check("charging, with the voltage the driver gave",
          row_is(&d, DIAG_ROW_BATTERY, "76 % \xC2\xB7 Charging \xC2\xB7 4.05 V") &&
              !d.rows[DIAG_ROW_BATTERY].warn);
    apply_system(&d, &sv, "{\"power\":{\"source\":\"battery\",\"battery\":{\"present\":true,"
                          "\"capacity_percent\":9,\"status\":\"discharging\",\"voltage_v\":null}}}");
    check("low and discharging is a warning",
          row_is(&d, DIAG_ROW_BATTERY, "9 % \xC2\xB7 On battery") && d.rows[DIAG_ROW_BATTERY].warn);
    apply_system(&d, &sv, "{\"power\":{\"battery\":{\"present\":true,\"capacity_percent\":null,"
                          "\"status\":null}}}");
    check("a battery that says nothing", row_is(&d, DIAG_ROW_BATTERY, "Battery, level unknown"));
    apply_system(&d, &sv, "{\"power\":{\"battery\":{\"present\":false}}}");
    check("a gauge with no cell", row_is(&d, DIAG_ROW_BATTERY, "No battery"));
    check("and no gauge figures", row_is(&d, DIAG_ROW_GAUGE, SYSTEM_VIEW_UNKNOWN));

    /* ---- the keyboard base's gauge (docs/api/system.md) ----------------- */
    apply_system(&d, &sv, "{\"power\":{\"source\":\"battery\",\"battery\":{\"present\":true,"
                          "\"capacity_percent\":null,\"status\":\"discharging\","
                          "\"voltage_v\":3.787,\"current_a\":-0.57,\"reading\":\"ok\",\"age_s\":12,"
                          "\"gauge\":{\"validated\":false,\"soc_percent\":29,"
                          "\"full_charge_capacity_mah\":3512,\"design_capacity_mah\":3000}}}}");
    check("base gauge: status, voltage, signed current, level unknown",
          row_is(&d, DIAG_ROW_BATTERY,
                 "On battery \xC2\xB7 3.79 V \xC2\xB7 -0.57 A \xC2\xB7 level unknown") &&
              !d.rows[DIAG_ROW_BATTERY].warn);
    check("the gauge's own figures, labelled unvalidated",
          row_is(&d, DIAG_ROW_GAUGE,
                 "Unvalidated \xC2\xB7 SOC 29 % \xC2\xB7 full 3512 mAh \xC2\xB7 design 3000 mAh"));
    apply_system(&d, &sv, "{\"power\":{\"source\":\"external\",\"battery\":{\"present\":true,"
                          "\"capacity_percent\":null,\"status\":\"charging\",\"voltage_v\":4.1,"
                          "\"current_a\":0.8,\"reading\":\"ok\",\"gauge\":null}}}");
    check("charging current is signed plus",
          row_is(&d, DIAG_ROW_BATTERY,
                 "Charging \xC2\xB7 4.10 V \xC2\xB7 +0.80 A \xC2\xB7 level unknown"));
    check("no gauge object, no gauge figures", row_is(&d, DIAG_ROW_GAUGE, SYSTEM_VIEW_UNKNOWN));
    apply_system(&d, &sv, "{\"power\":{\"source\":\"unknown\",\"battery\":{\"present\":null,"
                          "\"capacity_percent\":null,\"status\":null,\"voltage_v\":null,"
                          "\"current_a\":null,\"reading\":\"stale\",\"age_s\":400,\"gauge\":null}}}");
    check("a stale reading: how old, no values, flagged",
          row_is(&d, DIAG_ROW_BATTERY, "No reading for 400 s") && d.rows[DIAG_ROW_BATTERY].warn);
    check("and the old gauge figures are gone", row_is(&d, DIAG_ROW_GAUGE, SYSTEM_VIEW_UNKNOWN));
    apply_system(&d, &sv, "{\"power\":{\"battery\":{\"reading\":\"no-answer\",\"gauge\":null}}}");
    check("the gauge not answering", row_is(&d, DIAG_ROW_BATTERY, "Gauge not answering"));
    apply_system(&d, &sv, "{\"power\":{\"battery\":{\"reading\":\"stopped\",\"gauge\":null}}}");
    check("reading stopped",
          row_is(&d, DIAG_ROW_BATTERY, "Reading stopped (keyboard protection)"));
    apply_system(&d, &sv, "{\"bluetooth\":{\"controllers\":[\"hci0\",\"hci1\"]}}");
    check("controllers named", row_is(&d, DIAG_ROW_BLUETOOTH, "hci0, hci1"));

    /* ---- sysd not answering ---- */
    apply_system(&d, &sv, NULL);
    check("sysd not answering is said, with a warning",
          row_is(&d, DIAG_ROW_SERVICES, "sysd not answering") && d.rows[DIAG_ROW_SERVICES].warn);
    check("and the values already shown are kept", row_is(&d, DIAG_ROW_BLUETOOTH, "hci0, hci1"));
    check("except the battery's: no old reading stays up as current",
          row_is(&d, DIAG_ROW_BATTERY, SYSTEM_VIEW_UNKNOWN) &&
              row_is(&d, DIAG_ROW_GAUGE, SYSTEM_VIEW_UNKNOWN));

    /* ---- the radio ---- */
    apply(diag_view_apply_radio, &d, NULL);
    check("radiod not answering", row_is(&d, DIAG_ROW_RADIO, "radiod not answering") &&
                                      d.rows[DIAG_ROW_RADIO].warn);
    apply(diag_view_apply_radio, &d, "{\"state\":\"off\",\"enabled\":false,\"profile\":{"
                                     "\"frequency_mhz\":869.618,\"spreading_factor\":8}}");
    check("switched off is a choice, not a warning",
          row_is(&d, DIAG_ROW_RADIO, "Off (switched off)") && !d.rows[DIAG_ROW_RADIO].warn);
    apply(diag_view_apply_radio, &d, "{\"state\":\"rx\",\"enabled\":true,\"profile\":{"
                                     "\"frequency_mhz\":869.618,\"spreading_factor\":8}}");
    check("receiving, with its channel", row_is(&d, DIAG_ROW_RADIO, "Receiving \xC2\xB7 869.618 MHz SF8"));
    apply(diag_view_apply_radio, &d, "{\"state\":\"error\",\"enabled\":true,\"profile_uncertain\":true}");
    check("on and failing is a warning, and says the profile is uncertain",
          row_is(&d, DIAG_ROW_RADIO, "On, not receiving \xC2\xB7 profile uncertain") &&
              d.rows[DIAG_ROW_RADIO].warn);

    /* ---- the mesh ---- */
    apply(diag_view_apply_mesh, &d, NULL);
    check("meshcored off (its default) is not a fault",
          row_is(&d, DIAG_ROW_MESH, "meshcored not running") && !d.rows[DIAG_ROW_MESH].warn);
    apply(diag_view_apply_mesh, &d, "{\"state\":\"online\",\"reason\":\"x\"}");
    check("online", row_is(&d, DIAG_ROW_MESH, "Online"));
    apply(diag_view_apply_mesh, &d, "{\"state\":\"degraded\",\"reason\":\"the radio is switched off\","
                                    "\"radio\":{\"radio_state\":\"off\"}}");
    check("waiting for a radio that is switched off is not a fault",
          row_is(&d, DIAG_ROW_MESH, "Waiting: the radio is switched off") && !d.rows[DIAG_ROW_MESH].warn);
    apply(diag_view_apply_mesh, &d, "{\"state\":\"error\",\"reason\":\"the profile was refused\"}");
    check("an error says why", row_is(&d, DIAG_ROW_MESH, "error: the profile was refused") &&
                                   d.rows[DIAG_ROW_MESH].warn);

    /* ---- crash reports ---- */
    apply(diag_view_apply_crashes, &d, "{\"available\":true,\"reports\":[],\"total\":0}");
    check("no crash reports", row_is(&d, DIAG_ROW_CRASHES, "None") && d.crash_count == 0);
    apply(diag_view_apply_crashes, &d,
          "{\"available\":true,\"total\":7,\"reports\":["
          "{\"file\":\"a\",\"process\":\"radiod\",\"pid\":7,\"time\":1790000000,\"signal\":11,"
          "\"signal_name\":\"SIGSEGV\",\"frames\":[\"/usr/sbin/radiod(+0x10)[0x1]\"]},"
          "{\"file\":\"b\",\"process\":\"shell\",\"pid\":9,\"time\":5,\"signal\":null,"
          "\"signal_name\":null,\"frames\":[]},"
          "{\"process\":\"x\",\"time\":1},{\"process\":\"x\",\"time\":1},{\"process\":\"x\",\"time\":1},"
          "{\"process\":\"x\",\"time\":1}]}");
    check("the total is the row, with a warning", row_is(&d, DIAG_ROW_CRASHES, "7 reports") &&
                                                      d.rows[DIAG_ROW_CRASHES].warn);
    check("at most the page's bound are listed", d.crash_count == DIAG_CRASH_MAX);
    check("a report: process, signal, time",
          strcmp(d.crashes[0].line, "radiod \xC2\xB7 SIGSEGV \xC2\xB7 09-21 14:13 UTC") == 0);
    check("and its first frame", strcmp(d.crashes[0].frame, "/usr/sbin/radiod(+0x10)[0x1]") == 0);
    check("a report written before the clock was set says so, signal unknown",
          strcmp(d.crashes[1].line, "shell \xC2\xB7 signal unknown \xC2\xB7 clock not set") == 0 &&
              d.crashes[1].frame[0] == '\0');
    apply(diag_view_apply_crashes, &d, NULL);
    check("sysd not answering for crashes", row_is(&d, DIAG_ROW_CRASHES, "sysd not answering"));

    /* ---- the log ---- */
    d.filter = DIAG_FILTER_ALL;
    apply(diag_view_apply_logs, &d,
          "{\"available\":true,\"returned\":3,\"skipped\":2,\"entries\":["
          "{\"ts\":\"2026-09-24T13:20:05.000Z\",\"source\":\"radiod\",\"level\":\"error\","
          "\"message\":\"transceiver is not in receive mode; state error\"},"
          "{\"ts\":\"2026-09-24T13:20:02.500Z\",\"source\":\"supervise-radiod\",\"level\":\"warn\","
          "\"message\":\"radiod exited rc=1\"},"
          "{\"ts\":\"1970-01-01T00:00:04.000Z\",\"source\":\"sysd\",\"level\":\"info\",\"message\":\"listening\"}]}");
    check("three entries, newest first as sysd gave them", d.log_count == 3);
    check("an error: its time, source and level in the head",
          strcmp(d.log[0].head, "09-24 13:20:05  radiod  ERROR") == 0 && d.log[0].severity == DIAG_SEV_ERROR);
    check("a warning", d.log[1].severity == DIAG_SEV_WARN);
    check("a line from before the clock was set keeps its time as written",
          strcmp(d.log[2].head, "01-01 00:00:04  sysd  INFO") == 0);
    check("the status counts and names the unreadable lines",
          strcmp(d.log_status, "3 newest entries \xC2\xB7 2 unreadable") == 0);
    {
        unsigned gen = d.log_generation;

        apply(diag_view_apply_logs, &d, NULL);
        check("sysd not answering keeps the list and says so",
              d.log_count == 3 && d.log_generation == gen &&
                  strstr(d.log_status, "not answering") != NULL);
    }
    d.filter = DIAG_FILTER_WARN;
    apply(diag_view_apply_logs, &d, "{\"available\":true,\"entries\":[],\"skipped\":0}");
    check("no warnings", d.log_count == 0 && strcmp(d.log_status, "No warnings or errors logged") == 0);
    apply(diag_view_apply_logs, &d, "{\"available\":false,\"entries\":[]}");
    check("no log directory", d.log_count == 0 &&
                                  strcmp(d.log_status, "No log directory on this card") == 0);
    apply(diag_view_apply_logs, &d, "{\"available\":true,\"entries\":[{\"level\":7},{}]}");
    check("malformed entries are shown as unknown, not dropped silently or crashed on",
          d.log_count == 2 && strstr(d.log[0].head, SYSTEM_VIEW_UNKNOWN) != NULL);
    {
        char big[8192];
        size_t off = 0;

        off += (size_t)snprintf(big + off, sizeof(big) - off, "{\"available\":true,\"entries\":[");
        for (i = 0; i < DIAG_LOG_MAX + 20; i++) {
            off += (size_t)snprintf(big + off, sizeof(big) - off, "%s{\"ts\":\"2026-09-24T13:00:00.000Z\","
                                    "\"source\":\"s\",\"level\":\"info\",\"message\":\"m\"}", i ? "," : "");
        }
        snprintf(big + off, sizeof(big) - off, "]}");
        apply(diag_view_apply_logs, &d, big);
        check("more entries than the page holds are cut to its bound", d.log_count == DIAG_LOG_MAX);
    }

    printf("diag_view_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
