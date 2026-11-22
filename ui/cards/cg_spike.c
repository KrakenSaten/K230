/*
 * Card games redesign, milestone 0: the measurement spike. See cg_spike.h.
 *
 * The palette is fixed in every theme (owner decision 2, 2026-11-22; the
 * design-system amendment follows with milestone 1b).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cg_spike.h"

#include "cg_atlas.h"
#include "cg_cards.h"
#include "cg_cloth.h"
#include "cg_tween.h"

#include "app.h"
#include "bj_card_draw.h"
#include "pocketlog/pocketlog.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(pos_font_sans_32_semibold)

#define CARD_W 124
#define CARD_H 174
#define CARD_R 9
#define SHADOW_M 14
#define SHADOW_DROP 6
#define N_CARDS 4
#define BACK N_CARDS /* atlas index of the back */
#define FRAME_MS 33

#define INK_IVORY 0xfaf4e4
#define INK_EDGE 0xb9ad92
#define INK_BLACK 0x1a1714
#define INK_RED 0xb3261e
#define INK_GOLD 0xd9b66a
#define INK_BACK 0x232a4a
#define CLOTH_LIT 0x7d2130
#define CLOTH_DEEP 0x3a0d16

/* The loop, in milliseconds from its start. */
#define DEAL_AT 200
#define DEAL_GAP 160
#define DEAL_MS 320
#define TURN_AT 1300
#define TURN_MS 240
#define SWEEP_AT 2600
#define SWEEP_GAP 40
#define SWEEP_MS 280
#define LOOP_MS 3400

struct spike_card {
    int x, y;            /* where it is drawn, root-relative */
    int scale_x;         /* 256 whole; less while it turns */
    int face_up;
    int rest_x, rest_y;  /* where the deal puts it */
};

struct spike_stat {
    uint32_t n;
    uint64_t sum;
    uint32_t max;
};

struct cg_spike {
    lv_obj_t *root;
    lv_timer_t *timer;
    lv_display_t *disp;
    uint16_t *cloth_px;
    lv_image_dsc_t cloth;
    struct cg_atlas atlas;
    lv_image_dsc_t shadow;
    struct spike_card card[N_CARDS];
    int w, h;
    int shoe_x, shoe_y, away_x, away_y;
    uint32_t loop_start;
    uint32_t loops;
    int moving;
    uint64_t last_frame_us, render_from_us, refr_from_us;
    struct spike_stat interval, render, refresh;
    uint32_t frames;
    uint32_t area_max;
};

static const cg_card_t faces[N_CARDS] = { 0 /* A spades */, 13 + 12 /* K hearts */, 39 + 6 /* 7 diamonds */,
                                          26 + 11 /* Q clubs */ };

static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void stat_add(struct spike_stat *s, uint64_t v)
{
    s->n++;
    s->sum += v;
    if (v > s->max) {
        s->max = (uint32_t)v;
    }
}

static uint32_t stat_mean(const struct spike_stat *s)
{
    return s->n ? (uint32_t)(s->sum / s->n) : 0;
}

/* ---- the stand-in card art ------------------------------------------------------ */

static void panel(lv_layer_t *layer, const lv_area_t *area, int radius, uint32_t fill, uint32_t border, int border_w)
{
    lv_draw_rect_dsc_t dsc;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_color = lv_color_hex(fill);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_color = lv_color_hex(border);
    dsc.border_width = border_w;
    dsc.border_opa = border_w ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_draw_rect(layer, &dsc, area);
}

static void compose(lv_layer_t *layer, const lv_area_t *area, int index, void *user)
{
    (void)user;
    if (index == BACK) {
        lv_area_t in = { area->x1 + 7, area->y1 + 7, area->x2 - 7, area->y2 - 7 };
        lv_area_t line = { in.x1 + 6, in.y1 + 6, in.x2 - 6, in.y2 - 6 };
        lv_draw_rect_dsc_t dsc;

        panel(layer, area, CARD_R, INK_IVORY, INK_EDGE, 1);
        panel(layer, &in, 5, INK_BACK, INK_BACK, 0);
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius = 3;
        dsc.bg_opa = LV_OPA_TRANSP;
        dsc.border_color = lv_color_hex(INK_GOLD);
        dsc.border_width = 2;
        dsc.border_opa = LV_OPA_70;
        lv_draw_rect(layer, &dsc, &line);
        bj_draw_suit(layer, (area->x1 + area->x2) / 2, (area->y1 + area->y2) / 2, 40, CG_DIAMONDS,
                     lv_color_hex(INK_GOLD));
    } else {
        cg_card_t c = faces[index];
        lv_color_t ink = lv_color_hex(cg_card_is_red(c) ? INK_RED : INK_BLACK);
        lv_area_t text = { area->x1 + 10, area->y1 + 4, area->x1 + 70, area->y1 + 44 };
        lv_draw_label_dsc_t label;

        panel(layer, area, CARD_R, INK_IVORY, INK_EDGE, 1);
        lv_draw_label_dsc_init(&label);
        label.text = cg_rank_text(cg_card_rank(c));
        label.font = &pos_font_sans_32_semibold;
        label.color = ink;
        lv_draw_label(layer, &label, &text);
        bj_draw_suit(layer, area->x1 + 24, area->y1 + 60, 24, cg_card_suit(c), ink);
        bj_draw_suit(layer, (area->x1 + area->x2) / 2, (area->y1 + area->y2) / 2 + 6, 68, cg_card_suit(c), ink);
        bj_draw_suit(layer, area->x2 - 24, area->y2 - 26, 24, cg_card_suit(c), ink);
    }
}

/* ---- drawing -------------------------------------------------------------------- */

static void blit(lv_layer_t *layer, const lv_image_dsc_t *src, int x, int y, int scale_x, int recolor_black)
{
    lv_draw_image_dsc_t dsc;
    lv_area_t area = { x, y, x + (int)src->header.w - 1, y + (int)src->header.h - 1 };

    lv_draw_image_dsc_init(&dsc);
    dsc.src = src;
    if (recolor_black) {
        dsc.recolor = lv_color_black();
        dsc.recolor_opa = LV_OPA_COVER;
    }
    if (scale_x != 256) {
        dsc.scale_x = scale_x;
        dsc.pivot.x = (int32_t)src->header.w / 2;
        dsc.pivot.y = (int32_t)src->header.h / 2;
    }
    lv_draw_image(layer, &dsc, &area);
}

static void on_draw(lv_event_t *e)
{
    struct cg_spike *s = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t co;
    int i;

    lv_obj_get_coords(s->root, &co);
    if (s->cloth_px) {
        blit(layer, &s->cloth, co.x1, co.y1, 256, 0);
    }
    if (!s->atlas.count) {
        return;
    }
    /* The shoe: a few backs, a hair apart. */
    for (i = 2; i >= 0; i--) {
        blit(layer, cg_atlas_image(&s->atlas, BACK), co.x1 + s->shoe_x + i * 2, co.y1 + s->shoe_y - i * 2, 256, 0);
    }
    for (i = 0; i < N_CARDS; i++) {
        const struct spike_card *c = &s->card[i];

        if (s->shadow.data && c->scale_x == 256) {
            blit(layer, &s->shadow, co.x1 + c->x - SHADOW_M, co.y1 + c->y - SHADOW_M + SHADOW_DROP, 256, 1);
        }
        if (c->scale_x > 0) {
            blit(layer, cg_atlas_image(&s->atlas, c->face_up ? i : BACK), co.x1 + c->x, co.y1 + c->y, c->scale_x, 0);
        }
    }
}

static uint32_t invalidate_card(struct cg_spike *s, int x, int y)
{
    lv_area_t co;
    lv_area_t a;

    lv_obj_get_coords(s->root, &co);
    a.x1 = co.x1 + x - SHADOW_M;
    a.y1 = co.y1 + y - SHADOW_M;
    a.x2 = co.x1 + x + CARD_W + SHADOW_M;
    a.y2 = co.y1 + y + CARD_H + SHADOW_M + SHADOW_DROP;
    lv_obj_invalidate_area(s->root, &a);
    return (uint32_t)((a.x2 - a.x1 + 1) * (a.y2 - a.y1 + 1));
}

/* ---- the loop ------------------------------------------------------------------- */

/* Where card i is, how wide and which side up, `t` ms into the loop. */
static void pose(const struct cg_spike *s, int i, uint32_t t, struct spike_card *out)
{
    const struct spike_card *c = &s->card[i];
    struct cg_tween deal = { s->shoe_x, s->shoe_y, c->rest_x, c->rest_y, DEAL_AT + (uint32_t)i * DEAL_GAP, DEAL_MS };
    struct cg_tween sweep = { c->rest_x, c->rest_y, s->away_x, s->away_y, SWEEP_AT + (uint32_t)i * SWEEP_GAP,
                              SWEEP_MS };

    *out = *c;
    out->scale_x = 256;
    /* The dealer's second card (3) is dealt face down and turned later. */
    out->face_up = i != 3;
    if (t < sweep.start_ms) {
        cg_tween_at(&deal, t, &out->x, &out->y);
    } else {
        cg_tween_at(&sweep, t, &out->x, &out->y);
    }
    if (i == 3 && t >= TURN_AT) {
        uint32_t into = t - TURN_AT;

        if (into < TURN_MS / 2) {
            out->scale_x = 256 - (int)(into * 256 / (TURN_MS / 2));
        } else {
            out->face_up = 1;
            out->scale_x = into >= TURN_MS ? 256 : (int)((into - TURN_MS / 2) * 256 / (TURN_MS / 2));
        }
    }
}

static void report(struct cg_spike *s)
{
    LOG_INFO("cards-spike: loop %u: %u frame(s); interval mean %u us max %u us; render mean %u us max %u us; "
             "refresh mean %u us max %u us; largest frame %u px (%u%% of %dx%d)",
             (unsigned)s->loops, (unsigned)s->frames, (unsigned)stat_mean(&s->interval), (unsigned)s->interval.max,
             (unsigned)stat_mean(&s->render), (unsigned)s->render.max, (unsigned)stat_mean(&s->refresh),
             (unsigned)s->refresh.max, (unsigned)s->area_max,
             (unsigned)(s->w * s->h ? (uint64_t)s->area_max * 100 / ((uint64_t)s->w * s->h) : 0), s->w, s->h);
    memset(&s->interval, 0, sizeof(s->interval));
    memset(&s->render, 0, sizeof(s->render));
    memset(&s->refresh, 0, sizeof(s->refresh));
    s->frames = 0;
    s->area_max = 0;
}

static void on_frame(lv_timer_t *timer)
{
    struct cg_spike *s = lv_timer_get_user_data(timer);
    uint32_t t = lv_tick_elaps(s->loop_start);
    uint32_t area = 0;
    int moved = 0;
    int i;

    if (t >= LOOP_MS) {
        s->loops++;
        report(s);
        s->loop_start = lv_tick_get();
        t = 0;
    }
    for (i = 0; i < N_CARDS; i++) {
        struct spike_card next;

        pose(s, i, t, &next);
        if (next.x != s->card[i].x || next.y != s->card[i].y || next.scale_x != s->card[i].scale_x ||
            next.face_up != s->card[i].face_up) {
            area += invalidate_card(s, s->card[i].x, s->card[i].y);
            s->card[i] = next;
            area += invalidate_card(s, next.x, next.y);
            moved = 1;
        }
    }
    if (moved) {
        uint64_t now = now_us();

        if (s->moving) {
            stat_add(&s->interval, now - s->last_frame_us);
        }
        s->last_frame_us = now;
        s->frames++;
        if (area > s->area_max) {
            s->area_max = area;
        }
    }
    s->moving = moved;
}

static void on_render(lv_event_t *e)
{
    struct cg_spike *s = lv_event_get_user_data(e);

    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:
        s->refr_from_us = now_us();
        break;
    case LV_EVENT_RENDER_START:
        s->render_from_us = now_us();
        break;
    case LV_EVENT_RENDER_READY:
        if (s->moving && s->render_from_us) {
            stat_add(&s->render, now_us() - s->render_from_us);
        }
        s->render_from_us = 0;
        break;
    case LV_EVENT_REFR_READY:
        if (s->moving && s->refr_from_us) {
            stat_add(&s->refresh, now_us() - s->refr_from_us);
        }
        s->refr_from_us = 0;
        break;
    default:
        break;
    }
}

/* ---- building ------------------------------------------------------------------- */

static void layout(struct cg_spike *s)
{
    int w = lv_obj_get_width(s->root);
    int h = lv_obj_get_height(s->root);
    int gap = 16;
    int x0 = (w - 2 * CARD_W - gap) / 2;
    struct cg_cloth cloth = { CLOTH_LIT, CLOTH_DEEP, w / 2, h * 42 / 100, w * 95 / 100, h * 55 / 100, 3 };
    uint64_t t0 = now_us();
    int i;

    if (w <= 0 || h <= 0 || (w == s->w && h == s->h)) {
        return;
    }
    s->w = w;
    s->h = h;
    free(s->cloth_px);
    s->cloth_px = malloc((size_t)w * h * 2);
    if (s->cloth_px) {
        cg_cloth_render(s->cloth_px, w, h, w, &cloth);
        memset(&s->cloth, 0, sizeof(s->cloth));
        s->cloth.header.magic = LV_IMAGE_HEADER_MAGIC;
        s->cloth.header.cf = LV_COLOR_FORMAT_RGB565;
        s->cloth.header.w = (uint32_t)w;
        s->cloth.header.h = (uint32_t)h;
        s->cloth.header.stride = (uint32_t)w * 2;
        s->cloth.data = (const uint8_t *)s->cloth_px;
        s->cloth.data_size = (uint32_t)w * h * 2;
    }
    LOG_INFO("cards-spike: cloth %dx%d in %u us, %u bytes", w, h, (unsigned)(now_us() - t0), (unsigned)(w * h * 2));

    s->shoe_x = w - CARD_W - 36;
    s->shoe_y = 84;
    s->away_x = 36;
    s->away_y = 84;
    for (i = 0; i < N_CARDS; i++) {
        /* 0 and 2 to the player (lower row), 1 and 3 to the dealer. */
        s->card[i].rest_x = x0 + (i / 2) * (CARD_W + gap);
        s->card[i].rest_y = (i % 2) ? h * 30 / 100 : h * 56 / 100;
        pose(s, i, pocketos_shell_reduced_motion() ? SWEEP_AT - 1 : 0, &s->card[i]);
    }
    lv_obj_invalidate(s->root);
}

static void on_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

static void on_back(lv_event_t *e)
{
    (void)e;
    pocketos_shell_go_home();
}

void *cg_spike_create(lv_obj_t *body)
{
    struct cg_spike *s = calloc(1, sizeof(*s));
    lv_obj_t *back;
    lv_obj_t *mark;
    uint64_t t0;

    if (!s) {
        return NULL;
    }
    pocketos_shell_set_fullscreen(true);
    s->root = lv_obj_create(body);
    lv_obj_remove_style_all(s->root);
    lv_obj_set_size(s->root, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(s->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s->root, on_draw, LV_EVENT_DRAW_MAIN, s);
    lv_obj_add_event_cb(s->root, on_size, LV_EVENT_SIZE_CHANGED, s);

    /* The game's own way back (owner decision 3): 64 px, top left. */
    back = lv_button_create(s->root);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 64, 64);
    lv_obj_set_pos(back, 8, 8);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, s);
    mark = lv_label_create(back);
    lv_label_set_text(mark, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(mark, &lv_font_montserrat_20, 0); /* the LV_SYMBOL_* source */
    lv_obj_set_style_text_color(mark, lv_color_hex(INK_IVORY), 0);
    lv_obj_center(mark);

    t0 = now_us();
    if (cg_atlas_build(&s->atlas, s->root, CARD_W, CARD_H, N_CARDS + 1, compose, NULL) != 0) {
        LOG_WARN("cards-spike: no atlas");
    }
    LOG_INFO("cards-spike: atlas %d x %dx%d RGB565A8 in %u us, %u bytes", s->atlas.count, CARD_W, CARD_H,
             (unsigned)(now_us() - t0), (unsigned)s->atlas.bytes);
    if (cg_shadow_build(&s->shadow, CARD_W, CARD_H, CARD_R, SHADOW_M, 120) != 0) {
        LOG_WARN("cards-spike: no shadow");
    }

    lv_obj_update_layout(body);
    layout(s);

    s->disp = lv_obj_get_display(s->root);
    lv_display_add_event_cb(s->disp, on_render, LV_EVENT_REFR_START, s);
    lv_display_add_event_cb(s->disp, on_render, LV_EVENT_RENDER_START, s);
    lv_display_add_event_cb(s->disp, on_render, LV_EVENT_RENDER_READY, s);
    lv_display_add_event_cb(s->disp, on_render, LV_EVENT_REFR_READY, s);
    if (pocketos_shell_reduced_motion()) {
        LOG_INFO("cards-spike: reduced motion, end state shown, no timer");
    } else {
        s->loop_start = lv_tick_get();
        s->timer = lv_timer_create(on_frame, FRAME_MS, s);
    }
    return s;
}

void cg_spike_destroy(void *spike)
{
    struct cg_spike *s = spike;

    if (!s) {
        return;
    }
    if (s->timer) {
        lv_timer_delete(s->timer);
    }
    if (s->disp) {
        lv_display_remove_event_cb_with_user_data(s->disp, on_render, s);
    }
    /* The draw handler reads these buffers; it goes before they do. */
    lv_obj_remove_event_cb_with_user_data(s->root, on_draw, s);
    lv_obj_remove_event_cb_with_user_data(s->root, on_size, s);
    cg_atlas_free(&s->atlas);
    cg_shadow_free(&s->shadow);
    free(s->cloth_px);
    free(s);
}
