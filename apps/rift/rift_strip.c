/*
 * The section strip. See rift_strip.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_strip.h"

#include "app.h"
#include "pos_styles.h"
#include "rift_comms.h"
#include "rift_format.h"
#include "rift_widgets.h"

#include <stdio.h>
#include <string.h>

/* The strip is navigation, and its five controls - the back slab and the four
 * section tabs - are one kind of thing, drawn as one (DS §51.3): a visible
 * face in the look of RIFT's secondary buttons (surface fill, hairline edge,
 * the slab's pressed look), RIFT_NAV_FACE_H tall, on a RIFT_NAV_ROW_H row, in
 * both orientations. The tab's word is in RIFT's button type, not the
 * caption's; the active tab keeps its accent word and underline. Before,
 * the tabs were bare caption words - landscape had a 36 px data row with a
 * 56 x 32 back slab - and they did not read as controls at all. */
#define FACE_H RIFT_NAV_FACE_H
/* The shell header's own back slab (DS §7): 72 wide, as tall as a tab. */
#define BACK_W 72
/* Inside a tab's face, either side of its word; as little as TAB_PAD_MIN
 * when the row is short of room (portrait at a larger text size), and the
 * short words below only when even that does not fit. */
#define TAB_PAD 20
#define TAB_PAD_MIN 8
/* Between two faces; each face takes half of it, and the air above and
 * below it, as extra target, so no tap on the row lands on nothing. */
#define FACE_GAP 8
#define FACE_REACH (FACE_GAP / 2)
/* As the screen's top row the back slab, like the shell header's, is
 * touched from the screen's top edge to the row's foot: the row's air above
 * and below it is (RIFT_NAV_ROW_H_TOP - FACE_H) / 2. */
#define BACK_REACH_TOP ((RIFT_NAV_ROW_H_TOP - FACE_H) / 2)
#define UNDERLINE_H 2

static const char *const section_name[RIFT_TAB_COUNT] = { "ACTIVITY", "NODES", "COMMS", "MAP",
                                                           "SYSTEM" };
static const char *const section_short[RIFT_TAB_COUNT] = { "ACT", "NODES", "COMMS", "MAP", "SYS" };

const char *rift_tab_word(int tab, int short_word)
{
    if (tab < 0 || tab >= RIFT_TAB_COUNT) {
        return "";
    }
    return short_word ? section_short[tab] : section_name[tab];
}

static int32_t word_w(lv_obj_t *label, const char *text)
{
    lv_point_t size;

    lv_text_get_size(&size, text, lv_obj_get_style_text_font(label, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    return size.x;
}

/* The tabs fitted to the row the body gives them, from the font's metrics
 * and the body's width alone - no layout is read, so this is safe inside a
 * layout pass. Their padding gives way first, down to TAB_PAD_MIN; then the
 * two long words are shortened (ACT, SYS). The row never scrolls and no
 * word is clipped, at any text size (DS §46). */
static void fit_tabs(struct rift_app *a)
{
    int32_t room;
    int32_t words = 0;
    int32_t pad;
    int short_words = 0;
    int pass;
    int i;

    if (!a->tab_label[0] || a->body_w <= 0) {
        return;
    }
    room = a->body_w - LV_MAX(RIFT_PAD, a->strip_inset_left) -
           LV_MAX(RIFT_PAD, a->strip_inset_right) - FACE_GAP * (RIFT_TAB_COUNT - 1) -
           (FACE_GAP + 12); /* the caption after the last tab: its gap and pad */
    if (a->wide) {
        room -= BACK_W + FACE_GAP;
    }
    for (pass = 0; pass < 2; pass++) {
        words = 0;
        for (i = 0; i < RIFT_TAB_COUNT; i++) {
            words += word_w(a->tab_label[i], rift_tab_word(i, short_words));
            if (a->tab_pill[i] && !lv_obj_has_flag(a->tab_pill[i], LV_OBJ_FLAG_HIDDEN)) {
                words += lv_obj_get_width(a->tab_pill[i]) + 6;
            }
        }
        if (room - words >= 2 * TAB_PAD_MIN * RIFT_TAB_COUNT) {
            break;
        }
        short_words = 1;
    }
    pad = (room - words) / (2 * RIFT_TAB_COUNT);
    pad = LV_CLAMP(TAB_PAD_MIN, pad, TAB_PAD);
    if (pad != a->tab_pad || short_words != a->tab_short) {
        a->tab_pad = pad;
        a->tab_short = short_words;
        for (i = 0; i < RIFT_TAB_COUNT; i++) {
            lv_obj_set_style_pad_hor(a->tab[i], pad, 0);
            lv_label_set_text(a->tab_label[i], rift_tab_word(i, short_words));
        }
    }
}

void rift_tabs_paint(struct rift_app *a)
{
    int i;

    fit_tabs(a);
    for (i = 0; i < RIFT_TAB_COUNT; i++) {
        int active = (i == (int)rift_tab_of(a->section));

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

    for (i = 0; i < RIFT_TAB_COUNT; i++) {
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

/* One face of the row: the secondary button's look (DS §7), FACE_H tall. */
static void face_style(lv_obj_t *face)
{
    pos_style_add(face, POS_STYLE_BUTTON_SECONDARY, 0);
    pos_style_add(face, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_height(face, FACE_H);
    lv_obj_set_ext_click_area(face, FACE_REACH);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face, LV_OBJ_FLAG_CLICKABLE);
}

void rift_tabs_build(struct rift_app *a)
{
    int i;

    a->strip = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->strip);
    pos_style_add(a->strip, POS_STYLE_DIVIDER, 0);
    lv_obj_set_width(a->strip, LV_PCT(100));
    lv_obj_set_height(a->strip, RIFT_NAV_ROW_H);
    lv_obj_set_flex_flow(a->strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(a->strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(a->strip, RIFT_PAD, 0);
    /* The divider is the row's bottom pixel; one at the top balances it, so a
     * face sits 4 px down with 4 px under it to the rule, and its reach
     * (FACE_REACH) covers the row from its top pixel to the rule. */
    lv_obj_set_style_pad_top(a->strip, 1, 0);
    lv_obj_set_style_pad_column(a->strip, FACE_GAP, 0);
    lv_obj_remove_flag(a->strip, LV_OBJ_FLAG_SCROLLABLE);

    /* The way back, in landscape, where the strip is the app's top row and
     * the shell's header with its back slab is not drawn (DS §37.2): the
     * shell's glyph on a face the size of the shell header's slab. Hidden
     * in portrait, where the shell's header has it. */
    a->back = lv_button_create(a->strip);
    lv_obj_remove_style_all(a->back);
    face_style(a->back);
    lv_obj_set_width(a->back, BACK_W);
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

    for (i = 0; i < RIFT_TAB_COUNT; i++) {
        lv_obj_t *head;

        /* A tab is a navigation control, never the 36 px row exception
         * (RIFT-DEV-1): a face as tall as the back slab, in either
         * orientation, as wide as its word and pill with TAB_PAD each side. */
        a->tab[i] = lv_obj_create(a->strip);
        lv_obj_remove_style_all(a->tab[i]);
        face_style(a->tab[i]);
        lv_obj_set_width(a->tab[i], LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(a->tab[i], TAB_PAD, 0);
        a->tab_pad = TAB_PAD;
        lv_obj_set_style_pad_ver(a->tab[i], 0, 0);
        lv_obj_set_flex_flow(a->tab[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(a->tab[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
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
        pos_style_add(a->tab_label[i], POS_STYLE_BUTTON_LABEL, 0);
        lv_label_set_text(a->tab_label[i], section_name[i]);
        a->tab_pill[i] = rift_unread_pill(head);

        /* The 2 px underline of handoff §3, in the accent, along the face's
         * foot under the word. A fill role rather than a colour set here
         * (tests/style_lint.sh). */
        a->tab_rule[i] = lv_obj_create(a->tab[i]);
        lv_obj_remove_style_all(a->tab_rule[i]);
        pos_style_add(a->tab_rule[i], POS_STYLE_BUTTON_PRIMARY, 0);
        lv_obj_set_style_radius(a->tab_rule[i], 0, 0);
        lv_obj_set_width(a->tab_rule[i], LV_PCT(100));
        lv_obj_set_height(a->tab_rule[i], UNDERLINE_H);
        /* Out of the column, so showing it never moves the word: every
         * face's word sits at the same height, active or not. */
        lv_obj_add_flag(a->tab_rule[i], LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(a->tab_rule[i], LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_obj_remove_flag(a->tab_rule[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(a->tab_rule[i], LV_OBJ_FLAG_HIDDEN);
    }

    a->cmd_hint = lv_label_create(a->strip);
    lv_obj_remove_style_all(a->cmd_hint);
    pos_style_add(a->cmd_hint, POS_STYLE_CAPTION, 0);
    lv_obj_set_flex_grow(a->cmd_hint, 1);
    lv_obj_set_style_text_align(a->cmd_hint, LV_TEXT_ALIGN_RIGHT, 0);
    /* Inside its box, not a margin: a flex-grown child is given the free
     * room without its margin taken off. */
    lv_obj_set_style_pad_left(a->cmd_hint, 12, 0);
    lv_label_set_long_mode(a->cmd_hint, LV_LABEL_LONG_CLIP);
    lv_label_set_text(a->cmd_hint, "");
}

void rift_tabs_shape(struct rift_app *a)
{
    int top = a->strip_at_top;

    /* The same faces in both shapes (DS §51.3). As the screen's top row
     * (landscape) the strip is the shell header's height, so the faces sit
     * where every other app's back slab does - 8 px down - and its ends keep
     * in from the rounded corners as the header's do (rift_app.c layout).
     * Under the shell's header (portrait) it is the 64 px row, 20 px in. */
    lv_obj_set_height(a->strip, top ? RIFT_NAV_ROW_H_TOP : RIFT_NAV_ROW_H);
    lv_obj_set_style_pad_left(a->strip, LV_MAX(RIFT_PAD, a->strip_inset_left), 0);
    lv_obj_set_style_pad_right(a->strip, LV_MAX(RIFT_PAD, a->strip_inset_right), 0);
    if (a->back) {
        lv_obj_set_ext_click_area(a->back, top ? BACK_REACH_TOP : FACE_REACH);
        if (a->wide) {
            lv_obj_remove_flag(a->back, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(a->back, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* The caption's parts, most needed first: the keys, then the counts. */
#define CAPTION_COUNTS 4
#define CAPTION_MAX 160

void rift_tabs_paint_caption(struct rift_app *a)
{
    const struct rift_model *m = &a->model;
    char hops[12];
    char counts[CAPTION_COUNTS][32];
    char text[CAPTION_COUNTS + 1][CAPTION_MAX];
    const char *candidate[CAPTION_COUNTS + 1];
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
        /* Enter writes to the selected node - unless it takes no messages. */
        keys = rift_node_can_message(rift_app_selected(a))
                   ? "\xE2\x86\x91\xE2\x86\x93 SELECT" RIFT_SEP "ENTER MESSAGE" RIFT_SEP
                   : "\xE2\x86\x91\xE2\x86\x93 SELECT" RIFT_SEP;
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
    snprintf(counts[0], sizeof(counts[0]), "%d KNOWN", m->node_count);
    snprintf(counts[1], sizeof(counts[1]), "%d NOW", active);
    snprintf(counts[2], sizeof(counts[2]), "%d FRESH", rift_model_fresh_count(m, now));
    snprintf(counts[3], sizeof(counts[3]), "MAX %s HOPS", hops);
    /* Right-aligned and clipped, a caption wider than the room it has is
     * cut at its left edge - at the keys, the part a reader needs - and at
     * a larger text size (DS §46) it is. So the counts go first, from the
     * last, and the keys only once there is nothing else left to drop. */
    for (i = CAPTION_COUNTS; i >= 0; i--) {
        size_t at = (size_t)snprintf(text[i], sizeof(text[i]), "%s", keys);
        int c;

        for (c = 0; c < i && at < sizeof(text[i]); c++) {
            at += (size_t)snprintf(text[i] + at, sizeof(text[i]) - at, "%s%s",
                                   c ? RIFT_SEP : "", counts[c]);
        }
        /* With every count dropped, the keys lose their trailing separator. */
        if (i == 0 && at >= sizeof(RIFT_SEP) - 1 &&
            strcmp(text[i] + at - (sizeof(RIFT_SEP) - 1), RIFT_SEP) == 0) {
            text[i][at - (sizeof(RIFT_SEP) - 1)] = '\0';
        }
        candidate[CAPTION_COUNTS - i] = text[i];
    }
    rift_cell_set_text_first_fit(a->cmd_hint, candidate, CAPTION_COUNTS + 1);
    lv_obj_remove_flag(a->cmd_hint, LV_OBJ_FLAG_HIDDEN);
}
