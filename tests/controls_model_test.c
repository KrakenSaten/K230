/*
 * DOORS Controls model test (ui/shell/controls_model.c): the tile texts from
 * radiod's state and sysd's answers, the antenna question before every
 * off-to-on, and the layout in both orientations - nothing overlapping,
 * everything inside the content area.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
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

static void check_layout(const char *what, bool landscape, int32_t w, int32_t h)
{
    struct controls_layout l;
    char name[128];
    int rc = controls_layout(landscape, w, h, &l);
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

    /* ---- layout: the panel is 568 x 1232, the content area is what the
     * chrome leaves (56 px bar in both orientations on the launcher, 32 px
     * compact in landscape under an app). ---- */
    check_layout("portrait, full bar", false, 568, 1232 - 56);
    check_layout("landscape, full bar", true, 1232, 568 - 56);
    check_layout("landscape, compact bar", true, 1232, 568 - 32);
    {
        struct controls_layout l;

        check("a portrait area too short to hold it is reported",
              controls_layout(false, 568, 900, &l) == -1);
        controls_layout(true, 1232, 512, &l);
        check("landscape puts Lock and Power in the header row",
              l.lock.y == l.back.y && l.power.x + l.power.w == 1232 - l.margin);
        controls_layout(false, 568, 1176, &l);
        check("portrait keeps Lock and Power at the foot", l.lock.y > l.list.y + l.list.h);
    }

    printf("controls_model_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
