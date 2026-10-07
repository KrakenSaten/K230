/*
 * PG 2048 view model: the key map on every panel, swipes, the layout in tall
 * and wide areas, the board grid, the tile look's structure, labels and the
 * motion curves.
 *
 * The colour contract - which fills and texts the tiles resolve to in every
 * theme and mode - is tests/g2048_theme_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "g2048_view.h"

#include <stdio.h>
#include <string.h>

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

/* ---- keys ------------------------------------------------------------------ */

static void test_keys(void)
{
    static const uint32_t others[] = { ' ', 'q', 'x', 'k', '2', '\t', 11u /* PREV */, 127u /* DEL */,
                                       2u /* HOME */, 3u /* END */, 0xE6u /* ae */, 0u };
    static const enum g2048_panel panels[] = { G2048_PANEL_PLAY, G2048_PANEL_CONFIRM,
                                               G2048_PANEL_WON, G2048_PANEL_OVER };
    const enum g2048_panel P = G2048_PANEL_PLAY;
    size_t i;
    size_t p;

    check("Up arrow moves up", g2048_view_command_for_key(P, 1, G2048_KEY_UP) == G2048_CMD_MOVE_UP);
    check("Down arrow moves down", g2048_view_command_for_key(P, 1, G2048_KEY_DOWN) == G2048_CMD_MOVE_DOWN);
    check("Left arrow moves left", g2048_view_command_for_key(P, 1, G2048_KEY_LEFT) == G2048_CMD_MOVE_LEFT);
    check("Right arrow moves right", g2048_view_command_for_key(P, 1, G2048_KEY_RIGHT) == G2048_CMD_MOVE_RIGHT);
    check("w and W move up", g2048_view_command_for_key(P, 1, 'w') == G2048_CMD_MOVE_UP &&
                                 g2048_view_command_for_key(P, 1, 'W') == G2048_CMD_MOVE_UP);
    check("a and A move left", g2048_view_command_for_key(P, 1, 'a') == G2048_CMD_MOVE_LEFT &&
                                   g2048_view_command_for_key(P, 1, 'A') == G2048_CMD_MOVE_LEFT);
    check("s and S move down", g2048_view_command_for_key(P, 1, 's') == G2048_CMD_MOVE_DOWN &&
                                   g2048_view_command_for_key(P, 1, 'S') == G2048_CMD_MOVE_DOWN);
    check("d and D move right", g2048_view_command_for_key(P, 1, 'd') == G2048_CMD_MOVE_RIGHT &&
                                    g2048_view_command_for_key(P, 1, 'D') == G2048_CMD_MOVE_RIGHT);
    check("n with a board to lose asks first", g2048_view_command_for_key(P, 1, 'n') == G2048_CMD_ASK_NEW &&
                                                   g2048_view_command_for_key(P, 1, 'N') == G2048_CMD_ASK_NEW);
    check("n on an untouched board just starts again",
          g2048_view_command_for_key(P, 0, 'n') == G2048_CMD_NEW_GAME);
    check("Enter, Esc and Backspace do nothing while playing",
          g2048_view_command_for_key(P, 1, G2048_KEY_ENTER) == G2048_CMD_NONE &&
              g2048_view_command_for_key(P, 1, G2048_KEY_ESC) == G2048_CMD_NONE &&
              g2048_view_command_for_key(P, 1, G2048_KEY_BACKSPACE) == G2048_CMD_NONE);

    check("confirming: n starts the new game",
          g2048_view_command_for_key(G2048_PANEL_CONFIRM, 1, 'n') == G2048_CMD_NEW_GAME);
    check("confirming: Enter keeps the board - Enter takes the safe, accented action",
          g2048_view_command_for_key(G2048_PANEL_CONFIRM, 1, G2048_KEY_ENTER) == G2048_CMD_CANCEL);
    check("confirming: Esc and Backspace keep it too",
          g2048_view_command_for_key(G2048_PANEL_CONFIRM, 1, G2048_KEY_ESC) == G2048_CMD_CANCEL &&
              g2048_view_command_for_key(G2048_PANEL_CONFIRM, 1, G2048_KEY_BACKSPACE) == G2048_CMD_CANCEL);
    check("won: Enter and Esc keep going",
          g2048_view_command_for_key(G2048_PANEL_WON, 1, G2048_KEY_ENTER) == G2048_CMD_KEEP_GOING &&
              g2048_view_command_for_key(G2048_PANEL_WON, 1, G2048_KEY_ESC) == G2048_CMD_KEEP_GOING);
    check("won: n starts again", g2048_view_command_for_key(G2048_PANEL_WON, 1, 'N') == G2048_CMD_NEW_GAME);
    check("over: Enter and n start again",
          g2048_view_command_for_key(G2048_PANEL_OVER, 0, G2048_KEY_ENTER) == G2048_CMD_NEW_GAME &&
              g2048_view_command_for_key(G2048_PANEL_OVER, 0, 'n') == G2048_CMD_NEW_GAME);
    check("over: Esc does not throw the final board away",
          g2048_view_command_for_key(G2048_PANEL_OVER, 0, G2048_KEY_ESC) == G2048_CMD_NONE);

    {
        int moves_elsewhere = 0;
        int others_act = 0;

        for (p = 1; p < 4; p++) {
            static const uint32_t moves[] = { G2048_KEY_UP, G2048_KEY_DOWN, G2048_KEY_LEFT, G2048_KEY_RIGHT,
                                              'w', 'a', 's', 'd' };
            for (i = 0; i < sizeof(moves) / sizeof(moves[0]); i++) {
                moves_elsewhere += g2048_view_command_for_key(panels[p], 1, moves[i]) != G2048_CMD_NONE;
            }
        }
        check("no move key moves the board under a question or a result", moves_elsewhere == 0);
        for (p = 0; p < 4; p++) {
            for (i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
                others_act += g2048_view_command_for_key(panels[p], 1, others[i]) != G2048_CMD_NONE;
            }
        }
        check("keys outside the map do nothing on any panel", others_act == 0);
    }

    {
        int ok = 1;
        int d;

        for (d = 0; d < G2048_DIR_COUNT; d++) {
            enum g2048_dir back = G2048_DIR_COUNT;

            ok &= g2048_view_cmd_dir(g2048_view_move_cmd((enum g2048_dir)d), &back) && (int)back == d;
        }
        check("every direction is a move command and back", ok);
        check("a command that is not a move has no direction",
              !g2048_view_cmd_dir(G2048_CMD_NEW_GAME, NULL) && !g2048_view_cmd_dir(G2048_CMD_NONE, NULL));
    }
}

static void test_panels(void)
{
    struct g2048_game g;

    g2048_new_game(&g, 3, 0);
    check("a new game shows the play panel", g2048_view_panel(&g, 0) == G2048_PANEL_PLAY);
    check("a fresh board has no progress to lose", !g2048_view_has_progress(&g));
    check("asking shows the confirmation", g2048_view_panel(&g, 1) == G2048_PANEL_CONFIRM);
    g.moves = 3;
    check("a moved board has progress", g2048_view_has_progress(&g));
    g.won = 1;
    check("a won game shows the goal, whatever was being asked", g2048_view_panel(&g, 1) == G2048_PANEL_WON);
    g.keep_going = 1;
    check("keep going returns to play", g2048_view_panel(&g, 0) == G2048_PANEL_PLAY);
    g.over = 1;
    check("an ended game shows the end, even while asking", g2048_view_panel(&g, 1) == G2048_PANEL_OVER);
    check("and has no progress left to lose", !g2048_view_has_progress(&g));
}

/* ---- swipes ---------------------------------------------------------------- */

static void test_swipes(void)
{
    enum g2048_dir d = G2048_DIR_COUNT;

    check("the threshold is a tenth of the board", g2048_view_swipe_threshold(528) == 52);
    check("and never under 32 px", g2048_view_swipe_threshold(200) == 32 && g2048_view_swipe_threshold(0) == 32);
    check("right", g2048_view_swipe(80, 0, 52, &d) && d == G2048_RIGHT);
    check("left", g2048_view_swipe(-80, 5, 52, &d) && d == G2048_LEFT);
    check("down: screen y grows downward", g2048_view_swipe(3, 90, 52, &d) && d == G2048_DOWN);
    check("up", g2048_view_swipe(-4, -90, 52, &d) && d == G2048_UP);
    check("exactly the threshold counts", g2048_view_swipe(52, 0, 52, &d) && d == G2048_RIGHT);
    check("one pixel short does not", !g2048_view_swipe(51, 0, 52, &d));
    check("a tap that wobbled is not a swipe", !g2048_view_swipe(6, -4, 52, &d));
    check("a 45 degree smear is not a guess", !g2048_view_swipe(100, 100, 52, &d) &&
                                                  !g2048_view_swipe(-100, 100, 52, &d));
    check("30 degrees off the axis is still a swipe", g2048_view_swipe(100, 57, 52, &d) && d == G2048_RIGHT);
    check("40 degrees off is not", !g2048_view_swipe(100, 84, 52, &d));
    check("the same holds vertically", g2048_view_swipe(57, -100, 52, &d) && d == G2048_UP &&
                                           !g2048_view_swipe(84, -100, 52, &d));
    check("no direction pointer is fine", g2048_view_swipe(0, 70, 52, NULL) == 1);
}

/* ---- layout ---------------------------------------------------------------- */

static int inside(struct g2048_rect r, int w, int h)
{
    return r.x >= 0 && r.y >= 0 && r.w >= 0 && r.h >= 0 && r.x + r.w <= w && r.y + r.h <= h;
}

static int overlap(struct g2048_rect a, struct g2048_rect b)
{
    return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 && a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

static void test_layout(void)
{
    struct g2048_layout l;
    int bad;
    int w;
    int h;

    /* The portrait body the shell gives today: 568 less 40 px of side
     * padding, 1232 less the status bar, header and body padding. */
    g2048_view_layout(528, 1060, &l);
    check("a tall area is stacked", l.arrangement == G2048_STACKED);
    check("the board takes the full width", l.board.w == 528 && l.board.h == 528 && l.board.x == 0);
    check("the HUD is on top", l.hud.x == 0 && l.hud.y == 0 && l.hud.w == 528 && l.hud.h == G2048_HUD_H);
    check("the controls are at the foot",
          l.controls.y + l.controls.h == 1060 && l.controls.w == 528 && l.controls.h == G2048_CONTROLS_H);
    check("the board sits centred between them",
          l.board.y - (l.hud.y + l.hud.h) == l.controls.y - (l.board.y + l.board.h));
    check("with at least the panel gap either side", l.board.y - G2048_HUD_H >= G2048_GAP &&
                                                        l.controls.y - (l.board.y + l.board.h) >= G2048_GAP);

    /* A wide area - no display gives one today, which is the point. */
    g2048_view_layout(1172, 470, &l);
    check("a wide area is side by side", l.arrangement == G2048_SIDE_BY_SIDE);
    check("the board takes the full height on the left", l.board.x == 0 && l.board.h == 470 && l.board.w == 470);
    check("the HUD tops the side column", l.hud.x == 470 + G2048_GAP && l.hud.y == 0 &&
                                              l.hud.x + l.hud.w == 1172);
    check("the controls foot it", l.controls.x == l.hud.x && l.controls.y + l.controls.h == 470);
    g2048_view_layout(700, 600, &l);
    check("a barely wide area keeps the side column's minimum width",
          l.arrangement == G2048_SIDE_BY_SIDE && l.hud.w >= G2048_SIDE_MIN_W && l.board.w == 700 - 240 - 22);

    g2048_view_layout(600, 600, &l);
    check("a square area stacks", l.arrangement == G2048_STACKED &&
                                      l.board.w == 600 - G2048_HUD_H - G2048_CONTROLS_H - 2 * G2048_GAP);

    /* Every size from a sliver to far beyond any panel keeps its rects
     * inside the area, apart, and the board square. */
    bad = 0;
    for (w = 20; w <= 2000; w += 37) {
        for (h = 20; h <= 2000; h += 41) {
            g2048_view_layout(w, h, &l);
            if (!inside(l.hud, w, h) || !inside(l.board, w, h) || !inside(l.controls, w, h)) {
                bad++;
                continue;
            }
            if (w >= 400 && h >= 400) {
                bad += overlap(l.hud, l.board) || overlap(l.board, l.controls) || overlap(l.hud, l.controls);
                bad += l.board.w != l.board.h || l.board.w < 1;
            }
        }
    }
    check("thousands of area sizes: rects inside, apart, board square", bad == 0);
    g2048_view_layout(0, 0, &l);
    check("an empty area does not break anything", inside(l.board, 1, 1));
}

static void test_cells(void)
{
    struct g2048_rect c0 = g2048_view_cell(528, 0);
    struct g2048_rect c15 = g2048_view_cell(528, 15);
    struct g2048_rect c1 = g2048_view_cell(528, 1);
    struct g2048_rect c4 = g2048_view_cell(528, 4);
    int bad = 0;
    int size;

    check("on 528 px the gap is 12", g2048_view_cell_gap(528) == 12);
    check("tiles are 117 px square", c0.w == 117 && c0.h == 117);
    check("the first cell is one gap in", c0.x == 12 && c0.y == 12);
    check("the next column is a tile and a gap across", c1.x == 12 + 129 && c1.y == 12);
    check("the next row is a tile and a gap down", c4.x == 12 && c4.y == 12 + 129);
    check("the last cell ends one gap short of the edge", c15.x + c15.w == 516 && c15.y + c15.h == 516);
    check("an index off the board is an empty rect", g2048_view_cell(528, 16).w == 0 &&
                                                         g2048_view_cell(528, -1).w == 0);
    for (size = 60; size <= 1200; size += 7) {
        int i;
        struct g2048_rect first = g2048_view_cell(size, 0);
        struct g2048_rect last = g2048_view_cell(size, 15);

        for (i = 0; i < G2048_CELLS; i++) {
            struct g2048_rect a = g2048_view_cell(size, i);
            int j;

            bad += !inside(a, size, size) || a.w != first.w || a.h != first.h;
            for (j = i + 1; j < G2048_CELLS; j++) {
                bad += overlap(a, g2048_view_cell(size, j));
            }
        }
        /* Centred: the margins either side differ by at most a pixel. */
        bad += (size - (last.x + last.w)) - first.x > 1 || first.x - (size - (last.x + last.w)) > 1;
    }
    check("every board size: cells inside, equal, apart and centred", bad == 0);
}

/* ---- tile look, labels, motion --------------------------------------------- */

static void test_look(void)
{
    struct pos_theme_tokens t;
    struct g2048_tile_look prev;
    struct g2048_tile_look look;
    int monotone = 1;
    int capped = 1;
    int borders_right = 1;
    int text_right = 1;
    uint8_t exp;
    char label[16];

    pos_theme_resolve(pos_theme_find("ice"), POS_MODE_NORMAL, &t);
    g2048_view_tile_look(1, &t, &prev);
    check("a 2 is a quiet slab with secondary text", prev.fill_base == POS_COLOR_SURFACE_RAISED &&
                                                         prev.fill_mix == 0 &&
                                                         prev.text == POS_COLOR_TEXT_SECONDARY &&
                                                         prev.border_px == 0);
    g2048_view_tile_look(2, &t, &look);
    check("a 4 is the same slab with primary text", look.fill_mix == 0 && look.text == POS_COLOR_TEXT_PRIMARY);
    for (exp = 2; exp <= G2048_EXP_MAX; exp++) {
        uint32_t fill;
        double on_accent;
        double primary;

        g2048_view_tile_look(exp, &t, &look);
        monotone &= look.fill_mix >= prev.fill_mix && look.fill_toward == POS_COLOR_ACCENT_PRIMARY;
        capped &= look.fill_mix <= G2048_MIX_MAX;
        borders_right &= (exp >= G2048_GOAL_EXP) == (look.border_px == 2) &&
                         look.border == POS_COLOR_ACCENT_PRIMARY;
        fill = g2048_view_fill_rgb(&look, &t);
        on_accent = pos_contrast(t.color[POS_COLOR_TEXT_ON_ACCENT], fill);
        primary = pos_contrast(t.color[POS_COLOR_TEXT_PRIMARY], fill);
        text_right &= look.text == (on_accent > primary ? POS_COLOR_TEXT_ON_ACCENT : POS_COLOR_TEXT_PRIMARY);
        prev = look;
    }
    check("the ramp only ever grows toward the accent", monotone);
    check("and never passes half", capped);
    check("the outline starts at the goal and only there", borders_right);
    check("the text is whichever of the two reads better on the fill", text_right);
    g2048_view_tile_look(G2048_GOAL_EXP, &t, &look);
    check("2048 is the ramp's top", look.fill_mix == G2048_MIX_MAX);

    /* The mix is LVGL's arithmetic, exactly. */
    {
        struct pos_theme_tokens fake;
        struct g2048_tile_look l = { POS_COLOR_BG, POS_COLOR_TEXT_PRIMARY, 128, POS_COLOR_TEXT_PRIMARY,
                                     POS_COLOR_ACCENT_PRIMARY, 0 };

        memset(&fake, 0, sizeof(fake));
        fake.color[POS_COLOR_BG] = 0x000000;
        fake.color[POS_COLOR_TEXT_PRIMARY] = 0xFFFFFF;
        check("half of white over black is 0x808080", g2048_view_fill_rgb(&l, &fake) == 0x808080u);
        l.fill_mix = 0;
        check("mix 0 is the base", g2048_view_fill_rgb(&l, &fake) == 0x000000u);
        l.fill_mix = 255;
        check("mix 255 is the colour mixed toward", g2048_view_fill_rgb(&l, &fake) == 0xFFFFFFu);
        fake.color[POS_COLOR_BG] = 0x102030;
        fake.color[POS_COLOR_TEXT_PRIMARY] = 0x305070;
        l.fill_mix = 64;
        /* red (0x30*64 + 0x10*191) * 0x8081 >> 23 = 24 = 0x18, green 44 = 0x2c,
         * blue (0x70*64 + 0x30*191) * 0x8081 >> 23 = 64 = 0x40 */
        check("channels mix independently", g2048_view_fill_rgb(&l, &fake) == 0x182c40u);
    }

    g2048_view_label(1, label, sizeof(label));
    check("exponent 1 reads 2", strcmp(label, "2") == 0);
    g2048_view_label(11, label, sizeof(label));
    check("exponent 11 reads 2048", strcmp(label, "2048") == 0);
    g2048_view_label(17, label, sizeof(label));
    check("exponent 17 reads 131072", strcmp(label, "131072") == 0);
}

static void test_motion(void)
{
    int t;
    int last = -1;
    int monotone = 1;
    int pop_ok = 1;
    int peak = 0;
    struct g2048_rect a = { 0, 0, 100, 100 };
    struct g2048_rect b = { 258, -129, 100, 100 };
    struct g2048_rect r;

    for (t = -10; t <= G2048_MOTION_MS + 20; t++) {
        int q = g2048_view_slide_q8(t);
        int p = g2048_view_pop_px(t);

        monotone &= q >= last && q >= 0 && q <= 256;
        last = q;
        pop_ok &= p >= 0 && p <= G2048_POP_PX && (t > G2048_SLIDE_MS || p == 0);
        if (p > peak) {
            peak = p;
        }
    }
    check("the slide eases from 0 to 256 and never goes back", monotone);
    check("it starts at 0 and has arrived by the end of the slide",
          g2048_view_slide_q8(0) == 0 && g2048_view_slide_q8(G2048_SLIDE_MS) == 256);
    check("eased out: more than half the way at half the time", g2048_view_slide_q8(G2048_SLIDE_MS / 2) > 128);
    check("a merged tile does not swell while sliding, and settles", pop_ok &&
                                                                         g2048_view_pop_px(G2048_MOTION_MS) == 0);
    check("it swells by the full amount at its peak", peak == G2048_POP_PX);
    check("a new tile is hidden during the slide", g2048_view_spawn_q8(G2048_SLIDE_MS - 1) == 0);
    check("then grows from half size", g2048_view_spawn_q8(G2048_SLIDE_MS) == 128);
    check("to full size", g2048_view_spawn_q8(G2048_MOTION_MS) == 256 && g2048_view_spawn_q8(9999) == 256);
    r = g2048_view_lerp(a, b, 0);
    check("lerp at 0 is the start", r.x == 0 && r.y == 0);
    r = g2048_view_lerp(a, b, 256);
    check("lerp at 256 is the end", r.x == 258 && r.y == -129);
    r = g2048_view_lerp(a, b, 128);
    check("lerp at 128 is half way, both signs", r.x == 129 && r.y == -64);
    r = g2048_view_lerp(a, b, 999);
    check("lerp clamps past the end", r.x == 258);
}

int main(void)
{
    test_keys();
    test_panels();
    test_swipes();
    test_layout();
    test_cells();
    test_look();
    test_motion();
    printf("g2048_view_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
