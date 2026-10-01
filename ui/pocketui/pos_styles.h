/*
 * PocketUI shared styles: one lv_style_t per token role (DS v0.1 §8).
 *
 * Every colour and font in the shell and in apps comes from these styles.
 * On a theme or mode change the engine rewrites the style properties and
 * calls lv_obj_report_style_change(NULL), so every object that uses the
 * shared styles follows without being touched. Nothing outside this module
 * may set a colour or font on an object directly (tests/style_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POS_STYLES_H
#define POS_STYLES_H

#include "lvgl.h"
#include "pos_theme.h"

enum pos_style_role {
    POS_STYLE_SCREEN = 0,          /* bg fill, text_primary, body font */
    POS_STYLE_STATUS_CLUSTER,      /* the status capsule (DS §36): bg fill, hairline border in line, radius 6 */
    POS_STYLE_PANEL,               /* hairline border in line, radius 6, no fill, pad 20 */
    POS_STYLE_SLAB,                /* surface fill, radius 6 (tiles, back button) */
    POS_STYLE_SLAB_PRESSED,        /* surface_raised fill + 2 px focus outline */
    POS_STYLE_TEXT_PRIMARY,        /* text_primary, body font */
    POS_STYLE_TEXT_SECONDARY,      /* text_secondary, body font */
    POS_STYLE_TEXT_MUTED,          /* text_muted, body font */
    POS_STYLE_CAPTION,             /* mono 14, text_secondary, +1 px tracking */
    POS_STYLE_VALUE,               /* mono 20, text_primary */
    POS_STYLE_TITLE,               /* sans 24 semibold, text_primary */
    POS_STYLE_ROW_TITLE,           /* sans 20, text_primary */
    POS_STYLE_HERO_40,             /* sans 40 semibold, text_primary, DS hero-40 */
    POS_STYLE_HERO_48,             /* sans 48 semibold, text_primary, -1 px tracking, DS hero-48 */
    POS_STYLE_BUTTON_LABEL,        /* mono 16 medium, +2 px tracking */
    POS_STYLE_BUTTON_PRIMARY,      /* accent fill, text_on_accent, radius 6 */
    POS_STYLE_BUTTON_PRIMARY_PRESSED,
    POS_STYLE_BUTTON_SECONDARY,    /* surface fill, hairline border, text_primary */
    POS_STYLE_BUTTON_DISABLED,     /* disabled_bg fill, disabled_fg text, no outline (DS §9) */
    POS_STYLE_CHIP,                /* chip geometry: 36 px tall, pad 12, radius 6, caption font */
    POS_STYLE_CHIP_RX,             /* radio_rx fill, text_on_accent */
    POS_STYLE_CHIP_TX,             /* radio_tx fill, text_on_accent */
    POS_STYLE_CHIP_OFF,            /* surface fill, text_secondary */
    POS_STYLE_CHIP_NA,             /* surface fill, text_muted */
    /* A chip for an active, selected or running thing that is not the radio
     * (DS §4: accent_primary is "active/selected", radio_rx is the RX chip).
     * The same fill as CHIP_RX in the themes where accent and RX coincide. */
    POS_STYLE_CHIP_ACTIVE,         /* accent_primary fill, text_on_accent */
    POS_STYLE_ACCENT_TEXT,         /* accent_primary text (chevrons, tile icons) */
    POS_STYLE_STATUS_OK_TEXT,
    POS_STYLE_STATUS_WARN_TEXT,
    POS_STYLE_STATUS_ERROR_TEXT,
    POS_STYLE_SYMBOL,              /* LV_SYMBOL_* glyph font (temporary, until DS icons) */
    POS_STYLE_SYMBOL_LARGE,        /* 32 px symbol font for launcher tiles */
    POS_STYLE_DIVIDER,             /* 1 px surface_raised bottom border (rows) */
    /* The selected outline of DS §7 - 2 px focus, and only the outline, so
     * a selected thing keeps whatever fill it already had. Separate from
     * POS_STYLE_SLAB_PRESSED, which carries the same outline but also the
     * raised fill of a finger currently on the glass: selection outlives the
     * press, and the two states have to be able to show at once. */
    POS_STYLE_SELECTED,
    /* Text field (DS §17.1). The states are separate roles so a field carries
     * only the ones it needs: error is optional per field. */
    POS_STYLE_FIELD,               /* surface fill, hairline line border, radius 6, pad 16 */
    POS_STYLE_FIELD_FOCUSED,       /* + the 2 px focus outline of DS §9 */
    POS_STYLE_FIELD_DISABLED,      /* disabled_bg fill, disabled_fg text, no border */
    POS_STYLE_FIELD_ERROR,         /* 1.5 px status_error border */
    POS_STYLE_FIELD_PLACEHOLDER,   /* text_secondary (never text_muted: DS §13 Outdoor) */
    POS_STYLE_FIELD_CURSOR,        /* 2 px accent_primary caret */
    /* Touch keyboard (DS §17.3). Key faces are slabs (POS_STYLE_SLAB and
     * POS_STYLE_SLAB_PRESSED); only the sheet and the engaged states need
     * roles of their own. */
    POS_STYLE_KB_SHEET,            /* bg fill, hairline top rule in line */
    POS_STYLE_KEY_ENGAGED,         /* accent_primary fill, text_on_accent (Shift on, Done) */
    POS_STYLE_KEY_LOCKED,          /* 2 px text_on_accent underline (Shift locked) */
    /* The Doors brand mark (DS §19): an A8 image drawn in accent_primary. */
    POS_STYLE_BRAND_MARK,
    /* App icons on launcher tiles (DS §20): A8 masks drawn in accent_primary. */
    POS_STYLE_APP_ICON,
    /* The DOORS environment (DS §31): the shell's own screens - lock,
     * launcher, quick controls - drawn over the photographic backgrounds of
     * the approved visual package. One fixed palette from that package (warm
     * white on dark glass), not the theme's: the art is the same in every
     * theme. The display mode still applies - Night dims and warms it,
     * Outdoor brightens the text and thickens the glass - so these are
     * refreshed with every other style. */
    POS_STYLE_ENV_BG,              /* background image: the mode's scrim as image recolour */
    POS_STYLE_ENV_CLUSTER,         /* the status capsule over the environment: glass, as ENV_PANEL */
    POS_STYLE_ENV_TEXT,            /* sans 20, env text */
    POS_STYLE_ENV_TEXT_SMALL,      /* sans 16, env text */
    POS_STYLE_ENV_TEXT_SECONDARY,  /* sans 20, env secondary text */
    POS_STYLE_ENV_CAPTION,         /* sans 16, env text, +2 px tracking (group names) */
    POS_STYLE_ENV_CLOCK,           /* 64 px clock digits */
    POS_STYLE_ENV_CLOCK_LARGE,     /* 96 px clock digits (lock screen) */
    POS_STYLE_ENV_TITLE,           /* sans 40 semibold, env text */
    POS_STYLE_ENV_PANEL,           /* dark glass: translucent fill, hairline, radius 4 */
    POS_STYLE_ENV_PANEL_PRESSED,   /* the same glass, lit */
    POS_STYLE_ENV_DIVIDER,         /* a 1 px rule: an object filled in the divider colour */
    POS_STYLE_ENV_GLYPH,           /* A8 glyph drawn in env text */
    POS_STYLE_ENV_SLIDER,          /* slider track */
    POS_STYLE_ENV_SLIDER_FILL,     /* slider indicator (LV_PART_INDICATOR) */
    POS_STYLE_ENV_SLIDER_KNOB,     /* slider knob (LV_PART_KNOB) */
    POS_STYLE_ENV_DOT,             /* a small "on" dot: env text fill, round */
    POS_STYLE_ENV_ICON,            /* full-colour portal icons: dimmed with the photograph in Night */
    POS_STYLE_COUNT
};

/* The DOORS package's hues (b_ui_layout.json "colors"), each app's colour on
 * the launcher. For icons' focus marks and other custom drawing; adjusted
 * for the display mode like the rest of the environment. */
enum pos_env_hue {
    POS_HUE_RADIO = 0,
    POS_HUE_MESH,
    POS_HUE_NETWORK,
    POS_HUE_TOOLS,
    POS_HUE_AI,
    POS_HUE_GAMES,
    POS_HUE_SETTINGS,
    POS_HUE_FILES,
    POS_HUE_APPS,
    POS_HUE_COUNT
};

lv_color_t pos_env_hue(enum pos_env_hue hue);

/* Identity accents (DS §37): eight hues at one lightness, for what is
 * somebody - a channel, a contact, a room - so the same one keeps the same
 * colour wherever it is shown. They are accents beside a name, never a
 * fill behind it, and never the only thing that says who: the name stays.
 * Every hue reads at 4.5:1 or better on every theme's bg, surface and
 * surface_raised in Normal and Outdoor (tests/theme_test.c), and follows
 * the display mode the way the package hues do. An index past the count
 * (POS_IDENTITY_COUNT, pos_theme.h) wraps, so a hash may be passed
 * straight in. The values are pos_theme.c's, so the audit needs no LVGL. */
lv_color_t pos_identity_hue(unsigned index);
/* A colour-only text style in that hue, to add over a role that brings the
 * font (a caption, a row title). */
lv_style_t *pos_style_identity(unsigned index);

/* ---- type (DS §3, §46) ---------------------------------------------------- *
 *
 * The font a semantic type role (pos_theme.h) draws in: exact when the
 * face carries that size, else the largest of the face below it. Every
 * role at every text size and display mode is exact (tests/text_size_test.c);
 * the fallback only keeps a future table edit from drawing nothing. */
const lv_font_t *pos_type_font(struct pos_type_spec spec);
bool pos_type_font_exact(struct pos_type_spec spec);
/* A font-only style showing what button labels look like at one text size,
 * whatever the current one is: Settings draws each of its text-size choices
 * in its own size. Add it over the button label's role. */
lv_style_t *pos_style_text_size_sample(enum pos_text_size size);
/* A font-only style: role's font as Small draws it (in the current display
 * mode), whatever the text size. Added over role on what keeps its size
 * when the rest of the screen follows the setting (DS §46.4): text lying on
 * a live picture, a web page's text, a file's contents. A role with no font
 * gives an empty style. */
lv_style_t *pos_style_fixed_size(enum pos_style_role role);

/* Initialise all styles from the current theme and hook the theme engine so
 * later selections refresh them. Call once after lv_init(). */
void pos_styles_init(void);
lv_style_t *pos_style(enum pos_style_role role);
/* Convenience: add a role style to an object for a state selector. */
void pos_style_add(lv_obj_t *obj, enum pos_style_role role, lv_style_selector_t selector);
/* Token colour as lv_color_t, for the rare draw-callback use. */
lv_color_t pos_theme_color(enum pos_color_token token);

/* Select theme and/or mode (either may be NULL), refresh every style, notify.
 * Returns 0, or -1 when the fallback was applied (why explains). */
int pos_theme_apply(const char *theme_id, const char *mode_name, char *why, size_t why_len);
/* Custom LVGL event sent to the active screen after a theme change. */
uint32_t pos_event_theme_changed(void);

/* Custom-draw widgets: objects that read pos_theme_color() inside a draw
 * event are NOT repainted by lv_obj_report_style_change (it only knows
 * shared styles). Register such an object here and the engine invalidates
 * it on every theme or mode change; the registration is removed when the
 * object is deleted. Returns 0, or -1 when the table (32) is full, in which
 * case the caller must subscribe to pos_event_theme_changed() itself. */
int pos_theme_watch(lv_obj_t *obj);

/* Sanctioned foreground/background pairs (all >= 4.5 in Normal and Outdoor,
 * checked by tests/theme_test.c): text_primary and text_secondary on bg,
 * surface and surface_raised; status_* and accent_primary on bg and
 * surface; text_on_accent on accent_primary, radio_rx and radio_tx. Chip
 * and button fills take text_on_accent, never text_primary. disabled_fg on
 * disabled_bg is intentionally low contrast and exempt. */

#endif
