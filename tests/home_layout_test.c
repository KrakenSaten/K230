/*
 * The DOORS launcher's geometry and groups (ui/shell/home_layout.h, DS §31):
 *
 *   - the registry's twenty-one apps land in four groups in the table's order,
 *     an app the table does not name goes to MORE (after the named groups,
 *     in registry order), an empty group is not drawn;
 *   - in both orientations of the reference panel every cell is inside its
 *     panel and inside the content area, no two cells or panels overlap,
 *     cells are at least the DS touch minimum, and the footer lies below the
 *     panels and clear of the rounded corners; portrait does not scroll;
 *   - landscape wraps (and then scrolls) instead of squeezing cells: with
 *     today's seventeen apps DEVICE goes to a second line (twelve fitted one).
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
                                        "files", "camera", "browser", "recorder", "vision", "solitaire",
                                        "blackjack", "2048", "zabbix" };
#define NREG ((int)(sizeof(registry) / sizeof(registry[0])))

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
    static const char *const want[] = { "rift", "radio", "wave", "zabbix", "browser", "notes", "calendar", "clock",
                                        "calculator", "fleet", "radar", "timber", "solitaire", "blackjack",
                                        "2048", "settings", "system", "files", "camera", "recorder", "vision" };
    int n = home_group_order(registry, NREG, order, count);
    int k;
    int same = n == 21;

    for (k = 0; same && k < n; k++) {
        same = strcmp(registry[order[k]], want[k]) == 0;
    }
    check("the twenty-one apps are shown in the table's order", same);
    check("CONNECTIONS holds RIFT, Radio, Wave, Zabbix, Browser", count[HOME_GROUP_CONNECT] == 5);
    check("WORKSPACE holds Notes, Calendar, Clock, Calculator", count[HOME_GROUP_WORK] == 4);
    check("PLAY holds Fleet, Radar, Timber, Solitaire, Blackjack, 2048", count[HOME_GROUP_PLAY] == 6);
    check("DEVICE holds Settings, System, Files, Camera, Recorder, Vision", count[HOME_GROUP_DEVICE] == 6);
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

int main(void)
{
    test_groups();
    test_reference();
    test_growth();
    printf("home_layout_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
