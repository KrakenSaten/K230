/*
 * PocketOS theme engine core. See pos_theme.h and POCKETOS-DS-v0.1 §4, §6, §8.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pos_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pos_theme_table.h"

#define MAX_LISTENERS 8

static const char *const token_names[POS_COLOR_COUNT] = {
    "bg", "surface", "surface_raised", "line", "text_primary", "text_secondary",
    "accent_primary", "accent_secondary", "status_ok", "status_warn",
    "status_error", "radio_rx", "radio_tx",
    "text_muted", "text_on_accent", "net_connected", "focus", "disabled_fg",
    "disabled_bg",
};

static const char *const mode_names[POS_MODE_COUNT] = { "normal", "outdoor", "night" };

/* ---- helpers ---------------------------------------------------------- */

static int channel_mix(int a, int b, double t)
{
    double v = a + (b - a) * t;

    return (int)floor(v + 0.5);
}

uint32_t pos_mix(uint32_t a, uint32_t b, double t)
{
    int r = channel_mix((a >> 16) & 0xff, (b >> 16) & 0xff, t);
    int g = channel_mix((a >> 8) & 0xff, (b >> 8) & 0xff, t);
    int bl = channel_mix(a & 0xff, b & 0xff, t);

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static double linear(int c8)
{
    double c = c8 / 255.0;

    return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t rgb)
{
    return 0.2126 * linear((rgb >> 16) & 0xff) + 0.7152 * linear((rgb >> 8) & 0xff) +
           0.0722 * linear(rgb & 0xff);
}

double pos_contrast(uint32_t a, uint32_t b)
{
    double la = luminance(a);
    double lb = luminance(b);
    double hi = la > lb ? la : lb;
    double lo = la > lb ? lb : la;

    return (hi + 0.05) / (lo + 0.05);
}

int pos_color_parse(const char *text, uint32_t *out)
{
    unsigned int v;
    size_t n;

    if (!text) {
        return -1;
    }
    if (*text == '#') {
        text++;
    }
    n = strlen(text);
    if (n != 6 || strspn(text, "0123456789abcdefABCDEF") != 6) {
        return -1;
    }
    if (sscanf(text, "%6x", &v) != 1) {
        return -1;
    }
    *out = v;
    return 0;
}

const char *pos_color_token_name(enum pos_color_token token)
{
    return (token >= 0 && token < POS_COLOR_COUNT) ? token_names[token] : "?";
}

const char *pos_mode_name(enum pos_mode mode)
{
    return (mode >= 0 && mode < POS_MODE_COUNT) ? mode_names[mode] : "?";
}

int pos_mode_parse(const char *text, enum pos_mode *out)
{
    int i;

    if (!text) {
        return -1;
    }
    for (i = 0; i < POS_MODE_COUNT; i++) {
        if (strcmp(text, mode_names[i]) == 0) {
            *out = (enum pos_mode)i;
            return 0;
        }
    }
    return -1;
}

/* ---- tables ----------------------------------------------------------- */

int pos_theme_count(void)
{
    return POS_THEME_TABLE_COUNT;
}

const struct pos_theme_def *pos_theme_at(int index)
{
    return (index >= 0 && index < POS_THEME_TABLE_COUNT) ? &pos_theme_table[index] : NULL;
}

const struct pos_theme_def *pos_theme_find(const char *id)
{
    int i;

    if (!id) {
        return NULL;
    }
    for (i = 0; i < POS_THEME_TABLE_COUNT; i++) {
        if (strcmp(pos_theme_table[i].id, id) == 0) {
            return &pos_theme_table[i];
        }
    }
    return NULL;
}

const char *pos_theme_fallback_id(void)
{
    return POS_THEME_FALLBACK_ID;
}

/* ---- resolution (§4 derived tokens, §6 mode rules) -------------------- */

static void derive(struct pos_theme_tokens *t)
{
    uint32_t *c = t->color;

    c[POS_COLOR_TEXT_MUTED] = pos_mix(c[POS_COLOR_TEXT_SECONDARY], c[POS_COLOR_BG], 0.20); /* DS §46.7: 4.5:1 */
    c[POS_COLOR_TEXT_ON_ACCENT] = c[POS_COLOR_BG];
    c[POS_COLOR_NET_CONNECTED] = c[POS_COLOR_STATUS_OK];
    c[POS_COLOR_FOCUS] = c[POS_COLOR_ACCENT_PRIMARY];
    c[POS_COLOR_DISABLED_FG] = pos_mix(c[POS_COLOR_TEXT_SECONDARY], c[POS_COLOR_BG], 0.50);
    c[POS_COLOR_DISABLED_BG] = c[POS_COLOR_SURFACE];
}

static int is_accent_status_radio(int token)
{
    return token >= POS_COLOR_ACCENT_PRIMARY && token <= POS_COLOR_RADIO_TX;
}

static void apply_outdoor(struct pos_theme_tokens *t)
{
    uint32_t *c = t->color;
    uint32_t text_secondary = c[POS_COLOR_TEXT_SECONDARY];
    int i;

    c[POS_COLOR_BG] = 0x000000;
    c[POS_COLOR_SURFACE] = pos_mix(c[POS_COLOR_SURFACE], 0x000000, 0.50);
    /* surface_raised unchanged */
    c[POS_COLOR_LINE] = pos_mix(c[POS_COLOR_LINE], text_secondary, 0.45);
    c[POS_COLOR_TEXT_PRIMARY] = 0xffffff;
    c[POS_COLOR_TEXT_SECONDARY] = pos_mix(text_secondary, 0xffffff, 0.40);
    for (i = 0; i < POS_THEME_BASE_COUNT; i++) {
        if (is_accent_status_radio(i)) {
            c[i] = pos_mix(c[i], 0xffffff, 0.15);
        }
    }
    t->hairline_px = 2;
    t->type_default_px = 20;
}

static void apply_night(struct pos_theme_tokens *t)
{
    uint32_t *c = t->color;
    int i;

    c[POS_COLOR_BG] = 0x000000;
    c[POS_COLOR_SURFACE] = pos_mix(c[POS_COLOR_SURFACE], 0x000000, 0.50);
    c[POS_COLOR_SURFACE_RAISED] = pos_mix(c[POS_COLOR_SURFACE_RAISED], 0x000000, 0.40);
    c[POS_COLOR_LINE] = pos_mix(c[POS_COLOR_LINE], 0x000000, 0.35);
    for (i = POS_COLOR_TEXT_PRIMARY; i < POS_THEME_BASE_COUNT; i++) {
        c[i] = pos_mix(pos_mix(c[i], 0x000000, 0.50), 0xffb060, 0.15);
    }
    t->hairline_px = 1;
    t->type_default_px = 16;
}

void pos_theme_resolve(const struct pos_theme_def *def, enum pos_mode mode,
                       struct pos_theme_tokens *out)
{
    int i;

    memset(out, 0, sizeof(*out));
    for (i = 0; i < POS_THEME_BASE_COUNT; i++) {
        out->color[i] = def->base[i];
    }
    out->hairline_px = 1;
    out->type_default_px = 16;
    if (mode == POS_MODE_OUTDOOR) {
        apply_outdoor(out);
    } else if (mode == POS_MODE_NIGHT) {
        apply_night(out);
    }
    derive(out);
}

int pos_theme_check(const struct pos_theme_def *def, char *why, size_t why_len)
{
    struct pos_theme_tokens t;
    const uint32_t *c = t.color;

    pos_theme_resolve(def, POS_MODE_NORMAL, &t);
    if (c[POS_COLOR_RADIO_RX] == c[POS_COLOR_RADIO_TX]) {
        snprintf(why, why_len, "radio_rx equals radio_tx");
        return -1;
    }
    if (c[POS_COLOR_STATUS_OK] == c[POS_COLOR_STATUS_WARN] ||
        c[POS_COLOR_STATUS_OK] == c[POS_COLOR_STATUS_ERROR] ||
        c[POS_COLOR_STATUS_WARN] == c[POS_COLOR_STATUS_ERROR]) {
        snprintf(why, why_len, "status colours are not pairwise different");
        return -1;
    }
    if (pos_contrast(c[POS_COLOR_TEXT_PRIMARY], c[POS_COLOR_BG]) < 7.0) {
        snprintf(why, why_len, "contrast(text_primary, bg) below 7");
        return -1;
    }
    if (pos_contrast(c[POS_COLOR_TEXT_ON_ACCENT], c[POS_COLOR_ACCENT_PRIMARY]) < 4.5 ||
        pos_contrast(c[POS_COLOR_TEXT_ON_ACCENT], c[POS_COLOR_RADIO_RX]) < 4.5 ||
        pos_contrast(c[POS_COLOR_TEXT_ON_ACCENT], c[POS_COLOR_RADIO_TX]) < 4.5) {
        snprintf(why, why_len, "contrast(text_on_accent, accent/rx/tx) below 4.5");
        return -1;
    }
    return 0;
}

/* ---- current selection ------------------------------------------------ */

static const struct pos_theme_def *cur_def;
static enum pos_mode cur_mode = POS_MODE_NORMAL;
static struct pos_theme_tokens cur_tokens;
static int cur_valid;

static struct {
    pos_theme_listener_t cb;
    void *user;
} listeners[MAX_LISTENERS];

static void ensure_current(void)
{
    if (!cur_valid) {
        cur_def = pos_theme_find(POS_THEME_FALLBACK_ID);
        if (!cur_def) {
            cur_def = &pos_theme_table[0];
        }
        cur_mode = POS_MODE_NORMAL;
        pos_theme_resolve(cur_def, cur_mode, &cur_tokens);
        cur_valid = 1;
    }
}

static void notify(void)
{
    int i;

    for (i = 0; i < MAX_LISTENERS; i++) {
        if (listeners[i].cb) {
            listeners[i].cb(listeners[i].user);
        }
    }
}

int pos_theme_select(const char *theme_id, const char *mode_name, char *why, size_t why_len)
{
    const struct pos_theme_def *def;
    enum pos_mode mode;
    struct pos_theme_tokens tokens;
    int rc = 0;

    ensure_current();
    if (why_len) {
        why[0] = '\0';
    }
    def = cur_def;
    mode = cur_mode;
    if (theme_id) {
        def = pos_theme_find(theme_id);
        if (!def) {
            snprintf(why, why_len, "unknown theme '%s', using %s", theme_id, POS_THEME_FALLBACK_ID);
            def = pos_theme_find(POS_THEME_FALLBACK_ID);
            mode = POS_MODE_NORMAL;
            rc = -1;
        } else if (pos_theme_check(def, why, why_len) < 0) {
            def = pos_theme_find(POS_THEME_FALLBACK_ID);
            mode = POS_MODE_NORMAL;
            rc = -1;
        }
    }
    if (rc == 0 && mode_name) {
        if (pos_mode_parse(mode_name, &mode) < 0) {
            snprintf(why, why_len, "unknown display mode '%s', using normal", mode_name);
            def = pos_theme_find(POS_THEME_FALLBACK_ID);
            mode = POS_MODE_NORMAL;
            rc = -1;
        }
    }
    pos_theme_resolve(def, mode, &tokens);
    if (def != cur_def || mode != cur_mode ||
        memcmp(&tokens, &cur_tokens, sizeof(tokens)) != 0) {
        cur_def = def;
        cur_mode = mode;
        cur_tokens = tokens;
        notify();
    }
    return rc;
}

const struct pos_theme_tokens *pos_theme_current(void)
{
    ensure_current();
    return &cur_tokens;
}

const struct pos_theme_def *pos_theme_current_def(void)
{
    ensure_current();
    return cur_def;
}

enum pos_mode pos_theme_current_mode(void)
{
    ensure_current();
    return cur_mode;
}

uint32_t pos_theme_rgb(enum pos_color_token token)
{
    ensure_current();
    return (token >= 0 && token < POS_COLOR_COUNT) ? cur_tokens.color[token] : 0xff00ff;
}

static enum pos_text_size cur_text_size = POS_TEXT_SIZE_DEFAULT;

int pos_theme_select_text_size(enum pos_text_size size)
{
    int rc = 0;

    if (size < 0 || size >= POS_TEXT_SIZE_COUNT) {
        size = POS_TEXT_SIZE_SMALL;
        rc = -1;
    }
    if (size != cur_text_size) {
        cur_text_size = size;
        notify();
    }
    return rc;
}

enum pos_text_size pos_theme_current_text_size(void)
{
    return cur_text_size;
}

struct pos_type_spec pos_type_current(enum pos_type_role role)
{
    return pos_type_resolve(role, cur_text_size, pos_theme_current_mode());
}

int pos_theme_add_listener(pos_theme_listener_t cb, void *user)
{
    int i;

    for (i = 0; i < MAX_LISTENERS; i++) {
        if (!listeners[i].cb) {
            listeners[i].cb = cb;
            listeners[i].user = user;
            return 0;
        }
    }
    return -1;
}

void pos_theme_remove_listener(pos_theme_listener_t cb, void *user)
{
    int i;

    for (i = 0; i < MAX_LISTENERS; i++) {
        if (listeners[i].cb == cb && listeners[i].user == user) {
            listeners[i].cb = NULL;
            listeners[i].user = NULL;
        }
    }
}

/* ---- text size and type roles (DS §46) --------------------------------- */

static const char *const text_size_names[POS_TEXT_SIZE_COUNT] = { "small", "medium", "large" };

const char *pos_text_size_name(enum pos_text_size size)
{
    return (size >= 0 && size < POS_TEXT_SIZE_COUNT) ? text_size_names[size] : "?";
}

int pos_text_size_parse(const char *text, enum pos_text_size *out)
{
    int i;

    if (!text) {
        return -1;
    }
    for (i = 0; i < POS_TEXT_SIZE_COUNT; i++) {
        if (strcmp(text, text_size_names[i]) == 0) {
            *out = (enum pos_text_size)i;
            return 0;
        }
    }
    return -1;
}

/* The type scale. The Small column is §3's, unchanged; Medium is about 1.2
 * times it and Large about 1.4, rounded to the sizes ui/pocketui/fonts
 * carries. Roles that are already display-sized keep one size: a 48 px
 * numeral is not what anybody needs larger, and growing it would only push
 * what is under it off the screen. The launcher's cell names grow less than
 * reading text because a cell is a fixed 124 px wide in portrait; DS §46
 * explains each choice. */
static const struct {
    const char *name;
    enum pos_type_face face;
    int px[POS_TEXT_SIZE_COUNT];
} type_scale[POS_TYPE_COUNT] = {
    [POS_TYPE_BODY] = { "body", POS_FACE_SANS, { 16, 19, 22 } },
    [POS_TYPE_LABEL] = { "label", POS_FACE_SANS, { 20, 24, 28 } },
    [POS_TYPE_TITLE] = { "title", POS_FACE_SANS_SEMIBOLD, { 24, 28, 32 } },
    [POS_TYPE_META] = { "meta", POS_FACE_MONO, { 14, 17, 19 } },
    [POS_TYPE_BUTTON] = { "button", POS_FACE_MONO_MEDIUM, { 16, 19, 22 } },
    [POS_TYPE_VALUE] = { "value", POS_FACE_MONO, { 20, 24, 28 } },
    [POS_TYPE_DISPLAY_40] = { "display-40", POS_FACE_SANS_SEMIBOLD, { 40, 40, 40 } },
    [POS_TYPE_DISPLAY_48] = { "display-48", POS_FACE_SANS_SEMIBOLD, { 48, 48, 48 } },
    [POS_TYPE_ENV_LABEL] = { "env-label", POS_FACE_SANS, { 20, 22, 24 } },
    [POS_TYPE_ENV_SMALL] = { "env-small", POS_FACE_SANS, { 16, 19, 22 } },
    [POS_TYPE_ENV_CAPTION] = { "env-caption", POS_FACE_SANS, { 16, 19, 22 } },
    [POS_TYPE_ENV_TITLE] = { "env-title", POS_FACE_SANS_SEMIBOLD, { 40, 40, 40 } },
};

const char *pos_type_role_name(enum pos_type_role role)
{
    return (role >= 0 && role < POS_TYPE_COUNT) ? type_scale[role].name : "?";
}

struct pos_type_spec pos_type_resolve(enum pos_type_role role, enum pos_text_size size,
                                      enum pos_mode mode)
{
    struct pos_type_spec s;

    if (role < 0 || role >= POS_TYPE_COUNT) {
        role = POS_TYPE_BODY;
    }
    if (size < 0 || size >= POS_TEXT_SIZE_COUNT) {
        size = POS_TEXT_SIZE_SMALL;
        mode = POS_MODE_NORMAL;
    }
    s.face = type_scale[role].face;
    s.px = type_scale[role].px[size];
    /* §6: Outdoor's type_default is a floor under body text, whatever the
     * text size, so Small in Outdoor is exactly what Outdoor always was. */
    if (role == POS_TYPE_BODY && mode == POS_MODE_OUTDOOR && s.px < 20) {
        s.px = 20;
    }
    return s;
}

/* ---- identity accents (DS §37) ----------------------------------------- */

static const uint32_t identity_hues[POS_IDENTITY_COUNT] = {
    0xf2917f, 0xe8ac6a, 0xd9cb6e, 0x9fd08a, 0x74d1c4, 0x86bff0, 0xb7a8f2, 0xe89dd2,
};

uint32_t pos_identity_rgb(unsigned index)
{
    return identity_hues[index % POS_IDENTITY_COUNT];
}

uint32_t pos_identity_rgb_mode(unsigned index, enum pos_mode mode)
{
    uint32_t rgb = pos_identity_rgb(index);

    if (mode == POS_MODE_NIGHT) {
        return pos_mix(pos_mix(rgb, 0x000000, 0.50), 0xffb060, 0.15);
    }
    if (mode == POS_MODE_OUTDOOR) {
        return pos_mix(rgb, 0xffffff, 0.15);
    }
    return rgb;
}
