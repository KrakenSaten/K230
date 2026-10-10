/*
 * Settings, inside: the app's state and what its three files share.
 *
 *   settings_app.c    the list of categories, moving between pages, the
 *                     layout, the app's lifecycle and the small builders
 *   settings_wifi.c   Network: Wi-Fi, its network list and the join sheet
 *   settings_pages.c  every other page: Display, Appearance, Sound,
 *                     Keyboard, Power & Sleep, Time & Region (and its zone
 *                     list), Developer
 *
 * The page on show is rebuilt whenever it changes and repainted in place
 * every tick otherwise, so a scroll position survives a poll (as before).
 * Every page has exactly one box that scrolls - the page itself - and
 * nothing inside it scrolls (DS §52.2).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_SETTINGS_INTERNAL_H
#define POCKETOS_SETTINGS_INTERNAL_H

#include "app.h"
#include "pocketui.h"
#include "settings_view.h"

#include <stdbool.h>
#include <stdint.h>

#define SETTINGS_BTN_H 64
#define SETTINGS_TOGGLE_W 120
#define SETTINGS_STEP_W 96
/* The value between a stepper's - and +: wide enough for "30 min". */
#define SETTINGS_VALUE_W 112
/* wifi.networks every this many ticks while nothing is changing; every tick
 * while a scan runs. */
#define SETTINGS_LIST_POLL_TICKS 3
/* Power & Sleep asks sysd for the battery every this many ticks (seconds). */
#define SETTINGS_BATTERY_POLL_TICKS 5
/* The Design System has six themes; room for a few more. */
#define SETTINGS_THEMES_MAX 8
#define SETTINGS_ZONES_MAX 48
/* DS section 7: the gap between panels, whichever way they lie. */
#define SETTINGS_PANEL_GAP 22
/* The portrait body's width on the reference panel (568 less the 20 px side
 * padding of DS section 7). The wide shape is only chosen when each of its
 * two columns keeps at least this much, so no panel is narrower there than
 * in portrait. */
#define SETTINGS_COLUMN_W 528

/* The pages, in the order of the list of categories (PAGE_SYSTEM is the
 * System app's row, not a page of this app), then the two pages one level
 * further in. */
enum settings_page {
    PAGE_ROOT = 0,
    PAGE_DISPLAY,
    PAGE_APPEARANCE,
    PAGE_SOUND,
    PAGE_KEYBOARD,
    PAGE_POWER,
    PAGE_TIME,
    PAGE_NETWORK,
    PAGE_SYSTEM,
    PAGE_DEVELOPER,
    PAGE_ZONES, /* Time & Region > Time zone */
    PAGE_SHEET, /* Network > a network */
    PAGE_COUNT
};

/* The categories on the first page: PAGE_DISPLAY .. PAGE_DEVELOPER. */
#define SETTINGS_FIRST_CATEGORY PAGE_DISPLAY
#define SETTINGS_CATEGORIES (PAGE_DEVELOPER - PAGE_DISPLAY + 1)

/* Which of a page's two columns a panel goes in (settings_panel). A page
 * whose second column is empty has one column across the body. */
enum settings_col {
    COL_LEFT = 0,
    COL_RIGHT,
};

struct settings_widgets {
    /* the list of categories */
    lv_obj_t *summary[SETTINGS_CATEGORIES];
    /* Network */
    lv_obj_t *toggle;
    lv_obj_t *toggle_label;
    lv_obj_t *headline;
    lv_obj_t *detail;
    lv_obj_t *store_note;
    lv_obj_t *scan_btn;
    lv_obj_t *disc_btn;
    lv_obj_t *list;
    lv_obj_t *list_note;
    lv_obj_t *sheet;
    lv_obj_t *sheet_side[2]; /* the network described | the field and the actions */
    lv_obj_t *field;
    lv_obj_t *show_label;
    lv_obj_t *sheet_error;
    /* Display */
    lv_obj_t *bright_value;
    lv_obj_t *bright_down;
    lv_obj_t *bright_up;
    lv_obj_t *bright_note;
    lv_obj_t *rot_btn[SV_ROTATION_MODES];
    lv_obj_t *rot_note;
    lv_obj_t *size_btn[POS_TEXT_SIZE_COUNT];
    /* Appearance */
    lv_obj_t *theme_list;
    lv_obj_t *theme_chip[SETTINGS_THEMES_MAX];
    lv_obj_t *mode_btn[POS_MODE_COUNT];
    /* Sound */
    lv_obj_t *vol_value;
    lv_obj_t *vol_down;
    lv_obj_t *vol_up;
    lv_obj_t *mute_btn;
    lv_obj_t *vol_note;
    /* Keyboard */
    lv_obj_t *kb_state;
    lv_obj_t *light_value;
    lv_obj_t *light_down;
    lv_obj_t *light_up;
    lv_obj_t *light_note;
    /* Power & Sleep: screen off, lock */
    lv_obj_t *timer_value[2];
    lv_obj_t *timer_down[2];
    lv_obj_t *timer_up[2];
    lv_obj_t *lock_start_btn;
    lv_obj_t *battery_line[SV_BATTERY_ROWS]; /* read-only battery lines */
    /* Time & Region */
    lv_obj_t *zone_place;
    lv_obj_t *zone_detail;
    lv_obj_t *local_time;
    lv_obj_t *clock_state;
    lv_obj_t *zone_list;
    lv_obj_t *zone_chip[SETTINGS_ZONES_MAX];
    /* Developer */
    lv_obj_t *overlay_btn;
};

struct settings_app {
    lv_obj_t *root;
    lv_obj_t *frame; /* the app's own box in the body; built once */
    lv_obj_t *body;  /* the page on show, rebuilt with it; the one box that scrolls */
    lv_obj_t *cols;  /* in the body: the page's two columns, which never scroll */
    lv_obj_t *col[2];
    /* What the layout in force was chosen from, and the one place that
     * decides whether a pass is needed at all (pocketui.h). */
    struct pocketui_layout_guard layout_guard;
    bool wide;
    int32_t full_w; /* the frame's content width */
    int32_t half_w; /* a column's width when the page has two, in the wide shape */
    enum settings_page page;
    int tick;
    struct sv_wifi wifi;
    struct sv_brightness bright;
    struct sv_rotation rot;
    struct sv_battery battery; /* Power & Sleep, read-only */
    int battery_tick;
    struct sv_network sel;
    enum sv_join_kind sel_kind;
    struct settings_widgets w;
    /* what the network list was built from, so it is rebuilt only when it changes */
    char list_sig[SV_MAX_NETWORKS * 120];
    int built_list_visible;
};

/* ---- settings_app.c: builders shared by the pages ---- */
lv_obj_t *settings_hrow(lv_obj_t *parent, int height);
lv_obj_t *settings_stack(lv_obj_t *parent, int gap);
lv_obj_t *settings_wrap_label(lv_obj_t *parent, const char *text, enum pos_style_role role);
/* A button that does not take focus from a text field when tapped, with the
 * DS disabled treatment; primary is the accented one. */
lv_obj_t *settings_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user, int primary);
void settings_accent(lv_obj_t *btn, int on);
void settings_set_enabled(lv_obj_t *obj, int on);
void settings_set_tone(lv_obj_t *lb, enum sv_tone tone);
void settings_set_hidden(lv_obj_t *obj, int hidden);
/* A panel on the page with its caption (NULL for none), in one of its two
 * columns: side by side when the body is wide, one above the other (left
 * first) when it is tall. */
lv_obj_t *settings_panel(struct settings_app *a, const char *caption, enum settings_col col);
/* title, -, value, + in one flat row; the three objects are handed back. */
void settings_stepper(lv_obj_t *panel, const char *title, lv_event_cb_t down, lv_event_cb_t up, void *user,
                      lv_obj_t **dn, lv_obj_t **value, lv_obj_t **upb);
/* A row with a title on the left and an ON/OFF button on the right. */
lv_obj_t *settings_switch_row(lv_obj_t *panel, const char *title, lv_event_cb_t cb, void *user);
void settings_switch_paint(lv_obj_t *btn, int on);

/* Go to a page: it is built, shaped and painted, and the header says which. */
void settings_go(struct settings_app *a, enum settings_page page);
cJSON *settings_netd(const char *method, cJSON *params, char *err, size_t n);
void settings_poll_status(struct settings_app *a);
void settings_poll_networks(struct settings_app *a);
void settings_poll_brightness(struct settings_app *a);
void settings_poll_rotation(struct settings_app *a);
/* sysd's system.status, for the battery on Power & Sleep. */
void settings_poll_battery(struct settings_app *a);
void settings_rebuild(struct settings_app *a);
void settings_repaint(struct settings_app *a);
void settings_shape(struct settings_app *a);

/* ---- settings_wifi.c ---- */
void settings_network_build(struct settings_app *a);
void settings_network_repaint(struct settings_app *a);
void settings_sheet_build(struct settings_app *a);
void settings_sheet_shape(struct settings_app *a);
void settings_sheet_close(struct settings_app *a);
/* After a size change: the field and its caption in view again. */
void settings_sheet_after_layout(struct settings_app *a);
/* The app is going: the passphrase first, then nothing may call back. */
void settings_wifi_teardown(struct settings_app *a);

/* ---- settings_pages.c ---- */
const char *settings_page_title(enum settings_page page);
/* The one line under a category on the first page. */
void settings_summary(struct settings_app *a, enum settings_page page, char *out, size_t n);
void settings_page_build(struct settings_app *a);
void settings_page_repaint(struct settings_app *a);
/* Inside a panel, the lists that take two to a line in the wide shape. */
void settings_page_shape(struct settings_app *a);

#endif
