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
    POS_STYLE_STATUS_BAR,          /* bg fill, bottom hairline in line */
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
    POS_STYLE_ACCENT_TEXT,         /* accent_primary text (chevrons, tile icons) */
    POS_STYLE_STATUS_OK_TEXT,
    POS_STYLE_STATUS_WARN_TEXT,
    POS_STYLE_STATUS_ERROR_TEXT,
    POS_STYLE_SYMBOL,              /* LV_SYMBOL_* glyph font (temporary, until DS icons) */
    POS_STYLE_SYMBOL_LARGE,        /* 32 px symbol font for launcher tiles */
    POS_STYLE_DIVIDER,             /* 1 px surface_raised bottom border (rows) */
    POS_STYLE_COUNT
};

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
