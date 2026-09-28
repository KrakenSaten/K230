/*
 * PG Solitaire view model: the layout in tall and wide areas, fan
 * compression, hit testing, and the interaction model - the cursor, picking
 * up, putting down, cancelling, drawing, sending home, dealing again - for
 * keys and for taps, plus the captions and a long random session of both
 * that must never leave the game or the selection in an impossible state.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "sol_view.h"

#include <stdarg.h>
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

static void check_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

#define S SOL_SPADES
#define H SOL_HEARTS
#define C SOL_CLUBS
#define D SOL_DIAMONDS

static void set_pile(struct sol_game *g, int pile, int down, ...)
{
    struct sol_stack *s = &g->pile[pile];
    va_list ap;

    s->n = 0;
    va_start(ap, down);
    for (;;) {
        int rank = va_arg(ap, int);
        int suit;

        if (rank == 0) {
            break;
        }
        suit = va_arg(ap, int);
        s->card[s->n++] = sol_card(rank, (enum sol_suit)suit);
    }
    va_end(ap);
    s->down = (uint8_t)down;
}

static int inside(struct sol_rect r, int w, int h)
{
    return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.x + r.w <= w && r.y + r.h <= h;
}

static int overlap(struct sol_rect a, struct sol_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static struct sol_hit hit_centre(const struct sol_table *t, const struct sol_game *g, int pile, int index)
{
    struct sol_rect r = index >= 0 ? sol_view_card(t, g, pile, index) : sol_view_slot(t, pile);

    /* The exposed strip of a fanned card is its top; aim just inside it. */
    return sol_view_hit(t, g, r.x + r.w / 2, r.y + 4);
}

/* ---- layout ----------------------------------------------------------------------- */

static void test_layout(void)
{
    struct sol_screen s;
    struct sol_table t;
    struct sol_game g;
    int p;
    int bad = 0;

    sol_view_screen(528, 1060, &s);
    check("the portrait body stacks", s.arrangement == SOL_STACKED);
    check("HUD on top, controls at the foot", s.hud.y == 0 && s.controls.y + s.controls.h == 1060);
    check("the table fills what is between", s.table.y == SOL_HUD_H + SOL_GAP &&
                                                 s.table.y + s.table.h + SOL_GAP == s.controls.y &&
                                                 s.table.w == 528);
    check("no block overlaps another", !overlap(s.hud, s.table) && !overlap(s.table, s.controls));

    sol_view_table(s.table.w, s.table.h, &t);
    check("seven columns across 528 px make 68 px cards", t.card_w == 68 && t.card_h == 95);
    check("the grid is centred", t.left == (528 - (7 * 68 + 6 * SOL_COL_GAP)) / 2);
    check("the tableau starts below the top row", t.tableau_y == SOL_TABLE_PAD + 95 + SOL_ROW_GAP);
    check("a face-up card shows a third of itself", t.fan_up == 31 && t.fan_down == 11);
    check("cards meet the 64 px touch minimum across", t.card_w >= 64);

    sol_view_screen(1172, 470, &s);
    check("a wide body puts the chrome beside the table", s.arrangement == SOL_SIDE_BY_SIDE &&
                                                              s.table.x == 0 && s.hud.x > s.table.w &&
                                                              s.controls.y + s.controls.h == 470);
    check("the controls take the column under the HUD, so a wrapped caption has room",
          s.controls.x == s.hud.x && s.controls.w == s.hud.w && s.controls.y == SOL_HUD_H + SOL_GAP);
    sol_view_table(s.table.w, s.table.h, &t);
    check("a short table binds the card size by its height", t.card_h <= (470 - 16 - 14) * 2 / 7 + 1 &&
                                                                 t.card_w < (s.table.w - 16 - 36) / 7);

    /* The longest column the rules allow - six face down, king to ace face
     * up - ends inside the table at both sizes, and in a sweep of others. */
    memset(&g, 0, sizeof(g));
    set_pile(&g, SOL_T0 + 6, 6, 1, S, 2, S, 3, S, 4, S, 5, S, 6, S, 13, H, 12, S, 11, H, 10, S, 9, H, 8, S, 7, H,
             6, S, 5, H, 4, S, 3, H, 2, S, 1, H, 0);
    {
        static const int sizes[][2] = { { 528, 820 }, { 910, 470 }, { 400, 400 }, { 700, 900 }, { 1172, 600 } };
        size_t k;

        for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            struct sol_rect last;

            sol_view_table(sizes[k][0], sizes[k][1], &t);
            last = sol_view_card(&t, &g, SOL_T0 + 6, g.pile[SOL_T0 + 6].n - 1);
            bad += !inside(last, t.w, t.h);
            for (p = 0; p < SOL_PILES; p++) {
                bad += !inside(sol_view_slot(&t, p), t.w, t.h);
            }
        }
    }
    check("a 19-card column and every slot stay inside the table at every size", bad == 0);
    sol_view_table(528, 820, &t);
    {
        int fd;
        int fu;

        sol_view_fan(&t, &g.pile[SOL_T0 + 6], &fd, &fu);
        check("the portrait table needs no compression for the longest column", fd == 11 && fu == 31);
        sol_view_table(910, 470, &t);
        sol_view_fan(&t, &g.pile[SOL_T0 + 6], &fd, &fu);
        check("a short one compresses face-down edges before faces", fd < t.fan_down && fu <= t.fan_up);
    }

    sol_view_table(528, 820, &t);
    bad = 0;
    for (p = 0; p < SOL_PILES; p++) {
        int q;

        for (q = p + 1; q < SOL_PILES; q++) {
            bad += overlap(sol_view_slot(&t, p), sol_view_slot(&t, q));
        }
    }
    check("no two slots overlap", bad == 0);
    check("pile columns: stock 0, waste 1, foundations 3..6, tableau 0..6",
          sol_view_pile_column(SOL_STOCK) == 0 && sol_view_pile_column(SOL_WASTE) == 1 &&
              sol_view_pile_column(SOL_F0) == 3 && sol_view_pile_column(SOL_F0 + 3) == 6 &&
              sol_view_pile_column(SOL_T0 + 4) == 4 && sol_view_pile_column(SOL_NO_PILE) == -1);
}

static void test_hits(void)
{
    struct sol_table t;
    struct sol_game g;
    struct sol_hit h;
    struct sol_rect r;

    sol_view_table(528, 820, &t);
    sol_deal(&g, 7);
    h = hit_centre(&t, &g, SOL_STOCK, 23);
    check("the stock is hit at its slot", h.pile == SOL_STOCK && h.index == 23);
    h = hit_centre(&t, &g, SOL_F0 + 2, -1);
    check("an empty foundation is hit as a pile", h.pile == SOL_F0 + 2 && h.index == -1);
    r = sol_view_slot(&t, SOL_WASTE);
    h = sol_view_hit(&t, &g, r.x + r.w + SOL_COL_GAP + r.w / 2, r.y + r.h / 2);
    check("the empty third place of the top row is felt", h.pile == SOL_NO_PILE);
    h = sol_view_hit(&t, &g, r.x + r.w + 2, r.y + 10);
    check("the gap between columns is felt", h.pile == SOL_NO_PILE);
    r = sol_view_slot(&t, SOL_T0);
    h = sol_view_hit(&t, &g, r.x + 5, t.top_y + t.card_h + SOL_ROW_GAP / 2);
    check("the gap between the rows is felt", h.pile == SOL_NO_PILE);
    h = hit_centre(&t, &g, SOL_T0 + 6, 3);
    check("a fanned face-down card is hit by its edge", h.pile == SOL_T0 + 6 && h.index == 3);
    r = sol_view_card(&t, &g, SOL_T0 + 6, 6);
    h = sol_view_hit(&t, &g, r.x + r.w / 2, r.y + r.h - 3);
    check("the top card is hit over its whole face", h.pile == SOL_T0 + 6 && h.index == 6);
    h = sol_view_hit(&t, &g, r.x + r.w / 2, r.y + r.h + 200);
    check("below a column is the column itself", h.pile == SOL_T0 + 6 && h.index == -1);
    g.pile[SOL_T0 + 2].n = 0;
    g.pile[SOL_T0 + 2].down = 0;
    r = sol_view_slot(&t, SOL_T0 + 2);
    h = sol_view_hit(&t, &g, r.x + r.w / 2, r.y + r.h / 2);
    check("an empty column is hit as a pile", h.pile == SOL_T0 + 2 && h.index == -1);
    check("left of the grid is felt", sol_view_hit(&t, &g, t.left - 1, t.tableau_y + 10).pile == SOL_NO_PILE);
    check("above it is felt", sol_view_hit(&t, &g, t.left + 5, t.top_y - 1).pile == SOL_NO_PILE);
}

/* ---- keys -------------------------------------------------------------------------- */

/* A small position with known moves:
 *   waste: 7H
 *   T0: 8S          T1: (down 2C) 9D 8C 7D    T2: empty    T3: KD
 *   T4: (down 3S) 6C    T5: AH    T6: 5H
 *   foundations: spades A 2
 *   stock: 4D 3C */
static void position(struct sol_game *g)
{
    memset(g, 0, sizeof(*g));
    set_pile(g, SOL_STOCK, 2, 4, D, 3, C, 0);
    set_pile(g, SOL_WASTE, 0, 7, H, 0);
    set_pile(g, SOL_F0 + S, 0, 1, S, 2, S, 0);
    set_pile(g, SOL_T0, 0, 8, S, 0);
    set_pile(g, SOL_T0 + 1, 1, 2, C, 9, D, 8, C, 7, D, 0);
    set_pile(g, SOL_T0 + 2, 0, 0);
    set_pile(g, SOL_T0 + 3, 0, 13, D, 0);
    set_pile(g, SOL_T0 + 4, 1, 3, S, 6, C, 0);
    set_pile(g, SOL_T0 + 5, 0, 1, H, 0);
    set_pile(g, SOL_T0 + 6, 0, 5, H, 0);
    g->moves = 5;
}

static void test_cursor(void)
{
    struct sol_game g;
    struct sol_ui ui;
    int i;

    position(&g);
    sol_view_reset(&ui, &g);
    check("reset: cursor on the first column's top card, nothing selected",
          ui.cursor_pile == SOL_T0 && ui.cursor_index == 0 && ui.sel_pile == -1 && !ui.keyboard);
    for (i = 0; i < 10; i++) {
        sol_view_key(&ui, &g, SOL_KEY_RIGHT);
    }
    check("Right stops at the last column", ui.cursor_pile == SOL_T0 + 6 && ui.keyboard);
    sol_view_key(&ui, &g, SOL_KEY_LEFT);
    check("Left goes back one", ui.cursor_pile == SOL_T0 + 5);
    sol_view_key(&ui, &g, '2');
    check("2 jumps to the second column's top card", ui.cursor_pile == SOL_T0 + 1 && ui.cursor_index == 3);
    sol_view_key(&ui, &g, SOL_KEY_UP);
    check("Up climbs the face-up run", ui.cursor_index == 2);
    sol_view_key(&ui, &g, SOL_KEY_UP);
    check("to its first face-up card", ui.cursor_index == 1);
    sol_view_key(&ui, &g, SOL_KEY_UP);
    check("then leaves for the top row above that column: the waste", ui.cursor_pile == SOL_WASTE);
    sol_view_key(&ui, &g, SOL_KEY_LEFT);
    sol_view_key(&ui, &g, SOL_KEY_LEFT);
    check("Left stops at the stock", ui.cursor_pile == SOL_STOCK);
    for (i = 0; i < 9; i++) {
        sol_view_key(&ui, &g, SOL_KEY_RIGHT);
    }
    check("Right along the top row stops at the last foundation", ui.cursor_pile == SOL_F0 + 3);
    sol_view_key(&ui, &g, SOL_KEY_LEFT);
    sol_view_key(&ui, &g, SOL_KEY_DOWN);
    check("Down from a foundation lands on the column below it", ui.cursor_pile == SOL_T0 + 5 && ui.cursor_index == 0);
    sol_view_key(&ui, &g, SOL_KEY_DOWN);
    check("Down at a column's last card stays", ui.cursor_pile == SOL_T0 + 5 && ui.cursor_index == 0);
    sol_view_key(&ui, &g, '3');
    check("an empty column's cursor is on the pile", ui.cursor_pile == SOL_T0 + 2 && ui.cursor_index == -1);
    sol_view_key(&ui, &g, SOL_KEY_UP);
    check("Up from an empty column goes to the top row", ui.cursor_pile == SOL_WASTE);
    sol_view_key(&ui, &g, '5');
    sol_view_key(&ui, &g, SOL_KEY_UP);
    sol_view_key(&ui, &g, SOL_KEY_DOWN);
    check("the cursor never rests on a face-down card", ui.cursor_pile == SOL_T0 + 4 && ui.cursor_index == 1);
}

static void test_pick_and_place(void)
{
    struct sol_game g;
    struct sol_ui ui;
    char cap[96];

    position(&g);
    sol_view_reset(&ui, &g);

    /* The waste's 7H onto T0's 8S. */
    sol_view_key(&ui, &g, SOL_KEY_UP);
    sol_view_key(&ui, &g, SOL_KEY_RIGHT);
    check("the cursor reaches the waste", ui.cursor_pile == SOL_WASTE);
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    check("Enter picks up the waste's top card", ui.sel_pile == SOL_WASTE && ui.sel_index == 0);
    sol_view_caption(&ui, &g, cap, sizeof(cap));
    check_str("the caption names it", cap, "7 OF HEARTS SELECTED \xC2\xB7 ESC TO CANCEL");
    sol_view_key(&ui, &g, '1');
    sol_view_key(&ui, &g, ' ');
    check("Space on the 8 of spades puts it down there", g.pile[SOL_T0].n == 2 &&
                                                            sol_top(&g, SOL_T0) == sol_card(7, H) &&
                                                            g.pile[SOL_WASTE].n == 0);
    check("the selection is gone and the cursor followed the card", ui.sel_pile == -1 &&
                                                                        ui.cursor_pile == SOL_T0 &&
                                                                        ui.cursor_index == 1);

    /* An illegal place: T6's 5H onto T3's KD. */
    sol_view_key(&ui, &g, '7');
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    sol_view_key(&ui, &g, '4');
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    check("an illegal place is refused and the selection kept", ui.sel_pile == SOL_T0 + 6 &&
                                                                    g.pile[SOL_T0 + 6].n == 1 &&
                                                                    ui.note == SOL_NOTE_REFUSED);
    sol_view_caption(&ui, &g, cap, sizeof(cap));
    check_str("the caption says why", cap, "BUILD DOWN ONE RANK AT A TIME");
    sol_view_key(&ui, &g, SOL_KEY_ESC);
    check("Esc drops it", ui.sel_pile == -1 && ui.note == SOL_NOTE_NONE);

    /* Colour: T1's 7D cannot go on T0's 7H (rank), and T4's 6C onto T0's 7H
     * is legal and flips the 3S under it. */
    sol_view_key(&ui, &g, '5');
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    sol_view_key(&ui, &g, '1');
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    check("a black 6 goes on a red 7 and uncovers the card below it",
          sol_top(&g, SOL_T0) == sol_card(6, C) && g.pile[SOL_T0 + 4].n == 1 && g.pile[SOL_T0 + 4].down == 0);
    sol_view_key(&ui, &g, '2');
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    sol_view_key(&ui, &g, '5');
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    check("a red 7 on a black 3 is refused on rank", ui.note == SOL_NOTE_REFUSED && ui.why == SOL_ERR_RANK);
    sol_view_key(&ui, &g, SOL_KEY_ESC);
    {
        struct sol_game same;
        struct sol_ui sui;
        char text[96];

        memset(&same, 0, sizeof(same));
        set_pile(&same, SOL_T0, 0, 8, S, 0);
        set_pile(&same, SOL_T0 + 1, 0, 7, C, 0);
        sol_view_reset(&sui, &same);
        sol_view_key(&sui, &same, '2');
        sol_view_key(&sui, &same, SOL_KEY_ENTER);
        sol_view_key(&sui, &same, '1');
        sol_view_key(&sui, &same, SOL_KEY_ENTER);
        sol_view_caption(&sui, &same, text, sizeof(text));
        check_str("a black 7 on a black 8 is refused on colour, in words", text,
                  "SAME COLOUR \xC2\xB7 ALTERNATE RED AND BLACK");
    }

    position(&g);
    sol_view_reset(&ui, &g);
    /* A run: take 8C 7D from T1 (Up once from the top card) onto... T0's 8S
     * cannot take an 8; move instead 9D 8C 7D to an empty column? Only kings.
     * So: move the KD to the empty column, then the run under T1 cannot go
     * anywhere - but a single 7D can go on T0's 8S. */
    sol_view_key(&ui, &g, '2');
    sol_view_key(&ui, &g, SOL_KEY_UP);
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    check("Up then Enter picks up a run of two", ui.sel_pile == SOL_T0 + 1 && ui.sel_index == 2);
    sol_view_caption(&ui, &g, cap, sizeof(cap));
    check_str("the caption counts the rest of the run", cap, "8 OF CLUBS +1 SELECTED \xC2\xB7 ESC TO CANCEL");
    sol_view_key(&ui, &g, '1');
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    check("an 8 onto an 8 is refused", ui.note == SOL_NOTE_REFUSED && ui.why == SOL_ERR_RANK);
    sol_view_key(&ui, &g, '2');
    sol_view_key(&ui, &g, SOL_KEY_DOWN);
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    check("Enter on the selection's own column at another card re-picks from there",
          ui.sel_pile == SOL_T0 + 1 && ui.sel_index == 3);
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    check("Enter on the selected card itself puts it back", ui.sel_pile == -1);
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    sol_view_key(&ui, &g, '1');
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    check("the red 7 goes onto the black 8", sol_top(&g, SOL_T0) == sol_card(7, D) && g.pile[SOL_T0 + 1].n == 3);

    /* The king to the empty column, and a queen refused there. */
    sol_view_key(&ui, &g, '4');
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    sol_view_key(&ui, &g, '3');
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    check("a king goes to the empty column", sol_top(&g, SOL_T0 + 2) == sol_card(13, D) && g.pile[SOL_T0 + 3].n == 0);
    sol_view_key(&ui, &g, '7');
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    sol_view_key(&ui, &g, '4');
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    sol_view_caption(&ui, &g, cap, sizeof(cap));
    check_str("anything else there is refused in words", cap, "ONLY A KING GOES TO AN EMPTY COLUMN");
    sol_view_key(&ui, &g, SOL_KEY_BACKSPACE);
    check("Backspace drops the selection too", ui.sel_pile == -1);

    /* Nothing to pick in an empty column. */
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    check("Enter on an empty column refuses", ui.note == SOL_NOTE_REFUSED && ui.why == SOL_ERR_EMPTY);
}

static void test_draw_home_new(void)
{
    struct sol_game g;
    struct sol_ui ui;
    char cap[96];

    position(&g);
    sol_view_reset(&ui, &g);
    sol_view_key(&ui, &g, 'd');
    check("d draws", g.pile[SOL_STOCK].n == 1 && sol_top(&g, SOL_WASTE) == sol_card(3, C));
    sol_view_key(&ui, &g, SOL_KEY_UP);
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    check("Enter on the stock draws too", g.pile[SOL_STOCK].n == 0 && sol_top(&g, SOL_WASTE) == sol_card(4, D));
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    sol_view_caption(&ui, &g, cap, sizeof(cap));
    check_str("an empty stock turns the waste over, and says so", cap, "WASTE TURNED OVER \xC2\xB7 PASS 1");

    /* f: the AH in T5 goes home; the 6C has nowhere. */
    sol_view_key(&ui, &g, '6');
    sol_view_key(&ui, &g, 'f');
    check("f sends the cursor's card to its foundation", g.pile[SOL_F0 + H].n == 1 && g.pile[SOL_T0 + 5].n == 0);
    sol_view_key(&ui, &g, '5');
    sol_view_key(&ui, &g, 'F');
    check("F with no foundation for it says so", ui.note == SOL_NOTE_NO_HOME && g.pile[SOL_T0 + 4].n == 2);
    sol_view_caption(&ui, &g, cap, sizeof(cap));
    check_str("in words", cap, "NO FOUNDATION TAKES THAT CARD YET");

    /* New game. */
    check("a touched deal has progress", sol_view_has_progress(&g));
    check("n asks first", sol_view_key(&ui, &g, 'n') == SOL_CMD_CHANGED && ui.confirming);
    sol_view_caption(&ui, &g, cap, sizeof(cap));
    check_str("the question", cap, "START A NEW GAME? THIS DEAL WILL BE LOST");
    {
        struct sol_game before = g;

        sol_view_key(&ui, &g, SOL_KEY_RIGHT);
        sol_view_key(&ui, &g, 'd');
        sol_view_key(&ui, &g, 'f');
        check("nothing else happens under the question", memcmp(&before, &g, sizeof(g)) == 0 && ui.confirming);
    }
    sol_view_key(&ui, &g, SOL_KEY_ENTER);
    check("Enter keeps playing", !ui.confirming);
    sol_view_key(&ui, &g, 'N');
    check("n twice deals", sol_view_key(&ui, &g, 'n') == SOL_CMD_NEW_GAME && !ui.confirming);
    sol_deal(&g, 1);
    sol_view_reset(&ui, &g);
    check("n on an untouched deal deals at once", sol_view_key(&ui, &g, 'n') == SOL_CMD_NEW_GAME);
    check("the NEW GAME button on an untouched deal deals at once", sol_view_new_game_button(&ui, &g) == SOL_CMD_NEW_GAME);
    sol_draw(&g);
    check("the button with progress asks", sol_view_new_game_button(&ui, &g) == SOL_CMD_CHANGED && ui.confirming);
    check("KEEP PLAYING closes the question", sol_view_keep_playing_button(&ui) == SOL_CMD_CHANGED && !ui.confirming);
    sol_view_new_game_button(&ui, &g);
    check("NEW GAME in the question deals", sol_view_new_game_button(&ui, &g) == SOL_CMD_NEW_GAME);

    /* Won. */
    memset(&g, 0, sizeof(g));
    g.won = 1;
    g.moves = 123;
    sol_view_reset(&ui, &g);
    sol_view_caption(&ui, &g, cap, sizeof(cap));
    check_str("a won game says how it went", cap, "SOLVED IN 123 MOVES");
    check("arrows do nothing after a win", sol_view_key(&ui, &g, SOL_KEY_LEFT) == SOL_CMD_CHANGED);
    check("Enter deals again", sol_view_key(&ui, &g, SOL_KEY_ENTER) == SOL_CMD_NEW_GAME);
    check("a won game has no progress to lose", !sol_view_has_progress(&g));
}

/* ---- taps ------------------------------------------------------------------------ */

static void test_taps(void)
{
    struct sol_table t;
    struct sol_game g;
    struct sol_ui ui;
    char cap[96];
    struct sol_hit felt = { SOL_NO_PILE, -1 };

    sol_view_table(528, 820, &t);
    position(&g);
    sol_view_reset(&ui, &g);
    sol_view_key(&ui, &g, SOL_KEY_RIGHT);
    check("a key shows the cursor", ui.keyboard);
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_WASTE, 0));
    check("a tap hides it and picks up the card", !ui.keyboard && ui.sel_pile == SOL_WASTE);
    sol_view_caption(&ui, &g, cap, sizeof(cap));
    check_str("the touch caption says how to cancel", cap, "7 OF HEARTS SELECTED \xC2\xB7 TAP FELT TO CANCEL");
    sol_view_tap(&ui, &g, felt);
    check("a tap on the felt drops the selection", ui.sel_pile == -1);
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_WASTE, 0));
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_T0, 0));
    check("a tap on a pile puts the selection down there", sol_top(&g, SOL_T0) == sol_card(7, H));
    {
        struct sol_rect r = sol_view_slot(&t, SOL_T0 + 5);
        struct sol_hit below;

        sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_T0 + 6, 0));
        below = sol_view_hit(&t, &g, r.x + r.w / 2, r.y + 400);
        sol_view_tap(&ui, &g, below);
        check("anywhere down a column places onto it (refused here on rank)",
              ui.note == SOL_NOTE_REFUSED && ui.sel_pile == SOL_T0 + 6);
    }
    sol_view_tap(&ui, &g, felt);
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_T0 + 5, 0));
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_T0 + 5, 0));
    check("tapping the selected ace again sends it home", g.pile[SOL_F0 + H].n == 1 && ui.sel_pile == -1);
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_T0 + 4, 1));
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_T0 + 4, 1));
    check("tapping a selected card with no home drops it", ui.sel_pile == -1 && g.pile[SOL_T0 + 4].n == 2);
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_T0 + 1, 3));
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_T0 + 1, 1));
    check("tapping further up the same run re-picks from there", ui.sel_pile == SOL_T0 + 1 && ui.sel_index == 1);
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_T0 + 1, 0));
    check("tapping its face-down card drops the selection", ui.sel_pile == -1);
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_T0 + 1, 0));
    check("tapping a face-down card with nothing selected refuses", ui.note == SOL_NOTE_REFUSED &&
                                                                         ui.why == SOL_ERR_FACE_DOWN);
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_T0 + 2, -1));
    check("tapping an empty column with nothing selected refuses", ui.why == SOL_ERR_EMPTY);
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_STOCK, 1));
    check("tapping the stock draws", g.pile[SOL_STOCK].n == 1);
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_F0 + S, 1));
    check("a foundation's top card can be picked up", ui.sel_pile == SOL_F0 + S && ui.sel_index == 1);
    sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_T0 + 6, 0));
    check("and is refused on a card it does not build on", ui.note == SOL_NOTE_REFUSED &&
                                                               ui.why == SOL_ERR_RANK &&
                                                               g.pile[SOL_F0 + S].n == 2);
    sol_view_tap(&ui, &g, felt);
    sol_view_new_game_button(&ui, &g);
    {
        struct sol_game before = g;

        sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_STOCK, 0));
        sol_view_tap(&ui, &g, hit_centre(&t, &g, SOL_T0, 0));
        check("taps on the table do nothing under the question", memcmp(&before, &g, sizeof(g)) == 0);
    }
}

/* ---- a long random session ---------------------------------------------------------- */

static void test_random_session(void)
{
    static const uint32_t keyset[] = { SOL_KEY_UP, SOL_KEY_DOWN, SOL_KEY_LEFT, SOL_KEY_RIGHT, SOL_KEY_ENTER, ' ',
                                       SOL_KEY_ESC, SOL_KEY_BACKSPACE, 'd', 'f', '1', '4', '7', 'q', 0xE6 };
    struct sol_table t;
    struct sol_game g;
    struct sol_ui ui;
    struct sol_rng rng;
    int broken = 0;
    int moved = 0;
    int step;
    char cap[96];

    sol_view_table(528, 820, &t);
    sol_rng_seed(&rng, 99);
    sol_deal(&g, 3);
    sol_view_reset(&ui, &g);
    for (step = 0; step < 200000; step++) {
        uint32_t before = g.moves;
        enum sol_cmd cmd;

        if (sol_rng_below(&rng, 2)) {
            cmd = sol_view_key(&ui, &g, keyset[sol_rng_below(&rng, sizeof(keyset) / sizeof(keyset[0]))]);
        } else {
            cmd = sol_view_tap(&ui, &g, sol_view_hit(&t, &g, (int)sol_rng_below(&rng, 528),
                                                     (int)sol_rng_below(&rng, 820)));
        }
        if (cmd == SOL_CMD_NEW_GAME || g.won) {
            sol_deal(&g, (uint32_t)step + 1u);
            sol_view_reset(&ui, &g);
        }
        moved += g.moves != before;
        if (!sol_game_valid(&g)) {
            broken++;
            printf("FAIL step %d: the game became invalid\n", step);
            break;
        }
        if (ui.sel_pile >= 0 && sol_can_pick(&g, ui.sel_pile, ui.sel_index) != SOL_OK) {
            broken++;
            printf("FAIL step %d: the selection points at something that cannot be picked\n", step);
            break;
        }
        if (ui.cursor_pile < 0 || ui.cursor_pile >= SOL_PILES) {
            broken++;
            break;
        }
        sol_view_caption(&ui, &g, cap, sizeof(cap));
        if (strlen(cap) > 56) {
            broken++;
            printf("FAIL step %d: caption too long for the line: %s\n", step, cap);
            break;
        }
    }
    check("200 000 random keys and taps: the game stays valid, the selection pickable, the caption short",
          broken == 0);
    check("and the session actually plays", moved > 1000);
}

int main(void)
{
    test_layout();
    test_hits();
    test_cursor();
    test_pick_and_place();
    test_draw_home_new();
    test_taps();
    test_random_session();
    printf("sol_view_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
