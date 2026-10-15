/*
 * PG Solitaire in the running app, driven through the real logical key
 * stream and a real LVGL pointer on the real table widget.
 *
 * The rules and view tests prove the game and the interaction model. This
 * proves what they cannot: that a key pushed into pos_input reaches the game
 * as the rules say it should, that a finger on the drawn table lands on the
 * card the geometry says, that neither steals the focus, that the buttons
 * answer the question and the win, that the screen fits in every mode and
 * theme with the longest column inside the table, and that the app writes
 * nothing and leaves nothing behind.
 *
 * The shell is not linked; this file hosts the app as ui/shell/shell.c does
 * and supplies app.h. Built by ui/shell/CMakeLists.txt (host only), run by
 * tests/sol_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"
#include "chrome.h"
#include "pocketui.h"
#include "sol_app.h"
#include "sol_palette.h"
#include "sol_store.h"
#include "sol_table_widget.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
/* What the shell keeps above the content area for this app's chrome
 * (chrome.h, DS §36), in the orientation the display is in now. */
#define STATUS_H chrome_height(chrome_resolve(app_solitaire.chrome, pocketui_display_geometry()->width > pocketui_display_geometry()->height, false))

extern const struct pocketos_app app_solitaire;

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

/* ---- app.h ---------------------------------------------------------------------- */

static int shell_calls;

void pocketos_shell_set_status_hint(const char *text)
{
    (void)text;
    shell_calls++;
}
void pocketos_shell_go_home(void) { shell_calls++; }
int pocketos_shell_reduced_motion(void)
{
    shell_calls++;
    return 0;
}
int64_t pocketos_shell_system_day(void)
{
    shell_calls++;
    return -1;
}
const char *pocketos_shell_radio_state(void)
{
    shell_calls++;
    return NULL;
}
void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user), void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
    shell_calls++;
}
void pocketos_shell_keyboard_hide(void) { shell_calls++; }
int pocketos_shell_keyboard_visible(void)
{
    shell_calls++;
    return 0;
}

/* ---- display and finger ------------------------------------------------------- */

static uint8_t draw_buf[PANEL_W * 40 * 4];
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;

/* A copy of every pixel flushed, for the checks that read what was drawn
 * (test_long_columns): rows at the display's width in its own colour format
 * (RGB565 in this build), as large as the largest display those checks open. */
#define FRAME_MAX_W 1920
#define FRAME_MAX_H 1080
static uint8_t frame[FRAME_MAX_W * FRAME_MAX_H * 4];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    int32_t w = lv_area_get_width(a);
    uint32_t bpp = lv_color_format_get_size(lv_display_get_color_format(d));
    uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)w, lv_display_get_color_format(d));
    int32_t hor = lv_display_get_horizontal_resolution(d);
    int32_t y;

    if ((bpp == 2 || bpp == 4) && hor <= FRAME_MAX_W) {
        for (y = a->y1; y <= a->y2 && y < FRAME_MAX_H; y++) {
            if (y >= 0 && a->x1 >= 0 && a->x2 < hor) {
                memcpy(&frame[((size_t)y * (size_t)hor + (size_t)a->x1) * bpp], px + (size_t)(y - a->y1) * stride,
                       (size_t)w * bpp);
            }
        }
    }
    lv_display_flush_ready(d);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = finger_state;
    data->point = finger_point;
}

static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static void drain(void)
{
    int t;

    for (t = 0; t < 1000 && pos_input_queued() > 0; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
    pump(40);
}

static void key(pos_key_t k)
{
    pos_input_push_key(k);
    drain();
}

static void tap_at(int x, int y)
{
    finger_point.x = x;
    finger_point.y = y;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(50);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(50);
}

static void tap_obj(lv_obj_t *obj)
{
    lv_area_t a;

    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    tap_at(a.x1 + lv_area_get_width(&a) / 2, a.y1 + lv_area_get_height(&a) / 2);
}

/* ---- hosting ------------------------------------------------------------------------ */

static lv_obj_t *g_content;
static lv_obj_t *app_root;
static lv_obj_t *app_body;
static void *app_priv;

static void app_start(void)
{
    lv_obj_t *header;

    app_root = lv_obj_create(g_content);
    lv_obj_remove_style_all(app_root);
    lv_obj_set_size(app_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app_root, LV_FLEX_FLOW_COLUMN);
    header = lv_obj_create(app_root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), POCKETUI_HEADER_H);
    app_body = lv_obj_create(app_root);
    lv_obj_remove_style_all(app_body);
    lv_obj_set_width(app_body, LV_PCT(100));
    lv_obj_set_flex_grow(app_body, 1);
    lv_obj_set_flex_flow(app_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(app_body, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_top(app_body, POCKETUI_BODY_PAD_TOP, 0);
    lv_obj_set_style_pad_row(app_body, POCKETUI_PAD, 0);
    lv_obj_add_flag(app_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(app_body, LV_DIR_VER);
    app_priv = app_solitaire.create(app_body);
    pump(60);
}

static void app_stop(void)
{
    app_solitaire.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(60);
}

static const struct sol_game *game(void) { return sol_app_game(app_priv); }
static const struct sol_ui *ui(void) { return sol_app_ui(app_priv); }
static lv_obj_t *sink(void) { return app_body ? lv_obj_get_child(app_body, 0) : NULL; }
static const char *caption(void) { return lv_label_get_text(sol_app_caption(app_priv)); }

static int shows(int index, const char *text)
{
    lv_obj_t *b = sol_app_button(app_priv, index);

    if (!text) {
        return lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN);
    }
    return !lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN) && strcmp(lv_label_get_text(lv_obj_get_child(b, 0)), text) == 0;
}

/* Screen coordinates of a card (or of a pile's slot for index -1). */
static lv_point_t card_point(int pile, int index)
{
    lv_obj_t *table = sol_app_table(app_priv);
    struct sol_table t;
    struct sol_rect r;
    lv_area_t a;
    lv_point_t p;

    lv_obj_update_layout(table);
    lv_obj_get_coords(table, &a);
    sol_table_geometry(table, &t);
    r = index >= 0 ? sol_view_card(&t, game(), pile, index) : sol_view_slot(&t, pile);
    p.x = a.x1 + r.x + r.w / 2;
    p.y = a.y1 + r.y + (sol_is_column(pile) && index >= 0 && index < game()->pile[pile].n - 1 ? 6 : r.h / 2);
    return p;
}

static void tap_card(int pile, int index)
{
    lv_point_t p = card_point(pile, index);

    tap_at(p.x, p.y);
}

/* Drive the keyboard cursor to a card, the way a person would. */
static void cursor_to(int pile, int index)
{
    int guard;

    if (sol_is_column(pile)) {
        key((pos_key_t)('1' + pile - SOL_T0));
        for (guard = 0; guard < 20 && ui()->cursor_index > index; guard++) {
            key(LV_KEY_UP);
        }
        return;
    }
    key((pos_key_t)('1' + (pile == SOL_STOCK ? 0 : pile == SOL_WASTE ? 1 : 3 + pile - SOL_F0)));
    for (guard = 0; guard < 20 && sol_is_column(ui()->cursor_pile); guard++) {
        key(LV_KEY_UP);
    }
}

/* The first legal move in the game other than to a foundation, or 0. */
static int find_move(const struct sol_game *g, int *from, int *index, int *to)
{
    int f;
    int t;

    for (f = SOL_WASTE; f < SOL_PILES; f++) {
        int i;

        for (i = 0; i < g->pile[f].n; i++) {
            for (t = SOL_T0; t < SOL_PILES; t++) {
                if (sol_can_move(g, f, i, t) == SOL_OK) {
                    *from = f;
                    *index = i;
                    *to = t;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* Draw until a legal tableau move exists, or give up. */
static int draw_until_move(int *from, int *index, int *to, int by_key)
{
    int i;

    for (i = 0; i < 80; i++) {
        if (find_move(game(), from, index, to)) {
            return 1;
        }
        if (by_key) {
            key('d');
        } else {
            tap_card(SOL_STOCK, -1);
        }
    }
    return 0;
}

static int dir_is_empty(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int empty = 1;

    if (!d) {
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            empty = 0;
        }
    }
    closedir(d);
    return empty;
}

static char state_dir[] = "/tmp/sol_app_state.XXXXXX";

static int file_size(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? (int)st.st_size : -1;
}

/* The same game: every pile's cards in order, face-down counts, counters. */
static int same_game(const struct sol_game *a, const struct sol_game *b)
{
    int p;

    if (a->seed != b->seed || a->moves != b->moves || a->passes != b->passes || a->won != b->won) {
        return 0;
    }
    for (p = 0; p < SOL_PILES; p++) {
        if (a->pile[p].n != b->pile[p].n || a->pile[p].down != b->pile[p].down ||
            memcmp(a->pile[p].card, b->pile[p].card, a->pile[p].n) != 0) {
            return 0;
        }
    }
    return 1;
}

/* ---- cases --------------------------------------------------------------------------- */


/* ---- landscape (DS §21) --------------------------------------------------------- */

static lv_display_t *g_disp;

static int32_t status_h(void)
{
    return STATUS_H;
}

#include "games_frame.h"

/* The shell turned to landscape: the table beside the HUD and the controls,
 * the cards at least as large as portrait's, every button and the caption
 * inside the body, and taps and keys still play there. */
static void test_landscape(void)
{
    struct sol_table portrait_t;
    struct sol_table t;
    lv_area_t body;
    lv_area_t cap;
    int stock;

    app_start();
    sol_table_geometry(sol_app_table(app_priv), &portrait_t);
    app_stop();
    games_use_display(g_disp, g_content, POS_ROTATION_270, status_h);
    pump(60);
    app_start();
    lv_obj_update_layout(app_body);
    lv_obj_get_coords(app_body, &body);
    check("landscape: the body is wider than tall", lv_area_get_width(&body) > lv_area_get_height(&body));
    check("landscape: the screen fits the body, every button whole",
          games_screen_fits(app_body, app_body, "solitaire landscape") == 0);
    sol_table_geometry(sol_app_table(app_priv), &t);
    check("landscape: the cards are no smaller than portrait's", t.card_w >= portrait_t.card_w && t.card_w >= 60);
    stock = game()->pile[SOL_STOCK].n;
    tap_card(SOL_STOCK, -1);
    check("landscape: a tap on the stock draws", game()->pile[SOL_STOCK].n != stock || stock == 0);
    key('d');
    check("landscape: so does d", game()->pile[SOL_STOCK].n != stock || stock == 0);
    /* The longest caption there is, with a card selected. */
    cursor_to(SOL_WASTE, 0);
    key(LV_KEY_ENTER);
    lv_obj_update_layout(app_body);
    lv_obj_get_coords(sol_app_caption(app_priv), &cap);
    check("landscape: a long caption wraps inside the side column",
          games_inside(&cap, &body) && cap.x1 > lv_obj_get_x(sol_app_table(app_priv)) + t.w);
    check("landscape: the selected state fits too", games_screen_fits(app_body, app_body, "solitaire selected") == 0);
    key(LV_KEY_ESC);
    games_use_display(g_disp, g_content, POS_ROTATION_0, status_h);
    pump(60);
    check("turned back to portrait with the app open, the screen fits again",
          games_screen_fits(app_body, app_body, "solitaire portrait again") == 0);
    lv_obj_get_coords(sol_app_caption(app_priv), &cap);
    check("and the caption is one line again", lv_area_get_height(&cap) <=
                                                 lv_font_get_line_height(lv_obj_get_style_text_font(
                                                     sol_app_caption(app_priv), 0)));
    app_stop();
    check("landscape: nothing left behind", lv_obj_get_child_count(g_content) == 0u && pos_input_focused() == NULL);
}

/* ---- long columns: every card's corner index stays readable ---------------------------- */

/* An external display as the shell runs one (shell_display.h): upright at
 * rotation 0, square corners, the content area under the app's chrome. */
static void use_external(int32_t w, int32_t h)
{
    struct pos_panel panel = { .width = w, .height = h, .corners = { 0, 0, 0, 0 } };
    struct pos_display_geometry geo;

    pos_display_geometry_init(&geo, &panel, POS_ROTATION_0);
    pocketui_set_display_geometry(&geo);
    lv_display_set_resolution(g_disp, geo.width, geo.height);
    lv_obj_set_size(g_content, geo.width, geo.height - status_h());
    lv_obj_set_pos(g_content, 0, status_h());
}

static const lv_font_t *role_font(enum pos_style_role role, int32_t *letter_space)
{
    lv_style_value_t v;

    *letter_space = 0;
    if (lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_LETTER_SPACE, &v) == LV_STYLE_RES_FOUND) {
        *letter_space = v.num;
    }
    return lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_FONT, &v) == LV_STYLE_RES_FOUND ? v.ptr : NULL;
}

static struct sol_game *game_rw(void) { return (struct sol_game *)sol_app_game(app_priv); }
static struct sol_ui *ui_rw(void) { return (struct sol_ui *)sol_app_ui(app_priv); }

/* The corner index sol_draw_face paints on a card `r` (screen coordinates):
 * the rank's line in the corner font, inset max(3, w / 14), as wide as the
 * rank; and the w / 4 pip beside it at the right inset, centred half a line
 * down, with a pixel around it for its edges. */
static void corner_boxes(lv_area_t r, sol_card_t card, lv_area_t box[2])
{
    int w = lv_area_get_width(&r);
    int pad = w / 14 < 3 ? 3 : w / 14;
    int pip = w / 4;
    int32_t ls;
    const lv_font_t *font = role_font(w >= 56 ? POS_STYLE_TITLE : POS_STYLE_ROW_TITLE, &ls);
    int line = font ? lv_font_get_line_height(font) : 20;
    lv_point_t size = { 0, 0 };
    int cx = r.x2 - pad - pip / 2;
    int cy = r.y1 + pad / 2 + line / 2;

    if (font) {
        lv_text_get_size(&size, sol_rank_text(sol_card_rank(card)), font, ls, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }
    box[0].x1 = r.x1 + pad;
    box[0].x2 = box[0].x1 + size.x - 1;
    box[0].y1 = r.y1 + pad / 2 - line / 12;
    box[0].y2 = box[0].y1 + line - 1;
    box[1].x1 = cx - pip / 2 - 1;
    box[1].x2 = cx + pip / 2 + 1;
    box[1].y1 = cy - pip / 2 - 1;
    box[1].y2 = cy + pip / 2 + 1;
    /* Only the card's own pixels: a pip that reaches past the top edge by an
     * antialiased pixel or two is shaded against whatever is behind it -
     * felt for the card alone, paper in a column. */
    box[0].y1 = LV_MAX(box[0].y1, r.y1);
    box[1].y1 = LV_MAX(box[1].y1, r.y1);
}

static int is_ink(int32_t x, int32_t y, lv_color_t ink)
{
    uint32_t bpp = lv_color_format_get_size(lv_display_get_color_format(g_disp));
    int32_t hor = lv_display_get_horizontal_resolution(g_disp);
    int32_t ver = lv_display_get_vertical_resolution(g_disp);
    const uint8_t *p;

    if (x < 0 || y < 0 || x >= hor || y >= ver) {
        return 0;
    }
    p = &frame[((size_t)y * (size_t)hor + (size_t)x) * bpp];
    if (bpp == 2) {
        uint16_t v;

        memcpy(&v, p, sizeof(v));
        return v == lv_color_to_u16(ink);
    }
    return p[0] == ink.blue && p[1] == ink.green && p[2] == ink.red;
}

/* The ink of `card`'s corner on `r` in the last frame, box by box and row by
 * row into `mask`. Returns the number of ink pixels. */
#define MASK_MAX (2 * 160 * 160)
static int corner_ink(lv_area_t r, sol_card_t card, uint8_t *mask, int *len)
{
    lv_color_t ink = sol_palette[sol_card_is_red(card) ? SOL_INK_RED : SOL_INK_BLACK];
    lv_area_t box[2];
    int n = 0;
    int k = 0;
    int b;

    corner_boxes(r, card, box);
    for (b = 0; b < 2; b++) {
        int32_t x;
        int32_t y;

        for (y = box[b].y1; y <= box[b].y2; y++) {
            for (x = box[b].x1; x <= box[b].x2 && k < MASK_MAX; x++) {
                mask[k] = (uint8_t)is_ink(x, y, ink);
                n += mask[k++];
            }
        }
    }
    *len = k;
    return n;
}

static void redraw(void)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(g_disp);
}

/* Where a card is drawn on the screen now, as the table's geometry has it. */
static lv_area_t card_area(int pile, int index)
{
    lv_obj_t *table = sol_app_table(app_priv);
    struct sol_table t;
    struct sol_rect r;
    lv_area_t ta;
    lv_area_t a;

    lv_obj_get_coords(table, &ta);
    sol_table_geometry(table, &t);
    r = sol_view_card(&t, game(), pile, index);
    a.x1 = ta.x1 + r.x;
    a.y1 = ta.y1 + r.y;
    a.x2 = a.x1 + r.w - 1;
    a.y2 = a.y1 + r.h - 1;
    return a;
}

/* Face-up card `index` of column `pile`, reached with the keyboard as a
 * person reaches it and the cursor then hidden, shows its whole corner:
 * every ink pixel of its rank and its pip that the same card shows drawn
 * alone at the head of the column, below the top row and inside the table.
 * Returns problems, each printed with `mode`. */
static int column_card_readable(const char *mode, int pile, int index)
{
    static uint8_t got[MASK_MAX];
    static uint8_t want[MASK_MAX];
    lv_obj_t *table = sol_app_table(app_priv);
    struct sol_game whole = *game();
    struct sol_ui ui_was;
    sol_card_t card = whole.pile[pile].card[index];
    struct sol_table t;
    lv_area_t ta;
    lv_area_t a;
    lv_area_t box[2];
    int got_len;
    int want_len;
    int n_want;
    int missing = 0;
    int outside;
    int k;

    cursor_to(pile, index);
    if (ui()->cursor_pile != pile || ui()->cursor_index != index) {
        printf("note %s: the keyboard does not reach column %d card %d\n", mode, pile - SOL_T0 + 1, index);
        return 1;
    }
    ui_was = *ui();
    ui_rw()->keyboard = 0;
    redraw();
    lv_obj_get_coords(table, &ta);
    sol_table_geometry(table, &t);
    a = card_area(pile, index);
    corner_boxes(a, card, box);
    outside = box[0].y1 < ta.y1 + t.top_y + t.card_h || box[1].y1 < ta.y1 + t.top_y + t.card_h ||
              box[0].y2 > ta.y2 || box[1].y2 > ta.y2;
    corner_ink(a, card, got, &got_len);

    game_rw()->pile[pile].n = 1;
    game_rw()->pile[pile].down = 0;
    game_rw()->pile[pile].card[0] = card;
    redraw();
    n_want = corner_ink(card_area(pile, 0), card, want, &want_len);
    *game_rw() = whole;
    *ui_rw() = ui_was;
    for (k = 0; k < want_len && k < got_len; k++) {
        missing += want[k] && !got[k];
    }
    if (n_want == 0 || missing || outside || got_len != want_len) {
        printf("note %s: column %d card %d of %d (%s of %s, %d down) shows %d of its %d index pixels%s "
               "(card at y %d, %d px below the one before)\n",
               mode, pile - SOL_T0 + 1, index + 1, whole.pile[pile].n, sol_rank_text(sol_card_rank(card)),
               sol_suit_name(sol_card_suit(card)), whole.pile[pile].down, n_want - missing, n_want, outside ? ", outside the tableau" : "",
               (int)(a.y1 - ta.y1), index > 0 ? (int)(a.y1 - card_area(pile, index - 1).y1) : 0);
        return 1;
    }
    return 0;
}

/* Every face-up card in every column. Returns problems. */
static int tableau_readable(const char *mode)
{
    int bad = 0;
    int c;

    for (c = 0; c < SOL_COLUMNS; c++) {
        const struct sol_stack *s = &game()->pile[SOL_T0 + c];
        int i;

        for (i = s->down; i < s->n; i++) {
            bad += column_card_readable(mode, SOL_T0 + c, i);
        }
    }
    key(LV_KEY_ESC);
    return bad;
}

/* The cold review's reachable game from seed 1 (2026-10-15), in its
 * notation: Tn a tableau column, W the waste, F1 the spades foundation, a
 * bracketed source index zero-based, D a draw and Dn n draws. It ends with
 * eleven face-up cards in column 1. */
static const char long_column_play[] =
    "T3[2]>T2 T3[1]>T5 T3[0]>T7 D6 W[5]>T1 "
    "T4[3]>T1 T4[2]>F1 D W[5]>T1 T7[6]>T1 "
    "D W[5]>T4 T4[2]>T2 D W[5]>T4 "
    "D4 W[8]>T6 D W[8]>T7 T5[4]>T7 "
    "T5[3]>T3 T7[5]>T5 T7[4]>T5 T6[6]>T7 D "
    "T7[4]>T6 D T6[6]>T7 D T7[4]>T6 "
    "W[10]>T1 T7[3]>T1 T2[1]>T7 T1[2]>T2 T6[6]>T2 "
    "T2[1]>T1 D T1[2]>T2 W[10]>T2 T2[1]>T1 "
    "W[9]>T1";

/* Play long_column_play through the app's key stream, checking every action
 * against the rules on a copy. Returns the number of actions played, or -1. */
static int play_long_column(void)
{
    char buf[sizeof(long_column_play)];
    char *save = NULL;
    char *tok;
    int actions = 0;

    memcpy(buf, long_column_play, sizeof(buf));
    for (tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        struct sol_game expect = *game();
        int from;
        int index;
        int to;
        int a;
        int b;
        char kind;

        if (tok[0] == 'D') {
            int n = tok[1] ? atoi(tok + 1) : 1;

            while (n-- > 0) {
                sol_draw(&expect);
                key('d');
                actions++;
            }
            if (!same_game(game(), &expect)) {
                printf("note: %s did not draw as the rules do\n", tok);
                return -1;
            }
            continue;
        }
        if (sscanf(tok, "T%d[%d]>%c%d", &a, &index, &kind, &b) == 4) {
            from = SOL_T0 + a - 1;
        } else if (sscanf(tok, "W[%d]>%c%d", &index, &kind, &b) == 3) {
            from = SOL_WASTE;
            if (index != game()->pile[SOL_WASTE].n - 1) {
                printf("note: %s is not the waste's top card\n", tok);
                return -1;
            }
        } else {
            printf("note: cannot read %s\n", tok);
            return -1;
        }
        to = kind == 'F' ? SOL_F0 + b - 1 : SOL_T0 + b - 1;
        cursor_to(from, index);
        key(LV_KEY_ENTER);
        cursor_to(to, game()->pile[to].n ? game()->pile[to].n - 1 : -1);
        key(LV_KEY_ENTER);
        if (sol_move(&expect, from, index, to, NULL) != SOL_OK || !same_game(game(), &expect)) {
            printf("note: %s did not move as the rules do\n", tok);
            return -1;
        }
        actions++;
    }
    return actions;
}

/* Open the app on a deal through its own save file. */
static void open_deal(const struct sol_game *g)
{
    check("the deal can be stored", sol_store_save(g) == 0);
    app_start();
}

/* The longest column the rules allow, in column 7: six face down, then king
 * to ace face up; the other cards in the stock. A valid game. */
static void longest_column(struct sol_game *g)
{
    struct sol_stack *col = &g->pile[SOL_T0 + 6];
    uint8_t used[SOL_DECK];
    int r;
    int c;

    memset(g, 0, sizeof(*g));
    memset(used, 0, sizeof(used));
    g->seed = 20260913u; /* sol_app.c's review deal, so its "longest" state is this game */
    g->moves = 1;
    for (r = 1; r <= 6; r++) {
        col->card[col->n++] = sol_card(r, SOL_CLUBS);
    }
    col->down = 6;
    for (r = SOL_KING; r >= 1; r--) {
        col->card[col->n++] = sol_card(r, (r % 2) ? SOL_SPADES : SOL_HEARTS);
    }
    for (c = 0; c < col->n; c++) {
        used[col->card[c]] = 1;
    }
    for (c = 0; c < SOL_DECK; c++) {
        if (sol_card_valid((sol_card_t)c) && !used[c]) {
            g->pile[SOL_STOCK].card[g->pile[SOL_STOCK].n++] = (sol_card_t)c;
        }
    }
    g->pile[SOL_STOCK].down = g->pile[SOL_STOCK].n;
}

/* The first move of a run of two or more cards between columns, or 0. */
static int find_run_move(const struct sol_game *g, int *from, int *index, int *to)
{
    int f;

    for (f = SOL_T0; f < SOL_PILES; f++) {
        int i;

        for (i = g->pile[f].down; i < g->pile[f].n - 1; i++) {
            int t;

            for (t = SOL_T0; t < SOL_PILES; t++) {
                if (sol_can_move(g, f, i, t) == SOL_OK) {
                    *from = f;
                    *index = i;
                    *to = t;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* The table's tableau as it is seen: below the top row, inside the table. */
static int strip_visible(lv_point_t p)
{
    lv_obj_t *table = sol_app_table(app_priv);
    struct sol_table t;
    lv_area_t ta;

    lv_obj_get_coords(table, &ta);
    sol_table_geometry(table, &t);
    return p.y >= ta.y1 + t.tableau_y && p.y <= ta.y2 && p.x >= ta.x1 && p.x <= ta.x2;
}

/* A finger drag down the table's left margin by dy, in steps, then lifted:
 * up from near its foot, or down from its middle. */
static void drag_table(int dy)
{
    lv_area_t ta;
    int x;
    int y;
    int s;

    lv_obj_get_coords(sol_app_table(app_priv), &ta);
    x = ta.x1 + 4;
    y = dy < 0 ? ta.y2 - 10 : ta.y1 + lv_area_get_height(&ta) / 2;
    finger_point.x = x;
    finger_point.y = y;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(30);
    for (s = 1; s <= 10; s++) {
        finger_point.y = y + dy * s / 10;
        pump(20);
    }
    pump(100);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(600);
}

/* The reproduced position at every text size on the panel in both
 * orientations and on the HDMI sizes, and the longest legal column: every
 * face-up card's rank and suit can be brought into view and read, from the
 * pixels the real fonts drew. Then a run is picked up and moved there by
 * keys, and again by touch. */
static void test_long_columns(void)
{
    static const char *const sizes[] = { "small", "medium", "large" };
    static const char *const places[] = { "panel portrait", "panel landscape", "HDMI 1280x720", "HDMI 1920x1080" };
    enum pos_text_size was = pos_theme_current_text_size();
    struct sol_game seed1;
    struct sol_game played;
    struct sol_game longest;
    char what[160];
    char why[96];
    int p;
    int s;

    pos_theme_apply(NULL, "normal", why, sizeof(why));
    sol_deal(&seed1, 1);
    longest_column(&longest);
    check("the longest column is a valid game", sol_game_valid(&longest));
    for (p = 0; p < 4; p++) {
        switch (p) {
        case 0:
            games_use_display(g_disp, g_content, POS_ROTATION_0, status_h);
            break;
        case 1:
            games_use_display(g_disp, g_content, POS_ROTATION_270, status_h);
            break;
        case 2:
            use_external(1280, 720);
            break;
        default:
            use_external(1920, 1080);
            break;
        }
        pump(60);
        for (s = 0; s < 3; s++) {
            int actions;

            pos_theme_select_text_size((enum pos_text_size)s);
            open_deal(&seed1);
            actions = play_long_column();
            snprintf(what, sizeof(what), "%s, %s: the 49 actions play through the keys as the rules say", places[p],
                     sizes[s]);
            check(what, actions == 49 && sol_game_valid(game()));
            snprintf(what, sizeof(what), "%s, %s: column 1 ends with eleven face-up cards", places[p], sizes[s]);
            check(what, game()->pile[SOL_T0].n - game()->pile[SOL_T0].down == 11);
            played = *game();
            {
                struct sol_table t;
                struct sol_rect last;
                int fd;
                int fu;

                sol_table_geometry(sol_app_table(app_priv), &t);
                sol_view_fan(&t, &game()->pile[SOL_T0], &fd, &fu);
                last = sol_view_card(&t, game(), SOL_T0, game()->pile[SOL_T0].n - 1);
                printf("note %s, %s: table %dx%d, cards %dx%d, fans %d/%d, tableau from %d; column 1 steps %d px, "
                       "its last card ends at %d\n",
                       places[p], sizes[s], t.w, t.h, t.card_w, t.card_h, t.fan_down, t.fan_up, t.tableau_y, fu,
                       last.y + last.h);
            }
            snprintf(what, sizeof(what), "%s, %s: every face-up index of the reproduced game reads whole", places[p],
                     sizes[s]);
            check(what, tableau_readable(what) == 0);
            check("reading it changed nothing", same_game(game(), &played));
            app_stop();

            open_deal(&longest);
            snprintf(what, sizeof(what), "%s, %s: every index of six down and king to ace reads whole", places[p],
                     sizes[s]);
            check(what, tableau_readable(what) == 0);
            app_stop();
        }
    }

    /* A run moved in the reproduced position, by keys and by touch, on the
     * table where the column is longest: panel landscape at Large. */
    games_use_display(g_disp, g_content, POS_ROTATION_270, status_h);
    pos_theme_select_text_size(POS_TEXT_SIZE_LARGE);
    pump(60);

    /* The review states the captures use are this game, scrolled to the
     * cursor: on column 1's last card, then its first. */
    setenv("PGSOLITAIRE_SCREEN", "long", 1);
    app_start();
    check("review state long: the game the keys played", same_game(game(), &played));
    check("with the keyboard cursor on column 1's last card, in view",
          ui()->keyboard && ui()->cursor_pile == SOL_T0 && ui()->cursor_index == game()->pile[SOL_T0].n - 1 &&
              strip_visible(card_point(SOL_T0, game()->pile[SOL_T0].n - 1)));
    app_stop();
    setenv("PGSOLITAIRE_SCREEN", "longtop", 1);
    app_start();
    check("review state longtop: the cursor on column 1's first card, in view",
          ui()->cursor_pile == SOL_T0 && ui()->cursor_index == game()->pile[SOL_T0].down &&
              strip_visible(card_point(SOL_T0, game()->pile[SOL_T0].down)));
    app_stop();
    setenv("PGSOLITAIRE_SCREEN", "longest", 1);
    app_start();
    unsetenv("PGSOLITAIRE_SCREEN");
    check("review state longest: the longest column, the cursor on its ace in view",
          same_game(game(), &longest) && ui()->cursor_pile == SOL_T0 + 6 &&
              strip_visible(card_point(SOL_T0 + 6, 18)));
    app_stop();
    for (s = 0; s < 2; s++) {
        struct sol_game expect;
        int from;
        int index;
        int to;

        open_deal(&seed1);
        play_long_column();
        played = *game();
        expect = *game();
        if (!find_run_move(&expect, &from, &index, &to)) {
            check("the reproduced game has a run to move", 0);
            app_stop();
            break;
        }
        sol_move(&expect, from, index, to, NULL);
        if (s == 0) {
            cursor_to(from, index);
            key(LV_KEY_ENTER);
            check("keys: Enter picks up the run", ui()->sel_pile == from && ui()->sel_index == index);
            check("keys: the picked card is in view", strip_visible(card_point(from, index)));
            cursor_to(to, game()->pile[to].n - 1);
            check("keys: the destination's top card is in view", strip_visible(card_point(to, game()->pile[to].n - 1)));
            key(LV_KEY_ENTER);
            check("keys: Enter puts the run down exactly as the rules move it", same_game(game(), &expect));
        } else {
            lv_point_t at = card_point(from, index);
            int guard;

            for (guard = 0; guard < 8 && !strip_visible(at); guard++) {
                lv_area_t ta;

                lv_obj_get_coords(sol_app_table(app_priv), &ta);
                drag_table(at.y > ta.y2 ? -120 : 120);
                check("touch: a drag on the table neither picks nor moves", ui()->sel_pile == -1 &&
                                                                               same_game(game(), &played));
                at = card_point(from, index);
            }
            check("touch: the run's first card can be brought into view", strip_visible(at));
            tap_at(at.x, at.y);
            check("touch: a tap on its strip picks up the run", ui()->sel_pile == from && ui()->sel_index == index);
            {
                lv_area_t ta;
                struct sol_table t;
                struct sol_rect slot;

                lv_obj_get_coords(sol_app_table(app_priv), &ta);
                sol_table_geometry(sol_app_table(app_priv), &t);
                slot = sol_view_slot(&t, to);
                tap_at(ta.x1 + slot.x + slot.w / 2, ta.y1 + t.tableau_y + 6);
            }
            check("touch: a tap on the destination column moves it exactly as the rules do",
                  same_game(game(), &expect));
        }
        app_stop();
    }
    unlink(sol_store_path());
    pos_theme_select_text_size(was);
    games_use_display(g_disp, g_content, POS_ROTATION_0, status_h);
    pump(60);
}

static void test_open(void)
{
    int c;
    int ok = 1;

    app_start();
    check("the app returns its state", app_priv != NULL);
    check("id and launcher name", strcmp(app_solitaire.id, "solitaire") == 0 &&
                                      strcmp(app_solitaire.name, "Solitaire") == 0 && app_solitaire.icon);
    check("a tick, which is where saves happen", app_solitaire.tick != NULL);
    for (c = 0; c < SOL_COLUMNS; c++) {
        ok &= game()->pile[SOL_T0 + c].n == c + 1;
    }
    check("it opens on a fresh deal", ok && game()->pile[SOL_STOCK].n == 24 && game()->moves == 0 &&
                                          sol_game_valid(game()));
    check("the root has the focus", pos_input_focused() == sink());
    check("the caption tells a finger what to do", strcmp(caption(), "TAP A CARD, THEN WHERE IT GOES") == 0);
    check("one NEW GAME button", shows(0, NULL) && shows(1, "NEW GAME"));
    check("no cursor until a key is used", !ui()->keyboard);
}

static void test_keys(void)
{
    struct sol_game expect;
    int from;
    int index;
    int to;
    int i;

    key(LV_KEY_RIGHT);
    check("a key shows the cursor and says what the keys do", ui()->keyboard &&
                                                              strstr(caption(), "ARROWS") != NULL);
    expect = *game();
    key('d');
    sol_draw(&expect);
    check("d draws exactly as the rules do", memcmp(&expect, game(), sizeof(expect)) == 0);

    for (i = 0; i < 6; i++) {
        if (!draw_until_move(&from, &index, &to, 1)) {
            break;
        }
        expect = *game();
        cursor_to(from, index);
        check("the cursor reaches the card", ui()->cursor_pile == from &&
                                                 (!sol_is_column(from) || ui()->cursor_index == index));
        key(LV_KEY_ENTER);
        check("Enter picks it up", ui()->sel_pile == from && ui()->sel_index == index);
        cursor_to(to, game()->pile[to].n ? game()->pile[to].n - 1 : -1);
        key(LV_KEY_ENTER);
        sol_move(&expect, from, index, to, NULL);
        check("Enter puts it down exactly as the rules move it", memcmp(&expect, game(), sizeof(expect)) == 0);
        check("and the selection is gone", ui()->sel_pile == -1);
    }
    check("keys made real moves", game()->moves > 1);

    /* Pick something, then cancel. */
    if (draw_until_move(&from, &index, &to, 1)) {
        cursor_to(from, index);
        key(' ');
        check("Space picks up too", ui()->sel_pile == from);
        key(LV_KEY_ESC);
        check("Esc drops the selection", ui()->sel_pile == -1);
        key(LV_KEY_ENTER);
        key(LV_KEY_BACKSPACE);
        check("so does Backspace", ui()->sel_pile == -1);
    }
    key('q');
    key(LV_KEY_NEXT);
    key(0xE6);
    check("unmapped keys leave the focus on the root", pos_input_focused() == sink() && sol_game_valid(game()));
}

static void test_taps(void)
{
    struct sol_game expect;
    int from;
    int index;
    int to;
    int i;

    tap_card(SOL_STOCK, -1);
    check("a tap hides the cursor", !ui()->keyboard);
    for (i = 0; i < 6; i++) {
        if (!draw_until_move(&from, &index, &to, 0)) {
            break;
        }
        expect = *game();
        tap_card(from, index);
        check("a tap on a card picks it up", ui()->sel_pile == from && ui()->sel_index == index);
        check("and leaves the focus on the root", pos_input_focused() == sink());
        tap_card(to, game()->pile[to].n ? game()->pile[to].n - 1 : -1);
        sol_move(&expect, from, index, to, NULL);
        check("a tap on the destination moves it exactly as the rules do",
              memcmp(&expect, game(), sizeof(expect)) == 0);
    }
    if (draw_until_move(&from, &index, &to, 0)) {
        lv_area_t a;

        tap_card(from, index);
        lv_obj_get_coords(sol_app_table(app_priv), &a);
        tap_at(a.x1 + 3, a.y2 - 3);
        check("a tap on the felt drops the selection", ui()->sel_pile == -1);
    }
    expect = *game();
    tap_card(SOL_STOCK, -1);
    sol_draw(&expect);
    check("a tap on the stock draws", memcmp(&expect, game(), sizeof(expect)) == 0);
}

static void test_new_game(void)
{
    struct sol_game before = *game();

    check("the deal has progress now", sol_view_has_progress(game()));
    tap_obj(sol_app_button(app_priv, 1));
    check("NEW GAME asks first", ui()->confirming && strstr(caption(), "START A NEW GAME?") != NULL);
    check("with KEEP PLAYING first in the accent", shows(0, "KEEP PLAYING") && shows(1, "NEW GAME"));
    tap_card(SOL_STOCK, -1);
    key('d');
    check("neither a tap nor a key plays under the question", memcmp(&before, game(), sizeof(before)) == 0);
    tap_obj(sol_app_button(app_priv, 0));
    check("KEEP PLAYING keeps the deal", !ui()->confirming && memcmp(&before, game(), sizeof(before)) == 0);
    key('n');
    key('n');
    check("n twice deals again", game()->moves == 0 && memcmp(before.pile, game()->pile, sizeof(before.pile)) != 0);
    check("with the cursor and selection reset", ui()->sel_pile == -1 && !ui()->confirming);
    key('d');
    tap_obj(sol_app_button(app_priv, 1));
    tap_obj(sol_app_button(app_priv, 1));
    check("NEW GAME twice deals too", game()->moves == 0 && pos_input_focused() == sink());
}

static void test_random(void)
{
    static const pos_key_t keys[] = { LV_KEY_UP, LV_KEY_DOWN, LV_KEY_LEFT, LV_KEY_RIGHT, LV_KEY_ENTER, ' ',
                                      LV_KEY_ESC, 'd', 'f', '1', '5', '7' };
    struct sol_rng rng;
    lv_area_t a;
    int step;
    int bad = 0;
    uint32_t moves = 0;

    sol_rng_seed(&rng, 5);
    lv_obj_get_coords(sol_app_table(app_priv), &a);
    for (step = 0; step < 1500; step++) {
        uint32_t before = game()->moves;

        if (sol_rng_below(&rng, 2)) {
            key(keys[sol_rng_below(&rng, sizeof(keys) / sizeof(keys[0]))]);
        } else {
            tap_at(a.x1 + (int)sol_rng_below(&rng, (uint32_t)lv_area_get_width(&a)),
                   a.y1 + (int)sol_rng_below(&rng, (uint32_t)lv_area_get_height(&a)));
        }
        moves += game()->moves - before;
        if (ui()->confirming) {
            key(LV_KEY_ESC);
        }
        if (!sol_game_valid(game()) || (ui()->sel_pile >= 0 && sol_can_pick(game(), ui()->sel_pile, ui()->sel_index))) {
            bad++;
            break;
        }
    }
    check("1500 random keys and taps through the app keep the game valid", bad == 0);
    check("and play it", moves > 50);
    check("the focus never left the root", pos_input_focused() == sink());
}

static void test_glass(void)
{
    static const char *const modes[] = { "normal", "outdoor", "night" };
    char why[128];
    char what[128];
    int m;

    for (m = 0; m < 3; m++) {
        lv_obj_t *table = sol_app_table(app_priv);
        struct sol_table t;
        lv_area_t body;
        lv_area_t ta;
        lv_area_t ca;
        lv_area_t b;
        int i;

        pos_theme_apply(NULL, modes[m], why, sizeof(why));
        pump(60);
        lv_obj_update_layout(app_body);
        lv_obj_get_coords(app_body, &body);
        lv_obj_get_coords(table, &ta);
        lv_obj_get_coords(lv_obj_get_child(sink(), 2), &ca);
        sol_table_geometry(table, &t);
        snprintf(what, sizeof(what), "%s: the table and controls are inside the body, apart", modes[m]);
        check(what, ta.x1 >= body.x1 + POCKETUI_PAD && ta.x2 <= body.x2 - POCKETUI_PAD && ca.y2 <= body.y2 - POCKETUI_PAD &&
                        ta.y2 < ca.y1 && ta.y1 > body.y1);
        snprintf(what, sizeof(what), "%s: cards are at least 64 px wide", modes[m]);
        check(what, t.card_w >= POCKETUI_TOUCH_MIN);
        snprintf(what, sizeof(what), "%s: nothing scrolls", modes[m]);
        check(what, lv_obj_get_scroll_bottom(app_body) <= 0 && lv_obj_get_scroll_top(app_body) == 0);
        for (i = 0; i < 2; i++) {
            lv_obj_t *btn = sol_app_button(app_priv, i);

            if (lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            lv_obj_get_coords(btn, &b);
            snprintf(what, sizeof(what), "%s: button %d is 64 px and spans to the edge when alone", modes[m], i);
            check(what, lv_area_get_height(&b) >= 64 && b.x2 == ca.x2);
        }
        {
            /* The longest column the rules allow ends inside the table. */
            struct sol_game longest;
            struct sol_rect last;
            int r;

            memset(&longest, 0, sizeof(longest));
            for (r = 0; r < 6; r++) {
                longest.pile[SOL_T0 + 6].card[longest.pile[SOL_T0 + 6].n++] = (sol_card_t)(26 + r);
            }
            for (r = 13; r >= 1; r--) {
                longest.pile[SOL_T0 + 6].card[longest.pile[SOL_T0 + 6].n++] =
                    sol_card(r, (r % 2) ? SOL_SPADES : SOL_HEARTS);
            }
            longest.pile[SOL_T0 + 6].down = 6;
            last = sol_view_card(&t, &longest, SOL_T0 + 6, longest.pile[SOL_T0 + 6].n - 1);
            snprintf(what, sizeof(what), "%s: a 19-card column ends inside the table", modes[m]);
            check(what, last.y + last.h <= t.h);
        }
    }
    for (m = 0; m < pos_theme_count(); m++) {
        pos_theme_apply(pos_theme_at(m)->id, "normal", why, sizeof(why));
        key(LV_KEY_LEFT);
        tap_card(SOL_STOCK, -1);
        pump(40);
    }
    pos_theme_apply("ice", "normal", why, sizeof(why));
    check("every theme draws the table and keeps playing", sol_game_valid(game()));
}

static void test_won_and_rounds(void)
{
    int i;

    app_stop();
    setenv("PGSOLITAIRE_SCREEN", "won", 1);
    app_start();
    unsetenv("PGSOLITAIRE_SCREEN");
    check("the won review state is won", game()->won && sol_cards_home(game()) == 52);
    check("the caption says how it went", strcmp(caption(), "SOLVED IN 212 MOVES") == 0);
    check("one accent NEW GAME button", shows(0, "NEW GAME") && shows(1, NULL));
    tap_card(SOL_F0, 12);
    check("the table ignores taps after a win", game()->won);
    key(LV_KEY_ENTER);
    check("Enter deals again", !game()->won && game()->moves == 0 && game()->pile[SOL_STOCK].n == 24);
    app_stop();

    for (i = 0; i < 5; i++) {
        app_start();
        key('d');
        tap_card(SOL_T0 + 3, 3);
        app_stop();
    }
    check("five rounds leave nothing behind", lv_obj_get_child_count(g_content) == 0u);
    check("and no focusable object", pos_input_focused() == NULL);
}

/* Leave, reopen, carry on: the deal comes back exactly, the interaction does
 * not; a new deal replaces the save; a won game, a damaged file and a review
 * state are each handled as sol_app.c says. Entered and left with the app
 * open. */
static void test_persistence(void)
{
    struct sol_game kept;
    struct sol_game disk;
    int from;
    int index;
    int to;
    int i;

    app_stop();
    unlink(sol_store_path());
    app_start();
    app_solitaire.tick(app_priv);
    app_stop();
    check("a fresh deal opened and left untouched writes nothing", file_size(sol_store_path()) == -1);

    app_start();
    for (i = 0; i < 4; i++) {
        if (draw_until_move(&from, &index, &to, 1)) {
            cursor_to(from, index);
            key(LV_KEY_ENTER);
            cursor_to(to, game()->pile[to].n ? game()->pile[to].n - 1 : -1);
            key(LV_KEY_ENTER);
        }
    }
    check("the deal has been played", game()->moves > 0);
    check("a move is not written as it happens", file_size(sol_store_path()) == -1);
    app_solitaire.tick(app_priv);
    check("the tick writes the deal: 101 bytes", file_size(sol_store_path()) == SOL_SAVE_SIZE);
    check("and what it wrote is the game on the table", sol_store_load(&disk) == 0 && same_game(&disk, game()));

    /* Leave with more played since the tick, and a card picked up. */
    key('d');
    key('d');
    if (draw_until_move(&from, &index, &to, 1)) {
        cursor_to(from, index);
        key(LV_KEY_ENTER);
    }
    check("a card is in hand as the app is left", ui()->sel_pile >= 0);
    kept = *game();
    app_stop();
    check("leaving saves the latest state", sol_store_load(&disk) == 0 && same_game(&disk, &kept));
    app_start();
    check("reopening resumes the same deal, card for card", same_game(game(), &kept));
    check("with the stock and waste where they were", game()->pile[SOL_STOCK].n == kept.pile[SOL_STOCK].n &&
                                                          sol_top(game(), SOL_WASTE) == sol_top(&kept, SOL_WASTE));
    check("but nothing in hand, no question and the cursor at the start",
          ui()->sel_pile == -1 && !ui()->confirming && ui()->cursor_pile == SOL_T0 && !ui()->keyboard);
    check("and the valid position the rules expect", sol_game_valid(game()));
    {
        struct sol_game expect = *game();

        key('d');
        sol_draw(&expect);
        check("the resumed deal plays on exactly as the rules say", same_game(game(), &expect));
    }

    /* A new game replaces the saved one. */
    key('n');
    key('n');
    check("n twice deals again", game()->moves == 0 && !same_game(game(), &kept));
    kept = *game();
    app_stop();
    app_start();
    check("after leaving, the new deal is the one that comes back", same_game(game(), &kept));

    /* A won game on disk is not resumed. */
    app_stop();
    {
        struct sol_game won;
        int s;
        int r;

        memset(&won, 0, sizeof(won));
        for (s = 0; s < SOL_SUITS; s++) {
            for (r = 1; r <= SOL_KING; r++) {
                won.pile[SOL_F0 + s].card[won.pile[SOL_F0 + s].n++] = sol_card(r, (enum sol_suit)s);
            }
        }
        won.won = 1;
        won.moves = 150;
        check("a won game can be stored", sol_store_save(&won) == 0);
    }
    app_start();
    check("a won game is not resumed: a new deal is dealt", !game()->won && game()->moves == 0 &&
                                                                game()->pile[SOL_STOCK].n == 24);
    app_solitaire.tick(app_priv);
    check("and it replaces the won game on the next tick", sol_store_load(&disk) == 0 && !disk.won &&
                                                               same_game(&disk, game()));
    app_stop();

    /* A damaged save. */
    {
        FILE *f = fopen(sol_store_path(), "wb");

        if (f) {
            fputs("not a solitaire save", f);
            fclose(f);
        }
    }
    app_start();
    check("a damaged save deals a new game", game()->moves == 0 && sol_game_valid(game()));
    check("and says so", strcmp(caption(), "SAVED GAME UNREADABLE \xC2\xB7 NEW DEAL") == 0);
    app_solitaire.tick(app_priv);
    check("the damaged file is left alone while nothing has been played",
          file_size(sol_store_path()) == (int)strlen("not a solitaire save"));
    key('d');
    check("the note goes once the new deal is played", strstr(caption(), "UNREADABLE") == NULL);
    app_solitaire.tick(app_priv);
    check("and the next save replaces the file", sol_store_load(&disk) == 0 && disk.moves == 1);
    app_stop();

    /* Review states neither read nor write the file. */
    unlink(sol_store_path());
    setenv("PGSOLITAIRE_SCREEN", "selected", 1);
    app_start();
    key('d');
    app_solitaire.tick(app_priv);
    app_stop();
    unsetenv("PGSOLITAIRE_SCREEN");
    check("a review state writes nothing", dir_is_empty(sol_store_dir()));
    app_start();
}

int main(void)
{
    lv_display_t *disp;
    lv_indev_t *finger;

    if (!mkdtemp(state_dir)) {
        printf("FAIL cannot make a temporary directory\n");
        return 1;
    }
    setenv("POCKETOS_STATE_DIR", state_dir, 1);
    unsetenv("PGSOLITAIRE_SCREEN");
    lv_init();
    g_disp = disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    finger = lv_indev_create();
    lv_indev_set_type(finger, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(finger, read_cb);
    pos_input_init();
    pocketui_init();
    pocketui_style_screen(lv_screen_active());
    g_content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_content);
    lv_obj_set_size(g_content, PANEL_W, PANEL_H - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    pump(60);

    test_open();
    test_keys();
    test_taps();
    test_new_game();
    test_random();
    test_glass();
    test_persistence();
    test_won_and_rounds();
    test_landscape();
    test_long_columns();

    check("the app asked the shell for nothing", shell_calls == 0);
    {
        char cmd[128];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", state_dir);
        if (system(cmd) != 0) {
            printf("note: could not remove %s\n", state_dir);
        }
    }
    printf("sol_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
