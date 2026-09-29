/*
 * The DOORS launcher's geometry (DS §31.3), as a pure function of the
 * content area and the group sizes. No LVGL, so the host suite can hold the
 * layout to its rules in every orientation and for app sets that do not
 * exist yet.
 *
 * The launcher is a header (the time and the date) above one glass panel
 * per group, each holding its apps' portal icons, and a footer with the two
 * shell actions (Lock, Controls).
 *
 *   portrait   panels stacked, four cells across, a group wraps to more
 *              rows when it holds more than four apps
 *   landscape  panels side by side in one row, each exactly as wide as its
 *              apps, the row centred; when the row would squeeze a cell
 *              below HOME_CELL_MIN_W the panels wrap onto more lines instead
 *
 * Nothing is shortened to fit: when the whole does not fit the content area
 * the layout grows past it (content_h > height) and the launcher scrolls.
 * With today's thirteen apps portrait does not scroll; landscape wraps DEVICE
 * onto a second line and scrolls to its footer (twelve fitted one row).
 *
 * A folder (an app group, "the folders" below) is one cell on this page
 * that stands for several apps; opening it shows a page of its own, laid
 * out by home_folder_layout_compute().
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef DOORS_HOME_LAYOUT_H
#define DOORS_HOME_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#define HOME_MAX_GROUPS 8
#define HOME_MAX_APPS 48

#define HOME_ICON 96            /* the portal icon's canvas (tools/design/gen_doors_ui.py) */
#define HOME_CELL_H 128         /* icon, 4 px, a 28 px label line */
#define HOME_CELL_W_PORTRAIT 124
#define HOME_CELL_MIN_W 84      /* below this a landscape row wraps */
#define HOME_CELL_WRAP_W 104    /* cell width once it has wrapped */
#define HOME_PORTRAIT_COLUMNS 4
#define HOME_PANEL_HEAD 40      /* the group's name, then a rule */
#define HOME_PANEL_PAD 8        /* inside a panel, left, right and between rows */
#define HOME_PANEL_BOTTOM 10
#define HOME_PANEL_GAP 14
#define HOME_BUTTON_H 64        /* POCKETUI_TOUCH_MIN */

struct home_rect {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

struct home_layout_in {
    int32_t width;          /* the content area: the whole display but the keyboard (DS §36) */
    int32_t height;
    bool landscape;
    /* What the panel's rounded corners take from the content area's bottom
     * corners (pos_display_rect_insets), so the footer keeps clear of them. */
    int32_t inset_left;
    int32_t inset_right;
    int32_t inset_bottom;
    /* The status cluster's widest box, in content coordinates (DS §36): the
     * header band narrows, symmetrically so the time stays centred, until
     * it is clear of it; the date below it is lower than the cluster and
     * keeps the whole row. w == 0: no cluster. */
    struct home_rect keepout;
    uint8_t ngroups;
    uint8_t count[HOME_MAX_GROUPS]; /* apps in each group, in order; 0 = group absent */
};

struct home_layout {
    struct home_rect header;                 /* the time, clear of the status cluster */
    struct home_rect date;                   /* the date under it, across the whole row */
    struct home_rect panel[HOME_MAX_GROUPS];
    struct home_rect cell[HOME_MAX_APPS];    /* every app, group by group, in content coordinates */
    struct home_rect lock_button;
    struct home_rect controls_button;
    int32_t cell_w;                          /* the landscape row's cell width (portrait: the fixed one) */
    bool small_labels;                       /* cells too narrow for the 20 px label font */
    bool wrapped;                            /* landscape panels did not fit one row */
    int32_t content_h;                       /* > height: the launcher scrolls */
    uint16_t napps;
};

/* 0, or -1 when the input cannot be laid out (too many groups or apps, or
 * an area too small for one cell). */
int home_layout_compute(const struct home_layout_in *in, struct home_layout *out);

/* ---- the groups (DS §31.2) ------------------------------------------------ *
 *
 * Which group an app is shown in, and in what colour, is the shell's
 * decision, not the app's: a table keyed by app id, so no app declares or
 * even knows its group. An app the table does not name is not lost - it is
 * shown under MORE, after the named groups, in registry order - and a group
 * nothing is installed in is simply not drawn.
 */
enum home_group {
    HOME_GROUP_CONNECT = 0,
    HOME_GROUP_WORK,
    HOME_GROUP_PLAY,
    HOME_GROUP_DEVICE,
    HOME_GROUP_MORE,
    HOME_GROUP_COUNT
};

/* The package hues, in pos_styles.h enum pos_env_hue order (home.c asserts it). */
enum home_hue {
    HOME_HUE_RADIO = 0,
    HOME_HUE_MESH,
    HOME_HUE_NETWORK,
    HOME_HUE_TOOLS,
    HOME_HUE_AI,
    HOME_HUE_GAMES,
    HOME_HUE_SETTINGS,
    HOME_HUE_FILES,
    HOME_HUE_APPS,
};

/* ---- the folders (app groups) ------------------------------------------- *
 *
 * A folder gathers apps the launcher would otherwise show one by one: its
 * page shows a single cell for all of them, where the first of them would
 * have been (for GAMES, in PLAY), and opening that cell shows a page with
 * the apps. Which folder an
 * app is in is the same table's decision as its group and its colour
 * (`folder` below), so a folder is one column of metadata, not a list of
 * exceptions, and an app still neither declares nor knows where it is shown.
 *
 * Only GAMES exists. Another folder (UTILITIES, RADIO, SYSTEM, MEDIA ...) is
 * one more enum value, one more row in the folder table (home_layout.c), a
 * portal icon for its cell (tools/design/gen_doors_ui.py) and its apps'
 * `folder` set - nothing in the launcher itself changes.
 */
enum home_folder {
    HOME_FOLDER_NONE = 0, /* shown on the launcher's own page */
    HOME_FOLDER_GAMES,
    HOME_FOLDER_COUNT
};

struct home_folder_def {
    const char *id;        /* stable: its art is icon-<id>, and shell.folder and shell.info name it */
    const char *name;      /* the cell's label and the folder page's title */
    enum home_hue hue;     /* the colour of its cell's focus mark and its portal icon */
};

struct home_entry {
    const char *id;
    enum home_group group;
    enum home_hue hue;
    enum home_folder folder;
};

const char *home_group_name(enum home_group g);
/* The table's entry for an app id, or NULL: an app the shell has no place for. */
const struct home_entry *home_entry_find(const char *id);
/* Sort n app ids into launcher order: order[k] is the index into ids of the
 * k-th app shown, count[g] how many each group holds. Returns how many were
 * placed (n, or HOME_MAX_APPS if there were more). Folders are not applied:
 * this is every app, as the table orders them. */
int home_group_order(const char *const *ids, int n, uint8_t order[HOME_MAX_APPS],
                     uint8_t count[HOME_GROUP_COUNT]);

/* A folder's definition, or NULL for HOME_FOLDER_NONE and anything out of
 * range. */
const struct home_folder_def *home_folder_get(enum home_folder f);
/* The folder with this id, or HOME_FOLDER_NONE. */
enum home_folder home_folder_find(const char *id);

/* One place on the launcher's own page: an app, or a folder standing for its
 * apps. */
struct home_item {
    uint8_t folder; /* HOME_FOLDER_NONE: an app, ids[index]; else this folder */
    uint8_t index;
};

/* The launcher's own page, in order: home_group_order's, except that every
 * app in a folder is left out and the folder takes one place instead, where
 * its first installed app would have been. A folder with no installed app
 * is not there at all; one with a single app is still a folder, so an app
 * is always found in the same place. count[g] is the places in each group.
 * Returns how many places. */
int home_root_order(const char *const *ids, int n, struct home_item items[HOME_MAX_APPS],
                    uint8_t count[HOME_GROUP_COUNT]);
/* The installed apps of folder f, in the table's order: order[k] is an index
 * into ids. Returns how many (0 for an empty or unknown folder). */
int home_folder_order(const char *const *ids, int n, enum home_folder f, uint8_t order[HOME_MAX_APPS]);

/* ---- a folder's page ------------------------------------------------------- *
 *
 * A way back and the folder's name at the top, clear of the status cluster,
 * then one glass panel holding the apps' cells, like a group's on the
 * launcher: four across in portrait; in landscape as many as fit a row,
 * the panel as wide as its cells and centred. A folder with more apps than
 * fit the screen grows past it and the page scrolls, as the launcher does.
 */
#define HOME_BACK_W 72          /* the back slab, as the app header's (DS §7) */
#define HOME_BACK_H 56

struct home_folder_layout_in {
    int32_t width;
    int32_t height;
    bool landscape;
    int32_t inset_bottom;
    struct home_rect keepout; /* the status cluster, as home_layout_in's */
    int n;                    /* apps in the folder */
};

struct home_folder_layout {
    struct home_rect back;
    struct home_rect title;
    struct home_rect panel;
    struct home_rect cell[HOME_MAX_APPS];
    int cols;
    int32_t cell_w;
    bool small_labels;
    int32_t content_h; /* > height: the page scrolls */
    uint16_t napps;
};

/* 0, or -1 when the input cannot be laid out (too many apps, or an area too
 * small for one cell). An empty folder lays out as a panel one row tall,
 * for a line that says so. */
int home_folder_layout_compute(const struct home_folder_layout_in *in, struct home_folder_layout *out);

#endif
