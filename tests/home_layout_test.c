/*
 * The DOORS launcher's geometry and groups (ui/shell/home_layout.h, DS §31,
 * §39, §42, §47):
 *
 *   - the registry's twenty-six apps land in two groups in the table's
 *     order: ESSENTIALS (Terminal, RIFT, Browser, Settings) and FOLDERS
 *     (every other app, each in a folder); System is a page of Settings and
 *     is placed nowhere; an app the table does not name goes to MORE (after
 *     the named groups, in registry order), an empty group is not drawn;
 *   - the launcher's page is three favorites, the four essentials and the
 *     three folders' cells - Apps, Utilities, Games - and nothing else;
 *     Apps holds DeskBuddy, MP3, Photo, Radio, Video, Vision, Wave and
 *     Zabbix (alphabetical), Utilities its seven tools in their order, Games
 *     its six games in theirs; no app is in two places, and every app but
 *     System is in exactly one;
 *   - in both orientations of the reference panel every cell is inside its
 *     panel and inside the content area, no two cells or panels overlap,
 *     cells are at least the DS touch minimum, and the footer lies below the
 *     panels and clear of the rounded corners; today's launcher scrolls in
 *     neither orientation and landscape keeps one row;
 *   - landscape wraps (and then scrolls) instead of squeezing cells, for a
 *     launcher with more groups and apps than today's;
 *   - folders: an empty folder has no cell, one app is still a folder, an
 *     app that is not installed is not in it; a folder's page holds 0 to 48
 *     apps in both orientations inside its panel, clear of the cluster, and
 *     scrolls exactly when it runs past the foot;
 *   - favorites: the settings keys, which ids a slot takes, an app a
 *     favorite once, the picker's list (every installed app once, in
 *     launcher order, less the other slots' and less System), a favorite
 *     already holding a grouped app or System still resolving, and the
 *     favorites' panel as the first row in both orientations.
 *
 * Pure C: built and run by the root Makefile (make test).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
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

/* The shell's registry order (ui/shell/shell.c apps[], then OPTIONAL_APPS).
 * tests/launcher_groups_shell_test.sh holds the running shell to the same
 * structure, from its own shell.info. */
static const char *const registry[] = { "radio", "system", "fleet", "radar", "timber", "notes",
                                        "clock", "calendar", "calculator", "settings", "wave", "rift",
                                        "files", "camera", "browser", "recorder", "vision", "video",
                                        "solitaire", "blackjack", "2048", "mp3", "deskbuddy", "terminal",
                                        "photo", "zabbix" };
#define NREG ((int)(sizeof(registry) / sizeof(registry[0])))

/* Every app the launcher places, as the table orders them (groups, then the
 * table's rows); System, a page of Settings, is not among them. */
static const char *const launcher_order[] = { "terminal", "rift", "browser", "settings",
                                              "deskbuddy", "mp3", "photo", "radio", "video", "vision", "wave",
                                              "zabbix", "clock", "calendar", "calculator", "notes", "files",
                                              "recorder", "camera", "fleet", "radar", "timber", "solitaire",
                                              "blackjack", "2048" };
#define NPLACED ((int)(sizeof(launcher_order) / sizeof(launcher_order[0])))

/* DS §47. */
static const char *const essentials[] = { "terminal", "rift", "browser", "settings" };
static const char *const apps_folder[] = { "deskbuddy", "mp3", "photo", "radio", "video", "vision", "wave", "zabbix" };
static const char *const utilities_folder[] = { "clock", "calendar", "calculator", "notes", "files", "recorder",
                                                "camera" };
static const char *const games_folder[] = { "fleet", "radar", "timber", "solitaire", "blackjack", "2048" };
#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

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
    int same = n == NPLACED && NPLACED == NREG - 1;

    for (k = 0; same && k < n; k++) {
        same = strcmp(registry[order[k]], launcher_order[k]) == 0;
    }
    check("every app but System is placed, once, in the table's order", same);
    for (k = 0; k < n; k++) {
        check("System is placed nowhere", strcmp(registry[order[k]], "system") != 0);
    }
    check("ESSENTIALS holds Terminal, RIFT, Browser and Settings", count[HOME_GROUP_ESSENTIALS] == 4);
    check("FOLDERS holds the other twenty-one", count[HOME_GROUP_FOLDERS] == 21);
    check("nothing is left for MORE", count[HOME_GROUP_MORE] == 0);
    check("group names are the package's capitals",
          strcmp(home_group_name(HOME_GROUP_ESSENTIALS), "ESSENTIALS") == 0 &&
              strcmp(home_group_name(HOME_GROUP_FOLDERS), "FOLDERS") == 0 &&
              strcmp(home_group_name(HOME_GROUP_MORE), "MORE") == 0);
    check("every table entry has a hue in range", home_entry_find("rift") && home_entry_find("rift")->hue == HOME_HUE_MESH &&
                                                  home_entry_find("system")->hue <= HOME_HUE_APPS);
    check("System is in the table, in no group and no folder (a page of Settings)",
          home_entry_find("system") && home_entry_find("system")->group == HOME_GROUP_NONE &&
              home_entry_find("system")->folder == HOME_FOLDER_NONE);
    for (k = 0; k < LEN(essentials); k++) {
        const struct home_entry *en = home_entry_find(essentials[k]);

        check("an essential is in ESSENTIALS and in no folder",
              en && en->group == HOME_GROUP_ESSENTIALS && en->folder == HOME_FOLDER_NONE);
    }

    {
        /* An app the table does not know, and one group missing entirely. */
        static const char *const ids[] = { "zeta", "notes", "alpha", "rift", "clock" };
        n = home_group_order(ids, 5, order, count);
        check("an unknown app is not lost", n == 5);
        check("unknown apps go to MORE, after the named groups, in registry order",
              count[HOME_GROUP_MORE] == 2 && strcmp(ids[order[3]], "zeta") == 0 && strcmp(ids[order[4]], "alpha") == 0);
        check("home_entry_find says no for an unknown id", home_entry_find("zeta") == NULL && home_entry_find(NULL) == NULL);
    }
    {
        static const char *const ids[] = { "notes", "clock" };

        n = home_group_order(ids, 2, order, count);
        check("a group nothing is installed in is empty", n == 2 && count[HOME_GROUP_ESSENTIALS] == 0);
    }
    {
        /* A page of another app is not lost to MORE either. */
        static const char *const ids[] = { "system", "settings" };

        n = home_group_order(ids, 2, order, count);
        check("System alone is placed nowhere, not under MORE",
              n == 1 && strcmp(ids[order[0]], "settings") == 0 && count[HOME_GROUP_MORE] == 0);
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
    /* Today's page (DS §47): the favorites, ESSENTIALS's four, FOLDERS's
     * three folder cells, MORE empty. */
    static const uint8_t today[] = { HOME_FAVORITES, 4, 3, 0 };
    /* A launcher of five groups and seventeen places, as it once was: kept
     * for the wrap rule, which today's page no longer needs. */
    static const uint8_t five[] = { 5, 4, 3, 4, 0 };
    struct home_layout_in in;
    struct home_layout l;
    int one_row;

    input(&in, false, today, 4);
    check("portrait lays out", home_layout_compute(&in, &l) == 0);
    check_layout("portrait", &in, &l, false);
    check("portrait: four 124 px columns, 20 px labels", l.cell_w == 124 && !l.small_labels);
    check("portrait: three panels in one column, one row each",
          l.panel[0].x == l.panel[2].x && l.panel[0].w == l.panel[2].w && l.panel[1].y > l.panel[0].y &&
              l.panel[2].y > l.panel[1].y && l.panel[0].h == l.panel[1].h && l.panel[1].h == l.panel[2].h);
    check("portrait: the first panel starts at row 142", l.panel[0].y == 142);

    input(&in, true, today, 4);
    check("landscape lays out", home_layout_compute(&in, &l) == 0);
    check_layout("landscape", &in, &l, false);
    check("landscape: ten places in one row, nothing wrapped",
          !l.wrapped && l.panel[1].y == l.panel[0].y && l.panel[2].y == l.panel[0].y &&
              l.panel[1].x > l.panel[0].x && l.panel[2].x > l.panel[1].x);
    check("landscape: 108 px cells, 16 px labels", l.cell_w == 108 && l.small_labels);
    check("landscape: a panel is exactly as wide as its places",
          l.panel[0].w == 3 * l.cell_w + 2 * HOME_PANEL_PAD && l.panel[1].w == 4 * l.cell_w + 2 * HOME_PANEL_PAD &&
              l.panel[2].w == 3 * l.cell_w + 2 * HOME_PANEL_PAD);
    check("landscape: the row is centred", abs(l.panel[0].x - (in.width - (l.panel[2].x + l.panel[2].w))) <= 1);
    /* With the keyboard base the landscape content area is 452 px tall (DS
     * §36, §40.3): the row and the footer still fit without a scroll. */
    in.height = 452;
    check("landscape above the keyboard lays out", home_layout_compute(&in, &l) == 0);
    check_layout("landscape above the keyboard", &in, &l, false);

    /* Five groups, seventeen places: in one row they would squeeze a cell
     * under HOME_CELL_MIN_W, so by the layout's own rule the panels wrap two
     * to a line and the launcher scrolls to its footer (home_layout.h). */
    input(&in, false, five, 5);
    check("five groups in portrait lay out", home_layout_compute(&in, &l) == 0);
    input(&in, true, five, 5);
    check("five groups in landscape lay out", home_layout_compute(&in, &l) == 0);
    check_layout("landscape, five groups", &in, &l, true);
    one_row = l.panel[1].y == l.panel[0].y && l.panel[1].x > l.panel[0].x && l.panel[3].y == l.panel[2].y &&
              l.panel[3].x > l.panel[2].x && l.panel[2].y > l.panel[0].y;
    check("landscape: seventeen places wrap - two panels on each of two lines, left to right", one_row && l.wrapped);
    check("landscape: wrapped cells are the wrap width, 16 px labels",
          l.cell_w == HOME_CELL_WRAP_W && l.small_labels);
    check("landscape: a wrapped panel is exactly as wide as its apps",
          l.panel[0].w == 5 * l.cell_w + 2 * HOME_PANEL_PAD && l.panel[1].w == 4 * l.cell_w + 2 * HOME_PANEL_PAD &&
              l.panel[2].w == 3 * l.cell_w + 2 * HOME_PANEL_PAD && l.panel[3].w == 4 * l.cell_w + 2 * HOME_PANEL_PAD);
    check("landscape: each line is centred",
          abs(l.panel[0].x - (in.width - (l.panel[1].x + l.panel[1].w))) <= 1 &&
              abs(l.panel[2].x - (in.width - (l.panel[3].x + l.panel[3].w))) <= 1);
    check("landscape: scrolls 70 px to its footer", l.content_h - in.height == 70);
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

/* Whether folder f holds exactly want[0..nwant-1], in that order. */
static int folder_is(enum home_folder f, const char *const *want, int nwant)
{
    uint8_t order[HOME_MAX_APPS];
    int n = home_folder_order(registry, NREG, f, order);
    int k;

    if (n != nwant) {
        printf("     folder %d holds %d app(s), not %d\n", (int)f, n, nwant);
        return 0;
    }
    for (k = 0; k < n; k++) {
        if (strcmp(registry[order[k]], want[k]) != 0) {
            printf("     folder %d: %s where %s was expected\n", (int)f, registry[order[k]], want[k]);
            return 0;
        }
    }
    return 1;
}

static void test_folders(void)
{
    struct home_item items[HOME_MAX_APPS];
    uint8_t order[HOME_MAX_APPS];
    uint8_t count[HOME_GROUP_COUNT];
    const struct home_folder_def *games = home_folder_get(HOME_FOLDER_GAMES);
    const struct home_folder_def *u = home_folder_get(HOME_FOLDER_UTILITIES);
    const struct home_folder_def *a = home_folder_get(HOME_FOLDER_APPS);
    int seen[NREG];
    int n;
    int k;
    int f;
    int ok;

    check("GAMES is a folder with an id and a name", games && strcmp(games->id, "games") == 0 &&
                                                        strcmp(games->name, "Games") == 0 &&
                                                        games->hue == HOME_HUE_GAMES);
    check("UTILITIES is a folder with an id, a name and the tools colour",
          u && strcmp(u->id, "utilities") == 0 && strcmp(u->name, "Utilities") == 0 && u->hue == HOME_HUE_TOOLS);
    check("APPS is a folder with an id, a name and the apps colour",
          a && strcmp(a->id, "apps") == 0 && strcmp(a->name, "Apps") == 0 && a->hue == HOME_HUE_APPS);
    check("each is found by its id, and nothing else is",
          home_folder_find("games") == HOME_FOLDER_GAMES && home_folder_find("utilities") == HOME_FOLDER_UTILITIES &&
              home_folder_find("apps") == HOME_FOLDER_APPS && home_folder_find("fleet") == HOME_FOLDER_NONE &&
              home_folder_find("system") == HOME_FOLDER_NONE && home_folder_find(NULL) == HOME_FOLDER_NONE &&
              home_folder_find("") == HOME_FOLDER_NONE);
    check("NONE and out-of-range have no definition",
          !home_folder_get(HOME_FOLDER_NONE) && !home_folder_get(HOME_FOLDER_COUNT) &&
              !home_folder_get((enum home_folder)-1));
    check("no folder id is an app id", home_favorite_resolve(registry, NREG, "apps") < 0 &&
                                           home_favorite_resolve(registry, NREG, "utilities") < 0 &&
                                           home_favorite_resolve(registry, NREG, "games") < 0);

    /* The launcher's page (DS §47): Terminal, RIFT, Browser, Settings, then
     * the Apps, Utilities and Games cells - seven places for twenty-six apps. */
    n = home_root_order(registry, NREG, items, count);
    check("the launcher's page has seven places", n == 7);
    ok = n == 7;
    for (k = 0; ok && k < LEN(essentials); k++) {
        ok = items[k].folder == HOME_FOLDER_NONE && strcmp(registry[items[k].index], essentials[k]) == 0;
    }
    check("Terminal, RIFT, Browser and Settings, in that order, are its first four", ok);
    check("then the Apps, Utilities and Games cells, in that order",
          n == 7 && items[4].folder == HOME_FOLDER_APPS && items[5].folder == HOME_FOLDER_UTILITIES &&
              items[6].folder == HOME_FOLDER_GAMES);
    check("ESSENTIALS holds four places, FOLDERS three, MORE none",
          count[HOME_GROUP_ESSENTIALS] == 4 && count[HOME_GROUP_FOLDERS] == 3 && count[HOME_GROUP_MORE] == 0);
    {
        static const char *const off_page[] = { "system", "photo", "vision", "deskbuddy", "wave", "mp3", "video",
                                                "zabbix", "radio", "camera", "clock", "calendar", "calculator",
                                                "notes", "files", "recorder", "fleet", "radar", "timber",
                                                "solitaire", "blackjack", "2048" };

        ok = 1;
        for (k = 0; k < LEN(off_page); k++) {
            if (items_hold(items, n, registry, off_page[k])) {
                printf("     %s is on the launcher's page\n", off_page[k]);
                ok = 0;
            }
        }
        check("System, every app of Apps and Utilities and every game are off the launcher's page", ok);
    }

    check("Apps holds DeskBuddy, MP3, Photo, Radio, Video, Vision, Wave and Zabbix, in that order",
          folder_is(HOME_FOLDER_APPS, apps_folder, LEN(apps_folder)));
    check("Utilities holds Clock, Calendar, Calculator, Notes, Files, Recorder and Camera, in that order",
          folder_is(HOME_FOLDER_UTILITIES, utilities_folder, LEN(utilities_folder)));
    check("Games holds Fleet, Radar, Timber, Solitaire, Blackjack and 2048, in that order",
          folder_is(HOME_FOLDER_GAMES, games_folder, LEN(games_folder)));
    check("NONE and an unknown folder hold nothing",
          home_folder_order(registry, NREG, HOME_FOLDER_NONE, order) == 0 &&
              home_folder_order(registry, NREG, HOME_FOLDER_COUNT, order) == 0);

    /* Every app in one place: its own cell or one folder - System in none. */
    memset(seen, 0, sizeof(seen));
    for (k = 0; k < n; k++) {
        if (items[k].folder == HOME_FOLDER_NONE) {
            seen[items[k].index]++;
        }
    }
    for (f = HOME_FOLDER_NONE + 1; f < HOME_FOLDER_COUNT; f++) {
        int m = home_folder_order(registry, NREG, (enum home_folder)f, order);

        for (k = 0; k < m; k++) {
            seen[order[k]]++;
        }
    }
    ok = 1;
    for (k = 0; k < NREG; k++) {
        int want = strcmp(registry[k], "system") == 0 ? 0 : 1;

        if (seen[k] != want) {
            printf("     %s is in %d place(s)\n", registry[k], seen[k]);
            ok = 0;
        }
    }
    check("no app is in two places, and every app but System is in exactly one", ok);

    {
        /* An empty folder: no game installed. */
        static const char *const ids[] = { "notes", "radio", "settings" };

        n = home_root_order(ids, 3, items, count);
        check("with no game installed there is no Games cell", n == 3 && folder_place(items, n, HOME_FOLDER_GAMES) < 0);
        check("and the folder is empty", home_folder_order(ids, 3, HOME_FOLDER_GAMES, order) == 0);
        check("Settings, Apps and Utilities are the places",
              items_hold(items, n, ids, "settings") && folder_place(items, n, HOME_FOLDER_APPS) == 1 &&
                  folder_place(items, n, HOME_FOLDER_UTILITIES) == 2);
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
        /* A shell built without Zabbix (-DPOCKETOS_WITH_ZABBIX=OFF): Apps is
         * one app shorter and still in its place. */
        static const char *const ids[] = { "radio", "system", "settings", "photo", "vision", "terminal" };

        n = home_root_order(ids, 6, items, count);
        check("without Zabbix the Apps cell is still after the essentials",
              n == 3 && folder_place(items, n, HOME_FOLDER_APPS) == 2 && items_hold(items, n, ids, "terminal") &&
                  items_hold(items, n, ids, "settings") && !items_hold(items, n, ids, "system"));
        check("and holds what is installed, in its order",
              home_folder_order(ids, 6, HOME_FOLDER_APPS, order) == 3 && strcmp(ids[order[0]], "photo") == 0 &&
                  strcmp(ids[order[1]], "radio") == 0 && strcmp(ids[order[2]], "vision") == 0);
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

    /* Favorites stored before DS §47 are app ids, not places, so they still
     * resolve when their app has moved into a folder - and System, which has
     * no place on the launcher any more, still resolves and opens. */
    {
        static const char *const moved[] = { "photo", "vision", "deskbuddy", "wave", "mp3", "video", "zabbix",
                                              "radio", "camera", "clock", "notes", "fleet", "2048", "system",
                                              "terminal", "settings" };
        int ok = 1;

        for (k = 0; k < LEN(moved); k++) {
            int i = home_favorite_resolve(registry, NREG, moved[k]);

            if (i < 0 || strcmp(registry[i], moved[k]) != 0 || home_favorite_check(registry, NREG, none, 1, moved[k]) != 0) {
                printf("     a favorite holding %s no longer resolves\n", moved[k]);
                ok = 0;
            }
        }
        check("a favorite holding an app now in a folder, or System, still resolves to it", ok);
    }

    /* Assign, change, duplicates. */
    check("an empty slot may be given any installed app", home_favorite_check(registry, NREG, none, 0, "rift") == 0 &&
                                                              home_favorite_check(registry, NREG, none, 2, "2048") == 0);
    check("and an app inside any folder",
          home_favorite_check(registry, NREG, none, 1, "calculator") == 0 &&
              home_favorite_check(registry, NREG, none, 1, "photo") == 0 &&
              home_favorite_check(registry, NREG, none, 1, "solitaire") == 0);
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
    check("nor a folder", home_favorite_check(registry, NREG, none, 0, "apps") == -1 &&
                              home_favorite_check(registry, NREG, none, 0, "games") == -1);

    /* The picker's list. */
    n = home_favorite_candidates(registry, NREG, none, 0, order);
    same = n == NPLACED;
    for (k = 0; same && k < n; k++) {
        same = strcmp(registry[order[k]], launcher_order[k]) == 0;
    }
    check("with nothing set the picker offers every app but System, once, in launcher order, folders' apps too",
          same);
    n = home_favorite_candidates(registry, NREG, some, 1, order);
    same = n == NPLACED - 2;
    for (k = 0; k < n; k++) {
        same = same && strcmp(registry[order[k]], "rift") != 0 && strcmp(registry[order[k]], "calculator") != 0 &&
               strcmp(registry[order[k]], "system") != 0;
    }
    check("it leaves out what the other slots hold", same);
    n = home_favorite_candidates(registry, NREG, some, 0, order);
    same = n == NPLACED - 1;
    for (k = 0; k < n; k++) {
        same = same && strcmp(registry[order[k]], "calculator") != 0;
    }
    check("but offers the slot's own app (choosing it changes nothing)",
          same && home_favorite_resolve(registry, NREG, "rift") >= 0 && strcmp(registry[order[1]], "rift") == 0);
    {
        /* A slot holding System, kept from before DS §47: the picker for it
         * offers everything else, and the slot keeps System until changed. */
        const char *with_system[HOME_FAVORITES] = { "system", NULL, NULL };

        n = home_favorite_candidates(registry, NREG, with_system, 1, order);
        same = n == NPLACED;
        for (k = 0; k < n; k++) {
            same = same && strcmp(registry[order[k]], "system") != 0;
        }
        check("a slot holding System is not disturbed: the picker never offers System", same);
        check("and another slot may not take it while one holds it",
              home_favorite_check(registry, NREG, with_system, 1, "system") == -2);
    }
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
            snprintf(what, sizeof(what), "%s: the favorites' panel and the groups", name);
            check(what, in.ngroups == HOME_GROUP_COUNT + 1 && in.count[0] == HOME_FAVORITES &&
                            in.count[1] == count[HOME_GROUP_ESSENTIALS] && in.count[2] == count[HOME_GROUP_FOLDERS] &&
                            in.count[HOME_GROUP_COUNT] == count[HOME_GROUP_MORE]);
            snprintf(what, sizeof(what), "%s: lays out", name);
            check(what, home_layout_compute(&in, &l) == 0);
            check_layout(name, &in, &l, false);
            snprintf(what, sizeof(what), "%s: 10 cells, three favorites and seven places", name);
            check(what, l.napps == HOME_FAVORITES + n && n == 7);
            for (k = 0; k < HOME_FAVORITES; k++) {
                first = first && inside(&l.cell[k], &l.panel[0]) && l.cell[k].y == l.cell[0].y &&
                        (k == 0 || l.cell[k].x > l.cell[k - 1].x);
            }
            /* Every other cell is on a later row, or - landscape, where all
             * three panels share one line - further right. */
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
            snprintf(what, sizeof(what), "%s: one row per panel, nothing wrapped", name);
            check(what, !l.wrapped && (land ? l.cell[l.napps - 1].y == l.cell[0].y
                                            : l.cell[HOME_FAVORITES + 3].y == l.cell[HOME_FAVORITES].y));
        }
        check("landscape with favorites does not scroll", l.content_h == in.height);
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
