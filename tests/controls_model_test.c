/*
 * DOORS Controls model test (ui/shell/controls_model.c): the tile texts from
 * radiod's state and sysd's answers, the antenna question before every
 * off-to-on, and the layout in both orientations - nothing overlapping,
 * everything inside the content area.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "controls_model.h"

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

static cJSON *status(const char *json)
{
    return cJSON_Parse(json);
}

static int battery_is(const char *json, const char *want)
{
    char out[CONTROLS_LINE_MAX];
    cJSON *st = status(json);
    int ok;

    controls_battery_text(st, out, sizeof(out));
    ok = strcmp(out, want) == 0;
    if (!ok) {
        printf("     got '%s', want '%s'\n", out, want);
    }
    cJSON_Delete(st);
    return ok;
}

static int battery_dot(const char *json)
{
    cJSON *st = status(json);
    int dot = controls_battery_dot(st);

    cJSON_Delete(st);
    return dot;
}

/* The frame the shell gives Controls (DS §36): the whole display, the top
 * edge's rounded corners (30 px; 50 at the top in landscape, DS §21.1), and
 * the status cluster's box on the shell's screens (the chip alone, about
 * 113 px wide, 14 px down, 44 tall, anchored to the top-right corner). */
#define CLUSTER_W 113

static struct controls_frame frame(bool landscape, int32_t w, int32_t h, bool cluster)
{
    struct controls_frame f;
    int32_t corner = landscape ? 50 : 30;

    memset(&f, 0, sizeof(f));
    f.landscape = landscape;
    f.width = w;
    f.height = h;
    f.inset_top_left = corner;
    f.inset_top_right = corner;
    if (cluster) {
        f.keepout.x = w - corner - CLUSTER_W;
        f.keepout.y = 14;
        f.keepout.w = CLUSTER_W;
        f.keepout.h = 44;
    }
    return f;
}

static void check_layout(const char *what, bool landscape, int32_t w, int32_t h)
{
    struct controls_layout l;
    char name[128];
    struct controls_frame f = frame(landscape, w, h, true);
    int rc = controls_layout(&f, &l);
    int i;
    int tiles_ok = 1;

    snprintf(name, sizeof(name), "%s: fits without overlap (%dx%d)", what, (int)w, (int)h);
    check(name, rc == 0);
    for (i = 0; i < CONTROLS_TILE_COUNT; i++) {
        tiles_ok &= l.tile[i].w >= 200 && l.tile[i].h >= 96;
    }
    snprintf(name, sizeof(name), "%s: tiles keep a readable size", what);
    check(name, tiles_ok);
    snprintf(name, sizeof(name), "%s: every tap target is at least 48 px tall", what);
    check(name, l.lock.h >= 48 && l.power.h >= 48 && l.list_row_h >= 48 && l.back.h >= 48);
    snprintf(name, sizeof(name), "%s: the two sliders have their own panels", what);
    check(name, !controls_rects_overlap(&l.brightness, &l.volume) && l.volume.h >= 80);
    snprintf(name, sizeof(name), "%s: the radios come first (Wi-Fi, Bluetooth, Radio)", what);
    check(name, l.tile[CONTROLS_TILE_WIFI].y == l.tile[CONTROLS_TILE_BLUETOOTH].y &&
                    l.tile[CONTROLS_TILE_RADIO].y > l.tile[CONTROLS_TILE_WIFI].y &&
                    l.tile[CONTROLS_TILE_ROTATION].y > l.tile[CONTROLS_TILE_RADIO].y);
    snprintf(name, sizeof(name), "%s: the antenna question is centred and inside", what);
    check(name, l.dialog.x >= 0 && l.dialog.y >= 0 && l.dialog.x + l.dialog.w <= w &&
                    l.dialog.y + l.dialog.h <= h &&
                    l.dialog.x == w - (l.dialog.x + l.dialog.w));
    /* DS §36: the header row lies at the top edge now, with the status
     * cluster at its right end and the rounded corners at both. */
    {
        const struct controls_rect *top[] = { &l.back, &l.header, &l.lock, &l.power };
        int32_t corner = f.inset_top_left;
        struct controls_rect tl = { 0, 0, corner, corner };
        struct controls_rect tr = { w - corner, 0, corner, corner };
        size_t k;
        int clear = 1;
        int corners = 1;

        for (k = 0; k < sizeof(top) / sizeof(top[0]); k++) {
            clear &= !controls_rects_overlap(top[k], &f.keepout);
            corners &= !controls_rects_overlap(top[k], &tl) && !controls_rects_overlap(top[k], &tr);
        }
        for (i = 0; i < CONTROLS_TILE_COUNT; i++) {
            clear &= !controls_rects_overlap(&l.tile[i], &f.keepout);
        }
        snprintf(name, sizeof(name), "%s: nothing lies under the status cluster", what);
        check(name, clear);
        snprintf(name, sizeof(name), "%s: nothing lies in a rounded top corner", what);
        check(name, corners);
        snprintf(name, sizeof(name), "%s: the back button and the tiles share a left edge", what);
        check(name, l.back.x == l.tile[CONTROLS_TILE_WIFI].x);
    }
}

int main(void)
{
    struct controls_radio_flow flow = { CONTROLS_CONFIRM_NONE };
    char out[CONTROLS_LINE_MAX];
    cJSON *st;

    /* ---- the radio tile ---- */
    check("radio off says Off", strcmp(controls_radio_text("off"), "Off") == 0);
    check("radio rx says Receiving, with the dot",
          strcmp(controls_radio_text("rx"), "Receiving") == 0 && controls_radio_dot("rx"));
    check("radio error is on, not receiving, and not idle",
          strcmp(controls_radio_text("error"), "On, not receiving") == 0 && controls_radio_on("error"));
    check("radiod not answering says so, no dot",
          strcmp(controls_radio_text(NULL), "Not answering") == 0 && !controls_radio_dot(NULL));
    check("off has no dot and is not on", !controls_radio_dot("off") && !controls_radio_on("off"));

    /* ---- the antenna question ---- */
    check("a tap on an off radio asks about the antenna",
          controls_radio_tapped(&flow, "off") == CONTROLS_TAP_ASK_ANTENNA &&
              flow.confirm == CONTROLS_CONFIRM_ANTENNA);
    check("a second tap while it is open does nothing",
          controls_radio_tapped(&flow, "off") == CONTROLS_TAP_NOTHING);
    check("Cancel does not switch the radio on", !controls_radio_answer(&flow, false));
    check("and closes the question", flow.confirm == CONTROLS_CONFIRM_NONE);
    check("Enable without an open question does nothing", !controls_radio_answer(&flow, true));
    controls_radio_tapped(&flow, "off");
    check("Enable on the open question switches it on", controls_radio_answer(&flow, true));
    check("and closes it", flow.confirm == CONTROLS_CONFIRM_NONE);
    check("a tap on a receiving radio switches it off with no question",
          controls_radio_tapped(&flow, "rx") == CONTROLS_TAP_SWITCH_OFF &&
              flow.confirm == CONTROLS_CONFIRM_NONE);
    check("a tap on a radio in error switches it off too",
          controls_radio_tapped(&flow, "error") == CONTROLS_TAP_SWITCH_OFF);
    check("a tap while radiod is not answering does nothing",
          controls_radio_tapped(&flow, NULL) == CONTROLS_TAP_NOTHING &&
              flow.confirm == CONTROLS_CONFIRM_NONE);
    controls_radio_tapped(&flow, "off");
    controls_radio_dismiss(&flow);
    check("hiding Controls cancels an open question",
          flow.confirm == CONTROLS_CONFIRM_NONE && !controls_radio_answer(&flow, true));
    check("the question names the antenna and the damage",
          strstr(CONTROLS_ANTENNA_TITLE, "antenna") && strstr(CONTROLS_ANTENNA_BODY, "damage") &&
              strcmp(CONTROLS_ANTENNA_ENABLE, "Enable radio") == 0 &&
              strcmp(CONTROLS_ANTENNA_CANCEL, "Cancel") == 0);

    /* ---- the setup question (radio_setup, 0.3.5) ---- */
    {
        struct controls_setup su;
        char body[400];

        st = cJSON_Parse("{\"state\":\"idle\",\"needed\":true,\"backend\":\"mock\",\"meshcored_enabled\":false}");
        controls_setup_parse(st, &su);
        cJSON_Delete(st);
        check("a fresh card's status: known, needed, idle", su.known && su.needed && !su.running && !su.failed);
        check("a tap on the mock's 'Receiving' asks the setup question, not switch the mock",
              controls_radio_tapped_setup(&flow, "rx", &su) == CONTROLS_TAP_ASK_SETUP &&
                  flow.confirm == CONTROLS_CONFIRM_SETUP);
        check("a second tap while it is open does nothing", controls_radio_tapped_setup(&flow, "rx", &su) == CONTROLS_TAP_NOTHING);
        check("the antenna answer does not answer the setup question",
              !controls_radio_answer(&flow, true) && flow.confirm == CONTROLS_CONFIRM_SETUP);
        check("Cancel starts nothing and closes it", !controls_radio_setup_answer(&flow, false) &&
                                                          flow.confirm == CONTROLS_CONFIRM_NONE);
        controls_radio_tapped_setup(&flow, "off", &su);
        check("an off mock is asked the setup question too", flow.confirm == CONTROLS_CONFIRM_SETUP);
        check("Set up starts it", controls_radio_setup_answer(&flow, true) && flow.confirm == CONTROLS_CONFIRM_NONE);
        check("Set up without an open question does nothing", !controls_radio_setup_answer(&flow, true));
        controls_radio_tapped_setup(&flow, "off", &su);
        controls_radio_dismiss(&flow);
        check("hiding Controls cancels the setup question", !controls_radio_setup_answer(&flow, true));
        check("the setup question carries the antenna warning",
              strstr(CONTROLS_SETUP_BODY, "antenna") && strstr(CONTROLS_SETUP_BODY, "damage") &&
                  strstr(CONTROLS_SETUP_BODY, "SX1262"));
        controls_setup_body(&su, body, sizeof(body));
        check("with no failure the body is the question alone", strcmp(body, CONTROLS_SETUP_BODY) == 0);

        st = cJSON_Parse("{\"state\":\"running\",\"needed\":true}");
        controls_setup_parse(st, &su);
        cJSON_Delete(st);
        check("while a setup runs a tap does nothing",
              su.running && controls_radio_tapped_setup(&flow, "rx", &su) == CONTROLS_TAP_NOTHING &&
                  flow.confirm == CONTROLS_CONFIRM_NONE);

        st = cJSON_Parse("{\"state\":\"failed\",\"needed\":true,\"error\":\"radiod did not come back on the SX1262\"}");
        controls_setup_parse(st, &su);
        cJSON_Delete(st);
        controls_setup_body(&su, body, sizeof(body));
        check("after a failure the question says what went wrong last time",
              su.failed && strstr(body, "Last attempt: radiod did not come back on the SX1262.") != NULL);

        st = cJSON_Parse("{\"state\":\"done\",\"needed\":false}");
        controls_setup_parse(st, &su);
        cJSON_Delete(st);
        check("once set up, an off radio gets the antenna question as before",
              su.done && controls_radio_tapped_setup(&flow, "off", &su) == CONTROLS_TAP_ASK_ANTENNA);
        controls_radio_dismiss(&flow);
        check("and an on radio is switched off as before",
              controls_radio_tapped_setup(&flow, "rx", &su) == CONTROLS_TAP_SWITCH_OFF);

        controls_setup_parse(NULL, &su);
        check("sysd not answering: unknown, and the tile works as before",
              !su.known && controls_radio_tapped_setup(&flow, "off", &su) == CONTROLS_TAP_ASK_ANTENNA);
        controls_radio_dismiss(&flow);
        st = cJSON_Parse("{\"needed\":true}");
        controls_setup_parse(st, &su);
        cJSON_Delete(st);
        check("a status without a state is unknown", !su.known);
    }

    /* ---- Bluetooth ---- */
    controls_bluetooth_text(NULL, out, sizeof(out));
    check("Bluetooth: sysd not answering", strcmp(out, "Not answering") == 0);
    st = status("{\"bluetooth\":{\"controllers\":[]}}");
    controls_bluetooth_text(st, out, sizeof(out));
    check("Bluetooth: no controller (unit A) is not available",
          strcmp(out, "Not available") == 0 && !controls_bluetooth_available(st));
    cJSON_Delete(st);
    st = status("{}");
    controls_bluetooth_text(st, out, sizeof(out));
    check("Bluetooth: an older sysd without the field is not available", strcmp(out, "Not available") == 0);
    cJSON_Delete(st);
    st = status("{\"bluetooth\":{\"controllers\":[\"hci0\"]}}");
    controls_bluetooth_text(st, out, sizeof(out));
    check("Bluetooth: a controller is present, not claimed on or off",
          strcmp(out, "Controller present") == 0 && controls_bluetooth_available(st));
    cJSON_Delete(st);

    /* ---- battery ---- */
    controls_battery_text(NULL, out, sizeof(out));
    check("battery: sysd not answering", strcmp(out, "Not answering") == 0);
    check("battery: USB only, no gauge (unit A)",
          battery_is("{\"power\":{\"source\":\"external\",\"battery\":null,\"supplies\":[]}}",
                     "External power"));
    check("battery: an older sysd without battery, source external",
          battery_is("{\"power\":{\"source\":\"external\",\"supplies\":[]}}", "External power"));
    check("battery: no power object at all", battery_is("{}", "Unknown"));
    check("battery: charging",
          battery_is("{\"power\":{\"source\":\"external\",\"battery\":{\"present\":true,"
                     "\"capacity_percent\":76,\"status\":\"charging\"}}}", "76 % \xc2\xb7 Charging"));
    check("battery: discharging",
          battery_is("{\"power\":{\"source\":\"battery\",\"battery\":{\"present\":true,"
                     "\"capacity_percent\":41,\"status\":\"discharging\"}}}", "41 % \xc2\xb7 On battery"));
    check("battery: full",
          battery_is("{\"power\":{\"source\":\"external\",\"battery\":{\"present\":true,"
                     "\"capacity_percent\":100,\"status\":\"full\"}}}", "100 % \xc2\xb7 Full"));
    check("battery: no percentage from the driver",
          battery_is("{\"power\":{\"source\":\"unknown\",\"battery\":{\"present\":true,"
                     "\"capacity_percent\":null,\"status\":\"charging\"}}}", "Charging"));
    check("battery: nothing known but that there is one",
          battery_is("{\"power\":{\"source\":\"unknown\",\"battery\":{\"present\":true,"
                     "\"capacity_percent\":null,\"status\":null}}}", "Battery"));
    check("battery: a percentage outside 0..100 is not shown",
          battery_is("{\"power\":{\"battery\":{\"present\":true,\"capacity_percent\":130,"
                     "\"status\":null}}}", "Battery"));
    check("battery: the gauge says none is fitted",
          battery_is("{\"power\":{\"source\":\"external\",\"battery\":{\"present\":false}}}",
                     "No battery"));
    check("battery dot: charging", battery_dot("{\"power\":{\"source\":\"unknown\",\"battery\":"
                                               "{\"present\":true,\"status\":\"charging\"}}}"));
    check("battery dot: none without a battery",
          !battery_dot("{\"power\":{\"source\":\"external\",\"battery\":null}}"));
    check("battery dot: none on battery",
          !battery_dot("{\"power\":{\"source\":\"battery\",\"battery\":{\"present\":true,"
                       "\"status\":\"discharging\"}}}"));

    /* ---- volume ---- */
    controls_volume_text(true, false, 70, out, sizeof(out));
    check("volume: the level", strcmp(out, "70 %") == 0);
    controls_volume_text(true, true, 70, out, sizeof(out));
    check("volume: muted", strcmp(out, "Muted") == 0);
    controls_volume_text(false, false, 70, out, sizeof(out));
    check("volume: no sound card", strcmp(out, "Not available") == 0);

    /* ---- layout: the panel is 568 x 1232, and the content area is all of
     * it - the status bar is gone (DS §36). ---- */
    check_layout("portrait", false, 568, 1232);
    check_layout("landscape", true, 1232, 568);
    {
        struct controls_layout l;
        struct controls_frame f = frame(false, 568, 900, true);

        check("a portrait area too short to hold it is reported", controls_layout(&f, &l) == -1);
        f = frame(true, 1232, 568, true);
        controls_layout(&f, &l);
        check("landscape puts Lock and Power in the header row, left of the status cluster",
              l.lock.y == l.back.y && l.power.x + l.power.w == f.keepout.x - 16);
        f = frame(true, 1232, 568, false);
        controls_layout(&f, &l);
        check("with no cluster, Power ends at the right margin", l.power.x + l.power.w == 1232 - l.margin);
        check("landscape: the margin is the 50 px top corner, not 36", l.margin == 50 && l.back.x == 50);
        f = frame(false, 568, 1232, true);
        controls_layout(&f, &l);
        check("portrait keeps Lock and Power at the foot", l.lock.y > l.list.y + l.list.h);
        check("portrait: the margin stays 36, clear of the 30 px corners", l.margin == 36);
        check("portrait: the header ends short of the status cluster",
              l.header.x + l.header.w <= f.keepout.x - 16 && l.header.w >= 240);
        f.keepout.x = 100; /* a cluster reaching over the whole header row */
        check("a cluster the header row cannot keep clear of is reported", controls_layout(&f, &l) == -1);
    }

    printf("controls_model_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
