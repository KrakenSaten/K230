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
#include "rift_device.h"
#include "rift_find.h"
#include "rift_graph.h"
#include "rift_manage.h"
#include "rift_net.h"
#include "rift_netview.h"
#include "rift_nodes.h"
#include "rift_sound.h"
#include "rift_store.h"
#include "rift_test_clock.h"
#include "rift_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
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
void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user),
                                 void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
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
static int fake_stops;
static int fake_available(void)
{
    return 1;
}
static int fake_play(int volume_percent)
{
    fake_plays++;
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

static void app_stop(void)
{
    app_rift.destroy(app);
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
             "\"type\":2,\"path_known\":true,\"hops\":8,\"direct\":false,"
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
                 "{\"message\":{\"id\":%d,\"direction\":\"%s\",\"peer_public_key\":\"" KEY_D "\","
                 "\"peer_name\":\"S\xC3\xB8rlandet\",\"text\":\"%s %d\",\"state\":\"%s\","
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
    check("nor does a channel message", fake_plays == plays);
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

    /* The setting, on ACTIVITY, in the Settings app's shape. */
    rift_app_show_section(app, RIFT_SEC_ACTIVITY);
    pump(60);
    toggle = action_of(find_exact(content(), "ON"));
    check("ACTIVITY has the DM sound's switch", toggle != NULL &&
                                                    find_text(content(), "Sound for a new DM") != NULL);
    check("a 56 px action", toggle && lv_obj_get_height(toggle) == RIFT_TOUCH_H);
    check("saying what it does", find_text(content(), "One short sound for a new direct message") !=
                                     NULL);
    check("with its caption drawn whole", caption_unclipped("NOTIFY"));
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
    check("the setting survives closing and opening", app->prefs.dm_sound == 0 &&
                                                          find_exact(content(), "OFF") != NULL);
    app_stop();

    /* A preferences file that cannot be written: the choice holds for the
     * session, and the screen says it will not outlast it. */
    setenv("POCKETOS_STATE_DIR", "/proc/rift-app-test-cannot-write", 1);
    app_start();
    quiet_client();
    check("a store that cannot be read is the defaults", app->prefs.dm_sound == 1);
    tap(action_of(find_exact(content(), "ON")));
    check("an unwritable store keeps the choice for the session",
          app->prefs.dm_sound == 0 && find_text(content(), "Not saved") != NULL);
    app_stop();
    setenv("POCKETOS_STATE_DIR", state_dir, 1);
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
    check("the strip is a data row with the way back in it",
          lv_obj_get_height(strip()) == RIFT_ROW_H && app->back && visible(app->back));
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
         * more than half the display, and shows more than half again the
         * eleven messages the handoff's layout showed. */
        check("and shows at least sixteen one-line messages above the composer", rows >= 16);
        check("the thread is more than half the display", thread_share(scroll) > 50.0);
        check("its header is a header row, not a data row",
              lv_obj_get_height(kid(kid(thread_pane(), 0), 0)) == rift_header_row_h());
        check("the strip is a data row in landscape", lv_obj_get_height(strip()) == RIFT_ROW_H);
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
    rift_app_show_section(app, RIFT_SEC_ACTIVITY);
    pump(120);

    /* Found on unit B: ACTIVITY opened scrolled down. A field in a closed
     * form was the first object the focus group got, LVGL focused it, and
     * focusing scrolls. Closed forms keep their fields out of the group. */
    check("closed forms keep their fields out of the focus group",
          lv_obj_get_group(rift_manage_name_field(app)) == NULL &&
              lv_obj_get_group(rift_manage_key_field(app)) == NULL &&
              lv_obj_get_group(rift_device_rename_field(app)) == NULL);
    check("so the keys go to the app's key sink, and ACTIVITY is read from its top",
          pos_input_focused() == app->keysink && lv_obj_get_scroll_y(app->activity_root) == 0);
    check("ACTIVITY lists the channels the service holds",
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
    rift_app_show_section(app, RIFT_SEC_ACTIVITY);
    pump(150);
    check("leaving ACTIVITY is Cancel for a LEAVE left up",
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
    rift_app_show_section(app, RIFT_SEC_ACTIVITY);
    pump(150);
    check("leaving ACTIVITY closes the form and empties its fields",
          !visible(rift_manage_name_field(app)) &&
              lv_textarea_get_text(rift_manage_key_field(app))[0] == '\0' &&
              lv_textarea_get_text(rift_manage_name_field(app))[0] == '\0');

    /* THIS DEVICE: the name, and the path hash size. */
    check("THIS DEVICE has RENAME", find_exact(content(), "RENAME") != NULL);
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
    check("a name set by the configuration cannot be renamed here, and says where it is set",
          lv_obj_has_state(action_of(find_exact(content(), "RENAME")), LV_STATE_DISABLED) &&
              find_text(content(), "MESHCORED_NAME") != NULL);
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
    rift_app_show_section(app, RIFT_SEC_ACTIVITY);
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

        tap(kid(row, 1));
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
    check("THIS DEVICE shows the new name, and when peers will see it",
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

    tap(action_of(find_exact(content(), "MESSAGE")));
    check("MESSAGE opens COMMS on the node, and sends nothing",
          app->section == RIFT_SEC_COMMS && rift_comms_open_peer(app) &&
              strcmp(rift_comms_open_peer(app), KEY_R2) == 0 && !rift_model_sending(&app->model));
    rift_app_show_section(app, RIFT_SEC_NET);
    pump(60);
    tap(action_of(find_exact(content(), "DETAIL \xE2\x80\xBA")));
    check("DETAIL opens the node in NODES", app->section == RIFT_SEC_NODES && app->detail_open);
    rift_app_show_section(app, RIFT_SEC_NET);
    pump(60);
    check("Back from NET is ACTIVITY", app_rift.back(app) == 1 &&
                                           app->section == RIFT_SEC_ACTIVITY);
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
    check("the strip is a 56 px navigation row, not a 36 px data row",
          lv_obj_get_height(strip()) == RIFT_TOUCH_H);
    check("and so is the command line", lv_obj_get_height(cmdline()) == RIFT_TOUCH_H);
    /* The key sink moved out of the command line so the line can go away
     * without taking the keys with it. It is 1 px, takes no taps, and is
     * never hidden. */
    check("the key sink is outside the command line",
          app->keysink && lv_obj_get_parent(app->keysink) == frame());
    check("takes no taps", !lv_obj_has_flag(app->keysink, LV_OBJ_FLAG_CLICKABLE));
    check("and is never hidden", visible(app->keysink));
    check("all four sections are in the navigation",
          find_text(strip(), "ACTIVITY") && find_text(strip(), "NODES") &&
              find_text(strip(), "COMMS") && find_text(strip(), "NET"));
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
              caption_unclipped("RADIO SERVICE") && caption_unclipped("THIS DEVICE") &&
                  caption_unclipped("RECENTLY HEARD") && caption_unclipped("MESH ACTIVITY"));
    }

    /* The traffic the service counted, in its own words. */
    check("the service's transmit counts are drawn, outcomes kept apart",
          find_text(content(), "TX 3 OK \xC2\xB7 1 FAILED") != NULL);

    /* ---- ADVERT: only on a press ------------------------------------------ */
    {
        lv_obj_t *near = action_of(find_text(content(), "ADVERT NEAR"));
        lv_obj_t *mesh = action_of(find_text(content(), "ADVERT MESH"));

        check("THIS DEVICE offers both adverts", near != NULL && mesh != NULL);
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
         * not keep is one nothing can be sent to. */
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
          find_text(content(), "joined on ACTIVITY, under CHANNELS") != NULL);

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
        rift_app_open_conversation(app, KEY_D);
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

    tap(tab(3));
    check("NET is reachable", app->section == RIFT_SEC_NET);
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
    check("with the same four panels", find_text(content(), "RADIO SERVICE") != NULL &&
                                           find_text(content(), "THIS DEVICE") != NULL &&
                                           find_text(content(), "RECENTLY HEARD") != NULL &&
                                           find_text(content(), "MESH ACTIVITY") != NULL);
    check("their captions drawn whole side by side too",
          caption_unclipped("RADIO SERVICE") && caption_unclipped("THIS DEVICE") &&
              caption_unclipped("RECENTLY HEARD"));
    /* THIS DEVICE heads the right column, so the one action on ACTIVITY is
     * on screen as it opens, not under a scroll. */
    check("the ADVERT buttons are in view without scrolling",
          within(action_of(find_text(content(), "ADVERT NEAR")), kid(content(), 0)) &&
              within(action_of(find_text(content(), "ADVERT MESH")), kid(content(), 0)));
    check("and inside the body", inside_body(content()));
    shot("landscape-activity");

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
        rift_app_open_conversation(app, KEY_D);
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
    /* ACTIVITY, NODES, COMMS and the one placeholder NET uses. Turning the
     * panel twice must not build a second set of them. */
    check("without making a second set of panels",
          lv_obj_get_child_count(content()) == 4u);
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
        check("with its sections built once", lv_obj_get_child_count(content()) == 4u);
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
    find_session();
    net_session();
    manage_session();
    manage_live_session();
    text_size_session();

    /* A destroyed app's timer must be gone: one more pass into a freed
     * block is the whole point of the round trip. */
    pump(600);
    check("and nothing is still running after it", app == NULL);

    check("the app never sent the shell home", home_calls == 0);
    (void)hint_calls;

    if (shots_dir) {
        printf("rift_app_test: %d screenshot(s) in %s\n", shots_taken, shots_dir);
    }
    printf("rift_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
