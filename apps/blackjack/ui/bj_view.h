/*
 * PG Blackjack view model: where the dealer's and the player's hands go in
 * whatever area the app is given, which key means which action in which
 * phase, what the buttons are, and every word on the table and the caption.
 *
 * Pure C: no LVGL, no I/O, no clock (tests/bj_lint.sh). LVGL's key numbers
 * are named here as numbers and bj_app.c checks them at compile time.
 *
 * ORIENTATION. The screen blocks stack in a tall area and sit side by side in
 * a wide one; the table sizes its cards from both its width and its height,
 * and a hand's overlap tightens so a long hand still fits across. The shell
 * only gives a portrait body today; the wide cases are covered by the view
 * test.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGBJ_VIEW_H
#define PGBJ_VIEW_H

#include "bj_rules.h"

#include <stddef.h>
#include <stdint.h>

/* LVGL's LV_KEY_UP, _DOWN, _RIGHT, _LEFT, _ENTER, _ESC, _BACKSPACE. */
#define BJ_KEY_UP 17u
#define BJ_KEY_DOWN 18u
#define BJ_KEY_RIGHT 19u
#define BJ_KEY_LEFT 20u
#define BJ_KEY_ENTER 10u
#define BJ_KEY_ESC 27u
#define BJ_KEY_BACKSPACE 8u

struct bj_rect {
    int x;
    int y;
    int w;
    int h;
};

/* ---- screen blocks ---------------------------------------------------------- */

#define BJ_HUD_H 92
#define BJ_HUD_GAP 8
#define BJ_CONTROLS_H 104
#define BJ_GAP 22
#define BJ_SIDE_W 260

struct bj_screen {
    int side_by_side;
    struct bj_rect hud; /* two panels: side by side when stacked, one above the other beside a wide table */
    struct bj_rect table;
    struct bj_rect controls;
};

void bj_view_screen(int w, int h, struct bj_screen *out);

/* ---- the HUD ------------------------------------------------------------------ */

/* Panel 0 is "BANK": the chips not on the table. Panel 1 is "BET": the chips
 * on the table during a hand, else what the next round will bet. */
const char *bj_view_hud_label(int panel);
void bj_view_hud_value(const struct bj_game *g, int panel, char *out, size_t len);

/* ---- the table --------------------------------------------------------------- */

#define BJ_TABLE_PAD 16
/* The height given to the label above each hand. */
#define BJ_LABEL_H 36

enum bj_row {
    BJ_ROW_DEALER = 0,
    BJ_ROW_PLAYER = 1
};

struct bj_table {
    int w;
    int h;
    int card_w;
    int card_h;
    int label_y[2]; /* per row */
    int cards_y[2];
};

/* Cards as large as a quarter of the table's width and half its height (less
 * labels) allow, 5 wide by 7 tall. The dealer's row at the top, the player's
 * at the foot, nearest the controls. */
void bj_view_table(int w, int h, struct bj_table *out);
/* Card `index` of a hand of `n` cards in a row: centred, each card offset by
 * a little over half a card, tighter when the hand would not fit. */
struct bj_rect bj_view_card(const struct bj_table *t, enum bj_row row, int n, int index);
/* Whether the dealer's card `index` is drawn face down right now. */
int bj_view_face_down(const struct bj_game *g, int index);
/* The open felt between the dealer's cards and the player's label, where a
 * settled round's result is shown. Its height is what there is; the table
 * leaves the result out when its text is taller (a short, wide table). */
struct bj_rect bj_view_result_area(const struct bj_table *t);

/* ---- phases, buttons and keys ---------------------------------------------------- */

enum bj_panel {
    BJ_PANEL_BET = 0, /* between rounds: bet and deal */
    BJ_PANEL_PLAY,    /* the player's turn */
    BJ_PANEL_BROKE    /* not enough chips to deal */
};

enum bj_panel bj_view_panel(const struct bj_game *g);

enum bj_cmd {
    BJ_CMD_NONE = 0,
    BJ_CMD_DEAL,
    BJ_CMD_BET_DOWN,
    BJ_CMD_BET_UP,
    BJ_CMD_HIT,
    BJ_CMD_STAND,
    BJ_CMD_DOUBLE,
    BJ_CMD_NEW_BANKROLL
};

/* The keyboard map, whatever produced the key:
 *
 *   BET    Enter, Space, n     deal (NEW ROUND after the first)
 *          Left, Down, -       bet down 10
 *          Right, Up, +, =     bet up 10
 *   PLAY   h, Enter            hit
 *          s, Space            stand
 *          d                   double down
 *   BROKE  Enter, Space, n     new bankroll
 *
 * Letters in either case. Anything else, and any key for another panel, is
 * BJ_CMD_NONE. */
enum bj_cmd bj_view_command_for_key(enum bj_panel panel, uint32_t key);

/* The three buttons, left to right, for a panel. NULL text hides a button.
 * The first is the accent: the action the panel is for. Between rounds the
 * other two say what they change and by how much: "BET −10", "BET +10". */
struct bj_button {
    const char *text;
    enum bj_cmd cmd;
    int enabled;
};
void bj_view_buttons(const struct bj_game *g, struct bj_button out[3]);

/* Run a command against the game. Returns the rules' answer; a command that
 * does not apply to the phase is BJ_ERR_PHASE and changes nothing. */
enum bj_result bj_view_run(struct bj_game *g, enum bj_cmd cmd);

/* ---- words ------------------------------------------------------------------------- */

/* "DEALER 17", "DEALER SHOWS 10" while the hole card is down, "DEALER" with
 * no cards. */
void bj_view_dealer_label(const struct bj_game *g, char *out, size_t len);
/* "YOU 15", "YOU SOFT 17", "YOU 24 BUST", "YOU BLACKJACK", "YOU" with no cards. */
void bj_view_player_label(const struct bj_game *g, char *out, size_t len);
/* +1 the player won the settled round, 0 pushed or no round, -1 lost. */
int bj_view_player_result(const struct bj_game *g);
/* A settled round's result on the felt: "+15", "−10" with a real minus sign,
 * "PUSH"; "" while no round is settled. */
void bj_view_result(const struct bj_game *g, char *out, size_t len);

/* The caption over the buttons. `last` is the result of the last command the
 * player gave, so a refused one can say why. */
void bj_view_caption(const struct bj_game *g, enum bj_cmd last_cmd, enum bj_result last, char *out, size_t len);
/* What the caption says instead while saving is in trouble, when no refusal
 * needs explaining: "NOT SAVED · PLAY CONTINUES" after a save failed, "SAVED
 * GAME UNREADABLE · NEW BANKROLL" when the saved game was refused on opening;
 * NULL otherwise. */
const char *bj_view_store_note(int save_failed, int unreadable);

#endif
