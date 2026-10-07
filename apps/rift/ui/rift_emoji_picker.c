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
#include "rift_emoji_pick.h"
#include "rift_emoji_style.h"
#include "rift_store.h"
#include "rift_widgets.h"

#include <string.h>

/* Every emoji and every tab is a touch target (RIFT_TOUCH_H), four apart,
 * inside an 8 px frame; the panel keeps this far from the screen's edges
 * and from the button. The emoji images are one size (18 px, rift_emoji_img)
 * at every text size; only the caption grows with it. */
#define CELL_PX RIFT_TOUCH_H
#define CELL_GAP 4
#define FRAME_PAD 8
#define EDGE_PX 16
#define ANCHOR_GAP 8
#define GRID_W (RIFT_EMOJI_COLS * CELL_PX + (RIFT_EMOJI_COLS - 1) * CELL_GAP)

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
    lv_obj_t *tab[RIFT_EMOJI_GROUPS];
    lv_obj_t *cell[RIFT_EMOJI_CELLS];
    const char *item[RIFT_EMOJI_CELLS];
    lv_obj_t *field;  /* the text area an emoji goes into */
    lv_obj_t *anchor; /* the button it was opened from */
    int had_keys;     /* the field has had the keys since it opened */
    unsigned n;
    unsigned group;
    unsigned selected;
} pk;

static void field_deleted(lv_event_t *e);

/* An emoji on a label, as message text is drawn: folded, so a sequence with
 * a variation selector is the one image (rift_emoji_fold). */
static void set_emoji(lv_obj_t *label, const char *utf8)
{
    char shown[32];

    rift_emoji_fold(utf8 ? utf8 : "", shown, sizeof(shown));
    lv_label_set_text(label, shown);
}

/* A touch target holding one emoji, out of the focus bookkeeping. */
static lv_obj_t *emoji_target(lv_obj_t *parent, lv_event_cb_t cb, unsigned index)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_t *label = lv_label_create(b);

    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(b, CELL_PX, CELL_PX);
    pos_style_add(b, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    lv_obj_remove_style_all(label);
    pos_style_add(label, POS_STYLE_BUTTON_LABEL, 0);
    rift_emoji_style_add(label, POS_STYLE_BUTTON_LABEL);
    lv_label_set_text(label, "");
    lv_obj_center(label);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
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
            set_emoji(lv_obj_get_child(pk.cell[i], 0), pk.item[i]);
            lv_obj_remove_flag(pk.cell[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            pk.item[i] = NULL;
            lv_obj_add_flag(pk.cell[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
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

static void on_cell(lv_event_t *e)
{
    choose((unsigned)(uintptr_t)lv_event_get_user_data(e));
}

static void on_tab(lv_event_t *e)
{
    show_group((unsigned)(uintptr_t)lv_event_get_user_data(e));
}

static void on_scrim(lv_event_t *e)
{
    (void)e;
    rift_emoji_picker_close();
}

/* Above the button when there is room - the composer is at the foot of the
 * screen in both shapes - and below it otherwise; its right edge on the
 * button's, and never past an edge of the screen. */
static void place(void)
{
    int32_t sw = lv_display_get_horizontal_resolution(NULL);
    int32_t sh = lv_display_get_vertical_resolution(NULL);
    int32_t w;
    int32_t h;
    int32_t x;
    int32_t y;
    lv_area_t a;

    lv_obj_update_layout(pk.box);
    w = lv_obj_get_width(pk.box);
    h = lv_obj_get_height(pk.box);
    lv_obj_get_coords(pk.anchor ? pk.anchor : pk.field, &a);
    x = a.x2 + 1 - w;
    if (x + w > sw - EDGE_PX) {
        x = sw - EDGE_PX - w;
    }
    if (x < EDGE_PX) {
        x = EDGE_PX;
    }
    y = a.y1 - ANCHOR_GAP - h;
    if (y < EDGE_PX) {
        y = a.y2 + ANCHOR_GAP;
    }
    if (y + h > sh - EDGE_PX) {
        y = sh - EDGE_PX - h;
    }
    lv_obj_set_pos(pk.box, x, y);
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return o;
}

bool rift_emoji_picker_open(lv_obj_t *field, lv_obj_t *anchor)
{
    lv_obj_t *body;
    lv_obj_t *tabs;
    lv_obj_t *grid;
    unsigned i;

    if (!field || lv_obj_has_state(field, LV_STATE_DISABLED) || !lv_obj_is_visible(field)) {
        return false;
    }
    rift_emoji_picker_close();
    pk.field = field;
    pk.anchor = anchor;
    lv_obj_add_event_cb(field, field_deleted, LV_EVENT_DELETE, NULL);

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
    lv_obj_set_size(pk.box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pk.box, LV_FLEX_FLOW_COLUMN);
    /* A tap between two emoji is not a tap outside. */
    lv_obj_add_flag(pk.box, LV_OBJ_FLAG_CLICKABLE);

    /* The group's name, one line across the panel, "..." where a larger
     * text size does not fit it (DS §46.5). */
    pk.title = lv_label_create(pk.box);
    lv_obj_remove_style_all(pk.title);
    pos_style_add(pk.title, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(pk.title, CELL_PX + FRAME_PAD + GRID_W);
    lv_label_set_text(pk.title, "");
    pocketui_label_fit(pk.title, 1);

    body = plain(pk.box);
    lv_obj_set_size(body, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, FRAME_PAD, 0);

    /* The groups' tabs, one per row of the grid. */
    tabs = plain(body);
    lv_obj_set_size(tabs, CELL_PX, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tabs, CELL_GAP, 0);
    for (i = 0; i < RIFT_EMOJI_GROUPS; i++) {
        pk.tab[i] = emoji_target(tabs, on_tab, i);
        set_emoji(lv_obj_get_child(pk.tab[i], 0), rift_emoji_groups[i].icon);
    }

    grid = plain(body);
    lv_obj_set_size(grid, GRID_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, CELL_GAP, 0);
    lv_obj_set_style_pad_row(grid, CELL_GAP, 0);
    for (i = 0; i < RIFT_EMOJI_CELLS; i++) {
        pk.cell[i] = emoji_target(grid, on_cell, i);
    }

    show_group(0);
    place();
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
    case LV_KEY_UP:
        /* Past the top row is the group before, on its bottom row: the
         * arrows reach every emoji without a key of their own for the tabs
         * (Tab is the focus group's, LV_KEY_NEXT). */
        if (pk.selected >= RIFT_EMOJI_COLS) {
            pk.selected -= RIFT_EMOJI_COLS;
        } else {
            unsigned col = pk.selected;

            show_group((pk.group + RIFT_EMOJI_GROUPS - 1) % RIFT_EMOJI_GROUPS);
            pk.selected = pk.n > col ? col + (pk.n - 1 - col) / RIFT_EMOJI_COLS * RIFT_EMOJI_COLS : 0;
        }
        break;
    case LV_KEY_DOWN:
        if (pk.selected + RIFT_EMOJI_COLS < pk.n) {
            pk.selected += RIFT_EMOJI_COLS;
        } else {
            unsigned col = pk.selected % RIFT_EMOJI_COLS;

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
    rift_emoji_style_add(lv_obj_get_child(b, 0), POS_STYLE_BUTTON_LABEL);
    set_emoji(lv_obj_get_child(b, 0), BUTTON_ICON);
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
