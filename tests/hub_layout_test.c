/*
 * Games Hub geometry test (ui/shell/hub_layout.c, DS §60): the wall in both
 * orientations - no two posters overlap, everything fits the content area
 * and keeps out of the status cluster and the panel's corner squares, every
 * launch target and PLAY is large enough, the lobby's parts stay inside the
 * lobby - the frames it must refuse, the games it stands for (the launcher's
 * own Games folder, ui/shell/home_layout.c), and the keys.
 *
 *   hub_layout_test          run the checks
 *   hub_layout_test --art    print "<art name> <w> <h>" for every tile in
 *                            both orientations (tests/doors_ui_assets_test.sh
 *                            holds the art files to it)
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "home_layout.h"
#include "hub_layout.h"

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

static void checkf(const char *what, const char *name, int ok)
{
    char line[160];

    snprintf(line, sizeof(line), "%s: %s", what, name);
    check(line, ok);
}

/* The frame the launcher gives the hub (DS §36): the whole display, the
 * corner squares of DS §21.1 (30 px; 50 at the top in landscape), and the
 * status cluster's box on the shell's screens (about 113 px wide, 14 px
 * down, 44 tall, anchored to the top-right corner). */
#define CLUSTER_W 113

static struct hub_frame frame(bool landscape, int32_t w, int32_t h, bool cluster)
{
    struct hub_frame f;
    int32_t top = landscape ? 50 : 30;

    memset(&f, 0, sizeof(f));
    f.landscape = landscape;
    f.width = w;
    f.height = h;
    f.corner_top_left = top;
    f.corner_top_right = top;
    f.corner_bottom_left = 30;
    f.corner_bottom_right = 30;
    if (cluster) {
        f.keepout.x = w - top - CLUSTER_W;
        f.keepout.y = 14;
        f.keepout.w = CLUSTER_W;
        f.keepout.h = 44;
    }
    return f;
}

static int inside_tile(const struct hub_rect *part, const struct hub_rect *tile)
{
    return part->w > 0 && part->h > 0 && hub_rect_inside(part, tile);
}

static void check_layout(const char *what, const struct hub_frame *f)
{
    struct hub_layout l;
    struct hub_rect area = { 0, 0, f->width, f->height };
    struct hub_rect corner_box;
    struct hub_rect corners[4];
    int rc = hub_layout(f, &l);
    int ok;
    int a;
    int b;

    checkf(what, "there is a layout", rc == 0);
    if (rc != 0) {
        return;
    }
    corners[0] = (struct hub_rect){ 0, 0, f->corner_top_left, f->corner_top_left };
    corners[1] = (struct hub_rect){ f->width - f->corner_top_right, 0, f->corner_top_right, f->corner_top_right };
    corners[2] = (struct hub_rect){ 0, f->height - f->corner_bottom_left, f->corner_bottom_left,
                                    f->corner_bottom_left };
    corners[3] = (struct hub_rect){ f->width - f->corner_bottom_right, f->height - f->corner_bottom_right,
                                    f->corner_bottom_right, f->corner_bottom_right };

    ok = 1;
    for (a = 0; a < HUB_TILE_COUNT; a++) {
        for (b = a + 1; b < HUB_TILE_COUNT; b++) {
            ok = ok && !hub_rects_overlap(&l.tile[a], &l.tile[b]);
        }
    }
    checkf(what, "no two tiles overlap", ok);
    ok = 1;
    for (a = 0; a < HUB_TILE_COUNT; a++) {
        ok = ok && l.tile[a].w > 0 && l.tile[a].h > 0 && hub_rect_inside(&l.tile[a], &area) &&
             hub_rect_inside(&l.tile[a], &l.wall);
    }
    checkf(what, "every tile is inside the content area and the wall's box", ok);
    ok = 1;
    for (a = 0; a < HUB_TILE_COUNT; a++) {
        ok = ok && !hub_rects_overlap(&l.tile[a], &f->keepout);
        for (b = 0; b < 4; b++) {
            ok = ok && !hub_rects_overlap(&l.tile[a], &corners[b]);
        }
    }
    checkf(what, "no tile lies under the status cluster or in a corner square", ok);
    ok = 1;
    for (a = 0; a < HUB_GAMES; a++) {
        ok = ok && l.tile[a].w >= HUB_TOUCH_MIN_W && l.tile[a].h >= HUB_TOUCH_MIN_H;
    }
    checkf(what, "every poster is at least the smallest launch target", ok);
    ok = 1;
    for (a = 0; a < HUB_FEATURED; a++) {
        ok = ok && inside_tile(&l.chip[a], &l.tile[a]) && inside_tile(&l.play[a], &l.tile[a]) &&
             !hub_rects_overlap(&l.chip[a], &l.play[a]) && l.play[a].w >= HUB_PLAY_MIN_W &&
             l.play[a].h >= HUB_PLAY_MIN_H;
    }
    checkf(what, "FEATURED and PLAY lie on their posters, PLAY at least 134 x 46", ok);

    /* The header: the back corner (DS §48: from the screen's edges to the
     * slab's right edge and the row's foot), the title and the count. */
    corner_box = (struct hub_rect){ 0, 0, l.back.x + l.back.w, HUB_HEADER_H };
    ok = l.back.w == HUB_BACK_W && l.back.h == HUB_BACK_H && l.back.y == HUB_BACK_Y &&
         l.back.x == (f->landscape ? 36 : 28);
    checkf(what, "the back slab is where the folder page's is", ok);
    ok = !hub_rects_overlap(&corner_box, &l.title) && !hub_rects_overlap(&corner_box, &l.meta) &&
         !hub_rects_overlap(&l.title, &l.meta) && !hub_rects_overlap(&l.title, &f->keepout) &&
         !hub_rects_overlap(&l.meta, &f->keepout) && hub_rect_inside(&l.title, &area) &&
         hub_rect_inside(&l.meta, &area) && l.title.w >= 120 && l.meta.w >= 200;
    checkf(what, "the title and the count are clear of the back corner, the cluster and each other", ok);
    ok = l.wall.y >= HUB_HEADER_H && !hub_rects_overlap(&l.wall, &l.title) && !hub_rects_overlap(&l.wall, &l.meta) &&
         !hub_rects_overlap(&l.wall, &corner_box);
    checkf(what, "the wall starts under the header", ok);

    /* The lobby: one widget, its parts inside it and apart. */
    {
        const struct hub_rect *lobby = &l.tile[HUB_TILE_LOBBY];

        ok = inside_tile(&l.lobby_chip, lobby) && inside_tile(&l.lobby_host, lobby) &&
             inside_tile(&l.lobby_join, lobby) && !hub_rects_overlap(&l.lobby_host, &l.lobby_join) &&
             !hub_rects_overlap(&l.lobby_chip, &l.lobby_host) && !hub_rects_overlap(&l.lobby_chip, &l.lobby_join) &&
             l.lobby_host.w == l.lobby_join.w && l.lobby_host.h == l.lobby_join.h && l.lobby_host.y == l.lobby_join.y;
        if (l.lobby_text.w > 0) {
            ok = ok && inside_tile(&l.lobby_text, lobby) && !hub_rects_overlap(&l.lobby_text, &l.lobby_chip) &&
                 !hub_rects_overlap(&l.lobby_text, &l.lobby_host) && !hub_rects_overlap(&l.lobby_text, &l.lobby_join);
        }
        checkf(what, "the lobby's chip, sentence and two buttons lie inside it, apart", ok);
    }
    if (f->landscape) {
        ok = inside_tile(&l.badge, &l.tile[HUB_TILE_TIMBER]) && l.hint.w == 0 && l.lobby_text.w == 0;
        checkf(what, "Timber carries the PORTRAIT badge; no hint line, no lobby sentence", ok);
    } else {
        ok = l.badge.w == 0 && l.hint.w > 0 && hub_rect_inside(&l.hint, &area) &&
             l.hint.y >= l.wall.y + l.wall.h && !hub_rects_overlap(&l.hint, &corners[2]) &&
             !hub_rects_overlap(&l.hint, &corners[3]) && l.lobby_text.w > 0;
        checkf(what, "no badge; the hint line is under the wall and above the bottom corners", ok);
    }
}

static int refused(const struct hub_frame *f)
{
    struct hub_layout l;

    return hub_layout(f, &l) == -1;
}

static void test_games(void)
{
    const char *ids[HUB_GAMES + 1];
    uint8_t order[HOME_MAX_APPS];
    char name[64];
    int t;
    int ok = 1;

    for (t = 0; t < HUB_GAMES; t++) {
        const struct hub_game *g = hub_game(t);
        const struct home_entry *e = g ? home_entry_find(g->id) : NULL;

        ok = ok && g && e && e->folder == HOME_FOLDER_GAMES && hub_tile_find(g->id) == t;
        ids[t] = g ? g->id : "";
    }
    check("every hub game is in the launcher's Games folder, and found by its id", ok);
    ids[HUB_GAMES] = "notes";
    check("the launcher's Games folder holds exactly the hub's nine",
          home_folder_order(ids, HUB_GAMES + 1, HOME_FOLDER_GAMES, order) == HUB_GAMES);
    check("the lobby and anything out of range are no game",
          hub_game(HUB_TILE_LOBBY) == NULL && hub_game(-1) == NULL && hub_game(HUB_TILE_COUNT) == NULL &&
              hub_tile_find("lobby") == -1 && hub_tile_find(NULL) == -1 && hub_tile_find("notes") == -1);
    check("the featured games are RotorTerror then Relay Nine, and only they",
          strcmp(hub_game(0)->id, "rotorterror") == 0 && strcmp(hub_game(1)->id, "neon-outpost") == 0 &&
              hub_game(0)->featured && hub_game(1)->featured && !hub_game(2)->featured && !hub_game(8)->featured);
    ok = 1;
    for (t = 0; t < HUB_GAMES; t++) {
        ok = ok && hub_game(t)->portrait_only == (t == HUB_TILE_TIMBER);
    }
    check("Timber alone is portrait-only (DS §53)", ok);
    check("the other seven are Fleet, Radar, Timber, Solitaire, Blackjack, Poker, 2048",
          strcmp(hub_game(2)->id, "fleet") == 0 && strcmp(hub_game(3)->id, "radar") == 0 &&
              strcmp(hub_game(4)->id, "timber") == 0 && strcmp(hub_game(5)->id, "solitaire") == 0 &&
              strcmp(hub_game(6)->id, "blackjack") == 0 && strcmp(hub_game(7)->id, "poker") == 0 &&
              strcmp(hub_game(8)->id, "2048") == 0);

    check("the nine in the hub's order match", hub_games_match(ids, HUB_GAMES));
    {
        const char *rev[HUB_GAMES];

        for (t = 0; t < HUB_GAMES; t++) {
            rev[t] = ids[HUB_GAMES - 1 - t];
        }
        check("and in any other order", hub_games_match(rev, HUB_GAMES));
        rev[3] = rev[4];
        check("a game twice does not", !hub_games_match(rev, HUB_GAMES));
        rev[3] = "g01";
        check("nor a game the hub has no poster for", !hub_games_match(rev, HUB_GAMES));
    }
    check("eight of them, ten, or none do not",
          !hub_games_match(ids, HUB_GAMES - 1) && !hub_games_match(ids, HUB_GAMES + 1) && !hub_games_match(ids, 0) &&
              !hub_games_match(NULL, HUB_GAMES));

    check("art names", hub_art_name(HUB_TILE_RELAY_NINE, false, name, sizeof(name)) == 0 &&
                           strcmp(name, "hub-relay-nine-portrait") == 0 &&
                           hub_art_name(HUB_TILE_LOBBY, true, name, sizeof(name)) == 0 &&
                           strcmp(name, "hub-lobby-landscape") == 0 &&
                           hub_art_name(HUB_TILE_2048, true, name, sizeof(name)) == 0 &&
                           strcmp(name, "hub-2048-landscape") == 0);
    check("no art name for a tile out of range, or into a buffer too small",
          hub_art_name(HUB_TILE_COUNT, false, name, sizeof(name)) == -1 &&
              hub_art_name(-1, false, name, sizeof(name)) == -1 && hub_art_name(0, false, name, 8) == -1);
}

static void test_layouts(void)
{
    struct hub_frame f;
    struct hub_layout l;
    struct hub_layout wide;

    f = frame(false, 568, 1232, true);
    check_layout("portrait 568x1232", &f);
    f = frame(true, 1232, 568, true);
    check_layout("landscape 1232x568", &f);
    f = frame(false, 568, 1232, false);
    check_layout("portrait, no cluster", &f);
    f = frame(true, 1232, 568, false);
    check_layout("landscape, no cluster", &f);
    /* A rectangular panel (the simulator's default, HDMI). */
    f = frame(false, 568, 1232, true);
    f.corner_top_left = f.corner_top_right = f.corner_bottom_left = f.corner_bottom_right = 0;
    f.keepout.x = 568 - 20 - CLUSTER_W;
    check_layout("portrait, square corners", &f);
    f = frame(true, 1920, 1080, true);
    f.corner_top_left = f.corner_top_right = f.corner_bottom_left = f.corner_bottom_right = 0;
    check_layout("landscape 1920x1080", &f);

    /* The README's sizes, to the pixel. */
    f = frame(false, 568, 1232, true);
    hub_layout(&f, &l);
    check("portrait: RotorTerror 528 x 300, Relay Nine 258 x 310",
          l.tile[HUB_TILE_ROTORTERROR].w == 528 && l.tile[HUB_TILE_ROTORTERROR].h == 300 &&
              l.tile[HUB_TILE_RELAY_NINE].w == 258 && l.tile[HUB_TILE_RELAY_NINE].h == 310);
    check("portrait: four tiles 125 x 150, three 169 x 160, the lobby 528 x 200",
          l.tile[HUB_TILE_FLEET].w == 125 && l.tile[HUB_TILE_FLEET].h == 150 && l.tile[HUB_TILE_RADAR].w == 125 &&
              l.tile[HUB_TILE_TIMBER].h == 150 && l.tile[HUB_TILE_2048].w == 125 &&
              l.tile[HUB_TILE_SOLITAIRE].w == 169 && l.tile[HUB_TILE_BLACKJACK].w == 169 &&
              l.tile[HUB_TILE_POKER].w == 169 && l.tile[HUB_TILE_POKER].h == 160 &&
              l.tile[HUB_TILE_LOBBY].w == 528 && l.tile[HUB_TILE_LOBBY].h == 200);
    check("portrait: the wall is 20 px from each side and ends above the bottom corners",
          l.wall.x == 20 && l.wall.x + l.wall.w == 548 && l.wall.y + l.wall.h <= 1232 - 30);
    f = frame(true, 1232, 568, true);
    hub_layout(&f, &l);
    check("landscape: two tall featured posters, a 3 x 2 block of 155 px tiles, 2048 and the lobby below",
          l.tile[HUB_TILE_ROTORTERROR].h == l.tile[HUB_TILE_RELAY_NINE].h && l.tile[HUB_TILE_ROTORTERROR].h == 468 &&
              l.tile[HUB_TILE_ROTORTERROR].w == 378 && l.tile[HUB_TILE_RELAY_NINE].w == 290 &&
              l.tile[HUB_TILE_FLEET].w == 155 && l.tile[HUB_TILE_POKER].w == 155 &&
              l.tile[HUB_TILE_FLEET].y == l.tile[HUB_TILE_TIMBER].y &&
              l.tile[HUB_TILE_SOLITAIRE].y == l.tile[HUB_TILE_POKER].y &&
              l.tile[HUB_TILE_2048].y == l.tile[HUB_TILE_LOBBY].y &&
              l.tile[HUB_TILE_2048].x == l.tile[HUB_TILE_FLEET].x &&
              l.tile[HUB_TILE_LOBBY].x == l.tile[HUB_TILE_RADAR].x);
    check("landscape: the wall keeps 30 px from each side, so it is beside the bottom corners, not in them",
          l.wall.x == 30 && l.wall.x + l.wall.w == 1232 - 30 && l.wall.y + l.wall.h == 544);
    f = frame(true, 1920, 1080, false);
    hub_layout(&f, &wide);
    check("a wider area centres the wall and does not scale it",
          wide.wall.w == l.wall.w && wide.wall.h == l.wall.h && wide.wall.x == (1920 - 1232) / 2 + 30 &&
              wide.tile[HUB_TILE_LOBBY].w == l.tile[HUB_TILE_LOBBY].w && wide.back.x == l.back.x);

    /* What cannot hold the wall gets no layout: the launcher then shows the
     * folder page. */
    f = frame(false, 568, 1100, true);
    check("portrait above a keyboard (568 x 1100) is refused", refused(&f));
    f = frame(true, 1232, 452, true);
    check("landscape above a keyboard (1232 x 452) is refused", refused(&f));
    f = frame(false, 480, 1232, true);
    check("a narrower portrait is refused", refused(&f));
    f = frame(true, 1024, 600, true);
    check("a narrower landscape is refused", refused(&f));
    f = frame(true, 1232, 568, true);
    f.corner_bottom_left = 50;
    check("a bottom corner that would cut a poster is refused", refused(&f));
    f = frame(true, 1232, 568, true);
    f.keepout.h = 80;
    check("a status cluster reaching into the wall is refused", refused(&f));
    f = frame(false, 568, 1232, true);
    f.keepout.x = 100;
    f.keepout.w = 438;
    check("a status cluster leaving the title no room is refused", refused(&f));
    check("no frame, no layout", hub_layout(NULL, &l) == -1 && hub_layout(&f, NULL) == -1);
}

static int reach_all(const struct hub_layout *l, int from)
{
    bool seen[HUB_GAMES] = { false };
    int queue[HUB_GAMES];
    int head = 0;
    int tail = 0;
    int n = 0;
    int d;

    seen[from] = true;
    queue[tail++] = from;
    while (head < tail) {
        int t = queue[head++];

        n++;
        for (d = HUB_DIR_LEFT; d <= HUB_DIR_DOWN; d++) {
            int to = hub_neighbour(l, t, (enum hub_dir)d);

            if (to < 0 || to >= HUB_GAMES) {
                return 0; /* the lobby, or nonsense */
            }
            if (!seen[to]) {
                seen[to] = true;
                queue[tail++] = to;
            }
        }
    }
    return n == HUB_GAMES;
}

static void test_keys(void)
{
    struct hub_frame f;
    struct hub_layout p;
    struct hub_layout w;
    int t;
    int ok;

    f = frame(false, 568, 1232, true);
    hub_layout(&f, &p);
    f = frame(true, 1232, 568, true);
    hub_layout(&f, &w);

    ok = 1;
    for (t = 0; t < HUB_GAMES; t++) {
        ok = ok && reach_all(&p, t) && reach_all(&w, t);
    }
    check("from every poster the arrows reach every other, and never the lobby", ok);

#define MOVE(l, from, dir, to) (hub_neighbour(&(l), HUB_TILE_##from, HUB_DIR_##dir) == HUB_TILE_##to)
    check("portrait: Down from RotorTerror is Relay Nine, Up comes back",
          MOVE(p, ROTORTERROR, DOWN, RELAY_NINE) && MOVE(p, RELAY_NINE, UP, ROTORTERROR));
    check("portrait: Right from Relay Nine is Fleet, then Radar; Left comes back",
          MOVE(p, RELAY_NINE, RIGHT, FLEET) && MOVE(p, FLEET, RIGHT, RADAR) && MOVE(p, RADAR, LEFT, FLEET) &&
              MOVE(p, FLEET, LEFT, RELAY_NINE) && MOVE(p, TIMBER, LEFT, RELAY_NINE));
    check("portrait: the 2 x 2 block: Fleet over Timber, Radar over 2048",
          MOVE(p, FLEET, DOWN, TIMBER) && MOVE(p, TIMBER, UP, FLEET) && MOVE(p, RADAR, DOWN, 2048) &&
              MOVE(p, 2048, UP, RADAR) && MOVE(p, TIMBER, RIGHT, 2048) && MOVE(p, 2048, LEFT, TIMBER));
    check("portrait: the row of three under them",
          MOVE(p, RELAY_NINE, DOWN, SOLITAIRE) && MOVE(p, SOLITAIRE, UP, RELAY_NINE) &&
              MOVE(p, SOLITAIRE, RIGHT, BLACKJACK) && MOVE(p, BLACKJACK, RIGHT, POKER) &&
              MOVE(p, POKER, LEFT, BLACKJACK) && MOVE(p, TIMBER, DOWN, BLACKJACK) && MOVE(p, 2048, DOWN, POKER) &&
              MOVE(p, POKER, UP, 2048) && MOVE(p, BLACKJACK, UP, TIMBER));
    check("portrait: Fleet and Radar go up to RotorTerror",
          MOVE(p, FLEET, UP, ROTORTERROR) && MOVE(p, RADAR, UP, ROTORTERROR));
    check("portrait: the focus stops at the wall's edges, and above the lobby",
          MOVE(p, ROTORTERROR, UP, ROTORTERROR) && MOVE(p, ROTORTERROR, LEFT, ROTORTERROR) &&
              MOVE(p, ROTORTERROR, RIGHT, ROTORTERROR) && MOVE(p, SOLITAIRE, DOWN, SOLITAIRE) &&
              MOVE(p, BLACKJACK, DOWN, BLACKJACK) && MOVE(p, POKER, DOWN, POKER) && MOVE(p, POKER, RIGHT, POKER) &&
              MOVE(p, SOLITAIRE, LEFT, SOLITAIRE) && MOVE(p, RADAR, RIGHT, RADAR));

    check("landscape: RotorTerror, Relay Nine, Fleet, Radar, Timber along the top",
          MOVE(w, ROTORTERROR, RIGHT, RELAY_NINE) && MOVE(w, RELAY_NINE, RIGHT, FLEET) &&
              MOVE(w, FLEET, RIGHT, RADAR) && MOVE(w, RADAR, RIGHT, TIMBER) && MOVE(w, TIMBER, LEFT, RADAR) &&
              MOVE(w, FLEET, LEFT, RELAY_NINE) && MOVE(w, RELAY_NINE, LEFT, ROTORTERROR));
    check("landscape: the block's second row, and 2048 under it",
          MOVE(w, FLEET, DOWN, SOLITAIRE) && MOVE(w, RADAR, DOWN, BLACKJACK) && MOVE(w, TIMBER, DOWN, POKER) &&
              MOVE(w, SOLITAIRE, DOWN, 2048) && MOVE(w, 2048, UP, SOLITAIRE) && MOVE(w, POKER, UP, TIMBER) &&
              MOVE(w, SOLITAIRE, LEFT, RELAY_NINE) && MOVE(w, 2048, LEFT, RELAY_NINE));
    check("landscape: the focus stops at the wall's edges, and beside and above the lobby",
          MOVE(w, ROTORTERROR, LEFT, ROTORTERROR) && MOVE(w, ROTORTERROR, UP, ROTORTERROR) &&
              MOVE(w, ROTORTERROR, DOWN, ROTORTERROR) && MOVE(w, TIMBER, RIGHT, TIMBER) &&
              MOVE(w, 2048, RIGHT, 2048) && MOVE(w, 2048, DOWN, 2048) && MOVE(w, BLACKJACK, DOWN, BLACKJACK) &&
              MOVE(w, POKER, DOWN, POKER));
#undef MOVE
    check("a focus that is no game stays where it is",
          hub_neighbour(&p, HUB_TILE_LOBBY, HUB_DIR_UP) == HUB_TILE_LOBBY && hub_neighbour(&p, -1, HUB_DIR_UP) == -1 &&
              hub_neighbour(NULL, 2, HUB_DIR_UP) == 2);
}

static int print_art(void)
{
    int o;
    int t;

    for (o = 0; o < 2; o++) {
        struct hub_frame f = frame(o == 1, o ? HUB_LANDSCAPE_W : HUB_PORTRAIT_W, o ? HUB_LANDSCAPE_H : HUB_PORTRAIT_H,
                                   true);
        struct hub_layout l;
        char name[64];

        if (hub_layout(&f, &l) != 0) {
            return 1;
        }
        for (t = 0; t < HUB_TILE_COUNT; t++) {
            if (hub_art_name(t, o == 1, name, sizeof(name)) != 0) {
                return 1;
            }
            printf("%s %d %d %d %d\n", name, (int)l.tile[t].w, (int)l.tile[t].h, (int)l.tile[t].x, (int)l.tile[t].y);
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--art") == 0) {
        return print_art();
    }
    test_games();
    test_layouts();
    test_keys();
    printf("hub_layout_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
