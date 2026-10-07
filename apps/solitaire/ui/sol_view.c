/*
 * PG Solitaire view model. See sol_view.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sol_view.h"

#include <stdio.h>
#include <string.h>

static int max_i(int a, int b)
{
    return a > b ? a : b;
}

static int min_i(int a, int b)
{
    return a < b ? a : b;
}

static struct sol_rect rect(int x, int y, int w, int h)
{
    struct sol_rect r = { x, y, w, h };

    return r;
}

/* ---- blocks ---------------------------------------------------------------- */

void sol_view_screen(int w, int h, struct sol_screen *out)
{
    if (!out) {
        return;
    }
    w = max_i(w, 1);
    h = max_i(h, 1);
    if (w > h) {
        int table_w = max_i(w - SOL_SIDE_W - SOL_GAP, 1);
        int side_x = table_w + SOL_GAP;

        out->arrangement = SOL_SIDE_BY_SIDE;
        out->table = rect(0, 0, table_w, h);
        out->hud = rect(side_x, 0, max_i(w - side_x, 0), min_i(SOL_HUD_H, h));
        /* The rest of the column under the HUD, bottom-aligned: a caption
         * too long for the narrow column wraps upwards into it rather than
         * over the HUD or off the top of a 104 px block. */
        out->controls = rect(side_x, min_i(SOL_HUD_H + SOL_GAP, h), max_i(w - side_x, 0),
                             max_i(h - SOL_HUD_H - SOL_GAP, 0));
    } else {
        int table_h = max_i(h - SOL_HUD_H - SOL_CONTROLS_H - 2 * SOL_GAP, 1);

        out->arrangement = SOL_STACKED;
        out->hud = rect(0, 0, w, min_i(SOL_HUD_H, h));
        out->table = rect(0, min_i(SOL_HUD_H + SOL_GAP, h - 1), w, min_i(table_h, max_i(h - SOL_HUD_H - SOL_GAP, 1)));
        out->controls = rect(0, max_i(h - SOL_CONTROLS_H, 0), w, min_i(SOL_CONTROLS_H, h));
    }
}

/* ---- the table ----------------------------------------------------------------- */

void sol_view_table(int w, int h, struct sol_table *out)
{
    int by_width;
    int by_height;
    int grid;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->w = max_i(w, 1);
    out->h = max_i(h, 1);
    by_width = (out->w - 2 * SOL_TABLE_PAD - (SOL_COLUMNS - 1) * SOL_COL_GAP) / SOL_COLUMNS;
    /* A top row and a tableau card, and one and a half cards of fan: three
     * and a half card heights. */
    by_height = SOL_CARD_W_OF_H((out->h - 2 * SOL_TABLE_PAD - SOL_ROW_GAP) * 2 / 7);
    out->card_w = max_i(min_i(by_width, by_height), 8);
    out->card_h = SOL_CARD_H_OF_W(out->card_w);
    grid = SOL_COLUMNS * out->card_w + (SOL_COLUMNS - 1) * SOL_COL_GAP;
    out->left = max_i((out->w - grid) / 2, 0);
    out->top_y = SOL_TABLE_PAD;
    out->tableau_y = SOL_TABLE_PAD + out->card_h + SOL_ROW_GAP;
    /* A face-down card shows an edge; a face-up one shows its rank and suit,
     * which the card draws in its top third. */
    out->fan_down = max_i(out->card_h / 8, 3);
    out->fan_up = max_i(out->card_h / 3, 8);
}

int sol_view_pile_column(int pile)
{
    if (pile == SOL_STOCK) {
        return 0;
    }
    if (pile == SOL_WASTE) {
        return 1;
    }
    if (sol_is_foundation(pile)) {
        return 3 + pile - SOL_F0;
    }
    if (sol_is_column(pile)) {
        return pile - SOL_T0;
    }
    return -1;
}

static int column_x(const struct sol_table *t, int col)
{
    return t->left + col * (t->card_w + SOL_COL_GAP);
}

struct sol_rect sol_view_slot(const struct sol_table *t, int pile)
{
    int col = sol_view_pile_column(pile);

    if (!t || col < 0) {
        return rect(0, 0, 0, 0);
    }
    return rect(column_x(t, col), sol_is_column(pile) ? t->tableau_y : t->top_y, t->card_w, t->card_h);
}

void sol_view_fan(const struct sol_table *t, const struct sol_stack *column, int *down_px, int *up_px)
{
    int fd;
    int fu;
    int d;
    int u;
    int avail;

    if (!t || !column || !down_px || !up_px) {
        return;
    }
    fd = t->fan_down;
    fu = t->fan_up;
    d = column->down;
    u = column->n - column->down;
    avail = t->h - SOL_TABLE_PAD - t->tableau_y - t->card_h;
    if (d * fd + max_i(u - 1, 0) * fu > avail) {
        int min_fd = max_i(fd / 3, 2);
        int min_fu = max_i(fu / 3, 4);

        /* Face-down edges give way first: they carry no information. */
        if (d > 0) {
            fd = max_i((avail - max_i(u - 1, 0) * fu) / d, min_fd);
            fd = min_i(fd, t->fan_down);
        }
        if (u > 1 && d * fd + (u - 1) * fu > avail) {
            fu = max_i((avail - d * fd) / (u - 1), min_fu);
        }
    }
    *down_px = fd;
    *up_px = fu;
}

struct sol_rect sol_view_card(const struct sol_table *t, const struct sol_game *g, int pile, int index)
{
    struct sol_rect r;

    if (!t || !g || sol_view_pile_column(pile) < 0 || index < 0 || index >= g->pile[pile].n) {
        return rect(0, 0, 0, 0);
    }
    r = sol_view_slot(t, pile);
    if (sol_is_column(pile)) {
        const struct sol_stack *s = &g->pile[pile];
        int fd;
        int fu;

        sol_view_fan(t, s, &fd, &fu);
        r.y += min_i(index, s->down) * fd + max_i(index - s->down, 0) * fu;
    }
    return r;
}

struct sol_hit sol_view_hit(const struct sol_table *t, const struct sol_game *g, int x, int y)
{
    struct sol_hit hit = { SOL_NO_PILE, -1 };
    int step;
    int col;

    if (!t || !g || x < t->left || y < t->top_y) {
        return hit;
    }
    step = t->card_w + SOL_COL_GAP;
    col = (x - t->left) / step;
    if (col >= SOL_COLUMNS || (x - t->left) % step >= t->card_w) {
        return hit;
    }
    if (y < t->top_y + t->card_h) {
        static const int top_row[SOL_COLUMNS] = { SOL_STOCK, SOL_WASTE, SOL_NO_PILE, SOL_F0, SOL_F0 + 1,
                                                  SOL_F0 + 2, SOL_F0 + 3 };

        hit.pile = top_row[col];
        if (hit.pile != SOL_NO_PILE) {
            hit.index = g->pile[hit.pile].n - 1;
        }
        return hit;
    }
    if (y < t->tableau_y || y >= t->h) {
        return hit;
    }
    hit.pile = SOL_T0 + col;
    {
        int i;

        for (i = g->pile[hit.pile].n - 1; i >= 0; i--) {
            struct sol_rect r = sol_view_card(t, g, hit.pile, i);

            if (y >= r.y && y < r.y + r.h) {
                hit.index = i;
                break;
            }
        }
    }
    return hit;
}

/* ---- cursor and selection --------------------------------------------------- */

static const int top_row_piles[] = { SOL_STOCK, SOL_WASTE, SOL_F0, SOL_F0 + 1, SOL_F0 + 2, SOL_F0 + 3 };
#define TOP_ROW_COUNT ((int)(sizeof(top_row_piles) / sizeof(top_row_piles[0])))

static void clear_note(struct sol_ui *ui)
{
    ui->note = SOL_NOTE_NONE;
    ui->why = SOL_OK;
    ui->why_to = -1;
}

static void refuse(struct sol_ui *ui, enum sol_result why, int to)
{
    ui->note = SOL_NOTE_REFUSED;
    ui->why = (uint8_t)why;
    ui->why_to = (int8_t)to;
}

static void deselect(struct sol_ui *ui)
{
    ui->sel_pile = -1;
    ui->sel_index = -1;
}

static void cursor_to_column(struct sol_ui *ui, const struct sol_game *g, int col)
{
    int pile = SOL_T0 + min_i(max_i(col, 0), SOL_COLUMNS - 1);

    ui->cursor_pile = (int8_t)pile;
    ui->cursor_index = (int8_t)(g->pile[pile].n - 1);
}

/* Keep the cursor and the selection pointing at things that exist after the
 * game changed under them. */
static void normalise(struct sol_ui *ui, const struct sol_game *g)
{
    if (ui->cursor_pile < 0 || ui->cursor_pile >= SOL_PILES) {
        cursor_to_column(ui, g, 0);
    }
    if (sol_is_column(ui->cursor_pile)) {
        const struct sol_stack *s = &g->pile[ui->cursor_pile];

        if (s->n == 0) {
            ui->cursor_index = -1;
        } else if (ui->cursor_index >= s->n || ui->cursor_index < s->down) {
            ui->cursor_index = (int8_t)(s->n - 1);
        }
    } else {
        ui->cursor_index = (int8_t)(g->pile[ui->cursor_pile].n - 1);
    }
    if (ui->sel_pile >= 0 && sol_can_pick(g, ui->sel_pile, ui->sel_index) != SOL_OK) {
        deselect(ui);
    }
}

void sol_view_reset(struct sol_ui *ui, const struct sol_game *g)
{
    if (!ui || !g) {
        return;
    }
    memset(ui, 0, sizeof(*ui));
    deselect(ui);
    clear_note(ui);
    cursor_to_column(ui, g, 0);
}

int sol_view_has_progress(const struct sol_game *g)
{
    return g && g->moves > 0 && !g->won;
}

/* The index a pick at the cursor means: the cursor's card in a column, the
 * top card anywhere else. */
static int cursor_card(const struct sol_ui *ui, const struct sol_game *g)
{
    return sol_is_column(ui->cursor_pile) ? ui->cursor_index : g->pile[ui->cursor_pile].n - 1;
}

static void pick(struct sol_ui *ui, const struct sol_game *g, int pile, int index)
{
    enum sol_result r = index < 0 ? SOL_ERR_EMPTY : sol_can_pick(g, pile, index);

    if (r == SOL_OK) {
        ui->sel_pile = (int8_t)pile;
        ui->sel_index = (int8_t)index;
        clear_note(ui);
    } else {
        refuse(ui, r, -1);
    }
}

static void place(struct sol_ui *ui, struct sol_game *g, int to)
{
    enum sol_result r = sol_move(g, ui->sel_pile, ui->sel_index, to, NULL);

    if (r == SOL_OK) {
        deselect(ui);
        clear_note(ui);
        ui->cursor_pile = (int8_t)to;
        ui->cursor_index = (int8_t)(g->pile[to].n - 1);
    } else {
        refuse(ui, r, to);
    }
}

static void draw(struct sol_ui *ui, struct sol_game *g)
{
    deselect(ui);
    switch (sol_draw(g)) {
    case SOL_DREW:
        clear_note(ui);
        break;
    case SOL_TURNED:
        ui->note = SOL_NOTE_TURNED;
        break;
    case SOL_NOTHING:
    default:
        ui->note = SOL_NOTE_EMPTY;
        break;
    }
}

static void send_home(struct sol_ui *ui, struct sol_game *g)
{
    int pile = ui->sel_pile >= 0 ? ui->sel_pile : ui->cursor_pile;
    int index = ui->sel_pile >= 0 ? ui->sel_index : cursor_card(ui, g);
    int to = sol_foundation_target(g, pile, index);

    if (to < 0) {
        ui->note = SOL_NOTE_NO_HOME;
        return;
    }
    sol_move(g, pile, index, to, NULL);
    deselect(ui);
    clear_note(ui);
}

static void act(struct sol_ui *ui, struct sol_game *g)
{
    if (ui->cursor_pile == SOL_STOCK) {
        draw(ui, g);
        return;
    }
    if (ui->sel_pile < 0) {
        pick(ui, g, ui->cursor_pile, cursor_card(ui, g));
        return;
    }
    if (ui->cursor_pile == ui->sel_pile) {
        int at = cursor_card(ui, g);

        if (at != ui->sel_index && at >= 0 && sol_can_pick(g, ui->cursor_pile, at) == SOL_OK) {
            ui->sel_index = (int8_t)at;
        } else {
            deselect(ui);
        }
        clear_note(ui);
        return;
    }
    place(ui, g, ui->cursor_pile);
}

static void move_horizontal(struct sol_ui *ui, const struct sol_game *g, int delta)
{
    if (sol_is_column(ui->cursor_pile)) {
        cursor_to_column(ui, g, ui->cursor_pile - SOL_T0 + delta);
        return;
    }
    {
        int p;

        for (p = 0; p < TOP_ROW_COUNT; p++) {
            if (top_row_piles[p] == ui->cursor_pile) {
                break;
            }
        }
        p = min_i(max_i(p + delta, 0), TOP_ROW_COUNT - 1);
        ui->cursor_pile = (int8_t)top_row_piles[p];
        ui->cursor_index = (int8_t)(g->pile[ui->cursor_pile].n - 1);
    }
}

static void move_vertical(struct sol_ui *ui, const struct sol_game *g, int down)
{
    if (sol_is_column(ui->cursor_pile)) {
        const struct sol_stack *s = &g->pile[ui->cursor_pile];
        int col = ui->cursor_pile - SOL_T0;

        if (down) {
            if (ui->cursor_index >= 0 && ui->cursor_index < s->n - 1) {
                ui->cursor_index++;
            }
            return;
        }
        if (s->n > 0 && ui->cursor_index > s->down) {
            ui->cursor_index--;
            return;
        }
        ui->cursor_pile = (int8_t)(col == 0 ? SOL_STOCK : col <= 2 ? SOL_WASTE : SOL_F0 + col - 3);
        ui->cursor_index = (int8_t)(g->pile[ui->cursor_pile].n - 1);
        return;
    }
    if (down) {
        cursor_to_column(ui, g, sol_view_pile_column(ui->cursor_pile));
    }
}

enum sol_cmd sol_view_key(struct sol_ui *ui, struct sol_game *g, uint32_t key)
{
    int is_new = key == 'n' || key == 'N';

    if (!ui || !g) {
        return SOL_CMD_NONE;
    }
    normalise(ui, g);
    ui->keyboard = 1;
    if (g->won) {
        return (is_new || key == SOL_KEY_ENTER) ? SOL_CMD_NEW_GAME : SOL_CMD_CHANGED;
    }
    if (ui->confirming) {
        if (is_new) {
            ui->confirming = 0;
            return SOL_CMD_NEW_GAME;
        }
        if (key == SOL_KEY_ENTER || key == SOL_KEY_ESC || key == SOL_KEY_BACKSPACE) {
            ui->confirming = 0;
        }
        return SOL_CMD_CHANGED;
    }
    switch (key) {
    case SOL_KEY_LEFT:
        move_horizontal(ui, g, -1);
        break;
    case SOL_KEY_RIGHT:
        move_horizontal(ui, g, 1);
        break;
    case SOL_KEY_UP:
        move_vertical(ui, g, 0);
        break;
    case SOL_KEY_DOWN:
        move_vertical(ui, g, 1);
        break;
    case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        cursor_to_column(ui, g, (int)(key - '1'));
        break;
    case SOL_KEY_ENTER:
    case ' ':
        act(ui, g);
        break;
    case 'd':
    case 'D':
        draw(ui, g);
        break;
    case 'f':
    case 'F':
        send_home(ui, g);
        break;
    case SOL_KEY_ESC:
    case SOL_KEY_BACKSPACE:
        deselect(ui);
        clear_note(ui);
        break;
    case 'n':
    case 'N':
        if (!sol_view_has_progress(g)) {
            return SOL_CMD_NEW_GAME;
        }
        ui->confirming = 1;
        deselect(ui);
        break;
    default:
        return SOL_CMD_CHANGED; /* the cursor may have just appeared */
    }
    normalise(ui, g);
    return SOL_CMD_CHANGED;
}

enum sol_cmd sol_view_tap(struct sol_ui *ui, struct sol_game *g, struct sol_hit hit)
{
    if (!ui || !g) {
        return SOL_CMD_NONE;
    }
    normalise(ui, g);
    ui->keyboard = 0;
    if (ui->confirming || g->won) {
        return SOL_CMD_CHANGED;
    }
    if (hit.pile < 0 || hit.pile >= SOL_PILES) {
        deselect(ui);
        clear_note(ui);
        return SOL_CMD_CHANGED;
    }
    ui->cursor_pile = (int8_t)hit.pile;
    ui->cursor_index = (int8_t)hit.index;
    if (hit.pile == SOL_STOCK) {
        draw(ui, g);
    } else if (ui->sel_pile < 0) {
        if (hit.index >= 0) {
            pick(ui, g, hit.pile, hit.index);
        } else if (g->pile[hit.pile].n == 0) {
            refuse(ui, SOL_ERR_EMPTY, -1);
        }
    } else if (hit.pile == ui->sel_pile) {
        if (hit.index == ui->sel_index && sol_foundation_target(g, hit.pile, hit.index) >= 0) {
            send_home(ui, g);
        } else if (hit.index >= 0 && hit.index != ui->sel_index &&
                   sol_can_pick(g, hit.pile, hit.index) == SOL_OK) {
            ui->sel_index = (int8_t)hit.index;
            clear_note(ui);
        } else {
            deselect(ui);
            clear_note(ui);
        }
    } else {
        place(ui, g, hit.pile);
    }
    normalise(ui, g);
    return SOL_CMD_CHANGED;
}

enum sol_cmd sol_view_new_game_button(struct sol_ui *ui, const struct sol_game *g)
{
    if (!ui || !g) {
        return SOL_CMD_NONE;
    }
    if (ui->confirming || !sol_view_has_progress(g)) {
        ui->confirming = 0;
        return SOL_CMD_NEW_GAME;
    }
    ui->confirming = 1;
    deselect(ui);
    return SOL_CMD_CHANGED;
}

enum sol_cmd sol_view_keep_playing_button(struct sol_ui *ui)
{
    if (!ui || !ui->confirming) {
        return SOL_CMD_NONE;
    }
    ui->confirming = 0;
    return SOL_CMD_CHANGED;
}

/* ---- words ---------------------------------------------------------------------- */

const char *sol_view_reason(enum sol_result why, int to)
{
    switch (why) {
    case SOL_ERR_EMPTY:
        return "NOTHING THERE TO TAKE";
    case SOL_ERR_FACE_DOWN:
        return "THAT CARD IS FACE DOWN";
    case SOL_ERR_NOT_TOP:
        return "ONLY THE TOP CARD CAN MOVE";
    case SOL_ERR_NOT_A_RUN:
        return "THOSE CARDS ARE NOT A RUN";
    case SOL_ERR_SAME_PILE:
        return "ALREADY THERE";
    case SOL_ERR_ONE_CARD:
        return "FOUNDATIONS TAKE ONE CARD AT A TIME";
    case SOL_ERR_SUIT:
        return "THAT FOUNDATION IS FOR ANOTHER SUIT";
    case SOL_ERR_RANK:
        return sol_is_foundation(to) ? "FOUNDATIONS BUILD UP FROM THE ACE"
                                     : "BUILD DOWN ONE RANK AT A TIME";
    case SOL_ERR_COLOUR:
        return "SAME COLOUR \xC2\xB7 ALTERNATE RED AND BLACK";
    case SOL_ERR_KING_ONLY:
        return "ONLY A KING GOES TO AN EMPTY COLUMN";
    case SOL_ERR_STOCK:
        return to == SOL_STOCK ? "CARDS DO NOT GO BACK TO THE STOCK" : "TAP THE STOCK TO DRAW";
    case SOL_ERR_WON:
        return "THE GAME IS WON";
    case SOL_ERR_BAD_PILE:
    default:
        return "CARDS CANNOT GO THERE";
    }
}

void sol_view_caption(const struct sol_ui *ui, const struct sol_game *g, char *out, size_t len)
{
    if (!out || len == 0) {
        return;
    }
    if (!ui || !g) {
        out[0] = '\0';
        return;
    }
    if (g->won) {
        snprintf(out, len, "SOLVED IN %u MOVES", (unsigned)g->moves);
    } else if (ui->confirming) {
        snprintf(out, len, "START A NEW GAME? THIS DEAL WILL BE LOST");
    } else if (ui->note == SOL_NOTE_REFUSED) {
        snprintf(out, len, "%s", sol_view_reason((enum sol_result)ui->why, ui->why_to));
    } else if (ui->note == SOL_NOTE_NO_HOME) {
        snprintf(out, len, "NO FOUNDATION TAKES THAT CARD YET");
    } else if (ui->note == SOL_NOTE_TURNED) {
        snprintf(out, len, "WASTE TURNED OVER \xC2\xB7 PASS %u", (unsigned)g->passes);
    } else if (ui->note == SOL_NOTE_EMPTY) {
        snprintf(out, len, "STOCK AND WASTE ARE EMPTY");
    } else if (ui->sel_pile >= 0 && ui->sel_index >= 0 && ui->sel_index < g->pile[ui->sel_pile].n) {
        sol_card_t c = g->pile[ui->sel_pile].card[ui->sel_index];
        int more = g->pile[ui->sel_pile].n - 1 - ui->sel_index;
        char extra[8] = "";

        if (more > 0) {
            snprintf(extra, sizeof(extra), " +%d", more);
        }
        snprintf(out, len, "%s OF %s%s SELECTED \xC2\xB7 %s", sol_rank_text(sol_card_rank(c)),
                 sol_suit_name(sol_card_suit(c)), extra, ui->keyboard ? "ESC TO CANCEL" : "TAP FELT TO CANCEL");
    } else if (ui->keyboard) {
        snprintf(out, len, "ARROWS \xC2\xB7 ENTER PICK AND PLACE \xC2\xB7 D DRAW \xC2\xB7 F HOME");
    } else {
        snprintf(out, len, "TAP A CARD, THEN WHERE IT GOES");
    }
}
