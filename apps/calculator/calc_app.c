/*
 * PocketCalculator: a display and a keypad. Simple, not scientific - the
 * four operations with ordinary precedence, a decimal point, +/-, backspace
 * and clear - and nothing kept: closing the app forgets the calculation, and
 * tests/calculator_lint.sh fails the build if a store, a clock or a socket
 * ever appears here.
 *
 * This file is the screen and nothing else. Every decision about what a key
 * does belongs to calc_engine.c, every decision about what the display says
 * to calc_view.c, and both are pure and tested on the host; what is here is
 * the panel, nineteen keys and the one place keys arrive.
 *
 * INPUT. The keypad is the input, so no touch keyboard is shown or asked
 * for. The display panel is the single object in the shell's focus group
 * (DS v0.1 section 17.2), and whatever key the stream delivers to it - from
 * the simulator's keyboard, a physical keyboard or anything else that pushes
 * into pos_input - goes through calc_view_action_for_key() into the same
 * calc_apply() a tap on the keypad reaches. Nothing here knows or asks where
 * a key came from (section 17.4). The keypad's buttons do not take focus
 * when tapped, so typing and tapping can be mixed freely and the sink never
 * loses the keys.
 *
 * No focus ring is drawn on the sink. It is the only focusable object and is
 * focused for as long as the app is open, so a ring would be permanent and
 * would say nothing.
 *
 * MOTION. None. A key press is the pressed role style while a finger is down
 * and a relabelled display when it lifts, both immediate: there is nothing
 * for the reduced-motion setting to switch off (DS section 12).
 *
 * LAYOUT. One screen in two shapes, chosen from the box the app is given and
 * chosen again whenever that box changes size (see "the layout" below): the
 * display above the keypad when the box is tall, beside it when the box is
 * wide. The orientation is the system's (DS section 21.2); nothing here asks
 * what it is, only how much room there is.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "calc_view.h"

#include "app.h"
#include "pocketui.h"

#include <string.h>

/* The view names LVGL's three non-printing keys by number so that it can be
 * tested without LVGL. If LVGL ever renumbers them, this stops the build
 * rather than the Enter key. */
_Static_assert(CALC_KEY_ENTER == LV_KEY_ENTER, "calc_view.h Enter is not LVGL's");
_Static_assert(CALC_KEY_ESC == LV_KEY_ESC, "calc_view.h Esc is not LVGL's");
_Static_assert(CALC_KEY_BACKSPACE == LV_KEY_BACKSPACE, "calc_view.h Backspace is not LVGL's");

/* The keypad when the box is tall: four columns across the 528 px body (568
 * less the 20 px side padding of DS section 7), 8 px apart - the paired-button
 * gap of the same section - so each key is (528 - 3 * 8) / 4 = 126 px wide,
 * and 0 is two of them and a gap, 260. Five rows of 128 px with the same gap
 * make the keypad 5 * 128 + 4 * 8 = 672 px tall: keys twice the 64 px minimum
 * in height and nearly so in width, on a keypad a thumb works from the foot
 * of the panel. */
#define KEY_H 128
#define KEY_GAP 8
#define PAD_H (CALC_PAD_ROWS * KEY_H + (CALC_PAD_ROWS - 1) * KEY_GAP)
_Static_assert(KEY_H >= POCKETUI_TOUCH_MIN, "a key must meet the DS 64 px touch minimum");

/* Five rows at the touch minimum and their gaps. A wide box gets the display
 * beside the keypad only when it is at least this tall, so that no key there
 * can come out under 64 px (see "the layout" for what a shorter box gets). */
#define PAD_MIN_H (CALC_PAD_ROWS * POCKETUI_TOUCH_MIN + (CALC_PAD_ROWS - 1) * KEY_GAP)

/* Between the expression line and the number, inside the panel. */
#define DISPLAY_LINE_GAP 8

/* Room left beside the fitted expression. Its glyph widths are summed the
 * way LVGL lays them out, but kerning is not, and a line two pixels too wide
 * would be clipped instead of ellipsised. */
#define FIT_SLACK 4

struct calc_app {
    struct calc_engine engine;
    lv_obj_t *frame;      /* the app's own box in the body: display and keypad */
    lv_obj_t *display;    /* the panel: both lines, and the key sink */
    lv_obj_t *keypad;
    lv_area_t laid_out;   /* the frame's area when the layout was last chosen */
    lv_obj_t *expression; /* the line above */
    lv_obj_t *number;     /* the main line */
    /* The whole expression, before it is fitted to the line. Kept so a
     * theme or mode change, which can change the line's font, can fit it
     * again without asking the engine. */
    char expression_text[CALC_EXPRESSION_MAX];
};

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* ---- painting ---------------------------------------------------------- */

static int32_t glyph_width(uint32_t codepoint, void *user)
{
    lv_obj_t *label = user;
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);

    if (!font) {
        return 0;
    }
    return (int32_t)lv_font_get_glyph_width(font, codepoint, 0) +
           lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
}

/* The expression line, cut from the left with an ellipsis when it is wider
 * than the panel (calc_view_fit_left). */
static void fit_expression(struct calc_app *a)
{
    char fitted[CALC_EXPRESSION_MAX];
    int32_t room = lv_obj_get_content_width(a->display);

    if (room <= 0 && a->expression_text[0]) {
        lv_obj_update_layout(a->display);
        room = lv_obj_get_content_width(a->display);
    }
    calc_view_fit_left(a->expression_text, room - FIT_SLACK, glyph_width, a->expression,
                       fitted, sizeof(fitted));
    set_text(a->expression, fitted);
}

static void refresh(struct calc_app *a)
{
    char text[CALC_DISPLAY_MAX];

    calc_view_display(&a->engine, text, sizeof(text));
    set_text(a->number, text);
    calc_view_expression(&a->engine, a->expression_text, sizeof(a->expression_text));
    fit_expression(a);
}

/* ---- events ------------------------------------------------------------ */

/* A key from the stream. Unmapped keys are CALC_ACT_NONE and do nothing. */
static void on_key(lv_event_t *e)
{
    struct calc_app *a = lv_event_get_user_data(e);
    enum calc_action action = calc_view_action_for_key(lv_event_get_key(e));

    if (action != CALC_ACT_NONE && calc_apply(&a->engine, action)) {
        refresh(a);
    }
}

/* A finger on the keypad. The key carries its action. */
static void on_pad(lv_event_t *e)
{
    struct calc_app *a = lv_event_get_user_data(e);
    lv_obj_t *key = lv_event_get_current_target(e);
    enum calc_action action = (enum calc_action)(intptr_t)lv_obj_get_user_data(key);

    if (calc_apply(&a->engine, action)) {
        refresh(a);
    }
}

/* The theme engine rewrote the shared styles. In Outdoor the body font is
 * 20 px instead of 16, so an expression fitted for one no longer fits the
 * other. */
static void on_expression_style(lv_event_t *e)
{
    struct calc_app *a = lv_event_get_user_data(e);

    if (a->expression_text[0]) {
        fit_expression(a);
    }
}

/* The panel changed width - the layout changed shape - so the expression has
 * a different line to fit. */
static void on_display_size(lv_event_t *e)
{
    struct calc_app *a = lv_event_get_user_data(e);

    if (a->expression_text[0]) {
        fit_expression(a);
    }
}

/* ---- building ---------------------------------------------------------- */

/* The panel, taking whatever room the keypad leaves (the layout below says
 * which way), with the two lines at its foot, right-aligned as a
 * calculator's are. */
static void build_display(struct calc_app *a, lv_obj_t *root)
{
    lv_obj_t *panel = pocketui_card(root);

    lv_obj_set_flex_grow(panel, 1);
    lv_obj_add_event_cb(panel, on_display_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(panel, DISPLAY_LINE_GAP, 0);

    /* Secondary information in the secondary role; body type, so it follows
     * Outdoor's 20 px like every other body line. */
    a->expression = pocketui_label(panel, "", POS_STYLE_TEXT_SECONDARY);
    lv_obj_set_width(a->expression, LV_PCT(100));
    lv_label_set_long_mode(a->expression, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(a->expression, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_event_cb(a->expression, on_expression_style, LV_EVENT_STYLE_CHANGED, a);

    /* The number in hero-48. Sixteen characters of it are at most 16 * 28.8
     * px wide less the -1 px tracking, 445 px, inside the 484 px the panel
     * leaves in Outdoor in the tall layout (the wide one leaves more), which
     * is why calc_format_number stops at sixteen. */
    a->number = pocketui_label(panel, "0", POS_STYLE_HERO_48);
    lv_obj_set_width(a->number, LV_PCT(100));
    lv_label_set_long_mode(a->number, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(a->number, LV_TEXT_ALIGN_RIGHT, 0);

    /* The key sink. */
    lv_obj_add_event_cb(panel, on_key, LV_EVENT_KEY, a);
    pos_input_add_obj(panel);
    pos_input_focus(panel);
    a->display = panel;
}

static bool is_operator(enum calc_action action)
{
    return action == CALC_ACT_ADD || action == CALC_ACT_SUB || action == CALC_ACT_MUL ||
           action == CALC_ACT_DIV;
}

static bool is_function(enum calc_action action)
{
    return action == CALC_ACT_CLEAR || action == CALC_ACT_BACKSPACE ||
           action == CALC_ACT_NEGATE;
}

static void build_key(struct calc_app *a, lv_obj_t *pad, const struct calc_pad_key *k)
{
    lv_obj_t *key = lv_button_create(pad);
    lv_obj_t *face;
    const char *label = calc_view_key_label(k->action);

    lv_obj_remove_style_all(key);
    /* The type role goes on the key before the fill role, and the label
     * inherits both. The fill role sets the text colour for its fill -
     * text_on_accent on =, text_primary on a slab - and, added later, wins
     * over the type role's; the type role's font is left alone. DS section 3
     * has no keycap size: row-title's 20 px suits the 52 px keys of the
     * touch keyboard and would be lost on a 126 x 120 one, so the keys use
     * hero-40. */
    pos_style_add(key, POS_STYLE_HERO_40, 0);
    if (k->action == CALC_ACT_EQUALS) {
        /* The one accent fill on the screen: the primary button (DS 1). */
        pos_style_add(key, POS_STYLE_BUTTON_PRIMARY, 0);
        pos_style_add(key, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    } else if (is_function(k->action)) {
        /* Clear, backspace and +/- edit rather than enter: secondary
         * buttons, a hairline apart from the digits. */
        pos_style_add(key, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(key, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    } else {
        pos_style_add(key, POS_STYLE_SLAB, 0);
        if (is_operator(k->action)) {
            pos_style_add(key, POS_STYLE_ACCENT_TEXT, 0);
        }
        pos_style_add(key, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
    lv_obj_set_grid_cell(key, LV_GRID_ALIGN_STRETCH, k->col, k->span, LV_GRID_ALIGN_STRETCH,
                         k->row, 1);
    lv_obj_add_flag(key, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(key, LV_OBJ_FLAG_SCROLLABLE);
    /* A tap must not move focus off the display, or the next typed key
     * would go nowhere (DS 17.2). */
    lv_obj_clear_flag(key, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_user_data(key, (void *)(intptr_t)k->action);
    lv_obj_add_event_cb(key, on_pad, LV_EVENT_CLICKED, a);

    face = lv_label_create(key);
    if (label) {
        lv_label_set_text(face, label);
    } else {
        lv_label_set_text(face, LV_SYMBOL_BACKSPACE);
        pos_style_add(face, POS_STYLE_SYMBOL_LARGE, 0);
    }
    lv_obj_center(face);
}

/* The keypad's grid: four equal columns, and five rows that are either the
 * fixed KEY_H of the tall layout or equal shares of the height in the wide
 * one (see the layout below). */
static const int32_t pad_cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                    LV_GRID_TEMPLATE_LAST };
static const int32_t pad_rows_tall[] = { KEY_H, KEY_H, KEY_H, KEY_H, KEY_H, LV_GRID_TEMPLATE_LAST };
static const int32_t pad_rows_wide[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                         LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };

/* Built tall; the layout turns it when the box is wide. */
static void build_keypad(struct calc_app *a, lv_obj_t *root)
{
    lv_obj_t *pad = lv_obj_create(root);
    int i;

    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, LV_PCT(100), PAD_H);
    lv_obj_set_style_pad_column(pad, KEY_GAP, 0);
    lv_obj_set_style_pad_row(pad, KEY_GAP, 0);
    lv_obj_set_grid_dsc_array(pad, pad_cols, pad_rows_tall);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
    /* The keys in the outer columns and the bottom row sit on the keypad's
     * edge, and the 2 px pressed outline is drawn outside a key. Clipped to
     * the keypad, a pressed 7 or 0 would lose one side of it; the body's own
     * 20 px padding is where it lands instead. */
    lv_obj_add_flag(pad, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    for (i = 0; i < CALC_PAD_KEYS; i++) {
        build_key(a, pad, &calc_view_pad[i]);
    }
    a->keypad = pad;
}

/* ---- the layout -------------------------------------------------------- *
 *
 * The frame is the whole of the body the shell gives the app, and the layout
 * is chosen from its size alone:
 *
 *   TALL (portrait: 528 x 1060 on the reference panel). The display above the
 *   keypad, the keypad the five 128 px rows of KEY_H, the display taking the
 *   rest. The v0.0.9 layout.
 *
 *   WIDE (landscape: 1192 x 396). Five 128 px rows cannot fit, and a keypad
 *   squeezed under a display would leave rows of 40 px. So the display sits
 *   beside the keypad instead, the two sharing the width equally with the
 *   DS section 7 20 px gutter between them, and the five rows share the full
 *   height: about 70 px each, the same 19 keys in the same places, each wider
 *   than its portrait self. Half the width is still wider than the portrait
 *   display, so the number fits it as it fits there. Chosen only when every
 *   row still gets the 64 px touch minimum (PAD_MIN_H).
 *
 *   SHORT (no panel here gives one; a body with the keyboard sheet's height
 *   taken off would). Neither of the two fits, so the tall layout is kept
 *   whole, the display at its own height, and the frame scrolls - what the
 *   body did with the v0.0.9 layout - rather than a key shrinking below the
 *   minimum or disappearing.
 *
 * Whatever the shape, the keypad clears the panel's unsafe area (DS 21.1).
 * The body's own padding clears the straight edges, but a rounded corner
 * square is 30 px on the reference panel and the body's padding at the foot
 * is 20, so the keys in the bottom row would reach 10 px into the corner
 * squares at both ends. The frame therefore pads its foot by however far a
 * corner square reaches into it, measured from the platform's description
 * (pos_display.h), not assumed: on a panel with square corners the pad is 0
 * and the tall layout is the v0.0.9 layout to the pixel. The foot, rather
 * than the sides, gives way because the sides line up with the header's Back
 * button and the DS 7 body edges, and a few pixels of height cost less than
 * breaking that line. */

/* How far the unsafe area - straight-edge strips and corner squares - reaches
 * into box, which is in display coordinates. A strip insets the side it lies
 * along; a corner square insets the top or the foot of the box. */
static struct pos_insets unsafe_insets(const lv_area_t *box)
{
    const struct pos_display_geometry *g = pocketui_display_geometry();
    const struct pos_corners *c = &g->corners;
    struct pos_insets in = {
        .left = LV_MAX(0, g->edges.left - box->x1),
        .top = LV_MAX(0, g->edges.top - box->y1),
        .right = LV_MAX(0, box->x2 - (g->width - 1 - g->edges.right)),
        .bottom = LV_MAX(0, box->y2 - (g->height - 1 - g->edges.bottom)),
    };

    if (c->top_left > 0 && box->x1 < c->top_left && box->y1 < c->top_left) {
        in.top = LV_MAX(in.top, c->top_left - box->y1);
    }
    if (c->top_right > 0 && box->x2 >= g->width - c->top_right && box->y1 < c->top_right) {
        in.top = LV_MAX(in.top, c->top_right - box->y1);
    }
    if (c->bottom_right > 0 && box->x2 >= g->width - c->bottom_right &&
        box->y2 >= g->height - c->bottom_right) {
        in.bottom = LV_MAX(in.bottom, box->y2 - (g->height - c->bottom_right) + 1);
    }
    if (c->bottom_left > 0 && box->x1 < c->bottom_left && box->y2 >= g->height - c->bottom_left) {
        in.bottom = LV_MAX(in.bottom, box->y2 - (g->height - c->bottom_left) + 1);
    }
    return in;
}

static void layout(struct calc_app *a)
{
    lv_area_t box;
    struct pos_insets in;
    int32_t w;
    int32_t h;
    bool wide;
    bool fits;

    lv_obj_get_coords(a->frame, &box);
    if (lv_area_get_width(&box) <= 0 || lv_area_get_height(&box) <= 0 ||
        memcmp(&box, &a->laid_out, sizeof(box)) == 0) {
        return;
    }
    a->laid_out = box;
    in = unsafe_insets(&box);
    lv_obj_set_style_pad_left(a->frame, in.left, 0);
    lv_obj_set_style_pad_top(a->frame, in.top, 0);
    lv_obj_set_style_pad_right(a->frame, in.right, 0);
    lv_obj_set_style_pad_bottom(a->frame, in.bottom, 0);
    w = lv_area_get_width(&box) - in.left - in.right;
    h = lv_area_get_height(&box) - in.top - in.bottom;
    wide = w > h && h >= PAD_MIN_H;
    fits = wide || h >= PAD_H + POCKETUI_PAD;

    lv_obj_set_flex_flow(a->frame, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(a->display, fits ? 1 : 0);
    lv_obj_set_size(a->display, LV_PCT(100), wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(a->keypad, wide ? 1 : 0);
    lv_obj_set_size(a->keypad, LV_PCT(100), wide ? LV_PCT(100) : PAD_H);
    lv_obj_set_grid_dsc_array(a->keypad, pad_cols, wide ? pad_rows_wide : pad_rows_tall);
    if (fits) {
        /* Everything is in view: nothing to scroll, and the keypad's pressed
         * outlines may draw past the frame into the body's padding. */
        lv_obj_scroll_to_y(a->frame, 0, LV_ANIM_OFF);
        lv_obj_clear_flag(a->frame, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(a->frame, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    } else {
        /* Scrolled keys must not draw over the header. */
        lv_obj_add_flag(a->frame, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(a->frame, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    }
}

/* The frame is the body's content box, so this is the body changing size:
 * the shell's content area was resized, or this is the first layout pass
 * after the app was built. */
static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

static void build_frame(struct calc_app *a, lv_obj_t *root)
{
    lv_obj_t *frame = lv_obj_create(root);

    lv_obj_remove_style_all(frame);
    /* Exactly the body's content box, whatever is in it, so the layout is
     * always chosen from the room the shell gives and never from the size of
     * what the layout itself put there. */
    lv_obj_set_size(frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_gap(frame, POCKETUI_PAD, 0);
    lv_obj_set_scroll_dir(frame, LV_DIR_VER);
    lv_obj_add_event_cb(frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    a->frame = frame;
}

/* ---- the app ----------------------------------------------------------- */

static void *calculator_create(lv_obj_t *root)
{
    struct calc_app *a = lv_malloc_zeroed(sizeof(*a));

    if (!a) {
        return NULL;
    }
    calc_init(&a->engine);
    build_frame(a, root);
    build_display(a, a->frame);
    build_keypad(a, a->frame);
    lv_obj_update_layout(a->frame);
    layout(a);
    refresh(a);
    return a;
}

static void calculator_destroy(void *priv)
{
    /* The shell deletes the objects under root, which also takes the sink
     * out of the focus group. There is no timer, and nothing to save: a
     * calculation is not kept. */
    lv_free(priv);
}

LV_IMAGE_DECLARE(pos_app_icon_calculator);

const struct pocketos_app app_calculator = {
    .id = "calculator",
    .name = "Calculator",
    /* The launcher draws the Doors icon (DS section 20). The glyph stays as
     * the app's text icon. LVGL's symbol font has no calculator; the plus
     * sign is the nearest arithmetic glyph in it. The keyboard glyph was the
     * other candidate and was passed over because PocketOS has a touch
     * keyboard and a physical one, and a keyboard would read as either. */
    .icon = LV_SYMBOL_PLUS,
    .icon_mask = &pos_app_icon_calculator,
    .create = calculator_create,
    .tick = NULL,
    .destroy = calculator_destroy,
};
