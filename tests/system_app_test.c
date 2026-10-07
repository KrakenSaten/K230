/*
 * System in the running app, under a real LVGL pointer device: what a finger
 * actually reaches, what the app then asks sysd for, and where everything is
 * in portrait and in landscape.
 *
 * system_view_test proves the decisions - what a null reads as, when a poll
 * is stale, that nothing stops the machine without a confirmation. This one
 * proves what it cannot: that the panels those decisions fill are laid out
 * whole in both orientations and both corner shapes, that every control is a
 * real target inside the body and clear of the panel's rounded corners, that
 * long values stay inside their rows, that a finger scrolls to everything,
 * that the confirmations and the panel a power action leaves fit, and that
 * turning the display under the open app moves the panels without making a
 * second set of them or losing what is on show. Since DS §52 the screen is
 * four pages under a row of tabs - OVERVIEW, NETWORK, SERVICES, ABOUT - each
 * of which the page itself scrolls, if anything does, with nothing inside it
 * scrolling on its own; every tab is checked at Small, Medium and Large.
 *
 * sysd is not here, so this file plays it: shell_ipc_call_timeout() answers
 * system.info and system.status with scripted JSON and records every call,
 * and system.reboot and system.poweroff are answered - or refused - without
 * anything being stopped. radiod's radio.stats, netd's wifi.status and
 * meshcored's mesh.status, which the NETWORK page asks for, are scripted too. The shell is not here either: the app is hosted the
 * way ui/shell/shell.c hosts it (a header, then a padded body, on the
 * reference panel with its 30 px rounded corners), and the app.h entry points
 * are counters.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/system_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"
#include "pocketui.h"
#include "shell_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30 /* the corner squares of the unit's panel (DS 21.1) */
/* Where the shell's content area starts for this app in the display's
 * orientation (ui/shell/chrome.h, DS sections 30 and 36), so the frame built
 * here is the one shell.c builds: the top edge, since no chrome reserves a
 * row there any more. */
#define STATUS_H chrome_height(chrome_resolve(app_system.chrome, pocketui_display_geometry()->width > pocketui_display_geometry()->height, false))
/* Rows measured under the retired bars - 56 px in portrait (v0.0.10), 32 in
 * landscape (DS section 30) - moved up with the frame's top, which is the
 * top edge since the bar went (DS section 36): nothing else about those
 * places changed. */
#define V010_ROW(y) ((y) - 56 + STATUS_H)
#define V010_LROW(y) ((y) - 32 + STATUS_H)
#define PAIRED_BUTTON_H 56 /* DS 7: paired buttons inside a panel */
#define PANEL_GAP 22       /* DS 7 */
#define COLUMN_W 528       /* the portrait body on the reference panel */

extern const struct pocketos_app app_system;

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

/* ---- fixtures: what sysd answers -------------------------------------------------- */

/* Unit A as it reports itself on v0.0.10 (docs/hardware/V0.0.7_SYSTEM_STATUS_SMOKE.md
 * for the shape: two mounts, eth0 up, two Wi-Fi interfaces down and sit0 hidden). */
#define INFO_UNITA "{\"api_version\":0,\"version\":\"0.0.10\",\"build\":\"aaad9f4\",\"release_file\":\"0.0.10\"," \
    "\"release_build\":\"aaad9f4\",\"model\":\"Canaan CanMV-K230 with RM69A10 OLED\",\"kernel\":\"6.6.36\"," \
    "\"machine\":\"riscv64\",\"os\":\"Buildroot 2025.02.1\",\"vendor_sdk\":\"v1.2-20260909-22d02c6\",\"cpus\":1}"
#define STATUS_UNITA "{\"uptime_s\":7384,\"load\":[0.42,0.37,0.30],\"cpu_percent\":17," \
    "\"memory\":{\"total_kb\":1015808,\"available_kb\":523100,\"free_kb\":401000},\"temperature_c\":51.3," \
    "\"clock_set\":true,\"storage\":[{\"mount\":\"/\",\"total_bytes\":601882624,\"avail_bytes\":137363456}," \
    "{\"mount\":\"/boot\",\"total_bytes\":67108864,\"avail_bytes\":41943040}]," \
    "\"network\":[{\"name\":\"eth0\",\"operstate\":\"up\",\"carrier\":true,\"mac\":\"02:11:22:33:44:55\",\"ipv4\":\"192.168.10.157\"," \
    "\"rx_bytes\":" ETH0_RX ",\"tx_bytes\":" ETH0_TX "}," \
    "{\"name\":\"sit0\",\"operstate\":\"down\",\"carrier\":null,\"mac\":\"00:00:00:00\",\"ipv4\":null}," \
    "{\"name\":\"wlan0\",\"operstate\":\"down\",\"carrier\":null,\"mac\":\"02:11:22:33:44:56\",\"ipv4\":null}," \
    "{\"name\":\"wlan1\",\"operstate\":\"down\",\"carrier\":null,\"mac\":\"02:11:22:33:44:57\",\"ipv4\":null}]," \
    "\"services\":[" SVC_RUN("doors-shell", 246) "," SVC_RUN("netd", 238) "," SVC_RUN("radiod", 231) "," \
    SVC_RUN("sysd", 229) "]}"
/* eth0's byte counters in STATUS_UNITA (unit B's, 2026-10-02), and the same
 * two seconds later in STATUS_TRAFFIC: 40960 bytes in and 8192 out, 20 and 4
 * KB/s. */
#define ETH0_RX "23681229"
#define ETH0_TX "105768542"
#define STATUS_TRAFFIC_OF(rx, tx) "{\"uptime_s\":7386,\"load\":[0.42,0.37,0.30],\"cpu_percent\":17," \
    "\"memory\":{\"total_kb\":1015808,\"available_kb\":523100,\"free_kb\":401000},\"temperature_c\":51.3," \
    "\"clock_set\":true,\"storage\":[{\"mount\":\"/\",\"total_bytes\":601882624,\"avail_bytes\":137363456}," \
    "{\"mount\":\"/boot\",\"total_bytes\":67108864,\"avail_bytes\":41943040}]," \
    "\"network\":[{\"name\":\"eth0\",\"operstate\":\"up\",\"carrier\":true,\"mac\":\"02:11:22:33:44:55\"," \
    "\"ipv4\":\"192.168.10.157\",\"rx_bytes\":" rx ",\"tx_bytes\":" tx "}," \
    "{\"name\":\"wlan0\",\"operstate\":\"down\",\"carrier\":null,\"mac\":\"02:11:22:33:44:56\",\"ipv4\":null}," \
    "{\"name\":\"wlan1\",\"operstate\":\"down\",\"carrier\":null,\"mac\":\"02:11:22:33:44:57\",\"ipv4\":null}]," \
    "\"services\":[" SVC_RUN("doors-shell", 246) "," SVC_RUN("netd", 238) "," SVC_RUN("radiod", 231) "," \
    SVC_RUN("sysd", 229) "]}"
#define STATUS_TRAFFIC STATUS_TRAFFIC_OF("23722189", "105776734")
#define SVC_RUN(name, pid) \
    "{\"name\":\"" name "\",\"pid\":" #pid ",\"running\":true,\"crashloop\":false,\"last_exit_code\":null,\"restarts\":0}"
#define SVC_LOOP(name) \
    "{\"name\":\"" name "\",\"pid\":null,\"running\":false,\"crashloop\":true,\"last_exit_code\":139,\"restarts\":6}"
#define SVC_STOPPED(name) \
    "{\"name\":\"" name "\",\"pid\":null,\"running\":false,\"crashloop\":false,\"last_exit_code\":1,\"restarts\":0}"
#define IFACE(name, state, mac, ip) \
    "{\"name\":\"" name "\",\"operstate\":\"" state "\",\"carrier\":null,\"mac\":\"" mac "\",\"ipv4\":" ip "}"
#define MOUNT(path, total, avail) "{\"mount\":\"" path "\",\"total_bytes\":" total ",\"avail_bytes\":" avail "}"

/* Every value as long as it gets: a card that disagrees with the running
 * build, a model and a kernel wider than any row, a crash loop, a stopped
 * service with a long name, a volume counted in GB, the clock not set. */
#define LONG_BUILD "0.0.10-rc2+landscape \xC2\xB7 0123456789abcdef-dirty"
#define LONG_CARD "0.0.9 \xC2\xB7 c6cf41b-dirty-from-a-bench-deploy"
#define LONG_MODEL "Canaan CanMV-K230 with RM69A10 OLED on a LILYGO T-Display K230 carrier"
#define LONG_KERNEL "6.6.36-pocketos-g22d02c6-dirty-with-a-very-long-local-version riscv64"
#define LONG_SERVICE "pocketos-long-service-name-watchdog"
#define LOOP_DETAIL "exit 139 \xC2\xB7 6 restarts/60s"
#define INFO_LONG "{\"api_version\":0,\"version\":\"0.0.10-rc2+landscape\",\"build\":\"0123456789abcdef-dirty\"," \
    "\"release_file\":\"0.0.9\",\"release_build\":\"c6cf41b-dirty-from-a-bench-deploy\",\"model\":\"" LONG_MODEL "\"," \
    "\"kernel\":\"6.6.36-pocketos-g22d02c6-dirty-with-a-very-long-local-version\",\"machine\":\"riscv64\"}"
#define STATUS_LONG "{\"uptime_s\":277200,\"load\":[12.75,9.1,7.0],\"cpu_percent\":100," \
    "\"memory\":{\"total_kb\":1015808,\"available_kb\":1015808,\"free_kb\":0},\"temperature_c\":104.25," \
    "\"clock_set\":false,\"storage\":[" MOUNT("/", "601882624", "3145728") "," MOUNT("/boot", "67108864", "41943040") "," \
    MOUNT("/data", "63780264345", "63243393433") "]," \
    "\"network\":[" IFACE("eth0", "up", "02:11:22:33:44:55", "\"192.168.100.255\"") "," \
    IFACE("enx0123456789ab", "up", "02:11:22:33:44:58", "\"10.255.255.254\"") "," \
    IFACE("wlan0", "dormant", "02:11:22:33:44:56", "null") "," IFACE("sit0", "down", "00:00:00:00", "null") "]," \
    "\"services\":[" SVC_RUN("doors-shell", 246) "," SVC_LOOP("netd") "," SVC_RUN("radiod", 2147483) "," \
    SVC_RUN("sysd", 229) "," SVC_STOPPED(LONG_SERVICE) "]}"
/* Every row the view keeps: six mounts, eight interfaces and two more hidden,
 * twelve services. */
#define STATUS_MAX "{\"uptime_s\":60,\"load\":[1.0],\"cpu_percent\":5,\"memory\":null,\"temperature_c\":null," \
    "\"clock_set\":true,\"storage\":[" MOUNT("/", "601882624", "137363456") "," MOUNT("/boot", "67108864", "41943040") "," \
    MOUNT("/data", "63780264345", "12992276889") "," MOUNT("/media/usb", "17179869184", "16106127360") "," \
    MOUNT("/var/lib/pocketos", "943718400", "838860800") "," MOUNT("/tmp", "268435456", "267386880") "]," \
    "\"network\":[" IFACE("if0", "up", "02:00:00:00:00:00", "\"10.0.0.200\"") "," IFACE("if1", "down", "02:00:00:00:00:01", "null") "," \
    IFACE("if2", "up", "02:00:00:00:00:02", "\"10.0.2.202\"") "," IFACE("if3", "down", "02:00:00:00:00:03", "null") "," \
    IFACE("if4", "up", "02:00:00:00:00:04", "\"10.0.4.204\"") "," IFACE("if5", "down", "02:00:00:00:00:05", "null") "," \
    IFACE("if6", "up", "02:00:00:00:00:06", "\"10.0.6.206\"") "," IFACE("if7", "down", "02:00:00:00:00:07", "null") "," \
    IFACE("if8", "up", "02:00:00:00:00:08", "\"10.0.8.208\"") "," IFACE("if9", "down", "02:00:00:00:00:09", "null") "]," \
    "\"services\":[" SVC_RUN("svc-00", 200) "," SVC_RUN("svc-01", 201) "," SVC_RUN("svc-02", 202) "," \
    SVC_RUN("svc-03", 203) "," SVC_RUN("svc-04", 204) "," SVC_RUN("svc-05", 205) "," SVC_RUN("svc-06", 206) "," \
    SVC_RUN("svc-07", 207) "," SVC_RUN("svc-08", 208) "," SVC_RUN("svc-09", 209) "," SVC_LOOP("netd") "," \
    SVC_STOPPED("stopped-one") "]}"
/* The longest refusal a power action is given here: more than a line across
 * the landscape body in Outdoor type. */
#define REFUSAL "poweroff refused: /sbin/poweroff exited 1 (not permitted on this bench unit, see the log)"

#define RESTART_BODY "Services stop and the board reboots. This takes about 35 seconds."
#define POWEROFF_TERMINAL "Powering off...\n\nDisconnect USB power for about 30 seconds, " \
    "then reconnect to switch the board back on."

/* ---- the scripted sysd, and radiod's one call ----------------------------------------- */

#define CALLS_MAX 512
static struct {
    char service[16];
    char method[32];
    int timeout_ms;
} calls[CALLS_MAX];
static int call_count;
static int sysd_down;
static const char *g_info = INFO_UNITA;
static const char *g_status = STATUS_UNITA;
static const char *g_power_error;
static const char *g_radio = "rx";
/* Diagnostics: what radiod, meshcored and sysd's two readers answer, and the
 * level system.logs was last asked for. The log reply is built per call from
 * g_log_entries so a long log can be made without a fixture of that size. */
static const char *g_radio_status = "{\"state\":\"off\",\"enabled\":false,\"profile\":"
                                    "{\"frequency_mhz\":869.618,\"spreading_factor\":8}}";
static const char *g_mesh_status = "{\"state\":\"degraded\",\"reason\":\"the radio is switched off\","
                                   "\"radio\":{\"radio_state\":\"off\"}}";
static const char *g_crashes = "{\"available\":true,\"total\":1,\"reports\":[{\"file\":\"a\",\"process\":\"netd\","
                               "\"pid\":252,\"time\":1790000000,\"signal\":11,\"signal_name\":\"SIGSEGV\","
                               "\"frames\":[\"/usr/sbin/netd(+0x10)[0x1]\"]}]}";
static const char *g_radio_stats = "{\"tx_packets\":1,\"rx_packets\":848,\"rx_crc_errors\":36,"
                                   "\"last_rssi_dbm\":-74,\"last_snr_db\":12.25}";
static const char *g_wifi_status = "{\"available\":true,\"enabled\":true,\"state\":\"connected\","
                                   "\"ssid\":\"Home\",\"signal_bars\":3}";
static int g_log_entries = 3;
static char g_logs_level[16];
/* storage.status: NULL leaves the card unknown, as an older sysd would, so
 * the OVERVIEW the other cases check carries no expansion row. storage.expand
 * accepts unless g_expand_error is set, and turns the card to running. */
static const char *g_storage;
static const char *g_expand_error;
#define STORAGE_AVAILABLE "{\"usb\":{\"state\":\"absent\"},\"internal\":{\"state\":\"available\"," \
    "\"device\":\"/dev/mmcblk1p2\",\"disk_bytes\":15634268160,\"partition_bytes\":629145600," \
    "\"filesystem_bytes\":629145600,\"unused_bytes\":14871953408,\"can_expand\":true,\"reason\":null," \
    "\"error\":null,\"done\":false}}"
#define STORAGE_RUNNING "{\"internal\":{\"state\":\"running\",\"error\":null,\"done\":false}}"
#define STORAGE_RESTART "{\"internal\":{\"state\":\"reboot_required\",\"error\":null,\"done\":false}}"
#define STORAGE_DONE "{\"internal\":{\"state\":\"not_needed\",\"error\":null,\"done\":true}}"

static cJSON *logs_reply(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *entries = cJSON_AddArrayToObject(o, "entries");
    int i;

    cJSON_AddBoolToObject(o, "available", 1);
    cJSON_AddNumberToObject(o, "skipped", 0);
    for (i = 0; i < g_log_entries; i++) {
        cJSON *e = cJSON_CreateObject();
        /* The filter is sysd's: asked for errors it sends only errors. */
        const char *level = strcmp(g_logs_level, "error") == 0 ? "error"
                            : strcmp(g_logs_level, "warn") == 0 ? (i % 2 ? "error" : "warn")
                                                                 : (i % 3 == 0 ? "error" : i % 3 == 1 ? "warn" : "info");
        char msg[160];

        snprintf(msg, sizeof(msg), "entry %d at level %s, long enough to wrap across the narrowest "
                                   "column the page is ever given on this panel", i, level);
        cJSON_AddStringToObject(e, "ts", "2026-09-25T11:20:05.000Z");
        cJSON_AddStringToObject(e, "source", i % 2 ? "supervise-radiod" : "radiod");
        cJSON_AddStringToObject(e, "level", level);
        cJSON_AddStringToObject(e, "message", msg);
        cJSON_AddItemToArray(entries, e);
    }
    cJSON_AddNumberToObject(o, "returned", g_log_entries);
    return o;
}

static void calls_reset(void)
{
    call_count = 0;
}

static int called(const char *method)
{
    int i;
    int n = 0;

    for (i = 0; i < call_count; i++) {
        n += strcmp(calls[i].method, method) == 0;
    }
    return n;
}

cJSON *shell_ipc_call_timeout(const char *service, const char *method, cJSON *params, int timeout_ms, char *err,
                              size_t errlen)
{
    const char *reply = NULL;

    if (err && errlen) {
        err[0] = '\0';
    }
    if (call_count < CALLS_MAX) {
        snprintf(calls[call_count].service, sizeof(calls[0].service), "%s", service);
        snprintf(calls[call_count].method, sizeof(calls[0].method), "%s", method);
        calls[call_count].timeout_ms = timeout_ms;
        call_count++;
    }
    if (strcmp(method, "system.logs") == 0) {
        const cJSON *lv = params ? cJSON_GetObjectItemCaseSensitive(params, "level") : NULL;

        snprintf(g_logs_level, sizeof(g_logs_level), "%s", cJSON_IsString(lv) ? lv->valuestring : "");
    }
    cJSON_Delete(params);
    if (strcmp(service, "radiod") == 0) {
        if (g_radio && strcmp(method, "radio.info") == 0) {
            reply = "{\"region\":\"EU868\",\"backend\":\"sx1262\"}";
        } else if (g_radio && strcmp(method, "radio.status") == 0) {
            reply = g_radio_status;
        } else if (g_radio && strcmp(method, "radio.stats") == 0) {
            reply = g_radio_stats;
        }
    } else if (strcmp(service, "netd") == 0) {
        if (strcmp(method, "wifi.status") == 0) {
            reply = g_wifi_status;
        }
    } else if (strcmp(service, "meshcored") == 0) {
        if (strcmp(method, "mesh.status") == 0) {
            reply = g_mesh_status;
        }
    } else if (strcmp(service, "sysd") == 0 && !sysd_down) {
        if (strcmp(method, "system.logs") == 0) {
            return logs_reply();
        } else if (strcmp(method, "system.crashes") == 0) {
            reply = g_crashes;
        } else if (strcmp(method, "system.info") == 0) {
            reply = g_info;
        } else if (strcmp(method, "system.status") == 0) {
            reply = g_status;
        } else if (strcmp(method, "storage.status") == 0) {
            reply = g_storage;
        } else if (strcmp(method, "storage.expand") == 0) {
            if (g_expand_error) {
                snprintf(err, errlen, "%s", g_expand_error);
                return NULL;
            }
            g_storage = STORAGE_RUNNING;
            reply = "{\"state\":\"running\"}";
        } else if (strcmp(method, "system.reboot") == 0 || strcmp(method, "system.poweroff") == 0) {
            if (g_power_error) {
                snprintf(err, errlen, "%s", g_power_error);
                return NULL;
            }
            reply = strcmp(method, "system.reboot") == 0 ? "{\"action\":\"reboot\"}" : "{\"action\":\"poweroff\"}";
        }
    }
    return reply ? cJSON_Parse(reply) : NULL;
}

cJSON *shell_ipc_call(const char *service, const char *method, cJSON *params, char *err, size_t errlen)
{
    return shell_ipc_call_timeout(service, method, params, 0, err, errlen);
}

void shell_ipc_shutdown(void) { }

/* Every call on the LVGL thread is bounded (unit A, M5). */
static int every_call_bounded(void)
{
    int i;

    for (i = 0; i < call_count; i++) {
        if (calls[i].timeout_ms != SHELL_IPC_UI_TIMEOUT_MS) {
            return 0;
        }
    }
    return 1;
}

/* ---- the shell's side of app.h, as counters ------------------------------------------ */

static int shell_calls;
static int keyboard_requests;

void pocketos_shell_set_status_hint(const char *text)
{
    (void)text;
    shell_calls++;
}
void pocketos_shell_go_home(void) { shell_calls++; }
int pocketos_shell_reduced_motion(void) { return 0; }
int64_t pocketos_shell_system_day(void) { return -1; }
const char *pocketos_shell_radio_state(void) { return g_radio; }
void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user), void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
    keyboard_requests++;
}
void pocketos_shell_keyboard_hide(void) { keyboard_requests++; }
int pocketos_shell_keyboard_visible(void) { return 0; }

/* ---- display and finger ------------------------------------------------------------ */

static uint8_t draw_buf[PANEL_H * 40 * 4]; /* the long side, either way up */
static lv_display_t *disp;
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;
static lv_obj_t *g_content;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = finger_state;
    data->point = finger_point;
}

static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

/* The display the shell would open: the reference panel with corner squares
 * of `corner` px, turned to `rotation`, the geometry handed to PocketUI and
 * the content area below the status bar sized to it. Called with the app
 * open, it is the body changing shape under a running app. */
static void use_panel(int32_t long_side, enum pos_rotation rotation, int32_t corner)
{
    struct pos_panel panel = {
        .width = PANEL_W,
        .height = long_side,
        .corners = { corner, corner, corner, corner },
    };
    struct pos_display_geometry g;

    pos_display_geometry_init(&g, &panel, rotation);
    pocketui_set_display_geometry(&g);
    /* The lifted finger's last point could be off the turned display, which
     * LVGL warns about on every read. */
    finger_point.x = 0;
    finger_point.y = 0;
    lv_display_set_resolution(disp, g.width, g.height);
    lv_obj_set_size(g_content, g.width, g.height - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    pump(60);
}

static void use_display(enum pos_rotation rotation, int32_t corner)
{
    use_panel(PANEL_H, rotation, corner);
}

/* Whether the body the app is in has room for two portrait-wide columns and
 * the gap between them, and is wider than tall: the wide shape's rule. */
static void body_box(lv_area_t *b);
static int wide_display(void)
{
    lv_area_t b;

    body_box(&b);
    return lv_area_get_width(&b) > lv_area_get_height(&b) && lv_area_get_width(&b) >= 2 * COLUMN_W + PANEL_GAP;
}

/* ---- the app, hosted the way the shell hosts it -------------------------------------- */

static lv_obj_t *app_root;
static lv_obj_t *app_body;
static void *app_priv;

static void app_start(void)
{
    lv_obj_t *header;

    app_root = lv_obj_create(g_content);
    lv_obj_remove_style_all(app_root);
    lv_obj_set_size(app_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app_root, LV_FLEX_FLOW_COLUMN);
    header = lv_obj_create(app_root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), POCKETUI_HEADER_H);

    app_body = lv_obj_create(app_root);
    lv_obj_remove_style_all(app_body);
    lv_obj_set_width(app_body, LV_PCT(100));
    lv_obj_set_flex_grow(app_body, 1);
    lv_obj_set_flex_flow(app_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(app_body, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_top(app_body, POCKETUI_BODY_PAD_TOP, 0);
    lv_obj_set_style_pad_row(app_body, POCKETUI_PAD, 0);
    lv_obj_add_flag(app_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(app_body, LV_DIR_VER);
    app_priv = app_system.create(app_body);
    pump(80);
}

/* The shell's app_close(): destroy, then delete the root. */
static void app_stop(void)
{
    app_system.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(20);
}

static void tick(void)
{
    app_system.tick(app_priv);
    pump(60);
}

/* ---- finding things -------------------------------------------------------------------- */

/* NULL-safe, like everything the checks read the tree with, so that a layout
 * that lost an object fails its checks and the run goes on. */
static lv_obj_t *kid(lv_obj_t *parent, uint32_t i)
{
    return parent && lv_obj_get_child_count(parent) > i ? lv_obj_get_child(parent, (int32_t)i) : NULL;
}

static int32_t scroll_y(lv_obj_t *obj)
{
    return obj ? lv_obj_get_scroll_y(obj) : -1;
}

static int has_flag(lv_obj_t *obj, lv_obj_flag_t flag)
{
    return obj && lv_obj_has_flag(obj, flag);
}

/* The app's tree under the body, as system_app.c builds it: one frame, and in
 * it the page on show - the tabs (with the freshness line at their end), the
 * refusal toast and the two columns while live; the freshness line and a
 * confirmation; or only the panel a power action leaves. */
static lv_obj_t *frame_obj(void) { return kid(app_body, 0); }
static lv_obj_t *screen_obj(void) { return kid(frame_obj(), 0); }
static int live(void) { return screen_obj() && lv_obj_get_child_count(screen_obj()) == 3u; }
static lv_obj_t *tabs_obj(void) { return live() ? kid(screen_obj(), 0) : NULL; }
static lv_obj_t *toast_obj(void) { return live() ? kid(screen_obj(), 1) : NULL; }
static lv_obj_t *columns_obj(void) { return live() ? kid(screen_obj(), 2) : NULL; }
static lv_obj_t *column_obj(int i) { return kid(columns_obj(), (uint32_t)i); }
/* A confirmation's freshness row: the first of the page's two children. */
static lv_obj_t *freshness_row(void)
{
    return !live() && screen_obj() && lv_obj_get_child_count(screen_obj()) == 2u ? kid(screen_obj(), 0) : NULL;
}
static lv_obj_t *dialog_obj(void)
{
    return !live() && screen_obj() ? kid(screen_obj(), lv_obj_get_child_count(screen_obj()) - 1u) : NULL;
}

static lv_obj_t *find_visible(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        return t && strcmp(t, text) == 0 ? obj : NULL;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_visible(lv_obj_get_child(obj, i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static int shows(const char *text)
{
    return find_visible(app_body, text) != NULL;
}

/* The nearest ancestor with a click handler: the button a finger on this
 * label presses. */
static lv_obj_t *target_of(const char *text)
{
    lv_obj_t *o = find_visible(app_body, text);

    while (o && lv_obj_get_event_count(o) == 0) {
        o = lv_obj_get_parent(o);
    }
    return o;
}

/* The panel holding the label with this text: the child of a column (live)
 * or of the screen (a confirmation) it is in. */
static lv_obj_t *panel_of(const char *text)
{
    lv_obj_t *o = find_visible(app_body, text);

    while (o && lv_obj_get_parent(o) && lv_obj_get_parent(o) != column_obj(0) &&
           lv_obj_get_parent(o) != column_obj(1) && lv_obj_get_parent(o) != screen_obj()) {
        o = lv_obj_get_parent(o);
    }
    return o && lv_obj_get_parent(o) ? o : NULL;
}

/* The value beside a key in a key/value row: the row's second child. */
static lv_obj_t *value_of(const char *key)
{
    lv_obj_t *o = find_visible(app_body, key);

    while (o && lv_obj_get_parent(o) && lv_obj_get_style_height(lv_obj_get_parent(o), 0) != POCKETUI_ROW_H) {
        o = lv_obj_get_parent(o);
    }
    return o ? kid(lv_obj_get_parent(o), 1) : NULL;
}

/* NULL-safe, so that a layout that lost an object fails its checks and the
 * run goes on, rather than stopping in an LVGL assert that never returns. */
static void area_of(lv_obj_t *obj, lv_area_t *a)
{
    if (!obj) {
        lv_area_set(a, 0, 0, -1, -1);
        return;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, a);
}

static int within(const lv_area_t *in, const lv_area_t *out)
{
    return in->x1 >= out->x1 && in->x2 <= out->x2 && in->y1 >= out->y1 && in->y2 <= out->y2;
}

static int overlaps(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

/* Where the app may put anything: the body's content box. */
static void body_box(lv_area_t *b)
{
    lv_obj_update_layout(app_body);
    lv_obj_get_content_coords(app_body, b);
}

/* How far the foot corner squares reach above the body's foot. */
static int32_t foot_inset(void)
{
    const struct pos_display_geometry *g = pocketui_display_geometry();
    lv_area_t b;

    body_box(&b);
    return LV_MAX(0, b.y2 - (g->height - LV_MAX(g->corners.bottom_left, g->corners.bottom_right)) + 1);
}

/* The nearest ancestor that scrolls: what a finger drags to bring obj into
 * view, and what clips it. */
static lv_obj_t *scroller_of(lv_obj_t *obj)
{
    lv_obj_t *p = obj ? lv_obj_get_parent(obj) : NULL;

    while (p && !lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) {
        p = lv_obj_get_parent(p);
    }
    return p;
}

/* Whether obj is wholly on show: inside its scroller's box as it is scrolled
 * now, inside every box that clips it on the way there, and inside the body. */
static int in_view(lv_obj_t *obj)
{
    lv_area_t a;
    lv_area_t s;
    lv_area_t b;

    if (!obj) {
        return 0;
    }
    area_of(obj, &a);
    area_of(scroller_of(obj), &s);
    body_box(&b);
    return lv_area_get_height(&a) > 0 && within(&a, &s) && within(&a, &b);
}

static void check_rect(const char *what, lv_obj_t *obj, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_area_t a;

    checks++;
    area_of(obj, &a);
    if (!obj || a.x1 != x1 || a.y1 != y1 || a.x2 != x2 || a.y2 != y2) {
        failed++;
        printf("FAIL %s: is %d..%d x %d..%d, want %d..%d x %d..%d\n", what, (int)a.x1, (int)a.x2, (int)a.y1,
               (int)a.y2, (int)x1, (int)x2, (int)y1, (int)y2);
    } else {
        printf("ok   %s\n", what);
    }
}

static void tap_obj(lv_obj_t *obj, const char *what)
{
    lv_area_t a;

    if (!obj) {
        printf("FAIL tap on \"%s\": not on screen\n", what);
        failed++;
        checks++;
        return;
    }
    lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);
    area_of(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}

static void tap(const char *text)
{
    tap_obj(target_of(text), text);
}

/* A finger drawn dy pixels across a point in small moves, the way a scroll
 * reaches LVGL from the panel, then time for the scroll to come to rest. */
static void drag_at(int32_t x, int32_t y, int32_t dy)
{
    int step;

    finger_point.x = x;
    finger_point.y = y;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    for (step = 1; step <= 20; step++) {
        finger_point.y = y + dy * step / 20;
        pump(20);
    }
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(1500);
}

/* Fingers drag the scroller holding obj until obj is in view: from near the
 * edge the content has to come in from (so a gap between panels is as likely
 * under the finger as a panel), as far as it still has to move, at most
 * three quarters of the scroller at a time. */
static int scroll_to(lv_obj_t *obj)
{
    int i;

    for (i = 0; i < 16 && obj && !in_view(obj); i++) {
        lv_area_t s;
        lv_area_t o;
        int32_t most;

        area_of(scroller_of(obj), &s);
        area_of(obj, &o);
        most = lv_area_get_height(&s) * 3 / 4;
        if (o.y2 > s.y2) {
            drag_at(s.x1 + 3 * lv_area_get_width(&s) / 4, s.y2 - 12, -LV_MIN(o.y2 - s.y2 + 40, most));
        } else {
            drag_at(s.x1 + 3 * lv_area_get_width(&s) / 4, s.y1 + 12, LV_MIN(s.y1 - o.y1 + 40, most));
        }
    }
    return in_view(obj);
}

static int count_objects(lv_obj_t *obj)
{
    uint32_t i;
    int n = 1;

    for (i = 0; obj && i < lv_obj_get_child_count(obj); i++) {
        n += count_objects(lv_obj_get_child(obj, i));
    }
    return n;
}

/* How many visible objects under obj reach outside box's columns. */
static int x_outside(lv_obj_t *obj, const lv_area_t *box)
{
    uint32_t i;
    int n = 0;
    lv_area_t a;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    if (obj != app_body) {
        area_of(obj, &a);
        if (a.x1 < box->x1 || a.x2 > box->x2) {
            if (lv_obj_check_type(obj, &lv_label_class)) {
                printf("     outside %d..%d: \"%s\" at %d..%d\n", (int)box->x1, (int)box->x2,
                       lv_label_get_text(obj), (int)a.x1, (int)a.x2);
            }
            n++;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        n += x_outside(lv_obj_get_child(obj, i), box);
    }
    return n;
}

/* Every visible thing a finger can press: clickable, with a handler. */
static int collect_targets(lv_obj_t *obj, lv_obj_t **out, int n, int max)
{
    uint32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return n;
    }
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_event_count(obj) > 0 && n < max) {
        out[n++] = obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        n = collect_targets(lv_obj_get_child(obj, i), out, n, max);
    }
    return n;
}

/* Every visible label inside its parent's box, and no two visible children
 * of a row on top of each other: a long value is cut short inside its row and
 * never runs onto a name, a chip or past the panel. */
static int laid_out_whole(lv_obj_t *obj)
{
    uint32_t i;
    uint32_t j;
    int bad = 0;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_area_t l;
        lv_area_t p;

        area_of(obj, &l);
        lv_obj_get_content_coords(lv_obj_get_parent(obj), &p);
        if (l.x1 < p.x1 || l.x2 > p.x2 || l.y1 < p.y1 || l.y2 > p.y2) {
            printf("     label \"%.40s\" %d..%d x %d..%d outside its box %d..%d x %d..%d\n", lv_label_get_text(obj),
                   (int)l.x1, (int)l.x2, (int)l.y1, (int)l.y2, (int)p.x1, (int)p.x2, (int)p.y1, (int)p.y2);
            bad++;
        }
        return bad;
    }
    if (lv_obj_get_style_layout(obj, 0) == LV_LAYOUT_FLEX &&
        lv_obj_get_style_flex_flow(obj, 0) == LV_FLEX_FLOW_ROW) {
        for (i = 0; i < lv_obj_get_child_count(obj); i++) {
            lv_obj_t *a = lv_obj_get_child(obj, i);
            lv_area_t aa;

            if (lv_obj_has_flag(a, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            area_of(a, &aa);
            for (j = i + 1; j < lv_obj_get_child_count(obj); j++) {
                lv_obj_t *b = lv_obj_get_child(obj, j);
                lv_area_t bb;

                if (lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) {
                    continue;
                }
                area_of(b, &bb);
                if (overlaps(&aa, &bb)) {
                    printf("     side by side but overlapping: %d..%d and %d..%d at y %d\n", (int)aa.x1, (int)aa.x2,
                           (int)bb.x1, (int)bb.x2, (int)aa.y1);
                    bad++;
                }
            }
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        bad += laid_out_whole(lv_obj_get_child(obj, i));
    }
    return bad;
}

/* A label drawn with an ellipsis because its value is wider than its room. */
static int cut_short(lv_obj_t *label)
{
    const char *t = label && lv_obj_check_type(label, &lv_label_class) ? lv_label_get_text(label) : NULL;

    return t && strstr(t, "...") != NULL;
}

/* The screen on show, checked whole: every box that scrolls lies in the body
 * and the safe area, so nothing scrolled is ever drawn into a rounded corner;
 * every target is a finger's size (64 wide, and the DS 7 56 of a paired
 * button tall), on no other target, and can be brought wholly into view
 * inside its scroller; every label is laid out whole; and the body the shell
 * gives never scrolls (the app scrolls inside it). */
static void check_screen(const char *what, int expect_targets)
{
    static lv_obj_t *t[64];
    char msg[200];
    lv_area_t box;
    int n;
    int i;
    int j;
    int small = 0;
    int overlap = 0;
    int unreachable = 0;
    int unsafe = 0;

    body_box(&box);
    n = collect_targets(screen_obj(), t, 0, 64);
    snprintf(msg, sizeof(msg), "%s: %s", what, expect_targets ? "has targets" : "has nothing to press");
    check(msg, expect_targets ? n > 0 : n == 0);
    for (i = 0; i < n; i++) {
        lv_obj_t *s = scroller_of(t[i]);
        lv_area_t a;
        lv_area_t sa;

        area_of(t[i], &a);
        if (lv_area_get_height(&a) < PAIRED_BUTTON_H || lv_area_get_width(&a) < POCKETUI_TOUCH_MIN) {
            printf("     small target %dx%d\n", (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
            small++;
        }
        for (j = i + 1; j < n; j++) {
            lv_area_t b;

            area_of(t[j], &b);
            if (overlaps(&a, &b)) {
                overlap++;
            }
        }
        area_of(s, &sa);
        if (!s || !within(&sa, &box) || !pos_display_rect_is_safe(pocketui_display_geometry(), sa.x1, sa.y1, sa.x2,
                                                                  sa.y2)) {
            printf("     scroller %d..%d x %d..%d not inside the body %d..%d x %d..%d and the safe area\n",
                   (int)sa.x1, (int)sa.x2, (int)sa.y1, (int)sa.y2, (int)box.x1, (int)box.x2, (int)box.y1,
                   (int)box.y2);
            unsafe++;
        }
    }
    for (i = 0; i < n; i++) {
        lv_obj_t *s = scroller_of(t[i]);
        int32_t sy = scroll_y(s);

        lv_obj_scroll_to_view_recursive(t[i], LV_ANIM_OFF);
        if (!in_view(t[i])) {
            unreachable++;
        }
        if (s) {
            lv_obj_scroll_to_y(s, sy, LV_ANIM_OFF);
        }
    }
    pump(20);
    snprintf(msg, sizeof(msg), "%s: every target is at least 64 wide and 56 tall", what);
    check(msg, small == 0);
    snprintf(msg, sizeof(msg), "%s: no target lies on another", what);
    check(msg, overlap == 0);
    snprintf(msg, sizeof(msg), "%s: everything scrolls inside the body and the safe area", what);
    check(msg, unsafe == 0);
    snprintf(msg, sizeof(msg), "%s: every target can be scrolled wholly into view", what);
    check(msg, unreachable == 0);
    snprintf(msg, sizeof(msg), "%s: every label and row laid out whole", what);
    check(msg, laid_out_whole(screen_obj()) == 0);
    snprintf(msg, sizeof(msg), "%s: the body the shell gives does not scroll", what);
    check(msg, lv_obj_get_scroll_top(app_body) <= 0 && lv_obj_get_scroll_bottom(app_body) <= 0);
}

/* Objects inside the page that scroll on their own: anything but the page
 * itself whose content does not fit it (DS §52.2). */
static int nested_scrollers(lv_obj_t *obj, lv_obj_t *page)
{
    uint32_t i;
    int n = 0;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    /* A label whose text is cut with dots reports an overflow, but takes no
     * touch: nothing a finger can drag. */
    if (obj != page && !lv_obj_check_type(obj, &lv_label_class) && lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE) &&
        (lv_obj_get_scroll_top(obj) > 0 || lv_obj_get_scroll_bottom(obj) > 0 || lv_obj_get_scroll_left(obj) > 0 ||
         lv_obj_get_scroll_right(obj) > 0)) {
        lv_area_t a;

        area_of(obj, &a);
        printf("     nested scroller %d..%d x %d..%d\n", (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2);
        n++;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        n += nested_scrollers(lv_obj_get_child(obj, i), page);
    }
    return n;
}

static void check_page(const char *what, int expect_targets)
{
    char msg[200];

    check_screen(what, expect_targets);
    snprintf(msg, sizeof(msg), "%s: nothing inside the page scrolls on its own", what);
    check(msg, nested_scrollers(screen_obj(), screen_obj()) == 0);
}

/* How far the page reaches past its box: 0 when it fits without scrolling. */
static int32_t page_overflow(void)
{
    lv_obj_t *s = screen_obj();

    if (!s) {
        return -1;
    }
    lv_obj_update_layout(s);
    return LV_MAX(0, lv_obj_get_scroll_top(s)) + LV_MAX(0, lv_obj_get_scroll_bottom(s));
}

static const char *const tab_names[4] = { "OVERVIEW", "NETWORK", "SERVICES", "ABOUT" };

static void open_tab(const char *name)
{
    lv_obj_scroll_to_y(screen_obj(), 0, LV_ANIM_OFF);
    tap(name);
    pump(20);
}

/* The tab on show is the accented one: its fill is accent_primary. */
static int tab_on(const char *name)
{
    lv_obj_t *t = target_of(name);

    return t && lv_color_eq(lv_obj_get_style_bg_color(t, LV_PART_MAIN),
                            lv_color_hex(pos_theme_rgb(POS_COLOR_ACCENT_PRIMARY)));
}

/* The live page's arrangement, whichever the body is: the tabs across the
 * top with the freshness line in their row (at its end when wide, under the
 * buttons when tall), the columns under them - side by side when wide and
 * there are two, one above the other when tall - and the page the one box
 * that scrolls. */
static void check_live_shape(const char *name)
{
    char what[200];
    lv_area_t box;
    lv_area_t t;
    lv_area_t f;
    lv_area_t c0;
    lv_area_t c1;
    lv_area_t s;
    int wide = wide_display();
    int two = column_obj(1) && !has_flag(column_obj(1), LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *fresh = tabs_obj() ? kid(tabs_obj(), 4) : NULL;

    /* Where things are is read with the page at its top. */
    if (screen_obj()) {
        lv_obj_scroll_to_y(screen_obj(), 0, LV_ANIM_OFF);
    }
    body_box(&box);
    area_of(tabs_obj(), &t);
    area_of(column_obj(0), &c0);
    area_of(column_obj(1), &c1);
    area_of(screen_obj(), &s);
    snprintf(what, sizeof(what), "[%s] the tabs across the top of the body", name);
    check(what, live() && t.x1 == box.x1 && t.x2 == box.x2 && t.y1 == box.y1);
    if (fresh) {
        lv_area_t last;

        area_of(fresh, &f);
        area_of(kid(tabs_obj(), 3), &last);
        snprintf(what, sizeof(what), "[%s] the freshness line %s, right-aligned to the body", name,
                 wide ? "at the end of the tabs' row" : "under the tabs");
        check(what, f.x2 <= box.x2 && f.x2 >= box.x2 - 1 && (wide ? f.y1 < last.y2 && f.x1 > last.x2 : f.y1 > last.y2));
    }
    if (wide && two) {
        snprintf(what, sizeof(what), "[%s] two columns across the body, 22 px apart, none under 528 px", name);
        check(what, c0.x1 == box.x1 && c1.x2 == box.x2 && c1.x1 - c0.x2 - 1 == PANEL_GAP &&
                        lv_area_get_width(&c0) >= COLUMN_W && lv_area_get_width(&c1) >= COLUMN_W && c1.y1 == c0.y1);
    } else if (two) {
        snprintf(what, sizeof(what), "[%s] one column the body's width, the second under the first", name);
        check(what, c0.x1 == box.x1 && c0.x2 == box.x2 && c1.x1 == box.x1 && c1.x2 == box.x2 &&
                        c1.y1 == c0.y2 + 1 + PANEL_GAP);
    } else {
        snprintf(what, sizeof(what), "[%s] a page of one column, across the body", name);
        check(what, c0.x1 == box.x1 && c0.x2 == box.x2);
    }
    snprintf(what, sizeof(what), "[%s] the page is the one box that scrolls, to the foot less the corners", name);
    check(what, scroller_of(kid(column_obj(0), 0)) == screen_obj() && !has_flag(column_obj(0), LV_OBJ_FLAG_SCROLLABLE) &&
                    !has_flag(columns_obj(), LV_OBJ_FLAG_SCROLLABLE) && s.y1 == box.y1 &&
                    s.y2 == box.y2 - foot_inset() && s.x1 == box.x1 && s.x2 == box.x2);
}

/* A confirmation, or the panel a power action leaves: across the body when
 * tall; 528 px, centred, with the freshness line over it at its width, when
 * wide; and wholly in view without scrolling, in either. */
static void check_dialog_shape(const char *what_base)
{
    char what[200];
    lv_area_t box;
    lv_area_t d;
    lv_area_t fr;

    body_box(&box);
    area_of(dialog_obj(), &d);
    if (wide_display()) {
        snprintf(what, sizeof(what), "%s: 528 px wide, centred in the body", what_base);
        check(what, lv_area_get_width(&d) == COLUMN_W && d.x1 - box.x1 == box.x2 - d.x2 && d.y1 >= box.y1);
    } else {
        snprintf(what, sizeof(what), "%s: across the body", what_base);
        check(what, d.x1 == box.x1 && d.x2 == box.x2);
    }
    if (freshness_row()) {
        area_of(freshness_row(), &fr);
        snprintf(what, sizeof(what), "%s: under the freshness line, at the same width", what_base);
        check(what, fr.x1 == d.x1 && fr.x2 == d.x2 && d.y1 == fr.y2 + 1 + PANEL_GAP);
    }
    snprintf(what, sizeof(what), "%s: in view whole without scrolling", what_base);
    check(what, in_view(dialog_obj()) && scroll_y(screen_obj()) == 0);
}

/* ---- every page, laid out, in one orientation and display mode ----------------------- */

static void check_orientation(const char *name, enum pos_rotation rotation, int32_t corner, const char *mode)
{
    char what[200];
    char why[128];
    int wide;
    lv_area_t a;
    lv_area_t b;
    lv_area_t box;
    lv_obj_t *v;
    size_t k;

    use_display(rotation, corner);
    pos_theme_apply(NULL, mode, why, sizeof(why));
    sysd_down = 0;
    g_power_error = NULL;
    g_info = INFO_LONG;
    g_status = STATUS_LONG;
    app_start();
    tick();
    wide = wide_display();

    /* ---- each tab, with every value as long as it gets */
    for (k = 0; k < 4; k++) {
        open_tab(tab_names[k]);
        tick();
        snprintf(what, sizeof(what), "[%s] %s", name, tab_names[k]);
        check_live_shape(what);
        check_page(what, 1);
    }

    open_tab("ABOUT");
    snprintf(what, sizeof(what), "[%s] the card that disagrees is shown, whole or cut short in its row", name);
    v = value_of("Card");
    area_of(v, &a);
    area_of(v ? lv_obj_get_parent(v) : NULL, &b);
    check(what, v && (cut_short(v) || strcmp(lv_label_get_text(v), LONG_CARD) == 0) && within(&a, &b));
    snprintf(what, sizeof(what), "[%s] the model and the kernel too, each after its key", name);
    {
        lv_obj_t *m = value_of("Model");
        lv_obj_t *kn = value_of("Kernel");
        lv_area_t mk;
        lv_area_t kk;

        area_of(m, &a);
        area_of(find_visible(app_body, "Model"), &mk);
        area_of(kn, &b);
        area_of(find_visible(app_body, "Kernel"), &kk);
        check(what, cut_short(m) && cut_short(kn) && a.x1 > mk.x2 && b.x1 > kk.x2);
    }
    snprintf(what, sizeof(what), "[%s] the Doors row shows the running build", name);
    v = value_of("Doors");
    check(what, v && (strcmp(lv_label_get_text(v), LONG_BUILD) == 0 || cut_short(v)));
    open_tab("SERVICES");
    snprintf(what, sizeof(what), "[%s] a crash loop and a long stopped service are both listed", name);
    check(what, shows("CRASH LOOP") && shows("STOPPED") && shows(LOOP_DETAIL));
    open_tab("OVERVIEW");
    snprintf(what, sizeof(what), "[%s] a volume counted in GB, and the clock not set", name);
    check(what, shows("/data") && shows("58.9 GB free of 59.4 GB") && shows("not set"));
    snprintf(what, sizeof(what), "[%s] a finger reaches Power off", name);
    check(what, scroll_to(target_of("Power off")));
    {
        int32_t s0 = scroll_y(screen_obj());

        calls_reset();
        tick();
        tick();
        snprintf(what, sizeof(what), "[%s] a poll repaints in place: sysd asked, the scroll kept", name);
        check(what, called("system.status") == 1 && live() && scroll_y(screen_obj()) == s0);
    }

    /* ---- the restart confirmation */
    calls_reset();
    tap("Restart");
    snprintf(what, sizeof(what), "[%s] Restart asks first, and calls nothing", name);
    check(what, shows("Restart Doors?") && shows(RESTART_BODY) && shows("Cancel") && call_count == 0);
    snprintf(what, sizeof(what), "[%s] restart confirmation", name);
    check_dialog_shape(what);
    check_page(what, 1);
    tap("Cancel");
    snprintf(what, sizeof(what), "[%s] Cancel goes back, having called nothing", name);
    check(what, live() && !shows("Restart Doors?") && called("system.reboot") == 0 && tab_on("OVERVIEW"));

    /* ---- the power-off confirmation, refused */
    tap("Power off");
    snprintf(what, sizeof(what), "[%s] Power off asks first", name);
    check(what, shows("Power off Doors?") && call_count == 0);
    snprintf(what, sizeof(what), "[%s] power-off confirmation", name);
    check_dialog_shape(what);
    check_page(what, 1);
    g_power_error = REFUSAL;
    tap_obj(kid(lv_obj_get_parent(target_of("Cancel")), 1), "the confirmation's Power off");
    g_power_error = NULL;
    snprintf(what, sizeof(what), "[%s] a refused power-off calls sysd once and says why", name);
    check(what, called("system.poweroff") == 1 && live() && toast_obj() && !has_flag(toast_obj(), LV_OBJ_FLAG_HIDDEN) &&
                    strcmp(lv_label_get_text(toast_obj()), REFUSAL) == 0);
    body_box(&box);
    lv_obj_scroll_to_y(screen_obj(), 0, LV_ANIM_OFF);
    area_of(toast_obj(), &a);
    area_of(columns_obj(), &b);
    snprintf(what, sizeof(what), "[%s] the reason is read whole, across the top, above the panels", name);
    check(what, in_view(toast_obj()) && a.x1 == box.x1 && a.x2 == box.x2 && a.y2 < b.y1);
    snprintf(what, sizeof(what), "[%s] live with a refusal", name);
    check_live_shape(what);
    check_page(what, 1);

    /* ---- restarting */
    calls_reset();
    tap("Restart");
    tap_obj(kid(lv_obj_get_parent(target_of("Cancel")), 1), "the confirmation's Restart");
    snprintf(what, sizeof(what), "[%s] a confirmed restart calls system.reboot once", name);
    check(what, called("system.reboot") == 1 && called("system.poweroff") == 0);
    snprintf(what, sizeof(what), "[%s] and leaves only its panel", name);
    check(what, shows("Restarting...") && !freshness_row() && !shows("Restart") && !shows("OVERVIEW"));
    snprintf(what, sizeof(what), "[%s] restarting", name);
    check_dialog_shape(what);
    check_page(what, 0);
    calls_reset();
    tick();
    tick();
    snprintf(what, sizeof(what), "[%s] and nothing is polled any more", name);
    check(what, call_count == 0);
    app_stop();

    /* ---- powering off, the longest thing the panel ever says */
    app_start();
    tap("Power off");
    tap_obj(kid(lv_obj_get_parent(target_of("Cancel")), 1), "the confirmation's Power off");
    snprintf(what, sizeof(what), "[%s] a confirmed power-off says how to switch the board back on", name);
    check(what, called("system.poweroff") == 1 && shows(POWEROFF_TERMINAL));
    snprintf(what, sizeof(what), "[%s] powering off", name);
    check_dialog_shape(what);
    check_page(what, 0);
    app_stop();

    /* ---- every row there can be */
    g_info = INFO_UNITA;
    g_status = STATUS_MAX;
    app_start();
    snprintf(what, sizeof(what), "[%s] every row", name);
    check_live_shape(what);
    check_page(what, 1);
    snprintf(what, sizeof(what), "[%s] every row: a finger reaches the last mount", name);
    check(what, scroll_to(find_visible(app_body, "/tmp")));
    open_tab("SERVICES");
    snprintf(what, sizeof(what), "[%s] every row: a finger reaches the last service, and Diagnostics", name);
    check(what, scroll_to(find_visible(app_body, "stopped-one")) && scroll_to(target_of("Diagnostics")));
    open_tab("NETWORK");
    snprintf(what, sizeof(what), "[%s] every row: a finger reaches the last interface", name);
    check(what, scroll_to(find_visible(app_body, "2 interfaces hidden")));
    snprintf(what, sizeof(what), "[%s] every row, NETWORK", name);
    check_live_shape(what);
    check_page(what, 1);
    app_stop();

    /* ---- sysd not answering at all */
    sysd_down = 1;
    g_radio = NULL;
    app_start();
    tick();
    snprintf(what, sizeof(what), "[%s] sysd down: nothing reported, every unknown a dash", name);
    check(what, shows("no mounts reported") && shows("\xE2\x80\x94"));
    open_tab("NETWORK");
    check(what, shows("no interfaces") && shows("--") && shows("radiod not answering"));
    open_tab("SERVICES");
    check(what, shows("no services reported"));
    snprintf(what, sizeof(what), "[%s] sysd down", name);
    check_live_shape(what);
    check_page(what, 1);
    app_stop();
    sysd_down = 0;
    g_radio = "rx";
    g_status = STATUS_UNITA;
    pos_theme_apply(NULL, "normal", why, sizeof(why));
    (void)wide;
}

/* Each tab at each text size, both ways up: laid out whole, nothing scrolling
 * inside the page, and - on unit A's answers - short enough not to scroll at
 * all, but for NETWORK and SERVICES at Large. */
static void check_sizes(enum pos_rotation rotation, enum pos_text_size size)
{
    static const char *const sizes[] = { "Small", "Medium", "Large" };
    const char *orient = rotation == POS_ROTATION_270 ? "landscape" : "portrait";
    char what[160];
    size_t k;

    pos_theme_select_text_size(size);
    use_display(rotation, PANEL_CORNER);
    g_info = INFO_UNITA;
    g_status = STATUS_UNITA;
    app_start();
    tick();
    tick();
    for (k = 0; k < 4; k++) {
        int32_t over;

        open_tab(tab_names[k]);
        tick();
        snprintf(what, sizeof(what), "[%s %s] %s", orient, sizes[size], tab_names[k]);
        check(what, tab_on(tab_names[k]));
        check_live_shape(what);
        check_page(what, 1);
        over = page_overflow();
        printf("     %s scrolls %d px\n", what, (int)over);
        /* Every page fits at Small and Medium; at Large OVERVIEW and ABOUT
         * still do, and the two lists may take a scroll of the page. */
        if (size != POS_TEXT_SIZE_LARGE || k == 0 || k == 3) {
            char msg[200];

            snprintf(msg, sizeof(msg), "%s: fits without scrolling", what);
            check(msg, over == 0);
        }
    }
    app_stop();
    pos_theme_select_text_size(POS_TEXT_SIZE_SMALL);
}

int main(void)
{
    lv_indev_t *finger;

    /* Line by line, so a run that stops says where. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    finger = lv_indev_create();
    lv_indev_set_type(finger, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(finger, read_cb);
    pos_input_init();
    pocketui_init();
    pocketui_style_screen(lv_screen_active());
    g_content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_content);
    use_display(POS_ROTATION_0, PANEL_CORNER);

    /* ---- 1. it opens on OVERVIEW, and shows what sysd says ------------------------------ */
    calls_reset();
    app_start();
    check("the app returns its state and adds one box to the body",
          app_priv != NULL && lv_obj_get_child_count(app_body) == 1u);
    check("four tabs, OVERVIEW the one on show", shows("OVERVIEW") && shows("NETWORK") && shows("SERVICES") &&
                                                     shows("ABOUT") && tab_on("OVERVIEW") && !tab_on("ABOUT"));
    check("identity, once", called("system.info") == 1);
    check("vitals", shows("17 %") && shows("51.3 \xC2\xB0" "C") && shows("510/992 MB") && shows("0.42") &&
                        shows("2h 03m") && shows("synced"));
    check("storage", shows("/") && shows("131 MB free of 574 MB") && shows("/boot") && shows("40 MB free of 64 MB"));
    check("Restart and Power off on OVERVIEW", target_of("Restart") && target_of("Power off"));
    check("nothing of the other pages", !shows("eth0") && !shows("doors-shell") && !shows("Kernel"));
    check("radio.info once, for the radio's region", called("radio.info") == 1);
    check("live", shows("LIVE"));
    check("the page needs no scrolling", page_overflow() == 0);
    check("every call bounded by the UI timeout", every_call_bounded());

    calls_reset();
    tick();
    check("the first tick does not poll", called("system.status") == 0);
    tick();
    check("the second does", called("system.status") == 1);
    sysd_down = 1;
    tick();
    tick();
    tick();
    tick();
    tick();
    tick();
    check("sysd stops answering: every value kept, the line says how stale", shows("STALE 6s") && shows("17 %"));
    sysd_down = 0;
    tick();
    tick();
    check("and LIVE again when it answers", shows("LIVE"));

    /* NETWORK: the interfaces and their traffic, Wi-Fi, the radio, the mesh */
    calls_reset();
    open_tab("NETWORK");
    check("NETWORK: the tab on show", tab_on("NETWORK") && !tab_on("OVERVIEW"));
    check("NETWORK: asks radiod, netd and meshcored once each on the way in, bounded",
          called("radio.stats") == 1 && called("wifi.status") == 1 && called("mesh.status") == 1 &&
              every_call_bounded());
    check("NETWORK: interfaces, sit0 hidden and said so", shows("eth0") && shows("192.168.10.157") && shows("wlan1") &&
                                                               !shows("sit0") && shows("1 interface hidden"));
    check("NETWORK: Wi-Fi", shows("Connected to Home, signal 3/4"));
    check("NETWORK: the radio's chip, region and packets", shows("RX") && shows("EU868 \xC2\xB7 sx1262") &&
                                                              shows("848 received \xC2\xB7 1 sent \xC2\xB7 36 CRC errors") &&
                                                              shows("Last packet -74 dBm, SNR 12.2 dB"));
    check("NETWORK: the mesh", shows("Waiting: the radio is switched off"));
    check("NETWORK: eth0's traffic: nothing moved between the answers so far, and its totals",
          shows("\xE2\x86\x93" "0.0 \xE2\x86\x91" "0.0 KB/s \xC2\xB7 22.6 MB in \xC2\xB7 100.9 MB out"));
    check("NETWORK: an interface without counters shows the dash, not a zero", !shows("0 kB in \xC2\xB7 0 kB out"));
    g_status = STATUS_TRAFFIC;
    calls_reset();
    tick();
    tick();
    check("two seconds later: the rate between the two answers", shows("\xE2\x86\x93" "20 \xE2\x86\x91" "4.0 KB/s \xC2\xB7 "
                                                                        "22.6 MB in \xC2\xB7 100.9 MB out"));
    check("NETWORK polls one of its three links a tick, and sysd every other",
          called("system.status") == 1 && called("radio.stats") + called("wifi.status") + called("mesh.status") == 2);
    g_status = STATUS_UNITA;
    calls_reset();
    g_radio = NULL;
    tick();
    tick();
    tick();
    check("radiod not answering: radio.stats is not asked, and that is said",
          called("radio.stats") == 0 && shows("radiod not answering") && shows("--"));
    g_radio = "rx";

    /* SERVICES */
    open_tab("SERVICES");
    check("SERVICES: the services and Diagnostics", shows("doors-shell") && shows("pid 246") && shows("sysd") &&
                                                          shows("pid 229") && target_of("Diagnostics"));
    check("SERVICES: nothing of the other pages", !shows("eth0") && !shows("Restart"));
    g_status = STATUS_LONG;
    tick();
    tick();
    check("a service appearing rebuilds the page with it", shows(LONG_SERVICE) || shows("CRASH LOOP"));
    g_status = STATUS_UNITA;
    tick();
    tick();
    check("and going away again", !shows("CRASH LOOP") && shows("pid 238"));

    /* ABOUT */
    calls_reset();
    open_tab("ABOUT");
    check("ABOUT: the build, the model, the kernel, the platform, the SDK, the CPUs",
          shows("0.0.10 \xC2\xB7 aaad9f4") && shows("6.6.36 riscv64") && shows("Buildroot 2025.02.1") &&
              shows("v1.2-20260909-22d02c6") && shows("1") && value_of("Model") &&
              (strcmp(lv_label_get_text(value_of("Model")), "Canaan CanMV-K230 with RM69A10 OLED") == 0 ||
               cut_short(value_of("Model"))));
    check("ABOUT: the card agrees, so there is no card row", !shows("Card"));
    check("ABOUT: nothing on it is live, so no freshness line", !shows("LIVE"));
    tick();
    tick();
    tick();
    check("ABOUT: and nothing is polled while it is up", call_count == 0);

    /* Back: Diagnostics is the one level inside; the tabs are one level. */
    check("Back on a tab is the shell's: to Settings", app_system.back(app_priv) == 0 && tab_on("ABOUT"));
    check("the header's back slab takes Back first (app.h back_slab_in_app)", app_system.back_slab_in_app);
    open_tab("SERVICES");
    tap("Diagnostics");
    check("Diagnostics opens from SERVICES", shows("DIAGNOSTICS"));
    check("Back closes Diagnostics, one step, back to SERVICES",
          app_system.back(app_priv) == 1 && !shows("DIAGNOSTICS") && tab_on("SERVICES") && shows("doors-shell"));
    app_stop();
    check("nothing asked of the shell but the radio state; no keyboard", shell_calls == 0 && keyboard_requests == 0);

    /* ---- 2. every page in both orientations, both modes, both corner shapes -------------- */
    check_orientation("portrait", POS_ROTATION_0, PANEL_CORNER, "normal");
    check_orientation("landscape", POS_ROTATION_270, PANEL_CORNER, "normal");
    check_orientation("portrait, Outdoor", POS_ROTATION_0, PANEL_CORNER, "outdoor");
    check_orientation("landscape, Outdoor", POS_ROTATION_270, PANEL_CORNER, "outdoor");
    check_orientation("portrait, square corners", POS_ROTATION_0, 0, "normal");
    check_orientation("landscape, square corners", POS_ROTATION_270, 0, "normal");

    /* ---- 3. every tab at every text size, both ways up (DS §46, §52) ----------------------- */
    {
        static const enum pos_text_size sizes[] = { POS_TEXT_SIZE_SMALL, POS_TEXT_SIZE_MEDIUM, POS_TEXT_SIZE_LARGE };
        size_t s;

        for (s = 0; s < 3; s++) {
            check_sizes(POS_ROTATION_0, sizes[s]);
            check_sizes(POS_ROTATION_270, sizes[s]);
        }
    }

    /* ---- 4. where things are, to the pixel -------------------------------------------- */
    /* The tabs at the top of the body, the page under them; in landscape the
     * two columns of 585 px with the 22 px panel gap, and a confirmation of
     * 528 px in the middle of the body. */
    g_info = INFO_UNITA;
    g_status = STATUS_UNITA;
    {
        static const int32_t corners[] = { PANEL_CORNER, 0 };
        size_t k;

        for (k = 0; k < 2; k++) {
            int32_t c = corners[k];
            int32_t lift = c > 20 ? c - 20 : 0;
            char what[160];

            use_display(POS_ROTATION_0, c);
            app_start();
            tick();
            snprintf(what, sizeof(what), "portrait %d px corners: the page scrolls in the body's column", (int)c);
            check_rect(what, screen_obj(), 20, V010_ROW(152), 547, 1211 - lift);
            snprintf(what, sizeof(what), "portrait %d px corners: OVERVIEW, the first tab, at the top left", (int)c);
            {
                lv_area_t t;

                area_of(target_of("OVERVIEW"), &t);
                check(what, t.x1 == 20 && t.y1 == V010_ROW(152) && lv_area_get_height(&t) == PAIRED_BUTTON_H);
            }
            tap("Power off");
            snprintf(what, sizeof(what), "portrait %d px corners: the confirmation across the top", (int)c);
            {
                lv_area_t d;

                area_of(dialog_obj(), &d);
                check(what, d.x1 == 20 && d.x2 == 547 && d.y1 == V010_ROW(195));
            }
            app_stop();

            use_display(POS_ROTATION_270, c);
            app_start();
            tick();
            snprintf(what, sizeof(what), "landscape %d px corners: the left column", (int)c);
            {
                lv_area_t c0;
                lv_area_t c1;

                area_of(column_obj(0), &c0);
                area_of(column_obj(1), &c1);
                check(what, c0.x1 == 20 && c0.x2 == 604 && c1.x1 == 627 && c1.x2 == 1211 && c0.y1 == c1.y1);
            }
            snprintf(what, sizeof(what), "landscape %d px corners: LIVE at the right end of the tabs", (int)c);
            {
                lv_area_t l;

                area_of(find_visible(app_body, "LIVE"), &l);
                check(what, l.x2 == 1211 && l.y1 > V010_LROW(128) && l.y2 < V010_LROW(128) + PAIRED_BUTTON_H);
            }
            snprintf(what, sizeof(what), "landscape %d px corners: Restart and Power off in view on arrival", (int)c);
            check(what, in_view(target_of("Restart")) && in_view(target_of("Power off")) && page_overflow() == 0);
            (void)lift;
            tap("Power off");
            snprintf(what, sizeof(what), "landscape %d px corners: the confirmation in the middle", (int)c);
            {
                lv_area_t d;

                area_of(dialog_obj(), &d);
                check(what, d.x1 == 352 && d.x2 == 879 && d.y1 == V010_LROW(171));
            }
            app_stop();
        }
    }

    /* ---- 5. the room the wide shape needs --------------------------------------------- */
    /* Two columns only when each keeps the portrait body's 528 px: a body one
     * pixel narrower than two of them and the gap keeps one column, scrolled
     * by the page as in portrait. */
    use_panel(PANEL_H, POS_ROTATION_270, PANEL_CORNER);
    app_start();
    tick();
    {
        lv_area_t c0;
        lv_area_t c1;
        lv_area_t box;

        use_panel(2 * COLUMN_W + PANEL_GAP + 2 * POCKETUI_PAD - 1, POS_ROTATION_270, PANEL_CORNER);
        body_box(&box);
        area_of(column_obj(0), &c0);
        area_of(column_obj(1), &c1);
        check("a body 1077 px wide keeps one column, the second under the first",
              lv_area_get_width(&box) == 1077 && c1.y1 > c0.y2 && c1.x1 == c0.x1);
        check_live_shape("1077 px wide");
        check_page("1077 px wide", 1);
        use_panel(2 * COLUMN_W + PANEL_GAP + 2 * POCKETUI_PAD, POS_ROTATION_270, PANEL_CORNER);
        body_box(&box);
        area_of(column_obj(0), &c0);
        area_of(column_obj(1), &c1);
        check("a body 1078 px wide has two, 528 px each",
              lv_area_get_width(&box) == 1078 && c1.x1 > c0.x2 && lv_area_get_width(&c0) == COLUMN_W &&
                  lv_area_get_width(&c1) == COLUMN_W);
        check_live_shape("1078 px wide");
        check_page("1078 px wide", 1);
    }
    app_stop();

    /* A finger that lands in the room beside the panels - under the vitals,
     * in the left column, while the right column runs on below - still drags
     * the page: the columns take no scroll of their own. In landscape, with
     * six mounts, where the right column is taller than the room. */
    {
        lv_area_t upper;
        lv_area_t col;
        lv_point_t gap;
        lv_obj_t *hit;

        use_display(POS_ROTATION_270, PANEL_CORNER);
        g_status = STATUS_MAX;
        app_start();
        tick();
        area_of(panel_of("CPU"), &upper);
        area_of(column_obj(0), &col);
        gap.x = (col.x1 + col.x2) / 2;
        gap.y = upper.y2 + 1 + PANEL_GAP / 2;
        hit = lv_indev_search_obj(lv_screen_active(), &gap);
        check("a finger between two panels lands in the page", hit == screen_obj() || scroller_of(hit) == screen_obj());
        drag_at(gap.x, gap.y, -150);
        check("and drags the page, and nothing else", scroll_y(screen_obj()) > 0 && lv_obj_get_scroll_top(app_body) <= 0);
        app_stop();
        g_status = STATUS_UNITA;
    }

    /* ---- 6. the display turning under the open app ---------------------------------------- */
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    tick();
    {
        static lv_obj_t *t[64];
        int objects = count_objects(app_body);
        int targets = collect_targets(screen_obj(), t, 0, 64);
        lv_obj_t *restart = target_of("Restart");
        lv_obj_t *poweroff = target_of("Power off");
        int i;
        int same = 1;

        for (i = 0; i < 6; i++) {
            lv_area_t c0;
            lv_area_t c1;

            use_display(i % 2 == 0 ? POS_ROTATION_270 : POS_ROTATION_0, PANEL_CORNER);
            area_of(column_obj(0), &c0);
            area_of(column_obj(1), &c1);
            if (count_objects(app_body) != objects || collect_targets(screen_obj(), t, 0, 64) != targets ||
                target_of("Restart") != restart || target_of("Power off") != poweroff || !shows("17 %") ||
                !shows("LIVE") ||
                (i % 2 == 0 ? !(c1.x1 > c0.x2 && c1.y1 == c0.y1) : !(c1.y1 > c0.y2 && c1.x1 == c0.x1))) {
                printf("     turn %d: objects %d/%d targets %d\n", i, count_objects(app_body), objects,
                       collect_targets(screen_obj(), t, 0, 64));
                same = 0;
            }
        }
        check("turning six times keeps every object once, every value, the same buttons, and rearranges each time",
              same);
        use_display(POS_ROTATION_270, PANEL_CORNER);
        check("landscape again after the turns, one of everything", count_objects(app_body) == objects);
        check_live_shape("turned to landscape");
        check_page("turned to landscape", 1);
        calls_reset();
        tap("Restart");
        check("Restart, after the turns, still asks to restart", shows("Restart Doors?") && call_count == 0);
        tap("Cancel");
        tap("Power off");
        check("and Power off still asks to power off", shows("Power off Doors?") && call_count == 0);
    }
    use_display(POS_ROTATION_0, PANEL_CORNER);
    check("a confirmation open while the display turns stays open", shows("Power off Doors?"));
    check_dialog_shape("turned to portrait with the confirmation open");
    use_display(POS_ROTATION_270, PANEL_CORNER);
    check_dialog_shape("and back to landscape");
    tap("Cancel");
    check("Cancel then goes back, having called nothing", live() && call_count == 0);
    check_live_shape("back from the turned confirmation");

    g_power_error = REFUSAL;
    tap("Power off");
    tap_obj(kid(lv_obj_get_parent(target_of("Cancel")), 1), "the confirmation's Power off");
    g_power_error = NULL;
    use_display(POS_ROTATION_0, PANEL_CORNER);
    check("a refusal on show while the display turns is still read whole",
          in_view(toast_obj()) && strcmp(lv_label_get_text(toast_obj()), REFUSAL) == 0);
    check_live_shape("turned to portrait with a refusal");
    use_display(POS_ROTATION_270, PANEL_CORNER);
    check("and turned back", in_view(toast_obj()) && strcmp(lv_label_get_text(toast_obj()), REFUSAL) == 0);
    check_page("turned to landscape with a refusal", 1);
    app_stop();

    /* NETWORK turned: the same page, rearranged, its values kept */
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    open_tab("NETWORK");
    {
        int objects = count_objects(app_body);

        use_display(POS_ROTATION_270, PANEL_CORNER);
        check("NETWORK turned to landscape: interfaces beside the links, one of everything",
              count_objects(app_body) == objects && tab_on("NETWORK") && shows("eth0") &&
                  shows("Connected to Home, signal 3/4"));
        check_live_shape("NETWORK turned");
        check_page("NETWORK turned", 1);
    }
    app_stop();

    /* ---- 7. Diagnostics, in the real LVGL tree ------------------------------------------- */
    /* diag_view_test proves what the page says; this proves the page itself:
     * it opens from SERVICES, asks each service once per refresh with the UI
     * deadline, fills its rows, crash reports and log, filters through sysd,
     * stays inside the body's width in both shapes, holds no more objects
     * after many refreshes of a long log than after one, and Back returns to
     * SERVICES. */
    {
        static const enum pos_rotation rots[] = { POS_ROTATION_0, POS_ROTATION_270 };
        size_t k;

        for (k = 0; k < 2; k++) {
            const char *name = k ? "landscape" : "portrait";
            char what[160];
            lv_area_t box;
            int i;
            int objects;
            int stable = 1;

            use_display(rots[k], PANEL_CORNER);
            g_log_entries = 3;
            app_start();
            tick();
            open_tab("SERVICES");
            calls_reset();
            tap("Diagnostics");
            for (i = 0; i < 6; i++) {
                tick();
            }
            snprintf(what, sizeof(what), "[%s] Diagnostics opens its page", name);
            check(what, shows("DIAGNOSTICS") && shows("CRASH REPORTS") && shows("LOG") && target_of("Back") &&
                            target_of("Refresh") && !target_of("Restart") && !shows("OVERVIEW"));
            snprintf(what, sizeof(what), "[%s] a refresh asks each service once, all bounded", name);
            check(what, called("system.status") == 1 && called("radio.status") == 1 && called("mesh.status") == 1 &&
                            called("system.crashes") == 1 && called("system.logs") == 1 && every_call_bounded());
            snprintf(what, sizeof(what), "[%s] the radio row: switched off", name);
            check(what, shows("Off (switched off)"));
            snprintf(what, sizeof(what), "[%s] the mesh row: waiting for the radio", name);
            check(what, shows("Waiting: the radio is switched off"));
            snprintf(what, sizeof(what), "[%s] the version row: version and build", name);
            check(what, shows("0.0.10 \xC2\xB7 aaad9f4"));
            snprintf(what, sizeof(what), "[%s] the refresh has finished", name);
            check(what, shows("Up to date"));
            snprintf(what, sizeof(what), "[%s] the crash report and the log are listed", name);
            check(what, shows("netd \xC2\xB7 SIGSEGV \xC2\xB7 09-21 14:13 UTC") &&
                            shows("09-25 11:20:05  radiod  ERROR") &&
                            shows("09-25 11:20:05  supervise-radiod  WARN") && strcmp(g_logs_level, "all") == 0);
            snprintf(what, sizeof(what), "[%s] Diagnostics", name);
            check_page(what, 1);

            calls_reset();
            tap("Errors");
            tick();
            snprintf(what, sizeof(what), "[%s] Errors re-asks sysd for errors only, and nothing else", name);
            check(what, call_count == 1 && called("system.logs") == 1 && strcmp(g_logs_level, "error") == 0 &&
                            !shows("09-25 11:20:05  supervise-radiod  WARN"));
            tap("Warnings");
            tick();
            snprintf(what, sizeof(what), "[%s] Warnings asks for warnings and errors", name);
            check(what, strcmp(g_logs_level, "warn") == 0 && shows("09-25 11:20:05  radiod  WARN"));
            tap("All");
            tick();

            g_mesh_status = "{\"state\":\"error\",\"reason\":\"radiod refused the profile: 869.618 MHz "
                            "is outside the configured sub-band\"}";
            tap("Refresh");
            for (i = 0; i < 6; i++) {
                tick();
            }
            snprintf(what, sizeof(what), "[%s] a long mesh reason is shown whole", name);
            check(what, shows("error: radiod refused the profile: 869.618 MHz is outside the configured sub-band"));
            body_box(&box);
            snprintf(what, sizeof(what), "[%s] and stays inside the body's width", name);
            check(what, x_outside(app_body, &box) == 0);
            g_mesh_status = "{\"state\":\"degraded\",\"reason\":\"the radio is switched off\","
                            "\"radio\":{\"radio_state\":\"off\"}}";

            g_log_entries = 60;
            tap("Refresh");
            for (i = 0; i < 6; i++) {
                tick();
            }
            objects = count_objects(app_body);
            for (i = 0; i < 10; i++) {
                int j;

                tap("Refresh");
                for (j = 0; j < 6; j++) {
                    tick();
                }
                if (count_objects(app_body) != objects) {
                    printf("     refresh %d: %d objects, %d after the first\n", i, count_objects(app_body), objects);
                    stable = 0;
                }
            }
            snprintf(what, sizeof(what), "[%s] ten refreshes of a 60-line log: the same objects each time", name);
            check(what, stable);
            snprintf(what, sizeof(what), "[%s] the page lists at most 40 log entries", name);
            check(what, shows("entry 39 at level error, long enough to wrap across the narrowest column the page "
                              "is ever given on this panel") &&
                            !shows("entry 40 at level warn, long enough to wrap across the narrowest column the page "
                                   "is ever given on this panel"));
            body_box(&box);
            snprintf(what, sizeof(what), "[%s] everything on the page stays inside the body's width", name);
            check(what, x_outside(app_body, &box) == 0);

            tap("Back");
            tick();
            snprintf(what, sizeof(what), "[%s] Back returns to SERVICES", name);
            check(what, live() && tab_on("SERVICES") && target_of("Diagnostics") && !shows("CRASH REPORTS"));
            app_stop();
        }
        g_log_entries = 3;
    }

    /* ---- 8. closed and opened again, both ways up -------------------------------------- */
    {
        int i;

        for (i = 0; i < 4; i++) {
            use_display(i % 2 ? POS_ROTATION_270 : POS_ROTATION_0, PANEL_CORNER);
            calls_reset();
            app_start();
            check(i % 2 ? "reopened in landscape: on OVERVIEW, asking for its identity again"
                        : "reopened in portrait: on OVERVIEW, asking for its identity again",
                  called("system.info") == 1 && tab_on("OVERVIEW") && live() && shows("17 %"));
            app_stop();
        }
    }
    /* ---- 9. the microSD card's expansion, on OVERVIEW ----------------------------------- */
    {
        int r;

        for (r = 0; r < 2; r++) {
            const char *name = r ? "landscape" : "portrait";
            char what[160];

            use_display(r ? POS_ROTATION_90 : POS_ROTATION_0, PANEL_CORNER);
            g_storage = STORAGE_AVAILABLE;
            g_expand_error = NULL;
            calls_reset();
            app_start();
            snprintf(what, sizeof(what), "[%s] the card's unused space and Expand storage under STORAGE", name);
            check(what, shows("13.9 GB of the card is not used yet") && target_of("Expand storage") &&
                            called("storage.status") == 1);
            snprintf(what, sizeof(what), "[%s] OVERVIEW with the expansion", name);
            check_page(what, 1);

            calls_reset();
            tap("Expand storage");
            snprintf(what, sizeof(what), "[%s] Expand storage asks first and calls nothing", name);
            check(what, shows("Expand storage?") && shows("Cancel") && call_count == 0);
            snprintf(what, sizeof(what), "[%s] the expansion's confirmation", name);
            check_dialog_shape(what);
            tap("Cancel");
            snprintf(what, sizeof(what), "[%s] Cancel goes back, having called nothing", name);
            check(what, live() && call_count == 0 && target_of("Expand storage"));

            g_expand_error = "the storage expansion is already running";
            tap("Expand storage");
            tap("Expand");
            snprintf(what, sizeof(what), "[%s] a refused expansion says why", name);
            check(what, live() && called("storage.expand") == 1 && shows("the storage expansion is already running"));
            g_expand_error = NULL;

            calls_reset();
            tap("Expand storage");
            tap("Expand");
            snprintf(what, sizeof(what), "[%s] Expand calls storage.expand once and comes back running", name);
            check(what, called("storage.expand") == 1 && live() && !target_of("Expand storage") &&
                            shows("Expanding storage... keep the device powered"));
            g_storage = STORAGE_RESTART;
            tick();
            tick();
            snprintf(what, sizeof(what), "[%s] a restart is asked for when the kernel needs one", name);
            check(what, shows("Restart to finish expanding storage") && target_of("Restart"));
            g_storage = STORAGE_DONE;
            tick();
            tick();
            snprintf(what, sizeof(what), "[%s] and when it is done, it says so", name);
            check(what, shows("Storage expanded to use the whole card") && !target_of("Expand storage"));
            check("every call stayed within the UI deadline", every_call_bounded());
            app_stop();
        }
        g_storage = NULL;
    }

    use_display(POS_ROTATION_0, PANEL_CORNER);
    check("every round leaves nothing behind", lv_obj_get_child_count(g_content) == 0u);
    check("and nothing was asked of the shell, no keyboard at any point", shell_calls == 0 && keyboard_requests == 0);

    printf("system_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
