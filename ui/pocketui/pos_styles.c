/*
 * PocketUI shared styles implementation. See pos_styles.h.
 *
 * This is the only file allowed to turn tokens into lv_color_t and to name
 * font symbols (enforced by tests/style_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pos_styles.h"

#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(pos_font_sans_16)
LV_FONT_DECLARE(pos_font_sans_19)
LV_FONT_DECLARE(pos_font_sans_20)
LV_FONT_DECLARE(pos_font_sans_22)
LV_FONT_DECLARE(pos_font_sans_24)
LV_FONT_DECLARE(pos_font_sans_28)
LV_FONT_DECLARE(pos_font_sans_24_semibold)
LV_FONT_DECLARE(pos_font_sans_28_semibold)
LV_FONT_DECLARE(pos_font_sans_32_semibold)
LV_FONT_DECLARE(pos_font_sans_40_semibold)
LV_FONT_DECLARE(pos_font_sans_48_semibold)
LV_FONT_DECLARE(pos_font_mono_14)
LV_FONT_DECLARE(pos_font_mono_17)
LV_FONT_DECLARE(pos_font_mono_19)
LV_FONT_DECLARE(pos_font_mono_20)
LV_FONT_DECLARE(pos_font_mono_24)
LV_FONT_DECLARE(pos_font_mono_28)
LV_FONT_DECLARE(pos_font_mono_16_medium)
LV_FONT_DECLARE(pos_font_mono_19_medium)
LV_FONT_DECLARE(pos_font_mono_22_medium)
LV_FONT_DECLARE(pos_font_clock_64)
LV_FONT_DECLARE(pos_font_clock_96)

/* ---- type roles to fonts (DS §3, §46) ------------------------------------ *
 *
 * pos_theme.c says which face and size a semantic role takes at the current
 * text size; this is the one table that turns that into a bitmap font. The
 * fonts are const data in the binary, so the sizes not in use cost flash and
 * nothing else: no font is loaded or copied at run time.
 */
static const struct {
    enum pos_type_face face;
    int px;
    const lv_font_t *font;
} type_fonts[] = {
    { POS_FACE_SANS, 16, &pos_font_sans_16 },
    { POS_FACE_SANS, 19, &pos_font_sans_19 },
    { POS_FACE_SANS, 20, &pos_font_sans_20 },
    { POS_FACE_SANS, 22, &pos_font_sans_22 },
    { POS_FACE_SANS, 24, &pos_font_sans_24 },
    { POS_FACE_SANS, 28, &pos_font_sans_28 },
    { POS_FACE_SANS_SEMIBOLD, 24, &pos_font_sans_24_semibold },
    { POS_FACE_SANS_SEMIBOLD, 28, &pos_font_sans_28_semibold },
    { POS_FACE_SANS_SEMIBOLD, 32, &pos_font_sans_32_semibold },
    { POS_FACE_SANS_SEMIBOLD, 40, &pos_font_sans_40_semibold },
    { POS_FACE_SANS_SEMIBOLD, 48, &pos_font_sans_48_semibold },
    { POS_FACE_MONO, 14, &pos_font_mono_14 },
    { POS_FACE_MONO, 17, &pos_font_mono_17 },
    { POS_FACE_MONO, 19, &pos_font_mono_19 },
    { POS_FACE_MONO, 20, &pos_font_mono_20 },
    { POS_FACE_MONO, 24, &pos_font_mono_24 },
    { POS_FACE_MONO, 28, &pos_font_mono_28 },
    { POS_FACE_MONO_MEDIUM, 16, &pos_font_mono_16_medium },
    { POS_FACE_MONO_MEDIUM, 19, &pos_font_mono_19_medium },
    { POS_FACE_MONO_MEDIUM, 22, &pos_font_mono_22_medium },
};

#define TYPE_FONT_COUNT ((int)(sizeof(type_fonts) / sizeof(type_fonts[0])))

const lv_font_t *pos_type_font(struct pos_type_spec spec)
{
    const lv_font_t *best = NULL;
    int best_px = 0;
    int i;

    /* Exact, or else the largest of the face that is not larger (tests/
     * text_size_test.c proves every role at every size is exact). */
    for (i = 0; i < TYPE_FONT_COUNT; i++) {
        if (type_fonts[i].face != spec.face) {
            continue;
        }
        if (type_fonts[i].px == spec.px) {
            return type_fonts[i].font;
        }
        if (type_fonts[i].px < spec.px && type_fonts[i].px > best_px) {
            best = type_fonts[i].font;
            best_px = type_fonts[i].px;
        }
    }
    return best ? best : &pos_font_sans_16;
}

bool pos_type_font_exact(struct pos_type_spec spec)
{
    int i;

    for (i = 0; i < TYPE_FONT_COUNT; i++) {
        if (type_fonts[i].face == spec.face && type_fonts[i].px == spec.px) {
            return true;
        }
    }
    return false;
}

static const lv_font_t *role_font(enum pos_type_role role)
{
    return pos_type_font(pos_type_current(role));
}

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

/* ---- identity accents (DS §37) ------------------------------------------ *
 *
 * The hues and their mode adjustment are pos_theme.c's, where the contrast
 * audit can reach them without LVGL; this is the lv_color_t and the styles.
 */
static lv_style_t identity_styles[POS_IDENTITY_COUNT];

lv_color_t pos_identity_hue(unsigned index)
{
    return lv_color_hex(pos_identity_rgb_mode(index, pos_theme_current_mode()));
}

lv_style_t *pos_style_identity(unsigned index)
{
    return &identity_styles[index % POS_IDENTITY_COUNT];
}

static void fill_identity_styles(void)
{
    int i;

    for (i = 0; i < POS_IDENTITY_COUNT; i++) {
        reset(&identity_styles[i]);
        lv_style_set_text_color(&identity_styles[i], pos_identity_hue((unsigned)i));
    }
}

/* ---- fixed sizes (DS §46.4) --------------------------------------------- */

/* The type role each style role draws in; -1 for a style with no font. The
 * same pairs fill_styles() and fill_env_styles() use. */
static int style_type(enum pos_style_role role)
{
    switch (role) {
    case POS_STYLE_SCREEN:
    case POS_STYLE_TEXT_PRIMARY:
    case POS_STYLE_TEXT_SECONDARY:
    case POS_STYLE_TEXT_MUTED:
    case POS_STYLE_FIELD:
    case POS_STYLE_FIELD_PLACEHOLDER:
        return POS_TYPE_BODY;
    case POS_STYLE_CAPTION:
    case POS_STYLE_CHIP:
        return POS_TYPE_META;
    case POS_STYLE_VALUE:
        return POS_TYPE_VALUE;
    case POS_STYLE_TITLE:
        return POS_TYPE_TITLE;
    case POS_STYLE_ROW_TITLE:
        return POS_TYPE_LABEL;
    case POS_STYLE_BUTTON_LABEL:
        return POS_TYPE_BUTTON;
    case POS_STYLE_HERO_40:
        return POS_TYPE_DISPLAY_40;
    case POS_STYLE_HERO_48:
        return POS_TYPE_DISPLAY_48;
    case POS_STYLE_ENV_TEXT:
    case POS_STYLE_ENV_TEXT_SECONDARY:
        return POS_TYPE_ENV_LABEL;
    case POS_STYLE_ENV_TEXT_SMALL:
        return POS_TYPE_ENV_SMALL;
    case POS_STYLE_ENV_CAPTION:
        return POS_TYPE_ENV_CAPTION;
    case POS_STYLE_ENV_TITLE:
        return POS_TYPE_ENV_TITLE;
    default:
        return -1;
    }
}

static lv_style_t fixed_styles[POS_STYLE_COUNT];

lv_style_t *pos_style_fixed_size(enum pos_style_role role)
{
    return &fixed_styles[(role >= 0 && role < POS_STYLE_COUNT) ? role : POS_STYLE_SCREEN];
}

static void fill_fixed_styles(void)
{
    int i;

    for (i = 0; i < POS_STYLE_COUNT; i++) {
        int t = style_type((enum pos_style_role)i);

        reset(&fixed_styles[i]);
        if (t >= 0) {
            lv_style_set_text_font(&fixed_styles[i], pos_type_font(pos_type_resolve(
                                                         (enum pos_type_role)t, POS_TEXT_SIZE_SMALL,
                                                         pos_theme_current_mode())));
        }
    }
}

/* ---- holding a screen at Small (DS §46.4) ------------------------------ *
 *
 * For an app whose screens are laid out to fit exactly - a game board and
 * its controls on one screen - and which keeps the type Small draws at every
 * text size. The role an object draws in is recovered from the font it
 * resolves to now (every role at the current size has a font of its own
 * size, and the roles that share one share their Small size too), and a
 * font-only style holding that role's Small font is put in front of it.
 */
static lv_style_t held_styles[POS_TYPE_COUNT];

static void fill_held_styles(void)
{
    int r;

    for (r = 0; r < POS_TYPE_COUNT; r++) {
        reset(&held_styles[r]);
        lv_style_set_text_font(&held_styles[r], pos_type_font(pos_type_resolve(
                                                     (enum pos_type_role)r, POS_TEXT_SIZE_SMALL,
                                                     pos_theme_current_mode())));
    }
}

static void hold_part(lv_obj_t *obj, lv_part_t part)
{
    const lv_font_t *f = lv_obj_get_style_text_font(obj, part);
    int r;

    for (r = 0; r < POS_TYPE_COUNT; r++) {
        if (pos_type_font(pos_type_current((enum pos_type_role)r)) == f) {
            break;
        }
    }
    if (r == POS_TYPE_COUNT) {
        return; /* not a type role's font: a symbol font, a clock */
    }
    if (pos_type_font(pos_type_resolve((enum pos_type_role)r, POS_TEXT_SIZE_SMALL, pos_theme_current_mode())) ==
        f) {
        return; /* already Small's: at Small, nothing to do at all */
    }
    lv_obj_add_style(obj, &held_styles[r], part);
}

void pos_style_hold_small(lv_obj_t *root, lv_obj_t *except)
{
    uint32_t i;
    uint32_t n;

    if (!root || root == except) {
        return;
    }
    hold_part(root, LV_PART_MAIN);
    if (lv_obj_check_type(root, &lv_textarea_class)) {
        hold_part(root, LV_PART_TEXTAREA_PLACEHOLDER);
    }
    n = lv_obj_get_child_count(root);
    for (i = 0; i < n; i++) {
        pos_style_hold_small(lv_obj_get_child(root, (int32_t)i), except);
    }
}

/* ---- the text-size samples (DS §46) ------------------------------------- */

static lv_style_t size_samples[POS_TEXT_SIZE_COUNT];

lv_style_t *pos_style_text_size_sample(enum pos_text_size size)
{
    return &size_samples[(size >= 0 && size < POS_TEXT_SIZE_COUNT) ? size : POS_TEXT_SIZE_SMALL];
}

static void fill_size_samples(void)
{
    int i;

    for (i = 0; i < POS_TEXT_SIZE_COUNT; i++) {
        reset(&size_samples[i]);
        lv_style_set_text_font(&size_samples[i], pos_type_font(pos_type_resolve(
                                                     POS_TYPE_BUTTON, (enum pos_text_size)i,
                                                     pos_theme_current_mode())));
    }
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

    /* The status capsule on the photograph (DS §36.1): the launcher's glass,
     * colour only - its geometry is the app capsule's, which it overrides. */
    s = &styles[POS_STYLE_ENV_CLUSTER];
    reset(s);
    lv_style_set_bg_color(s, env(ENV_PANEL));
    lv_style_set_bg_opa(s, panel_opa);
    lv_style_set_border_color(s, env(ENV_STROKE));
    lv_style_set_border_opa(s, stroke_opa);
    lv_style_set_border_width(s, 1);

    env_text(&styles[POS_STYLE_ENV_TEXT], m == POS_MODE_OUTDOOR ? 0xffffff : ENV_TEXT,
             role_font(POS_TYPE_ENV_LABEL));
    env_text(&styles[POS_STYLE_ENV_TEXT_SMALL], m == POS_MODE_OUTDOOR ? 0xffffff : ENV_TEXT,
             role_font(POS_TYPE_ENV_SMALL));
    env_text(&styles[POS_STYLE_ENV_TEXT_SECONDARY], ENV_TEXT_2, role_font(POS_TYPE_ENV_LABEL));
    env_text(&styles[POS_STYLE_ENV_CAPTION], ENV_TEXT, role_font(POS_TYPE_ENV_CAPTION));
    lv_style_set_text_letter_space(&styles[POS_STYLE_ENV_CAPTION], 2);
    /* The clock digits are display type, the same at every text size. */
    env_text(&styles[POS_STYLE_ENV_CLOCK], m == POS_MODE_OUTDOOR ? 0xffffff : ENV_TEXT, &pos_font_clock_64);
    env_text(&styles[POS_STYLE_ENV_CLOCK_LARGE], m == POS_MODE_OUTDOOR ? 0xffffff : ENV_TEXT,
             &pos_font_clock_96);
    env_text(&styles[POS_STYLE_ENV_TITLE], m == POS_MODE_OUTDOOR ? 0xffffff : ENV_TEXT,
             role_font(POS_TYPE_ENV_TITLE));

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
    /* Body text: Outdoor's 20 px floor (§6) is part of the role (pos_theme.c). */
    const lv_font_t *body = role_font(POS_TYPE_BODY);
    const lv_font_t *meta = role_font(POS_TYPE_META);
    lv_style_t *s;

    s = &styles[POS_STYLE_SCREEN];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_BG));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_text_font(s, body);
    lv_style_set_pad_all(s, 0);

    /* The status capsule (DS §36.1): what the §7 status bar was - bg fill,
     * a hairline in line, caption text - shrunk to its content, so the
     * hairline runs round it instead of under the whole screen. Its
     * padding is the shell's (chrome.h), not a style's. */
    s = &styles[POS_STYLE_STATUS_CLUSTER];
    reset(s);
    lv_style_set_bg_color(s, tok(POS_COLOR_BG));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_border_color(s, tok(POS_COLOR_LINE));
    lv_style_set_border_opa(s, LV_OPA_COVER);
    lv_style_set_border_width(s, t->hairline_px);
    lv_style_set_radius(s, POS_RADIUS);
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
    lv_style_set_text_font(s, meta);
    lv_style_set_text_letter_space(s, 1);

    s = &styles[POS_STYLE_VALUE];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_text_font(s, role_font(POS_TYPE_VALUE));

    s = &styles[POS_STYLE_TITLE];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_text_font(s, role_font(POS_TYPE_TITLE));

    s = &styles[POS_STYLE_ROW_TITLE];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_text_font(s, role_font(POS_TYPE_LABEL));

    s = &styles[POS_STYLE_BUTTON_LABEL];
    reset(s);
    lv_style_set_text_font(s, role_font(POS_TYPE_BUTTON));
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
    lv_style_set_text_font(s, role_font(POS_TYPE_DISPLAY_40));
    lv_style_set_text_letter_space(s, 0); /* -1 % of 40 px rounds to 0 */

    s = &styles[POS_STYLE_HERO_48];
    reset(s);
    lv_style_set_text_color(s, tok(POS_COLOR_TEXT_PRIMARY));
    lv_style_set_text_font(s, role_font(POS_TYPE_DISPLAY_48));
    lv_style_set_text_letter_space(s, -1); /* -2 % of 48 px */

    s = &styles[POS_STYLE_CHIP];
    reset(s);
    lv_style_set_height(s, POS_CHIP_HEIGHT);
    lv_style_set_pad_hor(s, 12);
    {
        /* A label draws from the top of its content box, so the caption is
         * centred by the padding its own line leaves (DS §7). A chip drawn
         * in another font sets its own (the shell's radio chip, chrome.h). */
        int32_t spare = POS_CHIP_HEIGHT - lv_font_get_line_height(meta);

        lv_style_set_pad_top(s, spare > 0 ? spare / 2 : 0);
        lv_style_set_pad_bottom(s, spare > 0 ? spare - spare / 2 : 0);
    }
    lv_style_set_radius(s, POS_RADIUS);
    lv_style_set_border_width(s, 0);
    lv_style_set_text_font(s, meta);
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
    fill_identity_styles();
    fill_size_samples();
    fill_fixed_styles();
    fill_held_styles();
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
    for (i = 0; i < POS_IDENTITY_COUNT; i++) {
        lv_style_init(&identity_styles[i]);
    }
    for (i = 0; i < POS_TEXT_SIZE_COUNT; i++) {
        lv_style_init(&size_samples[i]);
    }
    for (i = 0; i < POS_STYLE_COUNT; i++) {
        lv_style_init(&fixed_styles[i]);
    }
    for (i = 0; i < POS_TYPE_COUNT; i++) {
        lv_style_init(&held_styles[i]);
    }
    theme_event = lv_event_register_id();
#if defined(POCKETUI_TEST_HOOKS) && POCKETUI_TEST_HOOKS
    {
        /* Host tests only (ui/shell/CMakeLists.txt): run a test written at
         * Small at another text size, unchanged (DS §46.6). */
        const char *ts = getenv("POCKETUI_TEST_TEXT_SIZE");
        enum pos_text_size size;

        if (ts && pos_text_size_parse(ts, &size) == 0) {
            pos_theme_select_text_size(size);
        }
    }
#endif
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
