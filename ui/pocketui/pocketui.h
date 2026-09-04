/*
 * PocketUI: design tokens and the few shared widgets the shell needs.
 * Direction (charter): dark UI, restrained neon accents, high readability,
 * touch-friendly. Modes (normal/outdoor/night) come later; only the token
 * table is designed to carry them.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETUI_H
#define POCKETUI_H

#include "lvgl.h"

/* Reference panel: 568x1232 portrait. Layout constants are in pixels at 1:1. */
#define POCKETUI_STATUS_BAR_H 56
#define POCKETUI_TOUCH_MIN 72     /* smallest comfortable touch target */
#define POCKETUI_PAD 20
#define POCKETUI_RADIUS 16

struct pocketui_tokens {
    lv_color_t bg;          /* screen background */
    lv_color_t surface;     /* cards, tiles */
    lv_color_t surface_hi;  /* pressed / highlighted surface */
    lv_color_t text;
    lv_color_t text_dim;
    lv_color_t accent;      /* restrained neon: cyan */
    lv_color_t accent_2;    /* secondary: violet */
    lv_color_t ok;
    lv_color_t warn;
    lv_color_t error;       /* used sparingly */
};

const struct pocketui_tokens *pocketui_tokens(void);

/* Apply background and default text styles to the active screen. */
void pocketui_init(void);
void pocketui_style_screen(lv_obj_t *screen);

/* A rounded surface container with padding. */
lv_obj_t *pocketui_card(lv_obj_t *parent);
/* Launcher tile: icon text (symbol or short text) and a label. */
lv_obj_t *pocketui_tile(lv_obj_t *parent, const char *icon, const char *label,
                        lv_event_cb_t on_click, void *user_data);
/* Key/value row inside a card: returns the value label for updates. */
lv_obj_t *pocketui_kv_row(lv_obj_t *parent, const char *key, const char *value);
/* Large accent button, full width of parent. */
lv_obj_t *pocketui_button(lv_obj_t *parent, const char *text, lv_event_cb_t on_click,
                          void *user_data);

#endif
