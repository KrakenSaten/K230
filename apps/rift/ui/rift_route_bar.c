/*
 * The route line of a direct conversation. See rift_route_bar.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_route_bar.h"

#include "pos_styles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each side of a button's word. */
#define BUTTON_PAD 20

struct rift_route_bar {
    struct rift_app *app;
    lv_obj_t *row;
    lv_obj_t *words;
    lv_obj_t *set;
    lv_obj_t *set_label;
    lv_obj_t *autob;
    int set_enabled;  /* -1 until told */
    int explicit_on;  /* the words carry the accent: an explicit route is set */
    int stacked;      /* the words over the buttons, for a narrow pane */
    char peer[RIFT_KEY_HEX];
};

void rift_route_bar_open_builder(struct rift_app *app, const char *peer)
{
    if (!app || !peer || rift_key_is_channel(peer) >= 0) {
        return;
    }
    if (rift_route_open(&app->model.route, peer) != 0) {
        return;
    }
    /* MAP is where repeaters have a place; its panel becomes the builder's
     * while the draft is open (ui/rift_route_view.c). The composer, and
     * what is typed in it, stay where they are. */
    rift_app_show_section(app, RIFT_SEC_MAP);
}

static void on_set(lv_event_t *e)
{
    struct rift_route_bar *b = lv_event_get_user_data(e);

    rift_route_bar_open_builder(b->app, b->peer);
}

static void on_auto(lv_event_t *e)
{
    struct rift_route_bar *b = lv_event_get_user_data(e);

    /* Back to the service's own choice for this conversation. The route
     * chosen is forgotten; no learned route of the contact was ever touched. */
    rift_route_auto(&b->app->model.route, b->peer);
    rift_app_refresh(b->app);
}

struct rift_route_bar *rift_route_bar_create(struct rift_app *app, lv_obj_t *parent, int stacked)
{
    struct rift_route_bar *b = calloc(1, sizeof(*b));

    if (!b) {
        return NULL;
    }
    b->app = app;
    b->set_enabled = -1;
    b->stacked = stacked;
    b->row = lv_obj_create(parent);
    lv_obj_remove_style_all(b->row);
    lv_obj_set_width(b->row, LV_PCT(100));
    lv_obj_set_height(b->row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b->row, stacked ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b->row, 12, 0);
    lv_obj_set_style_pad_row(b->row, 8, 0);
    lv_obj_set_style_pad_top(b->row, 8, 0);
    lv_obj_remove_flag(b->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b->row, LV_OBJ_FLAG_CLICKABLE);

    b->words = lv_label_create(b->row);
    lv_obj_remove_style_all(b->words);
    pos_style_add(b->words, POS_STYLE_CAPTION, 0);
    lv_label_set_long_mode(b->words, LV_LABEL_LONG_WRAP);
    if (stacked) {
        /* A line of its own, the buttons under it sharing theirs. */
        lv_obj_set_width(b->words, LV_PCT(100));
    } else {
        lv_obj_set_flex_grow(b->words, 1);
        lv_obj_set_width(b->words, 1);
    }
    lv_label_set_text(b->words, "");

    b->set = rift_action(b->row, "SET ROUTE", 0, 1, on_set, b);
    lv_obj_set_flex_grow(b->set, 0);
    b->set_label = lv_obj_get_child(b->set, 0);
    b->autob = rift_action(b->row, "AUTO", 0, 1, on_auto, b);
    lv_obj_set_flex_grow(b->autob, 0);
    if (stacked) {
        /* A line each: the pane is too narrow for EDIT ROUTE beside AUTO. */
        lv_obj_set_width(b->set, LV_PCT(100));
        lv_obj_set_width(b->autob, LV_PCT(100));
    }
    lv_obj_add_flag(b->autob, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(b->row, LV_OBJ_FLAG_HIDDEN);
    return b;
}

void rift_route_bar_destroy(struct rift_route_bar *b)
{
    free(b); /* the objects are the thread's, deleted with it */
}

static void set_enabled(struct rift_route_bar *b, int on)
{
    if (on != b->set_enabled) {
        rift_action_set_enabled(b->set, 0, on);
        b->set_enabled = on;
    }
}

/* The staged route in words: "EXPLICIT · 2 RELAYS: RPT-NORD › RPT-7". The
 * chain without "YOU" and the recipient, which the thread already names. */
static void explicit_words(const struct rift_model *m, const struct rift_route_stage *s,
                           char *out, size_t len)
{
    char chain[RIFT_CHAIN_MAX];
    char count[16];
    const char *relays = chain;

    if (s->count == 0) {
        snprintf(out, len, "ROUTE EXPLICIT" RIFT_SEP "DIRECT, NO RELAY" RIFT_SEP "NEVER FLOODED");
        return;
    }
    rift_route_chain(m, s->conv, (const char(*)[RIFT_ROUTE_KEY])s->via, s->count, chain,
                     sizeof(chain));
    /* "YOU › " off the front and " › <recipient>" off the end. */
    if (strncmp(chain, "YOU \xE2\x80\xBA ", 8) == 0) {
        relays = chain + 8;
    }
    {
        char *last = NULL;
        char *p = strstr((char *)relays, " \xE2\x80\xBA ");

        while (p) {
            last = p;
            p = strstr(p + 1, " \xE2\x80\xBA ");
        }
        if (last) {
            *last = '\0';
        }
    }
    snprintf(out, len, "ROUTE EXPLICIT" RIFT_SEP "%s: %s",
             rift_route_relays_word(s->count, count, sizeof(count)), relays);
}

void rift_route_bar_refresh(struct rift_route_bar *b, const char *peer)
{
    const struct rift_model *m;
    const struct rift_route *r;
    const struct rift_route_stage *s;
    char text[RIFT_CHAIN_MAX + 64];
    int explicit_on;

    if (!b) {
        return;
    }
    if (!peer || rift_key_is_channel(peer) >= 0) {
        lv_obj_add_flag(b->row, LV_OBJ_FLAG_HIDDEN);
        b->peer[0] = '\0';
        return;
    }
    m = &b->app->model;
    r = &m->route;
    snprintf(b->peer, sizeof(b->peer), "%s", peer);
    s = rift_route_staged(r, peer);
    explicit_on = s != NULL;
    if (s && !rift_route_supported(r)) {
        /* Set while a service took it, and this one has not said it does:
         * SEND is refused rather than sent any other way. */
        snprintf(text, sizeof(text),
                 "ROUTE EXPLICIT" RIFT_SEP "%s: SEND IS REFUSED, NOTHING GOES AUTO. "
                 "CHOOSE AUTO TO SEND ANOTHER WAY.",
                 r->caps_known ? "THIS MESHCORED DOES NOT TAKE A CHOSEN ROUTE"
                               : "WAITING FOR MESHCORED TO SAY IT TAKES ONE");
    } else if (s) {
        explicit_words(m, s, text, sizeof(text));
    } else if (!r->caps_known) {
        snprintf(text, sizeof(text), "ROUTE AUTO" RIFT_SEP "FLOOD OR LEARNED DIRECT" RIFT_SEP
                                     "ASKING MESHCORED ABOUT CHOSEN ROUTES");
    } else if (!rift_route_supported(r)) {
        snprintf(text, sizeof(text), "ROUTE AUTO" RIFT_SEP "FLOOD OR LEARNED DIRECT" RIFT_SEP
                                     "THIS MESHCORED CANNOT TAKE A CHOSEN ROUTE");
    } else {
        snprintf(text, sizeof(text), "ROUTE AUTO" RIFT_SEP "FLOOD OR LEARNED DIRECT");
    }
    rift_label_set(b->words, text);
    if (explicit_on != b->explicit_on) {
        if (explicit_on) {
            pos_style_add(b->words, POS_STYLE_ACCENT_TEXT, 0);
        } else {
            lv_obj_remove_style(b->words, pos_style(POS_STYLE_ACCENT_TEXT), 0);
        }
        b->explicit_on = explicit_on;
    }
    rift_label_set(b->set_label, s ? "EDIT ROUTE" : "SET ROUTE");
    {
        /* As wide as its longer word at this text size (DS §46), so the
         * button does not change width when its word does. */
        int32_t w = rift_cell_text_width(b->set_label, "EDIT ROUTE") + 2 * BUTTON_PAD;

        if (!b->stacked && w != lv_obj_get_style_width(b->set, 0)) {
            lv_obj_set_width(b->set, w);
            lv_obj_set_width(b->autob, rift_cell_text_width(b->set_label, "AUTO") + 2 * BUTTON_PAD);
        }
    }
    set_enabled(b, rift_route_supported(r) && !rift_model_sending(m));
    if (s) {
        lv_obj_remove_flag(b->autob, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(b->autob, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(b->row, LV_OBJ_FLAG_HIDDEN);
}
