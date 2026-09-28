/*
 * The DOORS launcher's folders (app groups, ui/shell/home.h) in the running
 * launcher: real LVGL, a real pointer and the real key stream, the art read
 * from the source tree. The shell is not linked; the test builds the
 * launcher itself, from registries of its own:
 *
 *   - today's eighteen apps: one Games cell, Fleet, Radar and Timber behind
 *     it, taps and keys open it, open a game, go back; the focus comes back
 *     to the Games cell; the keys stop at the edges; Esc at home does
 *     nothing; detached, the keys reach nothing;
 *   - no game installed (no Games cell), one game (still a folder), a game
 *     not installed (not in it);
 *   - thirty-three games (thirty more rows through home_layout.c's test
 *     seam): the folder's page scrolls and the keys bring the focus into
 *     view;
 *   - landscape, the folder opened again as a rotation restart does;
 *   - fifty open/back rounds and twenty launcher rebuilds: the objects on
 *     the screen and the art held come back to where they started.
 *
 * Built by ui/shell/CMakeLists.txt (host only), run by
 * tests/launcher_folder_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "art.h"
#include "home.h"
#include "home_layout.h"
#include "pocketui.h"
#include "pos_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_W 568
#define PANEL_H 1232

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

/* ---- what the shell would provide ------------------------------------------ */

int pocketos_shell_reduced_motion(void)
{
    return 0;
}

static const struct pocketos_app *opened;
static int opens;
static int locks;
static int controls;

static void on_open(const struct pocketos_app *app)
{
    opened = app;
    opens++;
}

static void on_lock(void)
{
    locks++;
}

static void on_controls(void)
{
    controls++;
}

static const struct home_actions actions = { on_open, on_lock, on_controls };

/* ---- a display that draws nowhere, and one finger ---------------------- */

static uint8_t draw_buf[PANEL_H * 40 * 2];
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;
static lv_display_t *disp;
static lv_obj_t *content;

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

static void tap_area(const lv_area_t *a)
{
    finger_point.x = a->x1 + lv_area_get_width(a) / 2;
    finger_point.y = a->y1 + lv_area_get_height(a) / 2;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
    finger_point.x = 0;
    finger_point.y = 0;
}

static void key(pos_key_t k)
{
    int t;

    pos_input_push_key(k);
    for (t = 0; t < 1000 && pos_input_queued() > 0; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
    pump(40);
}

static int objects(lv_obj_t *o)
{
    uint32_t i;
    int n = 1;

    for (i = 0; i < lv_obj_get_child_count(o); i++) {
        n += objects(lv_obj_get_child(o, (int32_t)i));
    }
    return n;
}

static const char *focus(void)
{
    const char *id = home_focus_id(NULL);

    return id ? id : "(none)";
}

static int focus_is(const char *id)
{
    return strcmp(focus(), id) == 0;
}

static int shown(void)
{
    bool s = false;

    home_focus_id(&s);
    return s;
}

static int on_page(const char *app_id)
{
    lv_area_t a;

    return home_cell_area(app_id, &a);
}

/* ---- the registries -------------------------------------------------------- */

#define MAX_APPS 64
static struct pocketos_app defs[MAX_APPS];
static const struct pocketos_app *registry[MAX_APPS];
static char extra_ids[30][8];

static const char *const today[] = { "radio", "system", "fleet", "radar", "timber", "notes",
                                     "clock", "calendar", "calculator", "settings", "wave", "rift",
                                     "files", "camera", "browser", "recorder", "vision", "zabbix" };
#define NTODAY ((int)(sizeof(today) / sizeof(today[0])))

/* The registry from ids, leaving out those in `skip` (a space-separated
 * list, NULL for none), with `extras` of the test's own games after them. */
static size_t make_registry(const char *skip, int extras)
{
    size_t n = 0;
    int k;

    for (k = 0; k < NTODAY; k++) {
        char pad[32];

        snprintf(pad, sizeof(pad), " %s ", today[k]);
        if (skip) {
            char list[128];

            snprintf(list, sizeof(list), " %s ", skip);
            if (strstr(list, pad)) {
                continue;
            }
        }
        memset(&defs[n], 0, sizeof(defs[n]));
        defs[n].id = today[k];
        defs[n].name = today[k];
        defs[n].icon = LV_SYMBOL_FILE;
        registry[n] = &defs[n];
        n++;
    }
    for (k = 0; k < extras && k < 30; k++) {
        snprintf(extra_ids[k], sizeof(extra_ids[k]), "g%02d", k + 1);
        memset(&defs[n], 0, sizeof(defs[n]));
        defs[n].id = extra_ids[k];
        defs[n].name = extra_ids[k];
        defs[n].icon = LV_SYMBOL_PLAY;
        registry[n] = &defs[n];
        n++;
    }
    return n;
}

/* ---- the launcher, built as the shell builds it ------------------------------ */

static void use_display(bool landscape)
{
    struct pos_display_geometry g;
    struct pos_panel panel;

    memset(&panel, 0, sizeof(panel));
    panel.width = PANEL_W;
    panel.height = PANEL_H;
    panel.corners.top_left = panel.corners.top_right = 30;
    panel.corners.bottom_left = panel.corners.bottom_right = 30;
    pos_display_geometry_init(&g, &panel, landscape ? POS_ROTATION_270 : POS_ROTATION_0);
    pocketui_set_display_geometry(&g);
    lv_display_set_resolution(disp, g.width, g.height);
    lv_obj_set_size(content, g.width, g.height);
    lv_obj_set_pos(content, 0, 0);
    pump(20);
}

static void build(size_t napps, bool landscape)
{
    /* The status cluster's box, as the shell hands it over (DS §36). */
    lv_area_t keepout;

    use_display(landscape);
    keepout.x1 = (landscape ? PANEL_H : PANEL_W) - (landscape ? 50 : 30) - 113;
    keepout.y1 = 14;
    keepout.x2 = keepout.x1 + 113 - 1;
    keepout.y2 = 14 + 44 - 1;
    opened = NULL;
    home_create(content, registry, napps, landscape, &keepout, &actions);
    home_keys_attach();
    pump(60);
}

static void teardown(void)
{
    home_destroy();
    pump(40);
}

static int tap_folder(void)
{
    lv_area_t a;

    if (!home_folder_area("games", &a)) {
        return 0;
    }
    tap_area(&a);
    return 1;
}

static int tap_back(void)
{
    lv_area_t a;

    if (!home_folder_back_area(&a)) {
        return 0;
    }
    tap_area(&a);
    return 1;
}

static int tap_app(const char *id)
{
    lv_area_t a;

    if (!home_cell_area(id, &a)) {
        return 0;
    }
    tap_area(&a);
    return 1;
}

/* ---- the cases ------------------------------------------------------------------ */

static void test_today(void)
{
    struct home_info hi;
    lv_area_t a;
    int k;

    build(make_registry(NULL, 0), false);
    home_info(&hi);
    check("today: eighteen apps in sixteen cells, one of them a folder",
          hi.apps == 18 && hi.cells == 16 && hi.folders == 1 && hi.icons_art == 16 && hi.icons_fallback == 0);
    check("today: the Games cell is on the launcher", home_folder_area("games", &a) && lv_area_get_width(&a) >= 64);
    check("today: Fleet, Radar and Timber are not", !on_page("fleet") && !on_page("radar") && !on_page("timber"));
    check("today: the other apps are", on_page("notes") && on_page("rift") && on_page("vision") && on_page("zabbix"));
    check("today: Games holds three", home_folder_size("games") == 3 && home_folder_size("nope") == -1);
    check("today: the launcher has the keys", pos_input_focused() != NULL);

    /* A finger. */
    check("a tap on Games", tap_folder());
    check("opens the folder", home_folder_current() && strcmp(home_folder_current(), "games") == 0);
    check("whose page has Fleet, Radar and Timber", on_page("fleet") && on_page("radar") && on_page("timber"));
    check("and nothing from the launcher's page", !on_page("notes") && !on_page("rift"));
    check("and a way back", home_folder_back_area(&a) && lv_area_get_width(&a) >= 64);
    check("a tap on a game", tap_app("radar"));
    check("opens that game", opens == 1 && opened && strcmp(opened->id, "radar") == 0);
    check("and leaves the folder open, to come back to", home_folder_current() != NULL);
    check("a tap on the way back", tap_back());
    check("goes back to the launcher's page", home_folder_current() == NULL && on_page("notes") && !on_page("fleet"));
    check("with the focus on the Games cell, unmarked after a finger", focus_is("games") && !shown());

    /* Keys. */
    key(LV_KEY_RIGHT);
    check("the first key shows the focus where it is", focus_is("games") && shown());
    key(LV_KEY_ENTER);
    check("Enter on Games opens it", home_folder_current() != NULL);
    check("with the focus on its first game", focus_is("fleet") && shown());
    key(LV_KEY_RIGHT);
    key(LV_KEY_RIGHT);
    check("Right, Right: Timber", focus_is("timber"));
    key(LV_KEY_RIGHT);
    check("Right at the end stays", focus_is("timber"));
    key(LV_KEY_UP);
    key(LV_KEY_DOWN);
    check("Up and Down with one row stay", focus_is("timber"));
    key(LV_KEY_LEFT);
    check("Left: Radar", focus_is("radar"));
    key(LV_KEY_ENTER);
    check("Enter opens the focused game", opens == 2 && opened && strcmp(opened->id, "radar") == 0);
    key(LV_KEY_ESC);
    check("Esc goes back, the focus on Games", home_folder_current() == NULL && focus_is("games") && shown());
    key(LV_KEY_ENTER);
    key(LV_KEY_BACKSPACE);
    check("and so does Backspace", home_folder_current() == NULL && focus_is("games"));
    key(LV_KEY_UP);
    check("Up from Games: the row above, nearest in x - Notes", focus_is("notes"));
    key(LV_KEY_DOWN);
    check("Down: Games again", focus_is("games"));
    key(LV_KEY_DOWN);
    check("Down from Games: DEVICE's first, Settings", focus_is("settings"));
    for (k = 0; k < 12; k++) {
        key(LV_KEY_UP);
    }
    check("Up and Up stop at the top row", focus_is("rift"));
    key(LV_KEY_LEFT);
    check("Left at the first cell stays", focus_is("rift"));
    key(LV_KEY_ESC);
    check("Esc at the launcher's page does nothing", home_folder_current() == NULL && focus_is("rift") && opens == 2);
    key(LV_KEY_ENTER);
    check("Enter on an app opens it", opens == 3 && opened && strcmp(opened->id, "rift") == 0);
    check("nothing asked for Lock or Controls", locks == 0 && controls == 0);

    /* Opened from outside - shell.folder, a rotation restart - with the keys
     * somewhere else: the way back still lands on the folder's cell. */
    check("the keys are on RIFT", focus_is("rift"));
    home_folder_open("games");
    key(LV_KEY_ESC);
    check("opened from outside, Esc comes back to the Games cell", home_folder_current() == NULL && focus_is("games"));

    /* Detached: an app is open over the launcher. */
    home_keys_detach();
    key(LV_KEY_RIGHT);
    key(LV_KEY_ENTER);
    check("detached, the keys reach nothing", opens == 3 && focus_is("games"));
    home_keys_attach();
    home_keys_attach();
    key(LV_KEY_RIGHT);
    check("attached (twice is once), they do again", focus_is("settings"));
    teardown();
}

static void test_rounds(void)
{
    int before_objects;
    size_t before_art;
    int k;

    build(make_registry(NULL, 0), false);
    before_objects = objects(lv_screen_active());
    before_art = art_bytes_held();
    for (k = 0; k < 50; k++) {
        if (k % 2) {
            tap_folder();
            tap_back();
        } else {
            key(LV_KEY_DOWN); /* show or move; then back to the folder by its id */
            home_folder_open("games");
            pump(20);
            key(LV_KEY_ESC);
        }
    }
    check("fifty open/back rounds end on the launcher's page", home_folder_current() == NULL);
    check("with exactly the objects it started with", objects(lv_screen_active()) == before_objects);
    check("and exactly the art", art_bytes_held() == before_art);
    home_folder_open("games");
    check("opening holds the folder's icons", art_bytes_held() > before_art);
    home_folder_open("games");
    check("opening the open folder again changes nothing", home_folder_current() != NULL);
    home_folder_close();
    home_folder_close();
    check("closing twice is closing once", home_folder_current() == NULL && art_bytes_held() == before_art);
    check("an unknown folder is not opened", !home_folder_open("tools") && !home_folder_open(NULL) &&
                                               home_folder_current() == NULL);
    teardown();
    check("after the rebuild rounds nothing is held", art_bytes_held() == 0);
}

static void test_sets(void)
{
    struct home_info hi;
    lv_area_t a;

    build(make_registry("fleet radar timber", 0), false);
    home_info(&hi);
    check("no game installed: no Games cell", !home_folder_area("games", &a) && hi.folders == 0 && hi.cells == 15);
    check("and the folder does not open", !home_folder_open("games") && home_folder_current() == NULL);
    teardown();

    build(make_registry("fleet radar", 0), false);
    check("one game: still behind the Games cell", home_folder_area("games", &a) && !on_page("timber"));
    tap_folder();
    check("which holds it alone", home_folder_current() && on_page("timber") && home_folder_size("games") == 1);
    key(LV_KEY_ENTER);
    check("and Enter opens it", opened && strcmp(opened->id, "timber") == 0);
    teardown();

    build(make_registry("radar", 0), false);
    tap_folder();
    check("a game not installed is not in the folder", on_page("fleet") && on_page("timber") && !on_page("radar") &&
                                                           home_folder_size("games") == 2);
    teardown();
}

static void test_many(void)
{
    lv_area_t first;
    lv_area_t a;
    int32_t top;
    int k;

    build(make_registry(NULL, 30), false);
    check("thirty-three games: one Games cell still", home_folder_area("games", &a) && !on_page("g01"));
    check("which holds them all", home_folder_size("games") == 33);
    tap_folder();
    check("the folder opens", home_folder_current() != NULL && on_page("g30"));
    home_cell_area("g30", &a);
    check("the last row starts below the screen", a.y2 > PANEL_H);
    home_cell_area("fleet", &first);
    top = first.y1;
    key(LV_KEY_DOWN);
    for (k = 0; k < 12; k++) {
        key(LV_KEY_DOWN);
    }
    check("Down, Down: the last row", focus_is("g30") || focus_is("g29") || focus_is("g28") || focus_is("g27"));
    home_cell_area(focus(), &a);
    home_cell_area("fleet", &first);
    check("brought into view: the page scrolled and the focused cell is on the screen",
          a.y1 >= 0 && a.y2 <= PANEL_H && first.y1 < top);
    key(LV_KEY_RIGHT);
    key(LV_KEY_RIGHT);
    key(LV_KEY_RIGHT);
    check("Right stops at the last game", focus_is("g30"));
    for (k = 0; k < 12; k++) {
        key(LV_KEY_UP);
    }
    home_cell_area("fleet", &first);
    check("Up, Up: back at the top, in view", first.y1 == top);
    key(LV_KEY_ESC);
    check("Esc: the launcher's page, on Games", home_folder_current() == NULL && focus_is("games"));
    teardown();
}

static void test_landscape(void)
{
    lv_area_t a;
    lv_area_t b;
    lv_area_t c;

    build(make_registry(NULL, 0), true);
    check("landscape: the Games cell", home_folder_area("games", &a));
    /* What a rotation restart does: the shell comes back and opens the
     * folder that was open (shell.c, DOORS_LAUNCHER_FOLDER). */
    check("landscape: the folder opens again by its id", home_folder_open("games"));
    /* Measured at once, before any refresh: opening places the page. */
    home_cell_area("fleet", &a);
    home_cell_area("radar", &b);
    home_cell_area("timber", &c);
    check("landscape: the three games on one row, left to right, placed at once",
          a.y1 == b.y1 && b.y1 == c.y1 && a.y1 > 60 && a.x2 < b.x1 && b.x2 < c.x1 && lv_area_get_width(&a) >= 64);
    check("landscape: inside the screen", a.x1 >= 0 && c.x2 < PANEL_H && c.y2 < PANEL_W);
    check("landscape: the way back, at the top left, clear of the corner",
          home_folder_back_area(&a) && a.x1 >= 30 && a.y1 >= 0 && a.y2 < 80 && lv_area_get_width(&a) == HOME_BACK_W);
    key(LV_KEY_RIGHT);
    key(LV_KEY_RIGHT);
    key(LV_KEY_RIGHT);
    check("landscape: the keys walk the row, the first key showing where the focus is", focus_is("timber"));
    tap_back();
    check("landscape: back on the launcher's page", home_folder_current() == NULL && home_folder_area("games", &a));
    teardown();
}

static void test_restarts(void)
{
    int before;
    int k;

    before = objects(lv_screen_active());
    for (k = 0; k < 20; k++) {
        build(make_registry(NULL, 0), k % 2);
        if (k % 3 == 0) {
            home_folder_open("games");
        }
        teardown();
    }
    check("twenty launcher rebuilds, some with the folder open, leave the screen as it was",
          objects(lv_screen_active()) == before && art_bytes_held() == 0);
    check("and no focus behind", pos_input_focused() == NULL);
}

int main(void)
{
    lv_indev_t *pointer;

    setvbuf(stdout, NULL, _IOLBF, 0);
    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_cb);
    pos_input_init();
    pocketui_init();
    content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(content);

    test_today();
    test_rounds();
    test_sets();
    test_many();
    test_landscape();
    test_restarts();

    printf("home_folder_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
