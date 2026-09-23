/*
 * PocketUI shared styles implementation. See pos_styles.h.
 *
 * This is the only file allowed to turn tokens into lv_color_t and to name
 * font symbols (enforced by tests/style_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pos_styles.h"

#include <string.h>

LV_FONT_DECLARE(pos_font_sans_16)
LV_FONT_DECLARE(pos_font_sans_20)
LV_FONT_DECLARE(pos_font_sans_24_semibold)
LV_FONT_DECLARE(pos_font_sans_40_semibold)
LV_FONT_DECLARE(pos_font_sans_48_semibold)
LV_FONT_DECLARE(pos_font_mono_14)
LV_FONT_DECLARE(pos_font_mono_16_medium)
LV_FONT_DECLARE(pos_font_mono_20)
LV_FONT_DECLARE(pos_font_clock_64)
LV_FONT_DECLARE(pos_font_clock_96)

#define POS_RADIUS 6
#define POS_PAD 20
#define POS_CHIP_HEIGHT 36
#define POS_FOCUS_OUTLINE 2
#define POS_FIELD_PAD 16 /* DS §17.1 */
#define POS_CARET_W 2    /* DS §17.1 */

static lv_style_t styles[POS_STYLE_COUNT];
static int initialised;
static uint32_t theme_event;

#define MAX_WATCHED 32
static lv_obj_t *watched[MAX_WATCHED];

lv_color_t pos_theme_color(enum pos_color_token token)
{
    return lv_color_hex(pos_theme_rgb(token));
}

static lv_color_t tok(enum pos_color_token token)
{
    return pos_theme_color(token);
}

static void reset(lv_style_t *s)
{
    lv_style_reset(s);
}

/* ---- the DOORS environment (DS §31) ------------------------------------ *
 *
 * The colours of the approved visual package (docs/design/brand/
 * doors-visual-pack-v1, b_ui_layout.json and the ui-layer SVGs): warm white
 * text on dark glass over the photographs. They are the art's, not a
 * theme's, so they do not follow the theme; they do follow the display
 * mode, by the same rules the theme engine uses for its own tokens.
 */
#define ENV_TEXT 0xeeeae2
#define ENV_TEXT_2 0xc3c0b9
#define ENV_PANEL 0x161c20
#define ENV_PANEL_LIT 0x2c3439
#define ENV_STROKE 0xa0a8a4
#define ENV_DIVIDER 0x929892
#define ENV_TRACK 0x777b7e

static const uint32_t env_hues[POS_HUE_COUNT] = {
    0xb5cfa5, 0xa6c4da, 0x9fbdd5, 0xdcb387, 0xb9afd4, 0xdfad85, 0xd5d7d5, 0xd7b78a, 0xe5e2d4,
};

/* A package colour as the current mode shows it: Night dims and warms it
 * exactly as it does the theme's text (pos_theme.c, apply_night); Outdoor
 * lifts it toward white like the theme's accents. */
static lv_color_t env(uint32_t rgb)
{
    enum pos_mode m = pos_theme_current_mode();

    if (m == POS_MODE_NIGHT) {
        rgb = pos_mix(pos_mix(rgb, 0x000000, 0.50), 0xffb060, 0.15);
    } else if (m == POS_MODE_OUTDOOR) {
        rgb = pos_mix(rgb, 0xffffff, 0.15);
    }
    return lv_color_hex(rgb);
}

lv_color_t pos_env_hue(enum pos_env_hue hue)
{
    return env(env_hues[(hue >= 0 && hue < POS_HUE_COUNT) ? hue : POS_HUE_APPS]);
}

static void env_text(lv_style_t *s, uint32_t rgb, const lv_font_t *font)
{
    reset(s);
    lv_style_set_text_color(s, env(rgb));
    lv_style_set_text_font(s, font);
}

static void fill_env_styles(void)
{
    enum pos_mode m = pos_theme_current_mode();
    /* Glass: the package's 56 % fill and 42 % hairline; Outdoor makes both
     * denser so text on them survives sunlight. */
    lv_opa_t panel_opa = m == POS_MODE_OUTDOOR ? 204 : 143;
    lv_opa_t stroke_opa = m == POS_MODE_OUTDOOR ? 178 : 107;
    lv_style_t *s;

    /* The scrims are baked into the backgrounds for Normal (tools/design/
     * gen_doors_ui.py); the other modes darken the photograph further at
     * draw time rather than shipping three copies of it. */
    s = &styles[POS_STYLE_ENV_BG];
    reset(s);
    lv_style_set_image_recolor(s, lv_color_hex(0x000000));
    lv_style_set_image_recolor_opa(s, m == POS_MODE_NIGHT ? 150 : m == POS_MODE_OUTDOOR ? 90 : LV_OPA_TRANSP);

    s = &styles[POS_STYLE_ENV_BAR];
    reset(s);
    lv_style_set_bg_opa(s, LV_OPA_TRANSP);
    lv_style_set_border_width(s, 0);

    env_text(&styles[POS_STYLE_ENV_TEXT], m == POS_MODE_OUTDOOR ? 0xffffff : ENV_TEXT, &pos_font_sans_20);
    env_text(&styles[POS_STYLE_ENV_TEXT_SMALL], m == POS_MODE_OUTDOOR ? 0xffffff : ENV_TEXT, &pos_font_sans_16);
    env_text(&styles[POS_STYLE_ENV_TEXT_SECONDARY], ENV_TEXT_2, &pos_font_sans_20);
    env_text(&styles[POS_STYLE_ENV_CAPTION], ENV_TEXT, &pos_font_sans_16);
    lv_style_set_text_letter_space(&styles[POS_STYLE_ENV_CAPTION], 2);
    env_text(&styles[POS_STYLE_ENV_CLOCK], m == POS_MODE_OUTDOOR ? 0xffffff : ENV_TEXT, &pos_font_clock_64);
    env_text(&styles[POS_STYLE_ENV_CLOCK_LARGE], m == POS_MODE_OUTDOOR ? 0xffffff : ENV_TEXT,
             &pos_font_clock_96);
    env_text(&styles[POS_STYLE_ENV_TITLE], m == POS_MODE_OUTDOOR ? 0xffffff : ENV_TEXT,
             &pos_font_sans_40_semibold);

    s = &styles[POS_STYLE_ENV_PANEL];
    reset(s);
    lv_style_set_bg_color(s, env(ENV_PANEL));
    lv_style_set_bg_opa(s, panel_opa);
    lv_style_set_border_color(s, env(ENV_STROKE));
    lv_style_set_border_opa(s, stroke_opa);
    lv_style_set_border_width(s, 1);
    lv_style_set_radius(s, 4);
    lv_style_set_pad_all(s, 0);

    s = &styles[POS_STYLE_ENV_PANEL_PRESSED];
    reset(s);
    lv_style_set_bg_color(s, env(ENV_PANEL_LIT));
    lv_style_set_bg_opa(s, 220);

    s = &styles[POS_STYLE_ENV_DIVIDER];
    reset(s);
    lv_style_set_bg_color(s, env(ENV_DIVIDER));
    lv_style_set_bg_opa(s, 102);
    lv_style_set_border_width(s, 0);
    lv_style_set_radius(s, 0);

    s = &styles[POS_STYLE_ENV_GLYPH];
    reset(s);
    lv_style_set_image_recolor(s, env(m == POS_MODE_OUTDOOR ? 0xffffff : ENV_TEXT));
    lv_style_set_image_recolor_opa(s, LV_OPA_COVER);

    s = &styles[POS_STYLE_ENV_SLIDER];
    reset(s);
    lv_style_set_bg_color(s, env(ENV_TRACK));
    lv_style_set_bg_opa(s, 140);
    lv_style_set_radius(s, 2);

    s = &styles[POS_STYLE_ENV_SLIDER_FILL];
    reset(s);
    lv_style_set_bg_color(s, env(ENV_TEXT));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_radius(s, 2);

    s = &styles[POS_STYLE_ENV_SLIDER_KNOB];
    reset(s);
    lv_style_set_bg_color(s, env(ENV_TEXT));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_radius(s, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(s, 8);

    /* The portal icons carry their own colours (they are art, not masks),
     * so Night has to dim them the way it dims the photograph under them,
     * or they would be the brightest thing on a dark screen. */
    s = &styles[POS_STYLE_ENV_ICON];
    reset(s);
    lv_style_set_image_recolor(s, lv_color_hex(0x000000));
    lv_style_set_image_recolor_opa(s, m == POS_MODE_NIGHT ? 110 : LV_OPA_TRANSP);

    s = &styles[POS_STYLE_ENV_DOT];
    reset(s);
    lv_style_set_bg_color(s, env(ENV_TEXT));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_radius(s, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(s, 0);
}

/* Fill every style from the current tokens. Called at init and on change. */
static void fill_styles(void)
{
    const struct pos_theme_tokens *t = pos_theme_current();
    const lv_font_t *body = t->type_default_px >= 20 ? &pos_font_sans_20 : &pos_font_sans_16;
    lv_style_t *s;

    s = &styles[POS_STYLE_SCREEN];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_BG));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_text_font(s, body);
    lv_style_set_pad_all(s, 0);

    s = &styles[POS_STYLE_STATUS_BAR];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_BG));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_border_color(s, tok(POS_COLOR_LINE));
    lv_style_set_border_width(s, t->hairline_px);
    lv_style_set_border_side(s, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_pad_hor(s, POS_PAD);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_SECONDARY));

    s = &styles[POS_STYLE_PANEL];
    reset(s);
    lv_style_set_bg_opa(s, LV_OPA_TRANSP);
    lv_style_set_border_color(s, tok(POS_COLOR_LINE));
    lv_style_set_border_width(s, t->hairline_px);
    lv_style_set_border_side(s, LV_BORDER_SIDE_FULL);
    lv_style_set_radius(s, POS_RADIUS);
    lv_style_set_pad_all(s, POS_PAD);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));

    s = &styles[POS_STYLE_SLAB];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_SURFACE));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_border_width(s, 0);
    lv_style_set_radius(s, POS_RADIUS);
    lv_style_set_shadow_width(s, 0);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));

    s = &styles[POS_STYLE_SLAB_PRESSED];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_SURFACE_RAISED));
    lv_style_set_outline_width(s, POS_FOCUS_OUTLINE);
    lv_style_set_outline_color(s, tok(POS_COLOR_FOCUS));
    lv_style_set_outline_pad(s, 0);

    s = &styles[POS_STYLE_TEXT_PRIMARY];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_text_font(s, body);

    s = &styles[POS_STYLE_TEXT_SECONDARY];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_SECONDARY));
    lv_style_set_text_font(s, body);

    s = &styles[POS_STYLE_TEXT_MUTED];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_MUTED));
    lv_style_set_text_font(s, body);

    s = &styles[POS_STYLE_CAPTION];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_SECONDARY));
    lv_style_set_text_font(s, &pos_font_mono_14);
    lv_style_set_text_letter_space(s, 1);

    s = &styles[POS_STYLE_VALUE];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_text_font(s, &pos_font_mono_20);

    s = &styles[POS_STYLE_TITLE];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_text_font(s, &pos_font_sans_24_semibold);

    s = &styles[POS_STYLE_ROW_TITLE];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_text_font(s, &pos_font_sans_20);

    s = &styles[POS_STYLE_BUTTON_LABEL];
    reset(s);
    lv_style_set_text_font(s, &pos_font_mono_16_medium);
    lv_style_set_text_letter_space(s, 2);

    s = &styles[POS_STYLE_BUTTON_PRIMARY];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_ACCENT_PRIMARY));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_ON_ACCENT));
    lv_style_set_border_width(s, 0);
    lv_style_set_radius(s, POS_RADIUS);
    lv_style_set_shadow_width(s, 0);
    lv_style_set_height(s, 64);

    s = &styles[POS_STYLE_BUTTON_PRIMARY_PRESSED];
    reset(s);
    lv_style_set_bg_color(s, lv_color_hex(pos_mix(t->color[POS_COLOR_ACCENT_PRIMARY],
                                                  t->color[POS_COLOR_TEXT_ON_ACCENT], 0.15)));
    lv_style_set_outline_width(s, POS_FOCUS_OUTLINE);
    lv_style_set_outline_color(s, tok(POS_COLOR_FOCUS));
    lv_style_set_outline_pad(s, 0);

    s = &styles[POS_STYLE_BUTTON_SECONDARY];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_SURFACE));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_border_color(s, tok(POS_COLOR_LINE));
    lv_style_set_border_width(s, t->hairline_px);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_radius(s, POS_RADIUS);
    lv_style_set_shadow_width(s, 0);
    lv_style_set_height(s, 64);

    s = &styles[POS_STYLE_BUTTON_DISABLED];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_DISABLED_BG));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_text_color(s, tok(POS_COLOR_DISABLED_FG));
    lv_style_set_border_width(s, 0);
    lv_style_set_outline_width(s, 0);
    lv_style_set_radius(s, POS_RADIUS);
    lv_style_set_shadow_width(s, 0);
    lv_style_set_height(s, 64);

    s = &styles[POS_STYLE_HERO_40];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_text_font(s, &pos_font_sans_40_semibold);
    lv_style_set_text_letter_space(s, 0); /* -1 % of 40 px rounds to 0 */

    s = &styles[POS_STYLE_HERO_48];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_text_font(s, &pos_font_sans_48_semibold);
    lv_style_set_text_letter_space(s, -1); /* -2 % of 48 px */

    s = &styles[POS_STYLE_CHIP];
    reset(s);
    lv_style_set_height(s, POS_CHIP_HEIGHT);
    lv_style_set_pad_hor(s, 12);
    {
        /* A label draws from the top of its content box, so the caption is
         * centred by the padding its own line leaves (DS §7). A chip drawn
         * in another font sets its own (the shell's radio chip, chrome.h). */
        int32_t spare = POS_CHIP_HEIGHT - lv_font_get_line_height(&pos_font_mono_14);

        lv_style_set_pad_top(s, spare > 0 ? spare / 2 : 0);
        lv_style_set_pad_bottom(s, spare > 0 ? spare - spare / 2 : 0);
    }
    lv_style_set_radius(s, POS_RADIUS);
    lv_style_set_border_width(s, 0);
    lv_style_set_text_font(s, &pos_font_mono_14);
    lv_style_set_text_letter_space(s, 1);
    lv_style_set_bg_opa(s, LV_OPA_COVER);

    s = &styles[POS_STYLE_CHIP_RX];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_RADIO_RX));
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_ON_ACCENT));

    s = &styles[POS_STYLE_CHIP_TX];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_RADIO_TX));
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_ON_ACCENT));

    s = &styles[POS_STYLE_CHIP_OFF];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_SURFACE));
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_SECONDARY));

    s = &styles[POS_STYLE_CHIP_NA];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_SURFACE));
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_MUTED));

    s = &styles[POS_STYLE_CHIP_ACTIVE];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_ACCENT_PRIMARY));
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_ON_ACCENT));

    s = &styles[POS_STYLE_ACCENT_TEXT];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_ACCENT_PRIMARY));

    s = &styles[POS_STYLE_STATUS_OK_TEXT];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_STATUS_OK));

    s = &styles[POS_STYLE_STATUS_WARN_TEXT];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_STATUS_WARN));

    s = &styles[POS_STYLE_STATUS_ERROR_TEXT];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_STATUS_ERROR));

    /* LV_SYMBOL_* glyphs come from the Montserrat symbol fonts until the DS
     * stroke icon set exists (implementation step 5+). */
    s = &styles[POS_STYLE_SYMBOL];
    reset(s);
    lv_style_set_text_font(s, &lv_font_montserrat_20);

    s = &styles[POS_STYLE_SYMBOL_LARGE];
    reset(s);
    lv_style_set_text_font(s, &lv_font_montserrat_32);

    s = &styles[POS_STYLE_DIVIDER];
    reset(s);
    lv_style_set_border_color(s, tok(POS_COLOR_SURFACE_RAISED));
    lv_style_set_border_width(s, 1); /* D3: dividers stay 1 px in every mode */
    lv_style_set_border_side(s, LV_BORDER_SIDE_BOTTOM);

    /* Selected, DS §7: the 2 px focus outline and nothing else, so it can be
     * added to a slab that keeps its own fill and can be pressed while it is
     * selected (PocketCalendar's day cells). */
    s = &styles[POS_STYLE_SELECTED];
    reset(s);
    lv_style_set_outline_width(s, POS_FOCUS_OUTLINE);
    lv_style_set_outline_color(s, tok(POS_COLOR_FOCUS));
    lv_style_set_outline_pad(s, 0);

    /* Text field, DS §17.1. Body font, so Outdoor's 20 px follows the theme
     * like every other body text. */
    s = &styles[POS_STYLE_FIELD];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_SURFACE));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_border_color(s, tok(POS_COLOR_LINE));
    lv_style_set_border_width(s, t->hairline_px);
    lv_style_set_radius(s, POS_RADIUS);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_text_font(s, body);
    lv_style_set_pad_left(s, POS_FIELD_PAD);
    lv_style_set_pad_right(s, POS_FIELD_PAD);
    lv_style_set_shadow_width(s, 0);

    /* The focused treatment is the global outline of DS §9 and nothing else:
     * a field must not grow a second, keyboard-only visual (§17.1). */
    s = &styles[POS_STYLE_FIELD_FOCUSED];
    reset(s);
    lv_style_set_outline_color(s, tok(POS_COLOR_FOCUS));
    lv_style_set_outline_width(s, POS_FOCUS_OUTLINE);
    lv_style_set_outline_opa(s, LV_OPA_COVER);
    lv_style_set_outline_pad(s, 0);

    s = &styles[POS_STYLE_FIELD_DISABLED];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_DISABLED_BG));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_border_width(s, 0);
    lv_style_set_text_color(s, tok(POS_COLOR_DISABLED_FG));

    /* DS §9 draws error outlines at 1.5 px; LVGL border widths are integers
     * and the document's other 1.5 px rules round up the same way. */
    s = &styles[POS_STYLE_FIELD_ERROR];
    reset(s);
    lv_style_set_border_color(s, tok(POS_COLOR_STATUS_ERROR));
    lv_style_set_border_width(s, 2);

    s = &styles[POS_STYLE_FIELD_PLACEHOLDER];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_SECONDARY));
    lv_style_set_text_font(s, body);

    s = &styles[POS_STYLE_FIELD_CURSOR];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_ACCENT_PRIMARY));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_border_width(s, 0);
    lv_style_set_width(s, POS_CARET_W);

    /* Touch keyboard, DS §17.3. */
    s = &styles[POS_STYLE_KB_SHEET];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_BG));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_border_color(s, tok(POS_COLOR_LINE));
    lv_style_set_border_width(s, t->hairline_px);
    lv_style_set_border_side(s, LV_BORDER_SIDE_TOP);
    lv_style_set_radius(s, 0);
    lv_style_set_shadow_width(s, 0);

    /* Shift engaged, and the Done key. §1's list of permitted bright fills
     * was extended for exactly these two by Amendment A. */
    s = &styles[POS_STYLE_KEY_ENGAGED];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_ACCENT_PRIMARY));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_ON_ACCENT));

    /* Shift locked: the underline is what separates locked from one-shot
     * without asking the eye to compare two fills (DS §17.3). */
    s = &styles[POS_STYLE_KEY_LOCKED];
    reset(s);
    lv_style_set_border_color(s, tok(POS_COLOR_TEXT_ON_ACCENT));
    lv_style_set_border_width(s, 2);
    lv_style_set_border_side(s, LV_BORDER_SIDE_BOTTOM);

    /* Brand mark, DS §19. The mark is an A8 mask with no colour of its own:
     * LVGL draws an A8 image in its recolour, so the token lives here like
     * any text colour and a theme or mode change repaints it. */
    s = &styles[POS_STYLE_BRAND_MARK];
    reset(s);
    lv_style_set_image_recolor(s, tok(POS_COLOR_ACCENT_PRIMARY));
    lv_style_set_image_recolor_opa(s, LV_OPA_COVER);

    /* App icons, DS §20: the same treatment as the mark, in their own role so
     * the two rules can change apart. Same accent the text icons had
     * (POS_STYLE_ACCENT_TEXT), so a tile's icon keeps its colour. */
    s = &styles[POS_STYLE_APP_ICON];
    reset(s);
    lv_style_set_image_recolor(s, tok(POS_COLOR_ACCENT_PRIMARY));
    lv_style_set_image_recolor_opa(s, LV_OPA_COVER);

    fill_env_styles();
}

static void on_theme_changed(void *user)
{
    (void)user;
    if (!initialised) {
        return;
    }
    fill_styles();
    lv_obj_report_style_change(NULL);
    for (int i = 0; i < MAX_WATCHED; i++) {
        if (watched[i]) {
            lv_obj_invalidate(watched[i]);
        }
    }
    if (lv_screen_active()) {
        lv_obj_send_event(lv_screen_active(), (lv_event_code_t)theme_event, NULL);
    }
}

void pos_styles_init(void)
{
    int i;

    if (initialised) {
        return;
    }
    for (i = 0; i < POS_STYLE_COUNT; i++) {
        lv_style_init(&styles[i]);
    }
    theme_event = lv_event_register_id();
    fill_styles();
    initialised = 1;
    pos_theme_add_listener(on_theme_changed, NULL);
}

lv_style_t *pos_style(enum pos_style_role role)
{
    return (role >= 0 && role < POS_STYLE_COUNT) ? &styles[role] : &styles[POS_STYLE_SCREEN];
}

void pos_style_add(lv_obj_t *obj, enum pos_style_role role, lv_style_selector_t selector)
{
    lv_obj_add_style(obj, pos_style(role), selector);
}

int pos_theme_apply(const char *theme_id, const char *mode_name, char *why, size_t why_len)
{
    return pos_theme_select(theme_id, mode_name, why, why_len);
}

uint32_t pos_event_theme_changed(void)
{
    return theme_event;
}

static void on_watched_deleted(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    int i;

    for (i = 0; i < MAX_WATCHED; i++) {
        if (watched[i] == obj) {
            watched[i] = NULL;
        }
    }
}

int pos_theme_watch(lv_obj_t *obj)
{
    int i;

    for (i = 0; i < MAX_WATCHED; i++) {
        if (watched[i] == obj) {
            return 0;
        }
    }
    for (i = 0; i < MAX_WATCHED; i++) {
        if (!watched[i]) {
            watched[i] = obj;
            lv_obj_add_event_cb(obj, on_watched_deleted, LV_EVENT_DELETE, NULL);
            return 0;
        }
    }
    return -1;
}
