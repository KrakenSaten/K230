/*
 * The traffic graph. See rift_graph.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_graph.h"

#include "pos_styles.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Between one minute's bar and the next, and the narrowest a bar gets when
 * the pane is narrow. */
#define BAR_GAP 3
#define BAR_MIN_W 3

struct graph_state {
    struct rift_traffic_bins bins;
    int have;
};

static enum pos_color_token class_token(enum rift_traffic_class c)
{
    switch (c) {
    case RIFT_TRAFFIC_MSG:
        return POS_COLOR_STATUS_OK;
    case RIFT_TRAFFIC_ADV:
        return POS_COLOR_RADIO_RX;
    case RIFT_TRAFFIC_OTHER:
    default:
        return POS_COLOR_TEXT_MUTED;
    }
}

const char *rift_traffic_class_word(enum rift_traffic_class c)
{
    switch (c) {
    case RIFT_TRAFFIC_MSG:
        return "MSG";
    case RIFT_TRAFFIC_ADV:
        return "ADV";
    case RIFT_TRAFFIC_OTHER:
    default:
        return "OTHER";
    }
}

int32_t rift_graph_height_of(unsigned count)
{
    /* A fixed ladder, not a share of the busiest minute: a bar keeps its
     * height when a busier minute arrives, and a quiet mesh's one frame is
     * a visible tick, not a bar scaled to nothing against a burst. */
    if (count == 0) {
        return 0;
    }
    if (count == 1) {
        return 4;
    }
    if (count <= 3) {
        return 9;
    }
    if (count <= 7) {
        return 15;
    }
    if (count <= 15) {
        return 21;
    }
    return RIFT_GRAPH_BAND;
}

static void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                 lv_color_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t a;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = color;
    a.x1 = x1;
    a.y1 = y1;
    a.x2 = x2;
    a.y2 = y2;
    lv_draw_rect(layer, &dsc, &a);
}

/* Split a bar's height between the classes present in the minute, in
 * proportion to their counts, with every class that is there at all given
 * at least one pixel taken from the largest. */
static void split(const uint16_t count[RIFT_TRAFFIC_CLASSES], int32_t h,
                  int32_t px[RIFT_TRAFFIC_CLASSES])
{
    unsigned total = 0;
    int32_t given = 0;
    int biggest = 0;
    int c;

    for (c = 0; c < RIFT_TRAFFIC_CLASSES; c++) {
        total += count[c];
        if (count[c] > count[biggest]) {
            biggest = c;
        }
    }
    for (c = 0; c < RIFT_TRAFFIC_CLASSES; c++) {
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
    /* Rounding and the one-pixel floors are settled on the largest class,
     * which is the one that can afford it. */
    px[biggest] += h - given;
    if (px[biggest] < 1 && count[biggest] > 0) {
        px[biggest] = 1;
    }
}

static void graph_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    const struct graph_state *s = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    int32_t w;
    int32_t pitch;
    int32_t bar_w;
    int32_t x;
    int32_t base;
    int i;

    if (!s || !layer) {
        return;
    }
    lv_obj_get_coords(obj, &area);
    w = lv_area_get_width(&area);
    pitch = w / RIFT_TRAFFIC_MINUTES;
    bar_w = pitch - BAR_GAP;
    if (bar_w < BAR_MIN_W) {
        bar_w = BAR_MIN_W;
    }
    if (pitch < bar_w) {
        pitch = bar_w;
    }
    /* Twenty slots, centred in whatever width the pane gave. */
    x = area.x1 + (w - (pitch * RIFT_TRAFFIC_MINUTES - BAR_GAP)) / 2;
    base = area.y2; /* the baseline row */
    for (i = 0; i < RIFT_TRAFFIC_MINUTES; i++, x += pitch) {
        int32_t px[RIFT_TRAFFIC_CLASSES];
        int32_t top = base - 1;
        unsigned total = 0;
        int32_t h;
        int c;

        /* A baseline under every slot, so a quiet minute is a minute that
         * was watched and not a gap in the graph. */
        fill(layer, x, base, x + bar_w - 1, base, pos_theme_color(POS_COLOR_LINE));
        if (!s->have) {
            continue;
        }
        for (c = 0; c < RIFT_TRAFFIC_CLASSES; c++) {
            total += s->bins.count[i][c];
        }
        h = rift_graph_height_of(total);
        if (h <= 0) {
            continue;
        }
        split(s->bins.count[i], h, px);
        /* Stacked from the baseline up in the legend's order: messages,
         * then adverts, then the rest. */
        for (c = 0; c < RIFT_TRAFFIC_CLASSES; c++) {
            if (px[c] <= 0) {
                continue;
            }
            fill(layer, x, top - px[c] + 1, x + bar_w - 1, top,
                 pos_theme_color(class_token((enum rift_traffic_class)c)));
            top -= px[c];
        }
    }
}

static void graph_delete(lv_event_t *e)
{
    free(lv_obj_get_user_data(lv_event_get_target_obj(e)));
}

lv_obj_t *rift_traffic_graph_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct graph_state *s = calloc(1, sizeof(*s));

    if (!s) {
        lv_obj_delete(obj);
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, RIFT_GRAPH_TOTAL_H);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, s);
    lv_obj_add_event_cb(obj, graph_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, graph_delete, LV_EVENT_DELETE, NULL);
    return obj;
}

void rift_traffic_graph_set(lv_obj_t *graph, const struct rift_traffic_bins *bins)
{
    struct graph_state *s = graph ? lv_obj_get_user_data(graph) : NULL;

    if (!s || !bins) {
        return;
    }
    if (s->have && memcmp(&s->bins, bins, sizeof(*bins)) == 0) {
        return;
    }
    s->bins = *bins;
    s->have = 1;
    lv_obj_invalidate(graph);
}

const struct rift_traffic_bins *rift_traffic_graph_bins(lv_obj_t *graph)
{
    const struct graph_state *s = graph ? lv_obj_get_user_data(graph) : NULL;

    return (s && s->have) ? &s->bins : NULL;
}

/* ---- the legend's swatch ------------------------------------------------ */

static void swatch_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    enum rift_traffic_class c = (enum rift_traffic_class)(intptr_t)lv_obj_get_user_data(obj);
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
         pos_theme_color(class_token(c)));
}

lv_obj_t *rift_traffic_swatch_create(lv_obj_t *parent, enum rift_traffic_class c)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, RIFT_SWATCH_PX + 2, RIFT_SWATCH_PX + 2);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, (void *)(intptr_t)c);
    lv_obj_add_event_cb(obj, swatch_draw, LV_EVENT_DRAW_MAIN, NULL);
    return obj;
}
