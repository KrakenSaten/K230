/*
 * The DOORS launcher's geometry and groups (ui/shell/home_layout.h, DS §31):
 *
 *   - the registry's twenty-four apps land in four groups in the table's order,
 *     an app the table does not name goes to MORE (after the named groups,
 *     in registry order), an empty group is not drawn;
 *   - in both orientations of the reference panel every cell is inside its
 *     panel and inside the content area, no two cells or panels overlap,
 *     cells are at least the DS touch minimum, and the footer lies below the
 *     panels and clear of the rounded corners; portrait does not scroll;
 *   - landscape wraps (and then scrolls) instead of squeezing cells: with
 *     today's seventeen apps DEVICE goes to a second line (twelve fitted one);
 *   - folders (app groups): the games are behind one Games cell where Fleet
 *     was, an empty folder has no cell, one game is still a folder, an app
 *     that is not installed is not in it; a folder's page holds 0 to 48 apps
 *     in both orientations inside its panel, clear of the cluster, and
 *     scrolls exactly when it runs past the foot; Clock, Calendar,
 *     Calculator, Notes, Files, Recorder and Camera are behind one Utilities
 *     cell where Clock was, and Zabbix, Vision, RIFT, DeskBuddy, MP3 and
 *     Video stay on the launcher's page;
 *   - favorites: the settings keys, which ids a slot takes, an app a
 *     favorite once, the picker's list (every installed app once, in
 *     launcher order, less the other slots'), and the favorites' panel as
 *     the first row in both orientations.
 *
 * Pure C: built and run by the root Makefile (make test).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "home_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The reference panel. The launcher's content area is the whole display:
 * there is no status bar (DS §36), only the status cluster in the top-right
 * corner, whose box the header keeps clear of - here as the shell makes it
 * on the launcher (chrome_cluster_box of the chip alone, about 113 px, 30 px
 * from the right edge in portrait and 50 in landscape, 14 px down, 44 tall). */
#define PANEL_W 568
#define PANEL_H 1232
#define CORNER 30
#define CORNER_LANDSCAPE_TOP 50
#define TOUCH_MIN 64
#define CLUSTER_W 113
#define CLUSTER_Y 14
#define CLUSTER_H 44

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

/* The shell's registry order (ui/shell/shell.c apps[], then OPTIONAL_APPS). */
static const char *const registry[] = { "radio", "system", "fleet", "radar", "timber", "notes",
                                        "clock", "calendar", "calculator", "settings", "wave", "rift",
                                        "files", "camera", "browser", "recorder", "vision", "video",
                                        "solitaire", "blackjack", "2048", "mp3", "deskbuddy", "zabbix" };
#define NREG ((int)(sizeof(registry) / sizeof(registry[0])))

/* Every app, as the table orders them (groups, then the table's rows). */
static const char *const launcher_order[] = { "rift", "radio", "wave", "zabbix", "browser", "clock", "calendar",
                                              "calculator", "notes", "deskbuddy", "fleet", "radar", "timber",
                                              "solitaire", "blackjack", "2048", "settings", "system", "files",
                                              "recorder", "camera", "vision", "mp3", "video" };

static int inside(const struct home_rect *a, const struct home_rect *b)
{
    return a->x >= b->x && a->y >= b->y && a->x + a->w <= b->x + b->w && a->y + a->h <= b->y + b->h;
}

static int overlap(const struct home_rect *a, const struct home_rect *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static void test_groups(void)
{
    uint8_t order[HOME_MAX_APPS];
    uint8_t count[HOME_GROUP_COUNT];
    int n = home_group_order(registry, NREG, order, count);
    int k;
    int same = n == 24;

    for (k = 0; same && k < n; k++) {
        same = strcmp(registry[order[k]], launcher_order[k]) == 0;
    }
    check("the twenty-four apps are shown in the table's order", same);
    check("CONNECTIONS holds RIFT, Radio, Wave, Zabbix, Browser", count[HOME_GROUP_CONNECT] == 5);
    check("WORKSPACE holds Clock, Calendar, Calculator, Notes, DeskBuddy", count[HOME_GROUP_WORK] == 5);
    check("PLAY holds Fleet, Radar, Timber, Solitaire, Blackjack, 2048", count[HOME_GROUP_PLAY] == 6);
    check("DEVICE holds Settings, System, Files, Recorder, Camera, Vision, MP3, Video", count[HOME_GROUP_DEVICE] == 8);
    check("nothing is left for MORE", count[HOME_GROUP_MORE] == 0);
    check("group names are the package's capitals",
          strcmp(home_group_name(HOME_GROUP_CONNECT), "CONNECTIONS") == 0 &&
              strcmp(home_group_name(HOME_GROUP_MORE), "MORE") == 0);
    check("every table entry has a hue in range", home_entry_find("rift") && home_entry_find("rift")->hue == HOME_HUE_MESH &&
                                                  home_entry_find("system")->hue <= HOME_HUE_APPS);

    {
        /* An app the table does not know, and one group missing entirely. */
        static const char *const ids[] = { "zeta", "notes", "alpha", "radio", "clock" };
        n = home_group_order(ids, 5, order, count);
        check("an unknown app is not lost", n == 5);
        check("unknown apps go to MORE, after the named groups, in registry order",
              count[HOME_GROUP_MORE] == 2 && strcmp(ids[order[3]], "zeta") == 0 && strcmp(ids[order[4]], "alpha") == 0);
        check("a group nothing is installed in is empty", count[HOME_GROUP_PLAY] == 0 && count[HOME_GROUP_DEVICE] == 0);
        check("home_entry_find says no for an unknown id", home_entry_find("zeta") == NULL && home_entry_find(NULL) == NULL);
    }
}

static void input(struct home_layout_in *in, bool landscape, const uint8_t *counts, int ng)
{
    memset(in, 0, sizeof(*in));
    in->width = landscape ? PANEL_H : PANEL_W;
    in->height = landscape ? PANEL_W : PANEL_H;
    in->landscape = landscape;
    in->inset_bottom = CORNER; /* the content area reaches the bottom corners */
    in->keepout.x = in->width - (landscape ? CORNER_LANDSCAPE_TOP : CORNER) - CLUSTER_W;
    in->keepout.y = CLUSTER_Y;
    in->keepout.w = CLUSTER_W;
    in->keepout.h = CLUSTER_H;
    in->ngroups = (uint8_t)ng;
    memcpy(in->count, counts, (size_t)ng);
}

static void check_layout(const char *name, const struct home_layout_in *in, const struct home_layout *l,
                         bool expect_scroll)
{
    char what[160];
    struct home_rect area = { 0, 0, in->width, l->content_h };
    int g;
    int k;
    int j;
    int c = 0;
    int ok_inside = 1;
    int ok_overlap = 1;
    int ok_touch = 1;

    snprintf(what, sizeof(what), "%s: scrolls only when expected (content %d of %d)", name, (int)l->content_h,
             (int)in->height);
    check(what, (l->content_h > in->height) == expect_scroll);
    for (g = 0; g < in->ngroups; g++) {
        if (!in->count[g]) {
            continue;
        }
        if (!inside(&l->panel[g], &area) || l->panel[g].x < 28) {
            ok_inside = 0;
        }
        for (j = 0; j < in->count[g]; j++, c++) {
            if (!inside(&l->cell[c], &l->panel[g])) {
                ok_inside = 0;
            }
            if (l->cell[c].w < TOUCH_MIN || l->cell[c].h < TOUCH_MIN) {
                ok_touch = 0;
            }
        }
        for (k = g + 1; k < in->ngroups; k++) {
            if (in->count[k] && overlap(&l->panel[g], &l->panel[k])) {
                ok_overlap = 0;
            }
        }
    }
    for (k = 0; k < c; k++) {
        for (j = k + 1; j < c; j++) {
            if (overlap(&l->cell[k], &l->cell[j])) {
                ok_overlap = 0;
            }
        }
    }
    snprintf(what, sizeof(what), "%s: %d cells laid out", name, c);
    check(what, c == l->napps);
    snprintf(what, sizeof(what), "%s: every cell inside its panel, every panel inside the area", name);
    check(what, ok_inside);
    snprintf(what, sizeof(what), "%s: no two cells or panels overlap", name);
    check(what, ok_overlap);
    snprintf(what, sizeof(what), "%s: every cell is at least %d x %d", name, TOUCH_MIN, TOUCH_MIN);
    check(what, ok_touch);
    snprintf(what, sizeof(what), "%s: the buttons are %d px tall, below every panel, side by side", name, TOUCH_MIN);
    {
        int below = 1;

        for (g = 0; g < in->ngroups; g++) {
            if (in->count[g] && l->panel[g].y + l->panel[g].h > l->lock_button.y) {
                below = 0;
            }
        }
        check(what, l->lock_button.h >= TOUCH_MIN && l->controls_button.h >= TOUCH_MIN && below &&
                        l->lock_button.y == l->controls_button.y &&
                        l->lock_button.x + l->lock_button.w < l->controls_button.x);
    }
    snprintf(what, sizeof(what), "%s: the footer keeps clear of the bottom corners", name);
    check(what, l->lock_button.y + l->lock_button.h <= l->content_h - in->inset_bottom &&
                    l->controls_button.x + l->controls_button.w <= in->width);
    snprintf(what, sizeof(what), "%s: the header is above every panel", name);
    {
        int above = l->header.h > 0;

        for (g = 0; g < in->ngroups; g++) {
            if (in->count[g] && l->header.y + l->header.h > l->panel[g].y) {
                above = 0;
            }
        }
        check(what, above);
    }
    /* DS §36: the status cluster lies over the top of the launcher. The
     * header band (the time and the date) and every panel keep clear of it,
     * and the band stays centred so the time is on the panels' centre line. */
    if (in->keepout.w > 0) {
        int clear = !overlap(&l->header, &in->keepout) && !overlap(&l->date, &in->keepout);

        for (g = 0; g < in->ngroups; g++) {
            if (in->count[g] && overlap(&l->panel[g], &in->keepout)) {
                clear = 0;
            }
        }
        snprintf(what, sizeof(what), "%s: the header and the panels keep clear of the status cluster", name);
        check(what, clear);
        snprintf(what, sizeof(what), "%s: the header band stays centred (%d..%d of %d)", name,
                 (int)l->header.x, (int)(l->header.x + l->header.w), (int)in->width);
        check(what, abs(l->header.x - (in->width - (l->header.x + l->header.w))) <= 1);
        snprintf(what, sizeof(what), "%s: the band is still wide enough for the time (%d)", name,
                 (int)l->header.w);
        check(what, l->header.w >= 220);
        snprintf(what, sizeof(what), "%s: the date keeps the whole row, under the cluster and above the panels",
                 name);
        {
            int above = 1;

            for (g = 0; g < in->ngroups; g++) {
                if (in->count[g] && l->date.y + l->date.h > l->panel[g].y) {
                    above = 0;
                }
            }
            check(what, l->date.w == in->width - 2 * l->date.x && l->date.y >= in->keepout.y + in->keepout.h &&
                            above);
        }
    }
}

static void test_reference(void)
{
    static const uint8_t today[] = { 5, 4, 3, 4, 0 };
    struct home_layout_in in;
    struct home_layout l;
    int one_row;

    input(&in, false, today, 5);
    check("portrait lays out", home_layout_compute(&in, &l) == 0);
    check_layout("portrait", &in, &l, false);
    check("portrait: four 124 px columns, 20 px labels", l.cell_w == 124 && !l.small_labels);
    check("portrait: the panels share one column", l.panel[0].x == l.panel[3].x && l.panel[0].w == l.panel[3].w);
    /* The content area is the screen now (DS §36), so these are screen
     * rows: under the 56 px bar the first panel was drawn at 192. */
    check("portrait: the first panel starts at row 142, 50 px higher than under the bar", l.panel[0].y == 142);

    /* Thirteen apps in one row would squeeze a cell to 81 px, under
     * HOME_CELL_MIN_W, so by the layout's own rule the panels wrap, and the
     * launcher scrolls to its footer (home_layout.h). Twelve fitted one row;
     * Files was the thirteenth and Camera (feat/camera-app-design) the
     * fourteenth - three panels on the first line, DEVICE on a second. Zabbix
     * (DS §35.4) is the fifteenth, a fourth cell in CONNECTIONS, and with it
     * the panels fall two to a line: CONNECTIONS and WORKSPACE, then PLAY and
     * DEVICE. Browser is the sixteenth, a fifth cell in CONNECTIONS: the
     * lines stay as they were, CONNECTIONS one cell wider. Recorder
     * (feat/recorder-app) is the seventeenth, a fifth cell in DEVICE,
     * which takes a second row inside its panel; the lines stay. */
    input(&in, true, today, 5);
    check("landscape lays out", home_layout_compute(&in, &l) == 0);
    check_layout("landscape", &in, &l, true);
    one_row = l.panel[1].y == l.panel[0].y && l.panel[1].x > l.panel[0].x && l.panel[3].y == l.panel[2].y &&
              l.panel[3].x > l.panel[2].x && l.panel[2].y > l.panel[0].y;
    check("landscape: seventeen apps wrap - two panels on each of two lines, left to right", one_row && l.wrapped);
    check("landscape: PLAY and DEVICE on the second line", l.panel[2].y > l.panel[1].y);
    check("landscape: wrapped cells are the wrap width, 16 px labels",
          l.cell_w == HOME_CELL_WRAP_W && l.small_labels);
    check("landscape: a panel is exactly as wide as its apps",
          l.panel[0].w == 5 * l.cell_w + 2 * HOME_PANEL_PAD && l.panel[1].w == 4 * l.cell_w + 2 * HOME_PANEL_PAD &&
              l.panel[2].w == 3 * l.cell_w + 2 * HOME_PANEL_PAD && l.panel[3].w == 4 * l.cell_w + 2 * HOME_PANEL_PAD);
    check("landscape: each line is centred",
          abs(l.panel[0].x - (in.width - (l.panel[1].x + l.panel[1].w))) <= 1 &&
              abs(l.panel[2].x - (in.width - (l.panel[3].x + l.panel[3].w))) <= 1);
    /* Under the 56 px bar the launcher was 512 px tall and scrolled 120 px to
     * its footer; with the 56 px back, less the 6 px the header moved down to
     * sit level with the cluster, it scrolls 70. */
    check("landscape: scrolls 70 px to its footer (120 under the bar)", l.content_h - in.height == 70);
}

static void test_growth(void)
{
    static const uint8_t many[] = { 6, 8, 5, 3, 4 };
    static const uint8_t lonely[] = { 0, 0, 0, 1, 0 };
    struct home_layout_in in;
    struct home_layout l;

    input(&in, false, many, 5);
    check("26 apps in portrait lay out", home_layout_compute(&in, &l) == 0);
    check_layout("portrait, 26 apps", &in, &l, true);
    check("portrait: a group of eight takes two rows", l.panel[1].h > l.panel[3].h);

    input(&in, true, many, 5);
    check("26 apps in landscape lay out", home_layout_compute(&in, &l) == 0);
    check_layout("landscape, 26 apps", &in, &l, true);
    check("landscape: many apps wrap instead of squeezing", l.wrapped && l.cell_w == HOME_CELL_WRAP_W);

    input(&in, true, lonely, 5);
    check("one app lays out", home_layout_compute(&in, &l) == 0);
    check_layout("landscape, one app", &in, &l, false);

    {
        static const uint8_t too_many[] = { 40, 10 };

        input(&in, false, too_many, 2);
        check("more apps than the launcher holds is refused", home_layout_compute(&in, &l) < 0);
        check("NULL is refused", home_layout_compute(NULL, &l) < 0);
    }
}

/* ---- folders (app groups) ------------------------------------------------ */

static int items_hold(const struct home_item *items, int n, const char *const *ids, const char *id)
{
    int k;

    for (k = 0; k < n; k++) {
        if (items[k].folder == HOME_FOLDER_NONE && strcmp(ids[items[k].index], id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int folder_place(const struct home_item *items, int n, enum home_folder f)
{
    int k;

    for (k = 0; k < n; k++) {
        if (items[k].folder == f) {
            return k;
        }
    }
    return -1;
}

static void test_folders(void)
{
    struct home_item items[HOME_MAX_APPS];
    uint8_t order[HOME_MAX_APPS];
    uint8_t count[HOME_GROUP_COUNT];
    const struct home_folder_def *games = home_folder_get(HOME_FOLDER_GAMES);
    int n;
    int k;
    int in_folders = 0;

    check("GAMES is a folder with an id and a name", games && strcmp(games->id, "games") == 0 &&
                                                        strcmp(games->name, "Games") == 0 &&
                                                        games->hue == HOME_HUE_GAMES);
    check("it is found by its id, and nothing else is",
          home_folder_find("games") == HOME_FOLDER_GAMES && home_folder_find("fleet") == HOME_FOLDER_NONE &&
              home_folder_find(NULL) == HOME_FOLDER_NONE && home_folder_find("") == HOME_FOLDER_NONE);
    check("NONE and out-of-range have no definition",
          !home_folder_get(HOME_FOLDER_NONE) && !home_folder_get(HOME_FOLDER_COUNT) &&
              !home_folder_get((enum home_folder)-1));
    for (k = 0; k < NREG; k++) {
        const struct home_entry *en = home_entry_find(registry[k]);

        if (en && en->folder != HOME_FOLDER_NONE) {
            in_folders++;
        }
    }
    check("the table puts the six games in GAMES and seven tools in UTILITIES, and nothing else in a folder",
          in_folders == 13 && home_entry_find("fleet")->folder == HOME_FOLDER_GAMES &&
              home_entry_find("radar")->folder == HOME_FOLDER_GAMES &&
              home_entry_find("timber")->folder == HOME_FOLDER_GAMES &&
              home_entry_find("solitaire")->folder == HOME_FOLDER_GAMES &&
              home_entry_find("blackjack")->folder == HOME_FOLDER_GAMES &&
              home_entry_find("2048")->folder == HOME_FOLDER_GAMES);

    /* Today's registry: one Games cell instead of six game cells, one
     * Utilities cell instead of seven. */
    n = home_root_order(registry, NREG, items, count);
    check("the launcher's page has thirteen places for twenty-four apps", n == 13);
    check("PLAY holds one place, the Games folder", count[HOME_GROUP_PLAY] == 1 &&
                                                         folder_place(items, n, HOME_FOLDER_GAMES) >= 0);
    check("CONNECTIONS is as it was, WORKSPACE is Utilities and DeskBuddy, DEVICE keeps five",
          count[HOME_GROUP_CONNECT] == 5 && count[HOME_GROUP_WORK] == 2 && count[HOME_GROUP_DEVICE] == 5 &&
              count[HOME_GROUP_MORE] == 0);
    check("no game is on the launcher's page",
          !items_hold(items, n, registry, "fleet") && !items_hold(items, n, registry, "radar") &&
              !items_hold(items, n, registry, "timber") && !items_hold(items, n, registry, "solitaire") &&
              !items_hold(items, n, registry, "blackjack") && !items_hold(items, n, registry, "2048"));
    check("the Games folder is where Fleet was: after WORKSPACE's two, before DEVICE",
          folder_place(items, n, HOME_FOLDER_GAMES) == 7);
    check("every other app still is",
          items_hold(items, n, registry, "deskbuddy") && items_hold(items, n, registry, "vision") &&
              items_hold(items, n, registry, "rift") && items_hold(items, n, registry, "zabbix") &&
              items_hold(items, n, registry, "mp3") && items_hold(items, n, registry, "video") &&
              items_hold(items, n, registry, "settings") && items_hold(items, n, registry, "system"));
    {
        const struct home_folder_def *u = home_folder_get(HOME_FOLDER_UTILITIES);
        static const char *const tools[] = { "clock", "calendar", "calculator", "notes", "files", "recorder",
                                             "camera" };
        static const char *const outside[] = { "zabbix", "vision", "rift", "radio", "deskbuddy", "mp3", "video",
                                               "settings", "system", "browser", "wave" };
        int ok = 1;

        check("UTILITIES is a folder with an id, a name and the tools colour",
              u && strcmp(u->id, "utilities") == 0 && strcmp(u->name, "Utilities") == 0 &&
                  u->hue == HOME_HUE_TOOLS && home_folder_find("utilities") == HOME_FOLDER_UTILITIES);
        check("the Utilities folder is WORKSPACE's first place, where Clock was, before DeskBuddy",
              folder_place(items, n, HOME_FOLDER_UTILITIES) == 5 && items[6].folder == HOME_FOLDER_NONE &&
                  strcmp(registry[items[6].index], "deskbuddy") == 0);
        n = home_folder_order(registry, NREG, HOME_FOLDER_UTILITIES, order);
        for (k = 0; k < 7 && n == 7; k++) {
            ok = ok && strcmp(registry[order[k]], tools[k]) == 0;
        }
        check("Utilities holds Clock, Calendar, Calculator, Notes, Files, Recorder and Camera, in that order",
              n == 7 && ok);
        n = home_root_order(registry, NREG, items, count);
        ok = 1;
        for (k = 0; k < (int)(sizeof(tools) / sizeof(tools[0])); k++) {
            ok = ok && !items_hold(items, n, registry, tools[k]);
        }
        check("and none of them is on the launcher's page", ok);
        ok = 1;
        for (k = 0; k < (int)(sizeof(outside) / sizeof(outside[0])); k++) {
            ok = ok && home_entry_find(outside[k])->folder == HOME_FOLDER_NONE &&
                 items_hold(items, n, registry, outside[k]);
        }
        check("Zabbix, Vision, RIFT, DeskBuddy, MP3, Video and the rest stay on the launcher's page", ok);
        check("Games is unchanged: six games in the table's order",
              home_folder_order(registry, NREG, HOME_FOLDER_GAMES, order) == 6 &&
                  strcmp(registry[order[0]], "fleet") == 0 && strcmp(registry[order[5]], "2048") == 0);
        check("folder ids are distinct", home_folder_find("games") != home_folder_find("utilities"));
    }
    n = home_folder_order(registry, NREG, HOME_FOLDER_GAMES, order);
    check("Games holds Fleet, Radar, Timber, Solitaire, Blackjack and 2048, in the table's order",
          n == 6 && strcmp(registry[order[0]], "fleet") == 0 && strcmp(registry[order[1]], "radar") == 0 &&
              strcmp(registry[order[2]], "timber") == 0 && strcmp(registry[order[3]], "solitaire") == 0 &&
              strcmp(registry[order[4]], "blackjack") == 0 && strcmp(registry[order[5]], "2048") == 0);
    check("NONE and an unknown folder hold nothing",
          home_folder_order(registry, NREG, HOME_FOLDER_NONE, order) == 0 &&
              home_folder_order(registry, NREG, HOME_FOLDER_COUNT, order) == 0);

    {
        /* An empty folder: no game installed. */
        static const char *const ids[] = { "notes", "radio", "settings" };

        n = home_root_order(ids, 3, items, count);
        check("with no game installed there is no Games cell", n == 3 && folder_place(items, n, HOME_FOLDER_GAMES) < 0 &&
                                                                    count[HOME_GROUP_PLAY] == 0);
        check("and the folder is empty", home_folder_order(ids, 3, HOME_FOLDER_GAMES, order) == 0);
    }
    {
        /* One game: still a folder, so the game is always found in the same place. */
        static const char *const ids[] = { "notes", "timber", "radio" };

        n = home_root_order(ids, 3, items, count);
        check("one game is still behind the Games cell", n == 3 && folder_place(items, n, HOME_FOLDER_GAMES) >= 0 &&
                                                              !items_hold(items, n, ids, "timber"));
        check("which holds it", home_folder_order(ids, 3, HOME_FOLDER_GAMES, order) == 1 &&
                                    strcmp(ids[order[0]], "timber") == 0);
    }
    {
        /* An app that is not installed (a shell built without it) is not in
         * the folder, and one the table does not know is not either. */
        static const char *const ids[] = { "radar", "zeta", "fleet", "clock" };

        n = home_root_order(ids, 4, items, count);
        check("an unknown app still goes to MORE, not to a folder",
              count[HOME_GROUP_MORE] == 1 && items_hold(items, n, ids, "zeta"));
        check("Games holds the two installed games, in the table's order",
              home_folder_order(ids, 4, HOME_FOLDER_GAMES, order) == 2 && strcmp(ids[order[0]], "fleet") == 0 &&
                  strcmp(ids[order[1]], "radar") == 0);
    }
}

static void folder_input(struct home_folder_layout_in *in, bool landscape, int n)
{
    memset(in, 0, sizeof(*in));
    in->width = landscape ? PANEL_H : PANEL_W;
    in->height = landscape ? PANEL_W : PANEL_H;
    in->landscape = landscape;
    in->inset_bottom = CORNER;
    in->keepout.x = in->width - (landscape ? CORNER_LANDSCAPE_TOP : CORNER) - CLUSTER_W;
    in->keepout.y = CLUSTER_Y;
    in->keepout.w = CLUSTER_W;
    in->keepout.h = CLUSTER_H;
    in->n = n;
}

static void check_folder_layout(const char *name, const struct home_folder_layout_in *in,
                                const struct home_folder_layout *l)
{
    char what[160];
    struct home_rect area = { 0, 0, in->width, l->content_h };
    int ok_inside = 1;
    int ok_overlap = 1;
    int ok_touch = 1;
    int j;
    int k;

    for (k = 0; k < in->n; k++) {
        if (!inside(&l->cell[k], &l->panel)) {
            ok_inside = 0;
        }
        if (l->cell[k].w < TOUCH_MIN || l->cell[k].h < TOUCH_MIN) {
            ok_touch = 0;
        }
        for (j = 0; j < k; j++) {
            if (overlap(&l->cell[k], &l->cell[j])) {
                ok_overlap = 0;
            }
        }
    }
    snprintf(what, sizeof(what), "%s: every cell is inside the panel", name);
    check(what, ok_inside && l->napps == in->n);
    snprintf(what, sizeof(what), "%s: no two cells overlap", name);
    check(what, ok_overlap);
    snprintf(what, sizeof(what), "%s: every cell is at least the touch minimum", name);
    check(what, ok_touch);
    snprintf(what, sizeof(what), "%s: the panel is inside the page, the back slab and the title above it", name);
    check(what, inside(&l->panel, &area) && inside(&l->back, &area) &&
                    l->back.y + l->back.h <= l->panel.y && l->title.y + l->title.h <= l->panel.y);
    snprintf(what, sizeof(what), "%s: the way back is the app header's slab, at the left", name);
    check(what, l->back.w == HOME_BACK_W && l->back.h == HOME_BACK_H && l->back.x < l->title.x &&
                    !overlap(&l->back, &l->title));
    snprintf(what, sizeof(what), "%s: the title keeps clear of the status cluster", name);
    check(what, !overlap(&l->title, &in->keepout) && !overlap(&l->back, &in->keepout) && l->title.w >= 200);
    snprintf(what, sizeof(what), "%s: the page scrolls exactly when the panel runs past the foot", name);
    check(what, (l->content_h > in->height) == (l->panel.y + l->panel.h + 42 > in->height));
}

static void test_folder_layout(void)
{
    static const int sizes[] = { 0, 1, 3, 6, 9, 13, 40, HOME_MAX_APPS };
    struct home_folder_layout_in in;
    static struct home_folder_layout l;
    char name[64];
    size_t s;
    int o;

    for (o = 0; o < 2; o++) {
        for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            folder_input(&in, o == 1, sizes[s]);
            snprintf(name, sizeof(name), "%s folder, %d app(s)", o ? "landscape" : "portrait", sizes[s]);
            check(name, home_folder_layout_compute(&in, &l) == 0);
            check_folder_layout(name, &in, &l);
        }
    }
    folder_input(&in, false, 6);
    home_folder_layout_compute(&in, &l);
    check("portrait: six games in four columns of 124 px, two rows, the panel the launcher's width",
          l.cols == 4 && l.cell_w == 124 && l.panel.h == 2 * HOME_CELL_H + HOME_PANEL_PAD + HOME_PANEL_HEAD +
                                                            HOME_PANEL_PAD + HOME_PANEL_BOTTOM &&
              l.panel.x == 28 && l.panel.w == PANEL_W - 56 && l.content_h == PANEL_H);
    folder_input(&in, true, 6);
    home_folder_layout_compute(&in, &l);
    check("landscape: six games on one row, the panel as wide as they are and centred",
          l.cols == 6 && l.cell[5].y == l.cell[0].y && l.panel.w == 6 * 124 + 2 * HOME_PANEL_PAD &&
              abs(l.panel.x - (PANEL_H - l.panel.x - l.panel.w)) <= 1 && l.content_h == PANEL_W);
    folder_input(&in, true, 40);
    home_folder_layout_compute(&in, &l);
    check("landscape: forty apps wrap at nine and the page scrolls", l.cols == 9 && l.content_h > PANEL_W &&
                                                                         l.cell[9].y > l.cell[0].y);
    folder_input(&in, false, 40);
    home_folder_layout_compute(&in, &l);
    check("portrait: forty apps take ten rows and the page scrolls", l.cell[39].y > l.cell[35].y &&
                                                                         l.content_h > PANEL_H);
    folder_input(&in, false, 0);
    home_folder_layout_compute(&in, &l);
    check("an empty folder is a panel one row tall", l.panel.h == HOME_CELL_H + HOME_PANEL_HEAD +
                                                                       HOME_PANEL_PAD + HOME_PANEL_BOTTOM);
    folder_input(&in, false, HOME_MAX_APPS + 1);
    check("more apps than a folder holds is refused", home_folder_layout_compute(&in, &l) < 0);
    check("NULL is refused", home_folder_layout_compute(NULL, &l) < 0);
}

/* ---- favorites ------------------------------------------------------------- */

static void test_favorites(void)
{
    uint8_t order[HOME_MAX_APPS];
    const char *none[HOME_FAVORITES] = { NULL, NULL, NULL };
    const char *some[HOME_FAVORITES] = { "rift", NULL, "calculator" };
    int n;
    int k;
    int same;

    check("three favorite slots", HOME_FAVORITES == 3);
    check("each has its settings key, counted from 1",
          strcmp(home_favorite_key(0), "launcher_favorite_1") == 0 &&
              strcmp(home_favorite_key(2), "launcher_favorite_3") == 0 && !home_favorite_key(-1) &&
              !home_favorite_key(3));
    check("an app id is lower-case letters, digits, _ and -, 1 to 31 of them",
          home_favorite_id_ok("rift") && home_favorite_id_ok("2048") && home_favorite_id_ok("a_b-c") &&
              home_favorite_id_ok("abcdefghijklmnopqrstuvwxyz01234") &&
              !home_favorite_id_ok("abcdefghijklmnopqrstuvwxyz012345") && !home_favorite_id_ok("") &&
              !home_favorite_id_ok(NULL) && !home_favorite_id_ok("Rift") && !home_favorite_id_ok("a b") &&
              !home_favorite_id_ok("../etc") && !home_favorite_id_ok("rift\n"));
    check("an installed app resolves to its place in the registry",
          home_favorite_resolve(registry, NREG, "rift") == 11 && home_favorite_resolve(registry, NREG, "radio") == 0);
    check("an app that is not installed, or no app, does not",
          home_favorite_resolve(registry, NREG, "ghost") < 0 && home_favorite_resolve(registry, NREG, NULL) < 0 &&
              home_favorite_resolve(registry, NREG, "") < 0 && home_favorite_resolve(registry, 0, "rift") < 0);

    /* Assign, change, duplicates. */
    check("an empty slot may be given any installed app", home_favorite_check(registry, NREG, none, 0, "rift") == 0 &&
                                                              home_favorite_check(registry, NREG, none, 2, "2048") == 0);
    check("and an app inside a folder", home_favorite_check(registry, NREG, none, 1, "calculator") == 0);
    check("a slot may be given what it holds, or changed",
          home_favorite_check(registry, NREG, some, 0, "rift") == 0 &&
              home_favorite_check(registry, NREG, some, 0, "vision") == 0);
    check("but not an app another slot holds (an app is a favorite once)",
          home_favorite_check(registry, NREG, some, 1, "rift") == -2 &&
              home_favorite_check(registry, NREG, some, 0, "calculator") == -2);
    check("nor an app that is not installed, nor a slot out of range",
          home_favorite_check(registry, NREG, none, 0, "ghost") == -1 &&
              home_favorite_check(registry, NREG, none, 3, "rift") == -1 &&
              home_favorite_check(registry, NREG, none, -1, "rift") == -1 &&
              home_favorite_check(registry, NREG, none, 0, NULL) == -1);

    /* The picker's list. */
    n = home_favorite_candidates(registry, NREG, none, 0, order);
    same = n == 24;
    for (k = 0; same && k < n; k++) {
        same = strcmp(registry[order[k]], launcher_order[k]) == 0;
    }
    check("with nothing set the picker offers every app, once, in launcher order, folders' apps too", same);
    n = home_favorite_candidates(registry, NREG, some, 1, order);
    same = n == 22;
    for (k = 0; k < n; k++) {
        same = same && strcmp(registry[order[k]], "rift") != 0 && strcmp(registry[order[k]], "calculator") != 0;
    }
    check("it leaves out what the other slots hold", same);
    n = home_favorite_candidates(registry, NREG, some, 0, order);
    same = n == 23;
    for (k = 0; k < n; k++) {
        same = same && strcmp(registry[order[k]], "calculator") != 0;
    }
    check("but offers the slot's own app (choosing it changes nothing)",
          same && home_favorite_resolve(registry, NREG, "rift") >= 0 && strcmp(registry[order[0]], "rift") == 0);
    {
        /* Not launchable (NULL), twice in the registry, unknown to the table,
         * not an id. */
        static const char *const ids[] = { "radio", NULL, "radio", "zeta", "Bad Id", "notes" };

        n = home_favorite_candidates(ids, 6, none, 0, order);
        check("an app is offered once, a non-launchable entry or a bad id never, an unknown app under MORE",
              n == 3 && strcmp(ids[order[0]], "radio") == 0 && strcmp(ids[order[1]], "notes") == 0 &&
                  strcmp(ids[order[2]], "zeta") == 0);
        check("resolving steps over a non-launchable entry", home_favorite_resolve(ids, 6, "notes") == 5);
    }
    {
        const char *all3[HOME_FAVORITES] = { "radio", "notes", "zeta" };
        static const char *const ids[] = { "radio", "notes", "zeta" };

        check("with three apps, all favorites, the third slot is offered only its own",
              home_favorite_candidates(ids, 3, all3, 2, order) == 1 && strcmp(ids[order[0]], "zeta") == 0);
    }

    /* The launcher's page: the favorites' panel first, three cells, the
     * first row in both orientations, and today's groups after it. */
    {
        struct home_item items[HOME_MAX_APPS];
        uint8_t count[HOME_GROUP_COUNT];
        struct home_layout_in in;
        struct home_layout l;
        int o;

        n = home_root_order(registry, NREG, items, count);
        for (o = 0; o < 2; o++) {
            bool land = o == 1;
            const char *name = land ? "landscape, favorites" : "portrait, favorites";
            char what[160];
            int first = 1;
            int g;

            input(&in, land, count, 0);
            home_layout_groups(&in, count);
            snprintf(what, sizeof(what), "%s: the favorites' panel and the five groups", name);
            check(what, in.ngroups == HOME_GROUP_COUNT + 1 && in.count[0] == HOME_FAVORITES &&
                            in.count[1] == count[HOME_GROUP_CONNECT] && in.count[HOME_GROUP_COUNT] == count[HOME_GROUP_MORE]);
            snprintf(what, sizeof(what), "%s: lays out", name);
            check(what, home_layout_compute(&in, &l) == 0);
            check_layout(name, &in, &l, true);
            snprintf(what, sizeof(what), "%s: 16 cells, three favorites and thirteen places", name);
            check(what, l.napps == HOME_FAVORITES + n && n == 13);
            for (k = 0; k < HOME_FAVORITES; k++) {
                first = first && inside(&l.cell[k], &l.panel[0]) && l.cell[k].y == l.cell[0].y &&
                        (k == 0 || l.cell[k].x > l.cell[k - 1].x);
            }
            /* Every other cell is on a later row, or - landscape, where
             * CONNECTIONS and WORKSPACE share the first line - further right. */
            for (k = HOME_FAVORITES; k < l.napps; k++) {
                first = first && (l.cell[k].y > l.cell[0].y ||
                                  (land && l.cell[k].y == l.cell[0].y && l.cell[k].x > l.cell[HOME_FAVORITES - 1].x));
            }
            for (g = 1; g < in.ngroups; g++) {
                first = first && (!in.count[g] || l.panel[g].y >= l.panel[0].y);
            }
            snprintf(what, sizeof(what), "%s: the favorites are the first row, left to right, above every other cell",
                     name);
            check(what, first);
            snprintf(what, sizeof(what), "%s: the favorites' panel is the first on its line", name);
            check(what, land ? l.panel[0].x < l.panel[1].x : l.panel[0].x == l.panel[1].x);
            snprintf(what, sizeof(what), "%s: a favorite is a cell like any other", name);
            check(what, l.cell[0].w == l.cell[HOME_FAVORITES].w && l.cell[0].h == HOME_CELL_H);
        }
        check("landscape with favorites still scrolls 70 px to its footer", l.content_h - in.height == 70);
    }
}

int main(void)
{
    test_groups();
    test_reference();
    test_growth();
    test_folders();
    test_folder_layout();
    test_favorites();
    printf("home_layout_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
