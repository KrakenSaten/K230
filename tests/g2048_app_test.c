/*
 * PG 2048 in the running app, driven by a real LVGL pointer device and the
 * real logical key stream, against a real save file in a temporary directory.
 *
 * The rules, store and view tests prove the game, the file and the maps. This
 * one proves what they cannot: that a key pushed into pos_input reaches the
 * game, that a finger dragged across the glass is a move and a wobble is not,
 * that the buttons do what the keys do without taking the focus, that the
 * screen fits in every display mode and theme, that a game is saved and comes
 * back exactly, that a finished, won or damaged save is handled, and that
 * motion and reduced motion both end in the same board.
 *
 * The shell is not linked: this file hosts the app the way ui/shell/shell.c
 * does (a header and a padded body) and defines the app.h entry points.
 *
 * Built by ui/shell/CMakeLists.txt beside the shell (host builds only) and
 * run by tests/g2048_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "chrome.h"
#include "g2048_app.h"
#include "g2048_board.h"
#include "g2048_store.h"
#include "g2048_view.h"
#include "pocketui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
/* What the shell keeps above the content area for this app's chrome
 * (chrome.h, DS §36), in the orientation the display is in now. */
#define STATUS_H chrome_height(chrome_resolve(app_2048.chrome, pocketui_display_geometry()->width > pocketui_display_geometry()->height, false))

extern const struct pocketos_app app_2048;

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

/* ---- the shell's side of app.h ------------------------------------------ */

static int reduced_motion;
static int keyboard_requests;
static int other_shell_calls;

void pocketos_shell_set_status_hint(const char *text)
{
    (void)text;
    other_shell_calls++;
}
void pocketos_shell_go_home(void) { other_shell_calls++; }
int pocketos_shell_reduced_motion(void) { return reduced_motion; }
int64_t pocketos_shell_system_day(void)
{
    other_shell_calls++;
    return -1;
}
const char *pocketos_shell_radio_state(void)
{
    other_shell_calls++;
    return NULL;
}
void pocketos_shell_keyboard_show(enum pocketos_kb_return ret, void (*on_done)(void *user), void *user)
{
    (void)ret;
    (void)on_done;
    (void)user;
    keyboard_requests++;
}
void pocketos_shell_keyboard_hide(void) { keyboard_requests++; }
int pocketos_shell_keyboard_visible(void) { return 0; }

/* ---- display and finger ------------------------------------------------ */

static uint8_t draw_buf[PANEL_W * 40 * 4];
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
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

static void push_key(pos_key_t key)
{
    pos_input_push_key(key);
    drain();
}

static lv_point_t centre_of(lv_obj_t *obj)
{
    lv_area_t a;
    lv_point_t p;

    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    p.x = a.x1 + lv_area_get_width(&a) / 2;
    p.y = a.y1 + lv_area_get_height(&a) / 2;
    return p;
}

static void tap_obj(lv_obj_t *obj)
{
    if (!obj) {
        check("tap on a missing object", 0);
        return;
    }
    finger_point = centre_of(obj);
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}

/* A finger that goes down at `from` and travels (dx, dy) in steps, the way a
 * thumb reports: several frames on the way, then lifted. */
static void drag(lv_point_t from, int dx, int dy)
{
    int i;

    finger_point = from;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(40);
    for (i = 1; i <= 6; i++) {
        finger_point.x = from.x + dx * i / 6;
        finger_point.y = from.y + dy * i / 6;
        pump(20);
    }
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(40);
}

/* ---- the app, hosted the way the shell hosts it ------------------------ */

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
    app_priv = app_2048.create(app_body);
    pump(60);
}

static void app_stop(void)
{
    app_2048.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(60);
}

static void settle(void)
{
    pump(G2048_MOTION_MS + 60);
}

static const struct g2048_game *game(void)
{
    return g2048_app_game(app_priv);
}

static lv_obj_t *sink(void)
{
    return app_body ? lv_obj_get_child(app_body, 0) : NULL;
}

static const char *caption(void)
{
    lv_obj_t *c = g2048_app_caption(app_priv);

    return c ? lv_label_get_text(c) : "(missing)";
}

static int button_shows(int index, const char *text)
{
    lv_obj_t *b = g2048_app_button(app_priv, index);

    if (!b) {
        return 0;
    }
    if (!text) {
        return lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN);
    }
    return !lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN) &&
           strcmp(lv_label_get_text(lv_obj_get_child(b, 0)), text) == 0;
}

/* The first direction that would change the board, or -1. */
static int movable_dir(const struct g2048_game *g)
{
    int d;

    for (d = 0; d < G2048_DIR_COUNT; d++) {
        if (g2048_can_move_dir(g, (enum g2048_dir)d)) {
            return d;
        }
    }
    return -1;
}

static const pos_key_t arrow[G2048_DIR_COUNT] = { LV_KEY_UP, LV_KEY_DOWN, LV_KEY_LEFT, LV_KEY_RIGHT };
static const pos_key_t letter[G2048_DIR_COUNT] = { 'w', 's', 'a', 'd' };

/* Make one legal move with an arrow key; if the game has ended, deal a new
 * one first (Enter on the end panel). Returns the direction moved. */
static int play_one(void)
{
    int d = movable_dir(game());

    if (d < 0) {
        push_key(LV_KEY_ENTER);
        d = movable_dir(game());
    }
    push_key(arrow[d < 0 ? 0 : d]);
    return d;
}

static int file_size(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? (int)st.st_size : -1;
}

/* ---- the glass ---------------------------------------------------------------- */

static int inside(const lv_area_t *in, const lv_area_t *out)
{
    return in->x1 >= out->x1 && in->x2 <= out->x2 && in->y1 >= out->y1 && in->y2 <= out->y2;
}

static int overlap(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

/* Whether a value label's text is no wider than the panel holding it. */
static int value_fits(lv_obj_t *label)
{
    lv_obj_t *panel = lv_obj_get_parent(label);
    lv_point_t size;

    lv_obj_update_layout(panel);
    lv_text_get_size(&size, lv_label_get_text(label), lv_obj_get_style_text_font(label, 0),
                     lv_obj_get_style_text_letter_space(label, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x <= lv_obj_get_content_width(panel);
}

static void check_layout(const char *mode)
{
    lv_obj_t *root = sink();
    lv_obj_t *hud = lv_obj_get_child(root, 0);
    lv_obj_t *board = g2048_app_board(app_priv);
    lv_obj_t *controls = lv_obj_get_child(root, 2);
    lv_area_t body;
    lv_area_t screen = { 0, STATUS_H, PANEL_W - 1, PANEL_H - 1 };
    lv_area_t h;
    lv_area_t b;
    lv_area_t c;
    char what[160];
    int i;

    lv_obj_update_layout(app_body);
    lv_obj_get_coords(app_body, &body);
    body.x1 += POCKETUI_PAD;
    body.x2 -= POCKETUI_PAD;
    body.y1 += POCKETUI_BODY_PAD_TOP;
    body.y2 -= POCKETUI_PAD;
    lv_obj_get_coords(hud, &h);
    lv_obj_get_coords(board, &b);
    lv_obj_get_coords(controls, &c);
    snprintf(what, sizeof(what), "%s: HUD, board and controls are inside the body", mode);
    check(what, inside(&h, &body) && inside(&b, &body) && inside(&c, &body) && inside(&body, &screen));
    snprintf(what, sizeof(what), "%s: and do not overlap", mode);
    check(what, !overlap(&h, &b) && !overlap(&b, &c) && !overlap(&h, &c));
    snprintf(what, sizeof(what), "%s: the board is square and spans the body", mode);
    check(what, lv_area_get_width(&b) == lv_area_get_height(&b) && lv_area_get_width(&b) == 528);
    snprintf(what, sizeof(what), "%s: nothing scrolls", mode);
    check(what, lv_obj_get_scroll_bottom(app_body) <= 0 && lv_obj_get_scroll_top(app_body) == 0);
    {
        int x1 = LV_COORD_MAX;
        int x2 = -1;

        lv_obj_update_layout(controls);
        for (i = 0; i < 2; i++) {
            lv_obj_t *btn = g2048_app_button(app_priv, i);
            lv_area_t a;

            if (!lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_get_coords(btn, &a);
                x1 = a.x1 < x1 ? a.x1 : x1;
                x2 = a.x2 > x2 ? a.x2 : x2;
            }
        }
        snprintf(what, sizeof(what), "%s: the buttons span the controls edge to edge", mode);
        check(what, x1 == c.x1 && x2 == c.x2);
    }
    for (i = 0; i < 2; i++) {
        lv_obj_t *btn = g2048_app_button(app_priv, i);
        lv_area_t a;

        if (lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_obj_get_coords(btn, &a);
        snprintf(what, sizeof(what), "%s: button %d meets the 64 px minimum and is on screen", mode, i);
        check(what, lv_area_get_height(&a) >= POCKETUI_TOUCH_MIN && lv_area_get_width(&a) >= POCKETUI_TOUCH_MIN &&
                        inside(&a, &c));
        snprintf(what, sizeof(what), "%s: button %d label is whole", mode, i);
        {
            lv_area_t l;

            lv_obj_get_coords(lv_obj_get_child(btn, 0), &l);
            check(what, inside(&l, &a));
        }
    }
    {
        lv_obj_t *score = lv_obj_get_child(lv_obj_get_child(hud, 0), 1);
        lv_obj_t *best = lv_obj_get_child(lv_obj_get_child(hud, 1), 1);
        lv_area_t p;
        lv_area_t v;

        lv_obj_get_coords(lv_obj_get_child(hud, 0), &p);
        lv_obj_get_coords(score, &v);
        snprintf(what, sizeof(what), "%s: the score is drawn whole inside its panel", mode);
        check(what, value_fits(score) && inside(&v, &p));
        snprintf(what, sizeof(what), "%s: and so is the best", mode);
        check(what, value_fits(best));
        lv_obj_get_coords(g2048_app_caption(app_priv), &v);
        snprintf(what, sizeof(what), "%s: the caption sits above the buttons", mode);
        check(what, inside(&v, &c));
    }
}

/* ---- cases --------------------------------------------------------------------- */

static char state_dir[] = "/tmp/g2048_app_state.XXXXXX";


/* ---- landscape (DS §21) --------------------------------------------------------- */

static lv_display_t *g_disp;

static int32_t status_h(void)
{
    return STATUS_H;
}

#include "games_frame.h"

/* The shell turned to landscape: the app opens in a wide body, its screen
 * fits it with every button whole, keys and a swipe still play, and turning
 * back with the app open lays it out for portrait again. */
static void test_landscape(void)
{
    lv_area_t board;
    lv_area_t body;
    int32_t bw;
    int32_t bh;
    int d;

    games_use_display(g_disp, g_content, POS_ROTATION_270, status_h);
    pump(60);
    app_start();
    lv_obj_update_layout(app_body);
    lv_obj_get_coords(app_body, &body);
    check("landscape: the body is wider than tall", lv_area_get_width(&body) > lv_area_get_height(&body));
    check("landscape: the screen fits the body, every button whole",
          games_screen_fits(app_body, app_body, "2048 landscape") == 0);
    lv_obj_get_coords(g2048_app_board(app_priv), &board);
    bw = lv_area_get_width(&board);
    bh = lv_area_get_height(&board);
    check("landscape: the board is square and a tile is a touch target",
          bw - bh <= 1 && bh - bw <= 1 && bw / G2048_SIDE >= POCKETUI_TOUCH_MIN);
    check("landscape: the board is beside the HUD, not under it",
          lv_obj_get_x(g2048_app_board(app_priv)) != lv_obj_get_x(lv_obj_get_child(sink(), 0)));
    {
        uint32_t moves = game()->moves;

        play_one();
        settle();
        check("landscape: an arrow key plays", game()->moves == moves + 1);
    }
    {
        static const int vec[G2048_DIR_COUNT][2] = { { 0, -160 }, { 0, 160 }, { -160, 0 }, { 160, 0 } };
        uint32_t moves = game()->moves;

        d = movable_dir(game());
        if (d >= 0) {
            drag(centre_of(g2048_app_board(app_priv)), vec[d][0], vec[d][1]);
            settle();
        }
        check("landscape: a swipe plays", d < 0 || game()->moves == moves + 1);
    }
    push_key('n'); /* the question: two buttons */
    check("landscape: the new-game question fits, both buttons whole",
          games_screen_fits(app_body, app_body, "2048 landscape, question") == 0);
    push_key('n');
    games_use_display(g_disp, g_content, POS_ROTATION_0, status_h);
    pump(60);
    check("turned back to portrait with the app open, the screen fits again",
          games_screen_fits(app_body, app_body, "2048 portrait again") == 0);
    check_layout("portrait again");
    app_stop();
    check("landscape: nothing left behind", lv_obj_get_child_count(g_content) == 0u &&
                                               pos_input_focused() == NULL && lv_anim_count_running() == 0);
}

static void test_open(void)
{
    app_start();
    check("the app returns its state", app_priv != NULL);
    check("its id", strcmp(app_2048.id, "2048") == 0);
    check("its launcher name", strcmp(app_2048.name, "2048") == 0);
    check("an icon", app_2048.icon && app_2048.icon[0]);
    check("a tick, which is where saves happen", app_2048.tick != NULL);
    check("a new game has two tiles and no score", g2048_tile_count(game()) == 2 && game()->score == 0);
    check("the root is the focused object", pos_input_focused() == sink());
    check("the play panel shows its hint", strcmp(caption(), "SWIPE OR USE THE ARROW KEYS") == 0);
    check("and one secondary NEW GAME button", button_shows(0, NULL) && button_shows(1, "NEW GAME"));
    app_2048.tick(app_priv);
    check("opening and ticking an untouched game writes nothing", file_size(g2048_store_path()) == -1);
}

static void test_keys(void)
{
    int d;
    int i;

    for (i = 0; i < 2; i++) {
        const pos_key_t *keys = i == 0 ? arrow : letter;

        for (d = 0; d < G2048_DIR_COUNT; d++) {
            struct g2048_game expect = *game();
            int could = g2048_can_move_dir(&expect, (enum g2048_dir)d);
            char what[96];

            push_key(keys[d]);
            settle();
            if (could) {
                g2048_move(&expect, (enum g2048_dir)d, NULL);
            }
            snprintf(what, sizeof(what), "%s key %d does exactly what the rules say",
                     i == 0 ? "arrow" : "letter", d);
            check(what, memcmp(&expect, game(), sizeof(expect)) == 0);
        }
    }
    /* Make sure there is progress, then ask to start again. */
    for (i = 0; i < 20 && game()->moves == 0; i++) {
        d = movable_dir(game());
        push_key(arrow[d]);
    }
    settle();
    check("the board has progress", g2048_view_has_progress(game()));
    {
        struct g2048_game before = *game();

        push_key('n');
        check("n asks before throwing a board away", g2048_app_confirming(app_priv) &&
                                                         strstr(caption(), "START A NEW GAME?") != NULL);
        check("with KEEP PLAYING first, in the accent, and NEW GAME second",
              button_shows(0, "KEEP PLAYING") && button_shows(1, "NEW GAME"));
        push_key(LV_KEY_LEFT);
        push_key(LV_KEY_UP);
        push_key('d');
        settle();
        check("no move key slides the board under the question", memcmp(&before, game(), sizeof(before)) == 0);
        push_key(LV_KEY_ENTER);
        check("Enter keeps playing", !g2048_app_confirming(app_priv) && memcmp(&before, game(), sizeof(before)) == 0);
        push_key('N');
        push_key(LV_KEY_ESC);
        check("Esc keeps playing too", !g2048_app_confirming(app_priv));
        push_key('n');
        push_key('n');
        check("n twice starts a new game", game()->moves == 0 && game()->score == 0 &&
                                               g2048_tile_count(game()) == 2 && !g2048_app_confirming(app_priv));
        check("keeping the best", game()->best == before.best);
        push_key('n');
        check("n on an untouched board just deals again, without asking",
              !g2048_app_confirming(app_priv) && game()->moves == 0);
    }
    push_key(' ');
    push_key('q');
    push_key(LV_KEY_NEXT);
    push_key(0xE6);
    check("unmapped keys leave the focus on the root", pos_input_focused() == sink());
}

static void test_fingers(void)
{
    lv_obj_t *board = g2048_app_board(app_priv);
    lv_point_t mid = centre_of(board);
    struct g2048_game expect;
    int d;
    int tries;

    static const int vec[G2048_DIR_COUNT][2] = { { 0, -200 }, { 0, 200 }, { -200, 0 }, { 200, 0 } };

    for (tries = 0; tries < 8; tries++) {
        for (d = 0; d < G2048_DIR_COUNT; d++) {
            char what[96];
            int could;

            expect = *game();
            could = g2048_can_move_dir(&expect, (enum g2048_dir)d);
            drag(mid, vec[d][0], vec[d][1]);
            settle();
            if (could) {
                g2048_move(&expect, (enum g2048_dir)d, NULL);
            }
            snprintf(what, sizeof(what), "a swipe in direction %d does what the rules say", d);
            check(what, memcmp(&expect, game(), sizeof(expect)) == 0);
        }
    }
    check("swiping moved the game along", game()->moves > 0);

    expect = *game();
    drag(mid, 24, 6);
    drag(mid, -20, 30);
    settle();
    check("a short wobble is not a move", memcmp(&expect, game(), sizeof(expect)) == 0);
    drag(mid, 180, 170);
    settle();
    check("a diagonal smear is not a guess", memcmp(&expect, game(), sizeof(expect)) == 0);

    /* A swipe that starts over the HUD, above the board, still counts. */
    d = movable_dir(game());
    if (d >= 0) {
        lv_obj_t *hud = lv_obj_get_child(sink(), 0);
        lv_point_t top = centre_of(hud);

        expect = *game();
        drag(top, vec[d][0] / 2, vec[d][1] / 2);
        settle();
        g2048_move(&expect, (enum g2048_dir)d, NULL);
        check("a swipe that starts on the HUD moves the board", memcmp(&expect, game(), sizeof(expect)) == 0);
    }

    /* The buttons do what the keys do, and never take the focus. */
    tap_obj(g2048_app_button(app_priv, 1));
    check("tapping NEW GAME with progress asks first", g2048_app_confirming(app_priv));
    check("the tap left the focus on the root", pos_input_focused() == sink());
    expect = *game();
    tap_obj(g2048_app_button(app_priv, 0));
    check("tapping KEEP PLAYING keeps the board", !g2048_app_confirming(app_priv) &&
                                                      memcmp(&expect, game(), sizeof(expect)) == 0);
    tap_obj(g2048_app_button(app_priv, 1));
    d = movable_dir(game());
    drag(mid, vec[d < 0 ? 0 : d][0], vec[d < 0 ? 0 : d][1]);
    settle();
    check("a swipe under the question does nothing", memcmp(&expect, game(), sizeof(expect)) == 0 &&
                                                         g2048_app_confirming(app_priv));
    tap_obj(g2048_app_button(app_priv, 1));
    check("tapping NEW GAME in the question starts again", game()->moves == 0 && !g2048_app_confirming(app_priv));
    check("and the focus is still the root", pos_input_focused() == sink());
}

static void test_motion(void)
{
    int d;
    struct g2048_game expect;

    reduced_motion = 0;
    push_key('n');
    push_key('n');
    play_one();
    check("a move animates", g2048_board_animating(g2048_app_board(app_priv)) && lv_anim_count_running() > 0);
    settle();
    check("and settles", !g2048_board_animating(g2048_app_board(app_priv)) && lv_anim_count_running() == 0);

    d = movable_dir(game());
    expect = *game();
    pos_input_push_key(arrow[d]);
    drain(); /* delivered, and the motion started, but far from over */
    check("the first move is still in motion", g2048_board_animating(g2048_app_board(app_priv)));
    g2048_move(&expect, (enum g2048_dir)d, NULL);
    d = movable_dir(game());
    if (d >= 0) {
        g2048_move(&expect, (enum g2048_dir)d, NULL);
        push_key(arrow[d]);
    }
    settle();
    check("a key during the motion finishes it and moves again", memcmp(&expect, game(), sizeof(expect)) == 0);

    reduced_motion = 1;
    play_one();
    check("with reduced motion nothing animates", !g2048_board_animating(g2048_app_board(app_priv)) &&
                                                      lv_anim_count_running() == 0);
    reduced_motion = 0;
}

static void test_save_resume(void)
{
    struct g2048_game kept;
    int d;
    int i;

    push_key('n');
    push_key('n');
    unlink(g2048_store_path());
    for (i = 0; i < 12; i++) {
        play_one();
    }
    settle();
    check("an ordinary move does not write; the card is not touched per swipe",
          file_size(g2048_store_path()) == -1);
    app_2048.tick(app_priv);
    check("the tick saves it: 43 bytes", file_size(g2048_store_path()) == G2048_SAVE_SIZE);
    play_one();
    settle();
    kept = *game();
    app_stop();
    {
        struct g2048_game on_disk;

        check("closing saves the last move", g2048_store_load(&on_disk) == 0 &&
                                                 memcmp(&on_disk, &kept, sizeof(kept)) == 0);
    }
    app_start();
    check("opening again resumes the same game exactly", memcmp(game(), &kept, sizeof(kept)) == 0);
    for (i = 0; i < 40; i++) {
        d = movable_dir(game());
        if (d < 0) {
            break;
        }
        g2048_move(&kept, (enum g2048_dir)d, NULL);
        push_key(letter[d]);
    }
    settle();
    check("and it plays on exactly as the original would, new tiles included",
          memcmp(game(), &kept, sizeof(kept)) == 0);
    check("the resumed root has the focus", pos_input_focused() == sink());
    app_stop();
}

/* A game one LEFT away from the end (tests/g2048_rules_test.c explains the
 * board): find a seed whose new tile ends it, store it, and play that move
 * through the app. */
static void test_end(void)
{
    static const uint8_t almost[G2048_CELLS] = { 1, 1, 6, 8, 3, 4, 5, 2, 4, 5, 7, 9, 5, 7, 9, 10 };
    struct g2048_game g;
    uint32_t seed;
    int found = 0;

    for (seed = 1; seed < 500 && !found; seed++) {
        struct g2048_game probe;

        memset(&probe, 0, sizeof(probe));
        memcpy(probe.cell, almost, sizeof(almost));
        probe.moves = 50;
        probe.score = 1000;
        probe.best = 3000;
        g2048_rng_seed(&probe.rng, seed);
        g = probe;
        if (g2048_move(&probe, G2048_LEFT, NULL) && probe.over) {
            found = 1;
        }
    }
    check("a seed that ends the stored game exists", found);
    check("the stored game is valid", g2048_game_valid(&g));
    check("and is written", g2048_store_save(&g) == 0);
    app_start();
    check("it resumes", memcmp(game(), &g, sizeof(g)) == 0);
    push_key(LV_KEY_LEFT);
    check("the last move ends it", game()->over);
    check("the controls say so", strcmp(caption(), "NO MOVES LEFT") == 0 &&
                                     button_shows(0, "NEW GAME") && button_shows(1, NULL));
    {
        struct g2048_game on_disk;

        check("an ended game is saved at once, not on the next tick",
              g2048_store_load(&on_disk) == 0 && on_disk.over);
    }
    settle();
    check("the board is still there to look at", g2048_tile_count(game()) == G2048_CELLS);
    push_key(LV_KEY_ESC);
    check("Esc does not throw it away", game()->over);
    push_key(LV_KEY_ENTER);
    check("Enter deals a new game", !game()->over && game()->moves == 0 && g2048_tile_count(game()) == 2);
    check("with the best score carried over", game()->best == 3000);
    app_stop();

    /* A finished game on disk is not resumed: its best survives. */
    g2048_store_save(&g);
    app_start();
    push_key(LV_KEY_LEFT);
    app_stop();
    app_start();
    check("a finished game is not resumed", !game()->over && game()->moves == 0);
    check("but its best is", game()->best == 3000);
    app_stop();
}

static void test_won(void)
{
    static const uint8_t near[G2048_CELLS] = { 10, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2 };
    struct g2048_game g;

    memset(&g, 0, sizeof(g));
    memcpy(g.cell, near, sizeof(near));
    g.moves = 400;
    g.score = 9000;
    g.best = 9000;
    g2048_rng_seed(&g.rng, 5);
    g2048_store_save(&g);
    app_start();
    push_key(LV_KEY_LEFT);
    check("making 2048 shows the goal", g2048_state_of(game()) == G2048_WON &&
                                            strcmp(caption(), "2048 \xC2\xB7 YOU MADE IT") == 0);
    check("with KEEP GOING first and NEW GAME second", button_shows(0, "KEEP GOING") && button_shows(1, "NEW GAME"));
    {
        struct g2048_game on_disk;

        check("the goal is saved at once", g2048_store_load(&on_disk) == 0 && on_disk.won && !on_disk.keep_going);
    }
    push_key(LV_KEY_DOWN);
    settle();
    check("the board waits for an answer", game()->moves == 401);
    tap_obj(g2048_app_button(app_priv, 0));
    check("KEEP GOING continues", g2048_state_of(game()) == G2048_PLAYING && game()->keep_going);
    push_key(LV_KEY_DOWN);
    settle();
    check("and the board moves again", game()->moves == 402);
    app_stop();
    app_start();
    check("a game that kept going resumes as one", game()->won && game()->keep_going &&
                                                       g2048_state_of(game()) == G2048_PLAYING);
    app_stop();
}

static void test_damaged(void)
{
    FILE *f = fopen(g2048_store_path(), "wb");
    int d;

    if (f) {
        fputs("this is not a saved game", f);
        fclose(f);
    }
    app_start();
    check("a damaged save starts a new game", game()->moves == 0 && g2048_tile_count(game()) == 2);
    check("and says so", strcmp(caption(), "SAVED GAME UNREADABLE \xC2\xB7 NEW GAME") == 0);
    app_2048.tick(app_priv);
    check("the damaged file is left alone until there is something to save",
          file_size(g2048_store_path()) == (int)strlen("this is not a saved game"));
    d = movable_dir(game());
    push_key(arrow[d]);
    check("the note goes once the new game is played", strcmp(caption(), "SWIPE OR USE THE ARROW KEYS") == 0);
    app_2048.tick(app_priv);
    {
        struct g2048_game on_disk;

        check("and the next save replaces the file", g2048_store_load(&on_disk) == 0 && on_disk.moves == 1);
    }
    app_stop();
}

static void test_glass(void)
{
    static const char *const modes[] = { "normal", "outdoor", "night" };
    char why[128];
    int t;
    int m;

    app_start();
    for (m = 0; m < 3; m++) {
        check("the mode applies", pos_theme_apply(NULL, modes[m], why, sizeof(why)) == 0);
        pump(60);
        check_layout(modes[m]);
        play_one();
        push_key('n'); /* the question: two buttons */
        check_layout(modes[m]);
        push_key('n');
        check_layout(modes[m]);
    }
    for (t = 0; t < pos_theme_count(); t++) {
        char what[96];

        pos_theme_apply(pos_theme_at(t)->id, "normal", why, sizeof(why));
        pump(60);
        push_key(arrow[movable_dir(game()) < 0 ? 0 : movable_dir(game())]);
        settle();
        snprintf(what, sizeof(what), "theme %s: the board draws and the app still plays", pos_theme_at(t)->id);
        check(what, lv_obj_is_valid(g2048_app_board(app_priv)));
    }
    pos_theme_apply("ice", "normal", why, sizeof(why));
    /* A seven-digit score still fits its panel. */
    {
        struct g2048_game big = *game();

        big.score = 3932156;
        big.best = 3932156;
        big.moves = 9999;
        app_stop();
        g2048_store_save(&big);
        app_start();
        check("a seven-digit score is drawn whole",
              value_fits(lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(sink(), 0), 0), 1)));
        check_layout("normal, seven-digit score");
    }
    app_stop();
}

static void test_rounds(void)
{
    int i;

    for (i = 0; i < 5; i++) {
        int d;

        app_start();
        d = movable_dir(game());
        if (d >= 0) {
            push_key(arrow[d]);
        }
        pump(30); /* close mid-animation */
        app_stop();
    }
    check("five rounds leave nothing behind", lv_obj_get_child_count(g_content) == 0u);
    check("and no focusable object", pos_input_focused() == NULL);
    check("and no animation", lv_anim_count_running() == 0);
}

static void test_debug_screen(void)
{
    char other[] = "/tmp/g2048_app_debug.XXXXXX";
    char path[256];

    if (!mkdtemp(other)) {
        check("temporary directory", 0);
        return;
    }
    setenv("POCKETOS_STATE_DIR", other, 1);
    setenv("PG2048_SCREEN", "won", 1);
    app_start();
    check("the review screen for the goal shows it", g2048_state_of(game()) == G2048_WON);
    push_key(LV_KEY_ENTER);
    app_2048.tick(app_priv);
    app_stop();
    unsetenv("PG2048_SCREEN");
    snprintf(path, sizeof(path), "%s/2048", other);
    check("a review screen never writes the save file", access(path, F_OK) != 0);
    rmdir(other);
    setenv("POCKETOS_STATE_DIR", state_dir, 1);
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
    unsetenv("PG2048_SCREEN");

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
    test_fingers();
    test_motion();
    test_save_resume();
    test_end();
    test_won();
    test_damaged();
    test_glass();
    test_rounds();
    test_debug_screen();
    test_landscape();

    check("no keyboard was asked for", keyboard_requests == 0);
    check("no other shell service was called", other_shell_calls == 0);
    {
        char cmd[128];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", state_dir);
        if (system(cmd) != 0) {
            printf("note: could not remove %s\n", state_dir);
        }
    }
    printf("g2048_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
