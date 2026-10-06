/*
 * RIFT in the running app: what is actually on the glass.
 *
 * rift_model_test and rift_format_test prove the decisions; rift_ipc_test
 * proves the connection. This proves the part neither can: that the chrome
 * and the two sections are laid out whole in both orientations, that the
 * 36 px row selects and never acts while everything that navigates stays at
 * 56, that the landscape split really is a list beside the detail of the
 * same node, that every value on screen is the model's and not a
 * fabrication, and that opening and leaving the app repeatedly leaves
 * nothing behind.
 *
 * meshcored is not here. The app finds no service - which is one of the
 * states it has to draw - and the fixtures go into its model directly,
 * through the same entry points its own client uses. The shell is not here
 * either: the app is hosted the way ui/shell/shell.c hosts it, and the
 * app.h entry points are counters.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * and run by tests/rift_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "pocketui.h"
#include "pos_input.h"
#include "rift_activity.h"
#include "rift_app.h"
#include "rift_comms.h"
#include "rift_contacts_view.h"
#include "rift_device.h"
#include "rift_emoji_pick.h"
#include "rift_emoji_picker.h"
#include "rift_find.h"
#include "rift_graph.h"
#include "rift_manage.h"
#include "rift_msgact.h"
#include "rift_net.h"
#include "rift_map.h"
#include "rift_mapview.h"
#include "rift_netview.h"
#include "rift_nodes.h"
#include "rift_repeater_view.h"
#include "rift_scan.h"
#include "rift_rxlog_view.h"
#include "rift_session.h"
#include "rift_sound.h"
#include "rift_store.h"
#include "rift_strip.h"
#include "rift_test_clock.h"
#include "rift_thread.h"

#include <math.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
#include "src/libs/lodepng/lodepng.h"
#endif

#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30
/* The status bar the shell gives this app in the display's orientation
 * (ui/shell/chrome.h, DS section 30), so the frame built here is the one
 * shell.c builds: none in either orientation, since the app is
 * fullscreen (DS section 30.4, stage 2). */
#define STATUS_H chrome_height(chrome_resolve(app_rift.chrome, pocketui_display_geometry()->width > pocketui_display_geometry()->height, false))

extern const struct pocketos_app app_rift;

static int failed;
static int checks;
static lv_obj_t *app_header; /* the shell's header row as the test builds it */
static int32_t header_h_for(int landscape);

static lv_obj_t *strip(void);
static int count_exact(lv_obj_t *obj, const char *text);
static lv_obj_t *kid(lv_obj_t *parent, uint32_t i);
/* The strip's i-th section tab: the back slab (landscape's way home) is the
 * strip's first child, and the tabs follow it. */
#define tab(i) kid(strip(), (uint32_t)(i) + 1)

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* ---- the shell's entry points, as counters ------------------------------- */

static int hint_calls;
static int home_calls;

void pocketos_shell_set_status_hint(const char *text)
{
    (void)text;
    hint_calls++;
}
void pocketos_shell_go_home(void)
{
    home_calls++;
}
/* What RIFT last told the shell runs behind other screens (DS §51): the
 * status cluster's mark, as the shell would hold it. */
static char bg_label[32];
static char bg_help[96];
static int bg_calls;
void pocketos_shell_set_background(const char *app_id, const char *label, const char *help)
{
    bg_calls++;
    if (!app_id || strcmp(app_id, "rift") != 0) {
        return;
    }
    snprintf(bg_label, sizeof(bg_label), "%s", label ? label : "");
    snprintf(bg_help, sizeof(bg_help), "%s", help ? help : "");
}
int pocketos_shell_reduced_motion(void)
{
    return 0;
}
int64_t pocketos_shell_system_day(void)
{
    return -1;
}
const char *pocketos_shell_radio_state(void)
{
    return NULL;
}
int pocketos_shell_brightness_get(void)
{
    return -1;
}
int pocketos_shell_brightness_set(int percent)
{
    (void)percent;
    return -1;
}
int pocketos_shell_set_appearance(const char *theme_id, const char *mode_name)
{
    (void)theme_id;
    (void)mode_name;
    return 0;
}
void pocketos_shell_orientation(struct pocketos_orientation *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}
int pocketos_shell_set_rotation_mode(enum pocketos_rotation_mode mode)
{
    (void)mode;
    return 0;
}
static int kb_shows;
void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user),
                                 void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
    kb_shows++;
}
void pocketos_shell_keyboard_hide(void) {}
int pocketos_shell_keyboard_visible(void)
{
    return 0;
}

/* The system volume, as the shell would report it: settable, so the DM
 * sound can be tried muted and not. */
static int test_volume = 80;
static int test_muted;
int pocketos_shell_volume_effective(void)
{
    return test_muted ? 0 : test_volume;
}
int pocketos_shell_volume_muted(void)
{
    return test_muted;
}

/* The DM sound, heard by a counter: what RIFT asked the platform to play,
 * at what volume, and how often it asked it to stop. */
static int fake_plays;
static int fake_volume;
static int fake_kind = -1;
static int fake_stops;
static int fake_available(void)
{
    return 1;
}
static int fake_play(enum rift_sound_kind kind, int volume_percent)
{
    fake_plays++;
    fake_kind = kind;
    fake_volume = volume_percent;
    return 0;
}

static void fake_stop(void)
{
    fake_stops++;
}
static const struct rift_sound_backend fake_sound = {
    .name = "test",
    .available = fake_available,
    .play = fake_play,
    .stop = fake_stop,
    .why = "a counter",
};

/* pocketlog's one entry point, so the app can log without the shell. */
void pocketlog_write(int level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

/* ---- display and finger --------------------------------------------------- */

static uint8_t draw_buf[PANEL_H * 40 * 4];
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

/* Time here is the test's, not the machine's. LVGL's tick and RIFT's clock
 * (rift_test_clock.c, linked in place of the real one) move together, by
 * exactly what is pumped, so the ages a screen draws - "45 s" - depend on the
 * fixtures and the pumping and never on how fast this run happens to be. With
 * the real clock, a second boundary crossed between stamping a fixture and
 * photographing it turned one label and failed the same-pixels check of
 * tests/rift_shell_test.sh now and then (more often under ASan or load). */
static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        rift_test_clock_advance(5);
        lv_timer_handler();
    }
}

static void use_display(enum pos_rotation rotation, int32_t corner)
{
    struct pos_panel panel = {
        .width = PANEL_W,
        .height = PANEL_H,
        .corners = { corner, corner, corner, corner },
    };
    struct pos_display_geometry g;

    pos_display_geometry_init(&g, &panel, rotation);
    pocketui_set_display_geometry(&g);
    finger_point.x = 0;
    finger_point.y = 0;
    lv_display_set_resolution(disp, g.width, g.height);
    lv_obj_set_size(g_content, g.width, g.height - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    /* Turned with the app open: the shell would rebuild the app in the
     * new shape's chrome; here the header row it would or would not build
     * is resized, which is the same body for the app. */
    if (app_header) {
        lv_obj_set_height(app_header, header_h_for(g.width > g.height));
    }
    pump(80);
}

/* ---- the app, hosted the way the shell hosts it --------------------------- */

static lv_obj_t *app_root;
static lv_obj_t *app_body;
static struct rift_app *app;

/* The shell's app header as the shell gives it (shell.c, app.h `header`):
 * 72 px in portrait, and nothing in landscape for an app that draws its own
 * top row there (DS §37.2), which RIFT does. */
static int32_t header_h_for(int landscape)
{
    return (landscape && app_rift.header == POCKETOS_HEADER_NONE_LANDSCAPE) ? 0
                                                                           : POCKETUI_HEADER_H;
}

static void app_start(void)
{
    lv_obj_t *header;

    app_root = lv_obj_create(g_content);
    lv_obj_remove_style_all(app_root);
    lv_obj_set_size(app_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app_root, LV_FLEX_FLOW_COLUMN);
    header = lv_obj_create(app_root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100),
                    header_h_for(pocketui_display_geometry()->width >
                                 pocketui_display_geometry()->height));
    app_header = header;

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
    app = app_rift.create(app_body);
    pump(120);
}

/* Park the app's own meshcored client.
 *
 * There is no service here, so the client would go on saying so - correctly -
 * over every fixture this test then puts in the model, and the screens would
 * be photographed with a reconnect notice across them. Its own behaviour is
 * tests/rift_ipc_test.c's subject, against a real service; what is left here
 * is the screens, and they are worth having deterministic. */
static void quiet_client(void)
{
    rift_ipc_close(&app->ipc);
    app->ipc.next_attempt_ms = rift_mono_ms() + 3600000;
}

/* Leave the app as the shell does on Back or Home: destroy, and the body
 * deleted. RIFT's session stays (DS §51). */
static void app_leave(void)
{
    app_rift.destroy(app);
    app = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    app_header = NULL;
    pump(60);
}

/* Leave, and end the session as the shell does when it stops: every session
 * below that calls app_start() again starts from nothing, as it always did. */
static void app_stop(void)
{
    app_rift.destroy(app);
    app_rift.shutdown();
    app = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    app_header = NULL;
    pump(60);
}

/* ---- fixtures ------------------------------------------------------------- */

#define KEY_A "a19ac21e7d0411223344556677889900aabbccddeeff001122334455667788b1"
#define KEY_B "b2cafe1e7d0411223344556677889900aabbccddeeff001122334455667788b2"
#define KEY_C "c3beef1e7d0411223344556677889900aabbccddeeff001122334455667788b3"
#define KEY_D "d4dead1e7d0411223344556677889900aabbccddeeff001122334455667788b4"
/* The node of unknown type the fixture never heard: a peer RIFT may write to. */
#define KEY_E "e500000000000000000000000000000000000000000000000000000000000005"

/* Five nodes, chosen to be every case the row has to draw: heard direct with
 * a full measurement, relayed with a long path and no SNR, a path nobody has
 * observed, a node with a name longer than the column and not in ASCII, and
 * one never heard at all. Ages are relative to the app's own clock. */
static void give_nodes(void)
{
    char json[2048];
    cJSON *o;
    int64_t now = rift_mono_ms();

    snprintf(json, sizeof(json),
             "{\"count\":5,\"nodes\":["
             "{\"public_key\":\"" KEY_A "\",\"node_hash\":\"a1\",\"name\":\"OSLO-01\","
             "\"type\":1,\"path_known\":true,\"hops\":0,\"direct\":true,"
             "\"last_heard_mono_ms\":%lld,\"last_rssi_dbm\":-71.0,\"last_snr_db\":9.5,"
             "\"last_advert_timestamp\":1750000000},"
             "{\"public_key\":\"" KEY_B "\",\"node_hash\":\"b2\",\"name\":\"HYTTA\","
             "\"type\":1,\"path_known\":true,\"hops\":8,\"direct\":false,"
             "\"path_hex\":\"a1c2d34a5b6c7d8e\",\"last_heard_mono_ms\":%lld,"
             "\"last_rssi_dbm\":-88.0},"
             "{\"public_key\":\"" KEY_C "\",\"node_hash\":\"c3\",\"name\":\"NO-3241 FO\","
             "\"type\":1,\"path_known\":false,\"last_heard_mono_ms\":%lld},"
             "{\"public_key\":\"" KEY_D "\",\"node_hash\":\"d4\","
             "\"name\":\"S\xC3\xB8rlandet fjellstasjon \xC3\xA6\xC3\xB8\xC3\xA5 relay\","
             "\"type\":2,\"path_known\":true,\"hops\":3,\"direct\":false,"
             "\"path_hex\":\"a1c2c3\",\"last_heard_mono_ms\":%lld,\"last_snr_db\":-7.5},"
             "{\"public_key\":\"e5000000000000000000000000000000000000000000000000000000"
             "00000005\",\"node_hash\":\"e5\",\"name\":\"never-heard\",\"path_known\":false}]}",
             (long long)(now - 90000), (long long)(now - 700000),
             (long long)(now - 4LL * 3600000), (long long)(now - 30LL * 3600000));
    o = cJSON_Parse(json);
    check("the node fixture is valid JSON", o != NULL);
    check("and the model takes it", rift_model_apply_nodes(&app->model, o) == 0);
    cJSON_Delete(o);
}

static void give_service(void)
{
    cJSON *o = cJSON_Parse("{\"state\":\"online\",\"reason\":\"receiving\","
                           "\"state_since_mono_ms\":100,\"radio\":{\"connected\":true,"
                           "\"lease_held\":true,\"online\":true,\"radio_state\":\"rx\"},"
                           "\"nodes\":5,\"counters\":{\"rx_events\":12,\"rx_delivered\":11,"
                           "\"nodes_unretained\":0,\"contacts_full\":0,\"tx_ok\":3,"
                           "\"tx_failed\":1,\"tx_unknown\":0,\"tx_rx_resume_failed\":0}}");

    rift_model_apply_status(&app->model, o, rift_mono_ms());
    cJSON_Delete(o);
    o = cJSON_Parse("{\"public_key\":\"5f0000000000000000000000000000000000000000000000000000"
                    "00000000ff\",\"node_hash\":\"5f\",\"name\":\"K230-A\"}");
    rift_model_apply_identity(&app->model, o);
    cJSON_Delete(o);
    o = cJSON_Parse("{\"service\":\"meshcored\",\"api_version\":0,\"version\":\"0.0.10\","
                    "\"build\":\"host\",\"protocol\":\"meshcore\"}");
    rift_model_apply_info(&app->model, o);
    cJSON_Delete(o);
    /* One frame with everything the service can report, and one with no
     * time on it at all: the feed has to draw both, and the second must
     * read as "?" rather than as this instant. */
    {
        char json[256];

        snprintf(json, sizeof(json),
                 "{\"kind\":\"rx\",\"payload_type\":\"advert\",\"bytes\":48,"
                 "\"mono_ms\":%lld,\"rssi_dbm\":-88.0,\"snr_db\":6.0}",
                 (long long)(rift_mono_ms() - 45000));
        o = cJSON_Parse(json);
        rift_model_apply_event(&app->model, "mesh.activity", o);
        cJSON_Delete(o);
    }
    o = cJSON_Parse("{\"kind\":\"tx\",\"result\":\"rx_resume_failed\",\"bytes\":72}");
    rift_model_apply_event(&app->model, "mesh.activity", o);
    cJSON_Delete(o);
    rift_app_refresh(app);
    pump(60);
}

/* A conversation with two peers: one where the last word was theirs and one
 * where it was ours, an acknowledged message, one that timed out, and a body
 * with a newline in it - which is one of the two control characters the API
 * lets through, and must not break a one-line row. */
static void give_messages(void)
{
    char json[2048];
    cJSON *o;
    int64_t now = rift_mono_ms();

    snprintf(json, sizeof(json),
             "{\"count\":5,\"total\":5,\"persistent\":false,\"messages\":["
             "{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
             "\"peer_name\":\"HYTTA\",\"text\":\"Str\xC3\xB8m tilbake p\xC3\xA5 hytta\","
             "\"state\":\"received\",\"mono_ms\":%lld,\"rssi_dbm\":-88.0,\"snr_db\":6.5},"
             "{\"id\":2,\"direction\":\"out\",\"peer_public_key\":\"" KEY_B "\","
             "\"text\":\"Fint, ser deg\",\"state\":\"acked\",\"mono_ms\":%lld,"
             "\"ack_mono_ms\":%lld},"
             "{\"id\":3,\"direction\":\"out\",\"peer_public_key\":\"" KEY_B "\","
             "\"text\":\"Pr\xC3\xB8ver direct\",\"state\":\"no_ack\",\"mono_ms\":%lld},"
             "{\"id\":4,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
             "\"peer_name\":\"HYTTA\",\"text\":\"to linjer\\nher\",\"state\":\"received\","
             "\"mono_ms\":%lld},"
             "{\"id\":5,\"direction\":\"out\",\"peer_public_key\":\"" KEY_A "\","
             "\"text\":\"On my way\",\"state\":\"sent_direct\",\"mono_ms\":%lld}]}",
             (long long)(now - 300000), (long long)(now - 240000),
             (long long)(now - 199000), (long long)(now - 120000),
             (long long)(now - 60000), (long long)(now - 30000));
    o = cJSON_Parse(json);
    check("the message fixture is valid JSON", o != NULL);
    check("and the model takes it", rift_model_apply_messages(&app->model, o) == 0);
    cJSON_Delete(o);
    rift_app_refresh(app);
    pump(60);
}

/* The channels the service holds, and one message on one of them.
 *
 * The fixture is the service's answer, verbatim: every channel on screen has
 * to be one mesh.channels reported, because this app has no key and cannot
 * derive a channel from anything. */
/* The conversation keys of the two channels give_channels joins, built the
 * way the model builds them (slot, hash, local name). */
static const char *site_key(void)
{
    static char key[RIFT_KEY_HEX];

    rift_channel_key(0, "8c", "SITE", key, sizeof(key));
    return key;
}

static const char *ops_key(void)
{
    static char key[RIFT_KEY_HEX];

    rift_channel_key(2, "4d", "OPS", key, sizeof(key));
    return key;
}

static void give_channels(void)
{
    cJSON *o;

    o = cJSON_Parse("{\"count\":2,\"max\":8,\"persistent\":true,\"channels\":["
                    "{\"channel\":0,\"name\":\"SITE\",\"channel_hash\":\"8c\","
                    "\"key_bits\":256,\"text_limit\":147,\"ack_expected\":false},"
                    "{\"channel\":2,\"name\":\"OPS\",\"channel_hash\":\"4d\","
                    "\"key_bits\":128,\"text_limit\":147,\"ack_expected\":false}]}");
    check("the channel fixture is valid JSON", o != NULL);
    check("and the model takes it", rift_model_apply_channels(&app->model, o) == 0);
    cJSON_Delete(o);
    rift_app_refresh(app);
    pump(60);
}

static void give_channel_message(void)
{
    char json[512];
    cJSON *o;
    int64_t now = rift_mono_ms();

    snprintf(json, sizeof(json),
             "{\"message\":{\"id\":30,\"direction\":\"in\",\"kind\":\"channel\","
             "\"channel\":0,\"channel_name\":\"SITE\",\"channel_hash\":\"8c\","
             "\"sender_name\":\"HYTTA\",\"text\":\"HYTTA: str\xC3\xB8m tilbake\","
             "\"state\":\"received\",\"ack_expected\":false,\"mono_ms\":%lld}}",
             (long long)(now - 45000));
    o = cJSON_Parse(json);
    check("the channel message fixture is valid JSON", o != NULL);
    rift_model_apply_event(&app->model, "mesh.message", o);
    cJSON_Delete(o);

    /* And one of ours, which is where the caption that matters is drawn:
     * sent_flood is the last state a channel message can reach, and the
     * caption has to say that rather than leave a permanent "SENT" reading
     * as a delivery that has not turned up. */
    snprintf(json, sizeof(json),
             "{\"message\":{\"id\":31,\"direction\":\"out\",\"kind\":\"channel\","
             "\"channel\":0,\"channel_name\":\"SITE\",\"channel_hash\":\"8c\","
             "\"sender_name\":\"K230-A\",\"text\":\"K230-A: mottatt\","
             "\"state\":\"sent_flood\",\"ack_expected\":false,\"mono_ms\":%lld}}",
             (long long)(now - 20000));
    o = cJSON_Parse(json);
    check("the outgoing channel fixture is valid JSON", o != NULL);
    rift_model_apply_event(&app->model, "mesh.message", o);
    cJSON_Delete(o);
    rift_app_refresh(app);
    pump(60);
}

/* A conversation longer than any pane it is drawn in: sixty messages with
 * the fourth node, the last of them "the newest line". It was twenty-four,
 * until a one-line message became one line tall and twenty-four fitted the
 * portrait pane with room to spare. */
#define LONG_THREAD 60
static void give_long_thread(void)
{
    char json[512];
    cJSON *o;
    int64_t now = rift_mono_ms();
    int i;

    for (i = 0; i < LONG_THREAD; i++) {
        snprintf(json, sizeof(json),
                 "{\"message\":{\"id\":%d,\"direction\":\"%s\",\"peer_public_key\":\"" KEY_E "\","
                 "\"peer_name\":\"never-heard\",\"text\":\"%s %d\",\"state\":\"%s\","
                 "\"mono_ms\":%lld}}",
                 100 + i, i % 2 ? "out" : "in", i == LONG_THREAD - 1 ? "the newest line" : "line",
                 i, i % 2 ? "sent_direct" : "received",
                 (long long)(now - 1000LL * (2 * LONG_THREAD - i)));
        o = cJSON_Parse(json);
        rift_model_apply_event(&app->model, "mesh.message", o);
        cJSON_Delete(o);
    }
    rift_app_refresh(app);
    pump(60);
}

/* One message arriving now, from the peer whose thread is not open: the only
 * thing that can make an unread badge appear. */
static void give_unread(void)
{
    char json[512];
    cJSON *o;

    snprintf(json, sizeof(json),
             "{\"message\":{\"id\":20,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
             "\"peer_name\":\"OSLO-01\",\"text\":\"er du der?\",\"state\":\"received\","
             "\"mono_ms\":%lld}}",
             (long long)rift_mono_ms());
    o = cJSON_Parse(json);
    rift_model_apply_event(&app->model, "mesh.message", o);
    cJSON_Delete(o);
    rift_app_refresh(app);
    pump(60);
}

/* ---- finding things -------------------------------------------------------- */

static lv_obj_t *kid(lv_obj_t *parent, uint32_t i)
{
    return parent && lv_obj_get_child_count(parent) > i ? lv_obj_get_child(parent, (int32_t)i)
                                                        : NULL;
}

static int visible(lv_obj_t *obj)
{
    while (obj) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
            return 0;
        }
        obj = lv_obj_get_parent(obj);
    }
    return 1;
}

/* The composer is private to its screen, and what these tests are about is
 * whether the keys reach it, so it is found the way a finger would: the one
 * text area showing under this subtree. */
static lv_obj_t *find_textarea(lv_obj_t *obj)
{
    uint32_t n = lv_obj_get_child_count(obj);
    uint32_t i;

    if (lv_obj_check_type(obj, &lv_textarea_class) && visible(obj)) {
        return obj;
    }
    for (i = 0; i < n; i++) {
        lv_obj_t *hit = find_textarea(lv_obj_get_child(obj, (int32_t)i));

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

/* The button a label belongs to: rift_action puts the label inside it. */
static lv_obj_t *action_of(lv_obj_t *label)
{
    return label ? lv_obj_get_parent(label) : NULL;
}

static lv_obj_t *find_text(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    if (!obj) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class) && visible(obj)) {
        const char *t = lv_label_get_text(obj);

        if (t && strstr(t, text)) {
            return obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_text(lv_obj_get_child(obj, (int32_t)i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

/* How many of a scrolling pane's children are whole on screen: the message
 * rows a reader can actually read in a thread of this height. */
static int rows_in_view(lv_obj_t *scroll)
{
    lv_area_t view;
    uint32_t i;
    int n = 0;

    if (!scroll) {
        return 0;
    }
    lv_obj_update_layout(scroll);
    lv_obj_get_content_coords(scroll, &view);
    for (i = 0; i < lv_obj_get_child_count(scroll); i++) {
        lv_obj_t *row = lv_obj_get_child(scroll, (int32_t)i);
        lv_area_t a;

        lv_obj_get_coords(row, &a);
        if (visible(row) && a.y1 >= view.y1 && a.y2 <= view.y2) {
            n++;
        }
    }
    return n;
}

/* The thread's scrolling area as a share of the whole display: how much of
 * the screen is messages (DS §37.2 is measured by this). */
static double thread_share(lv_obj_t *scroll)
{
    lv_area_t a;
    int32_t dw = lv_display_get_horizontal_resolution(disp);
    int32_t dh = lv_display_get_vertical_resolution(disp);

    if (!scroll) {
        return 0;
    }
    lv_obj_update_layout(scroll);
    lv_obj_get_content_coords(scroll, &a);
    printf("     thread area: %dx%d of %dx%d = %.1f%%\n", (int)lv_area_get_width(&a),
           (int)lv_area_get_height(&a), (int)dw, (int)dh,
           100.0 * lv_area_get_width(&a) * lv_area_get_height(&a) / ((double)dw * dh));
    return 100.0 * lv_area_get_width(&a) * lv_area_get_height(&a) / ((double)dw * dh);
}

static int count_visible_of_height(lv_obj_t *obj, int32_t height)
{
    uint32_t i;
    int n = 0;

    if (!obj) {
        return 0;
    }
    if (visible(obj) && lv_obj_get_height(obj) == height) {
        n++;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        n += count_visible_of_height(lv_obj_get_child(obj, (int32_t)i), height);
    }
    return n;
}

/* Is the top of this caption drawn? LVGL clips a child to its parent's box,
 * or - for a parent that lets its children overflow - to that box grown by
 * the parent's extended draw size, and every ancestor clips again
 * (lv_obj_redraw). A caption centred on its panel's top rule rises above
 * the panel, so each ancestor between it and the screen has to leave room. */
static lv_obj_t *content(void);

/* How far an object says it draws beyond its box, asked the way LVGL asks it
 * (LV_EVENT_REFR_EXT_DRAW_SIZE) - the getter's name is not the same in every
 * LVGL this builds against. */
static int32_t ext_draw_of(lv_obj_t *obj)
{
    int32_t size = 0;

    lv_obj_send_event(obj, LV_EVENT_REFR_EXT_DRAW_SIZE, &size);
    return size;
}

static int caption_unclipped_in(lv_obj_t *root, const char *text)
{
    lv_obj_t *caption = find_text(root, text);
    lv_obj_t *o;
    lv_area_t c;

    if (!caption) {
        return 0;
    }
    lv_obj_update_layout(caption);
    lv_obj_get_coords(caption, &c);
    for (o = lv_obj_get_parent(caption); o; o = lv_obj_get_parent(o)) {
        lv_area_t a;

        lv_obj_get_coords(o, &a);
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) {
            int32_t e = ext_draw_of(o);

            lv_area_increase(&a, e, e);
        }
        if (c.y1 < a.y1) {
            printf("     caption \"%s\" clipped %d px by an ancestor\n", text, (int)(a.y1 - c.y1));
            return 0;
        }
    }
    return 1;
}

/* COMMS's thread pane, so a search for a message's words finds its body and
 * not the conversation list's preview of it. */
static lv_obj_t *thread_pane(void)
{
    return kid(kid(content(), 2), 1);
}

static int caption_unclipped(const char *text)
{
    return caption_unclipped_in(content(), text);
}

static lv_obj_t *ancestor(lv_obj_t *obj, int n)
{
    while (obj && n-- > 0) {
        obj = lv_obj_get_parent(obj);
    }
    return obj;
}

/* Every visible button under obj whose label is wider than the button: a
 * word cut at both edges. LVGL's flex grow gives buttons equal shares
 * whatever their words need, so a long word in a row of four is the case. */
static int labels_overflowing(lv_obj_t *obj)
{
    uint32_t i;
    int n = 0;

    if (!obj || !visible(obj)) {
        return 0;
    }
    if (lv_obj_check_type(obj, &lv_button_class) && lv_obj_get_child_count(obj) > 0) {
        lv_obj_t *label = lv_obj_get_child(obj, 0);

        lv_obj_update_layout(obj);
        if (lv_obj_check_type(label, &lv_label_class) &&
            lv_obj_get_width(label) > lv_obj_get_width(obj)) {
            printf("     \"%s\" is %d px in a %d px button\n", lv_label_get_text(label),
                   (int)lv_obj_get_width(label), (int)lv_obj_get_width(obj));
            n++;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        n += labels_overflowing(lv_obj_get_child(obj, (int32_t)i));
    }
    return n;
}

/* Visible objects in the disabled state under obj. */
static int count_disabled(lv_obj_t *obj)
{
    uint32_t i;
    int n = 0;

    if (!obj || !visible(obj)) {
        return 0;
    }
    if (lv_obj_has_state(obj, LV_STATE_DISABLED)) {
        n++;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        n += count_disabled(lv_obj_get_child(obj, (int32_t)i));
    }
    return n;
}

/* A label whose text is exactly this, shown under obj. */
static lv_obj_t *find_exact(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    if (!obj) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class) && visible(obj)) {
        const char *t = lv_label_get_text(obj);

        if (t && strcmp(t, text) == 0) {
            return obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_exact(lv_obj_get_child(obj, (int32_t)i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

/* Is all of obj inside the visible part of view? */
static int within(lv_obj_t *obj, lv_obj_t *view)
{
    lv_area_t a;
    lv_area_t v;

    if (!obj || !view) {
        return 0;
    }
    lv_obj_update_layout(view);
    lv_obj_get_coords(obj, &a);
    lv_obj_get_coords(view, &v);
    return a.y1 >= v.y1 && a.y2 <= v.y2;
}

/* A mesh of n nodes, every one heard a different number of seconds ago, so
 * the list is taller than any body it is drawn in. */
/* A key for the i-th synthetic node or peer: 64 hex characters, a first
 * byte from salt, zeros, and a distinct tail. */
static const char *key_of(int i, int salt)
{
    static char buf[4][RIFT_KEY_HEX];
    static int at;
    char *key = buf[at++ % 4];

    snprintf(key, RIFT_KEY_HEX, "%02x", (unsigned)((salt + i) & 0xff));
    memset(key + 2, '0', 58);
    snprintf(key + 60, 5, "%04x", (unsigned)(i & 0xfff));
    return key;
}

static void give_many_nodes(int n)
{
    size_t cap = (size_t)n * 256 + 64;
    char *json = malloc(cap);
    size_t at = 0;
    cJSON *o;
    int64_t now = rift_mono_ms();
    int i;

    if (!json) {
        return;
    }
    at += (size_t)snprintf(json + at, cap - at, "{\"nodes\":[");
    for (i = 0; i < n; i++) {
        at += (size_t)snprintf(json + at, cap - at,
                               "%s{\"public_key\":\"%s\",\"name\":\"MANY-%02d\",\"type\":1,"
                               "\"path_known\":true,\"hops\":1,\"direct\":false,"
                               "\"path_hex\":\"a1\",\"last_heard_mono_ms\":%lld}",
                               i ? "," : "", key_of(i, 0x10), i,
                               (long long)(now - 1000LL * (i + 1)));
    }
    snprintf(json + at, cap - at, "]}");
    o = cJSON_Parse(json);
    check("the many-node fixture is valid JSON", o != NULL);
    rift_model_apply_nodes(&app->model, o);
    cJSON_Delete(o);
    free(json);
    rift_app_refresh(app);
    pump(80);
}

static void tap(lv_obj_t *obj)
{
    lv_area_t a;

    if (!obj) {
        return;
    }
    /* A finger reaches a control below the fold by scrolling to it first,
     * so the test does what the finger does rather than tapping a point
     * that is off the panel. */
    lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);
    pump(40);
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(80);
}

/* Everything on screen must be inside the body and clear of the panel's
 * rounded corners. */
static int inside_body(lv_obj_t *obj)
{
    lv_area_t a;
    lv_area_t b;

    if (!obj) {
        return 0;
    }
    lv_obj_update_layout(app_body);
    lv_obj_get_content_coords(app_body, &b);
    lv_obj_get_coords(obj, &a);
    return a.x1 >= b.x1 && a.x2 <= b.x2 && a.y1 >= b.y1 && a.y2 <= b.y2;
}

/* ---- screenshots ----------------------------------------------------------- *
 *
 * $RIFT_SHOTS_DIR: one PNG per screen this test reaches, taken from the same
 * fixtures every run, so the images are reproducible. There is no status bar
 * in them: that is the shell's, and the shell is not here.
 */
static const char *shots_dir;
static int shots_taken;
static const char *g_state_dir;

static void shot(const char *name)
{
#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
    char path[512];
    lv_draw_buf_t *snap;
    unsigned char *rgb;
    uint32_t y;
    unsigned rc;

    if (!shots_dir) {
        return;
    }
    pump(120);
    snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    if (!snap) {
        printf("FAIL snapshot %s\n", name);
        failed++;
        return;
    }
    rgb = malloc((size_t)snap->header.w * snap->header.h * 3);
    if (!rgb) {
        lv_draw_buf_destroy(snap);
        return;
    }
    /* LVGL RGB888 is stored B,G,R in memory; PNG wants R,G,B. */
    for (y = 0; y < snap->header.h; y++) {
        const unsigned char *src =
            (const unsigned char *)snap->data + (size_t)y * snap->header.stride;
        unsigned char *dst = rgb + (size_t)y * snap->header.w * 3;
        uint32_t x;

        for (x = 0; x < snap->header.w; x++) {
            dst[x * 3 + 0] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3 + 0];
        }
    }
    snprintf(path, sizeof(path), "%s/%s.png", shots_dir, name);
    rc = lodepng_encode24_file(path, rgb, snap->header.w, snap->header.h);
    free(rgb);
    lv_draw_buf_destroy(snap);
    if (rc) {
        printf("FAIL writing %s\n", path);
        failed++;
        return;
    }
    shots_taken++;
#else
    (void)name;
#endif
}

static lv_obj_t *frame(void)
{
    return kid(app_body, 0);
}
static lv_obj_t *strip(void)
{
    return kid(frame(), 0);
}
static lv_obj_t *content(void)
{
    return kid(frame(), 1);
}
static lv_obj_t *cmdline(void)
{
    return kid(frame(), 2);
}

/* ---- synthetic traffic ------------------------------------------------------ */

/* One live mesh.message event: what the service raises for a message that
 * has just arrived or been sent, and again on every state change. */
/* One live channel message on SITE (slot 0, hash 8c), as give_channels()
 * holds it. */
static void live_chan(int id, const char *dir, long stamp, const char *text)
{
    char json[768];
    cJSON *o;

    snprintf(json, sizeof(json),
             "{\"message\":{\"id\":%d,\"direction\":\"%s\",\"kind\":\"channel\",\"channel\":0,"
             "\"channel_name\":\"SITE\",\"channel_hash\":\"8c\",\"sender_name\":\"Kari\","
             "\"text\":\"Kari: %s\",\"state\":\"%s\",\"timestamp\":%ld}}",
             id, dir, text, strcmp(dir, "in") == 0 ? "received" : "sent_flood", stamp);
    o = cJSON_Parse(json);
    rift_model_apply_event(&app->model, "mesh.message", o);
    cJSON_Delete(o);
}

static void live_dm(int id, const char *dir, const char *key, long stamp, const char *text)
{
    char json[768];
    cJSON *o;

    snprintf(json, sizeof(json),
             "{\"message\":{\"id\":%d,\"direction\":\"%s\",\"peer_public_key\":\"%s\","
             "\"text\":\"%s\",\"state\":\"%s\",\"timestamp\":%ld,"
             "\"mono_ms\":%lld}}",
             id, dir, key, text, strcmp(dir, "in") == 0 ? "received" : "sent_direct", stamp,
             (long long)rift_mono_ms());
    o = cJSON_Parse(json);
    rift_model_apply_event(&app->model, "mesh.message", o);
    cJSON_Delete(o);
}

/* The pulse a NODES row draws for the node of this name: the last cell of
 * the row line (name -> name box -> line). */
static enum rift_pulse node_pulse(const char *name)
{
    lv_obj_t *label = find_exact(content(), name);
    lv_obj_t *line = ancestor(label, 2);

    return line ? rift_pulse_get(lv_obj_get_child(line, -1)) : RIFT_PULSE_NONE;
}

static int file_says(const char *path, const char *want)
{
    char buf[256];
    size_t got;
    FILE *f = fopen(path, "r");

    if (!f) {
        return 0;
    }
    got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    return strstr(buf, want) != NULL;
}

/* ---- the DM sound, end to end --------------------------------------------- */

static void sound_session(const char *state_dir)
{
    char path[512];
    lv_obj_t *toggle;
    int plays;
    int stops;

    app_start();
    quiet_client();
    give_nodes();
    give_service();
    check("the DM sound is on unless the reader turned it off", app->prefs.dm_sound == 1);
    plays = fake_plays;
    give_messages();
    pump(120);
    check("the history found on opening makes no sound", fake_plays == plays);

    live_dm(900, "in", KEY_B, 1900, "a new one");
    pump(120);
    check("a new direct message makes one sound", fake_plays == plays + 1);
    check("the direct message's sound", fake_kind == RIFT_SOUND_DM);
    check("at the system volume", fake_volume == 80);
    live_dm(900, "in", KEY_B, 1900, "a new one");
    pump(120);
    check("the same event again makes no second one", fake_plays == plays + 1);

    pump(RIFT_NOTIFY_GAP_MS + 200);
    plays = fake_plays;
    live_dm(901, "out", KEY_B, 1901, "mine");
    pump(120);
    check("the reader's own message makes none", fake_plays == plays);
    {
        cJSON *o = cJSON_Parse("{\"message\":{\"id\":902,\"direction\":\"in\",\"kind\":\"channel\","
                               "\"channel\":0,\"channel_name\":\"SITE\",\"channel_hash\":\"8c\","
                               "\"sender_name\":\"X\",\"text\":\"X: all\",\"state\":\"received\"}}");

        rift_model_apply_event(&app->model, "mesh.message", o);
        cJSON_Delete(o);
        pump(120);
    }
    check("a channel message makes the other sound",
          fake_plays == plays + 1 && fake_kind == RIFT_SOUND_CHANNEL);
    pump(RIFT_NOTIFY_GAP_MS + 200);
    plays = fake_plays;
    live_dm(903, "in", KEY_B, 1900, "a new one");
    pump(120);
    check("nor the sender's retry of one already heard", fake_plays == plays);
    {
        cJSON *o = cJSON_Parse("{\"persistent\":false,\"messages\":[{\"id\":904,"
                               "\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
                               "\"text\":\"missed while away\",\"state\":\"received\"}]}");

        rift_model_apply_messages(&app->model, o);
        cJSON_Delete(o);
        rift_app_refresh(app);
        pump(120);
    }
    check("nor a reconnect's snapshot, even of a message not seen before",
          fake_plays == plays && rift_model_unread(&app->model, KEY_A) >= 1);
    {
        int i;

        for (i = 0; i < 10; i++) {
            live_dm(910 + i, "in", i % 2 ? KEY_A : KEY_B, 1910 + i, "burst");
        }
        pump(120);
        for (i = 0; i < 10; i++) {
            live_dm(920 + i, "in", KEY_A, 1920 + i, "more");
            pump(200);
        }
    }
    check("twenty in a few seconds are one sound", fake_plays == plays + 1);

    /* The setting, on SYSTEM, in the Settings app's shape. */
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(60);
    toggle = action_of(find_exact(content(), "ON"));
    check("SYSTEM has the DM sound's switch", toggle != NULL &&
                                                    find_text(content(), "Sound for a new DM") != NULL);
    check("a 56 px action", toggle && lv_obj_get_height(toggle) == RIFT_TOUCH_H);
    check("saying what it does", find_text(content(), "A new direct message: two short rising notes") !=
                                     NULL);
    check("beside the channel message's own switch",
          find_text(content(), "Sound for channels") != NULL &&
              find_text(content(), "one softer note") != NULL);
    check("with its caption drawn whole", caption_unclipped("SOUND"));
    shot("portrait-activity-notify");
    tap(toggle);
    snprintf(path, sizeof(path), "%s/rift/prefs.v1", state_dir);
    check("a press turns it off", app->prefs.dm_sound == 0 &&
                                      find_exact(content(), "OFF") != NULL);
    check("and says so", find_text(content(), "Off: a new direct message is shown") != NULL);
    check("stored in the app's own preferences file", file_says(path, "dm_sound=0"));
    pump(RIFT_NOTIFY_GAP_MS + 200);
    plays = fake_plays;
    live_dm(940, "in", KEY_B, 1940, "while off");
    pump(120);
    check("off, a new direct message makes no sound", fake_plays == plays);
    check("and is still unread", rift_model_unread(&app->model, KEY_B) >= 1);

    tap(action_of(find_exact(content(), "OFF")));
    check("a second press turns it on again", app->prefs.dm_sound == 1 &&
                                                  file_says(path, "dm_sound=1"));
    test_muted = 1;
    rift_app_refresh(app);
    pump(60);
    check("muted, the switch says nothing will be heard", find_text(content(), "muted") != NULL);
    live_dm(941, "in", KEY_B, 1941, "while muted");
    pump(120);
    check("and nothing is played", fake_plays == plays);
    test_muted = 0;

    /* The build as it ships: no platform sound to ask for. */
    rift_sound_set_backend(NULL);
    rift_app_refresh(app);
    pump(60);
    check("with no system sound the switch says so",
          find_text(content(), "No system notification sound") != NULL);
    live_dm(942, "in", KEY_B, 1942, "unheard");
    pump(120);
    check("and nothing is asked of a backend that is not there", fake_plays == plays);
    rift_sound_set_backend(&fake_sound);

    tap(action_of(find_exact(content(), "ON")));
    check("off again", app->prefs.dm_sound == 0);
    stops = fake_stops;
    app_stop();
    check("closing RIFT stops its sound, and leaves nothing open", fake_stops == stops + 1);

    app_start();
    quiet_client();
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(60);
    check("the setting survives closing and opening", app->prefs.dm_sound == 0 &&
                                                          find_exact(content(), "OFF") != NULL);
    app_stop();

    /* A preferences file that cannot be written: the choice holds for the
     * session, and the screen says it will not outlast it. */
    setenv("POCKETOS_STATE_DIR", "/proc/rift-app-test-cannot-write", 1);
    app_start();
    quiet_client();
    check("a store that cannot be read is the defaults", app->prefs.dm_sound == 1);
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(60);
    tap(action_of(find_exact(content(), "ON")));
    check("an unwritable store keeps the choice for the session",
          app->prefs.dm_sound == 0 && find_text(content(), "Not saved") != NULL);
    app_stop();
    setenv("POCKETOS_STATE_DIR", state_dir, 1);

    /* ---- the channel sound, and one channel muted ------------------------- */
    app_start();
    quiet_client();
    give_service();
    give_channels();
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(120);
    rift_app_set_dm_sound(app, 1); /* the file above was left with it off */
    pump(RIFT_NOTIFY_GAP_MS + 200);
    plays = fake_plays;
    live_chan(950, "in", 2950, "hei");
    pump(120);
    check("a new channel message makes the channel sound",
          fake_plays == plays + 1 && fake_kind == RIFT_SOUND_CHANNEL);
    {
        lv_obj_t *mute = action_of(find_exact(content(), "MUTE"));
        lv_obj_t *ch_row;
        lv_obj_t *ch_toggle;
        char site[RIFT_KEY_HEX] = "";

        check("each channel row has MUTE, as a 56 px action",
              mute && count_exact(content(), "MUTE") == 2 &&
                  lv_obj_get_height(mute) == RIFT_TOUCH_H);
        tap(mute);
        pump(120);
        if (app->prefs.mute_count == 1) {
            snprintf(site, sizeof(site), "%s", app->prefs.mute[0]);
        }
        check("a press mutes that channel, and only that one",
              app->prefs.mute_count == 1 && site[0] == '#' && strstr(site, ":8c:") != NULL);
        check("the row says so: MUTED, on that row alone",
              count_exact(content(), "MUTED") == 1 && count_exact(content(), "MUTE") == 1);
        snprintf(path, sizeof(path), "%s/rift/prefs.v1", state_dir);
        check("stored in the app's own preferences file", file_says(path, "channel_mute=#0:8c:"));
        pump(RIFT_NOTIFY_GAP_MS + 200);
        plays = fake_plays;
        live_chan(951, "in", 2951, "while muted");
        pump(120);
        check("a muted channel makes no sound", fake_plays == plays);
        check("but its message is received, kept and unread",
              rift_model_unread(&app->model, site) >= 1);
        live_dm(952, "in", KEY_B, 2952, "a dm meanwhile");
        pump(120);
        check("and a direct message still sounds", fake_plays == plays + 1 &&
                                                       fake_kind == RIFT_SOUND_DM);

        /* The global channel switch is not a mute, and a mute is not it. */
        ch_row = lv_obj_get_parent(find_text(content(), "Sound for channels"));
        ch_toggle = ch_row ? lv_obj_get_child(ch_row, 1) : NULL;
        tap(action_of(find_exact(content(), "MUTED")));
        pump(120);
        check("MUTED again unmutes it", app->prefs.mute_count == 0 &&
                                            !file_says(path, "channel_mute="));
        tap(ch_toggle);
        pump(120);
        check("channel sounds off is the switch, not a mute",
              app->prefs.ch_sound == 0 && app->prefs.mute_count == 0 &&
                  file_says(path, "channel_sound=0") &&
                  find_text(content(), "Off: a new channel message is shown") != NULL);
        pump(RIFT_NOTIFY_GAP_MS + 200);
        plays = fake_plays;
        live_chan(953, "in", 2953, "channels off");
        pump(120);
        check("with channel sounds off, an unmuted channel makes no sound", fake_plays == plays);
        tap(ch_toggle);
        pump(120);
        pump(RIFT_NOTIFY_GAP_MS + 200);
        plays = fake_plays;
        live_chan(954, "out", 2954, "mine");
        pump(120);
        check("this device's own channel message makes none", fake_plays == plays);
        rift_app_refresh(app);
        rift_app_show_section(app, RIFT_SEC_COMMS);
        pump(200);
        rift_app_show_section(app, RIFT_SEC_SYSTEM);
        pump(200);
        check("and repainting, or the thread being drawn again, makes none", fake_plays == plays);
        tap(action_of(find_exact(content(), "MUTE")));
        pump(120);
    }
    app_stop();
    app_start();
    quiet_client();
    give_channels();
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(120);
    check("the mute survives closing and opening",
          app->prefs.mute_count == 1 && find_exact(content(), "MUTED") != NULL);
    tap(action_of(find_exact(content(), "MUTED")));
    pump(120);
    rift_app_set_dm_sound(app, 0); /* as the sessions after this one found it */
    app_stop();
}

/* ---- scale: the whole node table, many contacts, a long history ------------ */

/* What one repaint of the section on screen costs on this host, printed and
 * not checked: a host is not the board, and a timing check would fail on a
 * loaded machine rather than on a slow app. It is here so a change that
 * makes a repaint scale with the mesh is visible in the log. */
static void report_refresh_cost(const char *what)
{
    struct timespec t0;
    struct timespec t1;
    int n;

    double refresh_ms;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (n = 0; n < 20; n++) {
        rift_app_refresh(app);
        lv_obj_update_layout(lv_screen_active());
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    refresh_ms =
        ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / n;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (n = 0; n < 20; n++) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(disp);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("     %s: %.2f ms per refresh and layout, %.2f ms per full redraw (host)\n", what,
           refresh_ms,
           ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / n);
}

/* Opened in landscape, as the shell opens it from a turned launcher: the
 * frame is created in the console shape rather than turned into it. The
 * running shell hung here on 2026-09-28 while every screenshot run - which
 * opens the app at boot - passed, so the path has a session of its own. */
static void landscape_start_session(void)
{
    use_display(POS_ROTATION_270, PANEL_CORNER);
    app_start();
    check("opened in landscape, the app is wide from the first pass", app && app->wide);
    quiet_client();
    give_nodes();
    give_service();
    pump(200);
    check("the strip is the screen's top row with the way back in it",
          lv_obj_get_height(strip()) == RIFT_NAV_ROW_H_TOP && app->back && visible(app->back));
    give_messages();
    rift_app_show_section(app, RIFT_SEC_COMMS);
    pump(120);
    rift_app_open_conversation(app, KEY_B);
    pump(120);
    check("a conversation opens", find_text(content(), "Fint, ser deg") != NULL);
    check("with the command line as the composer, a data row",
          lv_obj_get_height(cmdline()) == RIFT_ROW_H && app->composer &&
              visible(lv_obj_get_parent(app->composer)));
    rift_app_show_section(app, RIFT_SEC_NODES);
    pump(120);
    rift_app_show_section(app, RIFT_SEC_ACTIVITY);
    pump(120);
    check("and the app is still inside the body", inside_body(content()));
    app_stop();
    use_display(POS_ROTATION_0, PANEL_CORNER);
}

/* ---- every text size (DS §46) ---------------------------------------------- */

/* The width a label's words take in the font and spacing it is drawn in,
 * which is what a text size changes. */
static int32_t words_w(lv_obj_t *label)
{
    lv_point_t size;
    const char *t = lv_label_get_text(label);

    if (!t || !t[0]) {
        return 0;
    }
    lv_text_get_size(&size, t, lv_obj_get_style_text_font(label, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(label, LV_PART_MAIN), LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    return size.x;
}

/* Visible one-line labels under obj that do not show their words whole:
 * wider than their own box, which LVGL clips at an edge - the left one, for
 * a right-aligned number - or reaching outside their parent's box, where
 * they are drawn over a neighbour or cut by the row. A wrapping label says
 * its words on more lines and is not counted. */
static int captions_clipped(lv_obj_t *obj)
{
    uint32_t i;
    int n = 0;

    if (!obj || !visible(obj)) {
        return 0;
    }
    if (lv_obj_check_type(obj, &lv_label_class) &&
        lv_label_get_long_mode(obj) != LV_LABEL_LONG_WRAP && words_w(obj) > 0) {
        lv_area_t c;
        lv_area_t p;

        lv_obj_update_layout(obj);
        lv_obj_get_coords(obj, &c);
        lv_obj_get_coords(lv_obj_get_parent(obj), &p);
        if (words_w(obj) > lv_obj_get_content_width(obj)) {
            printf("     \"%s\" is %d px in a %d px cell\n", lv_label_get_text(obj),
                   (int)words_w(obj), (int)lv_obj_get_content_width(obj));
            n++;
        } else if (c.x1 < p.x1 || c.x2 > p.x2) {
            printf("     \"%s\" at x %d..%d reaches outside its row's %d..%d\n",
                   lv_label_get_text(obj), (int)c.x1, (int)c.x2, (int)p.x1, (int)p.x2);
            n++;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        n += captions_clipped(lv_obj_get_child(obj, (int32_t)i));
    }
    return n;
}

/* RIFT at Small, Medium and Large, upright and turned: every caption in the
 * strip and the three sections that draw one shows its words whole. Seen on
 * unit B at Large in landscape (docs/hardware/RIFT_MANAGEMENT_GATE.md, "Seen,
 * not changed"): the strip's keys cut at their left edge, NODES' HOPS and
 * HEARD clipped by their columns, COMMS' CONVERSATIONS run under HEARD, and
 * MESH ACTIVITY's caption cut by its legend. The shell re-creates the open
 * app when the size changes (DS §46), so each size is a new app here too. */
static lv_obj_t *net_rings(void);
static void give_node_json(const char *json);

static void text_size_session(void)
{
    static const enum pos_text_size sizes[] = { POS_TEXT_SIZE_SMALL, POS_TEXT_SIZE_MEDIUM,
                                                POS_TEXT_SIZE_LARGE };
    static const enum pos_rotation turns[] = { POS_ROTATION_270, POS_ROTATION_0 };
    enum pos_text_size was = pos_theme_current_text_size();
    size_t s;
    size_t t;

    for (t = 0; t < sizeof(turns) / sizeof(turns[0]); t++) {
        for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            const char *size = pos_text_size_name(sizes[s]);
            const char *shape = turns[t] == POS_ROTATION_0 ? "portrait" : "landscape";
            char what[160];

            pos_theme_select_text_size(sizes[s]);
            use_display(turns[t], PANEL_CORNER);
            app_start();
            quiet_client();
            give_nodes();
            give_service();
            give_messages();
            pump(200);

            rift_app_show_section(app, RIFT_SEC_ACTIVITY);
            pump(200);
            snprintf(what, sizeof(what), "%s, %s: ACTIVITY's captions are whole", size, shape);
            check(what, captions_clipped(frame()) == 0);
            snprintf(what, sizeof(what), "%s, %s: MESH ACTIVITY keeps its value beside the legend",
                     size, shape);
            check(what, find_text(content(), "PEAK 1/MIN") != NULL);
            if (app->wide && sizes[s] == POS_TEXT_SIZE_LARGE) {
                shot("landscape-activity-large");
            }

            rift_app_show_section(app, RIFT_SEC_SYSTEM);
            pump(200);
            snprintf(what, sizeof(what), "%s, %s: SYSTEM's captions are whole, its words in their buttons",
                     size, shape);
            check(what, captions_clipped(frame()) == 0 && labels_overflowing(content()) == 0);

            rift_app_show_section(app, RIFT_SEC_NODES);
            pump(200);
            snprintf(what, sizeof(what), "%s, %s: NODES' captions are whole", size, shape);
            check(what, captions_clipped(frame()) == 0);
            {
                lv_obj_t *hops = find_exact(content(), "HOPS");
                lv_obj_t *heard = find_exact(content(), "HEARD");

                snprintf(what, sizeof(what), "%s, %s: HOPS and HEARD fit their columns", size,
                         shape);
                check(what, hops && heard && words_w(hops) <= lv_obj_get_content_width(hops) &&
                                words_w(heard) <= lv_obj_get_content_width(heard));
            }
            if (app->wide) {
                /* The strip's caption leads with the keys, whatever it has to
                 * leave off its end; 256 nodes is unit B's count. */
                give_many_nodes(256);
                pump(200);
                snprintf(what, sizeof(what), "%s, %s: the strip names the keys first and whole",
                         size, shape);
                check(what, app->cmd_hint &&
                                strncmp(lv_label_get_text(app->cmd_hint),
                                        "\xE2\x86\x91\xE2\x86\x93 SELECT" RIFT_SEP
                                        "ENTER MESSAGE",
                                        strlen("\xE2\x86\x91\xE2\x86\x93 SELECT" RIFT_SEP
                                               "ENTER MESSAGE")) == 0 &&
                                captions_clipped(strip()) == 0);
                printf("     %s strip: \"%s\"\n", size, lv_label_get_text(app->cmd_hint));
                if (sizes[s] == POS_TEXT_SIZE_LARGE) {
                    shot("landscape-nodes-large");
                }
                give_nodes();
                rift_app_refresh(app);
                pump(200);
            }

            rift_app_show_section(app, RIFT_SEC_COMMS);
            rift_app_open_conversation(app, KEY_B);
            pump(200);
            snprintf(what, sizeof(what), "%s, %s: COMMS' captions are whole", size, shape);
            check(what, captions_clipped(frame()) == 0);
            {
                lv_obj_t *title = find_text(content(), "CONVERSA");
                lv_obj_t *heard = find_exact(content(), "HEARD");
                lv_area_t a = { 0 };
                lv_area_t b = { 0 };

                if (title && heard) {
                    lv_obj_get_coords(title, &a);
                    lv_obj_get_coords(heard, &b);
                    printf("     \"%s\" %d..%d, HEARD %d..%d\n", lv_label_get_text(title),
                           (int)a.x1, (int)a.x2, (int)b.x1, (int)b.x2);
                }
                snprintf(what, sizeof(what),
                         "%s, %s: the list's title ends before HEARD begins", size, shape);
                check(what, title && heard && a.x2 < b.x1);
            }
            if (app->wide && sizes[s] == POS_TEXT_SIZE_LARGE) {
                shot("landscape-comms-large");
            }
            /* NET: a mesh spread over rings 1 to 9, half of it placed by
             * advert, as unit B's is. Every ring's word and count fits its
             * own column - in landscape a ninth of the body - and none runs
             * into the next (unit B, Large, 2026-10-02: "1 DIRECT 2"). */
            {
                char json[512];
                int i;
                int bad = 0;
                int seen = 0;
                lv_obj_t *rings;

                for (i = 0; i < 60; i++) {
                    snprintf(json, sizeof(json),
                             "{\"reason\":\"discovered\",\"node\":{\"public_key\":\"%064x\","
                             "\"name\":\"N%02d\",\"type\":2,\"path_known\":%s,\"hops\":%d,"
                             "\"last_heard_mono_ms\":%lld,\"advert_hops\":%d,"
                             "\"advert_mono_ms\":%lld}}",
                             0x5000 + i, i, i % 2 ? "true" : "false", i % 9 + 1,
                             (long long)(rift_mono_ms() - 30000), i % 9,
                             (long long)(rift_mono_ms() - 30000));
                    give_node_json(json);
                }
                rift_app_show_section(app, RIFT_SEC_NET);
                rift_app_refresh(app);
                pump(300);
                rings = net_rings();
                for (i = 0; rings && i < (int)lv_obj_get_child_count(rings); i++) {
                    lv_obj_t *box = kid(rings, (uint32_t)i);
                    lv_obj_t *head = kid(box, 0);
                    lv_area_t bx;
                    lv_area_t col;
                    uint32_t j;

                    if (!visible(box)) {
                        continue;
                    }
                    seen++;
                    /* The heading column: in portrait the row holds the pills too. */
                    lv_obj_get_coords(head, &bx);
                    lv_obj_get_coords(box, &col);
                    for (j = 0; j < 2; j++) {
                        lv_obj_t *l = kid(head, j);
                        lv_area_t la;
                        int32_t ends;

                        if (!l || !visible(l)) {
                            continue;
                        }
                        lv_obj_get_coords(l, &la);
                        /* Where the words end, not the label's box: and in
                         * landscape, where the columns meet edge to edge, at
                         * least 8 px short of the next ring's words. */
                        ends = la.x1 + words_w(l) - 1;
                        if (words_w(l) > lv_obj_get_content_width(l) || la.x2 > bx.x2 ||
                            (app->wide && ends > col.x2 - 8)) {
                            bad++;
                            printf("     NET %s %s: \"%s\" %d px in %d (words end %d, column %d)\n",
                                   size, shape, lv_label_get_text(l), (int)words_w(l),
                                   (int)lv_obj_get_content_width(l), (int)ends, (int)col.x2);
                        }
                    }
                }
                snprintf(what, sizeof(what),
                         "%s, %s: NET's ring words and counts fit their columns (%d rings)", size,
                         shape, seen);
                check(what, seen >= 10 && bad == 0);
                if (sizes[s] == POS_TEXT_SIZE_LARGE) {
                    printf("     NET %s %s:", size, shape);
                    for (i = 0; rings && i < (int)lv_obj_get_child_count(rings); i++) {
                        lv_obj_t *head = kid(kid(rings, (uint32_t)i), 0);

                        if (visible(head)) {
                            printf(" [%s|%s]", lv_label_get_text(kid(head, 0)),
                                   visible(kid(head, 1)) ? lv_label_get_text(kid(head, 1)) : "");
                        }
                    }
                    printf("\n");
                    shot(app->wide ? "landscape-net-large" : "portrait-net-large");
                }
            }
            app_stop();
        }
    }
    pos_theme_select_text_size(was);
    use_display(POS_ROTATION_0, PANEL_CORNER);
}

static void scale_session(void)
{
    lv_obj_t *list;
    char sel[RIFT_KEY_HEX];
    char last[16];
    lv_mem_monitor_t mem_before;
    lv_mem_monitor_t mem_after;
    int i;

    app_start();
    quiet_client();
    give_service();
    /* The last session left the sound off, and this one counts sounds. */
    rift_app_set_dm_sound(app, 1);

    /* As many nodes as the cache holds - a thousand, more than meshcored's
     * table today - with the heap and the repaint measured against them. */
    snprintf(last, sizeof(last), "MANY-%02d", RIFT_MAX_NODES - 1);
    lv_mem_monitor(&mem_before);
    give_many_nodes(RIFT_MAX_NODES);
    check("the model holds every node the cache is sized for",
          app->model.node_count == RIFT_MAX_NODES);
    rift_app_show_section(app, RIFT_SEC_NODES);
    pump(120);
    list = ancestor(find_exact(content(), "MANY-00"), 4);
    check("a thousand nodes are one list", list != NULL && lv_obj_get_scroll_bottom(list) > 0);
    check("which builds rows for the screen, not for the mesh",
          rift_nodes_rows_built(app) > 0 && rift_nodes_rows_built(app) <= 48);
    report_refresh_cost("NODES, every node the cache holds");
    lv_mem_monitor(&mem_after);
    {
        struct rusage ru;

        getrusage(RUSAGE_SELF, &ru);
        /* LVGL's heap monitor reads 0 under the C library allocator, so the
         * process's own high-water mark is the measure of the whole. */
        printf("     %d nodes, %d rows built (portrait); model %zu KB (a node %zu B, a message "
               "%zu B); LVGL heap %zu KB used of %zu; process max RSS %ld KB\n",
               app->model.node_count, rift_nodes_rows_built(app), sizeof(app->model) / 1024,
               sizeof(struct rift_node), sizeof(struct rift_message),
               (mem_after.total_size - mem_after.free_size) / 1024, mem_after.total_size / 1024,
               ru.ru_maxrss);
    }
    {
        /* The order alone, at this size, is what a repaint must pay at
         * least: it was quadratic once. */
        const struct rift_node *order[RIFT_MAX_NODES];
        struct timespec t0;
        struct timespec t1;
        int n;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (n = 0; n < 100; n++) {
            rift_model_order(&app->model, rift_app_now(app), order, RIFT_MAX_NODES);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        printf("     ordering %d nodes: %.3f ms (host)\n", RIFT_MAX_NODES,
               ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) /
                   n);
    }
    {
        int rows = count_visible_of_height(list, RIFT_ROW_H);

        /* A 36 px row, which the name box inside it matches: two per row. */
        check("and fills the portrait pane with rows", rows / 2 >= 25);
    }
    lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
    pump(80);
    check("the last of them is reached by scrolling",
          find_exact(content(), last) != NULL && within(find_exact(content(), last), list));
    check("with no more rows built for it", rift_nodes_rows_built(app) <= 48);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    pump(60);
    check("and back at the top the first is there again",
          find_exact(content(), "MANY-00") != NULL && within(find_exact(content(), "MANY-00"), list));

    /* The selection is a key, not a row: it survives the list re-ordering
     * under it, and the arrows walk the order as it is now. */
    snprintf(sel, sizeof(sel), "%s", key_of(200, 0x10));
    rift_app_select(app, sel);
    pump(80);
    check("a node two hundred rows down can be selected",
          rift_app_selected(app) && strcmp(rift_app_selected(app)->key, sel) == 0);
    check("and is brought into view, expansion and all",
          within(ancestor(find_exact(content(), "MANY-200"), 3), list));
    {
        char json[256];
        cJSON *o;

        snprintf(json, sizeof(json),
                 "{\"reason\":\"advert\",\"node\":{\"public_key\":\"%s\",\"name\":\"MANY-200\","
                 "\"path_known\":true,\"hops\":1,\"path_hex\":\"a1\",\"last_heard_mono_ms\":%lld}}",
                 sel, (long long)rift_mono_ms());
        o = cJSON_Parse(json);
        rift_model_apply_event(&app->model, "mesh.node", o);
        cJSON_Delete(o);
        rift_app_refresh(app);
        pump(80);
    }
    check("heard again, it moves to the top of the order and stays selected",
          app->have_selected && strcmp(app->selected, sel) == 0);
    rift_nodes_key(app, LV_KEY_DOWN);
    pump(60);
    check("and the down arrow walks the new order, not the old index",
          strcmp(app->selected, key_of(0, 0x10)) == 0);
    {
        char json[160];
        cJSON *o;

        snprintf(json, sizeof(json), "{\"reason\":\"removed\",\"node\":{\"public_key\":\"%s\"}}",
                 app->selected);
        o = cJSON_Parse(json);
        rift_model_apply_event(&app->model, "mesh.node", o);
        cJSON_Delete(o);
        rift_app_refresh(app);
        pump(60);
    }
    check("a selected node that is forgotten leaves no selection behind",
          rift_app_selected(app) == NULL);
    check("and the arrows start again from the top", rift_nodes_key(app, LV_KEY_DOWN) == 1 &&
                                                        rift_app_selected(app) != NULL);

    use_display(POS_ROTATION_270, PANEL_CORNER);
    pump(120);
    /* MANY-00 was forgotten above; MANY-01 is the top of the list now. */
    lv_obj_scroll_to_y(ancestor(find_exact(content(), "MANY-200"), 4), 0, LV_ANIM_OFF);
    pump(60);
    list = ancestor(find_exact(content(), "MANY-01"), 4);
    check("turned, the list is still bounded by the screen",
          app->wide && rift_nodes_rows_built(app) <= 48);
    printf("     %d rows built after turning\n", rift_nodes_rows_built(app));
    if (list) {
        lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
        pump(80);
    }
    check("and reaches its last node in landscape too",
          list && find_exact(content(), last) && within(find_exact(content(), last), list));
    check("the strip counts what is active now", find_text(strip(), " NOW") != NULL);
    check("and the turned list stays inside the body", inside_body(content()));
    shot("landscape-nodes-256");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    pump(120);

    /* As many conversations as the list holds, from a snapshot: history,
     * so no sound. */
    {
        size_t cap = (size_t)RIFT_MAX_CONVERSATIONS * 400 + 128;
        char *json = malloc(cap);
        size_t at = 0;
        cJSON *o;

        at += (size_t)snprintf(json + at, cap - at, "{\"persistent\":false,\"messages\":[");
        for (i = 0; i < RIFT_MAX_CONVERSATIONS; i++) {
            at += (size_t)snprintf(json + at, cap - at,
                                   "%s{\"id\":%d,\"direction\":\"in\",\"peer_public_key\":\"%s\","
                                   "\"peer_name\":\"PEER-%02d\",\"text\":\"hello %d\","
                                   "\"state\":\"received\",\"timestamp\":%d,\"mono_ms\":%lld}",
                                   i ? "," : "", 1 + i, key_of(i, 0x40), i, i, 1000 + i,
                                   (long long)(rift_mono_ms() - 1000LL * (RIFT_MAX_CONVERSATIONS - i)));
        }
        snprintf(json + at, cap - at, "]}");
        o = cJSON_Parse(json);
        check("every conversation the list holds is taken", rift_model_apply_messages(&app->model, o) == 0);
        cJSON_Delete(o);
        free(json);
        o = cJSON_Parse("{\"channels\":[],\"count\":0,\"max\":8}");
        rift_model_apply_channels(&app->model, o);
        cJSON_Delete(o);
    }
    rift_app_show_section(app, RIFT_SEC_COMMS);
    pump(120);
    /* Newest first: the last peer spoke last and heads the list; PEER-00
     * spoke first and is at its foot, a screenful and more down. */
    {
        char top_peer[16];

        snprintf(top_peer, sizeof(top_peer), "PEER-%02d", RIFT_MAX_CONVERSATIONS - 1);
        list = ancestor(find_exact(content(), top_peer), 3);
        printf("     %d conversations: %d rows built, top row %s\n", RIFT_MAX_CONVERSATIONS,
               rift_comms_rows_built(app), list ? "found" : "NOT FOUND");
    }
    check("every conversation is in the list", list && rift_comms_rows_built(app) > 0 &&
                                                   lv_obj_get_scroll_bottom(list) > 0);
    if (!list) {
        /* Nothing below can be asked of a list that is not there, and an
         * LVGL call on NULL halts for ever (LV_ASSERT_HANDLER). */
        check("the conversation list is there to be tested", 0);
        app_stop();
        return;
    }
    check("which builds rows for the screen, not for the peers",
          rift_comms_rows_built(app) <= 40 && rift_comms_rows_built(app) < RIFT_MAX_CONVERSATIONS);
    printf("     %d conversations, %d rows built (portrait)\n", RIFT_MAX_CONVERSATIONS,
           rift_comms_rows_built(app));
    check("with nothing open the list takes the height",
          list && lv_obj_get_height(list) >= 20 * RIFT_ROW_H);
    check("and the oldest is not built until it is scrolled to",
          find_exact(content(), "PEER-00") == NULL);
    lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
    pump(80);
    check("the oldest conversation is reached by scrolling",
          find_exact(content(), "PEER-00") && within(find_exact(content(), "PEER-00"), list));
    check("with no more rows built for it", rift_comms_rows_built(app) <= 40);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    pump(60);
    shot("portrait-comms-many");
    rift_app_open_conversation(app, key_of(10, 0x40));
    pump(120);
    check("opening one gives the thread the height back",
          list && lv_obj_get_height(list) <= 5 * RIFT_ROW_H + 8 &&
              lv_obj_get_height(thread_pane()) > lv_obj_get_height(content()) / 2);
    check("with the open conversation in view", within(ancestor(find_exact(content(), "PEER-10"), 2),
                                                      list));
    {
        lv_obj_t *before[RIFT_MAX_CONVERSATIONS];
        uint32_t n = list ? lv_obj_get_child_count(list) : 0;
        int kept = 1;
        uint32_t k;

        for (k = 0; k < n && k < RIFT_MAX_CONVERSATIONS; k++) {
            before[k] = lv_obj_get_child(list, (int32_t)k);
        }
        live_dm(500, "in", key_of(40, 0x40), 5000, "news");
        rift_app_refresh(app);
        pump(80);
        for (k = 0; k < n && k < RIFT_MAX_CONVERSATIONS; k++) {
            if (lv_obj_get_child(list, (int32_t)k) != before[k]) {
                kept = 0;
            }
        }
        check("a message that re-orders the list rebuilds no row", kept &&
                                                                 lv_obj_get_child_count(list) == n);
        /* The pool's objects keep their order in the tree; which row is at
         * the top is where it was placed, not which child it is - and the
         * top is only built once the list is scrolled to it, which it was
         * not while the open conversation was revealed near the foot. */
        lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
        pump(80);
        check("and its conversation is the top row now",
              find_exact(list, "PEER-40") &&
                  lv_obj_get_y(ancestor(find_exact(list, "PEER-40"), 2)) <= 2);
    }

    /* A long history with one peer: 200 messages, the window at its end. */
    {
        size_t cap = 200 * 300 + 128;
        char *json = malloc(cap);
        size_t at = 0;
        cJSON *o;

        at += (size_t)snprintf(json + at, cap - at, "{\"persistent\":false,\"messages\":[");
        for (i = 0; i < 200; i++) {
            at += (size_t)snprintf(json + at, cap - at,
                                   "%s{\"id\":%d,\"direction\":\"%s\",\"peer_public_key\":\"%s\","
                                   "\"peer_name\":\"LONG\",\"text\":\"history %d\","
                                   "\"state\":\"%s\",\"timestamp\":%d,\"mono_ms\":%lld}",
                                   i ? "," : "", 1000 + i, i % 3 ? "in" : "out", key_of(99, 0x50), i,
                                   i % 3 ? "received" : "acked", 2000 + i,
                                   (long long)(rift_mono_ms() - 1000LL * (200 - i)));
        }
        snprintf(json + at, cap - at, "]}");
        o = cJSON_Parse(json);
        rift_model_apply_messages(&app->model, o);
        cJSON_Delete(o);
        free(json);
    }
    rift_app_open_conversation(app, key_of(99, 0x50));
    pump(120);
    {
        lv_obj_t *newest = find_text(thread_pane(), "history 199");
        lv_obj_t *scroll = ancestor(newest, 4);
        lv_obj_t *second;

        check("a long history opens at its end", newest && within(newest, scroll) &&
                                                     lv_obj_get_scroll_y(scroll) > 0);
        check("drawing a bounded window of it",
              scroll && lv_obj_get_child_count(scroll) == (uint32_t)RIFT_THREAD_ROWS);
        check("and saying how much is earlier", find_text(thread_pane(), "136 EARLIER") != NULL);
        report_refresh_cost("COMMS, a 200-message thread open");
        shot("portrait-comms-long");
        second = scroll ? lv_obj_get_child(scroll, 1) : NULL;
        live_dm(1200, "in", key_of(99, 0x50), 9000, "history 200");
        rift_app_refresh(app);
        pump(80);
        check("a message arriving moves the window along without rebuilding it",
              scroll && lv_obj_get_child(scroll, 0) == second &&
                  lv_obj_get_child_count(scroll) == (uint32_t)RIFT_THREAD_ROWS);
        check("and is read at the end, where the reader was",
              find_text(thread_pane(), "history 200") &&
                  within(find_text(thread_pane(), "history 200"), scroll));
        lv_obj_scroll_to_y(scroll, 0, LV_ANIM_OFF);
        pump(60);
        live_dm(1201, "in", key_of(99, 0x50), 9001, "history 201");
        rift_app_refresh(app);
        pump(80);
        check("a reader scrolled back into the history is not pulled down by one",
              lv_obj_get_scroll_bottom(scroll) > 0 && lv_obj_get_scroll_y(scroll) < 200);
        lv_obj_scroll_to_y(scroll, LV_COORD_MAX, LV_ANIM_OFF);
        pump(60);
        printf("     portrait thread: %d messages whole on screen\n", rows_in_view(scroll));
        thread_share(scroll);
    }
    /* The same long thread turned: the landscape thread pane has 354 px
     * under the strip less its header and the command line, and DS §37.2
     * spends them on messages. Counted and photographed, so the density of
     * this shape is a number in the log and not an impression. */
    use_display(POS_ROTATION_270, PANEL_CORNER);
    pump(120);
    {
        lv_obj_t *newest = find_text(thread_pane(), "history 201");
        lv_obj_t *scroll = ancestor(newest, 4);
        int rows;

        check("turned, the long thread is read at its end", newest && within(newest, scroll));
        rows = rows_in_view(scroll);
        printf("     landscape thread: %d messages whole on screen\n", rows);
        /* The console shape (DS §37.2): no shell header, a data-row strip
         * with the way back in it, a narrow list, a one-line thread header,
         * the details pane closed, a short command line. The thread has
         * more than half the display, and still shows more than the eleven
         * messages the handoff's layout showed - fourteen, where it was
         * seventeen before a message's body went from body type to the
         * label type (the owner's call, 2026-10-03: what is read is
         * larger). */
        check("and shows at least fourteen one-line messages above the composer", rows >= 14);
        check("the thread is more than half the display", thread_share(scroll) > 50.0);
        check("its header is a header row, not a data row",
              lv_obj_get_height(kid(kid(thread_pane(), 0), 0)) == rift_header_row_h());
        check("the strip is the screen's top row in landscape (DS 51.3)",
              lv_obj_get_height(strip()) == RIFT_NAV_ROW_H_TOP);
        check("with the way back in it", app->back && visible(app->back) &&
                                             within(app->back, strip()));
        check("and the command line is one too", lv_obj_get_height(cmdline()) == RIFT_ROW_H);
        check("the details pane is closed", !app->details_open &&
                                                find_text(content(), "OF 67 SENT") == NULL);
        check("and the header says how to open it", find_text(content(), "DETAILS") != NULL);
        check("nothing has moved off the body", inside_body(content()) && inside_body(strip()) &&
                                                    inside_body(cmdline()));
        shot("landscape-comms-long");
        {
            int home_before = home_calls;

            tap(app->back);
            check("the back slab goes home", home_calls == home_before + 1);
            /* A reader's press, not the app's doing: the count the final
             * check reads is of the app sending the shell home on its own. */
            home_calls = home_before;
        }
    }
    use_display(POS_ROTATION_0, PANEL_CORNER);
    pump(120);

    /* A burst: a hundred messages in one pass at the socket. */
    {
        unsigned dup = app->model.msgs_duplicate;
        int plays;

        pump(RIFT_NOTIFY_GAP_MS + 200);
        plays = fake_plays;
        for (i = 0; i < 100; i++) {
            live_dm(2000 + i, "in", key_of(i % 10, 0x40), 20000 + i, "burst");
        }
        rift_app_refresh(app);
        pump(120);
        check("a hundred arrivals are a hundred messages, none twice",
              app->model.msgs_duplicate == dup);
        check("and one sound", fake_plays == plays + 1);
        check("and the screen is still inside the body", inside_body(content()));
    }
    app_stop();
}

/* ==== feat/rift-management ===================================================
 *
 * Each session opens an app of its own, so what it checks does not depend on
 * what the long walk above left on screen. */

/* a starts before b in reading order: wholly above it, or on the same line
 * and to its left. */
static int reads_before(lv_obj_t *a, lv_obj_t *b)
{
    lv_area_t x;
    lv_area_t y;

    if (!a || !b) {
        return 0;
    }
    lv_obj_update_layout(a);
    lv_obj_get_coords(a, &x);
    lv_obj_get_coords(b, &y);
    if (x.y2 <= y.y1) {
        return 1;
    }
    return x.y1 < y.y2 && x.x2 <= y.x1;
}

/* How many visible labels under obj say exactly this. */
static int count_exact(lv_obj_t *obj, const char *text)
{
    uint32_t i;
    int n = 0;

    if (!obj || !visible(obj)) {
        return 0;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && strcmp(t, text) == 0) {
            n++;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        n += count_exact(lv_obj_get_child(obj, (int32_t)i), text);
    }
    return n;
}

/* One message on the SITE channel. sender NULL leaves sender_name out, as the
 * service does when the payload carries no "<name>: " it could parse. */
static void give_site_line(int id, const char *dir, const char *sender, const char *body)
{
    char json[1024];
    char who[96] = "";
    char text[512];
    cJSON *o;

    if (sender) {
        snprintf(who, sizeof(who), "\"sender_name\":\"%s\",", sender);
        snprintf(text, sizeof(text), "%s: %s", sender, body);
    } else {
        snprintf(text, sizeof(text), "%s", body);
    }
    snprintf(json, sizeof(json),
             "{\"message\":{\"id\":%d,\"direction\":\"%s\",\"kind\":\"channel\","
             "\"channel\":0,\"channel_name\":\"SITE\",\"channel_hash\":\"8c\",%s"
             "\"text\":\"%s\",\"state\":\"%s\",\"ack_expected\":false,\"mono_ms\":%lld}}",
             id, dir, who, text, strcmp(dir, "out") == 0 ? "sent_flood" : "received",
             (long long)(rift_mono_ms() - 10000));
    o = cJSON_Parse(json);
    check("a channel line fixture is valid JSON", o != NULL);
    rift_model_apply_event(&app->model, "mesh.message", o);
    cJSON_Delete(o);
}

#define LONG_SENDER "S\xC3\xB8rlandet fjellstasjon relay"
#define LONG_BODY "Stromen er tilbake p\xC3\xA5 hytta, veien er br\xC3\xB8ytet opp til " \
                  "demningen og radioen p\xC3\xA5 toppen svarer igjen etter natten uten nett"

/* COMMS: on a channel, who spoke comes before what they said. */
static void sender_session(void)
{
    int pass;

    app_start();
    quiet_client();
    give_service();
    give_channels();
    give_site_line(40, "in", "HYTTA", "kort");
    give_site_line(41, "in", LONG_SENDER, LONG_BODY);
    give_site_line(42, "in", NULL, "ingen navn her");
    give_site_line(43, "out", "K230-A", "mitt svar");
    rift_app_open_conversation(app, site_key());
    pump(120);

    for (pass = 0; pass < 2; pass++) {
        const char *where = pass ? "turned: " : "";
        char what[160];
        lv_obj_t *who = find_exact(thread_pane(), "HYTTA?");
        lv_obj_t *body = find_exact(thread_pane(), "kort");
        lv_obj_t *long_who = find_exact(thread_pane(), LONG_SENDER "?");
        lv_obj_t *long_body = find_exact(thread_pane(), LONG_BODY);
        lv_obj_t *anon = find_exact(thread_pane(), "UNNAMED");
        lv_obj_t *anon_body = find_exact(thread_pane(), "ingen navn her");

        snprintf(what, sizeof(what), "%sa channel sender is drawn before the message", where);
        check(what, who && body && reads_before(who, body));
        snprintf(what, sizeof(what), "%sin the same message as it, not a line of its own", where);
        check(what, who && body && lv_obj_get_parent(who) == lv_obj_get_parent(body) &&
                        lv_obj_get_index(who) < lv_obj_get_index(body));
        snprintf(what, sizeof(what), "%sand the caption still comes after the body", where);
        check(what, body && lv_obj_get_index(body) <
                                lv_obj_get_index(lv_obj_get_child(lv_obj_get_parent(body), -1)));
        snprintf(what, sizeof(what), "%sa long name over a long message: the name, then the body under it",
                 where);
        check(what, long_who && long_body && reads_before(long_who, long_body));
        snprintf(what, sizeof(what), "%sneither is cut off the pane", where);
        check(what, long_who && long_body && inside_body(long_who) &&
                        lv_obj_get_width(long_body) <= lv_obj_get_width(thread_pane()));
        snprintf(what, sizeof(what), "%sa line with no claimed name says so, first", where);
        check(what, anon && anon_body && reads_before(anon, anon_body));
        snprintf(what, sizeof(what), "%sour own line names nobody: the rule's side says whose",
                 where);
        check(what, find_exact(thread_pane(), "K230-A?") == NULL &&
                        find_exact(thread_pane(), "mitt svar") != NULL);
        snprintf(what, sizeof(what), "%sand the stored text is untouched", where);
        check(what, strstr(app->model.msg[app->model.msg_count - 1].text, "K230-A: mitt svar") != NULL);
        if (pass == 0) {
            shot("portrait-comms-channel-senders");
            use_display(POS_ROTATION_270, PANEL_CORNER);
            pump(160);
        } else {
            shot("landscape-comms-channel-senders");
        }
    }
    /* A direct thread has no sender line at all: two parties, and the
     * header and the rule already say which. */
    give_messages();
    rift_app_open_conversation(app, KEY_B);
    pump(120);
    check("a direct thread names no sender before its lines",
          find_exact(thread_pane(), "HYTTA?") == NULL &&
              find_text(thread_pane(), "tilbake p\xC3\xA5 hytta") != NULL);
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_stop();
}

/* A node row, given whole: for the zero-hop fixtures. */
static void give_node_json(const char *json)
{
    cJSON *o = cJSON_Parse(json);

    check("a node fixture is valid JSON", o != NULL);
    rift_model_apply_event(&app->model, "mesh.node", o);
    cJSON_Delete(o);
}

#define KEY_R1 "71aa000000000000000000000000000000000000000000000000000000000071"
#define KEY_R2 "72bb000000000000000000000000000000000000000000000000000000000072"
#define KEY_R3 "73cc000000000000000000000000000000000000000000000000000000000073"
#define KEY_CH "74dd000000000000000000000000000000000000000000000000000000000074"

static void type_into(lv_obj_t *field, const char *text)
{
    pos_input_focus(field);
    pump(40);
    while (*text) {
        pos_input_push_key((uint32_t)(unsigned char)*text++);
        pump(30);
    }
    pump(120);
}

/* NODES: finding a node, and the repeaters heard zero-hop. */
static void find_session(void)
{
    lv_obj_t *field;
    char json[1024];
    int64_t now;

    app_start();
    quiet_client();
    give_service();
    give_nodes();
    rift_app_show_section(app, RIFT_SEC_NODES);
    pump(120);
    field = rift_find_field(app);
    check("NODES has a search field", field && visible(field));
    check("which is empty, and the whole list shows",
          field && lv_textarea_get_text(field)[0] == '\0' && find_text(content(), "OSLO-01") &&
              find_text(content(), "HYTTA") && find_text(content(), "never-heard"));
    check("with no CLEAR while there is nothing to clear", find_exact(content(), "CLEAR") == NULL);

    type_into(field, "oslo");
    check("typing narrows the list as it goes, whatever the case",
          find_exact(content(), "OSLO-01") != NULL && find_text(content(), "HYTTA") == NULL &&
              find_text(content(), "never-heard") == NULL);
    check("and says how many of how many match", find_text(content(), "1 of 5 match") != NULL);
    check("CLEAR is there now", find_exact(content(), "CLEAR") != NULL);
    check("the match is listed once", count_exact(content(), "OSLO-01") == 1);

    rift_find_set_query(app, "  S\xC3\x98RLANDET ");
    pump(120);
    check("setting the query fills the field with all of it",
          strcmp(lv_textarea_get_text(field), "  S\xC3\x98RLANDET ") == 0);
    check("capital \xC3\x98 finds a small \xC3\xB8, and spaces around a query do not count",
          find_text(content(), "S\xC3\xB8rlandet") != NULL && find_exact(content(), "OSLO-01") == NULL);
    rift_find_set_query(app, "b2");
    pump(120);
    check("two hex characters find a node by its hash",
          find_exact(content(), "HYTTA") != NULL && find_exact(content(), "OSLO-01") == NULL);
    rift_find_set_query(app, "B2CAFE1E");
    pump(120);
    check("and more of its key, in capitals", find_exact(content(), "HYTTA") != NULL);
    rift_find_set_query(app, "zzz");
    pump(120);
    check("nothing matching is said, and no row is left over",
          find_text(content(), "No node matches \"zzz\"") != NULL &&
              find_text(content(), "OSLO-01") == NULL && find_text(content(), "HYTTA") == NULL);
    shot("portrait-nodes-find-none");
    tap(action_of(find_exact(content(), "CLEAR")));
    pump(150);
    check("CLEAR gives the whole list back",
          app->node_query[0] == '\0' && lv_textarea_get_text(field)[0] == '\0' &&
              find_text(content(), "OSLO-01") && find_text(content(), "HYTTA") &&
              find_text(content(), "never-heard"));
    check("and goes away again", find_exact(content(), "CLEAR") == NULL);
    type_into(field, "hy");
    pos_input_push_key(LV_KEY_ESC);
    pump(120);
    check("Esc in the field clears what was typed",
          app->node_query[0] == '\0' && find_text(content(), "OSLO-01") != NULL);
    check("searching changed nothing the cache holds",
          app->model.node_count == 5 && rift_model_find(&app->model, KEY_A) &&
              strcmp(rift_model_find(&app->model, KEY_A)->name, "OSLO-01") == 0);

    /* ---- ZERO-HOP ------------------------------------------------------- */
    now = rift_mono_ms();
    snprintf(json, sizeof(json),
             "{\"reason\":\"discovered\",\"node\":{\"public_key\":\"" KEY_R1 "\",\"name\":\"RPT-NORD\","
             "\"type\":2,\"path_known\":false,\"last_heard_mono_ms\":%lld,\"advert_hops\":0,"
             "\"advert_mono_ms\":%lld,\"last_rssi_dbm\":-61.0}}",
             (long long)(now - 20000), (long long)(now - 20000));
    give_node_json(json);
    snprintf(json, sizeof(json),
             "{\"reason\":\"discovered\",\"node\":{\"public_key\":\"" KEY_R2 "\",\"name\":\"RPT-SYD\","
             "\"type\":2,\"path_known\":false,\"last_heard_mono_ms\":%lld,\"advert_hops\":3,"
             "\"advert_mono_ms\":%lld}}",
             (long long)(now - 30000), (long long)(now - 30000));
    give_node_json(json);
    snprintf(json, sizeof(json),
             "{\"reason\":\"path\",\"node\":{\"public_key\":\"" KEY_R3 "\",\"name\":\"RPT-VEST\","
             "\"type\":2,\"path_known\":true,\"hops\":0,\"direct\":true,"
             "\"last_heard_mono_ms\":%lld}}",
             (long long)(now - 40000));
    give_node_json(json);
    snprintf(json, sizeof(json),
             "{\"reason\":\"discovered\",\"node\":{\"public_key\":\"" KEY_CH "\",\"name\":\"CHAT-NEAR\","
             "\"type\":1,\"path_known\":false,\"last_heard_mono_ms\":%lld,\"advert_hops\":0,"
             "\"advert_mono_ms\":%lld}}",
             (long long)(now - 50000), (long long)(now - 50000));
    give_node_json(json);
    rift_app_refresh(app);
    pump(80);
    tap(action_of(find_exact(content(), "ZERO-HOP")));
    pump(150); /* the list follows on the app's next timer pass */
    check("ZERO-HOP turns the view on", app->node_zero_hop == 1);
    check("and shows the repeaters heard with no relay between",
          find_exact(content(), "RPT-NORD") != NULL && find_exact(content(), "RPT-VEST") != NULL);
    check("not a repeater three relays out", find_exact(content(), "RPT-SYD") == NULL);
    check("nor one whose route has relays on it", find_exact(content(), "HYTTA") == NULL);
    check("nor a chat node heard straight: it is not a repeater",
          find_exact(content(), "CHAT-NEAR") == NULL && find_exact(content(), "OSLO-01") == NULL);
    check("the groups say which view this is", find_text(content(), "ZERO-HOP RPT") != NULL);
    check("and the footer what it is built on",
          find_text(content(), "no relay in between") != NULL);
    shot("portrait-nodes-zero-hop");
    rift_find_set_query(app, "vest");
    pump(120);
    check("search works inside the zero-hop view",
          find_exact(content(), "RPT-VEST") != NULL && find_exact(content(), "RPT-NORD") == NULL);
    rift_find_set_query(app, "");
    rift_model_drop_node(&app->model, KEY_R1);
    rift_model_drop_node(&app->model, KEY_R3);
    rift_app_refresh(app);
    pump(120);
    check("an empty zero-hop view says so, and that nothing is sent to look",
          find_text(content(), "No repeater heard zero-hop") != NULL &&
              find_text(content(), "nothing is sent") != NULL);
    tap(action_of(find_exact(content(), "ZERO-HOP")));
    pump(150);
    check("and off again shows every node", app->node_zero_hop == 0 &&
                                               find_exact(content(), "RPT-SYD") != NULL &&
                                               find_exact(content(), "OSLO-01") != NULL);

    use_display(POS_ROTATION_270, PANEL_CORNER);
    pump(160);
    check("turned, the search bar is a data row",
          rift_find_field(app) && visible(rift_find_field(app)) &&
              lv_obj_get_height(action_of(find_exact(content(), "ZERO-HOP"))) == RIFT_ROW_H);
    rift_find_set_query(app, "oslo");
    pump(120);
    check("and still narrows the list", find_exact(content(), "OSLO-01") != NULL &&
                                             find_text(content(), "HYTTA") == NULL);
    check("every word inside its button", labels_overflowing(content()) == 0);
    shot("landscape-nodes-find");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_stop();
}

/* ---- managing the node, with no service: what is checked before asking -- */

/* A field's text, set the way typing would leave it. */
static void field_set(lv_obj_t *field, const char *text)
{
    check("the field is there", field != NULL);
    if (field) {
        lv_textarea_set_text(field, text);
    }
    pump(40);
}

static void manage_session(void)
{
    lv_obj_t *add;
    lv_obj_t *join;

    app_start();
    quiet_client();
    give_service();
    give_channels();
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(120);

    /* Found on unit B: ACTIVITY opened scrolled down. A field in a closed
     * form was the first object the focus group got, LVGL focused it, and
     * focusing scrolls. Closed forms keep their fields out of the group. */
    check("closed forms keep their fields out of the focus group",
          lv_obj_get_group(rift_manage_name_field(app)) == NULL &&
              lv_obj_get_group(rift_manage_key_field(app)) == NULL &&
              lv_obj_get_group(rift_device_rename_field(app)) == NULL);
    check("so the keys go to the app's key sink, and SYSTEM is read from its top",
          pos_input_focused() == app->keysink && lv_obj_get_scroll_y(app->system_root) == 0);
    check("SYSTEM lists the channels the service holds",
          find_exact(content(), "SITE") != NULL && find_exact(content(), "OPS") != NULL);
    check("with their slot, hash and key size",
          find_text(content(), "SLOT 2 \xC2\xB7 HASH 4d \xC2\xB7 128-BIT") != NULL);
    check("and what every channel is: an unscoped flood",
          find_text(content(), "2 OF 8 SLOTS \xC2\xB7 UNSCOPED FLOOD") != NULL);
    check("each row can be left", count_exact(content(), "LEAVE") == 2);
    shot("portrait-activity-channels");

    /* LEAVE asks first; Cancel changes nothing. */
    tap(action_of(find_exact(content(), "LEAVE")));
    pump(150);
    check("LEAVE asks first, naming the channel", find_text(content(), "Leave SITE?") != NULL);
    check("and says what it costs", find_text(content(), "Nothing on the air gives it back") != NULL);
    check("nothing is asked yet", app->model.manage_op.kind == RIFT_ACTION_NONE);
    shot("portrait-activity-leave-confirm");
    tap(action_of(find_exact(content(), "CANCEL")));
    pump(150);
    check("Cancel puts it away and asks nothing",
          find_text(content(), "Leave SITE?") == NULL &&
              app->model.manage_op.kind == RIFT_ACTION_NONE);

    tap(action_of(find_exact(content(), "LEAVE")));
    pump(150);
    rift_app_show_section(app, RIFT_SEC_NODES);
    pump(60);
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(150);
    check("leaving SYSTEM is Cancel for a LEAVE left up",
          find_text(content(), "Leave SITE?") == NULL);

    tap(action_of(find_exact(content(), "LEAVE")));
    pump(150);
    {
        /* The confirmation's LEAVE is the one after its CANCEL; the rows'
         * own LEAVEs are disabled while it is up. */
        lv_obj_t *confirm_cancel = action_of(find_exact(content(), "CANCEL"));
        lv_obj_t *bar = confirm_cancel ? lv_obj_get_parent(confirm_cancel) : NULL;

        tap(kid(bar, 1));
    }
    pump(150);
    check("the confirmation's LEAVE asks to leave slot 0, by name",
          app->model.manage_op.kind == RIFT_ACTION_CHANNEL_REMOVE &&
              app->model.manage_op.value == 0 && strcmp(app->model.manage_op.label, "SITE") == 0);
    check("and with no service says nothing was changed",
          app->model.manage_op.failed &&
              strstr(app->model.manage_op.error, "nothing was changed") != NULL);
    check("the channel is still listed: only the service's answer removes it",
          find_exact(content(), "SITE") != NULL);

    /* ADD CHANNEL: a form in place, checked before anything is asked. */
    add = action_of(find_exact(content(), "ADD CHANNEL"));
    tap(add);
    pump(150);
    check("ADD CHANNEL opens a form in place, a hashtag by default",
          rift_manage_name_field(app) && visible(rift_manage_name_field(app)) &&
              find_text(content(), "A public topic") != NULL);
    check("with no key field for a hashtag", !visible(rift_manage_key_field(app)));
    check("its name field joins the focus group while the form is open, the key's does not",
          lv_obj_get_group(rift_manage_name_field(app)) != NULL &&
              lv_obj_get_group(rift_manage_key_field(app)) == NULL);
    join = action_of(find_exact(content(), "JOIN"));
    tap(join);
    pump(150);
    check("an empty name is said, not sent",
          find_text(content(), "needs a name after the #") != NULL &&
              app->model.manage_op.kind == RIFT_ACTION_CHANNEL_REMOVE);
    field_set(rift_manage_name_field(app), "abcdefghijabcdefghijabcdefghijk");
    tap(join);
    pump(150);
    check("a hashtag name that only fits without its # is refused, not cut",
          find_text(content(), "at most 31 bytes") != NULL);
    field_set(rift_manage_name_field(app), "  oslo ");
    tap(join);
    pump(150);
    check("a hashtag is asked for under its canonical name",
          app->model.manage_op.kind == RIFT_ACTION_CHANNEL_ADD &&
              strcmp(app->model.manage_op.label, "#oslo") == 0);
    check("and with no service the form says nothing was changed, and stays",
          find_text(content(), "nothing was changed") != NULL &&
              visible(rift_manage_name_field(app)));

    tap(action_of(find_exact(content(), "KEY")));
    pump(150);
    check("KEY shows the key field", visible(rift_manage_key_field(app)) &&
                                         find_text(content(), "somebody shared") != NULL);
    field_set(rift_manage_name_field(app), "Felles");
    field_set(rift_manage_key_field(app), "not a key!");
    tap(join);
    pump(150);
    check("a key that is not base64 is said, before anything is asked",
          find_text(content(), "not a base64 key") != NULL &&
              strcmp(app->model.manage_op.label, "#oslo") == 0);
    field_set(rift_manage_key_field(app), "AAECAwQFBgcICQoL");
    tap(join);
    pump(150);
    check("and one of the wrong length", find_text(content(), "16 or 32 bytes") != NULL);
    field_set(rift_manage_name_field(app), "SITE");
    field_set(rift_manage_key_field(app), "izOH6cXN6mrJ5e26oRXNcg==");
    tap(join);
    pump(150);
    check("a name already joined is said", find_text(content(), "called SITE is joined") != NULL);
    tap(action_of(find_exact(content(), "PRIVATE")));
    pump(150);
    check("PRIVATE needs no key: it makes one", !visible(rift_manage_key_field(app)) &&
                                                   find_text(content(), "new random key") != NULL);
    shot("portrait-activity-add-channel");
    rift_app_show_section(app, RIFT_SEC_NODES);
    pump(60);
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(150);
    check("leaving SYSTEM closes the form and empties its fields",
          !visible(rift_manage_name_field(app)) &&
              lv_textarea_get_text(rift_manage_key_field(app))[0] == '\0' &&
              lv_textarea_get_text(rift_manage_name_field(app))[0] == '\0');

    /* THIS DEVICE: the name, and the path hash size. */
    check("DEVICE has RENAME", find_exact(content(), "RENAME") != NULL);
    check("and the path hash choices, disabled until the service says the size",
          find_exact(content(), "2 B") != NULL &&
              lv_obj_has_state(action_of(find_exact(content(), "2 B")), LV_STATE_DISABLED));
    tap(action_of(find_exact(content(), "RENAME")));
    pump(150);
    check("RENAME opens the name in place, filled with the current one",
          rift_device_rename_field(app) && visible(rift_device_rename_field(app)) &&
              strcmp(lv_textarea_get_text(rift_device_rename_field(app)), "K230-A") == 0);
    field_set(rift_device_rename_field(app), "   ");
    tap(action_of(find_exact(content(), "SAVE")));
    pump(150);
    check("a name of spaces is said, not sent",
          find_text(content(), "not only spaces") != NULL &&
              app->model.manage_op.kind != RIFT_ACTION_RENAME);
    field_set(rift_device_rename_field(app), "Ny K230");
    tap(action_of(find_exact(content(), "SAVE")));
    pump(150);
    check("a good name is asked for", app->model.manage_op.kind == RIFT_ACTION_RENAME &&
                                          strcmp(app->model.manage_op.label, "Ny K230") == 0);
    {
        cJSON *o = cJSON_Parse("{\"public_key\":\"5f0000000000000000000000000000000000000000000000"
                               "00000000000000ff\",\"name\":\"K230-A\",\"name_source\":\"config\"}");

        rift_model_apply_identity(&app->model, o);
        cJSON_Delete(o);
    }
    rift_app_refresh(app);
    pump(150);
    /* A configured name is this node's to change like any other: the form
     * that was open stays open, and nothing sends the reader elsewhere. */
    check("a name set by the configuration is renamed here too: the form stays",
          visible(rift_device_rename_field(app)) &&
              find_text(content(), "MESHCORED_NAME") == NULL);
    tap(action_of(find_exact(content(), "CANCEL")));
    pump(150);
    check("and with the form closed RENAME is there to press",
          !visible(rift_device_rename_field(app)) &&
              !lv_obj_has_state(action_of(find_exact(content(), "RENAME")), LV_STATE_DISABLED));
    /* The service took a rename and said it could not write it. */
    check("no unsaved warning before there is one",
          find_text(content(), "could not save it") == NULL ||
              !visible(find_text(content(), "could not save it")));
    memset(&app->model.manage_op, 0, sizeof(app->model.manage_op));
    app->model.manage_op.kind = RIFT_ACTION_RENAME;
    app->model.manage_op.done = 1;
    app->model.manage_op.unsaved = 1;
    app->model.manage_op.have_mono = 1;
    app->model.manage_op.mono_ms = rift_mono_ms();
    rift_app_refresh(app);
    pump(150);
    check("a rename the service could not save is not shown as saved: a warning says so",
          find_text(content(), "could not save it") != NULL &&
              visible(find_text(content(), "could not save it")) &&
              find_text(content(), "old name returns") != NULL);
    check("and the caption says NOT SAVED rather than only RENAMED",
          find_text(content(), "NOT SAVED") != NULL &&
              find_text(content(), "AFTER YOUR NEXT ADVERT") == NULL);
    check("with RENAME still there to try again",
          !lv_obj_has_state(action_of(find_exact(content(), "RENAME")), LV_STATE_DISABLED));
    /* Refused: said under the name, in the service's words, not only in
     * the status line at the foot of the panel. */
    rift_model_action_failed(&app->model, RIFT_ACTION_RENAME,
                             "the name is set by meshcored's configuration", rift_mono_ms());
    rift_app_refresh(app);
    pump(150);
    {
        lv_obj_t *said = find_text(content(), "Not renamed - the name is still K230-A");
        lv_obj_t *button = action_of(find_exact(content(), "RENAME"));
        lv_area_t s = { 0 };
        lv_area_t r = { 0 };

        if (said && button) {
            lv_obj_get_coords(said, &s);
            lv_obj_get_coords(button, &r);
        }
        check("a refused rename is said under the name, with the service's reason",
              said && visible(said) &&
                  strstr(lv_label_get_text(said), "meshcored's configuration") != NULL);
        check("right under the Name row, not at the foot of the panel",
              said && button && s.y1 >= r.y2 && s.y1 - r.y2 < 160);
        check("and does not read as unsaved", find_text(content(), "could not save it") == NULL ||
                                                  !visible(find_text(content(), "could not save it")));
    }
    app->model.manage_op.unknown = 1;
    rift_app_refresh(app);
    pump(150);
    check("a rename nobody answered says that the name may or may not have changed",
          find_text(content(), "may or may not have changed") != NULL &&
              visible(find_text(content(), "may or may not have changed")));
    memset(&app->model.manage_op, 0, sizeof(app->model.manage_op));
    app->model.manage_op.kind = RIFT_ACTION_RENAME;
    app->model.manage_op.done = 1;
    app->model.manage_op.have_mono = 1;
    app->model.manage_op.mono_ms = rift_mono_ms();
    rift_app_refresh(app);
    pump(150);
    check("a saved rename carries no warning",
          (find_text(content(), "could not save it") == NULL ||
           !visible(find_text(content(), "could not save it"))) &&
              find_text(content(), "Not renamed") == NULL &&
              find_text(content(), "AFTER YOUR NEXT ADVERT") != NULL);
    {
        cJSON *o = cJSON_Parse("{\"bytes\":1,\"allowed\":[1,2,3]}");

        rift_model_apply_path_hash(&app->model, o);
        cJSON_Delete(o);
        rift_model_action_clear(&app->model, RIFT_ACTION_RENAME);
    }
    rift_app_refresh(app);
    pump(150);
    check("with the size known, the choices are there and 1 B is the chosen one",
          !lv_obj_has_state(action_of(find_exact(content(), "2 B")), LV_STATE_DISABLED) &&
              find_text(content(), "1 BYTE PER RELAY") != NULL);
    tap(action_of(find_exact(content(), "3 B")));
    pump(150);
    check("a move off 1 byte asks first, and says what it costs",
          find_text(content(), "Use 3-byte path hashes?") != NULL &&
              find_text(content(), "drop such floods") != NULL &&
              app->model.manage_op.kind != RIFT_ACTION_PATH_HASH);
    shot("portrait-activity-path-hash-confirm");
    tap(action_of(find_exact(content(), "CANCEL")));
    pump(150);
    check("Cancel asks nothing", find_text(content(), "Use 3-byte") == NULL &&
                                     app->model.manage_op.kind != RIFT_ACTION_PATH_HASH);
    tap(action_of(find_exact(content(), "2 B")));
    pump(150);
    tap(action_of(find_exact(content(), "USE IT")));
    pump(150);
    check("the confirmation asks for that size",
          app->model.manage_op.kind == RIFT_ACTION_PATH_HASH && app->model.manage_op.value == 2);

    use_display(POS_ROTATION_270, PANEL_CORNER);
    pump(160);
    check("turned, the panels are still there, every word inside its button",
          find_exact(content(), "ADD CHANNEL") != NULL && labels_overflowing(content()) == 0);
    shot("landscape-activity-manage");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_stop();
}

/* ---- managing the node, end to end against the scripted service ---------- */

/* Does this block of memory hold these bytes anywhere? */
static int mem_holds(const void *block, size_t len, const char *what)
{
    size_t n = strlen(what);
    const unsigned char *b = block;
    size_t i;

    for (i = 0; n && i + n <= len; i++) {
        if (memcmp(b + i, what, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static int live_until(int (*cond)(void), int ms)
{
    int t;

    for (t = 0; t < ms; t += 20) {
        if (cond()) {
            return 1;
        }
        pump(20);
        usleep(20000);
    }
    return cond();
}

static int live_ready(void)
{
    return app && app->model.channels_valid && app->model.have_path_hash &&
           app->model.have_identity;
}

static int live_settled(void)
{
    return app && !app->model.manage_op.active;
}

static int live_two_channels(void)
{
    return app && app->model.channel_count == 2;
}

static int live_one_channel(void)
{
    return app && app->model.channel_count == 1;
}

static pid_t live_spawn(const char *bin, const char *manage_log)
{
    pid_t pid = fork();

    if (pid == 0) {
        setenv("FAKE_MESHCORED_STATE", "online", 1);
        setenv("FAKE_MESHCORED_REASON", "receiving", 1);
        setenv("FAKE_MESHCORED_NODES", "[]", 1);
        setenv("FAKE_MESHCORED_CHANNELS",
               "[{\"channel\":0,\"name\":\"SITE\",\"channel_hash\":\"8c\",\"key_bits\":128,"
               "\"text_limit\":147,\"ack_expected\":false}]",
               1);
        setenv("FAKE_MESHCORED_MANAGE", manage_log, 1);
        setenv("FAKE_MESHCORED_LIFE_MS", "120000", 1);
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* ---- feat/rift-comms-usability ---------------------------------------------
 *
 * COMMS' rows at a readable size, the Public channel first, and the composer
 * taking the keys when a conversation opens - in both shapes, and the rows
 * at every text size. */

static int32_t top_of(lv_obj_t *obj)
{
    lv_area_t a;

    if (!obj) {
        return -1;
    }
    lv_obj_get_coords(obj, &a);
    return a.y1;
}

static void give_public_channels(void)
{
    cJSON *o;

    /* The service's answer: slot 1 holds MeshCore's well-known Public key
     * under a local name; slot 3 is merely called Public. */
    o = cJSON_Parse("{\"count\":4,\"max\":8,\"persistent\":true,\"channels\":["
                    "{\"channel\":0,\"name\":\"SITE\",\"channel_hash\":\"8c\","
                    "\"key_bits\":256,\"text_limit\":147,\"ack_expected\":false},"
                    "{\"channel\":1,\"name\":\"torget\",\"channel_hash\":\"11\","
                    "\"key_bits\":128,\"well_known\":\"public\",\"text_limit\":147,"
                    "\"ack_expected\":false},"
                    "{\"channel\":2,\"name\":\"OPS\",\"channel_hash\":\"4d\","
                    "\"key_bits\":128,\"text_limit\":147,\"ack_expected\":false},"
                    "{\"channel\":3,\"name\":\"Public\",\"channel_hash\":\"11\","
                    "\"key_bits\":128,\"text_limit\":147,\"ack_expected\":false}]}");
    check("the Public channel fixture is taken",
          o != NULL && rift_model_apply_channels(&app->model, o) == 0);
    cJSON_Delete(o);
    rift_app_refresh(app);
    pump(60);
}

/* ---- Public, muted ------------------------------------------------------ */

/* MeshCore's Public channel is identified by the service from its key
 * (well_known), never by its name: muting it keeps it the one Public row,
 * first in COMMS, the same channel in SYSTEM > CHANNELS, and silent. */
static void public_mute_session(void)
{
    char pub[RIFT_KEY_HEX];
    char decoy[RIFT_KEY_HEX];
    lv_obj_t *row;
    cJSON *o;
    int plays;

    rift_channel_key(1, "11", "torget", pub, sizeof(pub));
    rift_channel_key(3, "11", "Public", decoy, sizeof(decoy));
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    quiet_client();
    give_nodes();
    give_service();
    give_messages();
    give_public_channels();
    rift_app_set_dm_sound(app, 1);
    rift_app_set_channel_sound(app, 1);
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(150);
    check("SYSTEM lists the Public channel once, by the service's key",
          count_exact(content(), "torget") == 1 && count_exact(content(), "Public") == 1);
    row = ancestor(find_exact(content(), "torget"), 2);
    /* Public is mandatory in Doors: no LEAVE on its row, MUTE kept; every
     * other channel keeps its LEAVE. */
    check("the Public row has MUTE and no LEAVE, and says it is the standard channel",
          row && visible(kid(row, 1)) && !visible(kid(row, 2)) &&
              find_text(content(), "STANDARD \xC2\xB7 SLOT 1") != NULL);
    check("the other three channels can still be left", count_exact(content(), "LEAVE") == 3);
    tap(row ? kid(row, 1) : NULL);
    pump(150);
    check("MUTE on Public mutes the Public channel and nothing else",
          app->prefs.mute_count == 1 && rift_app_channel_muted(app, pub) &&
              !rift_app_channel_muted(app, decoy));
    check("it is still there, once, with the same identity",
          count_exact(content(), "torget") == 1 && rift_model_key_channel(&app->model, pub) &&
              rift_model_key_channel(&app->model, pub)->is_public);
    rift_app_show_section(app, RIFT_SEC_COMMS);
    pump(200);
    {
        /* As COMMS draws it: the Public row above every other row on screen. */
        static const char *const others[] = { "HYT", "OSL", "SIT", "OPS", "Pub" };
        int32_t top = top_of(find_exact(content(), "torget"));
        int above = 1;
        size_t k;

        for (k = 0; k < sizeof(others) / sizeof(others[0]); k++) {
            lv_obj_t *o = find_text(content(), others[k]);

            if (o && top_of(o) <= top) {
                above = 0;
            }
        }
        check("muted, Public is still the first row in COMMS",
              top >= 0 && above && find_text(content(), "Pub") != NULL);
    }
    check("COMMS shows the Public row once", count_exact(content(), "torget") == 1);
    pump(RIFT_NOTIFY_GAP_MS + 200);
    plays = fake_plays;
    o = cJSON_Parse("{\"message\":{\"id\":990,\"direction\":\"in\",\"kind\":\"channel\","
                    "\"channel\":1,\"channel_name\":\"torget\",\"channel_hash\":\"11\","
                    "\"sender_name\":\"Per\",\"text\":\"Per: hei\",\"state\":\"received\","
                    "\"timestamp\":7990}}");
    rift_model_apply_event(&app->model, "mesh.message", o);
    cJSON_Delete(o);
    pump(150);
    check("a message on muted Public makes no sound, and is unread",
          fake_plays == plays && rift_model_unread(&app->model, pub) >= 1);
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(150);
    row = ancestor(find_exact(content(), "torget"), 2);
    tap(row ? kid(row, 1) : NULL);
    pump(150);
    check("unmuted, the same Public channel, still once",
          app->prefs.mute_count == 0 && count_exact(content(), "torget") == 1);
    pump(RIFT_NOTIFY_GAP_MS + 200);
    plays = fake_plays;
    o = cJSON_Parse("{\"message\":{\"id\":991,\"direction\":\"in\",\"kind\":\"channel\","
                    "\"channel\":1,\"channel_name\":\"torget\",\"channel_hash\":\"11\","
                    "\"sender_name\":\"Per\",\"text\":\"Per: igjen\",\"state\":\"received\","
                    "\"timestamp\":7991}}");
    rift_model_apply_event(&app->model, "mesh.message", o);
    cJSON_Delete(o);
    pump(150);
    check("and a message on it makes the channel sound again",
          fake_plays == plays + 1 && fake_kind == RIFT_SOUND_CHANNEL);
    rift_app_set_dm_sound(app, 0);
    app_stop();
}

static void comms_usability_session(void)
{
    static const enum pos_text_size sizes[] = { POS_TEXT_SIZE_SMALL, POS_TEXT_SIZE_MEDIUM,
                                                POS_TEXT_SIZE_LARGE };
    static const enum pos_rotation turns[] = { POS_ROTATION_0, POS_ROTATION_270 };
    /* The start of each other row's name: at the larger sizes the list cuts
     * a name with an ellipsis, as it always has. */
    static const char *const others[] = { "HYT", "OSL", "SIT", "OPS", "Pub" };
    enum pos_text_size was = pos_theme_current_text_size();
    char pub[RIFT_KEY_HEX];
    size_t s;
    size_t t;
    size_t k;

    rift_channel_key(1, "11", "torget", pub, sizeof(pub));

    /* ---- the rows and the order, at every size, in both shapes ---------- */
    for (t = 0; t < sizeof(turns) / sizeof(turns[0]); t++) {
        for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            const char *size = pos_text_size_name(sizes[s]);
            const char *shape = turns[t] == POS_ROTATION_0 ? "portrait" : "landscape";
            char what[200];
            lv_obj_t *name;
            lv_obj_t *line;
            lv_obj_t *list;
            int32_t row_h = RIFT_ROW_H;
            int first = 1;
            int seen = 0;

            pos_theme_select_text_size(sizes[s]);
            use_display(turns[t], PANEL_CORNER);
            app_start();
            quiet_client();
            give_nodes();
            give_service();
            give_messages();
            give_public_channels();
            give_unread();
            rift_app_show_section(app, RIFT_SEC_COMMS);
            pump(200);

            name = find_exact(content(), "torget");
            line = ancestor(name, 1);
            list = ancestor(name, 3);
            snprintf(what, sizeof(what), "%s, %s: the Public channel has a row", size, shape);
            check(what, name != NULL && line != NULL && list != NULL);
            if (!name || !line || !list) {
                app_stop();
                continue;
            }
            /* Every other row that is on screen is under it. Landscape's
             * list is shorter than its six rows, so the last may not be
             * built; the newest talk, which would have headed the list, is. */
            for (k = 0; k < sizeof(others) / sizeof(others[0]); k++) {
                lv_obj_t *o = find_text(list, others[k]);

                if (o) {
                    seen++;
                    if (top_of(o) <= top_of(name)) {
                        first = 0;
                    }
                }
            }
            snprintf(what, sizeof(what),
                     "%s, %s: it is the first row - above newer talk, and above a channel only "
                     "called Public",
                     size, shape);
            check(what, first && seen >= 1 && top_of(line) - top_of(list) < row_h / 2);
            if (!app->wide) {
                snprintf(what, sizeof(what),
                         "%s, %s: and every other row is there under it, the namesake too",
                         size, shape);
                check(what, seen == (int)(sizeof(others) / sizeof(others[0])));
            }
            {
                /* One row for it, however the list is walked. */
                uint32_t n = lv_obj_get_child_count(list);
                uint32_t c;
                int rows = 0;

                for (c = 0; c < n; c++) {
                    lv_obj_t *slot = lv_obj_get_child(list, (int32_t)c);

                    if (!lv_obj_has_flag(slot, LV_OBJ_FLAG_HIDDEN) &&
                        find_exact(slot, "torget")) {
                        rows++;
                    }
                }
                snprintf(what, sizeof(what), "%s, %s: and there is one of it", size, shape);
                check(what, rows == 1);
            }

            /* The list's rows are what they were: the 36 px data row, the
             * name in the row-title type, the age and the route beside it. */
            snprintf(what, sizeof(what), "%s, %s: a list row is the 36 px data row it was",
                     size, shape);
            check(what, lv_obj_get_height(line) == RIFT_ROW_H &&
                            lv_font_get_line_height(
                                lv_obj_get_style_text_font(name, LV_PART_MAIN)) ==
                                pocketui_role_line_height(POS_STYLE_ROW_TITLE));
            snprintf(what, sizeof(what), "%s, %s: the unread count and the age are still there",
                     size, shape);
            check(what, rift_model_unread_total(&app->model) > 0 &&
                            find_exact(content(), "HEARD") != NULL);

            /* The opened conversation is what is larger: the body and the
             * claimed sender in the label type, the caption a caption. */
            give_channel_message();
            rift_app_open_conversation(app, site_key());
            pump(250);
            {
                lv_obj_t *body = find_text(thread_pane(), "tilbake");
                lv_obj_t *sender = find_exact(thread_pane(), "HYTTA?");
                lv_obj_t *column = ancestor(body, 1);
                lv_obj_t *caption =
                    column ? lv_obj_get_child(column, (int32_t)lv_obj_get_child_count(column) - 1)
                           : NULL;
                lv_obj_t *mslot = ancestor(body, 3);
                int32_t body_lh = body ? lv_font_get_line_height(
                                             lv_obj_get_style_text_font(body, LV_PART_MAIN))
                                       : 0;

                snprintf(what, sizeof(what), "%s, %s: a channel message is drawn in the thread",
                         size, shape);
                check(what, body && sender && caption && caption != body && mslot);
                if (body && sender && caption && mslot) {
                    lv_area_t a;
                    lv_area_t v;

                    printf("     %s, %s: body %d px type (was %d), sender %d (was %d), caption "
                           "%d; line %d px (was %d)\n",
                           size, shape, pos_type_current(POS_TYPE_LABEL).px,
                           pos_type_current(POS_TYPE_BODY).px, pos_type_current(POS_TYPE_LABEL).px,
                           pos_type_current(POS_TYPE_META).px, pos_type_current(POS_TYPE_META).px,
                           (int)body_lh,
                           (int)pocketui_role_line_height(POS_STYLE_TEXT_PRIMARY));
                    snprintf(what, sizeof(what),
                             "%s, %s: the message body is the label type, larger than body type",
                             size, shape);
                    check(what, body_lh == pocketui_role_line_height(POS_STYLE_ROW_TITLE) &&
                                    body_lh > pocketui_role_line_height(POS_STYLE_TEXT_PRIMARY));
                    snprintf(what, sizeof(what),
                             "%s, %s: the sender is the same size, larger than a caption", size,
                             shape);
                    check(what, lv_font_get_line_height(
                                    lv_obj_get_style_text_font(sender, LV_PART_MAIN)) == body_lh &&
                                    body_lh > pocketui_role_line_height(POS_STYLE_CAPTION));
                    snprintf(what, sizeof(what),
                             "%s, %s: the caption after them is still a caption, with its state",
                             size, shape);
                    check(what, lv_font_get_line_height(
                                    lv_obj_get_style_text_font(caption, LV_PART_MAIN)) ==
                                    pocketui_role_line_height(POS_STYLE_CAPTION) &&
                                    strstr(lv_label_get_text(caption), "RECEIVED") != NULL);
                    snprintf(what, sizeof(what), "%s, %s: messages are %d px apart", size, shape,
                             app->wide ? 2 : 8);
                    check(what, lv_obj_get_style_pad_bottom(mslot, LV_PART_MAIN) ==
                                    (app->wide ? 2 : 8));
                    lv_obj_get_coords(body, &a);
                    lv_obj_get_coords(thread_pane(), &v);
                    snprintf(what, sizeof(what),
                             "%s, %s: the body's line is whole and inside the thread", size, shape);
                    check(what, lv_obj_get_height(body) >= body_lh && a.y1 >= v.y1 &&
                                    a.y2 <= v.y2 && a.x1 >= v.x1 && a.x2 <= v.x2);
                    snprintf(what, sizeof(what),
                             "%s, %s: a short message is still one line: sender, body, caption",
                             size, shape);
                    check(what, app->wide ? lv_obj_get_height(column) <= body_lh + 2 : 1);
                }
            }
            /* A direct thread: the same type, and no sender label. */
            rift_app_open_conversation(app, KEY_B);
            pump(250);
            {
                lv_obj_t *body = find_exact(thread_pane(), "Fint, ser deg");

                snprintf(what, sizeof(what), "%s, %s: a direct message's body is the label type",
                         size, shape);
                check(what, body && lv_font_get_line_height(
                                        lv_obj_get_style_text_font(body, LV_PART_MAIN)) ==
                                        pocketui_role_line_height(POS_STYLE_ROW_TITLE));
                snprintf(what, sizeof(what), "%s, %s: its state is still said under it", size,
                         shape);
                check(what, find_text(thread_pane(), "DELIVERED") != NULL &&
                                find_text(thread_pane(), "NO ACK") != NULL);
            }
            /* The list's names are fitted on a repaint, after the open row
             * has taken its slab's inset. */
            rift_app_refresh(app);
            pump(120);
            snprintf(what, sizeof(what), "%s, %s: COMMS' captions are whole", size, shape);
            check(what, captions_clipped(frame()) == 0);
            snprintf(what, sizeof(what), "%s, %s: and COMMS stays inside the body", size, shape);
            check(what, inside_body(content()));
            if (sizes[s] == POS_TEXT_SIZE_SMALL) {
                shot(app->wide ? "landscape-thread-small" : "portrait-thread-small");
            } else if (sizes[s] == POS_TEXT_SIZE_LARGE) {
                shot(app->wide ? "landscape-thread-large" : "portrait-thread-large");
            }
            app_stop();
        }
    }
    pos_theme_select_text_size(was);

    /* ---- the composer takes the keys when a conversation opens ---------- */
    for (t = 0; t < sizeof(turns) / sizeof(turns[0]); t++) {
        const char *shape = turns[t] == POS_ROTATION_0 ? "portrait" : "landscape";
        char what[200];
        char opened[RIFT_KEY_HEX];
        lv_obj_t *field;
        lv_group_t *dialog;

        use_display(turns[t], PANEL_CORNER);
        app_start();
        quiet_client();
        give_nodes();
        give_service();
        give_messages();
        give_public_channels();
        rift_app_show_section(app, RIFT_SEC_COMMS);
        pump(200);
        snprintf(what, sizeof(what), "%s: with nothing open the list has the keys", shape);
        check(what, pos_input_focused() == app->keysink && !app->composer_focused);

        /* A direct conversation, by a tap on its row. */
        tap(ancestor(find_exact(content(), "HYTTA"), 1));
        pump(200);
        field = app->wide ? app->composer : rift_comms_field(app);
        snprintf(what, sizeof(what), "%s: a tap on a direct conversation opens it", shape);
        check(what, app->have_conv && strcmp(app->conv, KEY_B) == 0 && field != NULL);
        snprintf(what, sizeof(what), "%s: and its composer has the keys, with no tap on it",
                 shape);
        check(what, pos_input_focused() == field && app->composer_focused);
        pos_input_push_key('h');
        pos_input_push_key('i');
        pump(120);
        snprintf(what, sizeof(what), "%s: so what is typed lands in the message", shape);
        check(what, field && strcmp(lv_textarea_get_text(field), "hi") == 0);
        {
            int r;

            for (r = 0; r < 6; r++) {
                rift_app_refresh(app);
                pump(40);
            }
        }
        pos_input_push_key('!');
        pump(80);
        snprintf(what, sizeof(what), "%s: and keeps landing there across repaints", shape);
        check(what, field && strcmp(lv_textarea_get_text(field), "hi!") == 0 &&
                        pos_input_focused() == field);
        pos_input_push_key(LV_KEY_ESC);
        pump(160);
        snprintf(what, sizeof(what), "%s: Esc clears what was typed and stays", shape);
        check(what, field && lv_textarea_get_text(field)[0] == '\0' &&
                        pos_input_focused() == field && app->section == RIFT_SEC_COMMS);

        /* A channel, by a tap; then the list walked by key from the field. */
        tap(ancestor(find_exact(content(), "torget"), 1));
        pump(200);
        snprintf(what, sizeof(what), "%s: a channel opened by a tap gives its composer the keys",
                 shape);
        check(what, strcmp(app->conv, pub) == 0 && pos_input_focused() == field);
        pos_input_push_key('o');
        pos_input_push_key('k');
        pump(120);
        snprintf(what, sizeof(what), "%s: and typing goes to it", shape);
        check(what, strcmp(lv_textarea_get_text(field), "ok") == 0);
        lv_textarea_set_text(field, "");
        snprintf(opened, sizeof(opened), "%s", app->conv);
        pos_input_push_key(LV_KEY_DOWN);
        pump(200);
        snprintf(what, sizeof(what),
                 "%s: Down still steps to the next conversation, from the composer", shape);
        check(what, strcmp(app->conv, opened) != 0 && pos_input_focused() == field);
        pos_input_push_key(LV_KEY_UP);
        pump(200);
        snprintf(what, sizeof(what), "%s: and Up back to the first row, the Public channel",
                 shape);
        check(what, strcmp(app->conv, pub) == 0 && pos_input_focused() == field);

        /* Back and Esc are what they were. */
        pos_input_push_key(LV_KEY_ESC);
        pump(200);
        snprintf(what, sizeof(what),
                 "%s: Esc with nothing typed is the list's Esc - back to ACTIVITY", shape);
        check(what, app->section == RIFT_SEC_ACTIVITY && pos_input_focused() == app->keysink &&
                        !app->composer_focused);
        tap(tab(2));
        pump(200);
        snprintf(what, sizeof(what),
                 "%s: back on COMMS the open conversation's composer has the keys again", shape);
        check(what, app->section == RIFT_SEC_COMMS && pos_input_focused() == field);
        snprintf(what, sizeof(what), "%s: the Back action leaves COMMS for ACTIVITY as before",
                 shape);
        check(what, app_rift.back(app) == 1 && app->section == RIFT_SEC_ACTIVITY);
        pump(200);
        snprintf(what, sizeof(what), "%s: and the keys are the list's there", shape);
        check(what, pos_input_focused() == app->keysink);

        /* A form on SYSTEM keeps the keys it has: repaints and arriving
         * messages do not hand them to a composer. */
        rift_app_show_section(app, RIFT_SEC_SYSTEM);
        pump(150);
        tap(action_of(find_exact(content(), "RENAME")));
        pump(150);
        pos_input_focus(rift_device_rename_field(app));
        pump(80);
        live_dm(700 + (int)t, "in", KEY_B, 9000, "mens du skriver");
        {
            int r;

            for (r = 0; r < 6; r++) {
                rift_app_refresh(app);
                pump(40);
            }
        }
        snprintf(what, sizeof(what), "%s: a form's field keeps the focus through a message "
                                     "arriving", shape);
        check(what, pos_input_focused() == rift_device_rename_field(app) &&
                        !app->focus_composer_pending);
        tap(action_of(find_exact(content(), "CANCEL")));
        pump(150);

        /* A dialog that has taken the keys (the shell redirects the group to
         * it) keeps them when a conversation opens under it. */
        pos_input_focus(app->keysink);
        pump(80);
        dialog = lv_group_create();
        snprintf(what, sizeof(what), "%s: a dialog takes the keys", shape);
        check(what, pos_input_push_group(dialog));
        rift_app_open_conversation(app, KEY_A);
        pump(240);
        snprintf(what, sizeof(what), "%s: a conversation opened under it does not take them",
                 shape);
        check(what, pos_input_group_redirected() && !app->composer_focused &&
                        !app->focus_composer_pending);
        pos_input_pop_group();
        lv_group_delete(dialog);
        pump(120);
        snprintf(what, sizeof(what), "%s: and when it closes the focus is where it was", shape);
        check(what, pos_input_focused() == app->keysink);

        /* The row tapped again is the reader asking again. */
        tap(ancestor(find_exact(content(), "OSLO-01"), 1));
        pump(200);
        snprintf(what, sizeof(what), "%s: tapping the open conversation's row gives the "
                                     "composer the keys", shape);
        check(what, strcmp(app->conv, KEY_A) == 0 && pos_input_focused() == field);
        snprintf(what, sizeof(what), "%s: no touch keyboard was asked for by any of it", shape);
        check(what, kb_shows == 0);
        app_stop();
    }

    /* Turning the panel while writing: the other shape's composer takes over. */
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    quiet_client();
    give_nodes();
    give_service();
    give_messages();
    rift_app_open_conversation(app, KEY_B);
    pump(200);
    check("portrait: opened from code, the thread's field has the keys",
          rift_comms_field(app) && pos_input_focused() == rift_comms_field(app));
    use_display(POS_ROTATION_270, PANEL_CORNER);
    pump(300);
    check("turned to landscape, the command line's composer has them",
          app->wide && pos_input_focused() == app->composer);
    use_display(POS_ROTATION_0, PANEL_CORNER);
    pump(300);
    check("and turned back, the thread's field again",
          !app->wide && pos_input_focused() == rift_comms_field(app));
    app_stop();
}

static void manage_live_session(void)
{
    const char *bin = getenv("RIFT_FAKE_MESHCORED");
    const char *run = getenv("POCKETOS_RUNTIME_DIR");
    char sock[512];
    char manage_log[512];
    char line[512];
    char shared[64] = "";
    struct stat st;
    int lines = 0;
    int waited;
    pid_t pid;
    FILE *f;

    if (!bin || !run || access(bin, X_OK) != 0) {
        printf("     (the live management session needs RIFT_FAKE_MESHCORED and "
               "POCKETOS_RUNTIME_DIR; not run)\n");
        return;
    }
    snprintf(sock, sizeof(sock), "%s/meshcored.sock", run);
    snprintf(manage_log, sizeof(manage_log), "%s/rift-manage-log", g_state_dir);
    unlink(manage_log);
    pid = live_spawn(bin, manage_log);
    for (waited = 0; waited < 5000 && stat(sock, &st) != 0; waited += 20) {
        usleep(20000);
    }
    check("the scripted service is up", pid > 0 && stat(sock, &st) == 0);

    app_start();
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    check("RIFT reads the service: identity, channels and the path hash size",
          live_until(live_ready, 8000));

    /* A private channel: a key made here, joined, shown once. */
    tap(action_of(find_exact(content(), "ADD CHANNEL")));
    pump(150);
    tap(action_of(find_exact(content(), "PRIVATE")));
    pump(150);
    field_set(rift_manage_name_field(app), "Hytta");
    tap(action_of(find_exact(content(), "JOIN")));
    check("the join is answered", live_until(live_settled, 8000) &&
                                      app->model.manage_op.done &&
                                      app->model.manage_op.kind == RIFT_ACTION_CHANNEL_ADD);
    check("and the channel is listed", live_until(live_two_channels, 8000) &&
                                           find_exact(content(), "Hytta") != NULL);
    pump(150);
    check("the key made here is shown once, to be shared",
          find_text(content(), "KEY TO SHARE") != NULL &&
              find_text(content(), "Give this key") != NULL);
    {
        lv_obj_t *share_key = find_text(content(), "==");

        if (share_key) {
            snprintf(shared, sizeof(shared), "%s", lv_label_get_text(share_key));
        }
    }
    check("as 16 bytes of base64", strlen(shared) == 24);
    check("and is held nowhere in the model",
          shared[0] && !mem_holds(&app->model, sizeof(app->model), shared));
    tap(action_of(find_exact(content(), "DONE")));
    pump(150);
    check("DONE puts it away for good", find_text(content(), "KEY TO SHARE") == NULL &&
                                            (!shared[0] || find_text(content(), shared) == NULL));

    /* Leave it again, through the confirmation. */
    {
        lv_obj_t *row_name = find_exact(content(), "Hytta");
        lv_obj_t *row = ancestor(row_name, 2);
        lv_obj_t *confirm_cancel;

        tap(kid(row, 2)); /* name, MUTE, LEAVE */
        pump(150);
        check("its LEAVE asks first", find_text(content(), "Leave Hytta?") != NULL);
        confirm_cancel = action_of(find_exact(content(), "CANCEL"));
        tap(kid(confirm_cancel ? lv_obj_get_parent(confirm_cancel) : NULL, 1));
    }
    check("the leave is answered", live_until(live_settled, 8000) && app->model.manage_op.done &&
                                       app->model.manage_op.kind == RIFT_ACTION_CHANNEL_REMOVE);
    check("and only that channel is gone", live_until(live_one_channel, 8000) &&
                                               find_exact(content(), "SITE") != NULL);
    pump(150);
    check("which the panel says", find_text(content(), "Hytta LEFT") != NULL);
    check("and the path hash size is still drawn as the chosen one after both",
          lv_color_eq(lv_obj_get_style_bg_color(action_of(find_exact(content(), "1 B")),
                                                LV_PART_MAIN),
                      pos_theme_color(POS_COLOR_ACCENT_PRIMARY)) &&
              !lv_color_eq(lv_obj_get_style_bg_color(action_of(find_exact(content(), "2 B")),
                                                     LV_PART_MAIN),
                           pos_theme_color(POS_COLOR_ACCENT_PRIMARY)));

    /* Rename. */
    tap(action_of(find_exact(content(), "RENAME")));
    pump(150);
    field_set(rift_device_rename_field(app), "K230-\xC3\x98st");
    tap(action_of(find_exact(content(), "SAVE")));
    check("the rename is answered", live_until(live_settled, 8000) && app->model.manage_op.done);
    pump(150);
    check("DEVICE shows the new name, and when peers will see it",
          find_exact(content(), "K230-\xC3\x98st") != NULL &&
              find_text(content(), "AFTER YOUR NEXT ADVERT") != NULL);

    /* Path hash size, through its confirmation. */
    tap(action_of(find_exact(content(), "2 B")));
    pump(150);
    tap(action_of(find_exact(content(), "USE IT")));
    check("the size is answered", live_until(live_settled, 8000) && app->model.manage_op.done &&
                                      app->model.path_hash_bytes == 2);
    pump(150);
    check("and said", find_text(content(), "2 BYTES PER RELAY") != NULL);
    check("with 2 B the chosen one",
          lv_color_eq(lv_obj_get_style_bg_color(action_of(find_exact(content(), "2 B")),
                                                LV_PART_MAIN),
                      pos_theme_color(POS_COLOR_ACCENT_PRIMARY)));
    tap(action_of(find_exact(content(), "1 B")));
    check("back to 1 byte needs no confirmation", live_until(live_settled, 8000) &&
                                                     app->model.path_hash_bytes == 1);
    app_stop();
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    f = fopen(manage_log, "r");
    while (f && fgets(line, sizeof(line), f)) {
        lines++;
        if (lines == 1) {
            check("the join carried the key that was shown, and the name typed",
                  shared[0] && strstr(line, shared) != NULL &&
                      strstr(line, "\"name\":\"Hytta\"") != NULL);
        }
    }
    if (f) {
        fclose(f);
    }
    check("five changes, each asked for once by a press: join, leave, rename, 2 B, 1 B",
          lines == 5);
    unlink(manage_log);
}

/* NET's rings container: the section's fourth child (the PATH panel, the
 * legend and the note come first). */
static lv_obj_t *net_rings(void)
{
    return kid(kid(content(), 3), 3);
}

/* NET: the rings, from real nodes, and nothing drawn that was not observed. */
static void net_session(void)
{
    struct rift_net net;
    char json[1024];
    lv_obj_t *hytta;
    lv_obj_t *oslo;
    cJSON *o;

    app_start();
    quiet_client();
    rift_app_show_section(app, RIFT_SEC_NET);
    pump(80);
    check("NET before any snapshot waits for the service",
          find_text(content(), "Waiting for meshcored") != NULL);
    o = cJSON_Parse("{\"count\":0,\"nodes\":[]}");
    rift_model_apply_nodes(&app->model, o);
    cJSON_Delete(o);
    rift_app_refresh(app);
    pump(80);
    check("an empty mesh is said, not drawn as empty rings",
          find_text(content(), "No node has adverted") != NULL);
    check("and with nothing selected the panel says how to choose",
          find_text(content(), "No node selected") != NULL &&
              find_exact(content(), "MESSAGE") == NULL);

    give_service();
    give_nodes();
    rift_app_refresh(app);
    pump(120);
    rift_net_build(&app->model, rift_mono_ms(), &net);
    check("the node heard direct is on ring 1",
          net.ring[1].count == 1 && strcmp(net.ring[1].node[0]->key, KEY_A) == 0);
    check("eight relays out is the 9+ ring",
          net.ring[9].count == 1 && strcmp(net.ring[9].node[0]->key, KEY_B) == 0);
    check("three relays out is ring 4",
          net.ring[4].count == 1 && strcmp(net.ring[4].node[0]->key, KEY_D) == 0);
    check("nodes with nothing observed are on NO PATH, not on a guess", net.ring[10].count == 2);
    check("and every ring was placed by a learned route",
          net.ring[1].route == 1 && net.ring[4].route == 1 && net.ring[9].route == 1);
    check("NET shows the rings it holds",
          find_exact(content(), "0 SELF") && find_exact(content(), "1 DIRECT") &&
              find_exact(content(), "9+") && find_exact(content(), "? NO PATH"));
    check("ring 0 is this device", find_exact(content(), "K230-A") != NULL);
    check("a pill per node, bounded",
          rift_net_view_pills(app, 1) == 1 && rift_net_view_pills(app, 10) == 2 &&
              rift_net_view_pills(app, 9) <= RIFT_NET_RING_SHOWN + 1);
    check("no node is drawn twice", count_exact(content(), "OSLO-01") == 1);

    hytta = action_of(find_exact(content(), "HYTTA"));
    tap(hytta);
    check("a pill selects its node", app->have_selected && strcmp(app->selected, KEY_B) == 0);
    check("and does nothing else: NET stays", app->section == RIFT_SEC_NET);
    check("the panel says which ring and how it was placed",
          find_text(content(), "RING 9+") != NULL && find_text(content(), "LEARNED ROUTE") != NULL);
    check("and writes the route out, from this device",
          find_text(content(), "K230-A \xE2\x80\xBA") != NULL);
    /* In the rings, not the PATH panel, which now names the node too. */
    hytta = action_of(find_exact(net_rings(), "HYTTA"));
    oslo = action_of(find_exact(net_rings(), "OSLO-01"));
    check("the selected node is filled in the accent",
          hytta && lv_color_eq(lv_obj_get_style_bg_color(hytta, 0),
                               pos_theme_color(POS_COLOR_ACCENT_PRIMARY)));
    check("a node the route runs through carries the outline",
          oslo && lv_obj_get_style_outline_width(oslo, 0) > 0);
    check("one it does not run through does not",
          lv_obj_get_style_outline_width(action_of(find_exact(content(), "never-heard")), 0) == 0);
    shot("portrait-net");

    /* A node placed by its advert alone says so, and draws no chain. */
    snprintf(json, sizeof(json),
             "{\"reason\":\"discovered\",\"node\":{\"public_key\":\"" KEY_R2 "\",\"name\":\"RPT-SYD\","
             "\"type\":2,\"path_known\":false,\"last_heard_mono_ms\":%lld,\"advert_hops\":2,"
             "\"advert_mono_ms\":%lld}}",
             (long long)(rift_mono_ms() - 30000), (long long)(rift_mono_ms() - 30000));
    give_node_json(json);
    rift_app_refresh(app);
    pump(80);
    rift_net_build(&app->model, rift_mono_ms(), &net);
    check("an advert two relays out is ring 3, placed by the advert",
          net.ring[3].count == 1 && net.ring[3].advert == 1);
    tap(action_of(find_exact(content(), "RPT-SYD")));
    check("and its panel says no route is learned, and how far the advert came",
          find_text(content(), "NO ROUTE LEARNED") != NULL &&
              find_text(content(), "THROUGH 2 RELAYS") != NULL);
    check("its ring's count says it was placed by an advert",
          find_text(content(), "1 \xC2\xB7 1 ADV") != NULL);
    check("and the legend counts rings as the panel does: hops, one more than the relays",
          find_text(content(), "RING = HOPS: 1 IS DIRECT") != NULL &&
              find_text(content(), "RELAYS BETWEEN") == NULL);

    check("a repeater's PATH panel offers no MESSAGE, only DETAIL",
          find_exact(content(), "MESSAGE") == NULL &&
              find_exact(content(), "DETAIL \xE2\x80\xBA") != NULL);
    tap(action_of(find_exact(net_rings(), "HYTTA")));
    pump(60);
    tap(action_of(find_exact(content(), "MESSAGE")));
    check("a chat node's MESSAGE opens COMMS on it, and sends nothing",
          app->section == RIFT_SEC_COMMS && rift_comms_open_peer(app) &&
              strcmp(rift_comms_open_peer(app), KEY_B) == 0 && !rift_model_sending(&app->model));
    rift_app_show_section(app, RIFT_SEC_NET);
    pump(60);
    tap(action_of(find_exact(net_rings(), "RPT-SYD")));
    pump(60);
    tap(action_of(find_exact(content(), "DETAIL \xE2\x80\xBA")));
    check("DETAIL opens the node in NODES", app->section == RIFT_SEC_NODES && app->detail_open);
    rift_app_show_section(app, RIFT_SEC_NET);
    pump(60);
    check("Back from NET is the node list it lives under", app_rift.back(app) == 1 &&
                                           app->section == RIFT_SEC_NODES);
    /* NET is reached from NODES, and leaves back to it. */
    pump(60);
    check("NODES' find bar offers NET", action_of(find_exact(content(), "NET")) != NULL);
    tap(action_of(find_exact(content(), "NET")));
    pump(60);
    check("NET opens under NODES: the NODES tab stays lit",
          app->section == RIFT_SEC_NET && rift_tab_of(app->section) == RIFT_SEC_NODES &&
              visible(app->tab_rule[RIFT_SEC_NODES]) && !visible(app->tab_rule[RIFT_SEC_ACTIVITY]));
    check("with the rings as they were", find_exact(content(), "1 DIRECT") != NULL);
    tap(action_of(find_text(content(), "LIST")));
    pump(60);
    check("LIST goes back to the node list", app->section == RIFT_SEC_NODES &&
                                                 find_text(content(), "HOPS") != NULL);
    tap(action_of(find_exact(content(), "NET")));
    pump(60);
    pos_input_push_key(LV_KEY_ESC);
    pump(60);
    check("and Esc in NET is the node list too", app->section == RIFT_SEC_NODES);
    rift_app_show_section(app, RIFT_SEC_NET);
    pump(60);

    use_display(POS_ROTATION_270, PANEL_CORNER);
    pump(160);
    {
        lv_obj_t *dir = find_exact(content(), "1 DIRECT");
        lv_obj_t *none = find_exact(content(), "? NO PATH");
        lv_area_t a;
        lv_area_t b;

        check("turned, NET is still there", app->section == RIFT_SEC_NET && dir && none);
        if (dir && none) {
            lv_obj_get_coords(dir, &a);
            lv_obj_get_coords(none, &b);
        }
        check("and the rings are columns side by side",
              dir && none && a.y1 == b.y1 && a.x2 < b.x1);
        check("inside the body", none && inside_body(none));
        check("with every word inside its button", labels_overflowing(content()) == 0);
    }
    shot("landscape-net");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_stop();
}

/* ---- RX LOG (DS §54) ---------------------------------------------------- */

static size_t heap_in_use(void);

/* One mesh.rx as the client hands it to the ring, `ago` ms before now. */
static void give_rx(long ago, const char *fields)
{
    char json[2048];
    cJSON *d;

    snprintf(json, sizeof(json), "{\"v\":1,\"mono_ms\":%lld,%s}", (long long)(rift_mono_ms() - ago),
             fields);
    d = cJSON_Parse(json);
    if (!d) {
        printf("FAIL an RX LOG fixture parses: %s\n", json);
        failed++;
        return;
    }
    rift_rxlog_apply(&app->rxlog, d, rift_mono_ms(), rift_rxlog_wall_now());
    cJSON_Delete(d);
}

#define RX_GT "\"type\":\"group_text\",\"type_code\":5,\"route\":\"flood\",\"path_kind\":\"hops\"," \
              "\"path_hash_size\":1,\"relayed\":false,\"own\":false,\"channel_hash\":\"a7\"," \
              "\"channel_known\":true,\"hash\":\"3c9a00000000aa01\""

/* What a busy minute sounds like: one channel message heard three times,
 * a DM, an advert, someone else's request, a rejected frame and a packet
 * that came a long way. */
static void give_rx_traffic(void)
{
    char hops[200];
    char json[600];
    int i;

    give_rx(9000, "\"seq\":1,\"bytes\":42,\"rssi_dbm\":-71,\"snr_db\":9,\"verdict\":\"new\",\"dup\":1,"
                  RX_GT ",\"path_hops\":4,\"path_hex\":\"6e677473\",\"decoded\":\"channel\","
                  "\"channel_name\":\"Public\",\"sender\":\"Anna\","
                  "\"text\":\"Kommer opp om 10 min \xF0\x9F\x91\x8D\"");
    give_rx(8885, "\"seq\":2,\"bytes\":42,\"rssi_dbm\":-76,\"snr_db\":7,\"verdict\":\"duplicate\","
                  "\"dup\":2," RX_GT ",\"path_hops\":4,\"path_hex\":\"6e677473\"");
    give_rx(8700, "\"seq\":3,\"bytes\":42,\"rssi_dbm\":-109,\"snr_db\":-8.5,\"verdict\":\"duplicate\","
                  "\"dup\":3," RX_GT ",\"path_hops\":0");
    give_rx(7000, "\"seq\":4,\"bytes\":60,\"rssi_dbm\":-64,\"snr_db\":11,\"verdict\":\"new\",\"dup\":1,"
                  "\"type\":\"text\",\"type_code\":2,\"route\":\"direct\",\"path_kind\":\"hops\","
                  "\"path_hash_size\":1,\"path_hops\":0,\"hash\":\"77aa000000000004\","
                  "\"dest_hash\":\"5f\",\"for_us\":true,\"src_hash\":\"b2\",\"decoded\":\"direct\","
                  "\"sender\":\"HYTTA\",\"recipient\":\"K230-A\",\"text\":\"Er du der?\","
                  "\"sender_public_key\":\"" KEY_B "\"");
    give_rx(6000, "\"seq\":5,\"bytes\":110,\"rssi_dbm\":-92,\"snr_db\":1.5,\"verdict\":\"new\","
                  "\"dup\":1,\"type\":\"advert\",\"type_code\":4,\"route\":\"flood\","
                  "\"path_kind\":\"hops\",\"path_hash_size\":1,\"path_hops\":1,\"path_hex\":\"4d\","
                  "\"hash\":\"adad000000000005\",\"src_hash\":\"a1\",\"decoded\":\"advert\","
                  "\"sender\":\"OSLO-01\",\"sender_public_key\":\"" KEY_A "\"");
    give_rx(5000, "\"seq\":6,\"bytes\":28,\"rssi_dbm\":-88,\"snr_db\":2,\"verdict\":\"new\",\"dup\":1,"
                  "\"type\":\"req\",\"type_code\":0,\"route\":\"flood\",\"path_kind\":\"hops\","
                  "\"path_hash_size\":1,\"path_hops\":2,\"path_hex\":\"4d73\","
                  "\"hash\":\"b2b2000000000006\",\"dest_hash\":\"c3\",\"for_us\":false,"
                  "\"src_hash\":\"d4\"");
    give_rx(4000, "\"seq\":7,\"bytes\":5,\"rssi_dbm\":-118,\"snr_db\":-14,\"verdict\":\"rejected\","
                  "\"reject\":\"unparsed\",\"type\":\"group_text\",\"type_code\":5,\"route\":\"flood\"");
    hops[0] = '\0';
    for (i = 0; i < 30; i++) {
        snprintf(hops + 4 * i, sizeof(hops) - (size_t)(4 * i), "%02x%02x", 0x10 + i, 0x80 + i);
    }
    snprintf(json, sizeof(json),
             "\"seq\":8,\"bytes\":180,\"rssi_dbm\":-99,\"snr_db\":-3,\"verdict\":\"new\",\"dup\":1,"
             "\"type\":\"advert\",\"type_code\":4,\"route\":\"flood\",\"path_kind\":\"hops\","
             "\"path_hash_size\":2,\"path_hops\":30,\"path_hex\":\"%s\","
             "\"hash\":\"fafa000000000008\",\"src_hash\":\"e5\"",
             hops);
    give_rx(3000, json);
    rift_app_refresh(app);
    pump(300);
}

/* The row whose first line says `word` in its state or type, or -1. */
static int rx_row_with(const char *word)
{
    char text[400];
    int i;

    for (i = 0; i < rift_rxlog_view_rows_shown(app); i++) {
        if (rift_rxlog_view_row_text(app, i, 0, text, sizeof(text)) == 0 && strstr(text, word)) {
            return i;
        }
    }
    return -1;
}

static lv_obj_t *rx_field(int row, int line, int i)
{
    lv_obj_t *r = rift_rxlog_view_row(app, row);

    if (!r) {
        return NULL;
    }
    /* A row is line one (the bar, then the eight fields), the path, and the
     * third line (who, what). */
    if (line == 0) {
        return kid(kid(r, 0), (uint32_t)i + 1);
    }
    if (line == 1) {
        return kid(r, 1);
    }
    return kid(kid(r, 2), (uint32_t)i);
}

static int same_colour(lv_obj_t *label, lv_color_t want)
{
    return label && lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), want);
}

static void rxlog_session(void)
{
    char text[600];
    lv_obj_t *b;
    int dup2;
    int dup3;
    int nodes;
    size_t heap_first = 0;
    size_t heap_last = 0;
    int i;

    app_start();
    quiet_client();
    give_nodes();
    give_service();
    nodes = app->model.node_count;

    /* ---- the way in ---- */
    b = rift_activity_rxlog_button(app);
    check("ACTIVITY has an RX LOG action", b && find_exact(b, "RX LOG") != NULL);
    tap(b);
    pump(60);
    check("which opens RX LOG", app->section == RIFT_SEC_RXLOG);
    check("under the ACTIVITY tab, as NET is under NODES",
          rift_tab_of(app->section) == RIFT_SEC_ACTIVITY && visible(app->tab_rule[RIFT_SEC_ACTIVITY]));
    check("an empty log with no service says it is waiting",
          find_text(content(), "Waiting for meshcored") != NULL);
    check("with PAUSE, CLEAR and the filter", visible(rift_rxlog_view_button(app, 0)) &&
                                                   visible(rift_rxlog_view_button(app, 1)) &&
                                                   find_exact(content(), "FILTER ALL") != NULL);

    /* ---- rows ---- */
    give_rx_traffic();
    check("every reception is a row, repeats included", rift_rxlog_view_rows_shown(app) == 8);
    rift_rxlog_view_row_text(app, 0, 0, text, sizeof(text));
    check("newest first", strstr(text, "ADV") != NULL && strstr(text, "180B") != NULL);
    dup2 = rx_row_with("DUP #2");
    dup3 = rx_row_with("DUP #3");
    check("DUP #2 and DUP #3 are rows of their own", dup2 >= 0 && dup3 >= 0 && dup2 != dup3);
    rift_rxlog_view_row_text(app, dup2, 0, text, sizeof(text));
    check("with the channel hash, packet hash, size and signal",
          strstr(text, "CH:A7") && strstr(text, "H:3C9A") && strstr(text, "42B") &&
              strstr(text, "RSSI:" RIFT_MINUS "76") && strstr(text, "SNR:7"));
    rift_rxlog_view_row_text(app, dup2, 1, text, sizeof(text));
    check("the whole path", strcmp(text, "PATH  6E > 67 > 74 > 73") == 0);
    rift_rxlog_view_row_text(app, dup2, 2, text, sizeof(text));
    check("and the text the first copy decoded, with the claimed sender",
          strstr(text, "#Public Anna?:") && strstr(text, "Kommer opp om 10 min"));
    {
        int first = rx_row_with("RX ");

        rift_rxlog_view_row_text(app, 7, 2, text, sizeof(text));
        check("the first reception reads the same", first >= 0 && strstr(text, "Kommer opp"));
    }
    {
        int dm = rx_row_with("RX MSG CH:--");

        rift_rxlog_view_row_text(app, dm < 0 ? 0 : dm, 2, text, sizeof(text));
        check("a direct message says who to whom", dm >= 0 && strstr(text, "DM HYTTA") &&
                                                       strstr(text, "K230-A") && strstr(text, "Er du der?"));
        check("its sender in the accent of its conversation",
              dm >= 0 && same_colour(rx_field(dm, 2, 0), pos_identity_hue(rift_ident_hash(KEY_B))));
    }
    {
        int req = rx_row_with("REQ");

        rift_rxlog_view_row_text(app, req < 0 ? 0 : req, 2, text, sizeof(text));
        check("someone else's request is encrypted, between whom it says",
              req >= 0 && strstr(text, "[ENCRYPTED") && strstr(text, "D4"));
        check("in a muted colour",
              req >= 0 && same_colour(rx_field(req, 2, 1), lv_color_hex(pos_theme_rgb(POS_COLOR_TEXT_MUTED))));
        check("REQ in the colour of the mesh at work", req >= 0 && same_colour(rx_field(req, 0, 2), pos_identity_hue(5)));
    }
    {
        int rej = rx_row_with("REJ");

        rift_rxlog_view_row_text(app, rej < 0 ? 0 : rej, 2, text, sizeof(text));
        check("a rejected frame says so", rej >= 0 && strstr(text, "[REJECTED"));
        check("in red, and its weak signal too",
              rej >= 0 && same_colour(rx_field(rej, 0, 1), lv_color_hex(pos_theme_rgb(POS_COLOR_STATUS_ERROR))) &&
                  same_colour(rx_field(rej, 0, 6), lv_color_hex(pos_theme_rgb(POS_COLOR_STATUS_ERROR))));
    }
    check("RX in its colour, DUP in another",
          same_colour(rx_field(7, 0, 1), pos_identity_hue(4)) &&
              same_colour(rx_field(dup2, 0, 1), pos_identity_hue(6)));
    check("a good signal is green", same_colour(rx_field(dup2, 0, 6), lv_color_hex(pos_theme_rgb(POS_COLOR_STATUS_OK))));
    check("a relayed path is orange, a direct one green",
          same_colour(rx_field(dup2, 1, 0), pos_identity_hue(1)) &&
              same_colour(rx_field(dup3, 1, 0), pos_identity_hue(3)));
    {
        lv_obj_t *path = rx_field(0, 1, 0);
        char want[300];
        size_t at = 0;

        for (i = 0; i < 30; i++) {
            at += (size_t)snprintf(want + at, sizeof(want) - at, "%s%02X%02X", i ? " > " : "PATH  ",
                                   0x10 + i, 0x80 + i);
        }
        text[0] = '\0';
        rift_rxlog_view_row_text(app, 0, 1, text, sizeof(text));
        check("a 30-hop path is printed whole", strcmp(text, want) == 0);
        if (path) {
            lv_obj_update_layout(path);
        }
        check("and wraps onto further lines rather than being cut",
              path && lv_obj_get_height(path) > rift_caption_h() && inside_body(path));
    }
    rift_rxlog_view_row_text(app, 0, 2, text, sizeof(text));
    check("an advert from a node nobody holds says its name is not known",
          strcmp(text, "[NAME NOT KNOWN]") == 0);
    check("every row's first line fits the portrait width",
          rift_rxlog_view_row(app, dup2) &&
              lv_obj_get_height(kid(rift_rxlog_view_row(app, dup2), 0)) <= rift_caption_h() + 2 &&
              inside_body(kid(rift_rxlog_view_row(app, dup2), 0)));
    shot("portrait-rxlog");

    /* ---- small and fixed, whatever the text size ---- */
    if (rx_field(0, 0, 0)) {
        const lv_font_t *small = lv_obj_get_style_text_font(rx_field(0, 0, 0), LV_PART_MAIN);

        pos_theme_select_text_size(POS_TEXT_SIZE_LARGE);
        pump(120);
        check("at Large the log keeps its compact face",
              lv_obj_get_style_text_font(rx_field(0, 0, 0), LV_PART_MAIN) == small);
        pos_theme_select_text_size(POS_TEXT_SIZE_SMALL);
        pump(120);
    }

    /* ---- PAUSE ---- */
    rift_rxlog_view_row_text(app, 0, 0, text, sizeof(text));
    {
        char before[600];

        snprintf(before, sizeof(before), "%s", text);
        tap(rift_rxlog_view_button(app, 0));
        check("PAUSE pauses", app->rxlog.paused && find_exact(content(), "RESUME") != NULL);
        give_rx(10, "\"seq\":9,\"bytes\":20,\"verdict\":\"new\",\"type\":\"ack\",\"type_code\":3,"
                    "\"route\":\"flood\",\"path_kind\":\"hops\",\"path_hash_size\":1,\"path_hops\":0,"
                    "\"hash\":\"ac00000000000009\",\"dup\":1");
        give_rx(5, "\"seq\":10,\"bytes\":20,\"verdict\":\"new\",\"type\":\"ack\",\"type_code\":3,"
                   "\"route\":\"flood\",\"path_kind\":\"hops\",\"path_hash_size\":1,\"path_hops\":0,"
                   "\"hash\":\"ac0000000000000a\",\"dup\":1");
        rift_app_refresh(app);
        pump(300);
        rift_rxlog_view_row_text(app, 0, 0, text, sizeof(text));
        check("and the view stays where it was", strcmp(text, before) == 0);
        check("while capture goes on, and says how much", app->rxlog.count == 10 &&
                                                              find_text(content(), "2 NEWER ABOVE") != NULL &&
                                                              find_text(content(), "PAUSED") != NULL);
        tap(rift_rxlog_view_button(app, 0));
        rift_rxlog_view_row_text(app, 0, 0, text, sizeof(text));
        check("RESUME is back to the newest, live",
              !app->rxlog.paused && strstr(text, "ACK") != NULL && find_text(content(), "LIVE") != NULL);
        rift_rxlog_view_row_text(app, 0, 2, text, sizeof(text));
        check("an ACK has nothing to read, and no third line", strcmp(text, "") == 0);
    }

    /* ---- the filter ---- */
    tap(rift_rxlog_view_button(app, 2));
    check("FILTER DUP shows only the repeats", find_exact(content(), "FILTER DUP") != NULL &&
                                                   rift_rxlog_view_rows_shown(app) == 2);
    tap(rift_rxlog_view_button(app, 2));
    check("MSG the messages", find_exact(content(), "FILTER MSG") != NULL &&
                                  rift_rxlog_view_rows_shown(app) == 5);
    tap(rift_rxlog_view_button(app, 2));
    check("ADV the adverts", rift_rxlog_view_rows_shown(app) == 2);
    tap(rift_rxlog_view_button(app, 2));
    check("CTRL the rest", rift_rxlog_view_rows_shown(app) == 3);
    tap(rift_rxlog_view_button(app, 2));
    check("and round to ALL", find_exact(content(), "FILTER ALL") != NULL &&
                                  rift_rxlog_view_rows_shown(app) == 10);

    /* ---- the detail ---- */
    tap(rift_rxlog_view_row(app, rx_row_with("DUP #2")));
    pump(60);
    check("a tap on a row opens its detail", rift_rxlog_view_detail(app) != NULL);
    check("with every field: the whole hash, MeshCore's verdict",
          rift_rxlog_view_detail(app) && strstr(lv_label_get_text(rift_rxlog_view_detail(app)), "3C9A00000000AA01") &&
              strstr(lv_label_get_text(rift_rxlog_view_detail(app)), "MESHCORE DUPLICATE") &&
              strstr(lv_label_get_text(rift_rxlog_view_detail(app)), "earlier copy"));
    shot("portrait-rxlog-detail");
    pos_input_push_key(LV_KEY_ESC);
    pump(60);
    check("Esc closes it and stays in RX LOG", rift_rxlog_view_detail(app) == NULL &&
                                                   app->section == RIFT_SEC_RXLOG);
    pos_input_push_key(LV_KEY_ESC);
    pump(60);
    check("Esc again is ACTIVITY", app->section == RIFT_SEC_ACTIVITY);
    pos_input_push_key('r');
    pump(60);
    check("R on ACTIVITY opens RX LOG", app->section == RIFT_SEC_RXLOG);

    /* ---- keys and scrolling ---- */
    pos_input_push_key(LV_KEY_HOME);
    pump(60);
    check("Home is the newest, live, with nothing selected",
          app->rxlog.top_uid == 0 && app->rxlog.selected_uid == 0 && !app->rxlog.paused);
    pos_input_push_key(LV_KEY_DOWN);
    pos_input_push_key(LV_KEY_DOWN);
    pump(60);
    check("Down selects, and moves the selection",
          app->rxlog.selected_uid == rift_rxlog_at(&app->rxlog, 1)->uid);
    pos_input_push_key(LV_KEY_ENTER);
    pump(60);
    check("Enter opens the selected one", rift_rxlog_view_detail(app) != NULL);
    check("and Back closes it first", app_rift.back(app) == 1 && rift_rxlog_view_detail(app) == NULL &&
                                          app->section == RIFT_SEC_RXLOG);
    rift_rxlog_view_scroll(app, 3);
    pump(60);
    rift_rxlog_view_row_text(app, 0, 0, text, sizeof(text));
    check("a drag of three rows shows the fourth newest at the top",
          rift_rxlog_at(&app->rxlog, 3) && app->rxlog.top_uid == rift_rxlog_at(&app->rxlog, 3)->uid &&
              find_text(content(), "HELD") != NULL);
    if (rift_rxlog_view_row(app, 0)) {
        lv_area_t a;
        lv_obj_t *list = lv_obj_get_parent(rift_rxlog_view_row(app, 0));

        lv_obj_get_coords(list, &a);
        finger_point.x = (a.x1 + a.x2) / 2;
        finger_point.y = a.y1 + 40;
        finger_state = LV_INDEV_STATE_PRESSED;
        pump(30);
        for (i = 0; i < 6; i++) {
            finger_point.y += 30;
            pump(20);
        }
        finger_state = LV_INDEV_STATE_RELEASED;
        pump(80);
        check("a finger dragging down goes back to the newest",
              app->rxlog.top_uid == 0 && find_text(content(), "LIVE") != NULL);
        check("and the drag opened nothing", rift_rxlog_view_detail(app) == NULL);
    }
    pos_input_push_key(LV_KEY_HOME);
    pump(60);

    /* ---- landscape ---- */
    use_display(POS_ROTATION_270, PANEL_CORNER);
    pump(200);
    rift_app_refresh(app);
    pump(300);
    check("turned, RX LOG is still there", app->section == RIFT_SEC_RXLOG &&
                                               rift_rxlog_view_rows_shown(app) == 10);
    check("each first line on one line",
          rift_rxlog_view_row(app, 0) &&
              lv_obj_get_height(kid(rift_rxlog_view_row(app, 0), 0)) <= rift_caption_h() + 2);
    check("inside the body", inside_body(rift_rxlog_view_button(app, 2)) && rift_rxlog_view_row(app, 0) &&
                                 inside_body(kid(rift_rxlog_view_row(app, 0), 0)));
    shot("landscape-rxlog");
    use_display(POS_ROTATION_0, PANEL_CORNER);
    pump(200);

    /* ---- what the service sends that it should not ---- */
    give_rx(1, "\"seq\":\"eleven\",\"bytes\":-1,\"verdict\":\"perhaps\"");
    give_rx(1, "\"seq\":11,\"bytes\":10,\"verdict\":\"new\",\"path_kind\":\"hops\","
               "\"path_hash_size\":1,\"path_hops\":9,\"path_hex\":\"00\"");
    rift_app_refresh(app);
    pump(300);
    check("a malformed event draws nothing and is counted",
          app->rxlog.count == 10 && find_text(content(), "2 REFUSED") != NULL);

    /* ---- the bound, CLEAR, and nothing left behind ---- */
    for (i = 0; i < RIFT_RXLOG_MAX + 100; i++) {
        char f[200];

        snprintf(f, sizeof(f), "\"seq\":%d,\"bytes\":12,\"verdict\":\"new\",\"hash\":\"%016x\","
                               "\"dup\":1", 100 + i, 0x5000 + i);
        give_rx(0, f);
    }
    rift_app_refresh(app);
    pump(300);
    check("the log keeps its bound", app->rxlog.count == RIFT_RXLOG_MAX && app->rxlog.evicted >= 110);
    for (i = 0; i < 6; i++) {
        app_leave();
        app_start();
        quiet_client();
        rift_app_show_section(app, RIFT_SEC_RXLOG);
        pump(300);
        if (i == 0) {
            heap_first = heap_in_use();
        }
        heap_last = heap_in_use();
    }
    check("left and opened again, the log is still there", app->rxlog.count == RIFT_RXLOG_MAX &&
                                                                rift_rxlog_view_rows_shown(app) > 0);
    check("and opening and closing it costs nothing that stays",
          heap_last <= heap_first + 16384);
    tap(rift_rxlog_view_button(app, 1));
    check("CLEAR empties the log", app->rxlog.count == 0 && rift_rxlog_view_rows_shown(app) == 0 &&
                                       find_text(content(), "Waiting for meshcored") != NULL);
    check("and nothing else", app->model.node_count == nodes);
    app_stop();
}

/* ---- RIFT behind other screens (DS §51) -------------------------------- */

static int timer_count(void)
{
    lv_timer_t *t = NULL;
    int n = 0;

    while ((t = lv_timer_get_next(t)) != NULL) {
        n++;
    }
    return n;
}

static size_t heap_in_use(void)
{
    return mallinfo2().uordblks;
}

/* A press at one point of the panel, as a finger would. */
static void tap_at(int32_t x, int32_t y)
{
    finger_point.x = x;
    finger_point.y = y;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(80);
}

/* Leave with Back or Home, come back, end it with CLOSE RIFT; and the same
 * twenty times over, with nothing left behind. The shell's part is the
 * counters: go_home is what the back slab and CLOSE RIFT ask for, and the
 * test then does what the shell does next (app_leave). */
static void background_session(void)
{
    struct rift_app *s;
    int timers_idle;
    int timers_open;
    int nodes;
    int plays;
    int home_before;
    int home_start = home_calls;
    unsigned unread_left;
    size_t heap_left;
    size_t heap_open;
    size_t heap_before;
    size_t heap_after;
    int round;

    use_display(POS_ROTATION_0, PANEL_CORNER);
    timers_idle = timer_count();
    check("before RIFT opens there is no session",
          rift_app_session() == NULL && !rift_app_in_background());
    app_start();
    quiet_client();
    s = app;
    timers_open = timer_count();
    check("opening starts one session, on screen",
          rift_app_session() == app && !rift_app_in_background() && app->opens == 1);
    check("with one timer of its own", timers_open == timers_idle + 1);
    check("and no RIFT in the status cluster while RIFT is on screen", bg_label[0] == '\0');
    give_nodes();
    give_service();
    give_messages();
    rift_app_show_section(app, RIFT_SEC_NODES);
    rift_find_set_query(app, "OSLO");
    pump(80);
    rift_app_open_conversation(app, KEY_B);
    pump(120);
    nodes = app->model.node_count;
    check("a conversation is open when RIFT is left",
          app->section == RIFT_SEC_COMMS && find_text(content(), "Fint, ser deg") != NULL);

    /* ---- leave: Home, or Back at the top level ---------------------------- */
    app_leave();
    check("leaving keeps the session", rift_app_session() == s && rift_app_in_background());
    check("with nothing of the screen left in it",
          s->frame == NULL && s->root == NULL && s->strip == NULL && s->back == NULL &&
              s->composer == NULL && s->keysink == NULL && s->nodes == NULL &&
              s->comms == NULL && s->activity == NULL && s->find == NULL && s->net == NULL &&
              s->manage == NULL && s->device == NULL && s->session == NULL &&
              s->theme_host == NULL);
    check("and its timer still running, the only one",
          s->pump != NULL && timer_count() == timers_open);
    check("the status cluster says RIFT", strcmp(bg_label, RIFT_BACKGROUND_LABEL) == 0);
    check("with the words for it: RIFT active in background",
          strcmp(bg_help, "RIFT active in background") == 0);
    check("what it knows is kept", s->model.node_count == nodes && s->model.msg_count > 0);
    check("and where the reader was: COMMS, the conversation, the find bar",
          s->section == RIFT_SEC_COMMS && s->have_conv && strcmp(s->conv, KEY_B) == 0 &&
              strcmp(s->node_query, "OSLO") == 0);

    /* While it is left: a direct message arrives, and the timer runs pass
     * after pass with no screen to touch. */
    plays = fake_plays;
    app = s;
    live_dm(5001, "in", KEY_B, 5001, "while away");
    app = NULL;
    pump(800);
    unread_left = (unsigned)rift_model_unread_total(&s->model);
    check("a message arriving while RIFT is left is taken in, unread", unread_left > 0);
    check("and no sound is asked for with no RIFT on screen", fake_plays == plays);
    check("the mark stays while it runs", strcmp(bg_label, RIFT_BACKGROUND_LABEL) == 0);

    /* ---- reopen ------------------------------------------------------------- */
    app_start();
    check("reopening is the same session, not a new one",
          app == s && app->opens == 2 && !rift_app_in_background());
    check("with no second timer", timer_count() == timers_open);
    check("the status cluster's RIFT goes", bg_label[0] == '\0');
    check("it opens where it was left: COMMS, the same conversation",
          app->section == RIFT_SEC_COMMS && rift_comms_open_peer(app) &&
              strcmp(rift_comms_open_peer(app), KEY_B) == 0);
    check("showing what arrived while it was away",
          find_text(content(), "while away") != NULL &&
              find_text(content(), "Fint, ser deg") != NULL);
    check("the find bar holds what it held",
          rift_find_field(app) && strcmp(lv_textarea_get_text(rift_find_field(app)), "OSLO") == 0);
    check("and no late sound for a message already filed", fake_plays == plays);
    check("the screen is whole: four sections, built once",
          lv_obj_get_child_count(content()) == (uint32_t)RIFT_SEC_COUNT && inside_body(content()));

    /* ---- Back and Home -------------------------------------------------------- */
    check("Back inside RIFT is RIFT's: COMMS goes to ACTIVITY",
          app_rift.back(app) == 1 && app->section == RIFT_SEC_ACTIVITY &&
              rift_app_session() == s);
    check("and at ACTIVITY it is the shell's (home), which keeps the session",
          app_rift.back(app) == 0);
    app_leave();
    check("left from ACTIVITY, still kept", rift_app_session() == s && rift_app_in_background());
    app_start();
    check("and back on ACTIVITY", app == s && app->section == RIFT_SEC_ACTIVITY);
    use_display(POS_ROTATION_270, PANEL_CORNER);
    pump(160);
    home_before = home_calls;
    tap(app->back);
    check("landscape: the back slab goes home", home_calls == home_before + 1);
    app_leave();
    check("which keeps the session too", rift_app_session() == s && rift_app_in_background() &&
                                             strcmp(bg_label, RIFT_BACKGROUND_LABEL) == 0);
    app_start();
    check("reopened turned, the same session, wide", app == s && app->wide);
    use_display(POS_ROTATION_0, PANEL_CORNER);
    pump(160);

    /* ---- leave and reopen, many times ------------------------------------------ */
    for (round = 0; round < 3; round++) {
        app_leave();
        app_start();
    }
    app_leave();
    heap_left = heap_in_use();
    app_start();
    heap_open = heap_in_use();
    heap_before = heap_open;
    for (round = 0; round < 20; round++) {
        app_leave();
        if (!rift_app_in_background() || timer_count() != timers_open) {
            break;
        }
        app_start();
    }
    heap_after = heap_in_use();
    printf("     a screen is %ld bytes; 20 leaves and reopens left %+ld\n",
           (long)heap_open - (long)heap_left, (long)heap_after - (long)heap_before);
    check("twenty leaves and reopens: one session, one timer, every round",
          round == 20 && app == s && timer_count() == timers_open && app->opens == 4 + 3 + 1 + 20);
    check("one screen at a time", lv_obj_get_child_count(g_content) == 1u &&
                                      lv_obj_get_child_count(content()) == (uint32_t)RIFT_SEC_COUNT);
    check("and less than one screen's memory kept over all twenty",
          heap_open > heap_left && heap_after < heap_before + (heap_open - heap_left));

    /* ---- CLOSE RIFT ------------------------------------------------------------ */
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(120);
    check("SYSTEM has CLOSE RIFT, in SESSION",
          rift_session_close_button(app) && find_exact(content(), "SESSION") != NULL &&
              visible(rift_session_close_button(app)));
    check("a 56 px target", lv_obj_get_height(rift_session_close_button(app)) == RIFT_TOUCH_H);
    check("and no confirmation up",
          !rift_session_confirming(app) && (find_exact(content(), "Close RIFT?") == NULL ||
                                            !visible(find_exact(content(), "Close RIFT?"))));
    tap(rift_session_close_button(app));
    pump(120);
    check("CLOSE RIFT asks first", rift_session_confirming(app) &&
                                       visible(find_exact(content(), "Close RIFT?")));
    check("and says what goes and what stays",
          find_text(content(), "meshcored and the radio keep running.") != NULL);
    check("Cancel is first and accented",
          lv_color_eq(lv_obj_get_style_bg_color(rift_session_confirm_button(app, 0), LV_PART_MAIN),
                      pos_theme_color(POS_COLOR_ACCENT_PRIMARY)));
    home_before = home_calls;
    tap(rift_session_confirm_button(app, 0));
    pump(120);
    check("Cancel ends nothing", !rift_session_confirming(app) && rift_app_session() == s &&
                                     home_calls == home_before && !app->ending);
    tap(rift_session_close_button(app));
    pump(120);
    rift_app_show_section(app, RIFT_SEC_NODES);
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(120);
    check("leaving SYSTEM is Cancel for it too", !rift_session_confirming(app));
    tap(rift_session_close_button(app));
    pump(120);
    tap(rift_session_confirm_button(app, 1));
    check("confirmed, it goes home", home_calls == home_before + 1 && app->ending);
    app_leave();
    check("and the session is over", rift_app_session() == NULL && !rift_app_in_background());
    check("its timer gone with it", timer_count() == timers_idle);
    check("the status cluster's RIFT gone", bg_label[0] == '\0');
    check("and the screen", lv_obj_get_child_count(g_content) == 0u);
    pump(300);

    /* ---- after CLOSE: a new session, from nothing ------------------------------ */
    app_start();
    quiet_client();
    check("opening after CLOSE RIFT starts a new session",
          app && app->opens == 1 && app->section == RIFT_SEC_ACTIVITY &&
              app->model.node_count == 0 && app->model.msg_count == 0 && !app->have_conv &&
              app->node_query[0] == '\0');
    app_stop();
    check("and the shell stopping ends it", rift_app_session() == NULL &&
                                                timer_count() == timers_idle && bg_label[0] == '\0');
    /* The presses on home were the reader's, not the app's doing. */
    home_calls = home_start;
}

/* The navigation row: the way back and the four tabs, five visible faces of
 * one size and one look (DS §51.3). Portrait's way back is the shell's header
 * slab (72 x 56, DS §48), not RIFT's, and is the shell's tests' subject. */
static int same_face(lv_obj_t *a, lv_obj_t *b)
{
    return lv_obj_get_style_bg_opa(a, LV_PART_MAIN) == lv_obj_get_style_bg_opa(b, LV_PART_MAIN) &&
           lv_color_eq(lv_obj_get_style_bg_color(a, LV_PART_MAIN),
                       lv_obj_get_style_bg_color(b, LV_PART_MAIN)) &&
           lv_obj_get_style_border_width(a, LV_PART_MAIN) ==
               lv_obj_get_style_border_width(b, LV_PART_MAIN) &&
           lv_obj_get_style_radius(a, LV_PART_MAIN) == lv_obj_get_style_radius(b, LV_PART_MAIN) &&
           lv_obj_get_height(a) == lv_obj_get_height(b);
}

/* ---- MAP ------------------------------------------------------------------ */

/* Every object under obj, itself included. */
static uint32_t count_objects(lv_obj_t *obj)
{
    uint32_t n = 1;
    uint32_t i;

    for (i = 0; obj && i < lv_obj_get_child_count(obj); i++) {
        n += count_objects(lv_obj_get_child(obj, (int32_t)i));
    }
    return n;
}

/* A node with a claimed location, as mesh.node carries it. */
static void give_located(const char *key, const char *name, int type, const char *where)
{
    char json[640];

    snprintf(json, sizeof(json),
             "{\"reason\":\"discovered\",\"node\":{\"public_key\":\"%s\",\"name\":\"%s\","
             "\"type\":%d,\"path_known\":false,\"last_heard_mono_ms\":%lld%s%s}}",
             key, name, type, (long long)(rift_mono_ms() - 60000), where[0] ? "," : "", where);
    give_node_json(json);
}

/* Tap the marker of this node, where the map put it. */
static void tap_marker(const char *key)
{
    const struct rift_node *n = rift_model_find(&app->model, key);
    const struct rift_map_view *v = rift_map_view_geometry(app);
    lv_area_t a;
    double x;
    double y;

    if (!n || !v) {
        return;
    }
    lv_obj_get_coords(rift_map_view_canvas(app), &a);
    rift_map_project(v, n->lat, n->lon, &x, &y);
    tap_at(a.x1 + (int32_t)x, a.y1 + (int32_t)y);
    pump(80);
}

#define KEY_M1 "81aa000000000000000000000000000000000000000000000000000000000081"
#define KEY_M2 "82bb000000000000000000000000000000000000000000000000000000000082"
#define KEY_M3 "83cc000000000000000000000000000000000000000000000000000000000083"
#define KEY_M4 "84dd000000000000000000000000000000000000000000000000000000000084"
#define KEY_M5 "85ee000000000000000000000000000000000000000000000000000000000085"

static void map_session(void)
{
    static const enum pos_rotation shapes[2] = { POS_ROTATION_0, POS_ROTATION_270 };
    int k;

    /* The geometry, on its own. */
    {
        static struct rift_model m;
        struct rift_map_view v;
        double x;
        double y;
        double lat;
        double lon;
        int32_t px;
        double metres;

        rift_model_init(&m);
        check("an empty mesh places nothing", rift_map_fit(&v, &m, 500, 400) == 0 &&
                                                  rift_map_located(&m) == 0);
        check("valid places are valid", rift_location_valid(59.9, 10.7) &&
                                            rift_location_valid(-33.9, 151.2));
        check("0,0 is none, past a pole or the date line is nothing, NaN is nothing",
              !rift_location_valid(0, 0) && !rift_location_valid(95, 10) &&
                  !rift_location_valid(10, 181) && !rift_location_valid(0.0 / 0.0, 10));
        m.node_count = 2;
        m.nodes[0].have_location = 1;
        m.nodes[0].lat = 59.90;
        m.nodes[0].lon = 10.70;
        m.nodes[1].have_location = 1;
        m.nodes[1].lat = 59.95;
        m.nodes[1].lon = 10.80;
        check("two located nodes are fitted", rift_map_fit(&v, &m, 500, 400) == 2);
        rift_map_project(&v, 59.90, 10.70, &x, &y);
        check("both inside the area, clear of its edge",
              x >= RIFT_MAP_FIT_MARGIN - 1 && y <= 400 - RIFT_MAP_FIT_MARGIN + 1);
        rift_map_project(&v, 59.95, 10.80, &x, &y);
        check("the other too", x <= 500 - RIFT_MAP_FIT_MARGIN + 1 && y >= RIFT_MAP_FIT_MARGIN - 1);
        check("north is up and east is right", x > 250 && y < 200);
        rift_map_unproject(&v, x, y, &lat, &lon);
        check("and back again", fabs(lat - 59.95) < 1e-9 && fabs(lon - 10.80) < 1e-9);
        check("a tap on a marker finds it", rift_map_hit(&v, &m, x + 5, y - 5) == 1);
        check("and one far from both finds nothing", rift_map_hit(&v, &m, 250, 200) == -1);
        metres = rift_map_scale(&v, 120, &px);
        check("the scale bar is a round length that fits",
              px > 0 && px <= 120 && (metres == 1000 || metres == 2000 || metres == 500 ||
                                      metres == 5000 || metres == 200));
        m.node_count = 1;
        rift_map_fit(&v, &m, 500, 400);
        check("one node alone is not an infinite zoom",
              v.ppd <= 400.0 / (RIFT_MAP_MIN_SPAN_M / RIFT_MAP_M_PER_DEG) + 1e-6);
        rift_map_zoom(&v, 1e9);
        check("nor is zooming in for ever", v.ppd <= 400.0 / (RIFT_MAP_MIN_SPAN_M / RIFT_MAP_M_PER_DEG) + 1e-6);
        rift_map_zoom(&v, 1e-9);
        check("and zooming out stops at the world", v.ppd >= 400.0 / RIFT_MAP_MAX_SPAN_DEG - 1e-6);
    }

    for (k = 0; k < 2; k++) {
        const char *tag = k ? "landscape" : "portrait";
        char what[200];

        use_display(shapes[k], PANEL_CORNER);
        app_start();
        quiet_client();
        give_nodes();
        give_service();
        tap(tab(RIFT_SEC_MAP));
        pump(120);
        snprintf(what, sizeof(what), "%s: MAP is a tab of its own", tag);
        check(what, app->section == RIFT_SEC_MAP);
        snprintf(what, sizeof(what), "%s: with no location to place, it says so and draws none",
                 tag);
        check(what, find_text(content(), "No node has said where it is") != NULL &&
                        rift_map_view_markers(app) == 0 &&
                        find_text(content(), "0 OF 5 KNOWN NODES HAVE A LOCATION") != NULL);
        snprintf(what, sizeof(what), "%s: and says this device has none", tag);
        check(what, find_text(content(), "THIS DEVICE HAS NONE") != NULL);

        give_located(KEY_M1, "RPT-HOLMEN", 2, "\"lat\":59.9672,\"lon\":10.6650");
        give_located(KEY_M2, "Kari-T", 1, "\"lat\":59.9139,\"lon\":10.7522");
        give_located(KEY_M3, "NULL-ISLAND", 1, "\"lat\":0,\"lon\":0");
        give_located(KEY_M4, "OFF-THE-MAP", 1, "\"lat\":95.0,\"lon\":10.0");
        give_located(KEY_M5, "NO-GPS", 1, "");
        rift_app_refresh(app);
        pump(200);
        snprintf(what, sizeof(what), "%s: valid coordinates are taken", tag);
        check(what, rift_model_find(&app->model, KEY_M1)->have_location &&
                        rift_model_find(&app->model, KEY_M2)->have_location);
        snprintf(what, sizeof(what), "%s: 0,0, out of range and missing are not", tag);
        check(what, !rift_model_find(&app->model, KEY_M3)->have_location &&
                        !rift_model_find(&app->model, KEY_M4)->have_location &&
                        !rift_model_find(&app->model, KEY_M5)->have_location);
        lv_refr_now(NULL);
        snprintf(what, sizeof(what), "%s: two markers, for the two that said where they are", tag);
        check(what, rift_map_view_markers(app) == 2 &&
                        find_text(content(), "2 OF 10 KNOWN NODES HAVE A LOCATION") != NULL &&
                        find_text(content(), "No node has said where it is") == NULL);
        snprintf(what, sizeof(what), "%s: the map is inside the body", tag);
        check(what, inside_body(content()) && labels_overflowing(content()) == 0);

        tap_marker(KEY_M1);
        snprintf(what, sizeof(what), "%s: a tap on the repeater selects it", tag);
        check(what, app->have_selected && strcmp(app->selected, KEY_M1) == 0 &&
                        find_text(content(), "RPT-HOLMEN") != NULL);
        snprintf(what, sizeof(what), "%s: and says what it is, where, and offers DETAIL, not MESSAGE",
                 tag);
        check(what, find_text(content(), "RPT") != NULL &&
                        find_text(content(), "59.96720\xC2\xB0 N") != NULL &&
                        find_exact(content(), "DETAIL \xE2\x80\xBA") != NULL &&
                        find_exact(content(), "MESSAGE") == NULL);
        shot(k ? "landscape-map" : "portrait-map");
        tap_marker(KEY_M2);
        snprintf(what, sizeof(what), "%s: a chat node offers MESSAGE", tag);
        check(what, strcmp(app->selected, KEY_M2) == 0 &&
                        find_exact(content(), "MESSAGE") != NULL);
        tap(action_of(find_exact(content(), "MESSAGE")));
        pump(80);
        snprintf(what, sizeof(what), "%s: which opens its conversation", tag);
        check(what, app->section == RIFT_SEC_COMMS && app->have_conv &&
                        strcmp(app->conv, KEY_M2) == 0);
        tap(tab(RIFT_SEC_MAP));
        pump(80);
        tap(action_of(find_exact(content(), "DETAIL \xE2\x80\xBA")));
        pump(80);
        snprintf(what, sizeof(what), "%s: DETAIL opens the node in NODES", tag);
        check(what, app->section == RIFT_SEC_NODES && strcmp(app->selected, KEY_M2) == 0);
        tap(tab(RIFT_SEC_MAP));
        pump(80);
        {
            double before = rift_map_view_geometry(app)->ppd;

            tap(action_of(find_exact(content(), "+")));
            pump(60);
            snprintf(what, sizeof(what), "%s: + zooms in", tag);
            check(what, rift_map_view_geometry(app)->ppd > before);
            tap(action_of(find_exact(content(), "FIT")));
            pump(120);
            snprintf(what, sizeof(what), "%s: FIT puts every located node back in view", tag);
            check(what, fabs(rift_map_view_geometry(app)->ppd - before) < 1e-6 * before);
        }
        app_stop();
    }

    /* A thousand known nodes, all of them placed: one drawn object, and a
     * draw that does not take a noticeable time. */
    use_display(POS_ROTATION_270, PANEL_CORNER);
    app_start();
    quiet_client();
    give_service();
    {
        uint32_t objects;
        int64_t t0;
        int64_t spent;
        int i;

        app->model.node_count = RIFT_MAX_NODES;
        for (i = 0; i < RIFT_MAX_NODES; i++) {
            struct rift_node *n = &app->model.nodes[i];

            memset(n, 0, sizeof(*n));
            snprintf(n->key, sizeof(n->key), "%064x", i + 1);
            snprintf(n->hash, sizeof(n->hash), "%02x", (i + 1) & 0xff);
            snprintf(n->name, sizeof(n->name), "N%04d", i);
            n->have_name = 1;
            n->have_type = 1;
            n->type = i % 5 == 0 ? 2 : 1;
            n->have_location = 1;
            n->lat = 58.0 + (double)(i % 40) * 0.05;
            n->lon = 8.0 + (double)(i / 40) * 0.1;
            n->have_heard = 1;
            n->heard_mono_ms = rift_mono_ms() - 1000;
            n->seq = (uint32_t)i + 1;
        }
        app->model.seq = RIFT_MAX_NODES + 1;
        tap(tab(RIFT_SEC_MAP));
        pump(200);
        objects = count_objects(app->map_root);
        t0 = rift_mono_ms();
        lv_obj_invalidate(rift_map_view_canvas(app));
        lv_refr_now(NULL);
        spent = rift_mono_ms() - t0;
        printf("     MAP with %d placed nodes: %d markers drawn, %lld ms a full draw, %u objects\n",
               RIFT_MAX_NODES, rift_map_view_markers(app), (long long)spent, (unsigned)objects);
        check("1000 located nodes are all placed", rift_map_view_markers(app) == RIFT_MAX_NODES);
        check("and drawn in one object, not one each", objects < 40);
        check("a full draw stays well under a second on the host", spent < 1000);
        {
            unsigned draws = rift_map_view_draws(app);

            rift_app_refresh(app);
            pump(1200);
            check("nothing changed, nothing is drawn again", rift_map_view_draws(app) == draws);
        }
        app->model.node_count = 0;
    }
    app_stop();
    use_display(POS_ROTATION_0, PANEL_CORNER);
}

/* ---- repeaters take no direct messages ----------------------------------- */

/* By the node's advertised type, never its name: upstream MeshCore's
 * repeater and sensor read text only from a logged-in admin, as a command. */
static void repeater_session(void)
{
    static const enum pos_rotation shapes[2] = { POS_ROTATION_0, POS_ROTATION_270 };
    int k;

    {
        struct rift_node n;

        memset(&n, 0, sizeof(n));
        check("a node of unknown type is not refused on a guess", rift_node_can_message(&n));
        n.have_type = 1;
        n.type = 1;
        check("a chat node takes direct messages", rift_node_can_message(&n));
        n.type = 3;
        check("so does a room server", rift_node_can_message(&n));
        n.type = 2;
        check("a repeater does not, and says why",
              !rift_node_can_message(&n) && strstr(rift_node_no_message_why(&n), "repeater"));
        n.type = 4;
        check("nor does a sensor", !rift_node_can_message(&n) &&
                                       strstr(rift_node_no_message_why(&n), "sensor"));
        snprintf(n.name, sizeof(n.name), "chat-repeater");
        n.have_name = 1;
        n.type = 1;
        check("a name that says repeater does not make one", rift_node_can_message(&n));
    }

    for (k = 0; k < 2; k++) {
        const char *tag = k ? "landscape" : "portrait";
        char what[160];

        use_display(shapes[k], PANEL_CORNER);
        app_start();
        quiet_client();
        give_nodes();
        give_service();
        give_messages();
        rift_app_show_section(app, RIFT_SEC_NODES);
        pump(80);

        /* A repeater cannot be opened as a conversation, by any door. */
        rift_app_open_conversation(app, KEY_D);
        pump(60);
        snprintf(what, sizeof(what), "%s: a repeater opens no conversation", tag);
        check(what, app->section == RIFT_SEC_NODES &&
                        !(app->have_conv && strcmp(app->conv, KEY_D) == 0));
        rift_app_select(app, KEY_D);
        pump(120);
        if (!app->wide) {
            snprintf(what, sizeof(what), "%s: its row offers DETAIL, not MESSAGE, and says why",
                     tag);
            check(what, find_exact(content(), "MESSAGE") == NULL &&
                            find_exact(content(), "DETAIL \xE2\x80\xBA") != NULL &&
                            find_text(content(), "A repeater takes no direct messages") != NULL);
            rift_app_open_detail(app, 1);
            pump(120);
        } else {
            pos_input_focus(app->keysink);
            pos_input_push_key(LV_KEY_ENTER);
            pump(120);
            snprintf(what, sizeof(what), "%s: Enter on a repeater opens nothing", tag);
            check(what, app->section == RIFT_SEC_NODES &&
                            !(app->have_conv && strcmp(app->conv, KEY_D) == 0));
            snprintf(what, sizeof(what), "%s: and the key hint offers no ENTER MESSAGE", tag);
            check(what, find_text(app->cmd_hint, "ENTER MESSAGE") == NULL);
        }
        snprintf(what, sizeof(what), "%s: the detail has no MESSAGE", tag);
        check(what, find_exact(content(), "MESSAGE") == NULL);
        snprintf(what, sizeof(what), "%s: and says it is a repeater, and where it is controlled",
                 tag);
        check(what, find_text(content(), "TAKES NO DIRECT MESSAGES") != NULL &&
                        find_text(content(), "CONTROL FROM ACTIVITY") != NULL);
        snprintf(what, sizeof(what), "%s: it keeps its telemetry, path and node actions", tag);
        check(what, find_text(content(), "RPT") != NULL && find_exact(content(), "RE-ROUTE") &&
                        find_exact(content(), "FORGET") && find_text(content(), "7.5") != NULL);
        snprintf(what, sizeof(what), "%s: its words fit their buttons", tag);
        check(what, labels_overflowing(content()) == 0);
        if (!app->wide) {
            shot("portrait-repeater-detail");
            rift_app_open_detail(app, 0);
        } else {
            shot("landscape-repeater-detail");
        }

        /* An ordinary chat node still can. */
        rift_app_open_conversation(app, KEY_A);
        pump(80);
        snprintf(what, sizeof(what), "%s: a chat node still opens a conversation", tag);
        check(what, app->section == RIFT_SEC_COMMS && app->have_conv &&
                        strcmp(app->conv, KEY_A) == 0);

        /* A thread that was open before its node's type was known: the
         * composer refuses, and nothing is asked of the service. */
        snprintf(app->conv, sizeof(app->conv), "%s", KEY_D);
        app->have_conv = 1;
        rift_app_refresh(app);
        pump(80);
        rift_comms_submit(app, "hei");
        pump(80);
        snprintf(what, sizeof(what), "%s: the composer refuses a repeater, in words", tag);
        check(what, !rift_model_sending(&app->model) && app->model.outbox.failed &&
                        strstr(app->model.outbox.error, "repeater takes no direct messages") &&
                        find_text(content(), "repeater takes no direct messages") != NULL);
        app_stop();
    }
    use_display(POS_ROTATION_0, PANEL_CORNER);
}

static void navigation_session(void)
{
    static const enum pos_rotation shapes[2] = { POS_ROTATION_270, POS_ROTATION_0 };
    static const enum pos_text_size sizes[3] = { POS_TEXT_SIZE_SMALL, POS_TEXT_SIZE_MEDIUM,
                                                 POS_TEXT_SIZE_LARGE };
    enum pos_text_size was = pos_theme_current_text_size();
    int k;
    int z;
    int i;

    for (k = 0; k < 2; k++) {
        int wide = shapes[k] == POS_ROTATION_270;
        const char *tag = wide ? "landscape" : "portrait";
        char what[200];
        lv_area_t s;
        lv_area_t t[RIFT_TAB_COUNT];
        lv_area_t l[RIFT_TAB_COUNT];
        lv_obj_t *probe;
        int tall = 1;
        int faces = 1;
        int gaps = 1;

        use_display(shapes[k], PANEL_CORNER);
        app_start();
        quiet_client();
        give_nodes();
        give_service();
        pump(200);
        lv_obj_get_coords(strip(), &s);
        for (i = 0; i < RIFT_TAB_COUNT; i++) {
            lv_obj_get_coords(tab(i), &t[i]);
            lv_obj_get_coords(app->tab_label[i], &l[i]);
            tall = tall && lv_area_get_height(&t[i]) == RIFT_NAV_FACE_H &&
                   t[i].y1 - s.y1 == (lv_area_get_height(&s) - RIFT_NAV_FACE_H) / 2;
            /* Visible: a filled face with an edge, the same as the next. */
            faces = faces && lv_obj_get_style_bg_opa(tab(i), LV_PART_MAIN) == LV_OPA_COVER &&
                    lv_obj_get_style_border_width(tab(i), LV_PART_MAIN) > 0 &&
                    same_face(tab(i), tab(0));
            if (i > 0) {
                gaps = gaps && t[i].x1 - t[i - 1].x2 - 1 == 8;
            }
        }
        snprintf(what, sizeof(what), "%s: the strip is a %d px row", tag,
                 wide ? RIFT_NAV_ROW_H_TOP : RIFT_NAV_ROW_H);
        check(what, lv_area_get_height(&s) == (wide ? RIFT_NAV_ROW_H_TOP : RIFT_NAV_ROW_H));
        snprintf(what, sizeof(what), "%s: every tab is a %d px face, centred",
                 tag, RIFT_NAV_FACE_H);
        check(what, tall);
        snprintf(what, sizeof(what), "%s: each tab is a visible face, all four alike", tag);
        check(what, faces);
        snprintf(what, sizeof(what), "%s: 8 px between the faces", tag);
        check(what, gaps);
        snprintf(what, sizeof(what), "%s: every word at one height, the active one too", tag);
        check(what, l[1].y1 == l[0].y1 && l[2].y1 == l[0].y1 && l[3].y1 == l[0].y1 &&
                        app->section == RIFT_SEC_ACTIVITY);
        /* The word is in RIFT's button type, the one its actions use, not
         * the caption's: a control, and larger. */
        probe = rift_action(lv_layer_top(), "X", 0, 1, NULL, NULL);
        snprintf(what, sizeof(what), "%s: the tab words are in RIFT's button type, larger than a caption",
                 tag);
        check(what, lv_obj_get_style_text_font(app->tab_label[0], LV_PART_MAIN) ==
                            lv_obj_get_style_text_font(lv_obj_get_child(probe, 0), LV_PART_MAIN) &&
                        lv_font_get_line_height(lv_obj_get_style_text_font(app->tab_label[0], LV_PART_MAIN)) >
                            lv_font_get_line_height(lv_obj_get_style_text_font(app->cmd_hint, LV_PART_MAIN)));
        lv_obj_delete(probe);
        snprintf(what, sizeof(what), "%s: the first face 20 px in", tag);
        check(what, wide || t[0].x1 == s.x1 + RIFT_PAD);
        /* A tap in the gap between two faces lands on the nearer one; the
         * air above and below a face is its target too. */
        tap_at(t[2].x1 - 3, (s.y1 + s.y2) / 2);
        snprintf(what, sizeof(what), "%s: a tap in the gap just before COMMS is COMMS", tag);
        check(what, app->section == RIFT_SEC_COMMS);
        tap_at(t[1].x2 + 3, (s.y1 + s.y2) / 2);
        snprintf(what, sizeof(what), "%s: a tap in the gap just after NODES is NODES", tag);
        check(what, app->section == RIFT_SEC_NODES);
        tap_at((t[RIFT_SEC_SYSTEM].x1 + t[RIFT_SEC_SYSTEM].x2) / 2, t[RIFT_SEC_SYSTEM].y1 - 4);
        snprintf(what, sizeof(what), "%s: SYSTEM answers 4 px above its face", tag);
        check(what, app->section == RIFT_SEC_SYSTEM);
        tap_at((t[RIFT_SEC_MAP].x1 + t[RIFT_SEC_MAP].x2) / 2, (s.y1 + s.y2) / 2);
        snprintf(what, sizeof(what), "%s: and MAP its own", tag);
        check(what, app->section == RIFT_SEC_MAP);
        tap_at((t[0].x1 + t[0].x2) / 2, t[0].y2 + 4);
        snprintf(what, sizeof(what), "%s: and ACTIVITY 4 px below its own", tag);
        check(what, app->section == RIFT_SEC_ACTIVITY);
        if (wide) {
            lv_area_t b;
            int home_before = home_calls;

            lv_obj_get_coords(app->back, &b);
            check("landscape: the back slab is a 72 x 56 face at the strip's left",
                  visible(app->back) && lv_area_get_width(&b) == 72 &&
                      lv_area_get_height(&b) == RIFT_NAV_FACE_H && b.x1 < t[0].x1 &&
                      within(app->back, strip()));
            check("landscape: in the tabs' look", same_face(app->back, tab(1)));
            {
                /* No band above the row (unit B, 2026-10-02): the strip is the
                 * screen's top row, as the shell's header is in every other
                 * app, and the slab is where that header's slab is - 8 px
                 * down, in from the rounded corner by the top bar's inset. */
                struct pos_insets bar =
                    pos_display_bar_insets(pocketui_display_geometry(), POS_EDGE_TOP);
                lv_area_t h;

                lv_obj_get_coords(app->cmd_hint, &h);
                check("landscape: the strip starts at the screen's top edge, no band above it",
                      s.y1 == 0 && lv_obj_get_style_pad_top(frame(), LV_PART_MAIN) == 0);
                check("landscape: Back sits where every app's back slab does: 8 px down",
                      b.y1 == (POCKETUI_HEADER_H - RIFT_NAV_FACE_H) / 2);
                check("landscape: and in from the rounded corner by the top bar's inset",
                      b.x1 == LV_MAX(RIFT_PAD, bar.left) &&
                          h.x2 <= pocketui_display_geometry()->width - LV_MAX(RIFT_PAD, bar.right));
                check("landscape: the sections start under the row, below the corners",
                      lv_obj_get_y(content()) >= RIFT_NAV_ROW_H_TOP && inside_body(content()));
            }
            tap_at((b.x1 + b.x2) / 2, s.y1);
            tap_at((b.x1 + b.x2) / 2, s.y2);
            tap_at(b.x2 + 3, (s.y1 + s.y2) / 2);
            check("landscape: Back answers at the strip's top row, its foot and in the gap after it",
                  home_calls == home_before + 3);
            home_calls = home_before;
            {
                lv_area_t h;

                lv_obj_get_coords(app->cmd_hint, &h);
                check("landscape: the strip's caption ends inside the strip, 20 px in",
                      visible(app->cmd_hint) && h.x2 <= s.x2 - RIFT_PAD);
            }
        } else {
            check("portrait: no back slab of RIFT's: the shell's header has the way back",
                  !visible(app->back));
        }
        /* Every text size: the five fit the row whole, nothing clipped, the
         * content still inside the body. */
        for (z = 0; z < 3; z++) {
            lv_area_t last;
            lv_area_t lab;
            int words = 1;

            pos_theme_select_text_size(sizes[z]);
            pump(300);
            lv_obj_get_coords(strip(), &s);
            lv_obj_get_coords(tab(RIFT_TAB_COUNT - 1), &last);
            for (i = 0; i < RIFT_TAB_COUNT; i++) {
                lv_obj_get_coords(app->tab_label[i], &lab);
                lv_obj_get_coords(tab(i), &t[i]);
                words = words && lab.x1 >= t[i].x1 && lab.x2 <= t[i].x2 &&
                        strcmp(lv_label_get_text(app->tab_label[i]),
                               rift_tab_word(i, app->tab_short)) == 0;
                /* Shortened only where the row is short of room. */
                words = words && (!app->tab_short || (!wide && sizes[z] != POS_TEXT_SIZE_SMALL));
            }
            snprintf(what, sizeof(what), "%s %s: every face inside the row (the last ends %d, row %d)",
                     tag, pos_text_size_name(sizes[z]), (int)last.x2, (int)(s.x2 - RIFT_PAD));
            check(what, last.x2 <= s.x2 - RIFT_PAD &&
                            lv_obj_get_height(strip()) ==
                                (wide ? RIFT_NAV_ROW_H_TOP : RIFT_NAV_ROW_H));
            snprintf(what, sizeof(what), "%s %s: each word whole inside its face", tag,
                     pos_text_size_name(sizes[z]));
            check(what, words && captions_clipped(strip()) == 0 && labels_overflowing(strip()) == 0);
            snprintf(what, sizeof(what), "%s %s: the sections still inside the body", tag,
                     pos_text_size_name(sizes[z]));
            check(what, inside_body(content()) && inside_body(strip()));
            if (!wide) {
                snprintf(what, sizeof(what), "portrait %s: the strip does not scroll sideways",
                         pos_text_size_name(sizes[z]));
                check(what, lv_obj_get_scroll_right(strip()) <= 0 && lv_obj_get_scroll_x(strip()) == 0);
            }
        }
        pos_theme_select_text_size(was);
        pump(200);
        app_stop();
    }
    use_display(POS_ROTATION_0, PANEL_CORNER);
}

/* The real connection, kept: one meshcored client from the first open to
 * CLOSE RIFT, however often RIFT is left and reopened, taking the mesh in
 * while it is left - and meshcored itself untouched by the close. */
static int bg_live_heard(void)
{
    struct rift_app *s = rift_app_session();
    int i;

    for (i = 0; s && i < s->model.msg_count; i++) {
        if (strcmp(s->model.msg[i].text, "heard while left") == 0) {
            return 1;
        }
    }
    return 0;
}

static int count_lines(const char *path, const char *line)
{
    char buf[256];
    FILE *f = fopen(path, "r");
    int n = 0;

    while (f && fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = '\0';
        n += strcmp(buf, line) == 0;
    }
    if (f) {
        fclose(f);
    }
    return n;
}

static void background_live_session(void)
{
    const char *bin = getenv("RIFT_FAKE_MESHCORED");
    const char *run = getenv("POCKETOS_RUNTIME_DIR");
    char sock[512];
    char methods[512];
    char events[512];
    struct rift_app *s;
    struct stat st;
    int waited;
    int round;
    int home_before;
    pid_t pid;
    FILE *f;

    if (!bin || !run || access(bin, X_OK) != 0) {
        printf("     (the live background session needs RIFT_FAKE_MESHCORED and "
               "POCKETOS_RUNTIME_DIR; not run)\n");
        return;
    }
    snprintf(sock, sizeof(sock), "%s/meshcored.sock", run);
    snprintf(methods, sizeof(methods), "%s/rift-bg-methods", g_state_dir);
    snprintf(events, sizeof(events), "%s/rift-bg-events", g_state_dir);
    unlink(methods);
    f = fopen(events, "w");
    if (f) {
        fprintf(f, "mesh.message|{\"message\":{\"id\":7,\"direction\":\"in\","
                   "\"peer_public_key\":\"" KEY_A "\",\"text\":\"heard while left\","
                   "\"state\":\"received\",\"timestamp\":1700000000,\"mono_ms\":-500}}\n");
        fclose(f);
    }
    pid = fork();
    if (pid == 0) {
        setenv("FAKE_MESHCORED_STATE", "online", 1);
        setenv("FAKE_MESHCORED_REASON", "receiving", 1);
        setenv("FAKE_MESHCORED_NODES", "[]", 1);
        setenv("FAKE_MESHCORED_METHODS", methods, 1);
        setenv("FAKE_MESHCORED_EVENTS", events, 1);
        setenv("FAKE_MESHCORED_EVENTS_AFTER_SNAPSHOT", "1", 1);
        setenv("FAKE_MESHCORED_LIFE_MS", "120000", 1);
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }
    for (waited = 0; waited < 5000 && stat(sock, &st) != 0; waited += 20) {
        usleep(20000);
    }
    check("live: the scripted service is up", pid > 0 && stat(sock, &st) == 0);

    /* Opened and left at once: what the service says next arrives while
     * RIFT has no screen. */
    app_start();
    s = app;
    check("live: RIFT connects", rift_ipc_connected(&app->ipc));
    app_leave();
    check("live: left, the session and its connection stay",
          rift_app_in_background() && rift_ipc_connected(&s->ipc));
    check("live: a direct message sent while RIFT is left reaches it",
          live_until(bg_live_heard, 8000));
    for (round = 0; round < 5; round++) {
        app_start();
        pump(100);
        app_leave();
    }
    app_start();
    pump(200);
    check("live: reopened six times, the same session and still connected",
          app == s && app->opens == 7 && rift_ipc_connected(&app->ipc));
    check("live: one connection, one subscription, for all of it",
          count_lines(methods, "mesh.subscribe") == 1);
    check("live: nothing was put on the air",
          count_lines(methods, "mesh.send") == 0 && count_lines(methods, "mesh.advert") == 0);

    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(120);
    home_before = home_calls;
    tap(rift_session_close_button(app));
    pump(120);
    tap(rift_session_confirm_button(app, 1));
    app_leave();
    home_calls = home_before;
    pump(300);
    check("live: CLOSE RIFT gives the subscription back",
          count_lines(methods, "mesh.unsubscribe") == 1);
    check("live: and the session is over", rift_app_session() == NULL);
    check("live: meshcored is still running after it", kill(pid, 0) == 0);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    unlink(methods);
    unlink(events);
}

/* ---- the composer's emoji picker -------------------------------------------- */

static void tap_point(int32_t x, int32_t y)
{
    finger_point.x = x;
    finger_point.y = y;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(80);
}

/* The button beside a composer's field: the next object in its row. */
static lv_obj_t *emoji_button_of(lv_obj_t *field)
{
    lv_obj_t *wrap = field ? lv_obj_get_parent(field) : NULL;

    return wrap ? lv_obj_get_child(lv_obj_get_parent(wrap), (int32_t)lv_obj_get_index(wrap) + 1)
                : NULL;
}

/* Nothing went towards the service: no request on its way and no refusal. */
static int nothing_sent(void)
{
    return !rift_model_sending(&app->model) && !app->model.outbox.failed;
}

static int stored_recent_first(const char *emoji)
{
    struct rift_prefs p;
    size_t n = strlen(emoji);

    return rift_store_load(&p) == 0 && strncmp(p.emoji_recent, emoji, n) == 0 &&
           (p.emoji_recent[n] == '\0' || p.emoji_recent[n] == ' ');
}

/* An emoji button at the right of the composer, in both shapes, at Large:
 * its picker inserts at the caret and sends nothing, keeps the field's
 * focus, and closes on Esc, on a tap outside and on a key it does not use;
 * what was picked comes first next time and is kept in the preferences. */
static void emoji_picker_session(void)
{
    static const enum pos_rotation turns[] = { POS_ROTATION_0, POS_ROTATION_270 };
    enum pos_text_size was = pos_theme_current_text_size();
    size_t t;

    pos_theme_select_text_size(POS_TEXT_SIZE_LARGE);
    for (t = 0; t < sizeof(turns) / sizeof(turns[0]); t++) {
        const char *shape = turns[t] == POS_ROTATION_0 ? "portrait" : "landscape";
        char what[200];
        char want[64];
        const char *picked;
        lv_obj_t *field;
        lv_obj_t *button;
        lv_area_t fa;
        lv_area_t ba;
        lv_area_t box;

        use_display(turns[t], PANEL_CORNER);
        app_start();
        quiet_client();
        give_nodes();
        give_service();
        give_messages();
        rift_app_show_section(app, RIFT_SEC_COMMS);
        pump(200);
        tap(ancestor(find_exact(content(), "HYTTA"), 1));
        pump(200);
        field = app->wide ? app->composer : rift_comms_field(app);
        button = emoji_button_of(field);
        snprintf(what, sizeof(what), "%s, Large: the composer has an emoji button", shape);
        check(what, field && button && visible(button) && (!app->wide || button == app->composer_emoji));
        if (!field || !button) {
            app_stop();
            continue;
        }
        lv_obj_get_coords(lv_obj_get_parent(field), &fa);
        lv_obj_get_coords(button, &ba);
        snprintf(what, sizeof(what), "%s, Large: at the right of the field, on its line", shape);
        check(what, ba.x1 >= fa.x2 && ba.y1 < fa.y2 && ba.y2 > fa.y1 && inside_body(button));
        snprintf(what, sizeof(what), "%s, Large: and it does not take the focus group's place",
                 shape);
        check(what, lv_obj_get_group(button) == NULL &&
                        !lv_obj_has_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE));

        pos_input_push_key('h');
        pos_input_push_key('i');
        pump(80);
        lv_textarea_set_cursor_pos(field, 1);
        tap(button);
        snprintf(what, sizeof(what), "%s, Large: a tap opens the picker on the common emoji",
                 shape);
        check(what, rift_emoji_picker_is_open() && rift_emoji_picker_group() == 0 &&
                        rift_emoji_picker_cell(RIFT_EMOJI_CELLS - 1) != NULL);
        snprintf(what, sizeof(what), "%s, Large: and the field keeps the keys and its caret",
                 shape);
        check(what, pos_input_focused() == field && lv_textarea_get_cursor_pos(field) == 1);
        lv_obj_update_layout(rift_emoji_picker_box());
        lv_obj_get_coords(rift_emoji_picker_box(), &box);
        snprintf(what, sizeof(what), "%s, Large: the picker is on screen, above the button",
                 shape);
        check(what, box.x1 >= 0 && box.y1 >= 0 && box.x2 < lv_display_get_horizontal_resolution(NULL) &&
                        box.y2 < ba.y1 && labels_overflowing(rift_emoji_picker_box()) == 0);

        /* The keys move the selection and change the group, and none of
         * them reaches the field. */
        pos_input_push_key(LV_KEY_RIGHT);
        pos_input_push_key(LV_KEY_DOWN);
        pump(80);
        snprintf(what, sizeof(what), "%s, Large: the arrows move the selection, not the caret",
                 shape);
        check(what, rift_emoji_picker_selected() == RIFT_EMOJI_COLS + 1 &&
                        lv_textarea_get_cursor_pos(field) == 1 &&
                        strcmp(lv_textarea_get_text(field), "hi") == 0);
        pos_input_push_key(LV_KEY_DOWN);
        pos_input_push_key(LV_KEY_DOWN);
        pump(80);
        snprintf(what, sizeof(what), "%s, Large: down past the last row is the next group",
                 shape);
        check(what, rift_emoji_picker_group() == 1 && rift_emoji_picker_selected() == 1 &&
                        strcmp(lv_textarea_get_text(field), "hi") == 0);
        snprintf(what, sizeof(what), "%s, Large: the group's name is whole", shape);
        check(what, captions_clipped(rift_emoji_picker_box()) == 0);
        pos_input_push_key(LV_KEY_UP);
        pump(80);
        snprintf(what, sizeof(what), "%s, Large: and up past the first row the one before",
                 shape);
        check(what, rift_emoji_picker_group() == 0 &&
                        rift_emoji_picker_selected() == 2 * RIFT_EMOJI_COLS + 1);
        tap(rift_emoji_picker_tab(2));
        snprintf(what, sizeof(what), "%s, Large: a tab shows its group", shape);
        check(what, rift_emoji_picker_is_open() && rift_emoji_picker_group() == 2 &&
                        pos_input_focused() == field);
        pos_input_push_key(LV_KEY_ESC);
        pump(80);
        snprintf(what, sizeof(what), "%s, Large: Esc closes it and leaves the message as it was",
                 shape);
        check(what, !rift_emoji_picker_is_open() && strcmp(lv_textarea_get_text(field), "hi") == 0 &&
                        pos_input_focused() == field && app->section == RIFT_SEC_COMMS);

        /* A tap on an emoji: in at the caret, nothing sent. */
        tap(button);
        picked = rift_emoji_picker_item(3);
        tap(rift_emoji_picker_cell(3));
        snprintf(want, sizeof(want), "h%si", picked ? picked : "?");
        snprintf(what, sizeof(what), "%s, Large: a tapped emoji goes in at the caret", shape);
        check(what, picked && strcmp(lv_textarea_get_text(field), want) == 0 &&
                        !rift_emoji_picker_is_open());
        snprintf(what, sizeof(what), "%s, Large: and nothing is sent", shape);
        check(what, nothing_sent());
        snprintf(what, sizeof(what), "%s, Large: typing goes on after it", shape);
        pos_input_push_key('!');
        pump(80);
        snprintf(want, sizeof(want), "h%s!i", picked ? picked : "?");
        check(what, pos_input_focused() == field && strcmp(lv_textarea_get_text(field), want) == 0);
        snprintf(what, sizeof(what), "%s, Large: it is kept as the most recent emoji", shape);
        check(what, picked && stored_recent_first(picked));

        /* Opened again: the recent one first, and Enter picks the
         * selection rather than sending the message. */
        lv_textarea_set_text(field, "");
        tap(button);
        snprintf(what, sizeof(what), "%s, Large: the recent emoji comes first next time", shape);
        check(what, picked && rift_emoji_picker_item(0) == rift_emoji_pick_find(picked));
        pos_input_push_key(LV_KEY_ENTER);
        pump(120);
        snprintf(what, sizeof(what), "%s, Large: Enter inserts the selected emoji, and sends nothing",
                 shape);
        check(what, picked && strcmp(lv_textarea_get_text(field), picked) == 0 &&
                        !rift_emoji_picker_is_open() && nothing_sent());

        /* A tap outside, and a key the picker has no use for. */
        tap(button);
        tap_point(box.x2 + 40 < lv_display_get_horizontal_resolution(NULL) ? box.x2 + 20 : 4, 4);
        snprintf(what, sizeof(what), "%s, Large: a tap outside closes it", shape);
        check(what, !rift_emoji_picker_is_open() && pos_input_focused() == field);
        tap(button);
        pos_input_push_key('k');
        pump(80);
        snprintf(want, sizeof(want), "%sk", picked ? picked : "?");
        snprintf(what, sizeof(what), "%s, Large: a letter closes it and is typed", shape);
        check(what, !rift_emoji_picker_is_open() && strcmp(lv_textarea_get_text(field), want) == 0);
        tap(button);
        pos_input_push_key(LV_KEY_NEXT);
        pump(120);
        snprintf(what, sizeof(what), "%s, Large: Tab takes the keys away, and closes it", shape);
        check(what, !rift_emoji_picker_is_open() && strcmp(lv_textarea_get_text(field), want) == 0);

        tap(button);
        app_stop();
        snprintf(what, sizeof(what), "%s: leaving RIFT takes the picker with it", shape);
        check(what, !rift_emoji_picker_is_open());
    }
    pos_theme_select_text_size(was);
}

/* ---- feat/rift-repeater-control ---------------------------------------------
 *
 * ACTIVITY's REPEATERS 0-HOP panel and a repeater's page, from fixtures (the
 * screens, in both shapes and at Large, and the keys), then against the
 * scripted service (what each press asks for, and what leaving RIFT does). */

#define KEY_RPT1 "f7a00000000000000000000000000000000000000000000000000000000000f7"
#define KEY_RPT2 "a7b10000000000000000000000000000000000000000000000000000000000a7"

static void rep_apply(const char *name, const char *json)
{
    cJSON *o = cJSON_Parse(json);

    check("the repeater fixture is valid JSON", o != NULL);
    rift_rep_apply_event(&app->model.repeater, name, o, rift_mono_ms());
    cJSON_Delete(o);
}

/* Three repeaters: the node-list repeater KEY_D and one whose advert was never
 * heard, both answering the latest round; and one from an earlier round. */
static void give_repeaters(int open)
{
    char json[1024];
    int64_t now = rift_mono_ms();

    snprintf(json, sizeof(json),
             "{\"reason\":\"reply\",\"round\":1,\"open\":false,\"repeater\":{"
             "\"public_key\":\"" KEY_RPT2 "\",\"node_hash\":\"a7\",\"name\":\"BERG-RPT\","
             "\"known\":true,\"round\":1,\"current\":true,\"mono_ms\":%lld,"
             "\"their_snr_db\":-2.5,\"snr_db\":-4.0,\"rssi_dbm\":-109}}",
             (long long)(now - 1800000));
    rep_apply("mesh.discover", json);
    {
        cJSON *o;

        snprintf(json, sizeof(json),
                 "{\"started\":true,\"round\":2,\"open\":%s,\"started_mono_ms\":%lld,"
                 "\"until_mono_ms\":%lld}",
                 open ? "true" : "false", (long long)(now - 12000),
                 (long long)(now + (open ? 18000 : -1000)));
        o = cJSON_Parse(json);
        rift_rep_apply_discover(&app->model.repeater, o);
        cJSON_Delete(o);
    }
    snprintf(json, sizeof(json),
             "{\"reason\":\"reply\",\"round\":2,\"open\":%s,\"repeater\":{"
             "\"public_key\":\"" KEY_D "\",\"node_hash\":\"d4\","
             "\"name\":\"S\xC3\xB8rlandet fjellstasjon \xC3\xA6\xC3\xB8\xC3\xA5 relay\","
             "\"known\":true,\"type\":2,\"round\":2,\"current\":true,\"mono_ms\":%lld,"
             "\"their_snr_db\":5.5,\"snr_db\":8.25,\"rssi_dbm\":-71}}",
             open ? "true" : "false", (long long)(now - 9000));
    rep_apply("mesh.discover", json);
    snprintf(json, sizeof(json),
             "{\"reason\":\"reply\",\"round\":2,\"open\":%s,\"repeater\":{"
             "\"public_key\":\"" KEY_RPT1 "\",\"node_hash\":\"f7\",\"known\":false,"
             "\"round\":2,\"current\":true,\"mono_ms\":%lld,\"their_snr_db\":1.0,"
             "\"snr_db\":-1.5,\"rssi_dbm\":-98}}",
             open ? "true" : "false", (long long)(now - 7000));
    rep_apply("mesh.discover", json);
    rift_app_refresh(app);
    pump(120);
}

#define REP_FIX_SESSION                                                                       \
    "\"session\":{\"active\":true,\"node\":\"" KEY_D "\",\"known\":true,\"type\":2,"           \
    "\"login\":\"ok\",\"legacy\":false,\"admin\":true,\"permissions\":1,\"acl\":3,"           \
    "\"firmware_level\":2,\"repeater_clock\":1790000000,\"stale_replies\":0,"                 \
    "\"malformed_replies\":0}"

/* Logged in to KEY_D, and what it answered: status, neighbours, version and
 * two commands. */
static void give_repeater_session(void)
{
    rep_apply("mesh.remote", "{\"reply\":{\"request_id\":1,\"kind\":\"login\",\"outcome\":"
                             "\"replied\",\"node\":\"" KEY_D "\"}," REP_FIX_SESSION "}");
    rep_apply("mesh.remote",
              "{\"reply\":{\"request_id\":2,\"kind\":\"status\",\"outcome\":\"replied\","
              "\"node\":\"" KEY_D "\",\"status\":{\"battery_mv\":4012,\"tx_queue\":0,"
              "\"noise_floor_dbm\":-118,\"last_rssi_dbm\":-81,\"last_snr_db\":6.5,"
              "\"packets_recv\":15532,\"packets_sent\":4410,\"air_time_s\":3605,"
              "\"uptime_s\":360500,\"sent_flood\":4000,\"sent_direct\":410,"
              "\"recv_flood\":15000,\"recv_direct\":532,\"err_events\":0,"
              "\"direct_dups\":3,\"flood_dups\":211}}," REP_FIX_SESSION "}");
    rep_apply("mesh.remote",
              "{\"reply\":{\"request_id\":3,\"kind\":\"neighbours\",\"outcome\":\"replied\","
              "\"node\":\"" KEY_D "\",\"neighbours\":{\"total\":2,\"entries\":["
              "{\"prefix\":\"a19ac21e7d04\",\"name\":\"OSLO-01\",\"heard_s_ago\":95,"
              "\"snr_db\":9.5},{\"prefix\":\"77aa00bb11cc\",\"heard_s_ago\":3700,"
              "\"snr_db\":-4.25}]}}," REP_FIX_SESSION "}");
    rep_apply("mesh.remote",
              "{\"reply\":{\"request_id\":4,\"kind\":\"owner\",\"outcome\":\"replied\","
              "\"node\":\"" KEY_D "\",\"owner\":{\"firmware\":\"v1.9.0 (Build: 12-Sep-2026)\","
              "\"name\":\"Sorlandet\",\"owner\":\"bench\"}}," REP_FIX_SESSION "}");
    rift_rep_note_command(&app->model.repeater, "ver");
    rep_apply("mesh.remote", "{\"reply\":{\"request_id\":5,\"kind\":\"cli\",\"outcome\":"
                             "\"replied\",\"node\":\"" KEY_D "\",\"text\":\"v1.9.0 (Build: "
                             "12-Sep-2026)\"}," REP_FIX_SESSION "}");
    rift_app_refresh(app);
    pump(150);
}

static int rep_live_listed(void)
{
    return app && app->model.repeater.scan.count >= 1 && !app->model.repeater.scan.asking;
}

static int rep_live_logged(void)
{
    return app && rift_rep_logged_in(&app->model.repeater, KEY_D);
}

static int rep_live_idle(void)
{
    return app && !rift_rep_busy(&app->model.repeater);
}

static int rep_live_status(void)
{
    return app && app->model.repeater.status.have && !rift_rep_busy(&app->model.repeater);
}

static int rep_live_lines(void)
{
    return app && app->model.repeater.line_count >= 3 && !rift_rep_busy(&app->model.repeater);
}

static int rep_log_count(const char *path, const char *prefix)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int n = 0;

    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        n += strncmp(line, prefix, strlen(prefix)) == 0;
    }
    fclose(f);
    return n;
}

static void repeater_screens(enum pos_rotation shape, enum pos_text_size size)
{
    const char *tag = shape == POS_ROTATION_0 ? "portrait" : "landscape";
    int large = size == POS_TEXT_SIZE_LARGE;
    char what[200];
    char name[96];
    int rows = 0;
    int i;

    pos_theme_select_text_size(size);
    use_display(shape, PANEL_CORNER);
    app_start();
    quiet_client();
    give_nodes();
    give_service();
    give_repeaters(0);
    rift_app_show_section(app, RIFT_SEC_ACTIVITY);
    pump(200);
    for (i = 0; i < RIFT_SCAN_ROWS; i++) {
        rows += rift_scan_row(app, i) && visible(rift_scan_row(app, i));
    }
    snprintf(what, sizeof(what), "%s%s: ACTIVITY lists the three repeaters, the earlier apart",
             tag, large ? ", Large" : "");
    check(what, find_exact(content(), "REPEATERS 0-HOP") && rows == 3 &&
                    visible(find_exact(content(), "EARLIER SCANS")));
    snprintf(what, sizeof(what), "%s%s: a repeater whose advert was never heard says so", tag,
             large ? ", Large" : "");
    check(what, find_text(content(), large ? "f7a00000" : "NO ADVERT") != NULL);
    snprintf(what, sizeof(what), "%s%s: each row is a 56 px target", tag, large ? ", Large" : "");
    check(what, lv_obj_get_height(rift_scan_row(app, 0)) >= RIFT_TOUCH_H);
    snprintf(what, sizeof(what), "%s%s: ACTIVITY's captions are whole, buttons unclipped", tag,
             large ? ", Large" : "");
    check(what, captions_clipped(frame()) == 0 && labels_overflowing(frame()) == 0);
    snprintf(name, sizeof(name), "rift-activity-repeaters-%s%s", tag, large ? "-large" : "");
    shot(name);

    /* Newest answer first: the unnamed repeater (7 s), then KEY_D (9 s). */
    tap(rift_scan_row(app, 1));
    pump(150);
    snprintf(what, sizeof(what), "%s%s: a row opens its repeater's page, under ACTIVITY", tag,
             large ? ", Large" : "");
    check(what, app->section == RIFT_SEC_REPEATER && rift_tab_of(app->section) == RIFT_SEC_ACTIVITY &&
                    app->model.repeater.have_target &&
                    strcmp(app->model.repeater.target, KEY_D) == 0);
    snprintf(what, sizeof(what), "%s%s: not logged in, nothing can be read or sent", tag,
             large ? ", Large" : "");
    check(what, find_text(content(), "Not logged in") != NULL &&
                    lv_obj_has_state(rift_repeater_view_part(app, RIFT_REPV_STATUS),
                                     LV_STATE_DISABLED) &&
                    lv_obj_has_state(rift_repeater_view_part(app, RIFT_REPV_SEND),
                                     LV_STATE_DISABLED));
    snprintf(name, sizeof(name), "rift-repeater-login-%s%s", tag, large ? "-large" : "");
    shot(name);

    give_repeater_session();
    snprintf(what, sizeof(what), "%s%s: logged in as admin, in words", tag, large ? ", Large" : "");
    check(what, find_text(content(), "Logged in as admin") != NULL &&
                    !visible(rift_repeater_view_part(app, RIFT_REPV_PASSWORD)));
    snprintf(what, sizeof(what), "%s%s: what the repeater answered is shown, and only that", tag,
             large ? ", Large" : "");
    check(what, find_text(content(), "4.01 V") != NULL);
    check("  and its neighbours, named where the node list names them",
          find_text(content(), "OSLO-01") != NULL && find_text(content(), "77aa00bb11cc") != NULL);
    check("  and its firmware", find_text(content(), "v1.9.0 (Build: 12-Sep-2026)") != NULL);
    {
        lv_obj_t *rx_time = find_exact(content(), "Receive time");

        /* visible(NULL) is 1: a row nobody can find is a hidden one. */
        check("  and not a tier it did not send", rx_time == NULL || !visible(rx_time));
    }
    snprintf(what, sizeof(what), "%s%s: the page fits its body, buttons unclipped", tag,
             large ? ", Large" : "");
    check(what, inside_body(content()) && labels_overflowing(app->repeater_root) == 0);
    lv_obj_scroll_to_y(app->repeater_root, 0, LV_ANIM_OFF);
    snprintf(name, sizeof(name), "rift-repeater-%s%s", tag, large ? "-large" : "");
    shot(name);

    /* The keys: Down walks the page's controls with the focus outline, Enter
     * presses; the first is the way back. */
    pos_input_focus(app->keysink);
    pump(40);
    pos_input_push_key(LV_KEY_DOWN);
    pump(60);
    snprintf(what, sizeof(what), "%s%s: Down puts the outline on the way back", tag,
             large ? ", Large" : "");
    check(what, lv_obj_has_state(rift_repeater_view_part(app, RIFT_REPV_BACK), LV_STATE_USER_1));
    pos_input_push_key(LV_KEY_ENTER);
    pump(120);
    snprintf(what, sizeof(what), "%s%s: and Enter takes it, to ACTIVITY", tag, large ? ", Large" : "");
    check(what, app->section == RIFT_SEC_ACTIVITY);
    pos_input_push_key(LV_KEY_DOWN);
    pump(60);
    pos_input_push_key(LV_KEY_DOWN);
    pump(60);
    pos_input_push_key(LV_KEY_ENTER);
    pump(120);
    snprintf(what, sizeof(what), "%s%s: on ACTIVITY Down and Enter open a repeater too", tag,
             large ? ", Large" : "");
    check(what, app->section == RIFT_SEC_REPEATER);
    app_stop();
}

static void repeater_control_session(void)
{
    const char *bin = getenv("RIFT_FAKE_MESHCORED");
    const char *run = getenv("POCKETOS_RUNTIME_DIR");
    enum pos_text_size was = pos_theme_current_text_size();
    char sock[512];
    char rlog[512];
    struct stat st;
    int waited;
    pid_t pid;

    repeater_screens(POS_ROTATION_0, POS_TEXT_SIZE_SMALL);
    repeater_screens(POS_ROTATION_270, POS_TEXT_SIZE_SMALL);
    repeater_screens(POS_ROTATION_0, POS_TEXT_SIZE_LARGE);
    repeater_screens(POS_ROTATION_270, POS_TEXT_SIZE_LARGE);
    pos_theme_select_text_size(was);
    use_display(POS_ROTATION_0, PANEL_CORNER);

    /* A round still open: SCAN says so and takes no press. */
    app_start();
    quiet_client();
    give_nodes();
    give_service();
    give_repeaters(1);
    rift_app_show_section(app, RIFT_SEC_ACTIVITY);
    pump(150);
    check("while a round is open SCAN says so and is not pressable",
          find_exact(content(), "SCANNING") != NULL &&
              lv_obj_has_state(rift_scan_button(app), LV_STATE_DISABLED));
    shot("rift-activity-repeaters-scanning");
    app_stop();

    if (!bin || !run || access(bin, X_OK) != 0) {
        printf("     (the live repeater session needs RIFT_FAKE_MESHCORED; not run)\n");
        return;
    }
    snprintf(sock, sizeof(sock), "%s/meshcored.sock", run);
    snprintf(rlog, sizeof(rlog), "%s/rift-remote-log", g_state_dir);
    unlink(rlog);
    pid = fork();
    if (pid == 0) {
        char nodes[1024];

        snprintf(nodes, sizeof(nodes),
                 "[{\"public_key\":\"" KEY_D "\",\"node_hash\":\"d4\",\"name\":\"RPT-D\","
                 "\"type\":2,\"path_known\":false,\"last_heard_mono_ms\":-5000}]");
        setenv("FAKE_MESHCORED_STATE", "online", 1);
        setenv("FAKE_MESHCORED_REASON", "receiving", 1);
        setenv("FAKE_MESHCORED_NODES", nodes, 1);
        setenv("FAKE_MESHCORED_REPEATER",
               "{\"public_key\":\"" KEY_D "\",\"node_hash\":\"d4\",\"name\":\"RPT-D\","
               "\"known\":true,\"type\":2,\"their_snr_db\":5.5,\"snr_db\":8.25,"
               "\"rssi_dbm\":-71}",
               1);
        setenv("FAKE_MESHCORED_REMOTE_LOG", rlog, 1);
        setenv("FAKE_MESHCORED_CLI_TIMEOUT_FIRST", "1", 1);
        setenv("FAKE_MESHCORED_LIFE_MS", "120000", 1);
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }
    for (waited = 0; waited < 5000 && stat(sock, &st) != 0; waited += 20) {
        usleep(20000);
    }
    check("the scripted service is up", pid > 0 && stat(sock, &st) == 0);

    app_start();
    rift_app_show_section(app, RIFT_SEC_ACTIVITY);
    check("RIFT reads the service", live_until(live_ready, 8000));
    check("opening RIFT asked no repeater anything that transmits",
          rep_log_count(rlog, "mesh.discover|") == 0 &&
              rep_log_count(rlog, "mesh.remote_login|") == 0);
    tap(rift_scan_button(app));
    check("SCAN 0-HOP asks the service once, and the repeater that answered is listed",
          live_until(rep_live_listed, 8000) && rep_log_count(rlog, "mesh.discover|") == 1 &&
              visible(rift_scan_row(app, 0)));
    tap(rift_scan_row(app, 0));
    pump(150);
    check("its page opens", app->section == RIFT_SEC_REPEATER);

    /* A wrong password: the service reports the wait running out. */
    type_into(rift_repeater_view_part(app, RIFT_REPV_PASSWORD), "nope");
    tap(rift_repeater_view_part(app, RIFT_REPV_LOGIN));
    check("a wrong password ends as no answer, and the field is empty",
          live_until(rep_live_idle, 8000) && app->model.repeater.login == RIFT_REP_LOGIN_TIMEOUT &&
              strcmp(lv_textarea_get_text(rift_repeater_view_part(app, RIFT_REPV_PASSWORD)), "") ==
                  0 &&
              find_text(content(), "wrong password") != NULL);
    /* The keys reach the field: Down to the way back, Down to the field,
     * Enter puts the keys in it (on LVGL's next pass). */
    pos_input_focus(app->keysink);
    pump(40);
    pos_input_push_key(LV_KEY_DOWN);
    pump(40);
    pos_input_push_key(LV_KEY_DOWN);
    pump(40);
    pos_input_push_key(LV_KEY_ENTER);
    pump(120);
    check("Down, Down, Enter puts the keys in the password field",
          pos_input_focused() == rift_repeater_view_part(app, RIFT_REPV_PASSWORD));
    type_into(rift_repeater_view_part(app, RIFT_REPV_PASSWORD), "hunter2");
    pos_input_push_key(LV_KEY_ENTER); /* Enter in the field is LOGIN */
    check("Enter in the field logs in, and the password is kept nowhere in RIFT",
          live_until(rep_live_logged, 8000) && !mem_holds(app, sizeof(*app), "hunter2") &&
              strcmp(lv_textarea_get_text(rift_repeater_view_part(app, RIFT_REPV_PASSWORD)), "") ==
                  0);
    pump(150);
    tap(rift_repeater_view_part(app, RIFT_REPV_STATUS));
    check("STATUS is answered", live_until(rep_live_status, 8000) &&
                                     find_text(content(), "4.01 V") != NULL);
    /* CLOCK first, and its answer lost: the service ends it as a timeout.
     * (Unit B, 2026-10-05: every button stayed grey after CLOCK.) */
    tap(rift_repeater_view_part(app, RIFT_REPV_QUICK_CLOCK));
    check("CLOCK with no answer ends as no answer in time, in the transcript too",
          live_until(rep_live_idle, 8000) && rep_log_count(rlog, "mesh.remote_cli|") == 1 &&
              find_text(content(), "NO ANSWER IN TIME") != NULL &&
              find_text(content(), "(no answer)") != NULL);
    pump(150);
    check("and every repeater button is usable again",
          !lv_obj_has_state(rift_repeater_view_part(app, RIFT_REPV_QUICK_CLOCK),
                            LV_STATE_DISABLED) &&
              !lv_obj_has_state(rift_repeater_view_part(app, RIFT_REPV_QUICK_VER),
                                LV_STATE_DISABLED) &&
              !lv_obj_has_state(rift_repeater_view_part(app, RIFT_REPV_STATUS),
                                LV_STATE_DISABLED) &&
              !lv_obj_has_state(rift_repeater_view_part(app, RIFT_REPV_SEND), LV_STATE_DISABLED));
    tap(rift_repeater_view_part(app, RIFT_REPV_QUICK_VER));
    check("VER is a read-only command, sent at once, and its answer shown",
          live_until(rep_live_lines, 8000) && rep_log_count(rlog, "mesh.remote_cli|") == 2 &&
              find_text(content(), "-> ver") != NULL);

    type_into(rift_repeater_view_part(app, RIFT_REPV_COMMAND), "reboot");
    tap(rift_repeater_view_part(app, RIFT_REPV_SEND));
    pump(150);
    check("reboot asks first, and nothing has been sent",
          visible(rift_repeater_view_part(app, RIFT_REPV_CONFIRM)) &&
              rep_log_count(rlog, "mesh.remote_cli|") == 2);
    tap(rift_repeater_view_part(app, RIFT_REPV_CANCEL));
    pump(150);
    check("CANCEL sends nothing", !visible(rift_repeater_view_part(app, RIFT_REPV_CONFIRM)) &&
                                      rep_log_count(rlog, "mesh.remote_cli|") == 2);
    lv_textarea_set_text(rift_repeater_view_part(app, RIFT_REPV_COMMAND), "");
    type_into(rift_repeater_view_part(app, RIFT_REPV_COMMAND), "erase");
    tap(rift_repeater_view_part(app, RIFT_REPV_SEND));
    pump(150);
    check("erase is refused and never sent", find_text(content(), "Not sent") != NULL &&
                                                  rep_log_count(rlog, "mesh.remote_cli|") == 2);
    shot("rift-repeater-live");

    app_leave();
    pump(300);
    check("leaving RIFT ends the repeater session",
          rep_log_count(rlog, "mesh.remote_logout|") >= 2 &&
              !rift_rep_logged_in(&rift_app_session()->model.repeater, KEY_D));
    app_start();
    app_stop();
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    unlink(sock);
    unlink(rlog);
}

/* ---- feat/rift-comms-reliability ---------------------------------------------
 *
 * Holding a message opens what can be done with it - RESEND for one that went
 * unacknowledged, REPLY for a channel line, COPY for any - and the same by
 * key; a reply is drawn as one; CONTACTS lists the stored contacts apart from
 * NODES, searched, with repeaters still repeaters. */

/* A finger held on an object for longer than LVGL's long press. */
static void hold(lv_obj_t *obj)
{
    lv_area_t a;

    if (!obj) {
        return;
    }
    lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);
    pump(40);
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(700);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(80);
}

static void key(uint32_t k)
{
    pos_input_push_key(k);
    pump(60);
}

/* What the app wrote on a socket it believes is meshcored's: the request
 * a press made, read off the far end. */
static int wire_holds(int fd, const char *want)
{
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);

    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    return mem_holds(buf, (size_t)n, want);
}

static void reliability_session(void)
{
    struct rift_msgact *act;
    lv_obj_t *field;
    enum pos_text_size was = pos_theme_current_text_size();
    int pass;
    int kb_before = kb_shows;

    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_start();
    quiet_client();
    give_nodes();
    give_service();
    give_messages();
    give_channels();
    give_site_line(60, "in", "Anna", "Are you coming up? \xF0\x9F\x91\x8D");
    give_site_line(61, "in", "OSLO-01", "@[Anna] \\\"Are you coming up?\\\" Ja, om ti minutter");
    rift_app_open_conversation(app, KEY_B);
    pump(160);
    act = rift_comms_actions(app);
    check("the thread has message actions, closed", act && rift_msgact_id(act) == 0);

    /* ---- RESEND: held, pressed, and on the wire as the service wants ---- */
    hold(find_exact(thread_pane(), "Pr\xC3\xB8ver direct"));
    check("holding a no-ACK message opens its actions on it", rift_msgact_id(act) == 3);
    check("with RESEND and COPY, and no REPLY in a direct thread",
          rift_msgact_button(act, RIFT_MSGACT_RESEND) &&
              visible(rift_msgact_button(act, RIFT_MSGACT_RESEND)) &&
              rift_msgact_button(act, RIFT_MSGACT_COPY) &&
              !rift_msgact_button(act, RIFT_MSGACT_REPLY));
    check("and the bar says which message, and that it was not acknowledged",
          find_text(thread_pane(), "NO ACK \xC2\xB7 you: Pr\xC3\xB8ver direct") != NULL);
    shot("portrait-comms-message-actions");
    {
        int sv[2];

        check("a socket stands in for meshcored", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        /* Non-blocking, as the client's own connection is
         * (pocketipc_connect_timeout): its reader polls it. */
        fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
        app->ipc.fd = sv[0];
        tap(rift_msgact_button(act, RIFT_MSGACT_RESEND));
        check("RESEND asks the service to resend that message by its id",
              wire_holds(sv[1], "\"resend\":3"));
        check("and the bar closes", rift_msgact_id(act) == 0);
        check("the submission is in flight, as any send", rift_model_sending(&app->model));
        quiet_client();
        close(sv[1]);
        pump(60);
        rift_model_send_clear(&app->model);
    }
    hold(find_exact(thread_pane(), "Fint, ser deg"));
    check("a delivered message has no RESEND", rift_msgact_id(act) == 2 &&
                                                   !rift_msgact_button(act, RIFT_MSGACT_RESEND));
    /* ---- COPY: the words into the composer, nothing sent ---- */
    tap(rift_msgact_button(act, RIFT_MSGACT_COPY));
    field = rift_comms_field(app);
    check("COPY puts the message's words in the composer",
          field && strcmp(lv_textarea_get_text(field), "Fint, ser deg") == 0);
    check("and sends nothing", !rift_model_sending(&app->model) && !app->model.outbox.failed);
    if (pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_hide();
    }
    lv_textarea_set_text(field, "");
    pump(60);

    /* ---- the same by key ---- */
    pos_input_focus(field);
    pump(60);
    key(LV_KEY_LEFT);
    pump(60);
    check("LEFT on an empty composer opens the actions on the newest message",
          rift_msgact_id(act) == 4 && rift_msgact_by_key(act));
    check("and the keys go to them", pos_input_focused() == app->keysink);
    key(LV_KEY_UP);
    check("UP moves to the message before", rift_msgact_id(act) == 3);
    check("whose RESEND the keys are on",
          rift_msgact_button(act, RIFT_MSGACT_RESEND) != NULL);
    key(LV_KEY_ENTER);
    check("ENTER does it: with no service, the reader is told nothing was sent",
          app->model.outbox.failed && strstr(app->model.outbox.error, "nothing was sent"));
    rift_model_send_clear(&app->model);
    key(LV_KEY_LEFT);
    check("LEFT once more opens them again", rift_msgact_id(act) == 4);
    key(LV_KEY_ESC);
    pump(60);
    check("ESC closes them and gives the composer the keys back",
          rift_msgact_id(act) == 0 && pos_input_focused() == field);

    /* ---- a reply on a channel ---- */
    rift_app_open_conversation(app, site_key());
    pump(160);
    check("a reply is drawn with its quotation on a line above",
          find_text(thread_pane(), "\xE2\x86\xB3 Anna?: Are you coming up?") != NULL);
    check("and the answer as the body", find_exact(thread_pane(), "Ja, om ti minutter") != NULL);
    check("the mention itself is not printed", find_text(thread_pane(), "@[Anna]") == NULL);
    check("the stored text is the plain text that came",
          strstr(app->model.msg[app->model.msg_count - 1].text,
                 "@[Anna] \"Are you coming up?\" Ja, om ti minutter") != NULL);
    hold(find_text(thread_pane(), "Are you coming up? "));
    check("holding a channel line offers REPLY, and no RESEND",
          rift_msgact_id(act) == 60 && rift_msgact_button(act, RIFT_MSGACT_REPLY) &&
              !rift_msgact_button(act, RIFT_MSGACT_RESEND));
    tap(rift_msgact_button(act, RIFT_MSGACT_REPLY));
    field = rift_comms_field(app);
    check("REPLY puts the mention and the quotation in the composer, emoji and all",
          field && strcmp(lv_textarea_get_text(field),
                          "@[Anna] \"Are you coming up? \xF0\x9F\x91\x8D\" ") == 0);
    check("and sends nothing", !rift_model_sending(&app->model));
    shot("portrait-comms-reply");
    if (pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_hide();
    }
    lv_textarea_set_text(field, "");
    pump(60);

    /* ---- CONTACTS ---- */
    for (pass = 0; pass < 2; pass++) {
        const char *shape = pass ? "landscape, large" : "portrait";
        char what[160];

        rift_app_open_conversation(app, KEY_B);
        pump(120);
        if (pass == 0) {
            tap(find_text(content(), "CONTACTS \xE2\x80\xBA"));
        } else {
            tap(find_exact(content(), "CONTACTS"));
        }
        /* The keys follow from the timer (the composer had them). */
        pump(200);
        snprintf(what, sizeof(what), "%s: CONTACTS opens from COMMS, with COMMS still lit", shape);
        check(what, app->section == RIFT_SEC_CONTACTS && rift_tab_of(app->section) == RIFT_SEC_COMMS);
        snprintf(what, sizeof(what), "%s: every stored contact, A to Z - not NODES' heard order",
                 shape);
        check(what, rift_contacts_view_count(app) == 5 &&
                        strcmp(rift_contacts_view_key_at(app, 0), KEY_B) == 0 &&
                        strcmp(rift_contacts_view_key_at(app, 3), KEY_A) == 0);
        snprintf(what, sizeof(what), "%s: a row says what kind of node and its key", shape);
        check(what, find_text(content(), "REPEATER") && find_text(content(), "B2CAFE1E"));
        snprintf(what, sizeof(what), "%s: the rows are a pool, not one per contact", shape);
        check(what, rift_contacts_view_rows_built(app) > 0 && rift_contacts_view_rows_built(app) <= 32);
        snprintf(what, sizeof(what), "%s: CONTACTS' captions are whole", shape);
        check(what, captions_clipped(frame()) == 0);
        type_into(rift_contacts_view_field(app), "osl");
        snprintf(what, sizeof(what), "%s: searching by name", shape);
        check(what, rift_contacts_view_count(app) == 1 &&
                        strcmp(rift_contacts_view_key_at(app, 0), KEY_A) == 0);
        lv_textarea_set_text(rift_contacts_view_field(app), "");
        pump(60);
        type_into(rift_contacts_view_field(app), "d4de");
        snprintf(what, sizeof(what), "%s: and by key prefix", shape);
        check(what, rift_contacts_view_count(app) == 1 &&
                        strcmp(rift_contacts_view_key_at(app, 0), KEY_D) == 0);
        pos_input_focus(app->keysink);
        pump(200);
        key(LV_KEY_DOWN);
        key(LV_KEY_ENTER);
        snprintf(what, sizeof(what),
                 "%s: a repeater is not written to: it says why, and stays (section %d, %d listed)",
                 shape, (int)app->section, rift_contacts_view_count(app));
        check(what, app->section == RIFT_SEC_CONTACTS && rift_contacts_view_note(app) &&
                        rift_contacts_view_note(app)[0]);
        if (pass == 0) {
            shot("portrait-contacts-repeater");
        } else {
            shot("landscape-contacts-large");
        }
        lv_textarea_set_text(rift_contacts_view_field(app), "");
        pump(60);
        tap(rift_contacts_view_filter_button(app, 1));
        pump(120);
        snprintf(what, sizeof(what), "%s: RECENT is who was spoken with, the latest first (%d, query '%s', recent %d)",
                 shape, rift_contacts_view_count(app), app->contacts_query, app->contacts_recent);
        check(what, rift_contacts_view_count(app) == 2 &&
                        strcmp(rift_contacts_view_key_at(app, 0), KEY_A) == 0 &&
                        strcmp(rift_contacts_view_key_at(app, 1), KEY_B) == 0);
        pos_input_focus(app->keysink);
        pump(200);
        key(LV_KEY_DOWN);
        key(LV_KEY_ENTER);
        snprintf(what, sizeof(what), "%s: ENTER on a contact opens its conversation", shape);
        check(what, app->section == RIFT_SEC_COMMS && app->have_conv && strcmp(app->conv, KEY_A) == 0);
        tap(find_text(content(), pass == 0 ? "CONTACTS \xE2\x80\xBA" : "CONTACTS"));
        tap(rift_contacts_view_filter_button(app, 0));
        pos_input_focus(app->keysink);
        pump(40);
        key(LV_KEY_ESC);
        snprintf(what, sizeof(what), "%s: ESC goes back to COMMS", shape);
        check(what, app->section == RIFT_SEC_COMMS);
        if (pass == 0) {
            /* Turned and at Large, as the owner's unit runs: the app opened
             * again over it, the way the shell rebuilds it for a new size. */
            app_stop();
            use_display(POS_ROTATION_270, PANEL_CORNER);
            pos_theme_select_text_size(POS_TEXT_SIZE_LARGE);
            app_start();
            quiet_client();
            give_nodes();
            give_service();
            give_messages();
            act = rift_comms_actions(app);
            pump(200);
        }
    }
    /* ---- landscape, Large: the bar still fits ---- */
    rift_app_open_conversation(app, KEY_B);
    pump(160);
    hold(find_exact(thread_pane(), "Pr\xC3\xB8ver direct"));
    check("landscape, large: holding a message opens its actions", rift_msgact_id(act) == 3);
    check("landscape, large: and their captions are whole", captions_clipped(frame()) == 0);
    check("landscape, large: inside the panel",
          inside_body(rift_msgact_button(act, RIFT_MSGACT_CLOSE)));
    shot("landscape-comms-message-actions-large");
    tap(rift_msgact_button(act, RIFT_MSGACT_CLOSE));
    check("CLOSE closes them", rift_msgact_id(act) == 0);
    pos_theme_select_text_size(was);
    use_display(POS_ROTATION_0, PANEL_CORNER);
    app_stop();
    /* COPY and REPLY bring the touch keyboard up in portrait, on purpose;
     * the sessions after this one count their own. */
    check("COPY and REPLY asked for the touch keyboard in portrait", kb_shows > kb_before);
    kb_shows = kb_before;
}

int main(void)
{
    lv_indev_t *indev;
    int round;

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);
    pos_input_init();
    shots_dir = getenv("RIFT_SHOTS_DIR");
    /* The preferences file goes somewhere of the test's own, and the sound
     * to a counter. */
    {
        static char state_dir[] = "/tmp/rift-app-test-XXXXXX";

        if (!mkdtemp(state_dir)) {
            printf("FAIL a state directory for the test\n");
            return 1;
        }
        setenv("POCKETOS_STATE_DIR", state_dir, 1);
        g_state_dir = state_dir;
    }
    rift_sound_set_backend(&fake_sound);
    /* The screen is the shell's job, and without it everything below is
     * drawn on LVGL's default white rather than on the theme's own
     * background. $RIFT_THEME and $RIFT_MODE pick which theme the
     * screenshots are taken in; the default is the platform's. */
    {
        char why[128];

        if (getenv("RIFT_THEME") || getenv("RIFT_MODE")) {
            pos_theme_select(getenv("RIFT_THEME"), getenv("RIFT_MODE"), why, sizeof(why));
        }
    }
    pocketui_init();
    pocketui_style_screen(lv_screen_active());

    g_content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_content);
    lv_obj_remove_flag(g_content, LV_OBJ_FLAG_SCROLLABLE);
    use_display(POS_ROTATION_0, PANEL_CORNER);

    /* ---- portrait, with no service at all ----------------------------- */
    app_start();
    check("the app opens", app != NULL);
    check("with the section strip", strip() != NULL);
    check("the content area", content() != NULL);
    check("and the command line", cmdline() != NULL);
    check("the strip is a 64 px navigation row, not a 36 px data row",
          lv_obj_get_height(strip()) == RIFT_NAV_ROW_H);
    check("and so is the command line", lv_obj_get_height(cmdline()) == RIFT_TOUCH_H);
    /* The key sink moved out of the command line so the line can go away
     * without taking the keys with it. It is 1 px, takes no taps, and is
     * never hidden. */
    check("the key sink is outside the command line",
          app->keysink && lv_obj_get_parent(app->keysink) == frame());
    check("takes no taps", !lv_obj_has_flag(app->keysink, LV_OBJ_FLAG_CLICKABLE));
    check("and is never hidden", visible(app->keysink));
    check("the tabs are ACTIVITY, NODES, COMMS, MAP and SYSTEM",
          find_text(strip(), "ACTIVITY") && find_text(strip(), "NODES") &&
              find_text(strip(), "COMMS") && find_exact(strip(), "MAP") &&
              find_text(strip(), "SYSTEM"));
    check("and NET is not a tab of its own (it is under NODES)",
          find_exact(strip(), "NET") == NULL);
    check("each of them is a 56 px target",
          lv_obj_get_height(tab(0)) == RIFT_TOUCH_H &&
              lv_obj_get_height(tab(3)) == RIFT_TOUCH_H);
    check("ACTIVITY is the section it opens on", app->section == RIFT_SEC_ACTIVITY);
    /* With no meshcored the app must say so, and must not draw a mesh. */
    check("with no service the state is drawn as absent",
          app->model.state == RIFT_SVC_ABSENT);
    check("and named in words", find_text(content(), "NOT RUNNING") != NULL);
    check("nothing is claimed about the nodes", app->model.node_count == 0);
    check("the command line says the service is not answering",
          find_text(cmdline(), "meshcored") != NULL);
    check("and the app did not go home by itself", home_calls == 0);
    shot("portrait-no-service");

    /* ---- ACTIVITY with a service and a mesh --------------------------- */
    quiet_client();
    give_nodes();
    give_service();
    check("the service state is drawn", find_text(content(), "ONLINE") != NULL);
    check("with this device's name", find_text(content(), "K230-A") != NULL);
    check("and the nodes heard most recently", find_text(content(), "OSLO-01") != NULL);
    check("the raw feed shows a received advert", find_text(content(), "advert") != NULL);
    /* Read the result, never the kind: three of the five results a tx
     * activity can carry are not "it went out". */
    check("and shows a transmit by its result, not as a success",
          find_text(content(), "rx_resume_failed") != NULL);
    /* The last twenty minutes as a bar each (DS §37.4): the fixture's one
     * received advert, forty-five seconds ago, is one advert in the newest
     * or the minute before it, and nothing else - the transmit is not
     * something this device heard. The words beside it name what the
     * colours mean. */
    {
        lv_obj_t *graph = rift_activity_graph(app);
        const struct rift_traffic_bins *bins = rift_traffic_graph_bins(graph);
        unsigned adv = 0;
        unsigned rest = 0;
        int i;
        int c;

        check("ACTIVITY has the traffic graph", graph != NULL && visible(graph) &&
                                                    lv_obj_get_height(graph) == RIFT_GRAPH_TOTAL_H);
        check("which is showing bins", bins != NULL && bins->started);
        for (i = 0; bins && i < RIFT_TRAFFIC_MINUTES; i++) {
            for (c = 0; c < RIFT_TRAFFIC_CLASSES; c++) {
                if (c == RIFT_TRAFFIC_ADV) {
                    adv += bins->count[i][c];
                } else {
                    rest += bins->count[i][c];
                }
            }
        }
        check("one advert heard, in the newest minutes",
              adv == 1 && rest == 0 &&
                  (bins->count[RIFT_TRAFFIC_MINUTES - 1][RIFT_TRAFFIC_ADV] +
                       bins->count[RIFT_TRAFFIC_MINUTES - 2][RIFT_TRAFFIC_ADV] ==
                   1));
        check("the caption says what the bars count",
              find_text(content(), "HEARD ON AIR") != NULL && find_text(content(), "PEAK 1/MIN"));
        check("and the legend names the classes in words",
              find_exact(content(), "MSG") && find_exact(content(), "ADV") &&
                  find_exact(content(), "OTHER"));
        /* Below the fold in portrait: the section scrolled to its end, as a
         * finger would (only the section - the test's body scrolls too, and
         * a recursive scroll would move that and clip every caption), then
         * it has to be inside the body like everything else. */
        lv_obj_scroll_to_y(ancestor(graph, 4), LV_COORD_MAX, LV_ANIM_OFF);
        pump(40);
        check("the graph is inside the body", inside_body(graph));
        /* A message heard now lands in the newest minute as a message, and
         * only a changed bin repaints the graph. */
        {
            cJSON *o;
            char json[160];

            snprintf(json, sizeof(json),
                     "{\"kind\":\"rx\",\"payload_type\":\"group_text\",\"bytes\":30,\"mono_ms\":%lld}",
                     (long long)rift_mono_ms());
            o = cJSON_Parse(json);
            rift_model_apply_event(&app->model, "mesh.activity", o);
            cJSON_Delete(o);
            rift_app_refresh(app);
            pump(60);
            bins = rift_traffic_graph_bins(graph);
            check("a message heard now is a message in the newest minute",
                  bins && bins->count[RIFT_TRAFFIC_MINUTES - 1][RIFT_TRAFFIC_MSG] == 1);
            check("the bar for it is drawn from the ladder",
                  rift_graph_height_of(1) == 4 && rift_graph_height_of(3) == 9 &&
                      rift_graph_height_of(16) == RIFT_GRAPH_BAND && rift_graph_height_of(0) == 0);
            /* A few minutes of a working mesh, for the photograph: heard
             * frames of every class spread over the last quarter hour. */
            for (i = 0; i < 40; i++) {
                snprintf(json, sizeof(json),
                         "{\"kind\":\"rx\",\"payload_type\":\"%s\",\"bytes\":30,\"mono_ms\":%lld}",
                         i % 5 == 0 ? "advert" : i % 3 == 0 ? "ack" : "text",
                         (long long)(rift_mono_ms() - 60000LL * ((i * 7) % 15) - 1000 * i));
                o = cJSON_Parse(json);
                rift_model_apply_event(&app->model, "mesh.activity", o);
                cJSON_Delete(o);
            }
            rift_app_refresh(app);
            pump(60);
            lv_obj_scroll_to_y(ancestor(graph, 4), LV_COORD_MAX, LV_ANIM_OFF);
            pump(40);
            shot("portrait-activity-traffic");
            /* graph > panel > column > split > the section's scrolling root */
            lv_obj_scroll_to_y(ancestor(graph, 4), 0, LV_ANIM_OFF);
            pump(40);
        }
    }
    check("everything on ACTIVITY is inside the body", inside_body(content()));
    shot("portrait-activity");

    /* A panel's caption is centred on its top rule, so half of it lies
     * above the panel's own box. LVGL clips a child to its parent, and every
     * caption was drawn with the top of its capitals cut off - on unit A as
     * well as here - until the panel let it overhang. */
    {
        lv_obj_t *caption = find_text(content(), "RADIO SERVICE");
        lv_obj_t *panel = caption ? lv_obj_get_parent(caption) : NULL;
        lv_area_t c;
        lv_area_t p;
        lv_area_t v;

        check("a panel caption is found", caption != NULL && panel != NULL);
        if (caption && panel) {
            lv_obj_update_layout(panel);
            lv_obj_get_coords(caption, &c);
            lv_obj_get_coords(panel, &p);
            lv_obj_get_coords(content(), &v);
            check("it straddles the panel's top rule", c.y1 < p.y1 && c.y2 > p.y1);
            check("and the panel lets it overhang instead of clipping it",
                  lv_obj_has_flag(panel, LV_OBJ_FLAG_OVERFLOW_VISIBLE) &&
                      ext_draw_of(panel) >= p.y1 - c.y1);
            check("into room the section has, not off the top of it", c.y1 >= v.y1);
        }
        check("and no ancestor clips any ACTIVITY caption either",
              caption_unclipped("RADIO SERVICE") && caption_unclipped("RECENTLY HEARD") &&
                  caption_unclipped("MESH ACTIVITY"));
    }
    /* ACTIVITY is status and traffic: nothing on it changes a setting. */
    check("ACTIVITY holds no settings: no RENAME, path hash, channel or sound control",
          find_exact(content(), "RENAME") == NULL && find_exact(content(), "2 B") == NULL &&
              find_exact(content(), "ADD CHANNEL") == NULL &&
              find_text(content(), "Sound for a new DM") == NULL &&
              find_exact(content(), "CLOSE RIFT") == NULL &&
              find_text(content(), "ADVERT NEAR") == NULL);

    /* The traffic the service counted, in its own words. */
    check("the service's transmit counts are drawn, outcomes kept apart",
          find_text(content(), "TX 3 OK \xC2\xB7 1 FAILED") != NULL);

    /* ---- ADVERT: only on a press, on SYSTEM's DEVICE panel ---------------- */
    rift_app_show_section(app, RIFT_SEC_SYSTEM);
    pump(60);
    check("SYSTEM holds the moved settings, captions whole",
          caption_unclipped("DEVICE") && caption_unclipped("ADDRESSING") &&
              caption_unclipped("SOUND") && caption_unclipped("CHANNELS") &&
              find_exact(content(), "RENAME") != NULL && find_exact(content(), "2 B") != NULL);
    {
        lv_obj_t *near = action_of(find_text(content(), "ADVERT NEAR"));
        lv_obj_t *mesh = action_of(find_text(content(), "ADVERT MESH"));

        check("DEVICE offers both adverts", near != NULL && mesh != NULL);
        check("as 56 px actions", near && lv_obj_get_height(near) == RIFT_TOUCH_H);
        check("which a service whose radio can send makes pressable",
              near && !lv_obj_has_state(near, LV_STATE_DISABLED) && mesh &&
                  !lv_obj_has_state(mesh, LV_STATE_DISABLED));
        check("and says what the two do before either is pressed",
              find_text(content(), "heard in direct range") != NULL);
        check("their words fit their buttons", labels_overflowing(content()) == 0);
        check("opening the app and drawing it asked for no advert",
              app->model.advert.kind == RIFT_ACTION_NONE);
        tap(near);
        /* The client is parked here - there is no service - so the press
         * reaches the client and is refused there, with a reason, where it
         * was made. tests/rift_ipc_test.c has the one that is answered. */
        check("a press asks for a zero-hop advert",
              app->model.advert.kind == RIFT_ACTION_ADVERT_NEAR);
        check("and with nobody to ask, says it was not sent rather than that it was",
              app->model.advert.failed && !app->model.advert.done &&
                  find_text(content(), "ZERO-HOP ADVERT \xC2\xB7 NOT DONE") != NULL);
        rift_model_action_clear(&app->model, RIFT_ACTION_ADVERT_NEAR);
        /* A radio the service says cannot send is not offered. */
        {
            cJSON *o = cJSON_Parse("{\"state\":\"degraded\",\"reason\":\"receiver down\","
                                   "\"radio\":{\"connected\":true,\"lease_held\":true,"
                                   "\"online\":false},\"nodes\":5,\"counters\":{"
                                   "\"rx_events\":12,\"nodes_unretained\":2,"
                                   "\"contacts_full\":2}}");

            rift_model_apply_status(&app->model, o, rift_mono_ms());
            cJSON_Delete(o);
            rift_app_refresh(app);
            pump(60);
        }
        check("a radio that cannot send takes the adverts away",
              lv_obj_has_state(near, LV_STATE_DISABLED) &&
                  lv_obj_has_state(mesh, LV_STATE_DISABLED));
        check("and says why", find_text(content(), "not ready to send") != NULL);
        /* A full node table: the service keeps no more, and a node it could
         * not keep is one nothing can be sent to. ACTIVITY says it. */
        rift_app_show_section(app, RIFT_SEC_ACTIVITY);
        pump(60);
        check("a full node table is said, with what to do about it",
              find_text(content(), "node table is full") != NULL &&
                  find_text(content(), "Forget a node in NODES") != NULL);
        {
            /* The service's counter only grows. Once a node has been
             * forgotten - here, by another client - the table has room, and
             * the adverts it turned away before say nothing about now. */
            cJSON *o = cJSON_Parse("{\"reason\":\"removed\",\"node\":{\"public_key\":\"" KEY_C
                                   "\"}}");

            rift_model_apply_event(&app->model, "mesh.node", o);
            cJSON_Delete(o);
            rift_app_refresh(app);
            pump(60);
        }
        check("once a node is forgotten, the table is not called full",
              find_text(content(), "node table is full") == NULL &&
                  app->model.nodes_unretained == 2);
        give_nodes();
        give_service();
        lv_obj_scroll_to_y(kid(content(), 0), 0, LV_ANIM_OFF);
        pump(40);
    }

    /* ---- NODES, portrait ---------------------------------------------- */
    tap(tab(1));
    check("tapping NODES opens it", app->section == RIFT_SEC_NODES);
    check("the column header is there", find_text(content(), "HOPS") != NULL);
    check("the fresh group is labelled with its count",
          find_text(content(), "HEARD < 12 H") != NULL);
    check("and the stale group with its own", find_text(content(), "NOT HEARD > 12 H") != NULL);
    check("a direct node reads DIR, never 0", find_text(content(), "DIR") != NULL);
    check("an unmeasured RSSI is ? and not a number",
          find_text(content(), RIFT_UNKNOWN) != NULL);
    check("a node with no route back is still listed",
          find_text(content(), "NO-3241 FO") != NULL);
    check("a name in a script that is not ASCII is drawn",
          find_text(content(), "\xC3\xB8rlandet") != NULL);
    /* A name too long for the column is shortened here, not by LVGL, and is
     * visibly shortened: the ellipsis is the only honest way to say that
     * what is on screen is not the whole name. */
    check("a name too long for its column ends in an ellipsis",
          find_text(content(), RIFT_ELLIPSIS) != NULL);
    {
        int rows = count_visible_of_height(content(), RIFT_ROW_H);

        check("the list is dense: five 36 px rows and nothing taller", rows >= 5);
    }
    check("nothing has spilled out of the body", inside_body(content()));
    /* The activity pulse is the age of the last time each was heard,
     * bucketed, and nothing else. */
    check("heard 90 s ago is NOW", node_pulse("OSLO-01") == RIFT_PULSE_NOW);
    check("12 min ago is RECENT", node_pulse("HYTTA") == RIFT_PULSE_RECENT);
    check("4 h ago is QUIET", node_pulse("NO-3241 FO") == RIFT_PULSE_QUIET);
    check("never heard draws no pulse at all, not a stale one",
          node_pulse("never-heard") == RIFT_PULSE_NONE);
    /* The footer used to repeat the group labels' counts on every list. It
     * is there now only for what the rows cannot say themselves. */
    check("an ordinary list has no footer repeating its counts",
          find_text(content(), "heard in the last 12 h") == NULL);
    check("and no command line under it either: there is nothing to type here",
          !visible(cmdline()));
    shot("portrait-nodes");

    /* A row selects, and only selects. */
    {
        lv_obj_t *row = find_text(content(), "HYTTA");
        lv_obj_t *line = row;

        while (line && lv_obj_get_height(line) != RIFT_ROW_H) {
            line = lv_obj_get_parent(line);
        }
        check("the node's row is a 36 px row", line != NULL);
        check("nothing is selected to begin with", !app->have_selected);
        tap(line);
        check("tapping it selects it", app->have_selected);
        check("the one it names", strcmp(app->selected, KEY_B) == 0);
        /* RIFT-DEV-1: nothing destructive or immediate fires from a row,
         * and the detail is a separate, larger target. */
        check("and does not open anything", !app->detail_open);
        check("the selection expands in place", find_text(content(), "RELAYED") != NULL);
        check("with the path written out", find_text(content(), "K230-A") != NULL);
        check("and the uncertainty said, not hidden",
              find_text(content(), "8 HOPS") != NULL);
        check("a 56 px action bar appears with it",
              count_visible_of_height(content(), RIFT_TOUCH_H) >= 3);
        check("the expansion offers MESSAGE", find_text(content(), "MESSAGE") != NULL);
        check("and DETAIL is there to open", find_text(content(), "DETAIL") != NULL);
        /* The design's third action, PATH, had nothing behind it and was
         * drawn disabled; a button that can never be pressed is width taken
         * from the two that can. */
        check("and nothing in it is a button that cannot be pressed",
              count_disabled(content()) == 0);
        check("each word fits its button", labels_overflowing(content()) == 0);
        shot("portrait-nodes-selected");
    }

    /* The pushed DETAIL screen. */
    tap(find_text(content(), "DETAIL"));
    check("DETAIL pushes the screen", app->detail_open);
    check("with the link state", find_text(content(), "LINK STATE") != NULL);
    check("the node's identity", find_text(content(), "IDENTITY") != NULL);
    check("its key, shortened", find_text(content(), "B2CA") != NULL);
    check("the hop ladder", find_text(content(), "hash only") != NULL);
    /* End to end over relays is not a measurement nobody took; it is one
     * that cannot exist. */
    check("and end to end over relays drawn as impossible, not unknown",
          find_text(content(), RIFT_EMDASH) != NULL);
    check("the detail screen fits the body", inside_body(content()));
    shot("portrait-node-detail");

    /* ---- the actions, first, and FORGET asking first ------------------- */
    {
        lv_obj_t *msg = action_of(find_text(content(), "MESSAGE"));
        lv_obj_t *reset = action_of(find_text(content(), "RE-ROUTE"));
        lv_obj_t *forget = action_of(find_exact(content(), "FORGET"));
        lv_obj_t *link = find_text(content(), "LINK STATE");
        lv_obj_t *cancel;
        lv_obj_t *confirm;
        lv_area_t a;
        lv_area_t b;

        check("the detail offers MESSAGE, RE-ROUTE and FORGET",
              msg != NULL && reset != NULL && forget != NULL);
        check("its panel captions are drawn whole",
              caption_unclipped("LINK STATE") && caption_unclipped("PATH"));
        check("and every action's word fits its button, four abreast in portrait",
              labels_overflowing(content()) == 0);
        if (msg && link) {
            lv_obj_get_coords(msg, &a);
            lv_obj_get_coords(link, &b);
            check("above the panels, where they are reached without scrolling", a.y2 < b.y1);
            check("and on screen as the detail opens", within(msg, content()));
        }
        check("the disabled NET action and its apology are gone",
              find_text(content(), "NET arrives") == NULL && count_disabled(content()) == 0);
        check("a node with a route can have it forgotten",
              reset && !lv_obj_has_state(reset, LV_STATE_DISABLED));
        check("and can be forgotten", forget && !lv_obj_has_state(forget, LV_STATE_DISABLED));

        tap(forget);
        check("FORGET asks before anything is done",
              find_text(content(), "Forget HYTTA?") != NULL);
        check("and nothing has been asked of the service yet",
              app->model.node_op.kind == RIFT_ACTION_NONE);
        check("saying what forgetting costs",
              find_text(content(), "comes back when it next adverts") != NULL);
        cancel = action_of(find_exact(content(), "CANCEL"));
        confirm = action_of(find_exact(content(), "FORGET"));
        check("the confirmation is two buttons, Cancel first",
              cancel && confirm && lv_obj_get_x(cancel) < lv_obj_get_x(confirm));
        /* DS §17.5: forgetting cannot be undone from here, so the accent
         * goes on the safe choice - the power-off precedent. */
        check("with the accent on Cancel, the safe choice",
              cancel && lv_color_eq(lv_obj_get_style_bg_color(cancel, LV_PART_MAIN),
                                    pos_theme_color(POS_COLOR_ACCENT_PRIMARY)));
        check("and the action bar out of the way while it asks", !visible(msg));
        check("the confirmation's words fit its buttons", labels_overflowing(content()) == 0);
        shot("portrait-forget-confirm");
        tap(cancel);
        check("Cancel puts the actions back", visible(msg) &&
                                                 find_text(content(), "Forget HYTTA?") == NULL);
        check("having asked nothing of anybody", app->model.node_op.kind == RIFT_ACTION_NONE);

        /* DS §17.5: any other way out is Cancel. A confirmation left up
         * behind the reader is a FORGET armed for whoever comes back. */
        tap(forget);
        tap(tab(0));
        pump(60);
        check("leaving NODES with FORGET asking closes the detail",
              app->section == RIFT_SEC_ACTIVITY && !app->detail_open);
        tap(tab(1));
        rift_app_open_detail(app, 1);
        pump(60);
        check("and coming back finds the actions, not the question",
              visible(msg) && find_text(content(), "Forget HYTTA?") == NULL);
        tap(forget);
        rift_app_open_detail(app, 0);
        pump(60);
        rift_app_open_detail(app, 1);
        pump(60);
        check("closing the detail is a Cancel too",
              visible(msg) && find_text(content(), "Forget HYTTA?") == NULL);
        check("and neither asked the service anything",
              app->model.node_op.kind == RIFT_ACTION_NONE);

        /* A confirmation belongs to the node it was asked about. */
        tap(forget);
        rift_app_select(app, KEY_A);
        pump(80);
        check("moving the selection drops a confirmation about another node",
              find_text(content(), "Forget ") == NULL && visible(msg));
        rift_app_select(app, KEY_C);
        pump(80);
        check("a node with no route has no route to forget",
              lv_obj_has_state(reset, LV_STATE_DISABLED));
        rift_app_select(app, KEY_B);
        pump(80);
        tap(forget);
        tap(action_of(find_exact(content(), "FORGET")));
        check("confirming asks for it, for that node",
              app->model.node_op.kind == RIFT_ACTION_FORGET &&
                  strcmp(app->model.node_op.key, KEY_B) == 0);
        /* The client is parked: the request is refused where it was made,
         * and nothing is claimed done. tests/rift_ipc_test.c has the one
         * that is answered. */
        check("and with nobody to ask, says it was not done",
              app->model.node_op.failed &&
                  find_text(content(), "FORGET \xC2\xB7 NOT DONE") != NULL);
        check("so the node is still listed", rift_model_find(&app->model, KEY_B) != NULL);
        rift_model_action_clear(&app->model, RIFT_ACTION_FORGET);
        rift_app_refresh(app);
        pump(40);
    }
    /* The way back is the detail's own action bar: the shell's back slab
     * goes home, not up a level, and RIFT changes nothing about it. */
    tap(find_text(content(), "NODES"));
    check("and it can be left again", !app->detail_open);
    check("with the list back", find_text(content(), "HEARD < 12 H") != NULL);

    /* Arrows and Enter, which is a list in the one Doors focus group. */
    check("the arrow keys move the selection", rift_nodes_key(app, LV_KEY_DOWN) == 1);
    pump(60);
    check("to another node", strcmp(app->selected, KEY_B) != 0);
    check("Enter opens the detail", rift_nodes_key(app, LV_KEY_ENTER) == 1);
    pump(60);
    check("which is open", app->detail_open);
    check("and Esc leaves it", rift_nodes_key(app, LV_KEY_ESC) == 1);
    pump(60);
    check("closed again", !app->detail_open);
    {
        /* The selection is a key, and the node it names can be forgotten
         * under it - by another client, or by a snapshot that no longer
         * holds it. Enter then has nothing to open. */
        char was[RIFT_KEY_HEX];

        snprintf(was, sizeof(was), "%s", app->selected);
        rift_model_drop_node(&app->model, was);
        rift_app_refresh(app);
        pump(40);
        check("Enter on a selection whose node has gone opens nothing",
              rift_nodes_key(app, LV_KEY_ENTER) == 0 && !app->detail_open);
        give_nodes();
        rift_app_refresh(app);
        pump(40);
    }

    /* ---- a long list keeps its place ------------------------------------ */
    give_many_nodes(44);
    {
        lv_obj_t *list = ancestor(find_exact(content(), "MANY-00"), 4);
        int32_t y;
        int i;

        check("a list longer than the body scrolls",
              list && lv_obj_get_scroll_bottom(list) > 0);
        lv_obj_scroll_to_y(list, 200, LV_ANIM_OFF);
        pump(40);
        y = lv_obj_get_scroll_y(list);
        check("and is read from part way down", y == 200);
        {
            /* A node heard for the first time: the list gains a row and
             * re-orders, which is a rebuild - and lv_obj_clean puts a
             * list's scroll back to its top. On a live mesh that happened
             * every few seconds under whoever was reading it. */
            cJSON *o = cJSON_Parse("{\"reason\":\"discovered\",\"node\":{\"public_key\":"
                                   "\"fe0000000000000000000000000000000000000000000000000000"
                                   "00000000fe\",\"name\":\"NEWCOMER\",\"path_known\":false,"
                                   "\"last_heard_mono_ms\":1}}");
            int before = app->model.node_count;

            check("a node heard for the first time is taken",
                  rift_model_apply_event(&app->model, "mesh.node", o) == 0 &&
                      app->model.node_count == before + 1);
            cJSON_Delete(o);
            rift_app_refresh(app);
            pump(60);
        }
        check("a new node rebuilds the list without throwing the reader back to the top",
              lv_obj_get_scroll_y(list) == y);
        for (i = 0; i < 40; i++) {
            rift_nodes_key(app, LV_KEY_DOWN);
            pump(20);
        }
        {
            const struct rift_node *sel = rift_app_selected(app);
            lv_obj_t *slot = sel ? ancestor(find_exact(content(), sel->name), 3) : NULL;

            check("forty presses of the down arrow select forty rows down",
                  sel && strncmp(sel->name, "MANY-", 5) == 0);
            check("and the selected row, expansion and all, is in view", within(slot, list));
        }
    }
    give_nodes();
    rift_app_select(app, KEY_B);
    pump(60);

    /* ---- COMMS, in portrait --------------------------------------------- */
    tap(tab(2));
    check("COMMS is reachable", app->section == RIFT_SEC_COMMS);
    check("before the service has answered it says it is waiting",
          find_text(content(), "Waiting for meshcored") != NULL);
    {
        /* A service that answered and has nothing: a different thing from
         * not having asked yet, and drawn as a different thing. */
        cJSON *o = cJSON_Parse("{\"messages\":[],\"count\":0,\"total\":0,\"persistent\":false}");

        rift_model_apply_messages(&app->model, o);
        cJSON_Delete(o);
        /* And the channel half of the same answer. "No channels" is only
         * true once the service has said so, so until mesh.channels has been
         * answered the list is still waiting rather than empty. */
        o = cJSON_Parse("{\"channels\":[],\"count\":0,\"max\":8,\"persistent\":true}");
        rift_model_apply_channels(&app->model, o);
        cJSON_Delete(o);
        rift_app_refresh(app);
        pump(60);
    }
    check("an answered, empty history says so instead",
          find_text(content(), "No conversations and no channels") != NULL);
    /* The design merges channels into this list, and a channel is joined
     * with its key on the service rather than here. An empty list says which
     * of the two it is rather than leaving a reader who knows the design to
     * wonder where the channels went. */
    check("and says where a channel is joined",
          find_text(content(), "joined on SYSTEM, under CHANNELS") != NULL);

    give_messages();
    check("the history on opening makes no sound", fake_plays == 0);
    /* The rows say what is there; a note counting them was a line taken
     * from the list. It is shown only when it says something they cannot. */
    check("with conversations the list's note has nothing to add, and is gone",
          find_text(content(), "no channels joined") == NULL &&
              find_text(content(), "No conversations") == NULL);
    check("both conversations are listed", find_text(content(), "HYTTA") != NULL &&
                                               find_text(content(), "OSLO-01") != NULL);
    check("a preview says who spoke last", find_text(content(), "you: On my way") != NULL);
    check("nothing is open yet", rift_comms_open_peer(app) == NULL);
    check("so the composer says what it is waiting for",
          find_text(content(), "Choose a conversation") != NULL);
    shot("portrait-comms-list");

    /* Opening a conversation shows its thread, and choosing where a message
     * would go must not send one. */
    rift_app_open_conversation(app, KEY_B);
    pump(80);
    check("the thread is open", rift_comms_open_peer(app) != NULL);
    check("and holds the messages", find_text(content(), "Fint, ser deg") != NULL);
    check("an acknowledged message says how long the ACK took",
          find_text(content(), "DELIVERED") != NULL && find_text(content(), "ACK 41 s") != NULL);
    check("one that timed out says NO ACK", find_text(content(), "NO ACK") != NULL);
    check("a received one carries what was measured for it",
          find_text(content(), "RECEIVED \xC2\xB7 \xE2\x88\x92" "88 dBm \xC2\xB7 SNR 6.5") !=
              NULL);
    check("nothing anywhere claims a submitted message was delivered",
          find_text(content(), "SENT \xC2\xB7 DIRECT") == NULL ||
              find_text(content(), "DELIVERED \xC2\xB7 ACK") != NULL);
    check("the thread header says how this peer is reached",
          find_text(content(), "RELAYED") != NULL);
    check("and the composer is usable now", find_text(content(), "SEND") != NULL);
    /* One caption line per message: how long ago, then what became of it.
     * The line of "you" / the peer's name that sat over every body said what
     * the side of the rule and the thread's header already say. */
    check("a message's caption is one line: age, state, evidence",
          find_text(content(), "4m \xC2\xB7 DELIVERED \xC2\xB7 ACK 41 s") != NULL);
    check("with no sender line over it", find_exact(content(), "you") == NULL);
    {
        lv_obj_t *send = action_of(find_text(content(), "SEND"));
        lv_obj_t *field = find_textarea(content());

        check("SEND is as wide as its word, not half the row",
              send && lv_obj_get_width(send) == 128);
        check("so the width goes to the field somebody types in",
              field && lv_obj_get_width(lv_obj_get_parent(field)) > 2 * 128);
    }
    check("with messages to read, the thread carries no standing note",
          find_text(content(), "does not survive a restart") == NULL &&
              find_text(content(), "OF 2 SENT") == NULL);
    shot("portrait-comms-thread");
    {
        /* A submission with no answer is not one the service refused: the
         * connection went before it said, and the message may be on the
         * air. The note says which of the two it was. */
        app->model.outbox.failed = 1;
        app->model.outbox.unknown = 1;
        snprintf(app->model.outbox.error, sizeof(app->model.outbox.error), "%s",
                 "meshcored went away before it answered");
        rift_app_refresh(app);
        pump(40);
        check("an unanswered submission says there was no answer",
              find_text(content(), "No answer: meshcored went away") != NULL &&
                  find_text(content(), "Not sent") == NULL);
        app->model.outbox.unknown = 0;
        snprintf(app->model.outbox.error, sizeof(app->model.outbox.error), "%s", "radio busy");
        rift_app_refresh(app);
        pump(40);
        check("and one the service refused says it was not sent",
              find_text(content(), "Not sent: radio busy") != NULL);
        rift_model_send_clear(&app->model);
        rift_app_refresh(app);
        pump(40);
        check("both go when the failure is put away",
              find_text(content(), "Not sent") == NULL &&
                  find_text(content(), "No answer") == NULL);
    }
    {
        /* The touch keyboard takes 296 px off the body (the shell shrinks
         * the content box). The thread shrinks with it, and is read at its
         * end again rather than keeping an offset that now hides the newest
         * message behind the sheet. */
        int32_t full = lv_obj_get_height(g_content);
        lv_obj_t *newest;

        give_long_thread();
        rift_app_open_conversation(app, KEY_E);
        pump(120);
        newest = find_text(thread_pane(), "the newest line");
        if (!newest || !within(newest, ancestor(newest, 4)) ||
            lv_obj_get_scroll_y(ancestor(newest, 4)) <= 0) {
            printf("     long thread: found=%d scroll_y=%d h=%d\n", newest != NULL,
                   newest ? (int)lv_obj_get_scroll_y(ancestor(newest, 4)) : -1,
                   newest ? (int)lv_obj_get_height(ancestor(newest, 4)) : -1);
        }
        check("a thread longer than its pane opens at its end",
              newest && within(newest, ancestor(newest, 4)) &&
                  lv_obj_get_scroll_y(ancestor(newest, 4)) > 0);
        lv_obj_set_height(g_content, full - 296);
        pump(300);
        newest = find_text(thread_pane(), "the newest line");
        check("with the keyboard up, the newest message is still in view",
              newest && within(newest, ancestor(newest, 4)));
        lv_obj_set_height(g_content, full);
        pump(300);
        rift_app_open_conversation(app, KEY_B);
        pump(120);
    }

    /* An unread message from the other conversation puts the pill on the
     * COMMS tab, and reading it takes it away. */
    give_unread();
    check("an arrival while another thread is open is unread",
          rift_model_unread(&app->model, KEY_A) == 1);
    check("which the tab says", rift_model_unread_total(&app->model) == 1);
    rift_app_open_conversation(app, KEY_A);
    pump(80);
    check("opening that conversation reads it", rift_model_unread(&app->model, KEY_A) == 0);
    check("and the tab has nothing left to say", rift_model_unread_total(&app->model) == 0);

    /* ---- channels, in portrait ------------------------------------------
     *
     * The approved design merges channels into this list with a "#" glyph.
     * Each of these is one of the ways a channel is NOT a conversation with
     * a node: it has no route, nobody is named by a key, and nothing
     * acknowledges what is sent on it.
     */
    give_channels();
    check("a joined channel is a row before anything has been said on it",
          find_text(content(), "SITE") != NULL && find_text(content(), "OPS") != NULL);
    /* What a channel cannot do is said where it matters - under every
     * message sent on one (NO ACK ON CHANNELS, below) - and not as a
     * permanent note over the list. */
    check("the list carries no standing note about channels",
          find_text(content(), "nothing acknowledges a channel message") == NULL);
    /* A channel has no path and cannot have one: a group frame is flooded to
     * whoever holds the key. FLOOD is the whole truth about how it travels,
     * and the route column says that rather than NO PATH, which would read
     * as something that could be learned. */
    check("a channel row says FLOOD in the route column",
          find_text(content(), "FLOOD") != NULL);
    shot("portrait-comms-channels");

    give_channel_message();
    check("a channel message arrives into its own conversation",
          rift_model_unread(&app->model, site_key()) == 1);
    check("and not into a peer's",
          rift_model_unread(&app->model, KEY_B) == 0 &&
              rift_model_unread(&app->model, KEY_A) == 0);
    rift_app_open_conversation(app, site_key());
    pump(80);
    check("the channel thread opens", rift_comms_open_peer(app) != NULL &&
                                          strcmp(rift_comms_open_peer(app), site_key()) == 0);
    check("and reading it clears the badge", rift_model_unread(&app->model, site_key()) == 0);
    check("the message is there", find_text(content(), "tilbake") != NULL);
    check("and so is ours", find_text(content(), "mottatt") != NULL);
    /* The sender's name came out of the payload and nothing signs it, so it
     * is drawn as a claim rather than the way a peer_name is. */
    check("the sender's name is marked as a claim",
          find_text(content(), "HYTTA?") != NULL);
    /* And in its identity accent (DS §37.3): the claimed name hashes to a
     * hue of the palette, the label carries that hue, and the rule beside
     * the message is the same one. The words are what say who. */
    {
        lv_obj_t *who = find_text(content(), "HYTTA?");
        lv_color_t want = pos_identity_hue(rift_ident_hash("HYTTA"));
        lv_color_t got = who ? lv_obj_get_style_text_color(who, 0) : lv_color_black();

        check("in the sender's identity accent", who && lv_color_eq(got, want));
        check("which is not the caption's own colour",
              !lv_color_eq(want, pos_theme_color(POS_COLOR_TEXT_SECONDARY)));
        /* The list pane comes before the thread pane, so the first SITE is
         * the conversation row's name, whose row starts with the mark. */
        check("and the channel's row carries an identity mark before its glyph",
              lv_obj_get_width(kid(ancestor(find_exact(content(), "SITE"), 1), 0)) ==
                  RIFT_IDENT_W);
    }
    /* The header says what a channel is reached by - the hash that actually
     * goes on the air - and never a hop count. */
    check("the header names the channel and how it travels",
          find_text(content(), "CHANNEL") != NULL && find_text(content(), "HASH 8c") != NULL);
    /* Nothing claims a delivery. DELIVERED and an ACK age are drawn only in
     * a message caption, and only the open thread draws captions - so with
     * the channel thread open they must be nowhere, even though the direct
     * conversation that has them is still listed above. */
    check("and nothing claims a channel message was delivered",
          find_text(content(), "DELIVERED") == NULL);
    check("nor that one timed out waiting",
          find_text(content(), "NO ACK \xC2\xB7") == NULL);
    check("it says outright that nothing acknowledges it",
          find_text(content(), "NO ACK ON CHANNELS") != NULL);
    check("the composer is usable on a channel", find_text(content(), "SEND") != NULL);
    /* MeshCore writes "<sender>: " into a channel payload, and the caption
     * already names the sender as a claim. The body says it once. */
    check("a channel line is its body, not the sender's name again",
          find_text(content(), "HYTTA: str") == NULL &&
              find_text(content(), "str\xC3\xB8m tilbake") != NULL);
    check("and this node's own line is what the reader typed",
          find_text(content(), "K230-A: mottatt") == NULL &&
              find_exact(content(), "mottatt") != NULL);
    check("which the list's preview says too", find_text(content(), "you: mottatt") != NULL);
    shot("portrait-comms-channel-thread");

    /* An empty channel says what will happen rather than nothing. */
    rift_app_open_conversation(app, ops_key());
    pump(80);
    check("an empty channel is still somewhere to write",
          find_text(content(), "Nothing on this channel yet") != NULL);
    check("and says who will be able to read it",
          find_text(content(), "holding the same key") != NULL);

    /* ---- the three things unit A found, 2026-09-20 ---------------------- */

    /* 1. The unread pill painted nothing: POS_STYLE_CHIP_RX carries the two
     * colours and not the opacity that makes a chip's fill appear, so the
     * pill took its space in the row and showed neither fill nor readable
     * text. Checked on the widget itself, because that is where the defect
     * was and a row is a poor place to see a missing opacity. */
    {
        lv_obj_t *pill = rift_unread_pill(content());

        rift_unread_pill_set(pill, 3);
        pump(30);
        check("an unread pill is not hidden when there is something to read",
              !lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN));
        check("and says how many", strcmp(lv_label_get_text(pill), "3") == 0);
        check("its fill is actually painted",
              lv_obj_get_style_bg_opa(pill, LV_PART_MAIN) == LV_OPA_COVER);
        check("in the token the handoff asks for",
              lv_color_eq(lv_obj_get_style_bg_color(pill, LV_PART_MAIN),
                          pos_theme_color(POS_COLOR_RADIO_RX)));
        check("and its text is the one meant to sit on that fill",
              lv_color_eq(lv_obj_get_style_text_color(pill, LV_PART_MAIN),
                          pos_theme_color(POS_COLOR_TEXT_ON_ACCENT)));
        /* Colour never carries it alone, and neither does a fill: nothing
         * is drawn at all when there is nothing unread. */
        rift_unread_pill_set(pill, 0);
        check("and nothing is drawn at zero", lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN));
        lv_obj_delete(pill);
    }

    /* 2. The composer lost the keyboard about once a second, because every
     * refresh re-enabled an already-enabled field and lv_group_add_obj()
     * re-adds by removing first. Ten refreshes is ten chances to lose it. */
    rift_app_open_conversation(app, KEY_B);
    pump(80);
    {
        lv_obj_t *field = find_textarea(content());
        int r;

        check("the portrait composer is there", field != NULL);
        pos_input_focus(field);
        pump(40);
        check("and takes the focus", pos_input_focused() == field);
        for (r = 0; r < 10; r++) {
            rift_app_refresh(app);
            pump(40);
        }
        check("ten refreshes later it still has it", pos_input_focused() == field);

        /* 3. What that cost was typing. A character before the refreshes and
         * one after must both arrive, in order. */
        lv_textarea_set_text(field, "");
        pos_input_push_key('h');
        pump(60);
        for (r = 0; r < 8; r++) {
            rift_app_refresh(app);
            pump(40);
        }
        pos_input_push_key('e');
        pump(60);
        for (r = 0; r < 8; r++) {
            rift_app_refresh(app);
            pump(40);
        }
        pos_input_push_key('i');
        pump(60);
        check("typing survives the refreshes in between",
              strcmp(lv_textarea_get_text(field), "hei") == 0);
        lv_textarea_set_text(field, "");
    }

    /* 4. There was no way to start one. The list holds only peers with
     * messages, so a node nobody had written to could not be written to.
     * NODES -> select -> MESSAGE now opens the conversation, and it opens
     * empty: no history is invented for it. */
    tap(tab(1));
    pump(60);
    check("NODES again", app->section == RIFT_SEC_NODES);
    rift_app_select(app, KEY_C);
    pump(80);
    check("a node with nothing ever said to it is selected",
          rift_app_selected(app) && strcmp(rift_app_selected(app)->key, KEY_C) == 0);
    {
        lv_obj_t *msg = action_of(find_text(content(), "MESSAGE"));

        check("the selected row offers MESSAGE", msg != NULL);
        tap(msg);
        pump(100);
        check("which opens COMMS", app->section == RIFT_SEC_COMMS);
        check("on a conversation with that node",
              rift_comms_open_peer(app) && strcmp(rift_comms_open_peer(app), KEY_C) == 0);
        check("the conversation is in the list", find_text(content(), "NO-3241 FO") != NULL);
        check("and it holds nothing", find_text(content(), "Nothing said yet") != NULL);
        /* The one place a reader needs to know the history is the
         * service's and does not outlive its restart: a thread that is
         * empty, which may be empty for that reason. */
        check("saying why that may be", find_text(content(), "keeps no history") != NULL);
        /* The one thing that must not have happened: a row invented to make
         * the thread look started. */
        {
            const struct rift_message *t[4];
            int older = 0;

            check("no message was invented for it",
                  rift_model_thread(&app->model, KEY_C, t, 4, &older) == 0 && older == 0);
        }
        check("but the composer is usable", rift_thread_refusal(app) == NULL);
        {
            lv_obj_t *field = find_textarea(content());

            pos_input_focus(field);
            pump(40);
            pos_input_push_key('x');
            pump(60);
            check("and takes what is typed into it",
                  strcmp(lv_textarea_get_text(field), "x") == 0);
            lv_textarea_set_text(field, "");
        }
        /* The conversation that already had messages is untouched by any of
         * this. */
        rift_app_open_conversation(app, KEY_B);
        pump(80);
        {
            const struct rift_message *t[8];
            int older = 0;

            check("and the existing conversation still holds its messages",
                  rift_model_thread(&app->model, KEY_B, t, 8, &older) == 4);
        }
    }

    tap(tab(1));
    tap(action_of(find_exact(content(), "NET")));
    check("NET is reachable, under NODES", app->section == RIFT_SEC_NET);
    check("and draws the rings, not a placeholder",
          find_text(content(), "not in this build") == NULL &&
              find_exact(content(), "1 DIRECT") != NULL);
    tap(tab(1));
    check("and NODES comes back", app->section == RIFT_SEC_NODES);

    /* ---- the same app, turned ------------------------------------------ */
    use_display(POS_ROTATION_270, PANEL_CORNER);
    pump(120);
    check("landscape splits the section", app->wide);
    check("the list is still there", find_text(content(), "OSLO-01") != NULL);
    check("the selected node's detail is beside it, not pushed",
          !app->detail_open && find_text(content(), "LINK STATE") != NULL);
    check("landscape shows the SNR column portrait has no room for",
          find_text(content(), "SNR") != NULL);
    /* The command line has nothing to hold on NODES - no field, no service
     * fault - so it is not there, and the keys are named in the strip. */
    check("the command line is not drawn where it has nothing to hold",
          !visible(cmdline()));
    check("the keys are named in the strip instead",
          find_text(strip(), "SELECT") != NULL && find_text(strip(), "ENTER MESSAGE") != NULL);
    check("and the section has the 56 px it gave back",
          lv_obj_get_height(content()) ==
              lv_obj_get_content_height(frame()) - lv_obj_get_height(strip()));
    check("and everything is inside the turned body", inside_body(content()));
    {
        /* The detail pane beside the list: its actions under the title,
         * not at the foot of four panels below the fold. */
        lv_obj_t *pane = kid(kid(content(), 1), 1);
        lv_obj_t *msg = action_of(find_text(pane, "MESSAGE"));

        check("the landscape detail's actions are in view without scrolling",
              msg && within(msg, pane));
        check("and their words fit their buttons", labels_overflowing(pane) == 0);
    }
    shot("landscape-nodes");
    {
        lv_area_t list;
        lv_area_t pane;
        lv_obj_t *root = kid(content(), 1);

        lv_obj_update_layout(root);
        lv_obj_get_coords(kid(root, 0), &list);
        lv_obj_get_coords(kid(root, 1), &pane);
        check("the two panes are side by side, list first", list.x2 <= pane.x1);
        check("and both have real width",
              lv_area_get_width(&list) > 300 && lv_area_get_width(&pane) > 300);
    }
    {
        /* A confirmation does not follow the reader into the other shape:
         * turning the panel is a way out, and any way out is Cancel. */
        lv_obj_t *pane = kid(kid(content(), 1), 1);

        tap(action_of(find_exact(pane, "FORGET")));
        check("FORGET asks in the landscape pane too",
              find_text(pane, "Forget ") != NULL);
        use_display(POS_ROTATION_0, PANEL_CORNER);
        pump(120);
        check("turned to portrait, nothing is pushed in its place",
              !app->wide && !app->detail_open && find_text(content(), "Forget ") == NULL);
        use_display(POS_ROTATION_270, PANEL_CORNER);
        pump(120);
        check("and turned back, the pane is not asking either",
              app->wide && find_text(content(), "Forget ") == NULL &&
                  find_text(kid(kid(content(), 1), 1), "MESSAGE") != NULL);
        check("and nothing was asked of the service",
              app->model.node_op.kind == RIFT_ACTION_NONE);
    }
    /* Landscape never pushes a detail screen: the pane is the detail. Enter
     * does what the keyboard is there for instead - it opens the selected
     * node's conversation, and sends nothing. */
    {
        char want[RIFT_KEY_HEX];

        snprintf(want, sizeof(want), "%s", app->selected);
        check("Enter in landscape opens the selected node's conversation",
              rift_nodes_key(app, LV_KEY_ENTER) == 1 && app->section == RIFT_SEC_COMMS &&
                  rift_comms_open_peer(app) && strcmp(rift_comms_open_peer(app), want) == 0);
        check("so nothing is pushed", !app->detail_open);
        check("and nothing is sent", !rift_model_sending(&app->model) &&
                                         !app->model.outbox.failed);
        tap(tab(1));
        pump(60);
    }

    tap(tab(0));
    check("ACTIVITY is laid out in landscape too", app->section == RIFT_SEC_ACTIVITY);
    check("with the same three panels", find_text(content(), "RADIO SERVICE") != NULL &&
                                            find_text(content(), "RECENTLY HEARD") != NULL &&
                                            find_text(content(), "MESH ACTIVITY") != NULL);
    check("their captions drawn whole side by side too",
          caption_unclipped("RADIO SERVICE") && caption_unclipped("RECENTLY HEARD"));
    check("and inside the body", inside_body(content()));
    shot("landscape-activity");

    /* ---- SYSTEM, turned ------------------------------------------------- */
    tap(tab(RIFT_SEC_SYSTEM));
    pump(120);
    check("SYSTEM is laid out in landscape too", app->section == RIFT_SEC_SYSTEM);
    check("its panels side by side, captions whole",
          caption_unclipped("DEVICE") && caption_unclipped("ADDRESSING") &&
              caption_unclipped("SOUND") && caption_unclipped("CHANNELS"));
    /* DEVICE heads the left column, so the ADVERT buttons are on screen as
     * SYSTEM opens, not under a scroll. */
    check("the ADVERT buttons are in view without scrolling",
          within(action_of(find_text(content(), "ADVERT NEAR")), app->system_root) &&
              within(action_of(find_text(content(), "ADVERT MESH")), app->system_root));
    check("SYSTEM is inside the body", inside_body(content()));
    check("with every word inside its button", labels_overflowing(content()) == 0);
    shot("landscape-system");

    /* ---- COMMS, turned --------------------------------------------------- */
    tap(tab(2));
    pump(80);
    check("COMMS splits too", app->section == RIFT_SEC_COMMS && app->wide);
    check("the list is on the left", find_text(content(), "HYTTA") != NULL);
    rift_app_open_conversation(app, KEY_B);
    pump(80);
    check("and the thread beside it", find_text(content(), "Fint, ser deg") != NULL);
    /* The details pane is not there until asked for (DS §37.2): the thread
     * has its width, and the header line carries the route compressed. */
    check("the details pane is closed by default",
          !app->details_open && find_text(content(), "OF 2 SENT") == NULL);
    check("and the header carries the route", find_text(thread_pane(), "OSLO-01") != NULL);
    check("and says how to open the pane", find_text(thread_pane(), "DETAILS") != NULL);
    tap(kid(kid(thread_pane(), 0), 0));
    check("a tap on the header opens it", app->details_open);
    check("with the route pane's own heading", find_text(content(), "ROUTE") != NULL);
    /* The route pane's, not the conversation list's column header. */
    check("drawn whole", caption_unclipped_in(kid(kid(content(), 2), 2), "ROUTE"));
    check("the delivery tally is this app's arithmetic, and says what it counts",
          find_text(content(), "OF 2 SENT") != NULL);
    check("everything is inside the turned body", inside_body(content()));
    shot("landscape-comms-details");

    /* The same pane, for a channel. There is no chain to draw and no
     * delivery to count, and it says so rather than drawing an empty one. */
    rift_app_open_conversation(app, site_key());
    pump(80);
    check("a channel opens in landscape too",
          find_text(content(), "SITE") != NULL && find_text(content(), "tilbake") != NULL);
    check("the route pane says a channel is a key and not a route",
          find_text(content(), "No route: a channel is a shared key") != NULL);
    check("and gives the hash that goes on the air",
          find_text(content(), "HASH 8c") != NULL);
    /* The route pane's own words, not the list's: the hop chain a peer gets
     * is replaced by what a channel actually is. */
    check("and nothing about hops in what it says instead",
          find_text(content(), "UNKNOWN HOP") == NULL);
    /* The tally counts what was sent and then stops: there is no DELIVERED
     * and no NO ACK, because neither is a number this protocol can produce
     * for a channel, and "0 DELIVERED" would read as a failure rather than
     * as something that cannot be measured. */
    check("the tally counts what was sent", find_text(content(), "1 SENT") != NULL);
    check("and says nothing acknowledges it",
          find_text(content(), "NOTHING ACKNOWLEDGES A CHANNEL") != NULL);
    check("without a delivery count", find_text(content(), "DELIVERED") == NULL);
    check("the turned channel view stays inside the body", inside_body(content()));
    shot("landscape-comms-channel");
    rift_app_open_conversation(app, KEY_B);
    pump(80);
    /* The command line is the composer in landscape (handoff §8), so the
     * thread does not carry a second one. */
    check("the command line has become the composer", app->composer != NULL &&
                                                          visible(lv_obj_get_parent(app->composer)));
    check("naming the peer it is addressing", find_text(cmdline(), "TO HYTTA") != NULL);
    check("the thread shows no second composer", find_text(content(), "SEND") == NULL);
    {
        /* The composer took its 56 px from under the thread after the
         * thread had been scrolled to its end, and hid the newest message -
         * until the command line was decided first and a thread whose pane
         * changed height is read at its end again. */
        lv_obj_t *newest = find_text(thread_pane(), "to linjer");

        check("the newest message is in view above the composer",
              newest && within(newest, ancestor(newest, 4)));
    }
    /* Closed again with the same tap, and the thread has its width back. */
    tap(kid(kid(thread_pane(), 0), 0));
    check("a second tap on the header closes the details pane",
          !app->details_open && find_text(content(), "OF 2 SENT") == NULL);
    check("and the thread pane is most of the width",
          lv_obj_get_width(thread_pane()) > lv_obj_get_width(content()) * 3 / 4);
    shot("landscape-comms");
    {
        /* The same, isolated from the thread's own re-scroll: in the one
         * refresh that opens a conversation, with no timer pass after it.
         * The command line is decided before the thread is laid out, so the
         * thread is scrolled to its end against the height it will have. */
        lv_obj_t *newest;

        app->have_conv = 0;
        rift_app_refresh(app);
        pump(60);
        check("with no conversation open the command line is not drawn", !visible(cmdline()));
        rift_app_open_conversation(app, KEY_E);
        newest = find_text(thread_pane(), "the newest line");
        check("opening one draws the composer, and the newest message above it, in one pass",
              visible(cmdline()) && newest && within(newest, ancestor(newest, 4)));
        pump(80);
        rift_app_open_conversation(app, KEY_B);
        pump(80);
    }

    /* TAB moves between the list and the composer - the design's "TAB PANE"
     * (handoff §9), and the only way to reach the composer from the list
     * without touching the panel. Pushed through the real key stream, not
     * by calling the handler, because what is being tested is that the key
     * arrives where this app thinks it does. */
    /* The refresh defect would have taken this composer too - the thread's
     * field is re-enabled on every refresh whichever orientation is up, and
     * re-adding it to the group moves the focus off whatever had it. */
    {
        int r;

        pos_input_focus(app->composer);
        pump(40);
        check("the landscape composer takes the focus", pos_input_focused() == app->composer);
        for (r = 0; r < 10; r++) {
            rift_app_refresh(app);
            pump(40);
        }
        check("and keeps it across ten refreshes", pos_input_focused() == app->composer);
        lv_textarea_set_text(app->composer, "");
        pos_input_push_key('z');
        pump(60);
        for (r = 0; r < 8; r++) {
            rift_app_refresh(app);
            pump(40);
        }
        pos_input_push_key('y');
        pump(60);
        check("and typing into it survives them",
              strcmp(lv_textarea_get_text(app->composer), "zy") == 0);
        lv_textarea_set_text(app->composer, "");
    }

    pos_input_focus(app->keysink);
    /* Long enough for the timer to repaint: the focus change is followed,
     * not acted on, from inside the event (see on_composer_focus). */
    pump(160);
    check("the list has the focus to begin with", pos_input_focused() == app->keysink);
    check("and the hint says how to reach the field",
          find_text(cmdline(), "TAB TO WRITE") != NULL);
    pos_input_push_key(LV_KEY_NEXT);
    pump(200);
    check("TAB moves it to the composer", pos_input_focused() == app->composer);
    check("which the app follows rather than decides", app->composer_focused);
    check("and the hint changes to what the keys do there",
          find_text(cmdline(), "ENTER SEND") != NULL);
    /* Esc is the way back, because TAB inside a text area types a tab. It
     * clears first, so nothing typed is lost by one keystroke. */
    pos_input_push_key('x');
    pump(120);
    check("what is typed reaches the composer",
          lv_textarea_get_text(app->composer)[0] == 'x');
    pos_input_push_key(LV_KEY_ESC);
    pump(120);
    check("Esc clears it and keeps the focus", pos_input_focused() == app->composer &&
                                                   lv_textarea_get_text(app->composer)[0] ==
                                                       '\0');
    pos_input_push_key(LV_KEY_ESC);
    pump(160);
    check("and Esc on an empty field gives the list its focus back",
          pos_input_focused() == app->keysink);
    check("which the app follows too", !app->composer_focused);

    /* Leaving the conversation takes the composer away again: a command line
     * that still offered to send would be addressing nobody. */
    tap(tab(1));
    pump(80);
    check("leaving COMMS puts the caption back",
          !visible(lv_obj_get_parent(app->composer)));
    tap(tab(2));
    pump(80);

    use_display(POS_ROTATION_0, PANEL_CORNER);
    pump(120);
    check("turning back gives the single column", !app->wide);
    /* One container a screen. Turning the panel twice must not build a
     * second set of them. */
    check("without making a second set of panels",
          lv_obj_get_child_count(content()) == (uint32_t)RIFT_SEC_COUNT);
    check("and the portrait composer is back under the thread",
          find_text(content(), "SEND") != NULL);

    app_stop();

    /* ---- open, leave, open again --------------------------------------- */
    for (round = 0; round < 3; round++) {
        app_start();
        check("the app opens again from nothing", app != NULL);
        quiet_client();
        give_nodes();
        give_service();
        check("with its sections built once", lv_obj_get_child_count(content()) == (uint32_t)RIFT_SEC_COUNT);
        check("and the mesh on screen", find_text(content(), "OSLO-01") != NULL);
        /* COMMS is reached on every round, so its IPC client, its rows and
         * its composer are created and destroyed three times over. A
         * conversation opened in the previous round must not survive into
         * this one: the app is new. */
        check("no conversation is carried over from the last time",
              rift_comms_open_peer(app) == NULL);
        give_messages();
        rift_app_show_section(app, RIFT_SEC_COMMS);
        pump(60);
        check("COMMS builds from nothing again", find_text(content(), "HYTTA") != NULL);
        rift_app_open_conversation(app, KEY_B);
        pump(60);
        check("and its thread with it", find_text(content(), "Fint, ser deg") != NULL);
        /* The channels too: the list, a thread on one, and a channel removed
         * while the rows for it are on screen. A row holds its conversation
         * key and its owner, so a removal that left one behind would be a
         * tap into a channel the model no longer has. */
        check("no channel is carried over either", app->model.channel_count == 0);
        give_channels();
        give_channel_message();
        pump(60);
        check("the channels build from nothing too", find_text(content(), "SITE") != NULL);
        rift_app_open_conversation(app, site_key());
        pump(60);
        check("and a channel thread opens in a fresh app",
              find_text(content(), "tilbake") != NULL);
        {
            cJSON *o = cJSON_Parse("{\"reason\":\"removed\",\"channel\":{\"channel\":0}}");

            rift_model_apply_event(&app->model, "mesh.channel", o);
            cJSON_Delete(o);
            rift_app_refresh(app);
            pump(80);
        }
        check("removing the open channel takes it out of the model",
              rift_model_channel(&app->model, 0) == NULL);
        /* The messages on it happened, so the thread is still readable; what
         * is gone is the channel, and the header says so rather than the
         * screen going blank or the app following a dead row. */
        check("but the thread it was in is still drawn",
              find_text(content(), "tilbake") != NULL);
        check("and says the channel is no longer joined",
              find_text(content(), "NOT JOINED ANY MORE") != NULL);
        /* A different channel takes the emptied slot while the old thread is
         * open. The thread stays the old channel's - its messages, its
         * header - and there is nowhere to write from it: mesh.send names a
         * slot, and a reply here would reach the new channel's audience. */
        {
            cJSON *o = cJSON_Parse("{\"reason\":\"added\",\"channel\":{\"channel\":0,"
                                   "\"name\":\"NYTT\",\"channel_hash\":\"e5\","
                                   "\"key_bits\":256,\"text_limit\":147}}");

            rift_model_apply_event(&app->model, "mesh.channel", o);
            cJSON_Delete(o);
            rift_app_refresh(app);
            pump(80);
        }
        check("a new channel in the slot leaves the open thread the old one's",
              rift_comms_open_peer(app) && strcmp(rift_comms_open_peer(app), site_key()) == 0 &&
                  find_text(content(), "tilbake") != NULL &&
                  find_text(content(), "NOT JOINED ANY MORE") != NULL);
        check("the new channel is a row of its own", find_text(content(), "NYTT") != NULL);
        check("the composer says there is nowhere to write",
              find_text(content(), "This channel is not joined any more") != NULL);
        rift_comms_submit(app, "svar");
        pump(40);
        check("and a reply from the old thread is refused, not sent to the new channel",
              app->model.outbox.failed && !rift_model_sending(&app->model) &&
                  strstr(app->model.outbox.error, "not joined") != NULL);
        rift_model_send_clear(&app->model);
        rift_app_open_conversation(app, ops_key());
        pump(60);
        check("another channel still opens afterwards",
              find_text(content(), "Nothing on this channel yet") != NULL);
        /* The app goes with a confirmation up and a refused request on
         * screen: the detail's objects, its handlers and the request state
         * all go with it. */
        rift_app_show_section(app, RIFT_SEC_NODES);
        rift_app_select(app, KEY_B);
        rift_app_open_detail(app, 1);
        pump(60);
        tap(action_of(find_exact(content(), "FORGET")));
        check("a confirmation can be up when the app goes",
              find_text(content(), "Forget HYTTA?") != NULL);
        rift_ipc_send_advert(&app->ipc, 1);
        app_stop();
        check("and leaves nothing of itself behind",
              lv_obj_get_child_count(g_content) == 0u);
    }
    /* The DM sound, from the history on opening to the switch that turns it
     * off, and the whole mesh at once: each in an app of its own. */
    sound_session(g_state_dir);
    landscape_start_session();
    scale_session();
    /* feat/rift-management: who said it before what was said, finding a node
     * and the zero-hop repeaters, and NET. */
    sender_session();
    reliability_session();
    find_session();
    net_session();
    /* feat/rift-rx-log: the receive log behind ACTIVITY (DS §54). */
    rxlog_session();
    repeater_session();
    repeater_control_session();
    map_session();
    manage_session();
    manage_live_session();
    comms_usability_session();
    public_mute_session();
    text_size_session();
    /* feat/rift-emoji-picker: the composer's emoji button and picker. */
    emoji_picker_session();
    /* feat/rift-background-lifecycle: RIFT kept behind other screens, its
     * mark, CLOSE RIFT, and the navigation row's targets (DS §51). */
    background_session();
    navigation_session();
    background_live_session();

    /* A destroyed app's timer must be gone: one more pass into a freed
     * block is the whole point of the round trip. */
    pump(600);
    check("and nothing is still running after it", app == NULL && rift_app_session() == NULL);

    check("the app never sent the shell home", home_calls == 0);
    (void)hint_calls;

    if (shots_dir) {
        printf("rift_app_test: %d screenshot(s) in %s\n", shots_taken, shots_dir);
    }
    printf("rift_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
