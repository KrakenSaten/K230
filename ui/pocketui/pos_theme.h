/*
 * PocketOS theme engine core (Design System v0.1, docs/design/POCKETOS-DS-v0.1.md).
 *
 * Pure C, no LVGL dependency, so it can be unit-tested natively. Holds the
 * Normal-mode base token tables (generated from themes.json), derives
 * the remaining tokens (§4), applies the Outdoor and Night rules (§6),
 * checks the §4 invariants and keeps the current (theme, mode) selection
 * with fallback (§8). Colour values are 0xRRGGBB.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POS_THEME_H
#define POS_THEME_H

#include <stddef.h>
#include <stdint.h>

enum pos_mode {
    POS_MODE_NORMAL = 0,
    POS_MODE_OUTDOOR,
    POS_MODE_NIGHT,
    POS_MODE_COUNT
};

/* Order matters: the first POS_THEME_BASE_COUNT entries are the base tokens
 * in the order used by the generated table; the rest are derived. */
enum pos_color_token {
    POS_COLOR_BG = 0,
    POS_COLOR_SURFACE,
    POS_COLOR_SURFACE_RAISED,
    POS_COLOR_LINE,
    POS_COLOR_TEXT_PRIMARY,
    POS_COLOR_TEXT_SECONDARY,
    POS_COLOR_ACCENT_PRIMARY,
    POS_COLOR_ACCENT_SECONDARY,
    POS_COLOR_STATUS_OK,
    POS_COLOR_STATUS_WARN,
    POS_COLOR_STATUS_ERROR,
    POS_COLOR_RADIO_RX,
    POS_COLOR_RADIO_TX,
    /* derived (§4) */
    POS_COLOR_TEXT_MUTED,
    POS_COLOR_TEXT_ON_ACCENT,
    POS_COLOR_NET_CONNECTED,
    POS_COLOR_FOCUS,
    POS_COLOR_DISABLED_FG,
    POS_COLOR_DISABLED_BG,
    POS_COLOR_COUNT
};

#define POS_THEME_BASE_COUNT 13

struct pos_theme_def {
    const char *id;     /* stable id used in persistence, e.g. "ice" */
    const char *name;   /* display name, e.g. "Ice & Ember" */
    uint32_t base[POS_THEME_BASE_COUNT];
};

struct pos_theme_tokens {
    uint32_t color[POS_COLOR_COUNT];
    int hairline_px;      /* 1, or 2 in Outdoor */
    int type_default_px;  /* 16, or 20 in Outdoor */
};

/* ---- pure helpers ---------------------------------------------------- */

/* Per-channel linear sRGB interpolation from a toward b, rounded half-up. */
uint32_t pos_mix(uint32_t a, uint32_t b, double t);
/* WCAG 2 contrast ratio (>= 1.0). */
double pos_contrast(uint32_t a, uint32_t b);
/* Parse "#rrggbb" or "rrggbb"; returns 0 on success. */
int pos_color_parse(const char *text, uint32_t *out);

/* Identity accents (DS §37): eight hues around the wheel at one lightness -
 * coral, orange, gold, green, teal, sky, violet, pink - for what is
 * somebody (a channel, a contact, a room), so the same one keeps the same
 * colour wherever it is shown. They are the package's, not a theme's: a
 * contact is the same contact whatever theme the reader chose. Every hue
 * reads at 4.5:1 or better on every theme's bg, surface and surface_raised
 * in Normal and Outdoor (tests/theme_test.c). An index past the count
 * wraps, so a hash may be passed straight in. */
#define POS_IDENTITY_COUNT 8
uint32_t pos_identity_rgb(unsigned index);
/* The same hue as a display mode shows it: Night dims and warms it as it
 * does the theme's text, Outdoor lifts it toward white as it does the
 * accents (the rule pos_styles.c applies to the package hues). */
uint32_t pos_identity_rgb_mode(unsigned index, enum pos_mode mode);

const char *pos_color_token_name(enum pos_color_token token);
const char *pos_mode_name(enum pos_mode mode);
/* Returns 0 on success, -1 for an unknown mode string. */
int pos_mode_parse(const char *text, enum pos_mode *out);

/* ---- theme tables ---------------------------------------------------- */

int pos_theme_count(void);
const struct pos_theme_def *pos_theme_at(int index);
const struct pos_theme_def *pos_theme_find(const char *id);
const char *pos_theme_fallback_id(void);

/* Resolve all tokens for (def, mode). Does not check invariants. */
void pos_theme_resolve(const struct pos_theme_def *def, enum pos_mode mode,
                       struct pos_theme_tokens *out);
/* §4 invariants, evaluated on the Normal-mode tokens of a theme.
 * Returns 0 when valid, -1 with a reason in why. */
int pos_theme_check(const struct pos_theme_def *def, char *why, size_t why_len);

/* ---- current selection ----------------------------------------------- */

/* Select theme and mode by id strings (either may be NULL to keep the
 * current value). Invalid or failing input falls back to the fallback theme
 * and Normal mode (§8) and returns -1 with a reason in why; the selection is
 * applied either way. Listeners are notified when tokens changed. */
int pos_theme_select(const char *theme_id, const char *mode_name, char *why, size_t why_len);
const struct pos_theme_tokens *pos_theme_current(void);
const struct pos_theme_def *pos_theme_current_def(void);
enum pos_mode pos_theme_current_mode(void);
uint32_t pos_theme_rgb(enum pos_color_token token);

/* ---- text size (DS §46) ------------------------------------------------ *
 *
 * One system-wide preference for how large text is drawn: Small, Medium or
 * Large. Small is the Design System's own type scale (§3) and the default;
 * Medium and Large raise the reading text by about a fifth and by about two
 * fifths. It is a property of the type roles, not a zoom: each semantic role
 * below has its own size at each step, so a heading stays a heading, and
 * display numerals and the environment's clock do not grow at all.
 *
 * The selection lives here beside the theme and the display mode and
 * notifies the same listeners, so the shared styles (pos_styles.c) refill
 * their fonts exactly as they refill their colours. Pure C: what size a role
 * takes is a table, testable without LVGL. */
enum pos_text_size {
    POS_TEXT_SIZE_SMALL = 0,
    POS_TEXT_SIZE_MEDIUM,
    POS_TEXT_SIZE_LARGE,
    POS_TEXT_SIZE_COUNT
};

#define POS_TEXT_SIZE_DEFAULT POS_TEXT_SIZE_SMALL

/* "small", "medium", "large"; "?" for anything else. */
const char *pos_text_size_name(enum pos_text_size size);
/* Returns 0 on success, -1 for anything but the three names (NULL too). */
int pos_text_size_parse(const char *text, enum pos_text_size *out);

/* The semantic type roles. Every text style in pos_styles.h draws in one of
 * these; which one is the style's business, not the caller's. */
enum pos_type_role {
    POS_TYPE_BODY = 0,    /* body text, descriptions, field text (Sans) */
    POS_TYPE_LABEL,       /* row titles, list names, labels that name a thing (Sans) */
    POS_TYPE_TITLE,       /* the app header's title, sheet and dialog titles (Sans semibold) */
    POS_TYPE_META,        /* captions, timestamps, chips, the status cluster's clock (Mono) */
    POS_TYPE_BUTTON,      /* button labels (Mono medium) */
    POS_TYPE_VALUE,       /* key/value values, readouts (Mono) */
    POS_TYPE_DISPLAY_40,  /* hero-40: already large, the same at every size (Sans semibold) */
    POS_TYPE_DISPLAY_48,  /* hero-48: likewise */
    /* The DOORS environment (DS §31): the shell's own screens over the art. */
    POS_TYPE_ENV_LABEL,   /* launcher cell names, Controls rows, the date (Sans) */
    POS_TYPE_ENV_SMALL,   /* narrow launcher cells and small environment text (Sans) */
    POS_TYPE_ENV_CAPTION, /* group names over the panels (Sans) */
    POS_TYPE_ENV_TITLE,   /* the environment's 40 px titles: the same at every size */
    POS_TYPE_COUNT
};

enum pos_type_face {
    POS_FACE_SANS = 0,
    POS_FACE_SANS_SEMIBOLD,
    POS_FACE_MONO,
    POS_FACE_MONO_MEDIUM,
    POS_FACE_COUNT
};

struct pos_type_spec {
    enum pos_type_face face;
    int px;
};

/* The face and pixel size a role draws in at a text size and display mode.
 * Small gives exactly the §3 scale. Outdoor keeps its rule (§6) as a floor:
 * body text is never under type_default (20 px) there. Out-of-range input
 * is clamped to the role's Small size in Normal. */
struct pos_type_spec pos_type_resolve(enum pos_type_role role, enum pos_text_size size,
                                      enum pos_mode mode);
const char *pos_type_role_name(enum pos_type_role role);

/* Select the text size. Listeners are notified when it changed, exactly as
 * for a theme or mode change. Returns 0, or -1 for a value out of range, in
 * which case Small is applied (the §8 fallback rule, for type). */
int pos_theme_select_text_size(enum pos_text_size size);
enum pos_text_size pos_theme_current_text_size(void);
/* A role at the current text size and display mode. */
struct pos_type_spec pos_type_current(enum pos_type_role role);

typedef void (*pos_theme_listener_t)(void *user);
/* Returns 0, or -1 when the listener table is full. */
int pos_theme_add_listener(pos_theme_listener_t cb, void *user);
void pos_theme_remove_listener(pos_theme_listener_t cb, void *user);

#endif
