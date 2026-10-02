/*
 * PocketUI widgets on shared role styles. See pocketui.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketui.h"

#include <string.h>

/* Until the shell says otherwise: the reference panel, upright, every pixel
 * visible - so a test or tool that builds widgets without a shell lays out
 * exactly as before safe areas existed. */
static struct pos_display_geometry geometry = {
    .rotation = POS_ROTATION_0,
    .native_width = 568,
    .native_height = 1232,
    .width = 568,
    .height = 1232,
};

void pocketui_init(void)
{
    pos_styles_init();
}

void pocketui_style_screen(lv_obj_t *screen)
{
    pos_style_add(screen, POS_STYLE_SCREEN, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

void pocketui_set_display_geometry(const struct pos_display_geometry *g)
{
    geometry = *g;
}

const struct pos_display_geometry *pocketui_display_geometry(void)
{
    return &geometry;
}

void pocketui_apply_bar_insets(lv_obj_t *bar, enum pos_edge edge)
{
    struct pos_insets in = pos_display_bar_insets(&geometry, edge);
    int32_t left = lv_obj_get_style_pad_left(bar, LV_PART_MAIN);
    int32_t right = lv_obj_get_style_pad_right(bar, LV_PART_MAIN);
    int32_t top = lv_obj_get_style_pad_top(bar, LV_PART_MAIN);
    int32_t bottom = lv_obj_get_style_pad_bottom(bar, LV_PART_MAIN);

    /* The bar keeps its own padding wherever that already clears the unsafe
     * area; only an inset larger than it moves the content. */
    lv_obj_set_style_pad_left(bar, LV_MAX(left, in.left), LV_PART_MAIN);
    lv_obj_set_style_pad_right(bar, LV_MAX(right, in.right), LV_PART_MAIN);
    lv_obj_set_style_pad_top(bar, LV_MAX(top, in.top), LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(bar, LV_MAX(bottom, in.bottom), LV_PART_MAIN);
}

/* ---- The top-left way back (DS §48) ----------------------------------- */

/* The corner is drawn and touched as one box, the face: a child of the slab
 * from POCKETUI_BACK_BLEED beyond the screen's top and left edges to the
 * slab's right edge and POCKETUI_BACK_FOOT below its bottom edge. Past the
 * edges its own rounded corner is off the screen, so on the glass its
 * top-left is the panel's own rounded corner, whatever its radius; its
 * right and bottom edges, the only ones seen, carry the hairline and the
 * slab radius. The slab itself draws nothing and keeps its place in the
 * row, so the title does not move. Everything is relative to the slab, so
 * a relayout (text size, the keyboard) moves the face with it and nothing
 * is measured; a rotation builds the screen again.
 *
 * LVGL looks for a touch among a box's children only inside the box, or,
 * when its overflow is visible, inside its extra drawing area; the slab
 * declares that area as far out as the face goes. The face is floating, so
 * it never counts for a layout or for scrolling, and it passes its events up
 * to the slab: CLICKED reaches the slab's own handler, and LVGL's PRESSED,
 * RELEASED and PRESS_LOST handling presses and releases the slab as each
 * passes; the slab's states trickle back down, so a key that presses the
 * slab lights the face too. One change does not pass as an event: a drag
 * that becomes a scroll takes PRESSED off the face directly, so the slab
 * follows the face's state as it changes.
 *
 * Not LVGL's extended click area: it grows a box by the same amount on all
 * four sides, over the title. Not its hit-test hook either: the struct it
 * fills is in a private header that the device's sysroot does not carry.
 * Not a per-corner radius: LVGL has one radius for all four corners. */
static void back_corner_event(lv_event_t *e)
{
    lv_obj_t *back = lv_event_get_current_target_obj(e);
    lv_obj_t *from = lv_event_get_target_obj(e);

    if (lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        lv_event_set_ext_draw_size(e, (int32_t)(intptr_t)lv_event_get_user_data(e));
    } else if (from != back && lv_obj_get_parent(from) == back &&
               lv_obj_has_state(from, LV_STATE_PRESSED) != lv_obj_has_state(back, LV_STATE_PRESSED)) {
        lv_obj_set_state(back, LV_STATE_PRESSED, lv_obj_has_state(from, LV_STATE_PRESSED));
    }
}

lv_obj_t *pocketui_back_corner(lv_obj_t *back, int32_t left, int32_t top, enum pos_style_role look,
                               enum pos_style_role pressed)
{
    int32_t w = lv_obj_get_style_width(back, LV_PART_MAIN);
    int32_t h = lv_obj_get_style_height(back, LV_PART_MAIN);
    int32_t centre;
    int32_t ext;
    lv_obj_t *face;
    uint32_t i;

    /* The slab's own size, set in pixels before this call (all three are
     * 72 x 56); a content or percent size has no pixels to reach from. */
    if (LV_COORD_IS_SPEC(w) || LV_COORD_IS_SPEC(h) || w <= 0 || h <= 0 || left < 0 || top < 0) {
        return NULL;
    }
    face = lv_obj_create(back);
    lv_obj_remove_style_all(face);
    pos_style_add(face, look, 0);
    pos_style_add(face, pressed, LV_STATE_PRESSED);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_EVENT_BUBBLE);
    /* A child's position counts from the slab's padding; the face's from the
     * slab's edge. */
    lv_obj_set_pos(face, -left - POCKETUI_BACK_BLEED - lv_obj_get_style_space_left(back, LV_PART_MAIN),
                   -top - POCKETUI_BACK_BLEED - lv_obj_get_style_space_top(back, LV_PART_MAIN));
    lv_obj_set_size(face, POCKETUI_BACK_BLEED + left + w, POCKETUI_BACK_BLEED + top + h + POCKETUI_BACK_FOOT);
    /* Drawn under the chevron. */
    lv_obj_move_to_index(face, 0);

    /* The chevron in the middle of what is seen of the face, but never
     * nearer the screen's edge than 14 px inside the slab (it stays clear of
     * the corner's inset whatever the inset is). */
    centre = (w - left) / 2;
    if (centre < 14) {
        centre = 14;
    }
    for (i = 0; i < lv_obj_get_child_count(back); i++) {
        lv_obj_t *c = lv_obj_get_child(back, (int32_t)i);

        if (lv_obj_check_type(c, &lv_label_class)) {
            lv_obj_align(c, LV_ALIGN_CENTER, centre - w / 2, (POCKETUI_BACK_FOOT - top) / 2);
        }
    }

    ext = (left > top ? left : top) + POCKETUI_BACK_BLEED;
    lv_obj_add_flag(back, LV_OBJ_FLAG_OVERFLOW_VISIBLE | LV_OBJ_FLAG_STATE_TRICKLE);
    lv_obj_add_event_cb(back, back_corner_event, LV_EVENT_REFR_EXT_DRAW_SIZE, (void *)(intptr_t)ext);
    lv_obj_add_event_cb(back, back_corner_event, LV_EVENT_STATE_CHANGED, NULL);
    lv_obj_refresh_ext_draw_size(back);
    return face;
}

/* ---- Responsive layout guard (DS §21.3, §22.2) ------------------------ */

/* Field by field, not memcmp: lv_area_t and struct pos_insets are plain
 * coordinate records, but a memcmp compares whatever padding the compiler put
 * between or after their members as well, and padding is not part of the
 * answer. What the layout was chosen from is these eight numbers. */
static bool area_same(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 && a->y2 == b->y2;
}

static bool insets_same(const struct pos_insets *a, const struct pos_insets *b)
{
    return a->left == b->left && a->top == b->top && a->right == b->right &&
           a->bottom == b->bottom;
}

bool pocketui_layout_begin(struct pocketui_layout_guard *guard, lv_obj_t *frame,
                           struct pos_insets *insets)
{
    lv_area_t box;
    struct pos_insets in;

    if (!guard || !frame || !insets) {
        return false;
    }
    lv_obj_get_coords(frame, &box);
    /* No area yet: nothing to lay out in, and nothing worth remembering
     * either. The guard is left exactly as it was, so a first real pass is
     * still treated as the first one. */
    if (lv_area_get_width(&box) <= 0 || lv_area_get_height(&box) <= 0) {
        return false;
    }
    in = pos_display_rect_insets(pocketui_display_geometry(), box.x1, box.y1, box.x2, box.y2);
    /* Nothing the layout is chosen from has changed, so there is nothing to
     * do - and a pass that ran anyway would be the whole cost of the app's
     * layout repeated on every one. */
    if (guard->valid && area_same(&box, &guard->area) && insets_same(&in, &guard->insets)) {
        return false;
    }
    guard->area = box;
    guard->insets = in;
    guard->valid = true;
    *insets = in;
    return true;
}

void pocketui_layout_guard_reset(struct pocketui_layout_guard *guard)
{
    if (!guard) {
        return;
    }
    memset(guard, 0, sizeof(*guard));
}

lv_obj_t *pocketui_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);

    lv_obj_remove_style_all(card);
    pos_style_add(card, POS_STYLE_PANEL, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *pocketui_tile(lv_obj_t *parent, const char *icon, const char *label,
                        lv_event_cb_t on_click, void *user_data)
{
    return pocketui_tile_mask(parent, NULL, icon, label, on_click, user_data);
}

lv_obj_t *pocketui_tile_mask(lv_obj_t *parent, const lv_image_dsc_t *mask, const char *icon,
                             const char *label, lv_event_cb_t on_click, void *user_data)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_t *ic;
    lv_obj_t *lb;

    lv_obj_remove_style_all(tile);
    pos_style_add(tile, POS_STYLE_SLAB, 0);
    pos_style_add(tile, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(tile, 12, 0);
    lv_obj_set_size(tile, LV_PCT(100), POCKETUI_TILE_H);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) {
        lv_obj_add_event_cb(tile, on_click, LV_EVENT_CLICKED, user_data);
    }
    /* Either way the icon is the tile's first child, top-left in the 12 px
     * inset, and not clickable, so a tap on it is a tap on the tile. */
    if (mask) {
        ic = lv_image_create(tile);
        lv_obj_remove_style_all(ic);
        pos_style_add(ic, POS_STYLE_APP_ICON, 0);
        lv_image_set_src(ic, mask);
    } else {
        ic = lv_label_create(tile);
        lv_label_set_text(ic, icon);
        pos_style_add(ic, POS_STYLE_SYMBOL_LARGE, 0);
        pos_style_add(ic, POS_STYLE_ACCENT_TEXT, 0);
    }
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);
    lb = lv_label_create(tile);
    lv_label_set_text(lb, label);
    pos_style_add(lb, POS_STYLE_ROW_TITLE, 0);
    lv_obj_align(lb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return tile;
}

lv_obj_t *pocketui_kv_row(lv_obj_t *parent, const char *key, const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *k;
    lv_obj_t *v;

    lv_obj_remove_style_all(row);
    pos_style_add(row, POS_STYLE_DIVIDER, 0);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, POCKETUI_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    k = lv_label_create(row);
    lv_label_set_text(k, key);
    pos_style_add(k, POS_STYLE_TEXT_SECONDARY, 0);
    v = lv_label_create(row);
    lv_label_set_text(v, value);
    pos_style_add(v, POS_STYLE_VALUE, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(v, LV_PCT(60), 0);
    return v;
}

lv_obj_t *pocketui_button(lv_obj_t *parent, const char *text, lv_event_cb_t on_click,
                          void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lb = lv_label_create(btn);

    lv_obj_remove_style_all(btn);
    pos_style_add(btn, POS_STYLE_BUTTON_PRIMARY, 0);
    pos_style_add(btn, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(lb, text);
    pos_style_add(lb, POS_STYLE_BUTTON_LABEL, 0);
    lv_obj_center(lb);
    if (on_click) {
        lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

lv_obj_t *pocketui_label(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *lb = lv_label_create(parent);

    lv_label_set_text(lb, text ? text : "");
    pos_style_add(lb, role, 0);
    return lb;
}

/* ---- Text metrics (DS §46) ---------------------------------------------- */

int32_t pocketui_text_width(lv_obj_t *label, const char *text)
{
    lv_point_t p;
    const lv_font_t *font = label ? lv_obj_get_style_text_font(label, LV_PART_MAIN) : NULL;

    if (!font || !text || !text[0]) {
        return 0;
    }
    lv_text_get_size(&p, text, font, lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    return p.x;
}

static const lv_font_t *role_font(enum pos_style_role role, int32_t *letter_space)
{
    lv_style_value_t v;

    *letter_space = 0;
    if (lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_LETTER_SPACE, &v) == LV_STYLE_RES_FOUND) {
        *letter_space = v.num;
    }
    if (lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_FONT, &v) == LV_STYLE_RES_FOUND) {
        return v.ptr;
    }
    return lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN);
}

int32_t pocketui_role_line_height(enum pos_style_role role)
{
    int32_t ls;
    const lv_font_t *font = role_font(role, &ls);

    return font ? lv_font_get_line_height(font) : 0;
}

int32_t pocketui_role_text_width(enum pos_style_role role, const char *text)
{
    lv_point_t p;
    int32_t ls;
    const lv_font_t *font = role_font(role, &ls);

    if (!font || !text || !text[0]) {
        return 0;
    }
    lv_text_get_size(&p, text, font, ls, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

void pocketui_label_fit(lv_obj_t *label, int lines)
{
    const lv_font_t *font = label ? lv_obj_get_style_text_font(label, LV_PART_MAIN) : NULL;

    if (!label || !font || lines < 1) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_height(label, lines * lv_font_get_line_height(font), 0);
}

void pocketui_label_rest_of_row(lv_obj_t *label)
{
    if (!label) {
        return;
    }
    lv_obj_set_width(label, 0);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    pocketui_label_fit(label, 1);
}

/* ---- Text field (DS §17.1) --------------------------------------------- */

#define POCKETUI_CARET_BLINK_MS 500 /* DS §17.1: 500 ms on, 500 ms off */
#define POCKETUI_FIELD_MIN_LINES 3

static bool reduced_motion;

void pocketui_set_reduced_motion(bool on)
{
    reduced_motion = on;
}

/* A tap focuses the field through the one focus model of DS §17.2, rather
 * than leaving LVGL's click-focus to run beside the group. */
static void field_clicked_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);

    if (!lv_obj_has_state(ta, LV_STATE_DISABLED)) {
        pos_input_focus(ta);
    }
}

lv_obj_t *pocketui_text_field(lv_obj_t *parent, const char *placeholder, bool single_line)
{
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_t *ta;

    lv_obj_remove_style_all(wrap);
    lv_obj_set_width(wrap, LV_PCT(100));
    lv_obj_set_height(wrap, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wrap, 8, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    ta = lv_textarea_create(wrap);
    lv_obj_remove_style_all(ta);
    pos_style_add(ta, POS_STYLE_FIELD, 0);
    pos_style_add(ta, POS_STYLE_FIELD_FOCUSED, LV_STATE_FOCUSED);
    pos_style_add(ta, POS_STYLE_FIELD_DISABLED, LV_STATE_DISABLED);
    pos_style_add(ta, POS_STYLE_FIELD_PLACEHOLDER, LV_PART_TEXTAREA_PLACEHOLDER);
    pos_style_add(ta, POS_STYLE_FIELD_CURSOR, LV_PART_CURSOR);
    lv_obj_set_width(ta, LV_PCT(100));

    lv_textarea_set_one_line(ta, single_line);
    if (single_line) {
        lv_obj_set_height(ta, POCKETUI_ROW_H);
    } else {
        /* Three body lines is the floor of DS §17.1. The field takes the
         * height its parent gives it and scrolls once the text passes that;
         * body line-height is 1.5 (DS §3). It grows into the wrapper rather
         * than being 100% of it, so an error caption takes its room from the
         * field: at 100% the caption was laid out under the wrapper's edge
         * and clipped away, and in a wrapper sized by its content the field
         * chased the caption's height without end. */
        const lv_font_t *f = lv_obj_get_style_text_font(ta, LV_PART_MAIN);
        int32_t line = f ? lv_font_get_line_height(f) : 16;
        int32_t min_h = POCKETUI_FIELD_MIN_LINES * line * 3 / 2;

        lv_obj_set_height(ta, min_h);
        lv_obj_set_style_min_height(ta, min_h, 0);
        lv_obj_set_flex_grow(ta, 1);
    }
    if (placeholder) {
        lv_textarea_set_placeholder_text(ta, placeholder);
    }

    /* Blinking caret, or a solid one under reduced motion (DS §12, §17.1). */
    lv_obj_set_style_anim_duration(ta, reduced_motion ? 0 : POCKETUI_CARET_BLINK_MS,
                                   LV_PART_CURSOR);

    lv_obj_add_event_cb(ta, field_clicked_cb, LV_EVENT_CLICKED, NULL);
    /* Deletion needs no handler: LVGL removes an object from its group in
     * the destructor, which clears the focus with it. */
    pos_input_add_obj(ta);
    return ta;
}

/* The caption lives in the wrapper, after the field. It is created the first
 * time an error is shown and hidden rather than deleted afterwards, so a
 * field toggling between valid and invalid does not churn objects. */
static lv_obj_t *field_error_label(lv_obj_t *ta, bool create)
{
    lv_obj_t *wrap = lv_obj_get_parent(ta);
    lv_obj_t *lb;

    if (!wrap) {
        return NULL;
    }
    if (lv_obj_get_child_count(wrap) > 1) {
        return lv_obj_get_child(wrap, 1);
    }
    if (!create) {
        return NULL;
    }
    lb = pocketui_label(wrap, "", POS_STYLE_STATUS_ERROR_TEXT);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_PCT(100));
    return lb;
}

void pocketui_text_field_set_error(lv_obj_t *field, const char *message)
{
    lv_obj_t *lb;

    if (!field) {
        return;
    }
    if (message && message[0]) {
        pos_style_add(field, POS_STYLE_FIELD_ERROR, 0);
        lb = field_error_label(field, true);
        if (lb) {
            lv_label_set_text(lb, message);
            lv_obj_clear_flag(lb, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    lv_obj_remove_style(field, pos_style(POS_STYLE_FIELD_ERROR), 0);
    lb = field_error_label(field, false);
    if (lb) {
        lv_obj_add_flag(lb, LV_OBJ_FLAG_HIDDEN);
    }
}

void pocketui_text_field_set_enabled(lv_obj_t *field, bool enabled)
{
    if (!field) {
        return;
    }
    if (enabled) {
        lv_obj_clear_state(field, LV_STATE_DISABLED);
        lv_obj_add_flag(field, LV_OBJ_FLAG_CLICKABLE);
        pos_input_add_obj(field);
        return;
    }
    /* Leaving the group is what gives up the focus: LVGL has no unfocused
     * state, so a disabled field is removed rather than merely defocused. */
    lv_group_remove_obj(field);
    lv_obj_add_state(field, LV_STATE_DISABLED);
    lv_obj_clear_flag(field, LV_OBJ_FLAG_CLICKABLE);
}
