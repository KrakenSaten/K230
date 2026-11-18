/*
 * PacketShark's INSPECT tab (milestone M6; mockup 03, DS §59.1, §59.6, the
 * deviations in §59.11): one packet, its two ends, its layers with their
 * fields, and its stored bytes in hex.
 *
 * Everything shown is text the helper formatted (DESIGN_DECISIONS D1): the
 * shell holds no packet bytes, only the hex lines it was sent. A field row
 * (36 px) only selects; the selected field's bytes are marked in the hex,
 * and FILTER BY on the capture bar filters the list on it when it stands
 * for a host, a port or a protocol. An end's slab filters on its host. PIN
 * keeps the packet past the buffer's turning (D9); the arrows walk the
 * list's order.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEX_TEXT_MAX (PSK_HEX_LINES * 48)
#define FIELD_INDENT 26 /* the selected outline starts here (mockup 03) */

/* ---- small parts ---------------------------------------------------------------------- */

static lv_obj_t *col(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *o = psk_box(parent);

    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

static lv_obj_t *line(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *o = psk_box(parent);

    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, enum pos_style_role role, lv_label_long_mode_t mode)
{
    lv_obj_t *lb = pocketui_label(parent, text, role);

    lv_label_set_long_mode(lb, mode);
    return lb;
}

static void tappable(lv_obj_t *o, lv_event_cb_t cb, void *user, intptr_t index)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    pos_style_add(o, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_user_data(o, (void *)index);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, user);
}

static int index_of(lv_event_t *e)
{
    return (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
}

/* ---- events --------------------------------------------------------------------------- */

static void on_layer(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    int i = index_of(e);

    if (i >= 0 && i < PSK_LAYERS) {
        a->layer_open[i] = !a->layer_open[i];
    } else {
        a->bytes_open = !a->bytes_open;
    }
    psk_inspect_paint(a);
}

static void on_field(lv_event_t *e)
{
    psk_select_field(lv_event_get_user_data(e), index_of(e));
}

static void on_end(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    const struct psk_model *m = a->model;
    const struct ppc_evt_end *end = index_of(e) ? &m->pkt.dst : &m->pkt.src;

    if (m->have_pkt && end->filter[0]) {
        psk_filter_by(a, end->filter);
    }
}

static void on_nav(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    int what = index_of(e);

    if (!a->model->have_pkt) {
        return;
    }
    if (what == 0) {
        psk_toggle_pin(a);
    } else {
        psk_open_packet(a, a->model->pkt.head.seq, what < 0 ? -1 : 1);
    }
}

/* ---- the hex view's mark ---------------------------------------------------------------- */

struct hex_mark {
    int32_t off;    /* the selected field's first byte, -1 none */
    uint32_t len;
    uint32_t lines;
};

/* The selected field's bytes, behind the hex (mockup 03): a raised cell
 * under each byte and the accent rule beneath it. Columns: offset (4),
 * two spaces, eight bytes of three characters with one more after the
 * fourth. */
static void hex_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    const struct hex_mark *hm = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    lv_draw_rect_dsc_t dsc;
    lv_area_t c;
    int32_t cw;
    int32_t lh;
    uint32_t b;

    if (!hm || !layer || !font || hm->off < 0 || hm->len == 0) {
        return;
    }
    cw = lv_font_get_glyph_width(font, '0', '0') + lv_obj_get_style_text_letter_space(obj, LV_PART_MAIN);
    lh = lv_font_get_line_height(font) + lv_obj_get_style_text_line_space(obj, LV_PART_MAIN);
    lv_obj_get_content_coords(obj, &c);
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 0;
    for (b = (uint32_t)hm->off; b < (uint32_t)hm->off + hm->len && b / PPC_HEX_PER_LINE < hm->lines; b++) {
        uint32_t j = b % PPC_HEX_PER_LINE;
        int32_t colx = 6 + 3 * (int32_t)j + (j >= 4 ? 1 : 0);
        lv_area_t r;

        r.x1 = c.x1 + colx * cw - 3;
        r.x2 = r.x1 + 2 * cw + 5;
        r.y1 = c.y1 + (int32_t)(b / PPC_HEX_PER_LINE) * lh;
        r.y2 = r.y1 + lh - 2;
        dsc.bg_color = pos_theme_color(POS_COLOR_SURFACE_RAISED);
        lv_draw_rect(layer, &dsc, &r);
        r.y1 = r.y2 - 1;
        dsc.bg_color = pos_theme_color(POS_COLOR_ACCENT_PRIMARY);
        lv_draw_rect(layer, &dsc, &r);
    }
}

static void mark_delete(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);

    free(lv_obj_get_user_data(obj));
    lv_obj_set_user_data(obj, NULL);
}

/* ---- building ------------------------------------------------------------------------- */

static lv_obj_t *face(lv_obj_t *parent, const char *text, bool symbol)
{
    lv_obj_t *f = psk_box(parent);
    lv_obj_t *lb;

    lv_obj_set_size(f, 68, PSK_TAB_H);
    pos_style_add(f, POS_STYLE_BUTTON_SECONDARY, 0);
    lv_obj_set_style_pad_all(f, 0, 0);
    lb = pocketui_label(f, text, symbol ? POS_STYLE_SYMBOL : POS_STYLE_BUTTON_LABEL);
    if (symbol) {
        pos_style_add(lb, POS_STYLE_ACCENT_TEXT, 0);
    }
    lv_obj_center(lb);
    return f;
}

static void layer_row(struct psk_app *a, lv_obj_t *parent, struct psk_layerrow *l, intptr_t index)
{
    l->row = line(parent, 10);
    lv_obj_set_height(l->row, PSK_ROW_H);
    lv_obj_set_style_radius(l->row, POCKETUI_RADIUS, 0);
    pos_style_add(l->row, POS_STYLE_DIVIDER, 0);
    tappable(l->row, on_layer, a, index);
    l->bar = psk_class_bar(l->row);
    lv_obj_set_height(l->bar, PSK_ROW_H - 20);
    l->tri = pocketui_label(l->row, LV_SYMBOL_RIGHT, POS_STYLE_SYMBOL);
    pos_style_add(l->tri, POS_STYLE_ACCENT_TEXT, 0);
    lv_obj_set_width(l->tri, 22);
    l->name = label(l->row, "", POS_STYLE_ROW_TITLE, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l->name, LV_SIZE_CONTENT);
    l->summary = label(l->row, "", POS_STYLE_CAPTION, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l->summary, 1);
    lv_obj_set_flex_grow(l->summary, 1);
    lv_obj_set_style_text_align(l->summary, LV_TEXT_ALIGN_RIGHT, 0);
    l->fields = NULL;
}

void psk_inspect_build(struct psk_app *a, lv_obj_t *page)
{
    lv_obj_t *head;
    lv_obj_t *left;
    lv_obj_t *top;
    lv_obj_t *nav;
    lv_obj_t *ends;
    lv_obj_t *layers;
    struct hex_mark *hm;
    int i;

    lv_obj_set_style_pad_row(page, 14, 0);
    a->in_empty = pocketui_card(page);
    lv_obj_set_width(a->in_empty, LV_PCT(100));
    lv_obj_set_height(a->in_empty, LV_SIZE_CONTENT);
    lv_obj_clear_flag(a->in_empty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(a->in_empty, 8, 0);
    label(a->in_empty, "NO PACKET OPEN", POS_STYLE_ROW_TITLE, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label(a->in_empty, "Tap a packet in PACKETS to see its fields and bytes.", POS_STYLE_TEXT_SECONDARY,
                           LV_LABEL_LONG_WRAP),
                     LV_PCT(100));

    a->in_note = label(page, "", POS_STYLE_CAPTION, LV_LABEL_LONG_WRAP);
    pos_style_add(a->in_note, POS_STYLE_STATUS_WARN_TEXT, 0);
    lv_obj_set_width(a->in_note, LV_PCT(100));
    psk_set_hidden(a->in_note, true);
    a->in_body = col(page, 14);

    /* The packet: number, class, finding; PIN, previous, next (mockup 03). */
    head = line(a->in_body, 8);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    left = col(head, 4);
    lv_obj_set_width(left, 1);
    lv_obj_set_flex_grow(left, 1);
    top = line(left, 12);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    a->in_num = label(top, "", POS_STYLE_VALUE, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(a->in_num, LV_SIZE_CONTENT);
    a->in_word = label(top, "", POS_STYLE_CAPTION, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(a->in_word, LV_SIZE_CONTENT);
    a->in_finding = label(top, "", POS_STYLE_CAPTION, LV_LABEL_LONG_CLIP);
    pos_style_add(a->in_finding, POS_STYLE_STATUS_WARN_TEXT, 0);
    lv_obj_set_width(a->in_finding, LV_SIZE_CONTENT);
    a->in_meta1 = label(left, "", POS_STYLE_CAPTION, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->in_meta1, LV_PCT(100));
    a->in_meta2 = label(left, "", POS_STYLE_CAPTION, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->in_meta2, LV_PCT(100));
    nav = line(head, 8);
    lv_obj_set_width(nav, LV_SIZE_CONTENT);
    /* Room for PIN's selected outline, drawn outside the face. */
    lv_obj_set_style_pad_all(nav, 3, 0);
    lv_obj_set_style_margin_top(nav, -3, 0);
    lv_obj_set_style_margin_right(nav, -3, 0);
    a->in_pin = face(nav, "PIN", false);
    tappable(a->in_pin, on_nav, a, 0);
    a->in_prev = face(nav, LV_SYMBOL_LEFT, true);
    tappable(a->in_prev, on_nav, a, -1);
    a->in_next = face(nav, LV_SYMBOL_RIGHT, true);
    tappable(a->in_next, on_nav, a, 1);

    /* The two ends: each a slab that filters on its host. */
    ends = line(a->in_body, 8);
    lv_obj_set_flex_align(ends, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (i = 0; i < 2; i++) {
        lv_obj_t *slab = col(ends, 2);

        lv_obj_set_width(slab, 1);
        lv_obj_set_flex_grow(slab, 1);
        lv_obj_set_style_min_height(slab, 84, 0);
        pos_style_add(slab, POS_STYLE_SLAB, 0);
        lv_obj_set_style_pad_all(slab, 12, 0);
        lv_obj_set_style_pad_top(slab, 8, 0);
        tappable(slab, on_end, a, i);
        label(slab, i ? "DESTINATION" : "SOURCE", POS_STYLE_CAPTION, LV_LABEL_LONG_CLIP);
        a->in_end_addr[i] = label(slab, "", POS_STYLE_VALUE, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(a->in_end_addr[i], LV_PCT(100));
        a->in_end_sub[i] = label(slab, "", POS_STYLE_CAPTION, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(a->in_end_sub[i], LV_PCT(100));
        a->in_end[i] = slab;
        if (i == 0) {
            lv_obj_t *arrow = label(ends, "\xe2\x86\x92", POS_STYLE_VALUE, LV_LABEL_LONG_CLIP);

            lv_obj_set_width(arrow, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_hor(arrow, 6, 0);
        }
    }

    /* The layers, each with a column for its fields; the fields are a pool
     * moved into the column of the layer they belong to. */
    layers = col(a->in_body, 0);
    for (i = 0; i < PSK_LAYERS; i++) {
        layer_row(a, layers, &a->layer[i], i);
        a->layer[i].fields = col(layers, 0);
        lv_obj_set_style_pad_left(a->layer[i].fields, FIELD_INDENT, 0);
        /* What the packet's findings say about this layer (M7, mockup 03):
         * the first line under the layer, in status_warn with its `!`. */
        a->layer[i].finding = label(a->layer[i].fields, "", POS_STYLE_CAPTION, LV_LABEL_LONG_WRAP);
        pos_style_add(a->layer[i].finding, POS_STYLE_STATUS_WARN_TEXT, 0);
        lv_obj_set_width(a->layer[i].finding, LV_PCT(100));
        lv_obj_set_style_pad_ver(a->layer[i].finding, 8, 0);
        psk_set_hidden(a->layer[i].finding, true);
        /* What the DNS and TCP diagnosis says of it (M11.1): not a fault,
         * so in the secondary text, under any finding. */
        a->layer[i].detail = label(a->layer[i].fields, "", POS_STYLE_CAPTION, LV_LABEL_LONG_WRAP);
        pos_style_add(a->layer[i].detail, POS_STYLE_TEXT_SECONDARY, 0);
        lv_obj_set_width(a->layer[i].detail, LV_PCT(100));
        lv_obj_set_style_pad_bottom(a->layer[i].detail, 8, 0);
        psk_set_hidden(a->layer[i].detail, true);
        /* Room for the selected outline at the right. */
        lv_obj_set_style_pad_right(a->layer[i].fields, 3, 0);
        psk_set_hidden(a->layer[i].row, true);
        psk_set_hidden(a->layer[i].fields, true);
    }
    for (i = 0; i < PSK_FIELDS; i++) {
        struct psk_fieldrow *f = &a->field[i];

        f->row = line(a->layer[0].fields, 10);
        lv_obj_set_style_min_height(f->row, PSK_FIELD_H, 0);
        lv_obj_set_style_pad_hor(f->row, 12, 0);
        lv_obj_set_style_pad_ver(f->row, 4, 0);
        lv_obj_set_style_radius(f->row, POCKETUI_RADIUS, 0);
        pos_style_add(f->row, POS_STYLE_DIVIDER, 0);
        tappable(f->row, on_field, a, i);
        f->name = label(f->row, "", POS_STYLE_CAPTION, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(f->name, 1);
        lv_obj_set_flex_grow(f->name, 1);
        f->value = label(f->row, "", POS_STYLE_VALUE, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(f->value, LV_SIZE_CONTENT);
        lv_obj_set_style_text_align(f->value, LV_TEXT_ALIGN_RIGHT, 0);
        psk_set_hidden(f->row, true);
    }

    /* Bytes: the stored bytes in hex, eight to a line. */
    {
        struct psk_layerrow b;

        layer_row(a, layers, &b, -1);
        a->in_bytes_row = b.row;
        a->in_bytes_tri = b.tri;
        a->in_bytes_sum = b.summary;
        psk_text(b.name, "Bytes");
        psk_class_bar_set(b.bar, -1);
    }
    a->in_hex = pocketui_label(layers, "", POS_STYLE_CAPTION);
    lv_obj_add_style(a->in_hex, pos_style_fixed_size(POS_STYLE_CAPTION), 0);
    lv_obj_set_style_text_letter_space(a->in_hex, 0, 0);
    lv_obj_set_style_text_line_space(a->in_hex, 4, 0);
    lv_obj_set_style_pad_left(a->in_hex, 38, 0);
    lv_obj_set_style_pad_top(a->in_hex, 6, 0);
    pos_style_add(a->in_hex, POS_STYLE_TEXT_PRIMARY, 0);
    lv_obj_remove_style(a->in_hex, pos_style_fixed_size(POS_STYLE_CAPTION), 0);
    lv_obj_add_style(a->in_hex, pos_style_fixed_size(POS_STYLE_CAPTION), 0);
    /* The mark's numbers live with the hex label and are freed with it. */
    hm = calloc(1, sizeof(*hm));
    if (hm) {
        hm->off = -1;
    }
    lv_obj_set_user_data(a->in_hex, hm);
    lv_obj_add_event_cb(a->in_hex, mark_delete, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(a->in_hex, hex_draw, LV_EVENT_DRAW_MAIN_BEGIN, a);
    a->in_hex_note = label(layers, "", POS_STYLE_CAPTION, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->in_hex_note, LV_PCT(100));
    lv_obj_set_style_pad_left(a->in_hex_note, 38, 0);
    a->insp_field = -1;
    a->bytes_open = true;
}

/* ---- painting ------------------------------------------------------------------------- */

/* A dots-mode label with an automatic height wraps rather than shortening
 * (DS §46.5): its height is held at one line of its role, at the text size
 * in force. */
static void one_line_of(lv_obj_t *lb, enum pos_style_role role)
{
    int32_t h = pocketui_role_line_height(role);

    if (lv_obj_get_style_height(lb, LV_PART_MAIN) != h) {
        lv_obj_set_height(lb, h);
    }
}


/* A new packet: the deepest protocol layer opens, the others close, nothing
 * is selected, and every field row goes to its layer's column in order. */
static void new_packet(struct psk_app *a, const struct psk_packet *p)
{
    uint32_t i;

    a->insp_shown = p->head.seq;
    a->insp_field = -1;
    for (i = 0; i < PSK_LAYERS; i++) {
        a->layer_open[i] = p->nlayers > 0 && i == p->nlayers - 1u;
    }
    /* A layer with a finding opens too: what is wrong is what is shown. */
    for (i = 0; i < p->nfindings; i++) {
        if (p->finding[i].layer < p->nlayers) {
            a->layer_open[p->finding[i].layer] = true;
        }
    }
    for (i = 0; i < PSK_FIELDS; i++) {
        lv_obj_t *want = i < p->nfields ? a->layer[p->field[i].layer].fields : a->layer[0].fields;

        lv_obj_set_parent(a->field[i].row, want);
        lv_obj_move_to_index(a->field[i].row, -1);
    }
}

static void paint_value(struct psk_fieldrow *f, const char *text)
{
    int32_t room = lv_obj_get_content_width(f->row) * 62 / 100;

    psk_text(f->value, text);
    /* Short values on one line at the right; a long one wraps in the right
     * part of the row and the row grows (DS §46.5). */
    if (room > 0 && pocketui_text_width(f->value, text) > room) {
        lv_obj_set_width(f->value, room);
    } else {
        lv_obj_set_width(f->value, LV_SIZE_CONTENT);
    }
}

static void paint_head(struct psk_app *a, const struct psk_packet *p)
{
    char b[96];
    char t[24];
    char dir[8];
    unsigned i;
    int hue = psk_class_hue(p->head.cls);

    snprintf(b, sizeof(b), "#%u", (unsigned)p->head.seq);
    psk_text(a->in_num, b);
    psk_text(a->in_word, p->head.word);
    /* The class word in its hue (a colour-only style over the caption);
     * OTHER keeps the caption's own secondary text. */
    for (i = 0; i < 8; i++) {
        lv_obj_remove_style(a->in_word, pos_style_identity(i), 0);
    }
    if (hue >= 0) {
        lv_obj_add_style(a->in_word, pos_style_identity((unsigned)hue), 0);
    }
    /* The packet's finding beside its class (mockup 03): the first one,
     * and how many more. */
    if (p->head.signal) {
        static const char *const names[PPC_SIGNAL_COUNT] = { "RETRANSMISSION", "RESET",     "DNS FAILURE",
                                                             "UNREACHABLE",    "NO ANSWER", "NO DNS REPLY" };
        unsigned first = PPC_SIGNAL_COUNT;
        unsigned more = 0;

        for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
            if (p->head.signal & (1u << i)) {
                if (first == PPC_SIGNAL_COUNT) {
                    first = i;
                } else {
                    more++;
                }
            }
        }
        if (first == PPC_SIGNAL_COUNT) {
            snprintf(b, sizeof(b), "! FINDING");
        } else if (more) {
            snprintf(b, sizeof(b), "! %s +%u", names[first], more);
        } else {
            snprintf(b, sizeof(b), "! %s", names[first]);
        }
        psk_text(a->in_finding, b);
    }
    psk_set_hidden(a->in_finding, p->head.signal == 0);
    snprintf(dir, sizeof(dir), "%s", p->head.dir);
    for (i = 0; dir[i]; i++) {
        if (dir[i] >= 'a' && dir[i] <= 'z') {
            dir[i] = (char)(dir[i] - 'a' + 'A');
        }
    }
    snprintf(b, sizeof(b), "%s · +%llu.%03llu MS · %s", psk_fmt_rel(p->head.rel_us, t, sizeof(t)),
             (unsigned long long)(p->head.gap_us / 1000u), (unsigned long long)(p->head.gap_us % 1000u), dir);
    psk_text(a->in_meta1, b);
    if (p->head.flags & PPC_ROWF_GONE) {
        snprintf(b, sizeof(b), "%u B ON WIRE · SUMMARY ONLY%s", (unsigned)p->head.wire_len,
                 (p->head.flags & PPC_ROWF_PINNED) ? " · PINNED" : "");
    } else {
        snprintf(b, sizeof(b), "%u B ON WIRE · %u B STORED%s", (unsigned)p->head.wire_len, (unsigned)p->head.stored,
                 (p->head.flags & PPC_ROWF_PINNED) ? " · PINNED" : "");
    }
    psk_text(a->in_meta2, b);
    psk_style_on(a->in_pin, POS_STYLE_SELECTED, (p->head.flags & PPC_ROWF_PINNED) != 0, &a->pin_selected);
    psk_tone(lv_obj_get_child(a->in_pin, 0), (p->head.flags & PPC_ROWF_PINNED) ? POS_STYLE_ACCENT_TEXT : -1, false);
    /* The arrows walk the list's order; at an end there is nowhere to go. */
    psk_set_hidden(a->in_prev, false);
    lv_obj_set_style_opa(lv_obj_get_child(a->in_prev, 0), p->head.rank > 0 && p->head.rank < p->head.match ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_opa(lv_obj_get_child(a->in_next, 0),
                         p->head.rank + 1u < p->head.match ? LV_OPA_COVER : LV_OPA_40, 0);
    for (i = 0; i < 2; i++) {
        const struct ppc_evt_end *e = i ? &p->dst : &p->src;

        psk_text(a->in_end_addr[i], e->addr);
        psk_text(a->in_end_sub[i], e->sub);
        psk_set_hidden(a->in_end_sub[i], e->sub[0] == '\0');
    }
}

static void paint_hex(struct psk_app *a, const struct psk_packet *p)
{
    static char text[HEX_TEXT_MAX];
    struct hex_mark *hm = lv_obj_get_user_data(a->in_hex);
    size_t o = 0;
    uint32_t i;
    char b[64];

    for (i = 0; i < p->nhex && o + 64 < sizeof(text); i++) {
        int k = snprintf(text + o, sizeof(text) - o, "%s%04x  %-24s    %s", i ? "\n" : "", (unsigned)p->hex[i].off,
                         p->hex[i].hex, p->hex[i].ascii);

        if (k > 0) {
            o += (size_t)k;
        }
    }
    text[o] = '\0';
    psk_text(a->in_hex, text);
    if (p->head.flags & PPC_ROWF_GONE) {
        snprintf(b, sizeof(b), "SUMMARY ONLY");
        psk_text(a->in_hex_note, "The bytes have left the capture buffer; the summary stays.");
    } else if (p->head.stored == 0) {
        snprintf(b, sizeof(b), "NONE STORED");
        psk_text(a->in_hex_note, "");
    } else {
        snprintf(b, sizeof(b), "%u STORED · HEX", (unsigned)p->head.stored);
        if (p->head.stored > p->nhex * PPC_HEX_PER_LINE) {
            char n[48];

            snprintf(n, sizeof(n), "FIRST %u OF %u STORED BYTES", (unsigned)(p->nhex * PPC_HEX_PER_LINE),
                     (unsigned)p->head.stored);
            psk_text(a->in_hex_note, n);
        } else {
            psk_text(a->in_hex_note, "");
        }
    }
    psk_text(a->in_bytes_sum, b);
    one_line_of(a->in_bytes_sum, POS_STYLE_CAPTION);
    psk_text(a->in_bytes_tri, a->bytes_open ? LV_SYMBOL_DOWN : LV_SYMBOL_RIGHT);
    psk_set_hidden(a->in_hex, !a->bytes_open || p->nhex == 0);
    psk_set_hidden(a->in_hex_note, !a->bytes_open || lv_label_get_text(a->in_hex_note)[0] == '\0');
    if (hm) {
        int32_t off = -1;
        uint32_t len = 0;

        if (a->insp_field >= 0 && (uint32_t)a->insp_field < p->nfields) {
            off = p->field[a->insp_field].off;
            len = p->field[a->insp_field].len;
        }
        if (hm->off != off || hm->len != len || hm->lines != p->nhex) {
            hm->off = off;
            hm->len = len;
            hm->lines = p->nhex;
            lv_obj_invalidate(a->in_hex);
        }
    }
}

void psk_inspect_paint(struct psk_app *a)
{
    const struct psk_model *m = a->model;
    const struct psk_packet *p = &m->pkt;
    bool show = psk_capture_present(a) && m->have_pkt && a->insp_seq != 0;
    uint32_t i;

    psk_text(a->in_note, a->insp_note);
    psk_set_hidden(a->in_note, a->insp_note[0] == '\0');
    psk_set_hidden(a->in_empty, show);
    psk_set_hidden(a->in_body, !show);
    if (!show) {
        return;
    }
    if (p->head.seq != a->insp_shown) {
        new_packet(a, p);
    }
    paint_head(a, p);
    for (i = 0; i < PSK_LAYERS; i++) {
        struct psk_layerrow *l = &a->layer[i];
        bool there = i < p->nlayers;

        psk_set_hidden(l->row, !there);
        psk_set_hidden(l->fields, !there || !a->layer_open[i]);
        if (!there) {
            continue;
        }
        psk_class_bar_set(l->bar, p->layer[i].cls);
        one_line_of(l->summary, POS_STYLE_CAPTION);
        psk_text(l->tri, a->layer_open[i] ? LV_SYMBOL_DOWN : LV_SYMBOL_RIGHT);
        psk_text(l->name, p->layer[i].name);
        psk_text(l->summary, p->layer[i].text);
        {
            char f[PSK_FINDINGS * (PPC_FIELD_TEXT + 4)];
            size_t o = 0;
            uint32_t k;

            f[0] = '\0';
            for (k = 0; k < p->nfindings && o < sizeof(f); k++) {
                if (p->finding[k].layer == i) {
                    o += (size_t)snprintf(f + o, sizeof(f) - o, "%s! %s", o ? "\n" : "", p->finding[k].text);
                }
            }
            psk_text(l->finding, f);
            psk_set_hidden(l->finding, f[0] == '\0');
        }
        {
            char f[PSK_DETAILS * (PPC_FIELD_TEXT + 2)];
            size_t o = 0;
            uint32_t k;

            f[0] = '\0';
            for (k = 0; k < p->ndetails && o < sizeof(f); k++) {
                if (p->detail[k].layer == i) {
                    o += (size_t)snprintf(f + o, sizeof(f) - o, "%s%s", o ? "\n" : "", p->detail[k].text);
                }
            }
            psk_text(l->detail, f);
            psk_set_hidden(l->detail, f[0] == '\0');
        }
    }
    for (i = 0; i < PSK_FIELDS; i++) {
        struct psk_fieldrow *f = &a->field[i];

        psk_set_hidden(f->row, i >= p->nfields);
        if (i >= p->nfields) {
            continue;
        }
        psk_text(f->name, p->field[i].name);
        paint_value(f, p->field[i].text);
        psk_style_on(f->row, POS_STYLE_SELECTED, (int)i == a->insp_field, &f->selected);
    }
    paint_hex(a, p);
}
