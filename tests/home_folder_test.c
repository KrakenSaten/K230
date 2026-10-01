/*
 * The DOORS launcher's folders (app groups, ui/shell/home.h) and favorites
 * in the running launcher: real LVGL, a real pointer and the real key
 * stream, the art read from the source tree. The shell is not linked; the
 * test builds the launcher itself, from registries of its own:
 *
 *   - today's twenty-six apps (DS §47): three empty favorites, Terminal,
 *     RIFT, Browser and Settings, then one Apps cell (DeskBuddy, MP3, Photo,
 *     Radio, Video, Vision, Wave, Zabbix), one Utilities cell (Clock,
 *     Calendar, Calculator, Notes, Files, Recorder and Camera) and one Games
 *     cell (the six games) - and no System; taps and keys open a folder,
 *     open an app from it, go back; the focus comes back to the folder's
 *     cell; the keys stop at the edges; Esc at home does nothing; detached,
 *     the keys reach nothing;
 *   - no game installed (no Games cell), one game (still a folder), a game
 *     not installed (not in it), no app of Apps (no Apps cell);
 *   - thirty-three games (thirty more rows through home_layout.c's test
 *     seam): the folder's page scrolls and the keys bring the focus into
 *     view;
 *   - landscape: one row of ten cells, the folders opened again as a
 *     rotation restart does;
 *   - favorites kept from before the apps moved into folders, and one
 *     holding System: each still opens its app directly;
 *   - favorites: a tap on an empty slot or a long press opens the picker,
 *     which offers every installed app once, and none another slot holds;
 *     choosing one sets the slot, which then opens that app; a long press
 *     changes or clears it; the long press's release opens nothing and
 *     chooses nothing, a scroll is not a long press; the keys (Enter on an
 *     empty slot, E on any) do the same; the choice is kept, survives a
 *     rebuild in either orientation, and an app that is not installed or an
 *     id that is none fails safe;
 *   - fifty open/back rounds, thirty picker rounds and twenty launcher
 *     rebuilds, some with a long press or the picker in flight: the objects
 *     on the screen and the art held come back to where they started.
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

/* Where the shell keeps the favorites (settings.conf): here, three strings
 * that outlive a launcher rebuild, as the file outlives a shell restart. */
static char kept[HOME_FAVORITES][64];
static int saves;
static int save_fails; /* > 0: the next saves fail, as a full disk would */

static const char *fav_get(int slot)
{
    return (slot >= 0 && slot < HOME_FAVORITES) ? kept[slot] : NULL;
}

static int fav_set(int slot, const char *id)
{
    if (slot < 0 || slot >= HOME_FAVORITES) {
        return -1;
    }
    if (save_fails > 0) {
        save_fails--;
        return -1;
    }
    snprintf(kept[slot], sizeof(kept[slot]), "%s", id ? id : "");
    saves++;
    return 0;
}

static void forget_favorites(void)
{
    memset(kept, 0, sizeof(kept));
    saves = 0;
    save_fails = 0;
}

static const struct home_actions actions = { on_open, on_lock, on_controls, fav_get, fav_set };

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

static void finger_at(const lv_area_t *a)
{
    finger_point.x = a->x1 + lv_area_get_width(a) / 2;
    finger_point.y = a->y1 + lv_area_get_height(a) / 2;
}

static void lift(void)
{
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
    finger_point.x = 0;
    finger_point.y = 0;
}

/* A finger held still on a for ms, then lifted: 60 ms is a tap, LVGL's long
 * press is 400. */
static void hold_area(const lv_area_t *a, int ms)
{
    finger_at(a);
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(ms);
    lift();
}

static void tap_area(const lv_area_t *a)
{
    hold_area(a, 60);
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

static int fav_is(int slot, const char *id)
{
    const char *got = home_favorite_id(slot);

    return id ? (got && strcmp(got, id) == 0) : got == NULL;
}

/* ---- the registries -------------------------------------------------------- */

#define MAX_APPS 64
static struct pocketos_app defs[MAX_APPS];
static const struct pocketos_app *registry[MAX_APPS];
static char extra_ids[30][8];

/* The shell's registry (ui/shell/shell.c apps[], Zabbix last). */
static const char *const today[] = { "radio", "system", "fleet", "radar", "timber", "notes",
                                     "clock", "calendar", "calculator", "settings", "wave", "rift",
                                     "files", "camera", "browser", "recorder", "vision", "video",
                                     "solitaire", "blackjack", "2048", "mp3", "deskbuddy", "terminal",
                                     "photo", "zabbix" };
#define NTODAY ((int)(sizeof(today) / sizeof(today[0])))
/* Today's registry less eight apps, for the cases that add thirty games of
 * their own and stay within HOME_MAX_APPS: eighteen apps, three of them games. */
#define EIGHTEEN "terminal photo video mp3 deskbuddy solitaire blackjack 2048"

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

static int tap_folder_id(const char *id)
{
    lv_area_t a;

    if (!home_folder_area(id, &a)) {
        return 0;
    }
    tap_area(&a);
    return 1;
}

static int tap_folder(void)
{
    return tap_folder_id("games");
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

static int hold_fav(int slot, int ms)
{
    lv_area_t a;

    if (!home_favorite_area(slot, &a)) {
        return 0;
    }
    hold_area(&a, ms);
    return 1;
}

static int tap_fav(int slot)
{
    return hold_fav(slot, 60);
}

/* ---- the cases ------------------------------------------------------------------ */

static void test_today(void)
{
    static const char *const apps_folder[] = { "deskbuddy", "mp3", "photo", "radio", "video", "vision", "wave",
                                               "zabbix" };
    static const char *const tools[] = { "clock", "calendar", "calculator", "notes", "files", "recorder", "camera" };
    static const char *const games[] = { "fleet", "radar", "timber", "solitaire", "blackjack", "2048" };
    struct home_info hi;
    lv_area_t a;
    lv_area_t f;
    lv_area_t t;
    int k;
    int ok;

    forget_favorites();
    build(make_registry(NULL, 0), false);
    home_info(&hi);
    check("today: twenty-six apps in seven cells, three of them folders, after three favorites",
          hi.apps == 26 && hi.cells == 7 && hi.folders == 3 && hi.icons_art == 7 && hi.icons_fallback == 0 &&
              hi.favorites == 3 && hi.favorites_set == 0);
    check("today: Terminal, RIFT, Browser and Settings are on the launcher's page",
          on_page("terminal") && on_page("rift") && on_page("browser") && on_page("settings"));
    check("today: in that order, on one row, under the favorites",
          home_cell_area("terminal", &t) && home_cell_area("settings", &a) && home_favorite_area(0, &f) &&
              t.y1 == a.y1 && t.x2 < a.x1 && f.y2 < t.y1);
    check("today: the Apps, Utilities and Games cells, in that order, on the row under them",
          home_folder_area("apps", &f) && home_folder_area("games", &a) && f.y1 == a.y1 && f.x2 < a.x1 &&
              f.y1 > t.y2 && lv_area_get_width(&f) >= 64 && home_folder_area("utilities", &t) && t.x1 > f.x2 &&
              t.x2 < a.x1);
    ok = !on_page("system");
    for (k = 0; k < (int)(sizeof(apps_folder) / sizeof(apps_folder[0])); k++) {
        ok = ok && !on_page(apps_folder[k]);
    }
    for (k = 0; k < (int)(sizeof(tools) / sizeof(tools[0])); k++) {
        ok = ok && !on_page(tools[k]);
    }
    for (k = 0; k < (int)(sizeof(games) / sizeof(games[0])); k++) {
        ok = ok && !on_page(games[k]);
    }
    check("today: no other app has a cell on the launcher's page, System included", ok);
    check("today: Apps holds eight, Utilities seven, Games six",
          home_folder_size("apps") == 8 && home_folder_size("utilities") == 7 && home_folder_size("games") == 6 &&
              home_folder_size("nope") == -1);
    check("today: the launcher has the keys", pos_input_focused() != NULL);
    check("today: three empty favorites, above Terminal", home_favorite_area(0, &f) && home_cell_area("terminal", &a) &&
                                                              f.y2 < a.y1 && fav_is(0, NULL) && fav_is(1, NULL) &&
                                                              fav_is(2, NULL) && !home_favorite_stored(0));

    /* A finger. */
    check("a tap on Games", tap_folder());
    check("opens the folder", home_folder_current() && strcmp(home_folder_current(), "games") == 0);
    ok = 1;
    for (k = 0; k < (int)(sizeof(games) / sizeof(games[0])); k++) {
        ok = ok && on_page(games[k]);
    }
    check("whose page has the six games", ok);
    check("and nothing from the launcher's page", !on_page("rift") && !on_page("terminal"));
    check("and no favorite", !home_favorite_area(0, &a));
    check("and a way back", home_folder_back_area(&a) && lv_area_get_width(&a) >= 64);
    check("a tap on a game", tap_app("radar"));
    check("opens that game", opens == 1 && opened && strcmp(opened->id, "radar") == 0);
    check("and leaves the folder open, to come back to", home_folder_current() != NULL);
    check("a tap on the way back", tap_back());
    check("goes back to the launcher's page", home_folder_current() == NULL && on_page("rift") && !on_page("fleet"));
    check("with the focus on the Games cell, unmarked after a finger", focus_is("games") && !shown());
    check("a tap on Utilities", tap_folder_id("utilities"));
    ok = home_folder_current() && strcmp(home_folder_current(), "utilities") == 0 && !on_page("fleet") &&
         !on_page("zabbix");
    for (k = 0; k < (int)(sizeof(tools) / sizeof(tools[0])); k++) {
        ok = ok && on_page(tools[k]);
    }
    check("opens it, with the seven tools in it", ok);
    check("a tool opens from it", tap_app("calculator") && opens == 2 && strcmp(opened->id, "calculator") == 0);
    tap_back();
    check("and back, the focus on Utilities", home_folder_current() == NULL && focus_is("utilities"));
    check("a tap on Apps", tap_folder_id("apps"));
    ok = home_folder_current() && strcmp(home_folder_current(), "apps") == 0 && !on_page("clock") &&
         !on_page("system") && !on_page("fleet");
    for (k = 0; k < (int)(sizeof(apps_folder) / sizeof(apps_folder[0])); k++) {
        ok = ok && on_page(apps_folder[k]);
    }
    check("opens it, with its eight apps and nothing else", ok);
    {
        lv_area_t first;
        lv_area_t last;

        home_cell_area("deskbuddy", &first);
        home_cell_area("zabbix", &last);
        check("in their order: DeskBuddy first, Zabbix last, four to a row in two rows",
              first.y1 < last.y1 && home_cell_area("radio", &a) && a.y1 == first.y1 && a.x1 > first.x1 &&
                  home_cell_area("video", &a) && a.y1 == last.y1 && a.x1 == first.x1);
    }
    check("an app opens from it", tap_app("photo") && opens == 3 && strcmp(opened->id, "photo") == 0);
    tap_back();
    check("and back, the focus on Apps", home_folder_current() == NULL && focus_is("apps"));

    /* Keys. */
    key(LV_KEY_RIGHT);
    check("the first key shows the focus where it is", focus_is("apps") && shown());
    key(LV_KEY_RIGHT);
    key(LV_KEY_RIGHT);
    check("Right, Right: Utilities, then Games", focus_is("games"));
    key(LV_KEY_RIGHT);
    key(LV_KEY_DOWN);
    check("Right and Down at the last cell of the last row stay", focus_is("games"));
    key(LV_KEY_ENTER);
    check("Enter on Games opens it", home_folder_current() != NULL);
    check("with the focus on its first game", focus_is("fleet") && shown());
    key(LV_KEY_RIGHT);
    key(LV_KEY_RIGHT);
    check("Right, Right: Timber", focus_is("timber"));
    for (k = 0; k < 4; k++) {
        key(LV_KEY_RIGHT);
    }
    check("Right walks the games in order and stops at the last, 2048", focus_is("2048"));
    key(LV_KEY_UP);
    check("Up: the row above, nearest in x - Radar", focus_is("radar"));
    key(LV_KEY_ENTER);
    check("Enter opens the focused game", opens == 4 && opened && strcmp(opened->id, "radar") == 0);
    key(LV_KEY_ESC);
    check("Esc goes back, the focus on Games", home_folder_current() == NULL && focus_is("games") && shown());
    key(LV_KEY_ENTER);
    key(LV_KEY_BACKSPACE);
    check("and so does Backspace", home_folder_current() == NULL && focus_is("games"));
    key(LV_KEY_UP);
    check("Up from Games: the row above, nearest in x - Browser", focus_is("browser"));
    key(LV_KEY_DOWN);
    check("Down: Games again", focus_is("games"));
    for (k = 0; k < 12; k++) {
        key(LV_KEY_UP);
    }
    check("Up and Up stop at the top row, the favorite nearest in x", focus_is("favorite-3"));
    key(LV_KEY_LEFT);
    key(LV_KEY_LEFT);
    key(LV_KEY_LEFT);
    check("Left at the first cell stays", focus_is("favorite-1"));
    key(LV_KEY_RIGHT);
    key(LV_KEY_RIGHT);
    check("Right: the favorites in order", focus_is("favorite-3"));
    key(LV_KEY_RIGHT);
    check("then Terminal, the first app", focus_is("terminal"));
    key(LV_KEY_ESC);
    check("Esc at the launcher's page does nothing", home_folder_current() == NULL && focus_is("terminal") &&
                                                         opens == 4);
    key(LV_KEY_ENTER);
    check("Enter on an app opens it", opens == 5 && opened && strcmp(opened->id, "terminal") == 0);
    check("nothing asked for Lock or Controls", locks == 0 && controls == 0);
    check("and nothing touched the favorites", saves == 0 && home_favorite_picking() < 0);

    /* Opened from outside - shell.folder, a rotation restart - with the keys
     * somewhere else: the way back still lands on the folder's cell. */
    check("the keys are on Terminal", focus_is("terminal"));
    home_folder_open("games");
    key(LV_KEY_ESC);
    check("opened from outside, Esc comes back to the Games cell", home_folder_current() == NULL && focus_is("games"));
    home_folder_open("games");
    check("one folder open at most: opening Utilities closes Games",
          home_folder_open("utilities") && strcmp(home_folder_current(), "utilities") == 0 && on_page("notes") &&
              !on_page("fleet"));
    check("and opening Apps closes Utilities",
          home_folder_open("apps") && strcmp(home_folder_current(), "apps") == 0 && on_page("vision") &&
              !on_page("notes"));
    key(LV_KEY_ESC);
    check("and Esc comes back to the Apps cell", home_folder_current() == NULL && focus_is("apps"));
    key(LV_KEY_RIGHT);

    /* Detached: an app is open over the launcher. */
    home_keys_detach();
    key(LV_KEY_RIGHT);
    key(LV_KEY_ENTER);
    check("detached, the keys reach nothing", opens == 5 && focus_is("utilities"));
    home_keys_attach();
    home_keys_attach();
    key(LV_KEY_RIGHT);
    check("attached (twice is once), they do again", focus_is("games"));
    teardown();
}

static void test_rounds(void)
{
    int before_objects;
    size_t before_art;
    int k;

    forget_favorites();
    build(make_registry(NULL, 0), false);
    before_objects = objects(lv_screen_active());
    before_art = art_bytes_held();
    for (k = 0; k < 50; k++) {
        if (k % 2) {
            tap_folder_id(k % 4 == 1 ? "games" : "utilities");
            tap_back();
        } else {
            key(LV_KEY_DOWN); /* show or move; then back to the folder by its id */
            home_folder_open(k % 4 ? "utilities" : "games");
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

    forget_favorites();
    build(make_registry("fleet radar timber solitaire blackjack 2048", 0), false);
    home_info(&hi);
    check("no game installed: no Games cell", !home_folder_area("games", &a) && hi.folders == 2 && hi.cells == 6);
    check("and the folder does not open", !home_folder_open("games") && home_folder_current() == NULL);
    teardown();

    build(make_registry("deskbuddy mp3 photo radio video vision wave zabbix", 0), false);
    home_info(&hi);
    check("no app of Apps installed: no Apps cell, Utilities and Games still there",
          !home_folder_area("apps", &a) && home_folder_area("utilities", &a) && home_folder_area("games", &a) &&
              hi.folders == 2 && hi.cells == 6 && !home_folder_open("apps"));
    teardown();

    build(make_registry("zabbix", 0), false);
    tap_folder_id("apps");
    check("a shell without Zabbix: Apps holds the other seven", home_folder_current() &&
                                                                    home_folder_size("apps") == 7 &&
                                                                    !on_page("zabbix") && on_page("wave"));
    teardown();

    build(make_registry("fleet radar solitaire blackjack 2048", 0), false);
    check("one game: still behind the Games cell", home_folder_area("games", &a) && !on_page("timber"));
    tap_folder();
    check("which holds it alone", home_folder_current() && on_page("timber") && home_folder_size("games") == 1);
    key(LV_KEY_ENTER);
    check("and Enter opens it", opened && strcmp(opened->id, "timber") == 0);
    teardown();

    build(make_registry("radar", 0), false);
    tap_folder();
    check("a game not installed is not in the folder", on_page("fleet") && on_page("timber") && !on_page("radar") &&
                                                           home_folder_size("games") == 5);
    teardown();

    build(make_registry("clock calendar calculator notes files recorder camera", 0), false);
    home_info(&hi);
    check("no tool installed: no Utilities cell, Apps and Games still there",
          !home_folder_area("utilities", &a) && home_folder_area("games", &a) && home_folder_area("apps", &a) &&
              hi.folders == 2 && !home_folder_open("utilities"));
    teardown();

    build(make_registry("camera notes", 0), false);
    tap_folder_id("utilities");
    check("a tool not installed is not in Utilities", home_folder_current() && home_folder_size("utilities") == 5 &&
                                                          !on_page("camera") && !on_page("notes") && on_page("clock"));
    teardown();
}

static void test_many(void)
{
    lv_area_t first;
    lv_area_t a;
    int32_t top;
    int k;

    forget_favorites();
    build(make_registry(EIGHTEEN, 30), false);
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

    /* Forty-eight apps: the picker offers them all, and scrolls. */
    tap_fav(0);
    check("the picker for forty-eight apps opens", home_favorite_picking() == 0 && on_page("g30") && on_page("rift") &&
                                                       !on_page("system"));
    home_cell_area("g30", &a);
    check("and runs past the screen", a.y2 > PANEL_H);
    tap_back();
    teardown();
}

static void test_landscape(void)
{
    lv_area_t a;
    lv_area_t b;
    lv_area_t c;

    forget_favorites();
    build(make_registry(NULL, 0), true);
    check("landscape: the Games cell", home_folder_area("games", &a));
    {
        /* DS §47: the favorites, the four apps and the three folders fit one
         * row, left to right, and the launcher does not scroll. */
        struct home_info hi;
        lv_area_t f;
        lv_area_t t;
        lv_area_t s;
        lv_area_t p;

        home_info(&hi);
        home_favorite_area(2, &f);
        home_cell_area("terminal", &t);
        home_cell_area("settings", &s);
        home_folder_area("apps", &p);
        check("landscape: ten cells on one row - favorites, Terminal .. Settings, Apps .. Games",
              f.y1 == t.y1 && t.y1 == s.y1 && s.y1 == p.y1 && p.y1 == a.y1 && f.x2 < t.x1 && t.x2 < s.x1 &&
                  s.x2 < p.x1 && p.x2 < a.x1 && a.x2 < PANEL_H && !hi.wrapped && !hi.scrolls);
    }
    /* What a rotation restart does: the shell comes back and opens the
     * folder that was open (shell.c, DOORS_LAUNCHER_FOLDER). */
    check("landscape: the folder opens again by its id", home_folder_open("games"));
    /* Measured at once, before any refresh: opening places the page. */
    home_cell_area("fleet", &a);
    home_cell_area("radar", &b);
    home_cell_area("timber", &c);
    check("landscape: the first three games on one row, left to right, placed at once",
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
    check("landscape: Utilities opens, its seven tools on one row",
          home_folder_open("utilities") && home_cell_area("clock", &a) && home_cell_area("camera", &b) &&
              a.y1 == b.y1 && a.x2 < b.x1 && b.x2 < PANEL_H);
    check("landscape: Apps opens, its eight apps on one row",
          home_folder_open("apps") && home_cell_area("deskbuddy", &a) && home_cell_area("zabbix", &b) &&
              a.y1 == b.y1 && a.x2 < b.x1 && b.x2 < PANEL_H);
    check("landscape: and Games, its six", home_folder_open("games") && home_cell_area("fleet", &a) &&
                                               home_cell_area("2048", &b) && a.y1 == b.y1 && b.x2 < PANEL_H);
    home_folder_close();
    teardown();
}

/* ---- favorites ------------------------------------------------------------------- */

/* Favorites kept before DS §47 hold app ids, not places: one holding an app
 * that has since moved into a folder, and one holding System, which has no
 * cell any more, still show their app and open it directly - not its folder,
 * not Settings - in both orientations. */
static void test_favorites_moved(void)
{
    struct home_info hi;
    int o;

    for (o = 0; o < 2; o++) {
        char what[120];

        forget_favorites();
        snprintf(kept[0], sizeof(kept[0]), "%s", "photo");
        snprintf(kept[1], sizeof(kept[1]), "%s", "system");
        snprintf(kept[2], sizeof(kept[2]), "%s", "2048");
        build(make_registry(NULL, 0), o == 1);
        home_info(&hi);
        snprintf(what, sizeof(what), "%s: Photo, System and 2048 kept as favorites are all shown set",
                 o ? "landscape" : "portrait");
        check(what, fav_is(0, "photo") && fav_is(1, "system") && fav_is(2, "2048") && hi.favorites_set == 3);
        snprintf(what, sizeof(what), "%s: none of them has a cell of its own on the launcher's page",
                 o ? "landscape" : "portrait");
        check(what, !on_page("photo") && !on_page("system") && !on_page("2048"));
        opens = 0;
        snprintf(what, sizeof(what), "%s: a tap on each opens that app directly, no folder in between",
                 o ? "landscape" : "portrait");
        check(what, tap_fav(0) && opens == 1 && strcmp(opened->id, "photo") == 0 && tap_fav(1) && opens == 2 &&
                        strcmp(opened->id, "system") == 0 && tap_fav(2) && opens == 3 &&
                        strcmp(opened->id, "2048") == 0 && home_folder_current() == NULL);
        snprintf(what, sizeof(what), "%s: and nothing was written back", o ? "landscape" : "portrait");
        check(what, saves == 0 && strcmp(kept[1], "system") == 0);
        teardown();
    }

    /* The picker of a slot holding System: it offers the apps, never System,
     * and leaving it keeps System. */
    build(make_registry(NULL, 0), false);
    check("the picker for the slot holding System opens", hold_fav(1, 600) && home_favorite_picking() == 1);
    check("it offers the apps of every folder, not System, nor what the other slots hold",
          on_page("vision") && on_page("clock") && on_page("solitaire") && on_page("terminal") &&
              !on_page("system") && !on_page("photo") && !on_page("2048"));
    key(LV_KEY_ESC);
    check("Esc keeps System in the slot", home_favorite_picking() < 0 && fav_is(1, "system") && saves == 0);
    check("the folders' ids are no app", home_favorite_set(0, "apps") == -1 && home_favorite_set(0, "games") == -1);
    check("an app in a folder may be made a favorite", home_favorite_set(0, "vision") == 0 && fav_is(0, "vision") &&
                                                           strcmp(kept[0], "vision") == 0);
    teardown();
    forget_favorites();
}

static void test_favorites(void)
{
    struct home_info hi;
    lv_area_t a;
    lv_area_t b;
    int opens_before;
    int k;

    forget_favorites();
    build(make_registry(NULL, 0), false);
    opens = 0;

    /* Empty: a tap opens the picker. */
    check("three empty slots, each at least the touch minimum",
          home_favorite_area(0, &a) && lv_area_get_width(&a) >= 64 && lv_area_get_height(&a) >= 64 &&
              home_favorite_area(2, &b) && a.y1 == b.y1 && a.x2 < b.x1 && fav_is(0, NULL) && fav_is(1, NULL) &&
              fav_is(2, NULL));
    check("a tap on an empty slot", tap_fav(0));
    check("opens its picker, not a folder, not an app",
          home_favorite_picking() == 0 && home_folder_current() == NULL && opens == 0);
    check("which offers every app installed, those in folders too",
          on_page("rift") && on_page("calculator") && on_page("fleet") && on_page("zabbix") && on_page("camera"));
    check("with the keys starting on the first, and no Clear for an empty slot", !focus_is("clear"));
    check("and a way back", home_folder_back_area(&a));
    tap_back();
    check("back: the slot as it was, the picker gone", home_favorite_picking() < 0 && fav_is(0, NULL) && saves == 0);

    /* Assign. */
    tap_fav(0);
    check("a tap on RIFT in the picker", tap_app("rift"));
    check("gives the slot RIFT and closes the picker, opening nothing",
          fav_is(0, "rift") && home_favorite_picking() < 0 && opens == 0 && on_page("terminal"));
    check("and keeps it", saves == 1 && strcmp(kept[0], "rift") == 0);
    check("RIFT keeps its own cell too", on_page("rift") && home_favorite_area(0, &a) && home_cell_area("rift", &b) &&
                                             a.y1 < b.y1);
    home_info(&hi);
    check("shell.info's count: one set", hi.favorites_set == 1 && hi.icons_art == 7);
    check("a tap on the slot opens RIFT", tap_fav(0) && opens == 1 && opened && strcmp(opened->id, "rift") == 0);

    /* Long press: change. */
    opens_before = opens;
    check("a long press on the slot", hold_fav(0, 600));
    check("opens its picker, and its release opens nothing",
          home_favorite_picking() == 0 && opens == opens_before && fav_is(0, "rift"));
    check("with a Clear, and the keys on what the slot holds", on_page("rift") && home_folder_back_area(&a));
    tap_app("calculator");
    check("choosing Calculator changes the slot", fav_is(0, "calculator") && strcmp(kept[0], "calculator") == 0);
    check("and the keys' place is back on the slot", focus_is("favorite-1"));

    /* The long press's finger stays down well after the picker has opened
     * under it, then lifts where a picker cell now is: nothing is chosen. */
    opens_before = opens;
    home_favorite_area(0, &a);
    finger_at(&a);
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(1500);
    check("held on: the picker is up", home_favorite_picking() == 0);
    lift();
    check("lifted: still up, nothing chosen, nothing opened",
          home_favorite_picking() == 0 && fav_is(0, "calculator") && opens == opens_before);
    tap_back();

    /* Duplicates. */
    hold_fav(1, 600);
    check("slot 2's picker leaves out Calculator, which slot 1 holds",
          home_favorite_picking() == 1 && !on_page("calculator") && on_page("rift") && on_page("clock"));
    tap_app("clock");
    check("slot 2 is Clock", fav_is(1, "clock") && strcmp(kept[1], "clock") == 0);
    check("an app is a favorite once: Clock for slot 3 is refused",
          home_favorite_set(2, "clock") == -2 && fav_is(2, NULL) && kept[2][0] == '\0');
    check("and an app that is not installed, or a slot that is not there",
          home_favorite_set(2, "ghost") == -1 && home_favorite_set(3, "rift") == -1 &&
              home_favorite_set(-1, "rift") == -1 && fav_is(2, NULL));
    check("giving a slot what it holds is fine", home_favorite_set(1, "clock") == 0 && fav_is(1, "clock"));

    /* Clear, by the keys. */
    key(LV_KEY_LEFT);
    key(LV_KEY_LEFT);
    check("the keys on the first slot", focus_is("favorite-1") && shown());
    key('e');
    check("E opens its picker, the keys on Calculator, what it holds",
          home_favorite_picking() == 0 && focus_is("calculator"));
    for (k = 0; k < 30; k++) {
        key(LV_KEY_LEFT);
    }
    check("Left, Left: the Clear cell first", focus_is("clear"));
    saves = 0;
    key(LV_KEY_ENTER);
    check("Enter on Clear empties the slot and keeps that",
          fav_is(0, NULL) && home_favorite_picking() < 0 && saves == 1 && kept[0][0] == '\0' &&
              !home_favorite_stored(0));
    check("back on the slot", focus_is("favorite-1"));
    key(LV_KEY_ENTER);
    check("Enter on an empty slot opens its picker", home_favorite_picking() == 0);
    key(LV_KEY_ESC);
    check("Esc leaves it as it was", home_favorite_picking() < 0 && fav_is(0, NULL) && focus_is("favorite-1"));
    key(LV_KEY_RIGHT);
    opens_before = opens;
    key(LV_KEY_ENTER);
    check("Enter on a set slot opens its app", opens == opens_before + 1 && strcmp(opened->id, "clock") == 0);
    key('E');
    check("E on a set slot opens its picker", home_favorite_picking() == 1 && focus_is("clock"));
    key(LV_KEY_BACKSPACE);
    check("Backspace leaves it", home_favorite_picking() < 0 && fav_is(1, "clock"));
    key(LV_KEY_RIGHT);
    key(LV_KEY_RIGHT);
    key('e');
    check("E on an app's cell does nothing", focus_is("terminal") && home_favorite_picking() < 0);
    home_folder_open("games");
    key('e');
    check("nor in a folder", home_favorite_picking() < 0 && home_folder_current() != NULL);
    home_folder_close();

    /* A scroll is not a long press: the finger starts on a slot and moves
     * the page before 400 ms, then stays down. Today's page fits the screen
     * (DS §47) and has nothing to scroll, so the launcher is built again in
     * a content area too short for it - as a page with more places would
     * be - where the drag does scroll it. */
    teardown();
    use_display(false);
    lv_obj_set_height(content, 700);
    home_create(content, registry, make_registry(NULL, 0), false, NULL, &actions);
    home_keys_attach();
    pump(60);
    home_favorite_area(1, &a);
    finger_at(&a);
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(40);
    for (k = 0; k < 10; k++) {
        finger_point.y -= 20;
        pump(10);
    }
    pump(700);
    lift();
    check("a scroll that starts on a slot opens no picker and nothing else",
          home_favorite_picking() < 0 && fav_is(1, "clock") && opens == opens_before + 1);
    teardown();

    /* Kept: a rebuild (a shell restart) in either orientation has them. */
    build(make_registry(NULL, 0), true);
    check("rebuilt in landscape, the favorites are as they were", fav_is(0, NULL) && fav_is(1, "clock") &&
                                                                  fav_is(2, NULL));
    home_favorite_area(0, &a);
    home_favorite_area(2, &b);
    {
        lv_area_t r;
        lv_area_t s;

        home_cell_area("rift", &r);
        home_cell_area("settings", &s);
        check("landscape: the three slots lead the one line, left to right",
              a.y1 == b.y1 && a.x2 < b.x1 && b.x2 < r.x1 && r.y1 == a.y1 && s.y1 == a.y1 && s.x1 > r.x2 &&
                  lv_area_get_width(&a) >= 64);
    }
    check("landscape: a long press opens the picker, whose release chooses nothing",
          hold_fav(2, 600) && home_favorite_picking() == 2 && fav_is(2, NULL));
    tap_app("vision");
    check("landscape: and the choice is made", fav_is(2, "vision") && strcmp(kept[2], "vision") == 0);
    teardown();
    build(make_registry(NULL, 0), false);
    check("and back in portrait, the same three", fav_is(0, NULL) && fav_is(1, "clock") && fav_is(2, "vision"));
    teardown();

    /* Fails safe: an app this build does not have, an id that is none, a
     * store that cannot be written. */
    snprintf(kept[0], sizeof(kept[0]), "%s", "radar");
    snprintf(kept[1], sizeof(kept[1]), "%s", "../../etc/passwd");
    snprintf(kept[2], sizeof(kept[2]), "%s", "deskbuddy");
    build(make_registry("radar deskbuddy", 0), false);
    home_info(&hi);
    check("an app not installed is an empty slot, kept as stored",
          fav_is(0, NULL) && home_favorite_stored(0) && strcmp(home_favorite_stored(0), "radar") == 0 &&
              fav_is(2, NULL) && strcmp(home_favorite_stored(2), "deskbuddy") == 0 && hi.favorites_set == 0);
    check("an id that is none is an empty slot, not stored", fav_is(1, NULL) && !home_favorite_stored(1));
    opens_before = opens;
    tap_fav(0);
    check("a tap on it opens nothing but the picker", opens == opens_before && home_favorite_picking() == 0);
    check("which does not offer the missing app, and offers a Clear", !on_page("radar") && on_page("fleet"));
    key(LV_KEY_LEFT);
    for (k = 0; k < 30; k++) {
        key(LV_KEY_LEFT);
    }
    check("Clear first", focus_is("clear"));
    key(LV_KEY_ENTER);
    check("clearing it forgets it", !home_favorite_stored(0) && kept[0][0] == '\0');
    save_fails = 1;
    check("a store that cannot be written: the choice still shows",
          home_favorite_set(0, "rift") == 0 && fav_is(0, "rift") && kept[0][0] == '\0');
    teardown();
    check("and lasts only until the launcher goes", kept[0][0] == '\0');
    forget_favorites();
    check("forgotten: nothing held", art_bytes_held() == 0);
}

static void test_restarts(void)
{
    int before;
    size_t art_before;
    int k;

    before = objects(lv_screen_active());
    forget_favorites();
    for (k = 0; k < 20; k++) {
        build(make_registry(NULL, 0), k % 2);
        if (k % 3 == 0) {
            home_folder_open(k % 2 ? "utilities" : "games");
        }
        if (k % 4 == 1) {
            home_favorite_set(k % 3, "rift");
            home_favorite_pick(k % 3);
        }
        teardown();
    }
    check("twenty launcher rebuilds, some with a folder or the picker open, leave the screen as it was",
          objects(lv_screen_active()) == before && art_bytes_held() == 0);
    check("and no focus behind", pos_input_focused() == NULL);

    /* A launcher torn down with a long press in flight - the picker asked
     * for and not yet built, or built under a finger still down - and one
     * with the keys' E in flight: nothing is left behind, and the next
     * launcher neither opens a picker nor ignores the next finger. */
    forget_favorites();
    for (k = 0; k < 12; k++) {
        lv_area_t a;
        int step;

        build(make_registry(NULL, 0), false);
        if (k % 2) {
            key(LV_KEY_RIGHT);
            pos_input_push_key('e');
            for (step = 0; step < k / 2; step++) {
                lv_tick_inc(5);
                lv_timer_handler();
            }
        } else {
            home_favorite_area(0, &a);
            finger_at(&a);
            finger_state = LV_INDEV_STATE_PRESSED;
            pump(400 + 10 * k);
        }
        teardown();
        finger_state = LV_INDEV_STATE_RELEASED;
        pump(60);
    }
    check("twelve launchers torn down mid long press leave the screen as it was",
          objects(lv_screen_active()) == before && art_bytes_held() == 0);
    build(make_registry(NULL, 0), false);
    pump(200);
    check("the next launcher has no picker up of its own accord", home_favorite_picking() < 0);
    check("and answers the next tap", tap_fav(1) && home_favorite_picking() == 1);
    art_before = art_bytes_held();
    for (k = 0; k < 30; k++) {
        tap_fav(1);
        pump(20);
        if (k % 3 == 0) {
            tap_app("rift");
        } else if (k % 3 == 1) {
            key(LV_KEY_ESC);
        } else {
            home_favorite_set(1, NULL);
        }
    }
    home_favorite_set(1, NULL);
    home_folder_close();
    check("thirty picker rounds end on the launcher's page with the art it had before the first",
          home_favorite_picking() < 0 && art_bytes_held() < art_before);
    teardown();
    check("and nothing held after", objects(lv_screen_active()) == before && art_bytes_held() == 0);
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
    test_favorites();
    test_favorites_moved();
    test_restarts();

    printf("home_folder_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
