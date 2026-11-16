/*
 * ACTIVITY's live traffic graph. See rift_graph.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_graph.h"

#include "app.h"
#include "pos_styles.h"
#include "rift_model.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The scope: how long a frame's arrival ripples, how often the sweep
 * crosses, and the RSSI its height runs between. */
#define RIPPLE_MS 900
#define SWEEP_MS 4000
#define RSSI_TOP (-45.0f)
#define RSSI_FLOOR (-125.0f)
#define SCOPE_PAD 7
/* A type that is not the isolated one. */
#define OPA_DIMMED 46

struct graph_state {
    const struct rift_air *air;
    struct rift_traffic_bins bins;
    int have_bins;
    int span;
    int isolate;
    int listening;
    uint32_t frame_tick; /* lv_tick at the last frame taken */
    /* What is drawn: the view as last read, each bar's height in frames as
     * it is shown (it grows to its count), and the scale as it is shown. */
    struct rift_air_view view;
    float shown[RIFT_AIR_BARS];
    float scale;
    int64_t shown_newest;
    int have_shown;
    /* What the bars were last painted from. */
    unsigned painted_serial;
    int32_t painted_scroll;
    int64_t painted_ms;
};

/* The two enums are index for index (rift_air.h). */
static lv_color_t type_color(int type)
{
    return pos_packet_hue((enum pos_packet_kind)type);
}

unsigned rift_graph_scale_for(unsigned peak)
{
    static const unsigned steps[] = { 4, 8, 12, 20 };
    unsigned mult = 1;
    size_t i;

    for (;;) {
        for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
            if (peak <= steps[i] * mult) {
                return steps[i] * mult;
            }
        }
        if (mult > 100000u) {
            return peak;
        }
        mult *= 10;
    }
}

/* ---- drawing primitives --------------------------------------------------- */

static void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                 lv_color_t color, lv_opa_t opa, int32_t radius)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t a;

    if (x2 < x1 || y2 < y1 || opa == 0) {
        return;
    }
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_opa = opa;
    dsc.bg_color = color;
    a.x1 = x1;
    a.y1 = y1;
    a.x2 = x2;
    a.y2 = y2;
    lv_draw_rect(layer, &dsc, &a);
}

static void ring(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t r, lv_color_t color,
                 lv_opa_t opa)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t a;

    if (r < 1 || opa == 0) {
        return;
    }
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_width = 2;
    dsc.border_opa = opa;
    dsc.border_color = color;
    a.x1 = cx - r;
    a.y1 = cy - r;
    a.x2 = cx + r;
    a.y2 = cy + r;
    lv_draw_rect(layer, &dsc, &a);
}

/* A caption in the object's own font (the caption role, at Small). align:
 * 0 starts at x, 1 ends at x, 2 is centred on it. */
static void caption(lv_layer_t *layer, lv_obj_t *obj, int32_t x, int32_t y, const char *text,
                    int align, enum pos_color_token token)
{
    const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    int32_t space = lv_obj_get_style_text_letter_space(obj, LV_PART_MAIN);
    lv_draw_label_dsc_t dsc;
    lv_point_t size;
    lv_area_t a;

    if (!font || !text[0]) {
        return;
    }
    lv_text_get_size(&size, text, font, space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.letter_space = space;
    dsc.color = pos_theme_color(token);
    dsc.text = text;
    dsc.text_local = 1; /* the words are the caller's, on its stack */
    a.x1 = align == 1 ? x - size.x : align == 2 ? x - size.x / 2 : x;
    a.y1 = y;
    a.x2 = a.x1 + size.x;
    a.y2 = y + size.y;
    lv_draw_label(layer, &dsc, &a);
}

/* ---- what is shown --------------------------------------------------------- */

static unsigned bar_total(const struct rift_air_view *v, int j)
{
    unsigned sum = 0;
    int c;

    for (c = 0; c < RIFT_AIR_TYPES; c++) {
        sum += v->bar[j][c];
    }
    return sum;
}

/* Read the record as of now and move what is shown toward it: all the way
 * when snap, else a part of the way, which is what makes a bar grow. Returns
 * whether anything shown is still moving. */
static int advance(struct graph_state *s, int64_t now, int snap)
{
    float want = 0;
    int moving = 0;
    int j;

    rift_air_read(s->air, now, s->span, &s->view);
    if (!s->have_shown || snap) {
        s->shown_newest = s->view.newest_bar;
        s->have_shown = 1;
    } else if (s->view.newest_bar != s->shown_newest) {
        int64_t by = s->view.newest_bar - s->shown_newest;

        /* A bar's time has passed: every bar is one place older, and keeps
         * the height it was shown at. */
        if (by > 0 && by < RIFT_AIR_BARS) {
            memmove(&s->shown[0], &s->shown[by], sizeof(s->shown[0]) * (size_t)(RIFT_AIR_BARS - by));
            memset(&s->shown[RIFT_AIR_BARS - by], 0, sizeof(s->shown[0]) * (size_t)by);
        } else {
            memset(s->shown, 0, sizeof(s->shown));
        }
        s->shown_newest = s->view.newest_bar;
    }
    for (j = 0; j < RIFT_AIR_BARS; j++) {
        float target = (float)bar_total(&s->view, j);
        float d = target - s->shown[j];

        if (snap || (d < 0.04f && d > -0.04f)) {
            s->shown[j] = target;
        } else {
            s->shown[j] += d * 0.3f;
            moving = 1;
        }
    }
    want = (float)rift_graph_scale_for(s->view.peak);
    if (snap || s->scale <= 0 || (want - s->scale < 0.05f && want - s->scale > -0.05f)) {
        s->scale = want;
    } else {
        s->scale += (want - s->scale) * 0.25f;
        moving = 1;
    }
    return moving;
}

/* ---- the scope -------------------------------------------------------------- */

static void draw_scope(lv_layer_t *layer, lv_obj_t *obj, const struct graph_state *s,
                       const lv_area_t *area, int64_t now, int animate)
{
    int32_t w = lv_area_get_width(area);
    int32_t top = area->y1;
    int32_t bottom = area->y1 + RIFT_GRAPH_SCOPE_H - 1;
    int32_t mid = (top + bottom) / 2;
    int marks = 0;
    int i;

    /* The scope's face: a raised strip with a centre line, so an empty one
     * is still an instrument and not a gap. */
    fill(layer, area->x1, top, area->x2, bottom, pos_theme_color(POS_COLOR_SURFACE), LV_OPA_COVER, 4);
    fill(layer, area->x1 + 4, mid, area->x2 - 4, mid, pos_theme_color(POS_COLOR_LINE), LV_OPA_50, 0);
    if (s->listening && animate) {
        static const struct {
            int32_t width;
            lv_opa_t opa;
        } tail[] = { { 44, 10 }, { 24, 20 }, { 10, 40 }, { 2, 150 } };
        int32_t x = area->x1 + (int32_t)(((now % SWEEP_MS + SWEEP_MS) % SWEEP_MS) * w / SWEEP_MS);
        size_t k;

        /* The sweep: the radio is listening. Nothing about what it hears. */
        for (k = 0; k < sizeof(tail) / sizeof(tail[0]); k++) {
            int32_t x1 = x - tail[k].width;

            fill(layer, x1 < area->x1 ? area->x1 : x1, top + 2, x, bottom - 2,
                 pos_theme_color(POS_COLOR_RADIO_RX), tail[k].opa, 0);
        }
    }
    for (i = 0; s->air; i++) {
        const struct rift_air_packet *p = rift_air_recent(s->air, i);
        int64_t age;
        int32_t x;
        int32_t y = mid;
        int32_t r;
        lv_opa_t opa;
        lv_color_t color;

        if (!p) {
            break;
        }
        age = now - p->mono_ms;
        if (age >= RIFT_AIR_SCOPE_MS) {
            continue; /* by its own stamp: a late report can sit among newer ones */
        }
        if (age < 0) {
            age = 0;
        }
        marks++;
        x = area->x2 - 4 - (int32_t)(age * (w - 8) / RIFT_AIR_SCOPE_MS);
        opa = (lv_opa_t)(255 - age * 150 / RIFT_AIR_SCOPE_MS);
        if (p->tx) {
            /* Said, not heard: an upright bar in the transmit token. */
            color = pos_theme_color(POS_COLOR_RADIO_TX);
            fill(layer, x - 1, top + SCOPE_PAD, x + 1, bottom - SCOPE_PAD, color, opa, 0);
            r = 3;
        } else {
            color = type_color(p->type);
            if (s->isolate >= 0 && p->type != s->isolate) {
                opa = (lv_opa_t)(opa / 5);
            }
            if (p->have_rssi) {
                float f = (p->rssi_dbm - RSSI_FLOOR) / (RSSI_TOP - RSSI_FLOOR);

                f = f < 0 ? 0 : f > 1 ? 1 : f;
                y = bottom - SCOPE_PAD - (int32_t)(f * (float)(RIFT_GRAPH_SCOPE_H - 2 * SCOPE_PAD));
            }
            r = 3 + (p->bytes < 0 ? 0 : p->bytes > 191 ? 3 : p->bytes / 64);
            fill(layer, x - r, y - r, x + r, y + r, color, opa, LV_RADIUS_CIRCLE);
        }
        if (animate && age < RIPPLE_MS) {
            ring(layer, x, y, r + 2 + (int32_t)(age * 16 / RIPPLE_MS), color,
                 (lv_opa_t)(220 - age * 220 / RIPPLE_MS));
        }
    }
    caption(layer, obj, area->x1 + 8, top + 3, "LIVE \xC2\xB7 60 S", 0, POS_COLOR_TEXT_MUTED);
    caption(layer, obj, area->x2 - 8, top + 3, "\xE2\x86\x91 RSSI", 1, POS_COLOR_TEXT_MUTED);
    if (marks == 0) {
        caption(layer, obj, area->x1 + w / 2, mid + 4,
                s->listening ? "LISTENING" : "RADIO NOT LISTENING", 2, POS_COLOR_TEXT_SECONDARY);
    }
}

/* ---- the bars ---------------------------------------------------------------- */

/* Split a bar's height between the types present, in proportion to their
 * counts, every type that is there at all given a pixel from the largest. */
static void split(const uint16_t count[RIFT_AIR_TYPES], int32_t h, int32_t px[RIFT_AIR_TYPES])
{
    unsigned total = 0;
    int32_t given = 0;
    int biggest = 0;
    int c;

    for (c = 0; c < RIFT_AIR_TYPES; c++) {
        total += count[c];
        if (count[c] > count[biggest]) {
            biggest = c;
        }
    }
    for (c = 0; c < RIFT_AIR_TYPES; c++) {
        px[c] = 0;
        if (total == 0 || count[c] == 0) {
            continue;
        }
        px[c] = (int32_t)(((int64_t)h * count[c] + total / 2) / total);
        if (px[c] < 1) {
            px[c] = 1;
        }
        given += px[c];
    }
    px[biggest] += h - given;
    if (px[biggest] < 1 && count[biggest] > 0) {
        px[biggest] = 1;
    }
}

static void draw_bars(lv_layer_t *layer, lv_obj_t *obj, const struct graph_state *s,
                      const lv_area_t *area)
{
    int32_t w = lv_area_get_width(area);
    int32_t top = area->y1 + RIFT_GRAPH_SCOPE_H + RIFT_GRAPH_GAP;
    int32_t base = top + RIFT_GRAPH_BAND - 1;
    int32_t room = RIFT_GRAPH_BAND - 2;
    float pitch = (float)w / (float)(RIFT_AIR_BARS - 1);
    int32_t gap = pitch >= 8.0f ? 2 : 1;
    float right = (float)area->x2 + 1.0f;
    float scale = s->scale > 0 ? s->scale : 4.0f;
    char text[40];
    int span_min = (int)((s->view.bar_ms * RIFT_AIR_BARS + 30000) / 60000);
    int j;
    int k;

    /* The scale's rules: the top of the scale and half of it, faint, and the
     * baseline every bar stands on. */
    fill(layer, area->x1, top, area->x2, top, pos_theme_color(POS_COLOR_LINE), LV_OPA_40, 0);
    fill(layer, area->x1, top + room / 2, area->x2, top + room / 2, pos_theme_color(POS_COLOR_LINE),
         LV_OPA_40, 0);
    fill(layer, area->x1, base, area->x2, base, pos_theme_color(POS_COLOR_LINE), LV_OPA_COVER, 0);
    for (j = 0; j < RIFT_AIR_BARS; j++) {
        /* The newest bar's time is still running: it has come in from the
         * right edge by as much of it as has passed. */
        float left = right - ((float)s->view.phase_permille / 1000.0f + (float)(RIFT_AIR_BARS - 1 - j)) * pitch;
        int32_t x1 = (int32_t)left;
        int32_t x2 = (int32_t)(left + pitch) - gap;
        int32_t px[RIFT_AIR_TYPES];
        int32_t y = base - 1;
        int32_t h;
        int c;

        if (x1 < area->x1) {
            x1 = area->x1;
        }
        if (x2 > area->x2) {
            x2 = area->x2;
        }
        if (x2 < x1 || s->shown[j] <= 0) {
            continue;
        }
        h = (int32_t)(s->shown[j] * (float)room / scale + 0.5f);
        if (h < 3) {
            h = 3; /* one frame in a busy window is still a visible tick */
        }
        if (h > room) {
            h = room;
        }
        split(s->view.bar[j], h, px);
        for (c = 0; c < RIFT_AIR_TYPES; c++) {
            if (px[c] <= 0) {
                continue;
            }
            fill(layer, x1, y - px[c] + 1, x2, y, type_color(c),
                 (s->isolate < 0 || s->isolate == c) ? LV_OPA_COVER : OPA_DIMMED, 0);
            y -= px[c];
        }
        if (j == RIFT_AIR_BARS - 1) {
            /* The bar still filling carries a light cap. */
            fill(layer, x1, y - 1, x2, y, pos_theme_color(POS_COLOR_TEXT_PRIMARY), LV_OPA_70, 0);
        }
    }
    /* What the top rule stands for, and the time under the bars. */
    snprintf(text, sizeof(text), "%u PER %d S", (unsigned)(scale + 0.5f), (int)(s->view.bar_ms / 1000));
    caption(layer, obj, area->x1 + 4, top + 3, text, 0, POS_COLOR_TEXT_MUTED);
    for (k = 0; k < span_min; k++) {
        int32_t x = area->x1 + (int32_t)((int64_t)w * k / span_min);

        if (span_min > 5 && k % 5 != 0) {
            continue;
        }
        fill(layer, x, base + 1, x, base + 4, pos_theme_color(POS_COLOR_LINE), LV_OPA_COVER, 0);
        snprintf(text, sizeof(text), k == 0 ? "-%d MIN" : "-%d", span_min - k);
        caption(layer, obj, x + (k == 0 ? 0 : 0), base + 4, text, k == 0 ? 0 : 2, POS_COLOR_TEXT_MUTED);
    }
    caption(layer, obj, area->x2, base + 4, "NOW", 1, POS_COLOR_TEXT_SECONDARY);
}

static void graph_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    const struct graph_state *s = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    int64_t now;

    if (!s || !layer) {
        return;
    }
    lv_obj_get_coords(obj, &area);
    now = rift_mono_ms();
    draw_scope(layer, obj, s, &area, now, !pocketos_shell_reduced_motion());
    /* A frame of the animation repaints the scope only: when nothing below
     * it is being repainted, nothing below it is drawn. */
    if (layer->_clip_area.y2 >= area.y1 + RIFT_GRAPH_SCOPE_H) {
        draw_bars(layer, obj, s, &area);
    }
}

/* ---- the frame --------------------------------------------------------------- */

static int32_t scroll_px(const struct graph_state *s, lv_obj_t *obj)
{
    return (int32_t)((int64_t)s->view.phase_permille * lv_obj_get_width(obj) /
                     (1000 * (RIFT_AIR_BARS - 1)));
}

static void repaint_all(struct graph_state *s, lv_obj_t *obj, int64_t now)
{
    s->painted_serial = s->air ? s->air->serial : 0;
    s->painted_scroll = scroll_px(s, obj);
    s->painted_ms = now;
    lv_obj_invalidate(obj);
}

/* One frame. Driven by an LVGL animation on the graph (below) and not by a
 * timer: the animation goes with the object, and RIFT keeps the one timer it
 * has always had (the session's). The animation's own value is not used -
 * it is a clock - and frames closer together than RIFT_GRAPH_FRAME_MS are
 * not taken. */
static void graph_frame(void *var, int32_t value)
{
    lv_obj_t *obj = var;
    struct graph_state *s = lv_obj_get_user_data(obj);
    uint32_t tick = lv_tick_get();
    int64_t now;
    lv_area_t scope;
    int i;
    int moving;

    (void)value;
    if (!s || lv_tick_diff(tick, s->frame_tick) < RIFT_GRAPH_FRAME_MS || !lv_obj_is_visible(obj)) {
        return;
    }
    s->frame_tick = tick;
    now = rift_mono_ms();
    if (pocketos_shell_reduced_motion()) {
        /* Nothing animates (DS §12): the picture is brought up to date when
         * something was heard, and once a second for the clock. */
        if ((s->air && s->air->serial != s->painted_serial) || now - s->painted_ms >= 1000) {
            advance(s, now, 1);
            repaint_all(s, obj, now);
        }
        return;
    }
    moving = advance(s, now, 0);
    if (moving || (s->air && s->air->serial != s->painted_serial) ||
        scroll_px(s, obj) != s->painted_scroll || now - s->painted_ms >= 1000) {
        repaint_all(s, obj, now);
        return;
    }
    /* The scope moves while the sweep runs or a frame is still on it. */
    moving = s->listening;
    for (i = 0; !moving && i < RIFT_AIR_RECENT; i++) {
        const struct rift_air_packet *p = rift_air_recent(s->air, i);

        if (!p) {
            break;
        }
        moving = now - p->mono_ms < RIFT_AIR_SCOPE_MS + 200;
    }
    if (!moving) {
        return;
    }
    lv_obj_get_coords(obj, &scope);
    scope.y2 = scope.y1 + RIFT_GRAPH_SCOPE_H - 1;
    lv_obj_invalidate_area(obj, &scope);
}

static void graph_delete(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct graph_state *s = lv_obj_get_user_data(obj);

    /* The animation first: it reads the state. */
    lv_anim_delete(obj, graph_frame);
    lv_obj_set_user_data(obj, NULL);
    free(s);
}

lv_obj_t *rift_traffic_graph_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct graph_state *s = calloc(1, sizeof(*s));
    lv_anim_t frames;

    if (!s) {
        lv_obj_delete(obj);
        return NULL;
    }
    s->span = RIFT_GRAPH_SPAN_20MIN;
    s->isolate = -1;
    lv_obj_remove_style_all(obj);
    /* The caption role for the words drawn on it, at Small whatever the
     * text size: the graph is an instrument of a fixed height. */
    pos_style_add(obj, POS_STYLE_CAPTION, 0);
    lv_obj_add_style(obj, pos_style_fixed_size(POS_STYLE_CAPTION), 0);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, RIFT_GRAPH_TOTAL_H);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    /* A tap turns the span over (rift_activity.c listens). */
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, s);
    lv_obj_add_event_cb(obj, graph_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, graph_delete, LV_EVENT_DELETE, NULL);
    advance(s, rift_mono_ms(), 1);
    lv_anim_init(&frames);
    lv_anim_set_var(&frames, obj);
    lv_anim_set_exec_cb(&frames, graph_frame);
    lv_anim_set_values(&frames, 0, 1000);
    lv_anim_set_duration(&frames, 1000);
    lv_anim_set_repeat_count(&frames, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&frames);
    return obj;
}

static struct graph_state *state_of(lv_obj_t *graph)
{
    return graph ? lv_obj_get_user_data(graph) : NULL;
}

void rift_traffic_graph_feed(lv_obj_t *graph, const struct rift_air *air)
{
    struct graph_state *s = state_of(graph);

    if (s && s->air != air) {
        s->air = air;
        advance(s, rift_mono_ms(), 1);
        repaint_all(s, graph, rift_mono_ms());
    }
}

void rift_traffic_graph_set(lv_obj_t *graph, const struct rift_traffic_bins *bins)
{
    struct graph_state *s = state_of(graph);

    if (s && bins) {
        s->bins = *bins;
        s->have_bins = 1;
    }
}

const struct rift_traffic_bins *rift_traffic_graph_bins(lv_obj_t *graph)
{
    const struct graph_state *s = state_of(graph);

    return (s && s->have_bins) ? &s->bins : NULL;
}

void rift_traffic_graph_set_span(lv_obj_t *graph, int slots_per_bar)
{
    struct graph_state *s = state_of(graph);

    if (s && s->span != slots_per_bar) {
        s->span = slots_per_bar;
        /* Another span is another set of bars: they start from the baseline
         * and grow, which is the turn made visible. */
        memset(s->shown, 0, sizeof(s->shown));
        s->have_shown = 0;
        advance(s, rift_mono_ms(), pocketos_shell_reduced_motion());
        repaint_all(s, graph, rift_mono_ms());
    }
}

int rift_traffic_graph_span(lv_obj_t *graph)
{
    const struct graph_state *s = state_of(graph);

    return s ? s->span : RIFT_GRAPH_SPAN_20MIN;
}

void rift_traffic_graph_isolate(lv_obj_t *graph, int type)
{
    struct graph_state *s = state_of(graph);

    if (s && s->isolate != type) {
        s->isolate = type;
        lv_obj_invalidate(graph);
    }
}

int rift_traffic_graph_isolated(lv_obj_t *graph)
{
    const struct graph_state *s = state_of(graph);

    return s ? s->isolate : -1;
}

void rift_traffic_graph_set_listening(lv_obj_t *graph, int listening)
{
    struct graph_state *s = state_of(graph);

    if (s && s->listening != (listening ? 1 : 0)) {
        s->listening = listening ? 1 : 0;
        lv_obj_invalidate(graph);
    }
}

/* ---- the legend's swatch ---------------------------------------------------- */

static void swatch_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    int type = (int)(intptr_t)lv_obj_get_user_data(obj);
    lv_area_t area;
    int32_t cy;
    int32_t x1;

    if (!layer) {
        return;
    }
    lv_obj_get_coords(obj, &area);
    cy = area.y1 + lv_area_get_height(&area) / 2;
    x1 = area.x1 + (lv_area_get_width(&area) - RIFT_SWATCH_PX) / 2;
    fill(layer, x1, cy - RIFT_SWATCH_PX / 2, x1 + RIFT_SWATCH_PX - 1, cy + RIFT_SWATCH_PX / 2 - 1,
         type == RIFT_SWATCH_TX ? pos_theme_color(POS_COLOR_RADIO_TX) : type_color(type),
         LV_OPA_COVER, 2);
}

lv_obj_t *rift_traffic_swatch_create(lv_obj_t *parent, int type)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, RIFT_SWATCH_PX + 2, RIFT_SWATCH_PX + 2);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, (void *)(intptr_t)type);
    lv_obj_add_event_cb(obj, swatch_draw, LV_EVENT_DRAW_MAIN, NULL);
    return obj;
}

void rift_traffic_swatch_set(lv_obj_t *swatch, int type)
{
    if (swatch && (int)(intptr_t)lv_obj_get_user_data(swatch) != type) {
        lv_obj_set_user_data(swatch, (void *)(intptr_t)type);
        lv_obj_invalidate(swatch);
    }
}

/* ---- the mix ------------------------------------------------------------------ */

struct mix_state {
    uint32_t total[RIFT_AIR_TYPES];
};

static void mix_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    const struct mix_state *s = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    uint64_t sum = 0;
    uint64_t run = 0;
    int32_t w;
    int c;

    if (!s || !layer) {
        return;
    }
    lv_obj_get_coords(obj, &area);
    w = lv_area_get_width(&area);
    for (c = 0; c < RIFT_AIR_TYPES; c++) {
        sum += s->total[c];
    }
    if (sum == 0) {
        fill(layer, area.x1, area.y1, area.x2, area.y2, pos_theme_color(POS_COLOR_SURFACE),
             LV_OPA_COVER, 2);
        return;
    }
    for (c = 0; c < RIFT_AIR_TYPES; c++) {
        int32_t x1 = area.x1 + (int32_t)(run * (uint64_t)w / sum);
        int32_t x2;

        if (s->total[c] == 0) {
            continue;
        }
        run += s->total[c];
        x2 = area.x1 + (int32_t)(run * (uint64_t)w / sum) - 1;
        /* A gap between two types, and never a type too thin to see. */
        if (x2 - x1 > 3) {
            x2 -= 2;
        }
        fill(layer, x1, area.y1, x2 < x1 ? x1 : x2, area.y2, type_color(c), LV_OPA_COVER, 2);
    }
}

static void mix_delete(lv_event_t *e)
{
    free(lv_obj_get_user_data(lv_event_get_target_obj(e)));
}

lv_obj_t *rift_traffic_mix_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct mix_state *s = calloc(1, sizeof(*s));

    if (!s) {
        lv_obj_delete(obj);
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, RIFT_MIX_H);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, s);
    lv_obj_add_event_cb(obj, mix_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, mix_delete, LV_EVENT_DELETE, NULL);
    return obj;
}

void rift_traffic_mix_set(lv_obj_t *mix, const uint32_t *type_total)
{
    struct mix_state *s = mix ? lv_obj_get_user_data(mix) : NULL;

    if (s && type_total && memcmp(s->total, type_total, sizeof(s->total)) != 0) {
        memcpy(s->total, type_total, sizeof(s->total));
        lv_obj_invalidate(mix);
    }
}
