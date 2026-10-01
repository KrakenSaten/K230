/*
 * Text size (DS §46), the LVGL half, and the layout audit it is checked with.
 *
 * 1. Styles. At Small every shared style draws in exactly the font it drew
 *    in before text sizes existed (named here, which only pos_styles.c may
 *    do in the product). Every semantic role at every size and display mode
 *    has a font of its own size - none falls back to a smaller one. A size
 *    change refills the styles at once: labels already on the screen grow,
 *    the screen hears the theme-changed event, and Small again gives back
 *    exactly the heights there were. The chip keeps its text inside its
 *    36 px at every size. Settings' samples show each size's button font.
 *
 * 2. The audit (pocketui_audit.h). Each kind it reports is provoked on a
 *    screen built for it - text too long for its box, a shortened label, a
 *    control past an edge nothing scrolls, two labels over each other, a label
 *    with no size - and must be found exactly there; a clean screen, a
 *    control that a scroller can bring into view, and a label lying on the
 *    button it belongs to must report nothing.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/text_size_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketui_audit.h"
#include "pos_styles.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(pos_font_sans_16)
LV_FONT_DECLARE(pos_font_sans_20)
LV_FONT_DECLARE(pos_font_sans_24_semibold)
LV_FONT_DECLARE(pos_font_sans_40_semibold)
LV_FONT_DECLARE(pos_font_sans_48_semibold)
LV_FONT_DECLARE(pos_font_mono_14)
LV_FONT_DECLARE(pos_font_mono_16_medium)
LV_FONT_DECLARE(pos_font_mono_20)

#define PANEL_W 568
#define PANEL_H 1232

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static uint8_t draw_buf[PANEL_W * 40 * 2];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static const lv_font_t *font_of(enum pos_style_role role)
{
    lv_style_value_t v;

    if (lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_FONT, &v) != LV_STYLE_RES_FOUND) {
        return NULL;
    }
    return v.ptr;
}

static int theme_events;

static void on_theme_event(lv_event_t *e)
{
    (void)e;
    theme_events++;
}

/* ---- 1. styles ----------------------------------------------------------- */

static void test_styles(lv_obj_t *screen)
{
    static const struct {
        enum pos_style_role role;
        const lv_font_t *font;
    } legacy[] = {
        { POS_STYLE_SCREEN, &pos_font_sans_16 },        { POS_STYLE_TEXT_PRIMARY, &pos_font_sans_16 },
        { POS_STYLE_TEXT_SECONDARY, &pos_font_sans_16 }, { POS_STYLE_TEXT_MUTED, &pos_font_sans_16 },
        { POS_STYLE_CAPTION, &pos_font_mono_14 },       { POS_STYLE_VALUE, &pos_font_mono_20 },
        { POS_STYLE_TITLE, &pos_font_sans_24_semibold }, { POS_STYLE_ROW_TITLE, &pos_font_sans_20 },
        { POS_STYLE_HERO_40, &pos_font_sans_40_semibold }, { POS_STYLE_HERO_48, &pos_font_sans_48_semibold },
        { POS_STYLE_BUTTON_LABEL, &pos_font_mono_16_medium }, { POS_STYLE_CHIP, &pos_font_mono_14 },
        { POS_STYLE_FIELD, &pos_font_sans_16 },         { POS_STYLE_FIELD_PLACEHOLDER, &pos_font_sans_16 },
        { POS_STYLE_ENV_TEXT, &pos_font_sans_20 },      { POS_STYLE_ENV_TEXT_SMALL, &pos_font_sans_16 },
        { POS_STYLE_ENV_TEXT_SECONDARY, &pos_font_sans_20 }, { POS_STYLE_ENV_CAPTION, &pos_font_sans_16 },
        { POS_STYLE_ENV_TITLE, &pos_font_sans_40_semibold },
    };
    static const enum pos_style_role grows[] = { POS_STYLE_TEXT_PRIMARY, POS_STYLE_CAPTION, POS_STYLE_VALUE,
                                                 POS_STYLE_TITLE, POS_STYLE_ROW_TITLE, POS_STYLE_BUTTON_LABEL,
                                                 POS_STYLE_ENV_TEXT, POS_STYLE_ENV_CAPTION };
    lv_obj_t *label[sizeof(grows) / sizeof(grows[0])];
    int32_t h_small[sizeof(grows) / sizeof(grows[0])];
    char what[160];
    size_t k;
    int z;
    int m;
    int r;
    char why[64];
    lv_style_value_t fixed;

    check("the shell starts at Small", pos_theme_current_text_size() == POS_TEXT_SIZE_SMALL);
    for (k = 0; k < sizeof(legacy) / sizeof(legacy[0]); k++) {
        snprintf(what, sizeof(what), "small: style role %d draws in the font it always did", (int)legacy[k].role);
        check(what, font_of(legacy[k].role) == legacy[k].font);
    }
    pos_theme_apply(NULL, "outdoor", why, sizeof(why));
    check("small, outdoor: body text is the 20 px it always was",
          font_of(POS_STYLE_TEXT_PRIMARY) == &pos_font_sans_20 && font_of(POS_STYLE_CAPTION) == &pos_font_mono_14);
    pos_theme_apply(NULL, "normal", why, sizeof(why));

    for (z = 0; z < POS_TEXT_SIZE_COUNT; z++) {
        for (m = 0; m < POS_MODE_COUNT; m++) {
            for (r = 0; r < POS_TYPE_COUNT; r++) {
                struct pos_type_spec s = pos_type_resolve((enum pos_type_role)r, (enum pos_text_size)z,
                                                          (enum pos_mode)m);

                snprintf(what, sizeof(what), "%s %s %s: a font of exactly %d px",
                         pos_text_size_name((enum pos_text_size)z), pos_mode_name((enum pos_mode)m),
                         pos_type_role_name((enum pos_type_role)r), s.px);
                check(what, pos_type_font_exact(s) && pos_type_font(s) &&
                                lv_font_get_line_height(pos_type_font(s)) >= s.px);
            }
        }
    }

    /* Live: labels on the screen, then the size changes under them. */
    lv_obj_add_event_cb(screen, on_theme_event, (lv_event_code_t)pos_event_theme_changed(), NULL);
    for (k = 0; k < sizeof(grows) / sizeof(grows[0]); k++) {
        label[k] = lv_label_create(screen);
        lv_obj_remove_style_all(label[k]);
        pos_style_add(label[k], grows[k], 0);
        lv_label_set_text(label[k], "Text size Ag");
    }
    lv_obj_update_layout(screen);
    for (k = 0; k < sizeof(grows) / sizeof(grows[0]); k++) {
        h_small[k] = lv_obj_get_height(label[k]);
    }
    theme_events = 0;
    for (z = POS_TEXT_SIZE_MEDIUM; z < POS_TEXT_SIZE_COUNT; z++) {
        pos_theme_select_text_size((enum pos_text_size)z);
        lv_obj_update_layout(screen);
        for (k = 0; k < sizeof(grows) / sizeof(grows[0]); k++) {
            snprintf(what, sizeof(what), "%s: a label already on screen in role %d grew (%d -> %d)",
                     pos_text_size_name((enum pos_text_size)z), (int)grows[k], (int)h_small[k],
                     (int)lv_obj_get_height(label[k]));
            check(what, lv_obj_get_height(label[k]) > h_small[k]);
        }
        snprintf(what, sizeof(what), "%s: the chip's caption fits its 36 px", pos_text_size_name((enum pos_text_size)z));
        {
            lv_style_value_t t, b;

            lv_style_get_prop(pos_style(POS_STYLE_CHIP), LV_STYLE_PAD_TOP, &t);
            lv_style_get_prop(pos_style(POS_STYLE_CHIP), LV_STYLE_PAD_BOTTOM, &b);
            check(what, t.num >= 0 && b.num >= 0 &&
                            t.num + b.num + lv_font_get_line_height(font_of(POS_STYLE_CHIP)) == 36);
        }
    }
    check("the screen heard each change once", theme_events == 2);
    {
        /* pos_style_hold_small: a subtree keeps Small's type at Large, but
         * for the part it is told to leave alone (DS §46.4, Fleet). */
        lv_obj_t *box = lv_obj_create(screen);
        lv_obj_t *held = lv_label_create(box);
        lv_obj_t *kept = lv_obj_create(box);
        lv_obj_t *free_label = lv_label_create(kept);
        lv_obj_t *title = lv_label_create(box);

        pos_style_add(held, POS_STYLE_TEXT_PRIMARY, 0);
        pos_style_add(free_label, POS_STYLE_TEXT_PRIMARY, 0);
        pos_style_add(title, POS_STYLE_TITLE, 0);
        pos_style_hold_small(box, kept);
        check("held at Large, body text draws in Small's 16 px face",
              lv_obj_get_style_text_font(held, LV_PART_MAIN) == &pos_font_sans_16);
        check("and a title in Small's 24 px semibold",
              lv_obj_get_style_text_font(title, LV_PART_MAIN) == &pos_font_sans_24_semibold);
        check("what it was told to leave follows the size",
              lv_obj_get_style_text_font(free_label, LV_PART_MAIN) == font_of(POS_STYLE_TEXT_PRIMARY) &&
                  font_of(POS_STYLE_TEXT_PRIMARY) != &pos_font_sans_16);
        /* A role put back afterwards (a refresh) wins until the hold runs again. */
        pos_style_add(held, POS_STYLE_TEXT_PRIMARY, 0);
        check("a role put back on top draws large again",
              lv_obj_get_style_text_font(held, LV_PART_MAIN) != &pos_font_sans_16);
        pos_style_hold_small(box, kept);
        pos_style_hold_small(box, kept);
        check("and the hold, run again (twice), takes it back to Small",
              lv_obj_get_style_text_font(held, LV_PART_MAIN) == &pos_font_sans_16);
        lv_obj_delete(box);
    }
    check("at Large, text over a live picture is still the Small caption face",
          lv_style_get_prop(pos_style_fixed_size(POS_STYLE_CAPTION), LV_STYLE_TEXT_FONT, &fixed) ==
                  LV_STYLE_RES_FOUND && fixed.ptr == &pos_font_mono_14);
    check("and a page's body text is the Small body face",
          lv_style_get_prop(pos_style_fixed_size(POS_STYLE_TEXT_PRIMARY), LV_STYLE_TEXT_FONT, &fixed) ==
                  LV_STYLE_RES_FOUND && fixed.ptr == &pos_font_sans_16);
    check("a role with no font gives a style with none",
          lv_style_get_prop(pos_style_fixed_size(POS_STYLE_DIVIDER), LV_STYLE_TEXT_FONT, &fixed) !=
              LV_STYLE_RES_FOUND);
    pos_theme_select_text_size(POS_TEXT_SIZE_SMALL);
    lv_obj_update_layout(screen);
    for (k = 0; k < sizeof(grows) / sizeof(grows[0]); k++) {
        snprintf(what, sizeof(what), "back at Small, role %d is exactly as tall as before", (int)grows[k]);
        check(what, lv_obj_get_height(label[k]) == h_small[k]);
    }
    for (k = 0; k < sizeof(legacy) / sizeof(legacy[0]); k++) {
        snprintf(what, sizeof(what), "back at Small, style role %d has its old font again", (int)legacy[k].role);
        check(what, font_of(legacy[k].role) == legacy[k].font);
    }
    for (z = 0; z < POS_TEXT_SIZE_COUNT; z++) {
        lv_style_value_t v;

        snprintf(what, sizeof(what), "the %s sample is the %s button font, whatever the current size",
                 pos_text_size_name((enum pos_text_size)z), pos_text_size_name((enum pos_text_size)z));
        check(what, lv_style_get_prop(pos_style_text_size_sample((enum pos_text_size)z), LV_STYLE_TEXT_FONT, &v) ==
                            LV_STYLE_RES_FOUND &&
                        v.ptr == pos_type_font(pos_type_resolve(POS_TYPE_BUTTON, (enum pos_text_size)z,
                                                                POS_MODE_NORMAL)));
    }
    for (k = 0; k < sizeof(grows) / sizeof(grows[0]); k++) {
        lv_obj_delete(label[k]);
    }
    lv_obj_remove_event_cb(screen, on_theme_event);
}

/* ---- 2. the audit -------------------------------------------------------- */

struct found {
    int n[POCKETUI_AUDIT_KIND_COUNT];
    char last_text[POCKETUI_AUDIT_TEXT_MAX];
};

static void collect(const struct pocketui_audit_issue *is, void *user)
{
    struct found *f = user;

    f->n[is->kind]++;
    snprintf(f->last_text, sizeof(f->last_text), "%s", is->text);
}

static void noop(lv_event_t *e)
{
    (void)e;
}

static lv_obj_t *fresh_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);

    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, PANEL_W, PANEL_H);
    pos_style_add(s, POS_STYLE_SCREEN, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(s);
    return s;
}

static lv_obj_t *box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *label_at(lv_obj_t *parent, const char *text, int32_t x, int32_t y)
{
    lv_obj_t *l = lv_label_create(parent);

    pos_style_add(l, POS_STYLE_TEXT_PRIMARY, 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

static struct found audit(lv_obj_t *s)
{
    struct found f;
    struct pocketui_audit_stats st;

    memset(&f, 0, sizeof(f));
    lv_obj_update_layout(s);
    pocketui_audit(s, collect, &f, &st);
    return f;
}

static int total(const struct found *f)
{
    return f->n[0] + f->n[1] + f->n[2] + f->n[3];
}

static void test_audit(void)
{
    lv_obj_t *s;
    lv_obj_t *o;
    lv_obj_t *b;
    struct found f;

    /* A clean screen: labels side by side, a button with its label. */
    s = fresh_screen();
    label_at(s, "One", 20, 20);
    label_at(s, "Two", 120, 20);
    b = box(s, 20, 80, 200, 64);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, noop, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label_at(b, "OK", 0, 0));
    f = audit(s);
    check("audit: a clean screen reports nothing", total(&f) == 0);

    /* Wrapped text in a box too short for it. */
    s = fresh_screen();
    o = label_at(s, "A sentence long enough to need three lines in this box", 20, 20);
    lv_obj_set_size(o, 160, 22);
    f = audit(s);
    check("audit: wrapped text in a short box is clipped", f.n[POCKETUI_AUDIT_CLIPPED] == 1 && total(&f) == 1);
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    f = audit(s);
    check("audit: and nothing once the box takes its content's height", total(&f) == 0);

    /* The dots mode shortening a label. */
    s = fresh_screen();
    o = label_at(s, "A name far too long for its cell", 20, 20);
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(o, 80, lv_font_get_line_height(font_of(POS_STYLE_TEXT_PRIMARY)));
    lv_refr_now(NULL);
    f = audit(s);
    check("audit: a label shortened with dots is truncated", f.n[POCKETUI_AUDIT_TRUNCATED] == 1 && total(&f) == 1);

    /* A control past the edge of a parent that does not scroll ... */
    s = fresh_screen();
    o = box(s, 0, 0, 300, 100);
    b = box(o, 250, 20, 120, 40);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, noop, LV_EVENT_CLICKED, NULL);
    f = audit(s);
    check("audit: a button past a fixed parent's edge is clipped", f.n[POCKETUI_AUDIT_CLIPPED] == 1);
    /* ... and the same in one that does. */
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(o, LV_DIR_HOR);
    f = audit(s);
    check("audit: the same button in a parent that scrolls to it is fine", f.n[POCKETUI_AUDIT_CLIPPED] == 0);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    f = audit(s);
    check("audit: but not if the parent scrolls the other way", f.n[POCKETUI_AUDIT_CLIPPED] == 1);

    /* Two pieces of text over each other. */
    s = fresh_screen();
    label_at(s, "Left words", 20, 20);
    label_at(s, "Right words", 40, 24);
    f = audit(s);
    check("audit: two labels over each other overlap", f.n[POCKETUI_AUDIT_OVERLAP] == 1 && total(&f) == 1);

    /* A control wholly under a smaller label is its surface, not a clash. */
    s = fresh_screen();
    b = box(s, 0, 0, 400, 400);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, noop, LV_EVENT_CLICKED, NULL);
    label_at(s, "On the board", 40, 40);
    f = audit(s);
    check("audit: a label lying on a larger control is not an overlap", total(&f) == 0);

    /* No size at all. */
    s = fresh_screen();
    o = label_at(s, "Nowhere", 20, 20);
    lv_obj_set_size(o, 0, 20);
    f = audit(s);
    check("audit: a label with no width is zero", f.n[POCKETUI_AUDIT_ZERO] == 1 && strcmp(f.last_text, "Nowhere") == 0);

    /* Hidden things are not on the screen. */
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    f = audit(s);
    check("audit: and nothing once it is hidden", total(&f) == 0);

    /* Larger text turns a fit into a clip: the reason the audit exists. */
    s = fresh_screen();
    o = label_at(s, "Twelve chars", 20, 20);
    lv_obj_set_size(o, LV_SIZE_CONTENT, lv_font_get_line_height(font_of(POS_STYLE_TEXT_PRIMARY)));
    f = audit(s);
    check("audit: a one-line box sized at Small fits at Small", total(&f) == 0);
    pos_theme_select_text_size(POS_TEXT_SIZE_LARGE);
    f = audit(s);
    check("audit: and is clipped once the text is Large", f.n[POCKETUI_AUDIT_CLIPPED] == 1);
    pos_theme_select_text_size(POS_TEXT_SIZE_SMALL);
}

int main(void)
{
    lv_display_t *disp;
    lv_obj_t *screen;

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    pos_styles_init();
    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, PANEL_W, PANEL_H);
    lv_screen_load(screen);

    test_styles(screen);
    test_audit();

    printf("text_size_ui_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
