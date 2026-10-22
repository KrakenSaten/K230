/*
 * RIFT COMMS: the emoji button and picker. See rift_emoji_picker.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_emoji_picker.h"

#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pos_input.h"
#include "pos_styles.h"
#include "rift_emoji.h"
#include "rift_emoji_font.h"
#include "rift_emoji_pick.h"
#include "rift_emoji_style.h"
#include "rift_store.h"
#include "rift_widgets.h"

#include <stdlib.h>
#include <string.h>

/* The panel takes the app's area above the composer, or all of it where
 * that is too low, EDGE_PX in from its edges; on a wide body (HDMI) no wider
 * than BOX_MAX_W. A row of three tabs and a header with CLOSE, each
 * RIFT_PRIMARY_H tall, then the group's emoji in as many columns as cells of
 * CELL_WANT fit across - never smaller than CELL_MIN (DS's 64 px touch
 * target), never larger than CELL_MAX - scrolling when the rows are more than
 * the panel holds. */
#define CELL_MIN 64
#define CELL_WANT 104
#define CELL_MAX 120
#define CELL_GAP 8
#define FRAME_PAD 12
#define EDGE_PX 8
#define ANCHOR_GAP 8
#define BOX_MAX_W 960
#define HEAD_H RIFT_PRIMARY_H
#define CLOSE_W 128
/* The emoji artwork is one size, 18 px (rift_emoji_img.h); here it is drawn
 * at a whole multiple of that, so every source pixel is the same number of
 * screen pixels: about half the cell, 2x at least, 4x at most. A tab's icon
 * is 2x. */
#define ART_PX 18
#define ART_MIN_X 2
#define ART_MAX_X 4
/* How far a finger may move between press and release and still pick: a
 * drag that does not scroll anything (the rows all fit) is not a tap. */
#define TAP_SLOP 12

/* The button's face: the first of the common emoji. */
#define BUTTON_ICON "\xF0\x9F\x99\x82" /* U+1F642 */

/* A button per composer: the portrait thread's and the landscape command
 * line's. */
#define BUTTONS 2

static struct rift_app *owner;
static struct {
    lv_obj_t *button;
    lv_obj_t *field;
} buttons[BUTTONS];

static struct {
    lv_obj_t *scrim; /* full screen on the top layer: a tap outside closes */
    lv_obj_t *box;
    lv_obj_t *title;
    lv_obj_t *close;
    lv_obj_t *grid;
    lv_obj_t *tab[RIFT_EMOJI_GROUPS];
    lv_obj_t *cell[RIFT_EMOJI_CELLS];
    const char *item[RIFT_EMOJI_CELLS];
    lv_obj_t *field;  /* the text area an emoji goes into */
    lv_obj_t *anchor; /* the button it was opened from */
    int had_keys;     /* the field has had the keys since it opened */
    lv_point_t press; /* where the finger went down on a cell */
    unsigned n;
    unsigned group;
    unsigned selected;
    unsigned cols;
    int32_t cell_px;
    int32_t art_px;
} pk;

static void field_deleted(lv_event_t *e);

/* The one code point an emoji folds to (rift_emoji_fold): what the colour
 * font's images are keyed by. 0 when it is not one. */
static uint32_t emoji_key(const char *utf8)
{
    char shown[32];
    const unsigned char *s = (const unsigned char *)shown;
    uint32_t c;
    int len;
    int i;

    rift_emoji_fold(utf8 ? utf8 : "", shown, sizeof(shown));
    c = s[0];
    len = c < 0x80u ? 1 : c >= 0xF0u ? 4 : c >= 0xE0u ? 3 : 2;
    if (len > 1) {
        c &= 0x3Fu >> (len - 1);
    }
    for (i = 1; i < len; i++) {
        if ((s[i] & 0xC0u) != 0x80u) {
            return 0;
        }
        c = (c << 6) | (s[i] & 0x3Fu);
    }
    return s[len] == '\0' ? c : 0;
}

/* The emoji's artwork on img, px square (aspect kept: a flag is wider than
 * it is tall). The pixel multiple is whole, so no source pixel is smeared
 * across two screen pixels. */
static void set_art(lv_obj_t *img, const char *utf8, int32_t px)
{
    lv_image_set_src(img, rift_emoji_image(emoji_key(utf8)));
    lv_obj_set_size(img, px, px);
}

static lv_obj_t *art(lv_obj_t *parent, int32_t px)
{
    lv_obj_t *img = lv_image_create(parent);

    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(img, false);
    lv_obj_set_size(img, px, px);
    lv_obj_center(img);
    return img;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return o;
}

/* A touch target holding one emoji, out of the focus bookkeeping. A drag on
 * it scrolls the grid it is in (the scroll is chained). */
static lv_obj_t *emoji_target(lv_obj_t *parent, int32_t w, int32_t h, int32_t art_px)
{
    lv_obj_t *b = lv_button_create(parent);

    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE |
                              LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_size(b, w, h);
    pos_style_add(b, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    (void)art(b, art_px);
    return b;
}

/* The selected one in the primary treatment, the rest secondary, as the
 * letter picker draws its choices. */
static void paint(lv_obj_t *b, int on)
{
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    pos_style_add(b, on ? POS_STYLE_BUTTON_PRIMARY : POS_STYLE_BUTTON_SECONDARY, 0);
}

static void show_selection(void)
{
    unsigned i;

    for (i = 0; i < RIFT_EMOJI_CELLS; i++) {
        paint(pk.cell[i], i == pk.selected);
    }
    for (i = 0; i < RIFT_EMOJI_GROUPS; i++) {
        paint(pk.tab[i], i == pk.group);
    }
    /* The selection is where the keys are: always in sight. */
    if (pk.selected < pk.n) {
        lv_obj_update_layout(pk.box);
        lv_obj_scroll_to_view(pk.cell[pk.selected], LV_ANIM_OFF);
    }
}

static unsigned recent_of(const struct rift_app *a, const char **out)
{
    return a ? rift_emoji_recent_parse(a->prefs.emoji_recent, out) : 0;
}

static void show_group(unsigned g)
{
    const char *recent[RIFT_EMOJI_RECENT_MAX];
    unsigned n_recent = recent_of(owner, recent);
    unsigned i;

    pk.group = g < RIFT_EMOJI_GROUPS ? g : 0;
    pk.n = rift_emoji_group_items(pk.group, recent, n_recent, pk.item);
    pk.selected = 0;
    lv_label_set_text(pk.title, rift_emoji_groups[pk.group].title);
    for (i = 0; i < RIFT_EMOJI_CELLS; i++) {
        if (i < pk.n) {
            lv_obj_t *img = lv_obj_get_child(pk.cell[i], 0);

            set_art(img, pk.item[i], pk.art_px);
            lv_obj_remove_flag(pk.cell[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            pk.item[i] = NULL;
            lv_obj_add_flag(pk.cell[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_scroll_to_y(pk.grid, 0, LV_ANIM_OFF);
    show_selection();
}

bool rift_emoji_picker_is_open(void)
{
    return pk.scrim != NULL;
}

void rift_emoji_picker_close(void)
{
    if (!pk.scrim) {
        return;
    }
    if (pk.field) {
        lv_obj_remove_event_cb_with_user_data(pk.field, field_deleted, NULL);
    }
    /* Async: a tap on an emoji closes the picker from inside that emoji's
     * own event, and an object must not be deleted under its own event.
     * Hidden now, so it is neither drawn nor tapped in the meantime. */
    lv_obj_add_flag(pk.scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(pk.scrim);
    memset(&pk, 0, sizeof(pk));
}

static void field_deleted(lv_event_t *e)
{
    (void)e;
    pk.field = NULL; /* already going; nothing to unhook */
    rift_emoji_picker_close();
}

/* The emoji goes first in the recent list, and the list into the
 * preferences file when it changed. A file that cannot be written leaves
 * the list as it is for this session (rift_store.h). */
static void remember(struct rift_app *a, const char *emoji)
{
    const char *recent[RIFT_EMOJI_RECENT_MAX];
    unsigned n;
    char text[RIFT_PREF_EMOJI_RECENT_LEN];

    if (!a) {
        return;
    }
    n = recent_of(a, recent);
    n = rift_emoji_recent_push(recent, n, emoji);
    if (rift_emoji_recent_format(recent, n, text, sizeof(text)) < 0 ||
        strcmp(text, a->prefs.emoji_recent) == 0) {
        return;
    }
    memcpy(a->prefs.emoji_recent, text, sizeof(text));
    a->prefs_saved = rift_store_save(&a->prefs) == 0;
    if (!a->prefs_saved) {
        LOG_WARN("rift: the recent emoji could not be stored at %s", rift_store_path());
    }
}

/* Insert the emoji at the field's caret, then close. Nothing is sent, and
 * nothing is inserted into a field that cannot take it any more. */
static void choose(unsigned i)
{
    const char *emoji = i < pk.n ? pk.item[i] : NULL;
    lv_obj_t *field = pk.field;
    struct rift_app *a = owner;

    rift_emoji_picker_close();
    if (!emoji || !field || lv_obj_has_state(field, LV_STATE_DISABLED)) {
        return;
    }
    lv_textarea_add_text(field, emoji);
    remember(a, emoji);
    if (a && pos_input_focused() != field) {
        /* Picked by a finger with the keys elsewhere: the composer takes
         * them, from the timer (rift_focus.c), so the next key types on. */
        a->focus_composer_pending = 1;
    }
}

static void on_cell_pressed(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);

    if (indev) {
        lv_indev_get_point(indev, &pk.press);
    }
}

/* A tap, not the end of a drag: LVGL sends no click after a scroll, and a
 * finger that wandered off its cell further than TAP_SLOP is a drag too. */
static void on_cell(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);

    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        lv_point_t at;

        lv_indev_get_point(indev, &at);
        if (abs(at.x - pk.press.x) > TAP_SLOP || abs(at.y - pk.press.y) > TAP_SLOP) {
            return;
        }
    }
    choose((unsigned)(uintptr_t)lv_event_get_user_data(e));
}

static void on_tab(lv_event_t *e)
{
    show_group((unsigned)(uintptr_t)lv_event_get_user_data(e));
}

static void on_close(lv_event_t *e)
{
    (void)e;
    rift_emoji_picker_close();
}

static void on_scrim(lv_event_t *e)
{
    /* Only a tap on the scrim itself: one inside the panel is not outside. */
    if (lv_event_get_target_obj(e) == pk.scrim) {
        rift_emoji_picker_close();
    }
}

/* Where the panel may go: the app's area (RIFT's frame - the shell's status
 * bar and the touch keyboard are not in it), EDGE_PX in; above the button
 * when the panel's header, tabs and a row of emoji fit there - so the
 * composer and its caret stay in sight - and over all of it otherwise. */
static void room(lv_area_t *r)
{
    lv_area_t anchor;
    int32_t least = 2 * FRAME_PAD + 2 * HEAD_H + 2 * CELL_GAP + CELL_MIN;

    if (owner && owner->frame) {
        lv_obj_update_layout(owner->frame);
        lv_obj_get_coords(owner->frame, r);
    } else {
        r->x1 = 0;
        r->y1 = 0;
        r->x2 = lv_display_get_horizontal_resolution(NULL) - 1;
        r->y2 = lv_display_get_vertical_resolution(NULL) - 1;
    }
    r->x1 += EDGE_PX;
    r->y1 += EDGE_PX;
    r->x2 -= EDGE_PX;
    r->y2 -= EDGE_PX;
    lv_obj_get_coords(pk.anchor ? pk.anchor : pk.field, &anchor);
    if (anchor.y1 - ANCHOR_GAP - r->y1 >= least && anchor.y1 - ANCHOR_GAP <= r->y2) {
        r->y2 = anchor.y1 - ANCHOR_GAP - 1;
    }
}

bool rift_emoji_picker_open(lv_obj_t *field, lv_obj_t *anchor)
{
    lv_area_t r;
    lv_obj_t *head;
    lv_obj_t *tabs;
    int32_t room_w;
    int32_t room_h;
    int32_t box_w;
    int32_t inner_w;
    int32_t grid_h;
    int32_t art_px;
    unsigned rows;
    unsigned i;

    if (!field || lv_obj_has_state(field, LV_STATE_DISABLED) || !lv_obj_is_visible(field)) {
        return false;
    }
    rift_emoji_picker_close();
    pk.field = field;
    pk.anchor = anchor;
    lv_obj_add_event_cb(field, field_deleted, LV_EVENT_DELETE, NULL);

    room(&r);
    room_w = lv_area_get_width(&r);
    room_h = lv_area_get_height(&r);
    box_w = room_w < BOX_MAX_W ? room_w : BOX_MAX_W;
    inner_w = box_w - 2 * FRAME_PAD;
    pk.cols = (unsigned)((inner_w + CELL_GAP) / (CELL_WANT + CELL_GAP));
    if (pk.cols < 1) {
        pk.cols = 1;
    }
    if (pk.cols > RIFT_EMOJI_CELLS) {
        pk.cols = RIFT_EMOJI_CELLS;
    }
    pk.cell_px = (inner_w - (int32_t)(pk.cols - 1) * CELL_GAP) / (int32_t)pk.cols;
    if (pk.cell_px > CELL_MAX) {
        pk.cell_px = CELL_MAX;
    }
    if (pk.cell_px < CELL_MIN) {
        pk.cell_px = CELL_MIN; /* a sliver of a body: never a smaller target */
    }
    art_px = (pk.cell_px / 2 + ART_PX / 2) / ART_PX;
    art_px = ART_PX * (art_px < ART_MIN_X ? ART_MIN_X : art_px > ART_MAX_X ? ART_MAX_X : art_px);
    pk.art_px = art_px;
    /* The grid as wide as its columns, so the panel hugs them. */
    inner_w = (int32_t)pk.cols * pk.cell_px + (int32_t)(pk.cols - 1) * CELL_GAP;
    box_w = inner_w + 2 * FRAME_PAD;
    rows = (RIFT_EMOJI_CELLS + pk.cols - 1) / pk.cols;
    grid_h = (int32_t)rows * pk.cell_px + (int32_t)(rows - 1) * CELL_GAP;
    if (grid_h > room_h - 2 * FRAME_PAD - 2 * (HEAD_H + CELL_GAP)) {
        grid_h = room_h - 2 * FRAME_PAD - 2 * (HEAD_H + CELL_GAP);
        if (grid_h < CELL_MIN) {
            grid_h = CELL_MIN;
        }
    }

    pk.scrim = plain(lv_layer_top());
    lv_obj_set_size(pk.scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(pk.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pk.scrim, on_scrim, LV_EVENT_CLICKED, NULL);

    pk.box = plain(pk.scrim);
    pos_style_add(pk.box, POS_STYLE_SCREEN, 0);
    pos_style_add(pk.box, POS_STYLE_PANEL, 0);
    /* The panel's outline on the screen's background: PANEL leaves the
     * background transparent, and the thread under it would show through. */
    lv_obj_set_style_bg_opa(pk.box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(pk.box, FRAME_PAD, 0);
    lv_obj_set_style_pad_row(pk.box, CELL_GAP, 0);
    lv_obj_set_size(pk.box, box_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pk.box, LV_FLEX_FLOW_COLUMN);
    /* A tap between two emoji is not a tap outside. */
    lv_obj_add_flag(pk.box, LV_OBJ_FLAG_CLICKABLE);

    /* The group's name, one line, "..." where a larger text size does not
     * fit it (DS §46.5), and CLOSE. */
    head = plain(pk.box);
    lv_obj_set_size(head, LV_PCT(100), HEAD_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, CELL_GAP, 0);
    pk.title = lv_label_create(head);
    lv_obj_remove_style_all(pk.title);
    pos_style_add(pk.title, POS_STYLE_CAPTION, 0);
    lv_obj_set_flex_grow(pk.title, 1);
    lv_label_set_text(pk.title, "");
    pocketui_label_fit(pk.title, 1);
    pk.close = rift_action(head, "CLOSE", 0, 1, on_close, NULL);
    lv_obj_set_flex_grow(pk.close, 0);
    lv_obj_set_size(pk.close, CLOSE_W, HEAD_H);
    lv_obj_remove_flag(pk.close, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    /* The groups' tabs, side by side across the panel. */
    tabs = plain(pk.box);
    lv_obj_set_size(tabs, LV_PCT(100), HEAD_H);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, CELL_GAP, 0);
    for (i = 0; i < RIFT_EMOJI_GROUPS; i++) {
        pk.tab[i] = emoji_target(tabs, CELL_MIN, HEAD_H, ART_PX * ART_MIN_X);
        lv_obj_set_flex_grow(pk.tab[i], 1);
        set_art(lv_obj_get_child(pk.tab[i], 0), rift_emoji_groups[i].icon, ART_PX * ART_MIN_X);
        lv_obj_add_event_cb(pk.tab[i], on_tab, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }

    /* The emoji, scrolling up and down when they do not all fit. */
    pk.grid = plain(pk.box);
    lv_obj_add_flag(pk.grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(pk.grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(pk.grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_size(pk.grid, inner_w, grid_h);
    lv_obj_set_flex_flow(pk.grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(pk.grid, CELL_GAP, 0);
    lv_obj_set_style_pad_row(pk.grid, CELL_GAP, 0);
    for (i = 0; i < RIFT_EMOJI_CELLS; i++) {
        pk.cell[i] = emoji_target(pk.grid, pk.cell_px, pk.cell_px, art_px);
        lv_obj_add_event_cb(pk.cell[i], on_cell_pressed, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(pk.cell[i], on_cell, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }

    /* Centred across the room, and at its foot: next to the composer. */
    lv_obj_update_layout(pk.box);
    lv_obj_set_pos(pk.box, r.x1 + (room_w - lv_obj_get_width(pk.box)) / 2,
                   r.y2 + 1 - lv_obj_get_height(pk.box));
    show_group(0);
    if (owner && pos_input_focused() != field) {
        /* The keys come to the picker through the field: the field takes
         * them, from the timer, as the composer does on opening. */
        owner->focus_composer_pending = 1;
    }
    return true;
}

/* A key for the field while the picker is up: true when the picker used
 * it. Any other key closes the picker and is the field's as always. */
static bool picker_key(uint32_t key)
{
    unsigned cols = pk.cols ? pk.cols : 1;

    switch (key) {
    case LV_KEY_LEFT:
        if (pk.selected > 0) {
            pk.selected--;
        }
        break;
    case LV_KEY_RIGHT:
        if (pk.selected + 1 < pk.n) {
            pk.selected++;
        }
        break;
    case LV_KEY_HOME:
        pk.selected = 0;
        break;
    case LV_KEY_END:
        pk.selected = pk.n ? pk.n - 1 : 0;
        break;
    case LV_KEY_UP:
        /* Past the top row is the group before, on its bottom row: the
         * arrows reach every emoji without a key of their own for the tabs
         * (Tab is the focus group's, LV_KEY_NEXT). */
        if (pk.selected >= cols) {
            pk.selected -= cols;
        } else {
            unsigned col = pk.selected;

            show_group((pk.group + RIFT_EMOJI_GROUPS - 1) % RIFT_EMOJI_GROUPS);
            pk.selected = pk.n > col ? col + (pk.n - 1 - col) / cols * cols : 0;
        }
        break;
    case LV_KEY_DOWN:
        if (pk.selected + cols < pk.n) {
            pk.selected += cols;
        } else {
            unsigned col = pk.selected % cols;

            show_group((pk.group + 1) % RIFT_EMOJI_GROUPS);
            pk.selected = col < pk.n ? col : 0;
        }
        break;
    case LV_KEY_ENTER:
        choose(pk.selected);
        return true;
    case LV_KEY_ESC:
        rift_emoji_picker_close();
        return true;
    default:
        rift_emoji_picker_close();
        return false;
    }
    show_selection();
    return true;
}

/* Before the text area's own handler (LV_EVENT_PREPROCESS): a key the picker
 * uses never reaches the field - Enter would send, the arrows would move the
 * caret, Esc would clear the message (rift_focus.c). */
static void on_field_key(lv_event_t *e)
{
    if (!pk.scrim || lv_event_get_target_obj(e) != pk.field) {
        return;
    }
    if (picker_key(lv_event_get_key(e))) {
        lv_event_stop_processing(e);
    }
}

static void on_button(lv_event_t *e)
{
    rift_emoji_picker_open(lv_event_get_user_data(e), lv_event_get_target_obj(e));
}

static void on_button_deleted(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target_obj(e);
    unsigned i;

    for (i = 0; i < BUTTONS; i++) {
        if (buttons[i].button == b) {
            buttons[i].button = NULL;
            buttons[i].field = NULL;
        }
    }
}

lv_obj_t *rift_emoji_button(struct rift_app *a, lv_obj_t *parent, lv_obj_t *field)
{
    lv_obj_t *b;
    lv_obj_t *label;
    unsigned i;

    if (!field) {
        return NULL;
    }
    owner = a;
    b = rift_action(parent, "", 0, 1, on_button, field);
    lv_obj_set_flex_grow(b, 0);
    lv_obj_set_width(b, RIFT_TOUCH_H);
    /* A tap on it leaves the focus in the field, as a tap on the picker
     * does: the caret is where the emoji goes. */
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    label = lv_obj_get_child(b, 0);
    rift_emoji_style_add(label, POS_STYLE_BUTTON_LABEL);
    {
        char shown[32];

        rift_emoji_fold(BUTTON_ICON, shown, sizeof(shown));
        lv_label_set_text(label, shown);
    }
    lv_obj_add_event_cb(b, on_button_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(field, on_field_key, (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS),
                        NULL);
    for (i = 0; i < BUTTONS; i++) {
        if (!buttons[i].button) {
            buttons[i].button = b;
            buttons[i].field = field;
            break;
        }
    }
    return b;
}

void rift_emoji_picker_check(struct rift_app *a)
{
    unsigned i;

    for (i = 0; i < BUTTONS; i++) {
        if (buttons[i].button) {
            rift_action_set_enabled(buttons[i].button, 0,
                                    !lv_obj_has_state(buttons[i].field, LV_STATE_DISABLED));
        }
    }
    if (!pk.scrim) {
        return;
    }
    if (!a || !a->frame || !pk.field || lv_obj_has_state(pk.field, LV_STATE_DISABLED) ||
        !lv_obj_is_visible(pk.field) || (pk.anchor && !lv_obj_is_visible(pk.anchor))) {
        rift_emoji_picker_close();
        return;
    }
    /* The keys went elsewhere - Tab, a dialog - after the field had them:
     * what the picker would answer is no longer the field's. Before that,
     * the field may still be about to take them (rift_emoji_picker_open). */
    if (pos_input_focused() == pk.field) {
        pk.had_keys = 1;
    } else if (pk.had_keys || !a->focus_composer_pending) {
        rift_emoji_picker_close();
    }
}

unsigned rift_emoji_picker_group(void)
{
    return pk.group;
}

unsigned rift_emoji_picker_selected(void)
{
    return pk.selected;
}

unsigned rift_emoji_picker_cols(void)
{
    return pk.scrim ? pk.cols : 0;
}

lv_obj_t *rift_emoji_picker_cell(unsigned i)
{
    return pk.scrim && i < pk.n ? pk.cell[i] : NULL;
}

const char *rift_emoji_picker_item(unsigned i)
{
    return pk.scrim && i < pk.n ? pk.item[i] : NULL;
}

lv_obj_t *rift_emoji_picker_tab(unsigned g)
{
    return pk.scrim && g < RIFT_EMOJI_GROUPS ? pk.tab[g] : NULL;
}

lv_obj_t *rift_emoji_picker_box(void)
{
    return pk.box;
}

lv_obj_t *rift_emoji_picker_grid(void)
{
    return pk.grid;
}

lv_obj_t *rift_emoji_picker_close_button(void)
{
    return pk.close;
}
