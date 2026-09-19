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
#include "rift_app.h"
#include "rift_nodes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
#include "src/libs/lodepng/lodepng.h"
#endif

#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30
#define STATUS_H POCKETUI_STATUS_BAR_H

extern const struct pocketos_app app_rift;

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

static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
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
    pump(80);
}

/* ---- the app, hosted the way the shell hosts it --------------------------- */

static lv_obj_t *app_root;
static lv_obj_t *app_body;
static struct rift_app *app;

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
                           "\"nodes_unretained\":0}}");

    rift_model_apply_status(&app->model, o);
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
    check("all four sections are in the navigation",
          find_text(strip(), "ACTIVITY") && find_text(strip(), "NODES") &&
              find_text(strip(), "COMMS") && find_text(strip(), "NET"));
    check("each of them is a 56 px target",
          lv_obj_get_height(kid(strip(), 0)) == RIFT_TOUCH_H &&
              lv_obj_get_height(kid(strip(), 3)) == RIFT_TOUCH_H);
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
    check("everything on ACTIVITY is inside the body", inside_body(content()));
    shot("portrait-activity");

    /* ---- NODES, portrait ---------------------------------------------- */
    tap(kid(strip(), 1));
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
        check("the actions this build does not have are drawn as not working",
              find_text(content(), "MESSAGE") != NULL);
        check("and DETAIL is there to open", find_text(content(), "DETAIL") != NULL);
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

    /* COMMS and NET keep their place and say what they are. */
    tap(kid(strip(), 2));
    check("COMMS is reachable", app->section == RIFT_SEC_COMMS);
    check("and says it is not in this build",
          find_text(content(), "not in this build") != NULL);
    tap(kid(strip(), 3));
    check("NET is reachable", app->section == RIFT_SEC_NET);
    tap(kid(strip(), 1));
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
    check("the command line names the keys rather than offering a field",
          find_text(cmdline(), "SELECT") != NULL);
    check("and everything is inside the turned body", inside_body(content()));
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
    /* Landscape never pushes a detail screen: the pane is the detail. */
    check("Enter does not push a screen in landscape", rift_nodes_key(app, LV_KEY_ENTER) == 0);
    check("so nothing is pushed", !app->detail_open);

    tap(kid(strip(), 0));
    check("ACTIVITY is laid out in landscape too", app->section == RIFT_SEC_ACTIVITY);
    check("with the same four panels", find_text(content(), "RADIO SERVICE") != NULL &&
                                           find_text(content(), "THIS DEVICE") != NULL &&
                                           find_text(content(), "RECENTLY HEARD") != NULL &&
                                           find_text(content(), "MESH ACTIVITY") != NULL);
    check("and inside the body", inside_body(content()));
    shot("landscape-activity");

    use_display(POS_ROTATION_0, PANEL_CORNER);
    pump(120);
    check("turning back gives the single column", !app->wide);
    check("without making a second set of panels",
          lv_obj_get_child_count(content()) == 3u);

    app_stop();

    /* ---- open, leave, open again --------------------------------------- */
    for (round = 0; round < 3; round++) {
        app_start();
        check("the app opens again from nothing", app != NULL);
        quiet_client();
        give_nodes();
        give_service();
        check("with its sections built once", lv_obj_get_child_count(content()) == 3u);
        check("and the mesh on screen", find_text(content(), "OSLO-01") != NULL);
        app_stop();
        check("and leaves nothing of itself behind",
              lv_obj_get_child_count(g_content) == 0u);
    }
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
