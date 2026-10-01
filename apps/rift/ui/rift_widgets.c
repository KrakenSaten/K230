/*
 * RIFT's own small components. See rift_widgets.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_widgets.h"

#include "pocketui.h"
#include "pos_styles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLYPH_PX 8
#define CELL_PX 6
#define DASH_ON 2
#define DASH_OFF 2

/* ---- drawing helpers ---------------------------------------------------- */

static void fill_box(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t side, lv_color_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t a;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 2;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = color;
    a.x1 = cx - side / 2;
    a.y1 = cy - side / 2;
    a.x2 = a.x1 + side - 1;
    a.y2 = a.y1 + side - 1;
    lv_draw_rect(layer, &dsc, &a);
}

static void hollow_box(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t side, lv_color_t color,
                       int32_t border)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t a;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 2;
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = color;
    dsc.border_width = border;
    dsc.border_opa = LV_OPA_COVER;
    a.x1 = cx - side / 2;
    a.y1 = cy - side / 2;
    a.x2 = a.x1 + side - 1;
    a.y2 = a.y1 + side - 1;
    lv_draw_rect(layer, &dsc, &a);
}

static void seg(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                lv_color_t color, int32_t width, int dashed)
{
    lv_draw_line_dsc_t dsc;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.opa = LV_OPA_COVER;
    if (dashed) {
        dsc.dash_width = DASH_ON;
        dsc.dash_gap = DASH_OFF;
    }
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

/* A square whose outline is broken: the one thing on screen that says "this
 * is not a measurement, it is an absence". Four dashed sides rather than a
 * border, because LVGL has no dashed border. */
static void dashed_box(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t side, lv_color_t color)
{
    int32_t x1 = cx - side / 2;
    int32_t y1 = cy - side / 2;
    int32_t x2 = x1 + side - 1;
    int32_t y2 = y1 + side - 1;

    seg(layer, x1, y1, x2, y1, color, 1, 1);
    seg(layer, x2, y1, x2, y2, color, 1, 1);
    seg(layer, x2, y2, x1, y2, color, 1, 1);
    seg(layer, x1, y2, x1, y1, color, 1, 1);
}

/* ---- the link glyph ----------------------------------------------------- */

struct glyph_state {
    enum rift_glyph kind;
};

static void glyph_delete(lv_event_t *e)
{
    free(lv_obj_get_user_data(lv_event_get_target_obj(e)));
}

static void glyph_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    const struct glyph_state *s = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    int32_t cx;
    int32_t cy;

    if (!s || !layer) {
        return;
    }
    lv_obj_get_coords(obj, &area);
    cx = area.x1 + lv_area_get_width(&area) / 2;
    cy = area.y1 + lv_area_get_height(&area) / 2;
    switch (s->kind) {
    case RIFT_GLYPH_DIRECT:
        fill_box(layer, cx, cy, GLYPH_PX, pos_theme_color(POS_COLOR_RADIO_RX));
        break;
    case RIFT_GLYPH_RELAYED:
        hollow_box(layer, cx, cy, GLYPH_PX, pos_theme_color(POS_COLOR_TEXT_SECONDARY), 1);
        break;
    case RIFT_GLYPH_UNKNOWN:
        dashed_box(layer, cx, cy, GLYPH_PX, pos_theme_color(POS_COLOR_TEXT_MUTED));
        break;
    case RIFT_GLYPH_STALE:
        fill_box(layer, cx, cy, GLYPH_PX, pos_theme_color(POS_COLOR_TEXT_MUTED));
        break;
    case RIFT_GLYPH_CHANNEL: {
        /* The "#" of handoff §6, drawn rather than typed. A label here would
         * put a second font and a second baseline into a row whose height is
         * already fixed by the glyph box; four strokes in the same 8 px box
         * every other glyph uses sit exactly where those do. */
        lv_color_t c = pos_theme_color(POS_COLOR_TEXT_SECONDARY);
        int32_t h = GLYPH_PX / 2;
        int32_t q = GLYPH_PX / 4;

        seg(layer, cx - q, cy - h, cx - q, cy + h, c, 1, 0);
        seg(layer, cx + q, cy - h, cx + q, cy + h, c, 1, 0);
        seg(layer, cx - h, cy - q, cx + h, cy - q, c, 1, 0);
        seg(layer, cx - h, cy + q, cx + h, cy + q, c, 1, 0);
        break;
    }
    case RIFT_GLYPH_SELF:
    default:
        fill_box(layer, cx, cy, GLYPH_PX, pos_theme_color(POS_COLOR_TEXT_PRIMARY));
        hollow_box(layer, cx, cy, GLYPH_PX + 4, pos_theme_color(POS_COLOR_FOCUS), 2);
        break;
    }
}

lv_obj_t *rift_glyph_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct glyph_state *s = calloc(1, sizeof(*s));

    if (!s) {
        lv_obj_delete(obj);
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, RIFT_GLYPH_BOX, RIFT_GLYPH_BOX);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, s);
    s->kind = RIFT_GLYPH_UNKNOWN;
    lv_obj_add_event_cb(obj, glyph_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, glyph_delete, LV_EVENT_DELETE, NULL);
    return obj;
}

void rift_glyph_set(lv_obj_t *glyph, enum rift_glyph kind)
{
    struct glyph_state *s = glyph ? lv_obj_get_user_data(glyph) : NULL;

    if (!s || s->kind == kind) {
        return;
    }
    s->kind = kind;
    lv_obj_invalidate(glyph);
}

/* ---- the hop strip ------------------------------------------------------ */

struct strip_state {
    struct rift_strip_cell cell[RIFT_STRIP_MAX];
    int count;
    int direct;
    int known;
    int stale;
};

static void strip_delete(lv_event_t *e)
{
    free(lv_obj_get_user_data(lv_event_get_target_obj(e)));
}

static void strip_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    const struct strip_state *s = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    lv_color_t relay = pos_theme_color(POS_COLOR_TEXT_SECONDARY);
    lv_color_t muted = pos_theme_color(POS_COLOR_TEXT_MUTED);
    lv_color_t rx = pos_theme_color(POS_COLOR_RADIO_RX);
    lv_area_t area;
    int32_t x[RIFT_STRIP_MAX];
    int32_t cy;
    int32_t left;
    int32_t span;
    int i;

    if (!s || !layer || s->count < 2) {
        return;
    }
    lv_obj_get_coords(obj, &area);
    left = area.x1 + CELL_PX / 2;
    span = lv_area_get_width(&area) - CELL_PX;
    if (span < 0) {
        span = 0;
    }
    cy = area.y1 + lv_area_get_height(&area) / 2;
    for (i = 0; i < s->count; i++) {
        x[i] = left + (s->count > 1 ? span * i / (s->count - 1) : 0);
    }
    /* Connectors first, so the cells sit on top of them. A connector next
     * to a hop this path does not name is dashed on that side, which is how
     * the strip says "the route continues, and I do not know through what"
     * without inventing a hop. */
    for (i = 0; i + 1 < s->count; i++) {
        int unknown_side = s->cell[i].kind == RIFT_CELL_UNKNOWN ||
                           s->cell[i + 1].kind == RIFT_CELL_UNKNOWN || !s->known;
        lv_color_t c = unknown_side ? muted : (s->direct ? rx : relay);
        int32_t x1 = x[i] + (s->cell[i].kind == RIFT_CELL_MORE ? 10 : CELL_PX / 2);
        int32_t x2 = x[i + 1] - (s->cell[i + 1].kind == RIFT_CELL_MORE ? 10 : CELL_PX / 2);

        if (x2 > x1) {
            seg(layer, x1, cy, x2, cy, c, 1, unknown_side);
        }
    }
    for (i = 0; i < s->count; i++) {
        switch (s->cell[i].kind) {
        case RIFT_CELL_SELF:
            fill_box(layer, x[i], cy, CELL_PX, pos_theme_color(POS_COLOR_TEXT_PRIMARY));
            break;
        case RIFT_CELL_RELAY:
            hollow_box(layer, x[i], cy, CELL_PX, relay, 1);
            break;
        case RIFT_CELL_UNKNOWN:
            dashed_box(layer, x[i], cy, CELL_PX, muted);
            break;
        case RIFT_CELL_MORE: {
            lv_draw_label_dsc_t dsc;
            lv_area_t box;
            char text[8];

            if (!font) {
                break;
            }
            snprintf(text, sizeof(text), "+%d", s->cell[i].more);
            lv_draw_label_dsc_init(&dsc);
            dsc.color = muted;
            dsc.font = font;
            dsc.align = LV_TEXT_ALIGN_CENTER;
            dsc.text = text;
            box.x1 = x[i] - 12;
            box.x2 = x[i] + 12;
            box.y1 = cy - (int32_t)font->line_height / 2;
            box.y2 = box.y1 + (int32_t)font->line_height;
            lv_draw_label(layer, &dsc, &box);
            break;
        }
        case RIFT_CELL_TARGET:
        default:
            if (!s->known) {
                dashed_box(layer, x[i], cy, CELL_PX, muted);
            } else if (s->stale) {
                fill_box(layer, x[i], cy, CELL_PX, muted);
            } else {
                fill_box(layer, x[i], cy, CELL_PX, s->direct ? rx : relay);
            }
            break;
        }
    }
}

lv_obj_t *rift_strip_create(lv_obj_t *parent, int32_t width)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct strip_state *s = calloc(1, sizeof(*s));

    if (!s) {
        lv_obj_delete(obj);
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    /* Only so the "+n" cell has a font to draw with; no font is named here
     * (tests/style_lint.sh). */
    pos_style_add(obj, POS_STYLE_CAPTION, 0);
    lv_obj_set_size(obj, width, RIFT_ROW_H);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, s);
    lv_obj_add_event_cb(obj, strip_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, strip_delete, LV_EVENT_DELETE, NULL);
    return obj;
}

void rift_strip_set(lv_obj_t *strip, const struct rift_path *path, int stale)
{
    struct strip_state *s = strip ? lv_obj_get_user_data(strip) : NULL;
    struct strip_state next;

    if (!s || !path) {
        return;
    }
    memset(&next, 0, sizeof(next));
    next.count = rift_strip_build(path, next.cell, RIFT_STRIP_MAX);
    next.direct = path->direct;
    next.known = path->known;
    next.stale = stale ? 1 : 0;
    if (memcmp(&next, s, sizeof(next)) == 0) {
        return;
    }
    *s = next;
    lv_obj_invalidate(strip);
}

void rift_strip_set_width(lv_obj_t *strip, int32_t width)
{
    if (strip && lv_obj_get_width(strip) != width) {
        lv_obj_set_width(strip, width);
    }
}

/* ---- the activity pulse ------------------------------------------------- */

#define PULSE_DOT 4
#define PULSE_GAP 2

struct pulse_state {
    enum rift_pulse level;
};

static void pulse_delete(lv_event_t *e)
{
    free(lv_obj_get_user_data(lv_event_get_target_obj(e)));
}

static void pulse_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    const struct pulse_state *s = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    int filled;
    int32_t cy;
    int32_t x;
    int i;

    if (!s || !layer || s->level == RIFT_PULSE_NONE) {
        /* Never heard: nothing is drawn, and the age beside it says "?". An
         * empty row of dots would read as "stale", which is a different
         * thing from "unknown". */
        return;
    }
    filled = s->level == RIFT_PULSE_NOW      ? 3
             : s->level == RIFT_PULSE_RECENT ? 2
             : s->level == RIFT_PULSE_QUIET  ? 1
                                             : 0;
    lv_obj_get_coords(obj, &area);
    cy = area.y1 + lv_area_get_height(&area) / 2;
    x = area.x1 + (lv_area_get_width(&area) - (3 * PULSE_DOT + 2 * PULSE_GAP)) / 2 +
        PULSE_DOT / 2;
    for (i = 0; i < 3; i++) {
        if (i < filled) {
            fill_box(layer, x, cy, PULSE_DOT,
                     pos_theme_color(s->level == RIFT_PULSE_NOW ? POS_COLOR_RADIO_RX
                                                                : POS_COLOR_TEXT_SECONDARY));
        } else {
            hollow_box(layer, x, cy, PULSE_DOT, pos_theme_color(POS_COLOR_TEXT_MUTED), 1);
        }
        x += PULSE_DOT + PULSE_GAP;
    }
}

lv_obj_t *rift_pulse_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct pulse_state *s = calloc(1, sizeof(*s));

    if (!s) {
        lv_obj_delete(obj);
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, RIFT_PULSE_W, RIFT_GLYPH_BOX);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, s);
    s->level = RIFT_PULSE_NONE;
    lv_obj_add_event_cb(obj, pulse_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, pulse_delete, LV_EVENT_DELETE, NULL);
    return obj;
}

void rift_pulse_set(lv_obj_t *pulse, enum rift_pulse level)
{
    struct pulse_state *s = pulse ? lv_obj_get_user_data(pulse) : NULL;

    if (!s || s->level == level) {
        return;
    }
    s->level = level;
    lv_obj_invalidate(pulse);
}

enum rift_pulse rift_pulse_get(lv_obj_t *pulse)
{
    const struct pulse_state *s;
    uint32_t i;
    uint32_t n;

    if (!pulse) {
        return RIFT_PULSE_NONE;
    }
    /* A pulse is the object that draws with pulse_draw; anything else's user
     * data is not a pulse_state and is not read as one. */
    n = lv_obj_get_event_count(pulse);
    for (i = 0; i < n; i++) {
        if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(pulse, i)) == pulse_draw) {
            s = lv_obj_get_user_data(pulse);
            return s ? s->level : RIFT_PULSE_NONE;
        }
    }
    return RIFT_PULSE_NONE;
}

/* ---- panels, rules and rows --------------------------------------------- */

/* LVGL clips an OVERFLOW_VISIBLE object's children to the object's box
 * GROWN BY ITS EXTENDED DRAW SIZE, not to nothing (lv_obj_redraw in
 * lv_refr.c), and a panel's is 0. So the flag alone left the caption cut
 * exactly where it was; the panel also has to say it draws that far out. */
int32_t rift_caption_h(void)
{
    return LV_MAX(RIFT_CAPTION_H, pocketui_role_line_height(POS_STYLE_CAPTION));
}

int32_t rift_caption_overhang(void)
{
    return rift_caption_h() / 2 + 3;
}

int32_t rift_group_h(void)
{
    return LV_MAX(RIFT_GROUP_H, rift_caption_h() + (RIFT_GROUP_H - RIFT_CAPTION_H));
}

int32_t rift_header_row_h(void)
{
    /* A caption line with its air, and the thread's header holds a name in
     * the row-title role: 28 px at Small either way. */
    int32_t h = LV_MAX(RIFT_HEADER_ROW_H, rift_caption_h() + (RIFT_HEADER_ROW_H - RIFT_CAPTION_H));

    return LV_MAX(h, pocketui_role_line_height(POS_STYLE_ROW_TITLE) + 2);
}

static void on_panel_ext_draw(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, rift_caption_overhang());
}

lv_obj_t *rift_panel(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *panel = lv_obj_create(parent);

    lv_obj_remove_style_all(panel);
    pos_style_add(panel, POS_STYLE_PANEL, 0);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    if (caption && caption[0]) {
        /* The caption is centred ON the top rule, so its upper half lies
         * outside the panel's own box, and LVGL clips a child to its parent:
         * every caption used to be drawn with the top of its capitals cut
         * off - on unit A too (docs/hardware/shots/rift-phase1-unitA-
         * activity-2055.png). The panel lets it overhang, by exactly as much
         * as it rises; what it overhangs is the gap above the panel, which
         * is the parent's padding or the row gap between panels, and a
         * parent whose top edge a panel sits on has to leave that much room
         * (rift_activity.c's columns). */
        lv_obj_add_flag(panel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_add_event_cb(panel, on_panel_ext_draw, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
        lv_obj_refresh_ext_draw_size(panel);
    }
    if (caption && caption[0]) {
        lv_obj_t *label = lv_label_create(panel);

        /* The caption sits in the rule rather than under it, which means
         * covering the hairline where it passes. The screen role supplies
         * the background fill so no colour is named here; the caption role
         * on top supplies the type. */
        lv_obj_remove_style_all(label);
        pos_style_add(label, POS_STYLE_SCREEN, 0);
        pos_style_add(label, POS_STYLE_CAPTION, 0);
        lv_obj_set_style_pad_hor(label, 6, 0);
        lv_obj_set_height(label, rift_caption_h());
        lv_label_set_text(label, caption);
        /* Out of the flex flow and placed by hand: 16 px in from the panel's
         * left edge (handoff §4) and centred on the top rule, which its own
         * background fill then interrupts. Both offsets are measured from
         * the content box, hence the panel's padding. */
        lv_obj_add_flag(label, LV_OBJ_FLAG_IGNORE_LAYOUT);
        /* The extra 3 px is the difference between the middle of the label's
         * line box and the middle of the ink in it: without it the rule
         * comes out along the top of the capitals rather than through the
         * middle of them. */
        lv_obj_set_pos(label, 16 - POCKETUI_PAD, -POCKETUI_PAD - rift_caption_h() / 2 - 3);
    }
    return panel;
}

lv_obj_t *rift_group_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_remove_style_all(label);
    pos_style_add(label, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_height(label, rift_group_h());
    lv_obj_set_style_pad_top(label, 6, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, text ? text : "");
    return label;
}

lv_obj_t *rift_rule(lv_obj_t *parent)
{
    lv_obj_t *rule = lv_obj_create(parent);

    lv_obj_remove_style_all(rule);
    pos_style_add(rule, POS_STYLE_DIVIDER, 0);
    lv_obj_set_width(rule, LV_PCT(100));
    lv_obj_set_height(rule, 1);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    return rule;
}

lv_obj_t *rift_action(lv_obj_t *parent, const char *text, int primary, int enabled,
                      lv_event_cb_t cb, void *user)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_t *label;

    lv_obj_remove_style_all(button);
    if (!enabled) {
        pos_style_add(button, POS_STYLE_BUTTON_DISABLED, 0);
        lv_obj_add_state(button, LV_STATE_DISABLED);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
    } else if (primary) {
        pos_style_add(button, POS_STYLE_BUTTON_PRIMARY, 0);
        pos_style_add(button, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    } else {
        pos_style_add(button, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(button, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
    lv_obj_set_height(button, RIFT_TOUCH_H);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    label = lv_label_create(button);
    lv_obj_remove_style_all(label);
    pos_style_add(label, POS_STYLE_BUTTON_LABEL, 0);
    lv_label_set_text(label, text ? text : "");
    lv_obj_center(label);
    /* The callback is attached whatever the starting state, so an action
     * that is enabled later works; a disabled one cannot be clicked, so it
     * cannot fire. */
    if (cb) {
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);
    }
    return button;
}

void rift_action_set_enabled(lv_obj_t *button, int primary, int enabled)
{
    if (!button) {
        return;
    }
    /* Only on a change, so a refresh that repaints every second does not
     * restyle - and invalidate - a button nobody touched. */
    if (enabled == !lv_obj_has_state(button, LV_STATE_DISABLED)) {
        return;
    }
    lv_obj_remove_style(button, pos_style(POS_STYLE_BUTTON_DISABLED), 0);
    lv_obj_remove_style(button, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(button, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(button, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    lv_obj_remove_style(button, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
    if (!enabled) {
        pos_style_add(button, POS_STYLE_BUTTON_DISABLED, 0);
        lv_obj_add_state(button, LV_STATE_DISABLED);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
        return;
    }
    lv_obj_remove_state(button, LV_STATE_DISABLED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    if (primary) {
        pos_style_add(button, POS_STYLE_BUTTON_PRIMARY, 0);
        pos_style_add(button, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    } else {
        pos_style_add(button, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(button, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
}

static int32_t text_width(const char *text, const lv_font_t *font, int32_t letter_space)
{
    lv_point_t size;

    lv_text_get_size(&size, text, font, letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

int32_t rift_cell_text_width(lv_obj_t *cell, const char *text)
{
    const lv_font_t *font = cell ? lv_obj_get_style_text_font(cell, LV_PART_MAIN) : NULL;

    if (!font || !text || !text[0]) {
        return 0;
    }
    return text_width(text, font, lv_obj_get_style_text_letter_space(cell, LV_PART_MAIN));
}

void rift_label_set(lv_obj_t *label, const char *text)
{
    const char *now;

    if (!label) {
        return;
    }
    now = lv_label_get_text(label);
    if (!now || strcmp(now, text ? text : "") != 0) {
        lv_label_set_text(label, text ? text : "");
    }
}

void rift_cell_set_text_fit(lv_obj_t *cell, const char *text)
{
    rift_cell_set_text_fit_room(cell, text, 0);
}

/* What a cell was last fitted to: FNV-1a of the text, folded with the width
 * and the font. Never 0, which is a cell nothing has been fitted into. */
static uintptr_t fit_print(const char *text, int32_t room, const lv_font_t *font)
{
    uint32_t h = 2166136261u;

    for (; *text; text++) {
        h ^= (uint8_t)*text;
        h *= 16777619u;
    }
    h ^= (uint32_t)room * 2654435761u;
    h ^= (uint32_t)(uintptr_t)font;
    return (uintptr_t)(h ? h : 1u);
}

void rift_cell_set_text_fit_room(lv_obj_t *cell, const char *text, int32_t room)
{
    const lv_font_t *font;
    int32_t letter_space;
    char buf[RIFT_NAME_MAX + 8];
    size_t keep;

    if (!cell) {
        return;
    }
    if (!text) {
        text = "";
    }
    font = lv_obj_get_style_text_font(cell, LV_PART_MAIN);
    letter_space = lv_obj_get_style_text_letter_space(cell, LV_PART_MAIN);
    if (room <= 0) {
        room = lv_obj_get_width(cell);
    }
    {
        uintptr_t print = fit_print(text, room, font);

        if ((uintptr_t)lv_obj_get_user_data(cell) == print) {
            return; /* this text, at this width, is what it already shows */
        }
        lv_obj_set_user_data(cell, (void *)print);
    }
    if (!font || room <= 0 || text_width(text, font, letter_space) <= room) {
        rift_label_set(cell, text);
        return;
    }
    /* Back off one character at a time until the text and the ellipsis fit.
     * rift_utf8_copy never cuts inside a character, so every candidate is
     * still text. */
    keep = strlen(text);
    if (keep > sizeof(buf) - sizeof(RIFT_ELLIPSIS) - 1) {
        keep = sizeof(buf) - sizeof(RIFT_ELLIPSIS) - 1;
    }
    for (; keep > 0; keep--) {
        size_t written = rift_utf8_copy(buf, keep + 1, text);

        if (written != keep) {
            continue; /* keep landed inside a character; try one shorter */
        }
        memcpy(buf + written, RIFT_ELLIPSIS, sizeof(RIFT_ELLIPSIS));
        if (text_width(buf, font, letter_space) <= room) {
            lv_label_set_text(cell, buf);
            return;
        }
    }
    lv_label_set_text(cell, RIFT_ELLIPSIS);
}

lv_obj_t *rift_cell(lv_obj_t *parent, enum pos_style_role role, int32_t width,
                    lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_remove_style_all(label);
    pos_style_add(label, role, 0);
    if (width > 0) {
        lv_obj_set_width(label, width);
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_text(label, "");
    return label;
}

lv_obj_t *rift_unread_pill(lv_obj_t *parent)
{
    lv_obj_t *pill = lv_label_create(parent);

    lv_obj_remove_style_all(pill);
    /* The two tokens the handoff asks for - radio_rx fill and
     * text_on_accent text - are exactly POS_STYLE_CHIP_RX's, so they come
     * from the role rather than from a colour written here
     * (tests/style_lint.sh). The chip's 36 px geometry is not what a pill
     * is, so the size, radius and padding are set below. */
    pos_style_add(pill, POS_STYLE_CAPTION, 0);
    pos_style_add(pill, POS_STYLE_CHIP_RX, 0);
    /* CHIP_RX carries the two colours and nothing else: the opacity that
     * makes a chip's fill appear lives in POS_STYLE_CHIP, with the 36 px
     * geometry this is not allowed to take. Without it the pill painted no
     * fill at all and its text_on_accent text landed on the page
     * background - on unit A, in ice/outdoor, completely invisible while
     * still taking its space in the row. The fill opacity is the one thing
     * that has to be said here; the colour is still the role's. */
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 2, 0);
    lv_obj_set_style_pad_hor(pill, 5, 0);
    lv_obj_set_style_pad_ver(pill, 0, 0);
    lv_obj_set_height(pill, LV_SIZE_CONTENT);
    lv_obj_set_width(pill, LV_SIZE_CONTENT);
    lv_label_set_long_mode(pill, LV_LABEL_LONG_CLIP);
    lv_label_set_text(pill, "");
    lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);
    return pill;
}

void rift_unread_pill_set(lv_obj_t *pill, int count)
{
    if (!pill) {
        return;
    }
    if (count <= 0) {
        /* A pill reading 0 is a badge that says there is nothing to read. */
        lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(pill, "");
        return;
    }
    if (count > 99) {
        lv_label_set_text(pill, "99+");
    } else {
        lv_label_set_text_fmt(pill, "%d", count);
    }
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_HIDDEN);
}

/* ---- the vertical rule beside a message --------------------------------- */

struct vrule_state {
    enum rift_tone tone;
    int has_ident;   /* drawn in an identity accent rather than a tone */
    uint32_t ident;
};

static void vrule_delete(lv_event_t *e)
{
    free(lv_obj_get_user_data(lv_event_get_target_obj(e)));
}

static void vrule_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    const struct vrule_state *s = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_rect_dsc_t dsc;
    lv_area_t area;

    if (!s || !layer || (s->tone == RIFT_TONE_NONE && !s->has_ident)) {
        return;
    }
    lv_obj_get_coords(obj, &area);
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.bg_opa = LV_OPA_COVER;
    if (s->has_ident) {
        dsc.bg_color = pos_identity_hue(s->ident);
        lv_draw_rect(layer, &dsc, &area);
        return;
    }
    switch (s->tone) {
    case RIFT_TONE_RX:
        dsc.bg_color = pos_theme_color(POS_COLOR_RADIO_RX);
        break;
    case RIFT_TONE_SECONDARY:
        dsc.bg_color = pos_theme_color(POS_COLOR_TEXT_SECONDARY);
        break;
    case RIFT_TONE_MUTED:
        dsc.bg_color = pos_theme_color(POS_COLOR_TEXT_MUTED);
        break;
    case RIFT_TONE_ACCENT:
    default:
        dsc.bg_color = pos_theme_color(POS_COLOR_ACCENT_PRIMARY);
        break;
    }
    lv_draw_rect(layer, &dsc, &area);
}

lv_obj_t *rift_vrule(lv_obj_t *parent, int32_t width)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct vrule_state *s = calloc(1, sizeof(*s));

    if (!s) {
        lv_obj_delete(obj);
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_width(obj, width);
    lv_obj_set_height(obj, LV_PCT(100));
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, s);
    s->tone = RIFT_TONE_SECONDARY;
    lv_obj_add_event_cb(obj, vrule_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, vrule_delete, LV_EVENT_DELETE, NULL);
    return obj;
}

void rift_vrule_set(lv_obj_t *rule, enum rift_tone tone)
{
    struct vrule_state *s = rule ? lv_obj_get_user_data(rule) : NULL;

    if (!s || (s->tone == tone && !s->has_ident)) {
        return;
    }
    s->tone = tone;
    s->has_ident = 0;
    lv_obj_invalidate(rule);
}

void rift_vrule_set_identity(lv_obj_t *rule, uint32_t index)
{
    struct vrule_state *s = rule ? lv_obj_get_user_data(rule) : NULL;

    if (!s || (s->has_ident && s->ident == index)) {
        return;
    }
    s->has_ident = 1;
    s->ident = index;
    lv_obj_invalidate(rule);
}
