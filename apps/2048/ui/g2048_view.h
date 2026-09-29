/*
 * PG 2048 view model: which key or swipe means what, where things go in
 * whatever area the app is given, how a tile looks in the current theme, and
 * how a move is eased on screen.
 *
 * Pure C, like the engine: no LVGL, no I/O, no clock (tests/g2048_lint.sh).
 * It names LVGL's key numbers as plain numbers, and g2048_app.c checks at
 * compile time that they still agree. Everything a finger, a key or a layout
 * change can do is therefore testable without a display
 * (tests/g2048_view_test.c, tests/g2048_theme_test.c).
 *
 * ORIENTATION. Nothing here assumes the 568 x 1232 portrait panel. The layout
 * takes the width and height of the area the app is given and places the
 * HUD, the board and the controls in it: stacked when the area is taller
 * than wide, side by side when it is wider. The shell gives a portrait or a
 * landscape body (DS §21); both are covered by the view test and the app
 * test.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PG2048_VIEW_H
#define PG2048_VIEW_H

#include "g2048_rules.h"
#include "pos_theme.h"

#include <stdint.h>

/* LVGL's LV_KEY_UP, _DOWN, _RIGHT, _LEFT, _ENTER, _ESC and _BACKSPACE. */
#define G2048_KEY_UP 17u
#define G2048_KEY_DOWN 18u
#define G2048_KEY_RIGHT 19u
#define G2048_KEY_LEFT 20u
#define G2048_KEY_ENTER 10u
#define G2048_KEY_ESC 27u
#define G2048_KEY_BACKSPACE 8u

/* ---- what is on the controls row --------------------------------------- */

enum g2048_panel {
    G2048_PANEL_PLAY = 0, /* hint and NEW GAME */
    G2048_PANEL_CONFIRM,  /* "start again?": KEEP PLAYING (safe, accent) and NEW GAME */
    G2048_PANEL_WON,      /* the goal: KEEP GOING (accent) and NEW GAME */
    G2048_PANEL_OVER      /* no moves: NEW GAME (accent) */
};

/* The panel a game shows. confirming is the app's "asked to start again"
 * flag; it only counts while the game is being played. */
enum g2048_panel g2048_view_panel(const struct g2048_game *g, int confirming);

/* A board with something to lose: at least one move made and not over.
 * Starting again from such a board asks first (DS 17.5). */
int g2048_view_has_progress(const struct g2048_game *g);

enum g2048_cmd {
    G2048_CMD_NONE = 0,
    G2048_CMD_MOVE_UP,
    G2048_CMD_MOVE_DOWN,
    G2048_CMD_MOVE_LEFT,
    G2048_CMD_MOVE_RIGHT,
    G2048_CMD_ASK_NEW,    /* show the confirmation */
    G2048_CMD_NEW_GAME,   /* start a new game now */
    G2048_CMD_CANCEL,     /* close the confirmation, keep the board */
    G2048_CMD_KEEP_GOING  /* acknowledge the goal and continue */
};

/* The keyboard map. One logical key to one command, whatever produced the
 * key (DS 17.4), depending only on the panel on screen:
 *
 *   PLAY     arrows, w a s d (either case)  move
 *            n N                            new game; asks first when there
 *                                           is progress to lose
 *   CONFIRM  n N                            new game
 *            Enter Esc Backspace            keep playing
 *   WON      Enter Esc                      keep going
 *            n N                            new game
 *   OVER     Enter n N                      new game
 *
 * Moves are ignored while any panel but PLAY is up, so a key held from the
 * last move cannot slide the board under a question. */
enum g2048_cmd g2048_view_command_for_key(enum g2048_panel panel, int has_progress, uint32_t key);

/* The command a direction is. */
enum g2048_cmd g2048_view_move_cmd(enum g2048_dir dir);
/* The direction a move command is; 0 when the command is not a move. */
int g2048_view_cmd_dir(enum g2048_cmd cmd, enum g2048_dir *dir);

/* ---- swipes -------------------------------------------------------------- */

/* A press that travelled (dx, dy) pixels from where it went down. A swipe
 * when the longer axis covers at least `threshold` and is at least half as
 * long again as the shorter one, so a diagonal smear does nothing rather than
 * guess. Screen axes: +x right, +y down. Returns 1 and sets dir for a swipe. */
int g2048_view_swipe(int dx, int dy, int threshold, enum g2048_dir *dir);
/* The distance a swipe must cover on a board of this size: a tenth of the
 * board, and never under 32 px. */
int g2048_view_swipe_threshold(int board_px);

/* ---- layout ---------------------------------------------------------------- */

struct g2048_rect {
    int x;
    int y;
    int w;
    int h;
};

enum g2048_arrangement {
    G2048_STACKED = 0,  /* HUD, board, controls from the top: a tall area */
    G2048_SIDE_BY_SIDE  /* board on the left, HUD and controls on the right */
};

/* Heights of the two chrome blocks, from DS v0.1 section 7: a panel with a
 * caption and a hero-40 value, and a caption over 64 px buttons. */
#define G2048_HUD_H 112
#define G2048_CONTROLS_H 104
/* Space between blocks: the DS gap between panels. */
#define G2048_GAP 22
/* The narrowest side column the wide arrangement accepts. */
#define G2048_SIDE_MIN_W 240

struct g2048_layout {
    enum g2048_arrangement arrangement;
    struct g2048_rect hud;
    struct g2048_rect board;    /* square */
    struct g2048_rect controls;
};

/* Place everything in a w x h area, coordinates relative to its top-left.
 * The board is square and as large as the area allows. Every rect is inside
 * the area and none overlap; an area too small for the chrome still gets a
 * board of at least one pixel and rects clamped to the area. */
void g2048_view_layout(int w, int h, struct g2048_layout *out);

/* A cell inside a board of size x size: the gap between tiles is a
 * forty-fourth of the board (12 px on 528) and never under 6, and whatever
 * the integer division leaves over is split around the edge so the grid is
 * centred. Relative to the board's top-left. */
struct g2048_rect g2048_view_cell(int board_px, int index);
int g2048_view_cell_gap(int board_px);

/* ---- how a tile looks ------------------------------------------------------ */

/* A tile is a slab. Its fill is `fill_base` mixed toward `fill_toward` by
 * fill_mix/255 (LVGL's lv_color_mix(toward, base, mix)), so it follows the
 * theme and the mode and names no colour. The ramp is quiet on purpose: DS
 * section 1 keeps large bright fills to a short list, and a fill much past a
 * third accent lands where neither light nor dark text reads (measured across
 * the five themes: at 128/255 the best text is 3.5:1 in Normal and 2.4:1 in
 * Night). So brightness is carried by the digits and, from the goal up, a
 * 2 px accent outline; the number on the tile always says what it is and the
 * colour only helps (DS section 2).
 *
 *   exp 1 (2)       surface_raised                 text_secondary
 *   exp 2 (4)       surface_raised                 text
 *   exp 3..10       surface_raised toward accent_primary, 10..80 of 255
 *   exp 11+ (2048+) the same at 88, plus 2 px accent_primary outline
 *
 * `text` is text_primary or text_on_accent, whichever reads better on the
 * resolved fill in the current tokens (on this ramp, text_primary). */
struct g2048_tile_look {
    enum pos_color_token fill_base;
    enum pos_color_token fill_toward;
    uint8_t fill_mix;
    enum pos_color_token text;
    enum pos_color_token border;
    uint8_t border_px; /* 0: no outline */
};

#define G2048_MIX_MAX 88

void g2048_view_tile_look(uint8_t exp, const struct pos_theme_tokens *tokens,
                          struct g2048_tile_look *out);
/* The fill as it will be drawn, 0xRRGGBB, computed exactly as LVGL mixes. */
uint32_t g2048_view_fill_rgb(const struct g2048_tile_look *look, const struct pos_theme_tokens *tokens);

/* The tile's label, "2" .. "131072". */
void g2048_view_label(uint8_t exp, char *out, int out_len);

/* ---- motion -------------------------------------------------------------- */

/* A move: tiles slide for G2048_SLIDE_MS, then a merged tile swells and
 * settles and a new tile grows in over G2048_SETTLE_MS. Subtle, short, and
 * never blocking: a key or swipe during it finishes it at once. With reduced
 * motion (DS 12) none of it is drawn. */
#define G2048_SLIDE_MS 100
#define G2048_SETTLE_MS 90
#define G2048_MOTION_MS (G2048_SLIDE_MS + G2048_SETTLE_MS)
/* How far a merged tile swells, in pixels per side, at its peak. */
#define G2048_POP_PX 4

/* Progress through the slide, 0..256, eased out (quadratic). */
int g2048_view_slide_q8(int elapsed_ms);
/* Where a sliding tile is drawn: from `a` to `b` by q8/256. */
struct g2048_rect g2048_view_lerp(struct g2048_rect a, struct g2048_rect b, int q8);
/* Extra pixels per side for a tile that merged, over the settle phase:
 * 0 during the slide, up to G2048_POP_PX and back to 0. */
int g2048_view_pop_px(int elapsed_ms);
/* A new tile's size as a fraction of 256 of its cell: hidden during the
 * slide, then grows from 128 to 256. */
int g2048_view_spawn_q8(int elapsed_ms);

#endif
