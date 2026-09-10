/*
 * PocketOS touch keyboard. See pos_keyboard.h.
 *
 * The two layers have the same shape - 10, 9, and 9 keys then a three-key
 * bottom row - so the buttons are built once and relabelled when the layer or
 * Shift changes. Only the code points behind them differ.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pos_keyboard.h"

#include "pos_input.h"
#include "pos_styles.h"

#include <string.h>

#define KB_ROWS 4
#define KB_MAX_IN_ROW 10
#define KB_KEYS (10 + 9 + 9 + 3)

enum key_role {
    K_CHAR,
    K_SHIFT,
    K_BKSP,
    K_LAYER,
    K_SPACE,
    K_RETURN
};

struct key_def {
    uint32_t lower; /* code point, or 0 for a control key */
    uint32_t upper; /* code point when Shift is engaged */
    uint8_t role;
};

#define C(c) { (c), (c), K_CHAR }
#define L(lo, up) { (lo), (up), K_CHAR }
#define CTRL(r) { 0, 0, (r) }

static const uint8_t row_len[KB_ROWS] = { 10, 9, 9, 3 };

/* DS §17.3: QWERTY, Shift for capitals. */
static const struct key_def alpha[KB_ROWS][KB_MAX_IN_ROW] = {
    { L('q', 'Q'), L('w', 'W'), L('e', 'E'), L('r', 'R'), L('t', 'T'),
      L('y', 'Y'), L('u', 'U'), L('i', 'I'), L('o', 'O'), L('p', 'P') },
    { L('a', 'A'), L('s', 'S'), L('d', 'D'), L('f', 'F'), L('g', 'G'),
      L('h', 'H'), L('j', 'J'), L('k', 'K'), L('l', 'L') },
    { CTRL(K_SHIFT), L('z', 'Z'), L('x', 'X'), L('c', 'C'), L('v', 'V'),
      L('b', 'B'), L('n', 'N'), L('m', 'M'), CTRL(K_BKSP) },
    { CTRL(K_LAYER), CTRL(K_SPACE), CTRL(K_RETURN) }
};

/* DS §17.3: digits on row 1, punctuation and æ ø å on rows 2 and 3. The
 * three Norwegian letters are here and not on the alpha layer by the owner's
 * ruling of 2026-09-10 closing caveat C9; Shift applies to them as to any
 * letter, which is why they carry an upper case and the punctuation does
 * not. */
static const struct key_def symbol[KB_ROWS][KB_MAX_IN_ROW] = {
    { C('1'), C('2'), C('3'), C('4'), C('5'),
      C('6'), C('7'), C('8'), C('9'), C('0') },
    { C('-'), C('/'), C(':'), C(';'), C('('),
      C(')'), C('$'), C('&'), C('@') },
    { CTRL(K_SHIFT), L(0xE6, 0xC6), L(0xF8, 0xD8), L(0xE5, 0xC5), C('.'),
      C(','), C('?'), C('!'), CTRL(K_BKSP) },
    { CTRL(K_LAYER), CTRL(K_SPACE), CTRL(K_RETURN) }
};

struct kb {
    lv_obj_t *sheet;
    lv_obj_t *keys[KB_KEYS];
    lv_obj_t *labels[KB_KEYS];
    uint8_t row[KB_KEYS]; /* where the key sits, so either layer can be read */
    uint8_t col[KB_KEYS];
    uint8_t layer;
    uint8_t shift;
    uint8_t ret;
    lv_obj_t *shift_key;
    lv_obj_t *return_key;
    lv_timer_t *repeat;
    uint32_t last_shift_tap;
    void (*done_cb)(void *user);
    void *done_user;
};

/* ---- helpers ----------------------------------------------------------- */

/* A code point as UTF-8, for a key's label. The stream itself carries code
 * points (DS §17.4); this is only what the glass shows. */
static void cp_to_utf8(uint32_t cp, char out[5])
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        out[1] = '\0';
    } else if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        out[2] = '\0';
    } else {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        out[3] = '\0';
    }
}

/* The two tables have the same shape, so a key's position identifies it in
 * whichever layer is showing. */
static const struct key_def *def_at(const struct kb *k, unsigned i)
{
    unsigned r = k->row[i];
    unsigned c = k->col[i];

    return k->layer == POS_KB_LAYER_SYMBOL ? &symbol[r][c] : &alpha[r][c];
}

static bool shifted(const struct kb *k)
{
    return k->shift != POS_KB_SHIFT_OFF;
}

static void stop_repeat(struct kb *k)
{
    if (k->repeat) {
        lv_timer_delete(k->repeat);
        k->repeat = NULL;
    }
}

/* ---- drawing state onto the keys --------------------------------------- */

static const char *control_label(const struct kb *k, enum key_role role)
{
    switch (role) {
    case K_SHIFT:
        return "SHIFT";
    case K_BKSP:
        return "BKSP";
    case K_LAYER:
        return k->layer == POS_KB_LAYER_SYMBOL ? "ABC" : "?123";
    case K_SPACE:
        return "SPACE";
    case K_RETURN:
        return k->ret == POS_KB_RETURN_NEWLINE ? "ENTER" : "DONE";
    default:
        return "";
    }
}

static void refresh(struct kb *k)
{
    char buf[5];
    unsigned i;

    for (i = 0; i < KB_KEYS; i++) {
        const struct key_def *d = def_at(k, i);

        if (d->role == K_CHAR) {
            cp_to_utf8(shifted(k) ? d->upper : d->lower, buf);
            lv_label_set_text(k->labels[i], buf);
        } else {
            lv_label_set_text(k->labels[i], control_label(k, (enum key_role)d->role));
        }
    }

    /* Shift: off is a plain key; engaged is the accent fill; locked adds the
     * underline. The letters themselves also change case, so the state is
     * legible without comparing two fills (DS §17.3, and §2's rule that
     * colour never carries meaning alone). */
    lv_obj_remove_style(k->shift_key, pos_style(POS_STYLE_KEY_ENGAGED), 0);
    lv_obj_remove_style(k->shift_key, pos_style(POS_STYLE_KEY_LOCKED), 0);
    if (k->shift != POS_KB_SHIFT_OFF) {
        pos_style_add(k->shift_key, POS_STYLE_KEY_ENGAGED, 0);
    }
    if (k->shift == POS_KB_SHIFT_LOCK) {
        pos_style_add(k->shift_key, POS_STYLE_KEY_LOCKED, 0);
    }

    /* Done is the one filled key of §17.3; Enter is an ordinary one. */
    lv_obj_remove_style(k->return_key, pos_style(POS_STYLE_KEY_ENGAGED), 0);
    if (k->ret == POS_KB_RETURN_DONE) {
        pos_style_add(k->return_key, POS_STYLE_KEY_ENGAGED, 0);
    }
}

/* ---- events ------------------------------------------------------------ */

static void repeat_cb(lv_timer_t *t)
{
    pos_input_push_key(LV_KEY_BACKSPACE);
    /* The first fire ends the initial delay; every one after it is a repeat.
     * Reduced motion does not silence this: DS §17.3 calls the repeat
     * function, not decoration. */
    lv_timer_set_period(t, POS_KB_REPEAT_MS);
}

static void key_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    struct kb *k = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    const struct key_def *d = NULL;
    unsigned i;

    for (i = 0; i < KB_KEYS; i++) {
        if (k->keys[i] == btn) {
            d = def_at(k, i);
            break;
        }
    }
    if (!d) {
        return;
    }

    if (code == LV_EVENT_PRESSED && d->role == K_BKSP) {
        pos_input_push_key(LV_KEY_BACKSPACE);
        stop_repeat(k);
        k->repeat = lv_timer_create(repeat_cb, POS_KB_REPEAT_DELAY_MS, k);
        return;
    }
    if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && d->role == K_BKSP) {
        stop_repeat(k);
        return;
    }
    if (code != LV_EVENT_CLICKED) {
        return;
    }

    switch (d->role) {
    case K_CHAR:
        pos_input_push_key(shifted(k) ? d->upper : d->lower);
        if (k->shift == POS_KB_SHIFT_ONCE) {
            k->shift = POS_KB_SHIFT_OFF;
            refresh(k);
        }
        break;
    case K_SHIFT:
        if (k->shift == POS_KB_SHIFT_OFF) {
            k->shift = POS_KB_SHIFT_ONCE;
        } else if (k->shift == POS_KB_SHIFT_ONCE) {
            /* A quick second tap locks; a slower one means the user changed
             * their mind about the one shot. */
            k->shift = (lv_tick_elaps(k->last_shift_tap) <= POS_KB_SHIFT_LOCK_MS)
                           ? POS_KB_SHIFT_LOCK
                           : POS_KB_SHIFT_OFF;
        } else {
            k->shift = POS_KB_SHIFT_OFF;
        }
        k->last_shift_tap = lv_tick_get();
        refresh(k);
        break;
    case K_LAYER:
        k->layer = k->layer == POS_KB_LAYER_ALPHA ? POS_KB_LAYER_SYMBOL : POS_KB_LAYER_ALPHA;
        refresh(k);
        break;
    case K_SPACE:
        pos_input_push_key(' ');
        if (k->shift == POS_KB_SHIFT_ONCE) {
            k->shift = POS_KB_SHIFT_OFF;
            refresh(k);
        }
        break;
    case K_RETURN:
        /* One key, one logical value. What differs is what the owner does
         * about it: a single-line field reports itself ready and the owner
         * hides the sheet; a multi-line field takes the line break. */
        pos_input_push_key(LV_KEY_ENTER);
        if (k->ret == POS_KB_RETURN_DONE && k->done_cb) {
            k->done_cb(k->done_user);
        }
        break;
    default:
        break;
    }
}

static void sheet_deleted_cb(lv_event_t *e)
{
    struct kb *k = lv_event_get_user_data(e);

    stop_repeat(k);
    lv_free(k);
}

/* ---- building ---------------------------------------------------------- */

static lv_obj_t *make_key(struct kb *k, lv_obj_t *parent, const struct key_def *d,
                          unsigned idx, unsigned r, unsigned c, int32_t w)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lb = lv_label_create(btn);

    lv_obj_remove_style_all(btn);
    pos_style_add(btn, POS_STYLE_SLAB, 0);
    pos_style_add(btn, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_size(btn, w, POS_KB_KEY_H);
    /* A key must never move the focus. Without this, LVGL's click-focus
     * defocuses the field the moment a groupless key is pressed, and DS
     * §17.2 says opening and using the keyboard leaves focus alone. */
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    /* Letters read as text; word keys read as labels (DS §17.3). */
    pos_style_add(lb, d->role == K_CHAR ? POS_STYLE_ROW_TITLE : POS_STYLE_BUTTON_LABEL, 0);
    lv_obj_center(lb);

    lv_obj_add_event_cb(btn, key_event_cb, LV_EVENT_CLICKED, k);
    lv_obj_add_event_cb(btn, key_event_cb, LV_EVENT_PRESSED, k);
    lv_obj_add_event_cb(btn, key_event_cb, LV_EVENT_RELEASED, k);
    lv_obj_add_event_cb(btn, key_event_cb, LV_EVENT_PRESS_LOST, k);

    k->keys[idx] = btn;
    k->labels[idx] = lb;
    k->row[idx] = (uint8_t)r;
    k->col[idx] = (uint8_t)c;
    return btn;
}

lv_obj_t *pos_keyboard_create(lv_obj_t *parent)
{
    struct kb *k = lv_malloc_zeroed(sizeof(*k));
    unsigned idx = 0;
    unsigned r;

    if (!k) {
        return NULL;
    }
    k->layer = POS_KB_LAYER_ALPHA;
    k->shift = POS_KB_SHIFT_OFF;
    k->ret = POS_KB_RETURN_DONE;

    k->sheet = lv_obj_create(parent);
    lv_obj_remove_style_all(k->sheet);
    pos_style_add(k->sheet, POS_STYLE_KB_SHEET, 0);
    lv_obj_set_size(k->sheet, POS_KB_W, POS_KB_H);
    lv_obj_align(k->sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(k->sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(k->sheet, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(k->sheet, POS_KB_PAD_H, 0);
    lv_obj_set_style_pad_right(k->sheet, POS_KB_PAD_H, 0);
    lv_obj_set_style_pad_top(k->sheet, POS_KB_PAD_V, 0);
    lv_obj_set_style_pad_bottom(k->sheet, POS_KB_PAD_V, 0);
    lv_obj_set_style_pad_row(k->sheet, POS_KB_ROW_GAP, 0);
    lv_obj_clear_flag(k->sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(k->sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(k->sheet, k);
    lv_obj_add_event_cb(k->sheet, sheet_deleted_cb, LV_EVENT_DELETE, k);

    for (r = 0; r < KB_ROWS; r++) {
        lv_obj_t *row = lv_obj_create(k->sheet);
        unsigned c;

        lv_obj_remove_style_all(row);
        lv_obj_set_height(row, POS_KB_KEY_H);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, POS_KB_GAP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        for (c = 0; c < row_len[r]; c++) {
            const struct key_def *d = &alpha[r][c];
            int32_t w = POS_KB_KEY_W;
            lv_obj_t *btn;

            if (d->role == K_SHIFT || d->role == K_BKSP || d->role == K_LAYER ||
                d->role == K_RETURN) {
                w = POS_KB_WIDE_W;
            }
            btn = make_key(k, row, d, idx, r, c, w);
            if (d->role == K_SPACE) {
                /* Space fills what the two wide keys leave (DS §17.3):
                 * 556 - 80 - 80 - two gaps = 388. */
                lv_obj_set_width(btn, POS_KB_W - 2 * POS_KB_PAD_H - 2 * POS_KB_WIDE_W -
                                          2 * POS_KB_GAP);
            }
            if (d->role == K_SHIFT) {
                k->shift_key = btn;
            }
            if (d->role == K_RETURN) {
                k->return_key = btn;
            }
            idx++;
        }
    }

    refresh(k);
    return k->sheet;
}

/* ---- the small public surface ------------------------------------------ */

static struct kb *kb_of(lv_obj_t *sheet)
{
    return sheet ? lv_obj_get_user_data(sheet) : NULL;
}

void pos_keyboard_set_return(lv_obj_t *kb, enum pos_kb_return ret)
{
    struct kb *k = kb_of(kb);

    if (k) {
        k->ret = (uint8_t)ret;
        refresh(k);
    }
}

void pos_keyboard_set_done_cb(lv_obj_t *kb, void (*cb)(void *user), void *user)
{
    struct kb *k = kb_of(kb);

    if (k) {
        k->done_cb = cb;
        k->done_user = user;
    }
}

void pos_keyboard_show(lv_obj_t *kb)
{
    struct kb *k = kb_of(kb);

    if (k) {
        /* No animation: DS §12's motion table has no row for a keyboard, and
         * inventing one is not this amendment's to do. Nothing here changes
         * under reduced motion because nothing here moves. */
        lv_obj_clear_flag(k->sheet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(k->sheet);
    }
}

void pos_keyboard_hide(lv_obj_t *kb)
{
    struct kb *k = kb_of(kb);

    if (k) {
        stop_repeat(k);
        lv_obj_add_flag(k->sheet, LV_OBJ_FLAG_HIDDEN);
    }
}

bool pos_keyboard_is_shown(lv_obj_t *kb)
{
    struct kb *k = kb_of(kb);

    return k && !lv_obj_has_flag(k->sheet, LV_OBJ_FLAG_HIDDEN);
}

enum pos_kb_layer pos_keyboard_layer(lv_obj_t *kb)
{
    struct kb *k = kb_of(kb);

    return k ? (enum pos_kb_layer)k->layer : POS_KB_LAYER_ALPHA;
}

enum pos_kb_shift pos_keyboard_shift(lv_obj_t *kb)
{
    struct kb *k = kb_of(kb);

    return k ? (enum pos_kb_shift)k->shift : POS_KB_SHIFT_OFF;
}

lv_obj_t *pos_keyboard_key(lv_obj_t *kb, const char *label)
{
    struct kb *k = kb_of(kb);
    unsigned i;

    if (!k || !label) {
        return NULL;
    }
    for (i = 0; i < KB_KEYS; i++) {
        if (strcmp(lv_label_get_text(k->labels[i]), label) == 0) {
            return k->keys[i];
        }
    }
    return NULL;
}
