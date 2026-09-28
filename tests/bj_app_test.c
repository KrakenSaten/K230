/*
 * PG Blackjack in the running app: rounds from a stacked shoe played through
 * the real key stream and real taps on the buttons, against a copy of the
 * game driven by the rules directly, so every key and tap is checked to do
 * exactly what the rules say.
 *
 * Also: the buttons change with the phase and never take the focus, a
 * disabled DOUBLE does nothing, running out of chips offers a new bankroll,
 * the HUD shows BANK and BET as the game holds them, the screen fits in every
 * mode and theme with the longest hand inside the table, and the app leaves
 * nothing behind.
 *
 * Persistence: a new session nobody touches writes nothing; the tick saves a
 * change and a settled round is saved at once; leaving in the middle of a
 * hand and reopening resumes it exactly, chips and cards, and it plays on per
 * the rules; a double, out of chips and a new bankroll survive reopening; a
 * damaged save opens a new bankroll, says so, and is replaced after the first
 * command; a failed save says so while play continues; the review states
 * never touch the file.
 *
 * The shell is not linked; this file hosts the app as ui/shell/shell.c does.
 * Run by tests/bj_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "bj_app.h"
#include "bj_store.h"
#include "bj_table_widget.h"
#include "pocketui.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
#define STATUS_H POCKETUI_STATUS_BAR_H
#define MINUS "\xE2\x88\x92"
#define DOT "\xC2\xB7"
#define BET_DOWN "BET " MINUS "10"
#define BET_UP "BET +10"

static char state_dir[] = "/tmp/bj_app_state.XXXXXX";

extern const struct pocketos_app app_blackjack;

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

/* ---- app.h ------------------------------------------------------------------------ */

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

/* ---- display, finger, keys ----------------------------------------------------------- */

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

static void key(pos_key_t k)
{
    int t;

    pos_input_push_key(k);
    for (t = 0; t < 1000 && pos_input_queued() > 0; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
    pump(40);
}

static void tap_obj(lv_obj_t *obj)
{
    lv_area_t a;

    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(50);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(50);
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
    app_priv = app_blackjack.create(app_body);
    pump(60);
}

static void app_stop(void)
{
    app_blackjack.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(60);
}

static struct bj_game *game(void) { return bj_app_game(app_priv); }
static lv_obj_t *sink(void) { return app_body ? lv_obj_get_child(app_body, 0) : NULL; }
static const char *caption(void) { return lv_label_get_text(bj_app_caption(app_priv)); }
static const char *value(int i) { return lv_label_get_text(bj_app_value(app_priv, i)); }

static int button_is(int i, const char *text, int enabled)
{
    lv_obj_t *b = bj_app_button(app_priv, i);

    if (!text) {
        return lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN);
    }
    return !lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN) && strcmp(lv_label_get_text(lv_obj_get_child(b, 0)), text) == 0 &&
           (enabled < 0 || lv_obj_has_state(b, LV_STATE_DISABLED) == !enabled);
}

/* Ranks dealt in order onto the top of the app's shoe. The shoe becomes every
 * card not on the table, each at most twice, so the game stays a position
 * the rules can reach and saves like one. */
static void stack(struct bj_game *g, ...)
{
    uint8_t left[BJ_DECK];
    bj_card_t picked[40];
    int k = 0;
    int n = 0;
    int c;
    int i;
    va_list ap;

    memset(left, BJ_DECKS, sizeof(left));
    for (i = 0; i < g->player.n; i++) {
        left[g->player.card[i]]--;
    }
    for (i = 0; i < g->dealer.n; i++) {
        left[g->dealer.card[i]]--;
    }
    va_start(ap, g);
    for (;;) {
        int r = va_arg(ap, int);
        int s;

        if (r == 0) {
            break;
        }
        for (s = 0; s < BJ_SUITS && !left[bj_card(r, (enum bj_suit)s)]; s++) {
        }
        picked[k] = bj_card(r, (enum bj_suit)(s % BJ_SUITS));
        left[picked[k++]]--;
    }
    va_end(ap);
    for (c = 0; c < BJ_DECK; c++) {
        for (i = 0; i < left[c]; i++) {
            g->shoe.card[n++] = (bj_card_t)c;
        }
    }
    for (i = k - 1; i >= 0; i--) {
        g->shoe.card[n++] = picked[i];
    }
    g->shoe.n = (uint8_t)n;
}

/* The same session, field by field and live card by live card. */
static int same_game(const struct bj_game *a, const struct bj_game *b)
{
    return a->bankroll == b->bankroll && a->bet == b->bet && a->stake == b->stake && a->last_delta == b->last_delta &&
           a->rounds == b->rounds && a->phase == b->phase && a->outcome == b->outcome && a->doubled == b->doubled &&
           a->reshuffled == b->reshuffled && a->shoe.rng.state == b->shoe.rng.state &&
           a->shoe.shuffles == b->shoe.shuffles && a->shoe.n == b->shoe.n &&
           memcmp(a->shoe.card, b->shoe.card, a->shoe.n) == 0 && a->player.n == b->player.n &&
           memcmp(a->player.card, b->player.card, a->player.n) == 0 && a->dealer.n == b->dealer.n &&
           memcmp(a->dealer.card, b->dealer.card, a->dealer.n) == 0;
}

static long file_size(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int read_file(const char *path, uint8_t *buf, size_t n)
{
    FILE *f = fopen(path, "rb");
    size_t got;

    if (!f) {
        return -1;
    }
    got = fread(buf, 1, n, f);
    fclose(f);
    return (int)got;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void tick(void)
{
    app_blackjack.tick(app_priv);
    pump(20);
}

static int entries(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        n += strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    }
    closedir(d);
    return n;
}

/* ---- cases ------------------------------------------------------------------------------ */

static void test_open(void)
{
    app_start();
    check("the app returns its state", app_priv != NULL);
    check("id and launcher name", strcmp(app_blackjack.id, "blackjack") == 0 &&
                                      strcmp(app_blackjack.name, "Blackjack") == 0 && app_blackjack.icon);
    check("a tick, for saving", app_blackjack.tick != NULL);
    check("it opens between rounds with a full bankroll", game()->phase == BJ_BETTING && game()->bankroll == 1000 &&
                                                              game()->bet == 10 && game()->player.n == 0);
    check_str("the HUD's left panel says BANK", lv_label_get_text(bj_app_hud_label(app_priv, 0)), "BANK");
    check_str("and shows the bankroll", value(0), "1000");
    check_str("its right panel says BET", lv_label_get_text(bj_app_hud_label(app_priv, 1)), "BET");
    check_str("and shows the bet", value(1), "10");
    check("the numbers are the large ones", lv_obj_get_style_text_font(bj_app_value(app_priv, 0), 0) ==
                                                lv_obj_get_style_text_font(bj_app_value(app_priv, 1), 0) &&
                                                lv_font_get_line_height(lv_obj_get_style_text_font(
                                                    bj_app_value(app_priv, 0), 0)) >= 40);
    check_str("the caption says what to do", caption(), "SET YOUR BET, THEN DEAL");
    check("DEAL, BET -10 (off at the minimum) and BET +10", button_is(0, "DEAL", 1) && button_is(1, BET_DOWN, 0) &&
                                                               button_is(2, BET_UP, 1));
    check("the root has the focus", pos_input_focused() == sink());
}

static void test_betting(void)
{
    key(LV_KEY_RIGHT);
    key(LV_KEY_UP);
    key('+');
    check_str("Right, Up and + raise the bet by 10 each", value(1), "40");
    key(LV_KEY_LEFT);
    key('-');
    check_str("Left and - lower it", value(1), "20");
    tap_obj(bj_app_button(app_priv, 2));
    check_str("tapping + raises it", value(1), "30");
    check("and leaves the focus on the root", pos_input_focused() == sink());
    tap_obj(bj_app_button(app_priv, 1));
    tap_obj(bj_app_button(app_priv, 1));
    check_str("tapping minus lowers it", value(1), "10");
    key(LV_KEY_DOWN);
    check_str("below the minimum the caption says why", caption(), "THE MINIMUM BET IS 10");
    tap_obj(bj_app_button(app_priv, 1));
    check("a disabled minus does nothing", game()->bet == 10);
}

static void test_rounds(void)
{
    struct bj_game expect;

    /* Keys: deal, hit, stand. */
    stack(game(), 5, 10, 6, 7, 4, 0); /* player 11, dealer 17; hit 4 = 15 */
    expect = *game();
    key(LV_KEY_ENTER);
    bj_deal(&expect);
    check("Enter deals exactly as the rules do", memcmp(&expect, game(), sizeof(expect)) == 0);
    check("HIT, STAND and DOUBLE", button_is(0, "HIT", 1) && button_is(1, "STAND", 1) && button_is(2, "DOUBLE", 1));
    check_str("the bankroll shows the bet taken", value(0), "990");
    check_str("the play caption", caption(), "YOU HAVE 11 " DOT " HIT OR STAND");
    key(LV_KEY_LEFT);
    key('n');
    check("bet keys do nothing mid-hand", memcmp(&expect, game(), sizeof(expect)) == 0);
    key('h');
    bj_hit(&expect);
    check("h hits exactly as the rules do", memcmp(&expect, game(), sizeof(expect)) == 0 && game()->player.n == 3);
    check("DOUBLE is shown but off after a hit", button_is(2, "DOUBLE", 0));
    tap_obj(bj_app_button(app_priv, 2));
    check("tapping it does nothing", memcmp(&expect, game(), sizeof(expect)) == 0);
    key('d');
    check_str("pressing d says why", caption(), "DOUBLE ONLY ON YOUR FIRST TWO CARDS");
    key('s');
    bj_stand(&expect);
    check("s stands exactly as the rules do", memcmp(&expect, game(), sizeof(expect)) == 0 && game()->phase == BJ_SETTLED);
    check_str("15 against 17 loses, in words", caption(), "DEALER WINS " DOT " " MINUS "10");
    check("NEW ROUND and the bet steps come back", button_is(0, "NEW ROUND", 1) && button_is(1, BET_DOWN, -1) &&
                                                       button_is(2, BET_UP, 1));
    check_str("the bankroll after the loss", value(0), "990");

    /* Buttons: deal, double. */
    stack(game(), 6, 10, 5, 7, 10, 0); /* 11 doubles on a 10 against 17 */
    expect = *game();
    tap_obj(bj_app_button(app_priv, 0));
    bj_deal(&expect);
    check("NEW ROUND deals", memcmp(&expect, game(), sizeof(expect)) == 0);
    tap_obj(bj_app_button(app_priv, 2));
    bj_double(&expect);
    check("tapping DOUBLE doubles exactly as the rules do", memcmp(&expect, game(), sizeof(expect)) == 0);
    check_str("and wins twice the bet", caption(), "YOU WIN " DOT " +20");
    check_str("the bankroll", value(0), "1010");
    check("the focus is still on the root", pos_input_focused() == sink());

    /* Space stands, Enter hits. */
    stack(game(), 10, 10, 2, 7, 5, 0); /* 12, hit 5 = 17 against 17 */
    key(' ');
    check("Space deals between rounds", game()->phase == BJ_PLAYER);
    key(LV_KEY_ENTER);
    check("Enter hits during a hand", game()->player.n == 3);
    key(' ');
    check("Space stands", game()->phase == BJ_SETTLED && game()->outcome == BJ_PUSH);
    check_str("a push in words", caption(), "PUSH " DOT " BET RETURNED");

    /* A natural settles on the deal. */
    stack(game(), 1, 9, 13, 7, 0);
    key('n');
    check("a blackjack settles on the deal", game()->outcome == BJ_PLAYER_BLACKJACK && game()->phase == BJ_SETTLED);
    check_str("paying 3:2", caption(), "BLACKJACK " DOT " +15");
    check("with the round-over buttons at once", button_is(0, "NEW ROUND", 1));
}

static void test_broke(void)
{
    game()->bankroll = 10;
    game()->bet = 10;
    bj_app_refresh(app_priv);
    stack(game(), 10, 10, 6, 9, 0);
    key(LV_KEY_ENTER);
    key('s');
    check("losing the last chips", game()->bankroll == 0);
    check_str("says so", caption(), "OUT OF CHIPS");
    check("and offers only a new bankroll", button_is(0, "NEW BANKROLL", 1) && button_is(1, NULL, -1) &&
                                                button_is(2, NULL, -1));
    key('h');
    key(LV_KEY_RIGHT);
    check("nothing else answers", game()->bankroll == 0 && game()->phase == BJ_SETTLED);
    tap_obj(bj_app_button(app_priv, 0));
    check("NEW BANKROLL starts again", game()->bankroll == 1000 && game()->bet == 10 && game()->phase == BJ_BETTING);
    check_str("and the HUD follows", value(0), "1000");
}

static void test_glass(void)
{
    static const char *const modes[] = { "normal", "outdoor", "night" };
    char why[128];
    char what[128];
    int m;

    stack(game(), 5, 10, 6, 7, 0);
    key(LV_KEY_ENTER);
    for (m = 0; m < 3; m++) {
        lv_obj_t *table = bj_app_table(app_priv);
        lv_obj_t *controls = lv_obj_get_child(sink(), 2);
        struct bj_table t;
        lv_area_t body;
        lv_area_t ta;
        lv_area_t ca;
        int i;

        pos_theme_apply(NULL, modes[m], why, sizeof(why));
        pump(60);
        lv_obj_update_layout(app_body);
        lv_obj_get_coords(app_body, &body);
        lv_obj_get_coords(table, &ta);
        lv_obj_get_coords(controls, &ca);
        bj_table_geometry(table, &t);
        snprintf(what, sizeof(what), "%s: table above controls, both inside the body", modes[m]);
        check(what, ta.y2 < ca.y1 && ca.y2 <= body.y2 - POCKETUI_PAD && ta.x1 >= body.x1 + POCKETUI_PAD &&
                        ta.x2 <= body.x2 - POCKETUI_PAD);
        snprintf(what, sizeof(what), "%s: nothing scrolls", modes[m]);
        check(what, lv_obj_get_scroll_bottom(app_body) <= 0 && lv_obj_get_scroll_top(app_body) == 0);
        for (i = 0; i < 3; i++) {
            lv_obj_t *b = bj_app_button(app_priv, i);
            lv_area_t ba;

            lv_obj_get_coords(b, &ba);
            snprintf(what, sizeof(what), "%s: button %d is at least 64 x 64 and inside the controls", modes[m], i);
            check(what, lv_area_get_height(&ba) >= 64 && lv_area_get_width(&ba) >= 64 && ba.x1 >= ca.x1 &&
                            ba.x2 <= ca.x2);
        }
        {
            struct bj_rect last = bj_view_card(&t, BJ_ROW_PLAYER, BJ_HAND_MAX, BJ_HAND_MAX - 1);
            struct bj_rect first = bj_view_card(&t, BJ_ROW_DEALER, BJ_HAND_MAX, 0);

            snprintf(what, sizeof(what), "%s: a 16-card hand fits across the table", modes[m]);
            check(what, first.x >= 0 && last.x + last.w <= t.w && last.y + last.h <= t.h);
        }
    }
    key('s');
    for (m = 0; m < pos_theme_count(); m++) {
        pos_theme_apply(pos_theme_at(m)->id, "normal", why, sizeof(why));
        key(LV_KEY_ENTER);
        key('s');
    }
    pos_theme_apply("ice", "normal", why, sizeof(why));
    check("every theme draws and keeps playing", game()->phase == BJ_SETTLED || game()->phase == BJ_BETTING);
}

static void test_random(void)
{
    static const pos_key_t keys[] = { LV_KEY_ENTER, ' ', 'h', 's', 'd', 'n', LV_KEY_LEFT, LV_KEY_RIGHT, '+', '-', 'q' };
    struct bj_rng rng;
    int step;
    int bad = 0;
    int unsaved = 0;
    uint32_t rounds = game()->rounds;

    bj_rng_seed(&rng, 17);
    for (step = 0; step < 3000; step++) {
        uint32_t chips = bj_chips(game());
        enum bj_phase before = game()->phase;

        if (bj_rng_below(&rng, 5) == 0) {
            tap_obj(bj_app_button(app_priv, (int)bj_rng_below(&rng, 3)));
        } else {
            key(keys[bj_rng_below(&rng, sizeof(keys) / sizeof(keys[0]))]);
        }
        if (step % 7 == 0) {
            tick();
        }
        unsaved += strncmp(caption(), "NOT SAVED", 9) == 0;
        if (game()->phase == BJ_SETTLED && before == BJ_PLAYER &&
            (int64_t)bj_chips(game()) != (int64_t)chips + game()->last_delta) {
            bad++;
        }
        if (game()->phase == BJ_PLAYER && before == BJ_PLAYER && bj_chips(game()) != chips) {
            bad++;
        }
    }
    check("3000 random keys and taps: chips are only ever won or lost by settling", bad == 0);
    check("and rounds were played", game()->rounds > rounds + 50);
    check("the focus never left the root", pos_input_focused() == sink());
    check("every position they reached could be saved", unsaved == 0);
    {
        struct bj_game disk;

        tick();
        check("and the last one is on disk", bj_store_load(&disk) == 0 && same_game(&disk, game()));
    }
}

static void test_persistence(void)
{
    struct bj_game before;
    struct bj_game disk;
    char s[32];

    /* Nothing to keep, nothing written. */
    app_stop();
    unlink(bj_store_path());
    app_start();
    check("with no save a new session opens", game()->phase == BJ_BETTING && game()->bankroll == 1000 &&
                                                  game()->rounds == 0);
    check_str("with the usual caption", caption(), "SET YOUR BET, THEN DEAL");
    tick();
    app_stop();
    check("a new session nobody touched writes nothing, ticked or closed", file_size(bj_store_path()) < 0);

    /* A change waits for the tick. */
    app_start();
    key(LV_KEY_RIGHT);
    key(LV_KEY_RIGHT);
    check("a changed bet is not written on the key", file_size(bj_store_path()) < 0);
    tick();
    check("the tick writes it: 181 bytes, bet 30", file_size(bj_store_path()) == BJ_SAVE_SIZE &&
                                                       bj_store_load(&disk) == 0 && disk.bet == 30);

    /* Leaving in the middle of a hand. */
    stack(game(), 1, 10, 2, 7, 5, 0); /* player A 2, dealer 10 7; hit 5: soft 18 */
    key(LV_KEY_ENTER);
    key('h');
    check("(a hand in play)", game()->phase == BJ_PLAYER && game()->player.n == 3 && game()->bankroll == 970);
    check("the deal and the hit wait for the tick too", bj_store_load(&disk) == 0 && disk.phase == BJ_BETTING);
    before = *game();
    app_stop();
    check("leaving saves the hand as it stands", bj_store_load(&disk) == 0 && same_game(&disk, &before));
    app_start();
    check("reopening resumes it exactly: shoe, hands, stake, bankroll", same_game(game(), &before));
    check("no chip lost or made by leaving", bj_chips(game()) == 1000 && game()->stake == 30);
    check("the hole card is still down", game()->phase == BJ_PLAYER && bj_view_face_down(game(), 1));
    check_str("BANK", value(0), "970");
    check_str("BET, on the table", value(1), "30");
    check_str("the caption picks the hand up", caption(), "YOU HAVE SOFT 18 " DOT " HIT OR STAND");
    check("HIT, STAND, and DOUBLE off after the hit", button_is(0, "HIT", 1) && button_is(1, "STAND", 1) &&
                                                          button_is(2, "DOUBLE", 0));
    check("the root has the focus", pos_input_focused() == sink());

    /* It plays on per the rules; a settled round is saved at once. */
    before = *game();
    key('s');
    bj_stand(&before);
    check("standing settles exactly as the rules do", same_game(game(), &before) &&
                                                          game()->outcome == BJ_PLAYER_WINS);
    check("paid: +30 and the stake back", game()->last_delta == 30 && game()->bankroll == 1030);
    check("the settled round is on disk without waiting for a tick", bj_store_load(&disk) == 0 &&
                                                                         same_game(&disk, game()));
    app_stop();
    app_start();
    check("reopened, the round is still settled", same_game(game(), &before));
    check_str("its result in words", caption(), "YOU WIN " DOT " +30");
    bj_view_result(game(), s, sizeof(s));
    check_str("and on the felt", s, "+30");
    check_str("BANK 1030", value(0), "1030");
    check("NEW ROUND and the bet steps", button_is(0, "NEW ROUND", 1) && button_is(1, BET_DOWN, 1) &&
                                             button_is(2, BET_UP, 1));

    /* A double, left the moment it settles. */
    stack(game(), 6, 10, 5, 7, 10, 0);
    key('n');
    key('d');
    before = *game();
    app_stop();
    app_start();
    check("a doubled win survives leaving at once", same_game(game(), &before) && game()->doubled &&
                                                        game()->stake == 60 && game()->last_delta == 60 &&
                                                        game()->bankroll == 1090);

    /* Out of chips, reopened; a new bankroll replaces the save. */
    game()->bankroll = 30;
    bj_app_refresh(app_priv);
    stack(game(), 10, 10, 6, 9, 0);
    key(LV_KEY_ENTER);
    key('s');
    check("(out of chips)", game()->bankroll == 0 && game()->phase == BJ_SETTLED);
    app_stop();
    app_start();
    check_str("reopened out of chips, it says so", caption(), "OUT OF CHIPS");
    check("and offers only a new bankroll", button_is(0, "NEW BANKROLL", 1) && button_is(1, NULL, -1));
    key(LV_KEY_ENTER);
    tick();
    check("a new bankroll replaces the save", bj_store_load(&disk) == 0 && disk.bankroll == 1000 &&
                                                  disk.phase == BJ_BETTING && disk.bet == 10);

    /* A damaged save. */
    app_stop();
    write_file(bj_store_path(), "garbage");
    app_start();
    check("a damaged save opens a new bankroll", game()->phase == BJ_BETTING && game()->bankroll == 1000 &&
                                                     game()->rounds == 0);
    check_str("and says so", caption(), "SAVED GAME UNREADABLE " DOT " NEW BANKROLL");
    tick();
    check("the damaged file is left while nothing has changed", file_size(bj_store_path()) == 7);
    key(LV_KEY_UP);
    check_str("the first command clears the note", caption(), "SET YOUR BET, THEN DEAL");
    tick();
    check("and the next save replaces the damaged file", bj_store_load(&disk) == 0 && disk.bet == 20);

    /* A save that cannot be made. */
    setenv("POCKETOS_STATE_DIR", "/dev/null/blackjack-cannot-save", 1);
    key(LV_KEY_UP);
    tick();
    check_str("a failed save says so", caption(), "NOT SAVED " DOT " PLAY CONTINUES");
    key(LV_KEY_LEFT);
    key(LV_KEY_LEFT);
    key(LV_KEY_LEFT);
    check_str("a refusal is still explained first", caption(), "THE MINIMUM BET IS 10");
    key(LV_KEY_RIGHT);
    key(LV_KEY_RIGHT);
    stack(game(), 10, 10, 9, 7, 0);
    key(LV_KEY_ENTER);
    key('s');
    check("play continues and the round settles", game()->phase == BJ_SETTLED && game()->outcome == BJ_PLAYER_WINS);
    check_str("still saying the save failed", caption(), "NOT SAVED " DOT " PLAY CONTINUES");
    setenv("POCKETOS_STATE_DIR", state_dir, 1);
    tick();
    check("once saving works again the tick catches up", bj_store_load(&disk) == 0 && same_game(&disk, game()));
    check_str("and the note goes", caption(), "YOU WIN " DOT " +30");
}

static void test_review_and_rounds(void)
{
    static const char *const screens[] = { "bet", "play", "win", "blackjack", "bust", "broke" };
    uint8_t saved_before[BJ_SAVE_SIZE + 1];
    uint8_t saved_after[BJ_SAVE_SIZE + 1];
    int n_before;
    size_t i;

    app_stop();
    n_before = read_file(bj_store_path(), saved_before, sizeof(saved_before));
    for (i = 0; i < sizeof(screens) / sizeof(screens[0]); i++) {
        char what[96];

        setenv("PGBLACKJACK_SCREEN", screens[i], 1);
        app_start();
        snprintf(what, sizeof(what), "review state %s opens", screens[i]);
        check(what, app_priv != NULL);
        if (strcmp(screens[i], "play") == 0) {
            check("the play review shows a live hand with the hole card down",
                  game()->phase == BJ_PLAYER && bj_view_face_down(game(), 1));
        } else if (strcmp(screens[i], "broke") == 0) {
            check("the broke review offers a new bankroll", button_is(0, "NEW BANKROLL", 1));
        } else if (strcmp(screens[i], "blackjack") == 0) {
            check("the blackjack review is one", game()->outcome == BJ_PLAYER_BLACKJACK);
        } else if (strcmp(screens[i], "bet") == 0) {
            check("the bet review is its own fixed session, not the saved one",
                  game()->rounds == 0 && game()->bankroll == 1000);
        }
        key(LV_KEY_ENTER);
        tick();
        app_stop();
    }
    unsetenv("PGBLACKJACK_SCREEN");
    check("the review states, played and ticked, never touched the save",
          n_before == BJ_SAVE_SIZE && read_file(bj_store_path(), saved_after, sizeof(saved_after)) == n_before &&
              memcmp(saved_before, saved_after, (size_t)n_before) == 0);
    for (i = 0; i < 5; i++) {
        app_start();
        key(LV_KEY_ENTER);
        key('h');
        app_stop();
    }
    check("five rounds leave nothing behind", lv_obj_get_child_count(g_content) == 0u);
    check("and no focusable object", pos_input_focused() == NULL);
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
    unsetenv("PGBLACKJACK_SCREEN");
    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
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
    test_betting();
    test_rounds();
    test_broke();
    test_glass();
    test_random();
    test_persistence();
    test_review_and_rounds();

    check("the app asked the shell for nothing", shell_calls == 0);
    check("and wrote only its own file, no temporary left", entries(state_dir) == 1 && entries(bj_store_dir()) == 1 &&
                                                                file_size(bj_store_path()) == BJ_SAVE_SIZE);
    {
        char cmd[128];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", state_dir);
        if (system(cmd) != 0) {
            printf("note: could not remove %s\n", state_dir);
        }
    }
    printf("bj_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
