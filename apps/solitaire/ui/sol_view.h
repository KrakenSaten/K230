/*
 * PG Solitaire view model: where the piles and cards go in whatever area the
 * app is given, which card a finger is on, and what a key or a tap does to
 * the cursor, the selection and the game.
 *
 * Pure C, like the rules: no LVGL, no I/O, no clock (tests/sol_lint.sh). It
 * names LVGL's key numbers as numbers and sol_app.c checks at compile time
 * that they agree, so the whole interaction model - keyboard first, touch
 * beside it - is tested on the host (tests/sol_view_test.c).
 *
 * ORIENTATION. Nothing here assumes the portrait panel. The table layout
 * sizes cards from both the width (seven columns) and the height (a top row
 * and a tableau with room to fan), so a wide area gets cards bound by its
 * height and a tall one cards bound by its width; tableau fans compress when
 * a column is longer than the table. The chrome around the table is stacked
 * in a tall area and side by side in a wide one. The shell gives a portrait
 * or a landscape body (DS §21); both are covered by the view test and the
 * app test.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGSOL_VIEW_H
#define PGSOL_VIEW_H

#include "sol_rules.h"

#include <stddef.h>
#include <stdint.h>

/* LVGL's LV_KEY_UP, _DOWN, _RIGHT, _LEFT, _ENTER, _ESC, _BACKSPACE. */
#define SOL_KEY_UP 17u
#define SOL_KEY_DOWN 18u
#define SOL_KEY_RIGHT 19u
#define SOL_KEY_LEFT 20u
#define SOL_KEY_ENTER 10u
#define SOL_KEY_ESC 27u
#define SOL_KEY_BACKSPACE 8u

struct sol_rect {
    int x;
    int y;
    int w;
    int h;
};

/* ---- the screen's blocks --------------------------------------------------- */

#define SOL_HUD_H 92
#define SOL_CONTROLS_H 104
#define SOL_GAP 22
#define SOL_SIDE_W 240

enum sol_arrangement {
    SOL_STACKED = 0,   /* HUD, table, controls from the top */
    SOL_SIDE_BY_SIDE   /* table left; HUD and controls on the right */
};

struct sol_screen {
    enum sol_arrangement arrangement;
    struct sol_rect hud;
    struct sol_rect table;
    struct sol_rect controls;
};

void sol_view_screen(int w, int h, struct sol_screen *out);

/* ---- the table ----------------------------------------------------------------- */

/* Card proportions: 5 wide by 7 tall. */
#define SOL_CARD_W_OF_H(h) ((h) * 5 / 7)
#define SOL_CARD_H_OF_W(w) ((w) * 7 / 5)
/* Space around the cards inside the table, between columns, and between the
 * top row and the tableau. */
#define SOL_TABLE_PAD 8
#define SOL_COL_GAP 6
#define SOL_ROW_GAP 14

struct sol_table {
    int w;
    int h;
    int card_w;
    int card_h;
    int left;          /* x of column 0, the grid centred in the table */
    int top_y;         /* y of the stock, waste and foundations */
    int tableau_y;     /* y of the first tableau card */
    int fan_down;      /* uncompressed offset below a face-down card */
    int fan_up;        /* uncompressed offset below a face-up card */
};

/* Size the cards for a table of w x h. Cards are as large as seven columns
 * across and a top row plus a tableau of about two and a half cards down
 * allow. */
void sol_view_table(int w, int h, struct sol_table *out);

/* The column (0..6) a pile is drawn in, or -1. The stock is column 0, the
 * waste column 1, the foundations columns 3..6, the tableau 0..6. */
int sol_view_pile_column(int pile);
/* Where an empty pile's slot is. */
struct sol_rect sol_view_slot(const struct sol_table *t, int pile);
/* Offsets for one tableau column as drawn: the uncompressed fans shrink,
 * face-down first, until the column's last card ends inside the table. */
void sol_view_fan(const struct sol_table *t, const struct sol_stack *column, int *down_px, int *up_px);
/* Where card `index` of `pile` is drawn. Stock, waste and foundations draw
 * every card in the slot, the top one last. */
struct sol_rect sol_view_card(const struct sol_table *t, const struct sol_game *g, int pile, int index);

struct sol_hit {
    int pile;  /* SOL_NO_PILE for the felt */
    int index; /* the card under the point, or -1 for the pile itself */
};

/* What a point in the table is on. A tableau column's target is its whole
 * width from the tableau row down, so a finger placing a card never has to
 * hit a strip of card; the card under the point, if any, is the topmost one
 * drawn there. */
struct sol_hit sol_view_hit(const struct sol_table *t, const struct sol_game *g, int x, int y);

/* ---- cursor and selection --------------------------------------------------- */

enum sol_note {
    SOL_NOTE_NONE = 0,
    SOL_NOTE_REFUSED,   /* `why` says why */
    SOL_NOTE_NO_HOME,   /* F with no foundation to take the card */
    SOL_NOTE_TURNED,    /* the waste went back to the stock */
    SOL_NOTE_EMPTY      /* stock and waste both empty */
};

struct sol_ui {
    int8_t cursor_pile;
    int8_t cursor_index; /* tableau only: the card the cursor is on, -1 for an empty column */
    int8_t sel_pile;     /* -1: nothing selected */
    int8_t sel_index;
    uint8_t keyboard;    /* the cursor is shown: a key was used since the last touch */
    uint8_t confirming;  /* asking whether to deal again */
    uint8_t note;        /* enum sol_note */
    uint8_t why;         /* enum sol_result for SOL_NOTE_REFUSED */
    int8_t why_to;       /* the pile the refused move was aimed at, or -1 */
};

enum sol_cmd {
    SOL_CMD_NONE = 0,     /* nothing changed */
    SOL_CMD_CHANGED,      /* redraw */
    SOL_CMD_NEW_GAME      /* the app deals a new game, then calls sol_view_reset */
};

/* Cursor on the first tableau column's top card, nothing selected. */
void sol_view_reset(struct sol_ui *ui, const struct sol_game *g);

/* Has the deal been touched: any draw or move. Dealing again then asks. */
int sol_view_has_progress(const struct sol_game *g);

/* A key from the stream. Keyboard map, whatever produced the key:
 *
 *   Left Right         cursor along its row (top row: stock, waste, four
 *                      foundations; tableau: seven columns)
 *   Up Down            in a column, up and down its face-up cards, which
 *                      chooses how much of a run to take; Up past the first
 *                      face-up card goes to the top row, Down from the top
 *                      row to the column below
 *   1 .. 7             cursor to that column's top card
 *   Enter Space        on the stock: draw. Otherwise pick up the card (and
 *                      the run below it) at the cursor, or put the selection
 *                      down on the cursor's pile; on the selection's own pile
 *                      it puts it back
 *   d D                draw
 *   f F                send the selected card, or the cursor's card, to its
 *                      foundation
 *   Esc Backspace      drop the selection
 *   n N                new game (asks first when the deal has progress:
 *                      n again deals; Enter, Esc or Backspace keeps playing)
 *   When won           Enter or n deals again
 *
 * Every key also makes the cursor visible. */
enum sol_cmd sol_view_key(struct sol_ui *ui, struct sol_game *g, uint32_t key);

/* A tap on the table at a hit. Hides the cursor. The felt drops the
 * selection; the stock draws; with nothing selected a tap picks up; with a
 * selection a tap on another pile puts it down there, on the selected card
 * again sends it to its foundation when one takes it, and elsewhere on its
 * own pile drops it or picks up from the new card. Taps on the table do
 * nothing while asking or after a win: the buttons answer those. */
enum sol_cmd sol_view_tap(struct sol_ui *ui, struct sol_game *g, struct sol_hit hit);

/* The buttons. */
enum sol_cmd sol_view_new_game_button(struct sol_ui *ui, const struct sol_game *g);
enum sol_cmd sol_view_keep_playing_button(struct sol_ui *ui);

/* The caption over the buttons, upper case, for DS mono captions. */
void sol_view_caption(const struct sol_ui *ui, const struct sol_game *g, char *out, size_t len);
/* Words for a refusal; `to` is the pile aimed at, or -1. */
const char *sol_view_reason(enum sol_result why, int to);

#endif
