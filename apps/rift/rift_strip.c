/*
 * The section strip. See rift_strip.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_strip.h"

#include "app.h"
#include "pos_styles.h"
#include "rift_comms.h"
#include "rift_format.h"
#include "rift_widgets.h"

#include <stdio.h>

#define STRIP_H RIFT_TOUCH_H
/* Landscape (DS §37.2): the strip is the app's top row, where the shell's
 * header was, and holds a back slab; it is data-row height there. Every
 * pixel it gives up is the thread's. */
#define STRIP_H_WIDE RIFT_ROW_H
#define BACK_W 56
#define TAB_GAP 32
#define UNDERLINE_H 2

static const char *const section_name[RIFT_SEC_COUNT] = { "ACTIVITY", "NODES", "COMMS", "NET" };

void rift_tabs_paint(struct rift_app *a)
{
    int i;

    for (i = 0; i < RIFT_SEC_COUNT; i++) {
        int active = (i == (int)a->section);

        if (!a->tab[i] || !a->tab_label[i]) {
            continue;
        }
        if (active) {
            pos_style_add(a->tab_label[i], POS_STYLE_ACCENT_TEXT, 0);
            lv_obj_remove_flag(a->tab_rule[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_style(a->tab_label[i], pos_style(POS_STYLE_ACCENT_TEXT), 0);
            lv_obj_add_flag(a->tab_rule[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* The unread count rides on COMMS, which is where the design puts it
     * (handoff §3, §6): a reader on another section still sees that
     * something arrived. */
    if (a->tab_pill[RIFT_SEC_COMMS]) {
        rift_unread_pill_set(a->tab_pill[RIFT_SEC_COMMS], rift_model_unread_total(&a->model));
    }
}

static void on_tab(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    lv_obj_t *tab = lv_event_get_target_obj(e);
    int i;

    for (i = 0; i < RIFT_SEC_COUNT; i++) {
        if (a->tab[i] == tab) {
            rift_app_show_section(a, (enum rift_section)i);
            return;
        }
    }
}

/* The landscape back slab: the shell's own way home, as its header's slab
 * is in portrait. */
static void on_back(lv_event_t *e)
{
    (void)e;
    pocketos_shell_go_home();
}

void rift_tabs_build(struct rift_app *a)
{
    int i;

    a->strip = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->strip);
    pos_style_add(a->strip, POS_STYLE_DIVIDER, 0);
    lv_obj_set_width(a->strip, LV_PCT(100));
    lv_obj_set_height(a->strip, STRIP_H);
    lv_obj_set_flex_flow(a->strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(a->strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(a->strip, RIFT_PAD, 0);
    lv_obj_set_style_pad_column(a->strip, TAB_GAP, 0);
    lv_obj_remove_flag(a->strip, LV_OBJ_FLAG_SCROLLABLE);

    /* The way back, in landscape, where the strip is the app's top row and
     * the shell's header with its back slab is not drawn (DS §37.2): a
     * slab the strip's height, with the shell's own glyph. Hidden in
     * portrait, where the shell's header has it. */
    a->back = lv_button_create(a->strip);
    lv_obj_remove_style_all(a->back);
    pos_style_add(a->back, POS_STYLE_SLAB, 0);
    pos_style_add(a->back, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_size(a->back, BACK_W, STRIP_H_WIDE - 4);
    lv_obj_set_style_margin_bottom(a->back, 2, 0);
    lv_obj_add_flag(a->back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->back, on_back, LV_EVENT_CLICKED, a);
    lv_obj_add_flag(a->back, LV_OBJ_FLAG_HIDDEN);
    {
        lv_obj_t *glyph = lv_label_create(a->back);

        lv_obj_remove_style_all(glyph);
        pos_style_add(glyph, POS_STYLE_SYMBOL, 0);
        pos_style_add(glyph, POS_STYLE_ACCENT_TEXT, 0);
        lv_label_set_text(glyph, LV_SYMBOL_LEFT);
        lv_obj_center(glyph);
    }

    for (i = 0; i < RIFT_SEC_COUNT; i++) {
        lv_obj_t *head;

        /* A tab is a 56 px navigation target in portrait, not a 36 px row:
         * navigation never shares the row exception (RIFT-DEV-1). Landscape
         * gives the tabs the strip's data-row height (DS §37.2). */
        a->tab[i] = lv_obj_create(a->strip);
        lv_obj_remove_style_all(a->tab[i]);
        lv_obj_set_height(a->tab[i], STRIP_H);
        lv_obj_set_width(a->tab[i], LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(a->tab[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(a->tab[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(a->tab[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(a->tab[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(a->tab[i], on_tab, LV_EVENT_CLICKED, a);

        /* The name and its unread pill share one row, so the pill sits
         * beside the word rather than under it and the underline below
         * still spans both. */
        head = lv_obj_create(a->tab[i]);
        lv_obj_remove_style_all(head);
        lv_obj_set_width(head, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(head, 1);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(head, 6, 0);
        lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);

        a->tab_label[i] = lv_label_create(head);
        lv_obj_remove_style_all(a->tab_label[i]);
        pos_style_add(a->tab_label[i], POS_STYLE_CAPTION, 0);
        lv_label_set_text(a->tab_label[i], section_name[i]);
        a->tab_pill[i] = rift_unread_pill(head);

        /* The 2 px underline of handoff §3, in the accent. A fill role
         * rather than a colour set here (tests/style_lint.sh). */
        a->tab_rule[i] = lv_obj_create(a->tab[i]);
        lv_obj_remove_style_all(a->tab_rule[i]);
        pos_style_add(a->tab_rule[i], POS_STYLE_BUTTON_PRIMARY, 0);
        lv_obj_set_style_radius(a->tab_rule[i], 0, 0);
        lv_obj_set_width(a->tab_rule[i], LV_PCT(100));
        lv_obj_set_height(a->tab_rule[i], UNDERLINE_H);
        lv_obj_remove_flag(a->tab_rule[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(a->tab_rule[i], LV_OBJ_FLAG_HIDDEN);
    }

    a->cmd_hint = lv_label_create(a->strip);
    lv_obj_remove_style_all(a->cmd_hint);
    pos_style_add(a->cmd_hint, POS_STYLE_CAPTION, 0);
    lv_obj_set_flex_grow(a->cmd_hint, 1);
    lv_obj_set_style_text_align(a->cmd_hint, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_pad_bottom(a->cmd_hint, 18, 0);
    lv_label_set_long_mode(a->cmd_hint, LV_LABEL_LONG_CLIP);
    lv_label_set_text(a->cmd_hint, "");
}

void rift_tabs_shape(struct rift_app *a)
{
    int wide = a->wide;
    int i;

    lv_obj_set_height(a->strip, wide ? STRIP_H_WIDE : STRIP_H);
    for (i = 0; i < RIFT_SEC_COUNT; i++) {
        lv_obj_set_height(a->tab[i], wide ? STRIP_H_WIDE : STRIP_H);
    }
    if (a->back) {
        if (wide) {
            lv_obj_remove_flag(a->back, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(a->back, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (a->cmd_hint) {
        lv_obj_set_style_pad_bottom(a->cmd_hint, wide ? 8 : 18, 0);
    }
}

void rift_tabs_paint_caption(struct rift_app *a)
{
    const struct rift_model *m = &a->model;
    char hops[12];
    const char *keys = "";
    int64_t now;
    int max_hops = -1;
    int active = 0;
    int i;

    if (!a->cmd_hint) {
        return;
    }
    if (!a->wide) {
        lv_obj_add_flag(a->cmd_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (a->section == RIFT_SEC_NODES) {
        keys = "\xE2\x86\x91\xE2\x86\x93 SELECT" RIFT_SEP "ENTER MESSAGE" RIFT_SEP;
    } else if (a->section == RIFT_SEC_COMMS && !rift_app_composer_live(a)) {
        keys = "\xE2\x86\x91\xE2\x86\x93 CHOOSE" RIFT_SEP;
    }
    now = rift_app_now(a);
    for (i = 0; i < m->node_count; i++) {
        if (m->nodes[i].path_known && m->nodes[i].hops > max_hops) {
            max_hops = m->nodes[i].hops;
        }
        /* Heard within the last five minutes: the NOW of the activity
         * pulse, counted over the whole cache. */
        if (rift_pulse_of(now - m->nodes[i].heard_mono_ms, m->nodes[i].have_heard) ==
            RIFT_PULSE_NOW) {
            active++;
        }
    }
    if (max_hops < 0) {
        snprintf(hops, sizeof(hops), "%s", RIFT_UNKNOWN);
    } else {
        snprintf(hops, sizeof(hops), "%d", max_hops);
    }
    lv_label_set_text_fmt(a->cmd_hint,
                          "%s%d KNOWN" RIFT_SEP "%d NOW" RIFT_SEP "%d FRESH" RIFT_SEP
                          "MAX %s HOPS",
                          keys, m->node_count, active, rift_model_fresh_count(m, now), hops);
    lv_obj_remove_flag(a->cmd_hint, LV_OBJ_FLAG_HIDDEN);
}
