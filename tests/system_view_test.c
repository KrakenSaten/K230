/*
 * System Status presentation tests: what the screen decides, without LVGL.
 *
 * The cases here are the ones that go wrong in a status screen - a null read
 * as a zero, a dropped poll blanking the numbers, a destructive action fired
 * before anyone confirmed it - and they are testable because those decisions
 * live in apps/system/system_view.c rather than in the LVGL callbacks.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "system_view.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static cJSON *parse(const char *json)
{
    cJSON *o = cJSON_Parse(json);

    if (!o) {
        fprintf(stderr, "bad fixture: %s\n", json);
    }
    return o;
}

static const struct system_view_metric *vital(const struct system_view *v, const char *label)
{
    int i;

    for (i = 0; i < 6; i++) {
        if (strcmp(v->vitals[i].label, label) == 0) {
            return &v->vitals[i];
        }
    }
    return NULL;
}

static int vital_is(const struct system_view *v, const char *label, const char *want)
{
    const struct system_view_metric *m = vital(v, label);

    return m && strcmp(m->value, want) == 0;
}

static const struct system_view_service *service(const struct system_view *v, const char *name)
{
    int i;

    for (i = 0; i < v->service_count; i++) {
        if (strcmp(v->services[i].name, name) == 0) {
            return &v->services[i];
        }
    }
    return NULL;
}

static int has_iface(const struct system_view *v, const char *name)
{
    int i;

    for (i = 0; i < v->iface_count; i++) {
        if (strcmp(v->ifaces[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Unit A's own answer, trimmed to what this screen reads. */
static const char status_json[] =
    "{\"uptime_s\":25488,\"load\":[0.15,0.04,0.01],\"cpu_percent\":21.4,"
    "\"memory\":{\"total_kb\":990544,\"available_kb\":929580,\"free_kb\":902596},"
    "\"temperature_c\":46.753,\"clock_set\":true,"
    "\"storage\":[{\"mount\":\"/\",\"total_bytes\":602017792,\"avail_bytes\":137756672},"
    "{\"mount\":\"/boot\",\"total_bytes\":72842240,\"avail_bytes\":48829440}],"
    "\"network\":[{\"name\":\"eth0\",\"operstate\":\"up\",\"carrier\":true,"
    "\"mac\":\"00:e0:4c:3a:5e:d0\",\"ipv4\":\"192.168.10.157\"},"
    "{\"name\":\"sit0\",\"operstate\":\"down\",\"carrier\":null,"
    "\"mac\":\"00:00:00:00\",\"ipv4\":null},"
    "{\"name\":\"wlan0\",\"operstate\":\"down\",\"carrier\":null,"
    "\"mac\":\"88:3b:dc:b7:9e:c7\",\"ipv4\":null},"
    "{\"name\":\"wlan1\",\"operstate\":\"down\",\"carrier\":null,"
    "\"mac\":\"8a:3b:dc:b7:9c:c7\",\"ipv4\":null}],"
    "\"power\":{\"source\":\"external\",\"supplies\":[]},"
    "\"services\":[{\"name\":\"pocketos-shell\",\"pid\":274,\"running\":true,"
    "\"crashloop\":false,\"last_exit_code\":null,\"restarts\":0},"
    "{\"name\":\"radiod\",\"pid\":253,\"running\":true,\"crashloop\":false,"
    "\"last_exit_code\":null,\"restarts\":0},"
    "{\"name\":\"sysd\",\"pid\":235,\"running\":true,\"crashloop\":false,"
    "\"last_exit_code\":null,\"restarts\":0}]}";

int main(void)
{
    struct system_view v;
    cJSON *o;
    const struct system_view_service *s;
    int i;

    /* ---- a board that answers everything ---- */
    system_view_init(&v);
    o = parse(status_json);
    if (!o) {
        return 1;
    }
    system_view_apply_status(&v, o, 10000);
    cJSON_Delete(o);

    check("cpu is rounded to whole percent", vital_is(&v, "CPU", "21 %"));
    check("temperature keeps one decimal", vital_is(&v, "TEMP", "46.8 \xC2\xB0" "C"));
    check("memory is available over total in MB", vital_is(&v, "MEMORY", "907/967 MB"));
    check("load shows the one-minute figure", vital_is(&v, "LOAD", "0.15"));
    check("uptime under a day is hours and minutes", vital_is(&v, "UPTIME", "7h 04m"));
    check("a synced clock says so", vital_is(&v, "CLOCK", "synced"));
    check("a synced clock is not a warning", vital(&v, "CLOCK")->warn == 0);
    check("a fresh poll is LIVE", strcmp(v.freshness, "LIVE") == 0 && v.stale == 0);

    check("both mounts are listed", v.mount_count == 2);
    check("the root mount is first", strcmp(v.mounts[0].mount, "/") == 0);
    check("free of total is spelled out",
          strcmp(v.mounts[0].detail, "131 MB free of 574 MB") == 0);
    check("the used share is a real percentage",
          v.mounts[0].have_percent && v.mounts[0].used_percent == 77);

    /* A build host's disk is three orders of magnitude larger than the card's
     * and the row is the same width, so a big volume changes units instead of
     * losing digits off the end. */
    o = parse("{\"storage\":[{\"mount\":\"/\",\"total_bytes\":1080863910568,"
              "\"avail_bytes\":989560529715}]}");
    if (!o) {
        return 1;
    }
    system_view_apply_status(&v, o, 11000);
    cJSON_Delete(o);
    check("a volume of 10 GB or more is shown in GB",
          strcmp(v.mounts[0].detail, "921.6 GB free of 1006.6 GB") == 0);
    check("and still meters correctly", v.mounts[0].have_percent && v.mounts[0].used_percent == 8);
    o = parse(status_json);
    if (!o) {
        return 1;
    }
    system_view_apply_status(&v, o, 12000);
    cJSON_Delete(o);
    check("the card's own volumes stay in MB",
          strcmp(v.mounts[0].detail, "131 MB free of 574 MB") == 0);

    /* ---- the network filter ---- */
    check("eth0 is shown", has_iface(&v, "eth0"));
    check("wlan0 is shown", has_iface(&v, "wlan0"));
    check("wlan1 is shown", has_iface(&v, "wlan1"));
    check("sit0 is hidden: its MAC is not six octets", !has_iface(&v, "sit0"));
    check("exactly one interface was hidden", v.ifaces_hidden == 1);
    check("three interfaces remain", v.iface_count == 3);
    check("an interface that is up says UP", strcmp(v.ifaces[0].state, "UP") == 0 && v.ifaces[0].up);
    check("its address is shown", strcmp(v.ifaces[0].addr, "192.168.10.157") == 0);
    check("an interface with no address shows the unknown dash",
          strcmp(v.ifaces[1].addr, SYSTEM_VIEW_UNKNOWN) == 0);

    /* ---- services ---- */
    check("all three services are listed", v.service_count == 3);
    s = service(&v, "sysd");
    check("a running service is RUNNING", s && s->state == SYSTEM_VIEW_SVC_RUNNING);
    check("a running service shows its pid", s && strcmp(s->detail, "pid 235") == 0);

    /* ---- null is unknown, zero is a measurement ---- */
    o = parse("{\"cpu_percent\":0,\"temperature_c\":null,\"memory\":null,\"load\":null,"
              "\"uptime_s\":null,\"clock_set\":false,\"storage\":[],\"network\":[],"
              "\"services\":[]}");
    if (!o) {
        return 1;
    }
    system_view_apply_status(&v, o, 20000);
    cJSON_Delete(o);
    check("cpu_percent 0 renders as 0 %, not unknown", vital_is(&v, "CPU", "0 %"));
    check("a null temperature is the unknown dash",
          vital_is(&v, "TEMP", SYSTEM_VIEW_UNKNOWN));
    check("null memory is the unknown dash", vital_is(&v, "MEMORY", SYSTEM_VIEW_UNKNOWN));
    check("a null load is the unknown dash", vital_is(&v, "LOAD", SYSTEM_VIEW_UNKNOWN));
    check("a null uptime is the unknown dash", vital_is(&v, "UPTIME", SYSTEM_VIEW_UNKNOWN));
    check("an unset clock says not set", vital_is(&v, "CLOCK", "not set"));
    check("an unset clock is a warning", vital(&v, "CLOCK")->warn == 1);
    check("an empty storage array reports no mounts", v.mount_count == 0);
    check("an empty network array hides nothing", v.iface_count == 0 && v.ifaces_hidden == 0);

    /* Half-known memory: the part that is known is still shown. */
    o = parse("{\"memory\":{\"total_kb\":990544,\"available_kb\":null}}");
    if (!o) {
        return 1;
    }
    system_view_apply_status(&v, o, 21000);
    cJSON_Delete(o);
    check("a known total survives an unknown available",
          vital_is(&v, "MEMORY", SYSTEM_VIEW_UNKNOWN "/967 MB"));

    /* ---- a storage row that cannot be metered ---- */
    o = parse("{\"storage\":[{\"mount\":\"/bad\",\"total_bytes\":0,\"avail_bytes\":0},"
              "{\"mount\":\"/weird\",\"total_bytes\":100,\"avail_bytes\":900},"
              "{\"mount\":\"/null\",\"total_bytes\":null,\"avail_bytes\":null}]}");
    if (!o) {
        return 1;
    }
    system_view_apply_status(&v, o, 22000);
    cJSON_Delete(o);
    check("a zero total draws no meter and does not divide", !v.mounts[0].have_percent);
    check("a zero total still says something", strlen(v.mounts[0].detail) > 0);
    check("free larger than total draws no meter", !v.mounts[1].have_percent);
    check("null sizes give the unknown dash",
          strcmp(v.mounts[2].detail, SYSTEM_VIEW_UNKNOWN) == 0 && !v.mounts[2].have_percent);

    /* ---- service states ---- */
    o = parse("{\"services\":["
              "{\"name\":\"looping\",\"pid\":null,\"running\":false,\"crashloop\":true,"
              "\"last_exit_code\":139,\"restarts\":6},"
              "{\"name\":\"loopingunknown\",\"pid\":null,\"running\":false,\"crashloop\":true,"
              "\"last_exit_code\":null,\"restarts\":4},"
              "{\"name\":\"stopped\",\"pid\":null,\"running\":false,\"crashloop\":false,"
              "\"last_exit_code\":null,\"restarts\":null}]}");
    if (!o) {
        return 1;
    }
    system_view_apply_status(&v, o, 23000);
    cJSON_Delete(o);
    s = service(&v, "looping");
    check("a crash-looped service is CRASH LOOP", s && s->state == SYSTEM_VIEW_SVC_CRASHLOOP);
    check("it names the exit code and the restart window",
          s && strcmp(s->detail, "exit 139 \xC2\xB7 6 restarts/60s") == 0);
    s = service(&v, "loopingunknown");
    check("an unknown exit code is left out rather than dashed",
          s && strcmp(s->detail, "4 restarts/60s") == 0);
    s = service(&v, "stopped");
    check("a stopped service is STOPPED", s && s->state == SYSTEM_VIEW_SVC_STOPPED);
    check("a stopped service invents no reason",
          s && strcmp(s->detail, SYSTEM_VIEW_UNKNOWN) == 0);

    /* ---- a failed poll keeps the screen, and the age keeps counting ---- */
    system_view_init(&v);
    o = parse(status_json);
    if (!o) {
        return 1;
    }
    system_view_apply_status(&v, o, 100000);
    cJSON_Delete(o);
    check("a good poll is LIVE", strcmp(v.freshness, "LIVE") == 0);
    system_view_apply_status(&v, NULL, 102000);
    check("a failed poll keeps the last CPU value", vital_is(&v, "CPU", "21 %"));
    check("a failed poll keeps the services", v.service_count == 3);
    check("a failed poll inside the window is still LIVE", strcmp(v.freshness, "LIVE") == 0);
    system_view_apply_status(&v, NULL, 108000);
    check("a poll that has been missing for 8 s is STALE",
          v.stale && strcmp(v.freshness, "STALE 8s") == 0);
    check("stale does not blank the values", vital_is(&v, "CPU", "21 %"));
    system_view_refresh_freshness(&v, 112000);
    check("the age keeps counting without a new poll", strcmp(v.freshness, "STALE 12s") == 0);
    o = parse(status_json);
    if (!o) {
        return 1;
    }
    system_view_apply_status(&v, o, 120000);
    cJSON_Delete(o);
    check("a later good poll recovers to LIVE", !v.stale && strcmp(v.freshness, "LIVE") == 0);

    /* ---- identity, and the card row that only appears when it matters ---- */
    system_view_init(&v);
    o = parse("{\"version\":\"0.0.7\",\"build\":\"fdc795f\",\"release_file\":\"0.0.7\","
              "\"release_build\":\"fdc795f\",\"model\":\"Canaan CanMV-K230 with RM69A10 OLED\","
              "\"kernel\":\"6.6.36\",\"machine\":\"riscv64\"}");
    if (!o) {
        return 1;
    }
    system_view_apply_info(&v, o);
    cJSON_Delete(o);
    check("the running build is version and build", strcmp(v.os_version, "0.0.7 \xC2\xB7 fdc795f") == 0);
    check("the card row is hidden when the card agrees", v.show_card == 0);
    check("the model is shown", strcmp(v.model, "Canaan CanMV-K230 with RM69A10 OLED") == 0);
    check("the kernel carries the machine", strcmp(v.kernel, "6.6.36 riscv64") == 0);

    o = parse("{\"version\":\"0.0.7\",\"build\":\"fdc795f\",\"release_file\":\"0.0.6\","
              "\"release_build\":\"e4682f6\"}");
    if (!o) {
        return 1;
    }
    system_view_apply_info(&v, o);
    cJSON_Delete(o);
    check("the card row appears when the card disagrees", v.show_card == 1);
    check("it shows what the card carries", strcmp(v.card_version, "0.0.6 \xC2\xB7 e4682f6") == 0);

    o = parse("{\"version\":\"0.0.7\",\"build\":\"fdc795f\",\"release_file\":null,"
              "\"release_build\":null}");
    if (!o) {
        return 1;
    }
    system_view_apply_info(&v, o);
    cJSON_Delete(o);
    check("a card with no release file disagrees and is shown", v.show_card == 1);
    check("and says it is unknown", strcmp(v.card_version, SYSTEM_VIEW_UNKNOWN) == 0);

    system_view_init(&v);
    system_view_apply_info(&v, NULL);
    check("a failed system.info leaves identity unknown rather than wrong",
          strcmp(v.os_version, SYSTEM_VIEW_UNKNOWN) == 0 && v.have_info == 0);

    /* ---- radio, from what the status bar already polled ---- */
    system_view_set_radio_detail(&v, "EU868", "mock");
    system_view_set_radio_state(&v, "rx");
    check("the radio chip is upper case", strcmp(v.radio_state, "RX") == 0);
    check("the radio detail is region and backend",
          strcmp(v.radio_detail, "EU868 \xC2\xB7 mock") == 0);
    system_view_set_radio_state(&v, NULL);
    check("an unreachable radiod shows -- and not an em dash the font lacks",
          strcmp(v.radio_state, "--") == 0);
    check("and losing the state does not lose the region and backend",
          strcmp(v.radio_detail, "EU868 · mock") == 0);

    /* ---- nothing destructive happens without a confirmation ---- */
    system_view_init(&v);
    check("nothing is pending on a fresh screen", system_view_confirm(&v) == NULL);
    system_view_request(&v, SYSTEM_VIEW_ACTION_REBOOT);
    check("asking to restart raises the dialog", v.phase == SYSTEM_VIEW_CONFIRM_REBOOT);
    check("the restart dialog is titled",
          strcmp(system_view_dialog_title(&v), "Restart Doors?") == 0);
    check("the restart dialog says what will happen",
          strstr(system_view_dialog_body(&v), "board reboots") != NULL);
    check("the screen is still polling while the dialog is up", system_view_is_polling(&v));
    system_view_cancel(&v);
    check("cancelling returns to the live screen", v.phase == SYSTEM_VIEW_LIVE);
    check("cancelling leaves nothing to call", system_view_confirm(&v) == NULL);

    system_view_request(&v, SYSTEM_VIEW_ACTION_REBOOT);
    check("confirming a restart yields system.reboot",
          strcmp(system_view_confirm(&v), "system.reboot") == 0);
    system_view_action_ok(&v);
    check("an accepted restart is terminal", v.phase == SYSTEM_VIEW_TERMINAL_REBOOT);
    check("a terminal screen stops polling", !system_view_is_polling(&v));
    check("a terminal screen says what it is doing",
          strcmp(system_view_terminal_text(&v), "Restarting...") == 0);
    check("a terminal screen has nothing left to call", system_view_confirm(&v) == NULL);
    system_view_request(&v, SYSTEM_VIEW_ACTION_POWEROFF);
    check("a terminal screen ignores further requests", v.phase == SYSTEM_VIEW_TERMINAL_REBOOT);

    system_view_init(&v);
    system_view_request(&v, SYSTEM_VIEW_ACTION_POWEROFF);
    check("the power-off dialog is titled",
          strcmp(system_view_dialog_title(&v), "Power off Doors?") == 0);
    check("the power-off dialog warns that it is not remotely recoverable",
          strstr(system_view_dialog_body(&v), "cannot be restarted remotely") != NULL);
    check("and gives the unplug instruction",
          strstr(system_view_dialog_body(&v), "30 seconds") != NULL);
    check("its confirm button says Power off",
          strcmp(system_view_dialog_confirm_label(&v), "Power off") == 0);
    check("confirming a power-off yields system.poweroff",
          strcmp(system_view_confirm(&v), "system.poweroff") == 0);
    system_view_action_ok(&v);
    check("the power-off terminal repeats the unplug instruction",
          strstr(system_view_terminal_text(&v), "30 seconds") != NULL);

    /* ---- which button the glass emphasises ----
     *
     * On a touch-only panel there is no input group and so no focus ring: the
     * styling is the whole of what says which choice is safe. Observed wrong
     * on unit A 2026-09-10, where the accent sat on "Power off". */
    system_view_init(&v);
    system_view_request(&v, SYSTEM_VIEW_ACTION_REBOOT);
    check("a restart keeps the accent on the action",
          system_view_dialog_emphasis(&v) == SYSTEM_VIEW_EMPHASIS_CONFIRM);
    check("and its wording is unchanged",
          strcmp(system_view_dialog_title(&v), "Restart Doors?") == 0 &&
              strstr(system_view_dialog_body(&v), "about 35 seconds") != NULL &&
              strcmp(system_view_dialog_confirm_label(&v), "Restart") == 0);
    system_view_cancel(&v);
    system_view_request(&v, SYSTEM_VIEW_ACTION_POWEROFF);
    check("a power-off moves the accent to Cancel",
          system_view_dialog_emphasis(&v) == SYSTEM_VIEW_EMPHASIS_CANCEL);
    check("and its wording is unchanged too",
          strcmp(system_view_dialog_title(&v), "Power off Doors?") == 0 &&
              strstr(system_view_dialog_body(&v), "cannot be restarted remotely") != NULL &&
              strstr(system_view_dialog_body(&v), "30 seconds") != NULL &&
              strcmp(system_view_dialog_confirm_label(&v), "Power off") == 0);
    check("emphasis alone still fires nothing", system_view_confirm(&v) != NULL &&
                                                    v.phase == SYSTEM_VIEW_CONFIRM_POWEROFF);
    system_view_cancel(&v);
    check("and cancelling from there leaves nothing to call",
          v.phase == SYSTEM_VIEW_LIVE && system_view_confirm(&v) == NULL);

    /* ---- radiod unreachable: configuration survives, live state does not ---- */
    system_view_init(&v);
    system_view_set_radio_detail(&v, "EU868", "mock");
    system_view_set_radio_state(&v, "rx");
    check("a reachable radiod is known", v.radio_state_known == 1);
    system_view_set_radio_state(&v, NULL);
    check("an unreachable radiod is not known", v.radio_state_known == 0);
    check("its chip goes to --", strcmp(v.radio_state, "--") == 0);
    check("but the region and backend are still there",
          strcmp(v.radio_detail, "EU868 \xC2\xB7 mock") == 0);
    check("the row is not blanked", v.radio_detail[0] != '\0' &&
                                        strcmp(v.radio_detail, SYSTEM_VIEW_UNKNOWN) != 0);
    system_view_set_radio_state(&v, "tx");
    check("and a reachable radiod is known again",
          v.radio_state_known == 1 && strcmp(v.radio_state, "TX") == 0);

    /* ---- the chip's treatment, which is the row's only live signal ----
     * Measured on the simulator 2026-09-10: the chip was built once in the
     * unavailable treatment and only its text was ever repainted, so a
     * recovered radio read RX in the same grey it wore while radiod was gone
     * (#8d99a6 on #10151b in both states). The text was right and the colour
     * said nothing, which is why the row looked unchanged on the panel. */
    system_view_init(&v);
    check("a radio nobody has polled yet is unknown",
          system_view_radio_chip_state(&v) == SYSTEM_VIEW_RADIO_UNKNOWN);
    system_view_set_radio_state(&v, "rx");
    check("receiving takes the rx treatment",
          system_view_radio_chip_state(&v) == SYSTEM_VIEW_RADIO_RX);
    system_view_set_radio_state(&v, "tx");
    check("sending takes the tx treatment",
          system_view_radio_chip_state(&v) == SYSTEM_VIEW_RADIO_TX);
    system_view_set_radio_state(&v, "idle");
    check("a radio that answered but is doing neither is idle, not unknown",
          system_view_radio_chip_state(&v) == SYSTEM_VIEW_RADIO_IDLE);
    system_view_set_radio_state(&v, NULL);
    check("an unreachable radiod is unknown, which is not the same as idle",
          system_view_radio_chip_state(&v) == SYSTEM_VIEW_RADIO_UNKNOWN);
    system_view_set_radio_state(&v, "rx");
    check("and the treatment comes back with the radio",
          system_view_radio_chip_state(&v) == SYSTEM_VIEW_RADIO_RX);

    /* ---- an action that failed leaves a usable screen ---- */
    system_view_init(&v);
    o = parse(status_json);
    if (!o) {
        return 1;
    }
    system_view_apply_status(&v, o, 5000);
    cJSON_Delete(o);
    system_view_request(&v, SYSTEM_VIEW_ACTION_REBOOT);
    (void)system_view_confirm(&v);
    system_view_action_failed(&v, "reboot already pending");
    check("a refused action returns to the live screen", v.phase == SYSTEM_VIEW_LIVE);
    check("it keeps polling", system_view_is_polling(&v));
    check("it carries the reason", strcmp(v.error, "reboot already pending") == 0);
    check("it did not blank the screen", vital_is(&v, "CPU", "21 %"));
    check("and there is nothing left pending", system_view_confirm(&v) == NULL);
    system_view_action_failed(&v, "");
    check("a failure with no message does not overwrite the live phase",
          v.phase == SYSTEM_VIEW_LIVE);

    system_view_init(&v);
    system_view_request(&v, SYSTEM_VIEW_ACTION_POWEROFF);
    (void)system_view_confirm(&v);
    system_view_action_failed(&v, NULL);
    check("a transport failure says something useful",
          strcmp(v.error, "sysd is not answering") == 0);

    /* ---- values that are too long, too many or absurd ---- */
    system_view_init(&v);
    o = parse("{\"uptime_s\":98765432,\"cpu_percent\":100,"
              "\"memory\":{\"total_kb\":99999999999,\"available_kb\":1},"
              "\"services\":[{\"name\":"
              "\"a-service-name-far-longer-than-any-real-one-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
              "\"pid\":4194304,\"running\":true,\"crashloop\":false,"
              "\"last_exit_code\":null,\"restarts\":0}],"
              "\"network\":[{\"name\":\"an-interface-name-longer-than-IFNAMSIZ-ever-allows\","
              "\"operstate\":\"an-operstate-nobody-has-ever-seen\","
              "\"mac\":\"00:11:22:33:44:55\",\"ipv4\":\"255.255.255.255\"}]}");
    if (!o) {
        return 1;
    }
    system_view_apply_status(&v, o, 30000);
    cJSON_Delete(o);
    check("a very long uptime becomes days and hours", vital_is(&v, "UPTIME", "1143d 2h"));
    check("100 % is not rounded away", vital_is(&v, "CPU", "100 %"));
    check("a long service name is truncated, not overflowed",
          v.service_count == 1 && strlen(v.services[0].name) < sizeof(v.services[0].name));
    check("a long interface name is truncated, not overflowed",
          v.iface_count == 1 && strlen(v.ifaces[0].name) < sizeof(v.ifaces[0].name));
    check("a long operstate is truncated, not overflowed",
          strlen(v.ifaces[0].state) < sizeof(v.ifaces[0].state));

    /* More rows than the screen holds: keep what fits, drop the rest, and do
     * not write past the arrays. */
    {
        char big[4096];
        size_t used = 0;

        used += (size_t)snprintf(big + used, sizeof(big) - used, "{\"services\":[");
        for (i = 0; i < 40; i++) {
            used += (size_t)snprintf(big + used, sizeof(big) - used,
                                     "%s{\"name\":\"svc%d\",\"pid\":%d,\"running\":true,"
                                     "\"crashloop\":false,\"last_exit_code\":null,"
                                     "\"restarts\":0}",
                                     i ? "," : "", i, 100 + i);
        }
        snprintf(big + used, sizeof(big) - used, "]}");
        o = parse(big);
        if (!o) {
            return 1;
        }
        system_view_apply_status(&v, o, 31000);
        cJSON_Delete(o);
        check("more services than fit are capped at the array size",
              v.service_count == SYSTEM_VIEW_MAX_SERVICES);
    }

    /* A status object with nothing in it at all must not crash or invent. */
    o = parse("{}");
    if (!o) {
        return 1;
    }
    system_view_apply_status(&v, o, 32000);
    cJSON_Delete(o);
    check("an empty status leaves every vital unknown",
          vital_is(&v, "CPU", SYSTEM_VIEW_UNKNOWN) && vital_is(&v, "MEMORY", SYSTEM_VIEW_UNKNOWN));
    check("and no rows at all", v.mount_count == 0 && v.iface_count == 0 && v.service_count == 0);

    /* ---- DS §52.5: traffic, the links, more identity ---- */
    {
        struct system_view n;
        char b[32];

        system_view_init(&n);
        check("links start as dashes", strcmp(n.wifi, SYSTEM_VIEW_UNKNOWN) == 0 &&
                                           strcmp(n.mesh, SYSTEM_VIEW_UNKNOWN) == 0 &&
                                           strcmp(n.radio_packets, SYSTEM_VIEW_UNKNOWN) == 0);
        o = parse("{\"network\":[{\"name\":\"eth0\",\"operstate\":\"up\",\"mac\":\"00:e0:4c:3a:5e:d0\","
                  "\"ipv4\":\"10.0.0.2\",\"rx_bytes\":1048576,\"tx_bytes\":2048},"
                  "{\"name\":\"wlan0\",\"operstate\":\"down\",\"mac\":\"88:3b:dc:b7:9e:c7\",\"ipv4\":null,"
                  "\"rx_bytes\":null,\"tx_bytes\":null}]}");
        if (!o) {
            return 1;
        }
        system_view_apply_status(&n, o, 10000);
        cJSON_Delete(o);
        check("first answer: totals only, no rate yet", strcmp(n.ifaces[0].traffic, "1.0 MB in \xC2\xB7 2 kB out") == 0);
        check("an interface without counters: the dash", strcmp(n.ifaces[1].traffic, SYSTEM_VIEW_UNKNOWN) == 0);
        o = parse("{\"network\":[{\"name\":\"eth0\",\"operstate\":\"up\",\"mac\":\"00:e0:4c:3a:5e:d0\","
                  "\"ipv4\":\"10.0.0.2\",\"rx_bytes\":1069056,\"tx_bytes\":6144}]}");
        system_view_apply_status(&n, o, 12000);
        cJSON_Delete(o);
        check("two seconds later: the rate over them (10.0 and 2.0 KB/s) and the totals",
              strcmp(n.ifaces[0].traffic, "\xE2\x86\x93" "10 \xE2\x86\x91" "2.0 KB/s \xC2\xB7 1.0 MB in \xC2\xB7 6 kB out") == 0);
        if (strstr(n.ifaces[0].traffic, "KB/s") == NULL) {
            printf("     traffic: %s\n", n.ifaces[0].traffic);
        }
        o = parse("{\"network\":[{\"name\":\"eth0\",\"operstate\":\"up\",\"mac\":\"00:e0:4c:3a:5e:d0\","
                  "\"ipv4\":\"10.0.0.2\",\"rx_bytes\":100,\"tx_bytes\":100}]}");
        system_view_apply_status(&n, o, 14000);
        cJSON_Delete(o);
        check("a counter that went down: no rate from it, never a wrapped one",
              strstr(n.ifaces[0].traffic, "KB/s") == NULL && strstr(n.ifaces[0].traffic, " in ") != NULL);
        system_view_apply_status(&n, NULL, 16000);
        check("sysd silent: the line stays as it was", strstr(n.ifaces[0].traffic, " in ") != NULL);

        system_view_bytes(512, b, sizeof(b));
        check("bytes: under a kilobyte", strcmp(b, "0 kB") == 0 || strcmp(b, "1 kB") == 0);
        system_view_bytes(1.5 * 1024 * 1024 * 1024, b, sizeof(b));
        check("bytes: gigabytes", strcmp(b, "1.5 GB") == 0);
        system_view_bytes(-1, b, sizeof(b));
        check("bytes: no count is the dash", strcmp(b, SYSTEM_VIEW_UNKNOWN) == 0);

        o = parse("{\"tx_packets\":1,\"rx_packets\":848,\"rx_crc_errors\":36,\"last_rssi_dbm\":-74,\"last_snr_db\":12.25}");
        system_view_apply_radio_stats(&n, o);
        cJSON_Delete(o);
        check("radio packets in words", strcmp(n.radio_packets, "848 received \xC2\xB7 1 sent \xC2\xB7 36 CRC errors") == 0);
        check("and the last packet's signal", strcmp(n.radio_signal, "Last packet -74 dBm, SNR 12.2 dB") == 0 ||
                                                  strcmp(n.radio_signal, "Last packet -74 dBm, SNR 12.3 dB") == 0);
        o = parse("{\"tx_packets\":0,\"rx_packets\":0,\"last_rssi_dbm\":0,\"last_snr_db\":0}");
        system_view_apply_radio_stats(&n, o);
        cJSON_Delete(o);
        check("nothing heard yet: no signal claimed", strcmp(n.radio_signal, "No packet received yet") == 0);
        system_view_apply_radio_stats(&n, NULL);
        check("radiod silent: said so, nothing else", strcmp(n.radio_packets, "radiod not answering") == 0 &&
                                                          n.radio_signal[0] == '\0');

        o = parse("{\"available\":true,\"enabled\":true,\"state\":\"connected\",\"ssid\":\"Home\",\"signal_bars\":3}");
        system_view_apply_wifi(&n, o);
        cJSON_Delete(o);
        check("Wi-Fi connected", strcmp(n.wifi, "Connected to Home, signal 3/4") == 0);
        o = parse("{\"available\":true,\"enabled\":false,\"state\":\"off\"}");
        system_view_apply_wifi(&n, o);
        cJSON_Delete(o);
        check("Wi-Fi off", strcmp(n.wifi, "Off") == 0);
        o = parse("{\"available\":false,\"enabled\":false,\"state\":\"unavailable\"}");
        system_view_apply_wifi(&n, o);
        cJSON_Delete(o);
        check("no Wi-Fi hardware", strcmp(n.wifi, "No Wi-Fi hardware") == 0);
        system_view_apply_wifi(&n, NULL);
        check("netd silent", strcmp(n.wifi, "Wi-Fi service is not running") == 0);

        o = parse("{\"state\":\"online\"}");
        system_view_apply_mesh(&n, o);
        cJSON_Delete(o);
        check("mesh online", strcmp(n.mesh, "Online") == 0 && !n.mesh_warn);
        o = parse("{\"state\":\"degraded\",\"reason\":\"the radio is switched off\",\"radio\":{\"radio_state\":\"off\"}}");
        system_view_apply_mesh(&n, o);
        cJSON_Delete(o);
        check("mesh waiting on a switched-off radio is not a fault",
              strcmp(n.mesh, "Waiting: the radio is switched off") == 0 && !n.mesh_warn);
        o = parse("{\"state\":\"error\",\"reason\":\"radiod unavailable\"}");
        system_view_apply_mesh(&n, o);
        cJSON_Delete(o);
        check("mesh error warns", strcmp(n.mesh, "error: radiod unavailable") == 0 && n.mesh_warn);
        system_view_apply_mesh(&n, NULL);
        check("meshcored off by default: the ordinary state", strcmp(n.mesh, "meshcored not running") == 0 &&
                                                                  !n.mesh_warn);

        o = parse("{\"version\":\"0.3.0\",\"build\":\"abc1234\",\"os\":\"Buildroot 2025.02.1\","
                  "\"vendor_sdk\":\"v1.2-20260909-22d02c6\",\"cpus\":1}");
        system_view_apply_info(&n, o);
        cJSON_Delete(o);
        check("about: platform, SDK and CPUs from system.info",
              strcmp(n.platform, "Buildroot 2025.02.1") == 0 && strcmp(n.sdk, "v1.2-20260909-22d02c6") == 0 &&
                  strcmp(n.cpus, "1") == 0);
    }

    /* The microSD card's expansion (storage.status "internal"). */
    {
        struct system_view e;
        cJSON *o;

        system_view_init(&e);
        check("expand: nothing known, nothing offered", e.expand == SYSTEM_VIEW_EXPAND_UNKNOWN && !e.expand_line[0]);
        system_view_request(&e, SYSTEM_VIEW_ACTION_EXPAND);
        check("expand: not offered, so not even asked", e.phase == SYSTEM_VIEW_LIVE);

        /* Unit B's card as flashed: 14.6 GB, 600 MiB used by the root. */
        o = parse("{\"usb\":{\"state\":\"absent\"},\"internal\":{\"state\":\"available\",\"device\":\"/dev/mmcblk1p2\","
                  "\"disk_bytes\":15634268160,\"partition_bytes\":629145600,\"filesystem_bytes\":629145600,"
                  "\"unused_bytes\":14871953408,\"can_expand\":true,\"reason\":null,\"error\":null,\"done\":false}}");
        system_view_apply_storage(&e, o);
        cJSON_Delete(o);
        check("available: offered, with what is unused",
              e.expand == SYSTEM_VIEW_EXPAND_OFFER && strcmp(e.expand_line, "13.9 GB of the card is not used yet") == 0 &&
                  !e.expand_warn);
        system_view_request(&e, SYSTEM_VIEW_ACTION_EXPAND);
        check("Expand asks first", e.phase == SYSTEM_VIEW_CONFIRM_EXPAND && system_view_is_polling(&e));
        check("the confirmation names it and what it adds",
              strcmp(system_view_dialog_title(&e), "Expand storage?") == 0 &&
                  strstr(system_view_dialog_body(&e), "adding 13.9 GB") != NULL &&
                  strstr(system_view_dialog_body(&e), "cannot be undone") != NULL &&
                  strcmp(system_view_dialog_confirm_label(&e), "Expand") == 0);
        check("the accent is on Cancel: it cannot be undone",
              system_view_dialog_emphasis(&e) == SYSTEM_VIEW_EMPHASIS_CANCEL);
        system_view_cancel(&e);
        check("Cancel calls nothing", e.phase == SYSTEM_VIEW_LIVE && system_view_confirm(&e) == NULL);
        system_view_request(&e, SYSTEM_VIEW_ACTION_EXPAND);
        check("confirming hands over storage.expand", strcmp(system_view_confirm(&e), "storage.expand") == 0);
        system_view_action_ok(&e);
        check("accepted: live again, shown running, still polling",
              e.phase == SYSTEM_VIEW_LIVE && e.expand == SYSTEM_VIEW_EXPAND_RUNNING && system_view_is_polling(&e) &&
                  strstr(e.expand_line, "Expanding storage") != NULL);
        system_view_request(&e, SYSTEM_VIEW_ACTION_EXPAND);
        check("running: not offered again", e.phase == SYSTEM_VIEW_LIVE);

        o = parse("{\"internal\":{\"state\":\"reboot_required\",\"error\":null,\"done\":false}}");
        system_view_apply_storage(&e, o);
        cJSON_Delete(o);
        check("reboot_required: says to restart", e.expand == SYSTEM_VIEW_EXPAND_RESTART &&
                                                    strcmp(e.expand_line, "Restart to finish expanding storage") == 0);
        system_view_apply_storage(&e, NULL);
        check("a missed poll keeps the line", e.expand == SYSTEM_VIEW_EXPAND_RESTART && e.expand_line[0]);

        o = parse("{\"internal\":{\"state\":\"finish\",\"partition_bytes\":15005122560,"
                  "\"filesystem_bytes\":629145600,\"error\":null,\"done\":false}}");
        system_view_apply_storage(&e, o);
        cJSON_Delete(o);
        check("finish: offered, with what the filesystem has not taken",
              e.expand == SYSTEM_VIEW_EXPAND_OFFER && strcmp(e.expand_line, "13.4 GB of the card is not used yet") == 0);

        o = parse("{\"internal\":{\"state\":\"available\",\"unused_bytes\":14871953408,"
                  "\"error\":\"The partition could not be grown (parted).\",\"done\":false}}");
        system_view_apply_storage(&e, o);
        cJSON_Delete(o);
        check("a failed expansion: still offered, the reason shown as a warning",
              e.expand == SYSTEM_VIEW_EXPAND_OFFER && e.expand_warn && strstr(e.expand_line, "parted") != NULL);
        system_view_request(&e, SYSTEM_VIEW_ACTION_EXPAND);
        system_view_action_failed(&e, "the storage expansion is already running");
        check("a refused call: live, with sysd's reason",
              e.phase == SYSTEM_VIEW_LIVE && strcmp(e.error, "the storage expansion is already running") == 0);

        o = parse("{\"internal\":{\"state\":\"not_needed\",\"error\":null,\"done\":true}}");
        system_view_apply_storage(&e, o);
        cJSON_Delete(o);
        check("done: nothing offered, and it says so",
              e.expand == SYSTEM_VIEW_EXPAND_NONE && strcmp(e.expand_line, "Storage expanded to use the whole card") == 0);
        o = parse("{\"internal\":{\"state\":\"unsupported\",\"reason\":\"the card is GPT\",\"done\":false}}");
        system_view_apply_storage(&e, o);
        cJSON_Delete(o);
        check("unsupported: nothing offered, nothing said", e.expand == SYSTEM_VIEW_EXPAND_NONE && !e.expand_line[0]);
        o = parse("{\"usb\":{\"state\":\"absent\"}}");
        system_view_apply_storage(&e, o);
        cJSON_Delete(o);
        check("a sysd without the internal card changes nothing", e.expand == SYSTEM_VIEW_EXPAND_NONE);
    }

    printf("system_view_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
