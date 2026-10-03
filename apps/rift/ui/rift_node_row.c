/*
 * One row of the NODES list. See rift_node_row.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_node_row.h"

#include "pos_styles.h"

#include <stdio.h>
#include <string.h>

int32_t rift_node_strip_width(const struct rift_app *a)
{
    return a->wide ? RIFT_STRIP_W_WIDE : RIFT_STRIP_W;
}

/* Each column's widest words: its header and the longest value
 * rift_format.c writes into it (an SNR below -10 dB, an age past 99 days). */
void rift_node_cols_widen(lv_obj_t *hops, lv_obj_t *rssi, lv_obj_t *snr, lv_obj_t *heard)
{
    rift_cell_widen(hops, "HOPS");
    rift_cell_widen(hops, "DIR");
    rift_cell_widen(rssi, "RSSI");
    rift_cell_widen(rssi, RIFT_MINUS "120");
    rift_cell_widen(snr, "SNR");
    rift_cell_widen(snr, RIFT_MINUS "20.0");
    rift_cell_widen(heard, "HEARD");
    rift_cell_widen(heard, ">99d");
}

lv_obj_t *rift_node_row_line(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *r = lv_obj_create(parent);

    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, height);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, COL_GAP, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    /* A layout box takes no taps. The row's own line asks for them back,
     * and it is the only thing in the list that is clickable: a nested box
     * that kept the default would swallow the tap meant for the row under
     * it (RIFT-DEV-1 wants the whole row width as the hit area). */
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

static void on_row(lv_event_t *e)
{
    const struct rift_node_row *r = lv_event_get_user_data(e);

    if (r->key[0]) {
        rift_app_select(r->app, r->key);
    }
}

static void on_detail(lv_event_t *e)
{
    const struct rift_node_row *r = lv_event_get_user_data(e);

    rift_app_open_detail(r->app, 1);
}

/* Write to this node. It opens COMMS on a conversation with the peer -
 * which may hold nothing yet, and is not given anything to make it look as
 * though it does. It sends nothing; it only points the composer. */
static void on_message(lv_event_t *e)
{
    const struct rift_node_row *r = lv_event_get_user_data(e);

    if (r->key[0]) {
        rift_app_open_conversation(r->app, r->key);
    }
}

static void build_expansion(struct rift_node_row *r)
{
    lv_obj_t *line;
    lv_obj_t *bar;

    r->expand = lv_obj_create(r->slot);
    lv_obj_remove_style_all(r->expand);
    lv_obj_set_width(r->expand, LV_PCT(100));
    lv_obj_set_height(r->expand, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r->expand, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r->expand, 6, 0);
    lv_obj_set_style_pad_bottom(r->expand, 6, 0);
    lv_obj_remove_flag(r->expand, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r->expand, LV_OBJ_FLAG_CLICKABLE);

    line = rift_node_row_line(r->expand, RIFT_ROW_H);
    r->exp_glyph = rift_glyph_create(line);
    r->exp_state = rift_cell(line, POS_STYLE_VALUE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(r->exp_state, 1);

    r->exp_chain = lv_label_create(r->expand);
    lv_obj_remove_style_all(r->exp_chain);
    pos_style_add(r->exp_chain, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(r->exp_chain, LV_PCT(100));
    lv_label_set_long_mode(r->exp_chain, LV_LABEL_LONG_WRAP);
    lv_label_set_text(r->exp_chain, "");

    r->exp_signal = lv_label_create(r->expand);
    lv_obj_remove_style_all(r->exp_signal);
    pos_style_add(r->exp_signal, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(r->exp_signal, LV_PCT(100));
    lv_label_set_long_mode(r->exp_signal, LV_LABEL_LONG_WRAP);
    lv_label_set_text(r->exp_signal, "");

    /* Two actions. The design's third, PATH, was drawn in the disabled
     * treatment with nothing behind it; the path is on DETAIL, whole, and a
     * button that can never be pressed is width taken from the two that can. */
    bar = rift_node_row_line(r->expand, RIFT_TOUCH_H);
    lv_obj_set_style_pad_column(bar, 12, 0);
    r->exp_message = rift_action(bar, "MESSAGE", 1, 1, on_message, r);
    rift_action(bar, "DETAIL \xE2\x80\xBA", 0, 1, on_detail, r);
    r->exp_why = lv_label_create(r->expand);
    lv_obj_remove_style_all(r->exp_why);
    pos_style_add(r->exp_why, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(r->exp_why, LV_PCT(100));
    lv_label_set_long_mode(r->exp_why, LV_LABEL_LONG_WRAP);
    lv_label_set_text(r->exp_why, "");
    lv_obj_add_flag(r->exp_why, LV_OBJ_FLAG_HIDDEN);
}

void rift_node_row_drop_expansion(struct rift_node_row *r)
{
    if (r->expand) {
        lv_obj_delete(r->expand);
        r->expand = NULL;
        r->exp_glyph = NULL;
        r->exp_state = NULL;
        r->exp_chain = NULL;
        r->exp_signal = NULL;
        r->exp_message = NULL;
        r->exp_why = NULL;
    }
}

/* One row of the pool, built once and bound to whichever node needs it. */
void rift_node_row_build(struct rift_node_row *r, struct rift_app *a, lv_obj_t *list)
{
    memset(r, 0, sizeof(*r));
    r->app = a;
    r->item = -1;

    r->slot = lv_obj_create(list);
    lv_obj_remove_style_all(r->slot);
    lv_obj_set_width(r->slot, LV_PCT(100));
    lv_obj_set_height(r->slot, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r->slot, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(r->slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r->slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(r->slot, LV_OBJ_FLAG_HIDDEN);

    r->line = rift_node_row_line(r->slot, RIFT_ROW_H);
    /* The whole row is the hit area, and a tap on it selects and does
     * nothing else (RIFT-DEV-1). */
    lv_obj_add_flag(r->line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r->line, on_row, LV_EVENT_CLICKED, r);

    /* The identity mark (DS §37.3) before the link glyph: who, then how. */
    r->ident = rift_vrule(r->line, RIFT_IDENT_W);
    rift_vrule_set(r->ident, RIFT_TONE_NONE);
    r->glyph = rift_glyph_create(r->line);
    /* The name and its role tag are one column, so the columns after them
     * do not move by the width of a tag that some rows have and some do
     * not. The name takes what the tag leaves and ends in an ellipsis when
     * it does not fit: a remote name is chosen by whoever is on the air and
     * can be any length. */
    r->namebox = rift_node_row_line(r->line, RIFT_ROW_H);
    lv_obj_set_flex_grow(r->namebox, 1);
    lv_obj_set_width(r->namebox, 1);
    lv_obj_set_style_pad_column(r->namebox, TAG_GAP, 0);
    r->name = rift_cell(r->namebox, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(r->name, 1);
    lv_obj_set_width(r->name, 1);
    r->tag = rift_cell(r->namebox, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    r->strip = rift_strip_create(r->line, rift_node_strip_width(a));
    r->hops = rift_cell(r->line, POS_STYLE_CAPTION, COL_HOPS, LV_TEXT_ALIGN_RIGHT);
    r->rssi = rift_cell(r->line, POS_STYLE_CAPTION, COL_RSSI, LV_TEXT_ALIGN_RIGHT);
    r->snr = rift_cell(r->line, POS_STYLE_CAPTION, COL_SNR, LV_TEXT_ALIGN_RIGHT);
    r->heard = rift_cell(r->line, POS_STYLE_CAPTION, COL_HEARD, LV_TEXT_ALIGN_RIGHT);
    rift_node_cols_widen(r->hops, r->rssi, r->snr, r->heard);
    /* After the age and close to it, and away from RSSI and SNR: it is when
     * the node was heard, and must not read as part of how loud it was. */
    r->pulse = rift_pulse_create(r->line);
    lv_obj_set_style_margin_left(r->pulse, PULSE_PULL, 0);
}

/* The selection's look, on the one row that shows it and off every other. */
void rift_node_row_select(struct rift_node_row *r, int selected)
{
    int expand = selected && !r->app->wide;

    if (selected != r->selected) {
        if (selected) {
            pos_style_add(r->slot, POS_STYLE_SLAB, 0);
            pos_style_add(r->slot, POS_STYLE_SELECTED, 0);
            lv_obj_set_style_pad_hor(r->slot, SELECTED_INSET_H, 0);
            lv_obj_set_style_pad_ver(r->slot, SELECTED_INSET_V, 0);
        } else {
            lv_obj_remove_style(r->slot, pos_style(POS_STYLE_SLAB), 0);
            lv_obj_remove_style(r->slot, pos_style(POS_STYLE_SELECTED), 0);
            lv_obj_set_style_pad_hor(r->slot, 0, 0);
            lv_obj_set_style_pad_ver(r->slot, 0, 0);
        }
        r->selected = selected;
    }
    if (expand && !r->expand) {
        build_expansion(r);
    } else if (!expand && r->expand) {
        rift_node_row_drop_expansion(r);
    }
}

/* The room the name has in a row: the name box less the tag and its gap.
 * Measured from the box, which has the same width in every row of one
 * shape; falls back to the cell's own width while nothing is laid out. */
static int32_t name_room(struct rift_node_row *r, const char *tag)
{
    int32_t box = lv_obj_get_width(r->namebox);

    if (box <= 0) {
        return 0;
    }
    if (tag && tag[0]) {
        box -= rift_cell_text_width(r->tag, tag) + TAG_GAP;
    }
    return box > 1 ? box : 1;
}

void rift_node_row_update(struct rift_node_row *r, const struct rift_node *n, int64_t now)
{
    struct rift_app *a = r->app;
    struct rift_path p;
    char text[RIFT_CHAIN_MAX];
    char small[RIFT_SIGNAL_MAX];
    const char *tag = rift_type_tag(n->type, n->have_type);
    int stale = rift_node_is_stale(n, now);

    if (rift_path_parse(n, &p) != 0) {
        memset(&p, 0, sizeof(p));
    }
    rift_glyph_set(r->glyph, rift_app_glyph(n, now));
    /* Who this is, for a node that is somebody: a chat node or a room, in
     * the accent its key hashes to, the same one its conversation carries.
     * A repeater or a sensor is infrastructure and stays neutral, unless a
     * state of its own asks for emphasis elsewhere in the row. */
    if (rift_ident_for_type(n->type, n->have_type)) {
        rift_vrule_set_identity(r->ident, rift_ident_hash(n->key));
    } else {
        rift_vrule_set(r->ident, RIFT_TONE_NONE);
    }
    /* The tag first, and the name fitted to what it leaves. The room is
     * worked out rather than read after a layout, so a row rebound in the
     * middle of a scroll is fitted to the width it is about to have, not to
     * the width the last node it showed left it. */
    rift_label_set(r->tag, tag ? tag : "");
    rift_fmt_label(n, text, sizeof(text));
    rift_cell_set_text_fit_room(r->name, text, name_room(r, tag));
    rift_strip_set_width(r->strip, rift_node_strip_width(a));
    rift_strip_set(r->strip, &p, stale);
    rift_fmt_hops(n, small, sizeof(small));
    rift_label_set(r->hops, small);
    rift_fmt_rssi(n->rssi_dbm, n->have_rssi, small, sizeof(small));
    rift_label_set(r->rssi, small);
    rift_fmt_snr(n->snr_db, n->have_snr, small, sizeof(small));
    rift_label_set(r->snr, small);
    /* SNR is one of the extra columns landscape has room for (handoff §9);
     * portrait shows the same node with one column fewer, never with a
     * value squeezed into a place it does not fit. */
    if (a->wide) {
        lv_obj_remove_flag(r->snr, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(r->snr, LV_OBJ_FLAG_HIDDEN);
    }
    /* How lately it was heard: the age, and the same age bucketed. Nothing
     * else goes into it - not the signal, not the hop count. */
    rift_pulse_set(r->pulse, rift_pulse_of(now - n->heard_mono_ms, n->have_heard));
    rift_fmt_age(now - n->heard_mono_ms, n->have_heard, small, sizeof(small));
    rift_label_set(r->heard, small);

    if (r->expand) {
        const struct rift_model *m = &a->model;
        const char *self =
            (m->have_identity && m->self_name[0]) ? m->self_name : "this device";
        char label[RIFT_LABEL_MAX];
        char rssi[RIFT_SIGNAL_MAX];
        char snr[RIFT_SIGNAL_MAX];
        char advert[RIFT_AGE_MAX];

        rift_glyph_set(r->exp_glyph, rift_app_glyph(n, now));
        rift_fmt_state(n, text, sizeof(text));
        rift_label_set(r->exp_state, text);
        rift_fmt_label(n, label, sizeof(label));
        rift_path_chain(self, &p, label, rift_app_resolve, a, text, sizeof(text));
        rift_label_set(r->exp_chain, text);
        rift_fmt_rssi(n->rssi_dbm, n->have_rssi, rssi, sizeof(rssi));
        rift_fmt_snr(n->snr_db, n->have_snr, snr, sizeof(snr));
        rift_fmt_age(now - n->heard_mono_ms, n->have_heard, advert, sizeof(advert));
        snprintf(text, sizeof(text),
                 "LAST HOP %s" RIFT_SEP "SNR %s" RIFT_SEP "HEARD %s" RIFT_SEP "%s" RIFT_SEP
                 "%u OBS",
                 rssi, snr, advert,
                 rift_pulse_word(rift_pulse_of(now - n->heard_mono_ms, n->have_heard)),
                 n->observations);
        rift_label_set(r->exp_signal, text);
        /* No MESSAGE for a node that takes none; the line under the bar
         * says why, and DETAIL is still there. */
        if (rift_node_can_message(n)) {
            lv_obj_remove_flag(r->exp_message, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(r->exp_why, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(r->exp_message, LV_OBJ_FLAG_HIDDEN);
            rift_label_set(r->exp_why, rift_node_no_message_why(n));
            lv_obj_remove_flag(r->exp_why, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
