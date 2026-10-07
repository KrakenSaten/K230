/*
 * The DOORS launcher (DS §31.3): the time and the date, the apps in glass
 * panels by group (home_layout.h), and two shell actions at the foot - Lock
 * and Controls. Built once per shell run, for the orientation of that run.
 *
 * Folders (app groups, home_layout.h): an app in a folder is not on the
 * launcher's own page; its folder's cell is, and opening that shows the
 * folder's page - a way back, the folder's name and its apps in one panel.
 * A folder's page is built when it opens and deleted when it closes; one
 * folder is open at most. Coming home from an app opened in a folder comes
 * back to the folder.
 *
 * Keys (DS §17.4): at home the launcher is the focused object and the
 * arrows move a focus among the cells of the page showing - the package's
 * press mark follows it once a key has been used - Enter opens the focused
 * app or folder, and Esc or Backspace goes back from a folder to the cell it
 * was opened from. The shell hands the keys to the launcher when home shows
 * and takes them away when an app or Controls covers it.
 *
 * The background is not the launcher's: the shell draws the home
 * photograph behind the status cluster and the launcher alike (shell.c), so
 * the two read as one surface.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_HOME_H
#define DOORS_HOME_H

#include "app.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

struct home_actions {
    void (*open)(const struct pocketos_app *app);
    void (*lock)(void);
    void (*controls)(void);
    /* Where the favorites live (home_layout.h, favorites): what slot
     * (0-based) holds, or NULL/"" for empty, read once per home_create; and
     * keeping a change, id NULL for empty, 0 or -1 when it could not be
     * kept (the launcher shows the change anyway). Either may be NULL: the
     * favorites then last as long as the launcher. */
    const char *(*favorite_get)(int slot);
    int (*favorite_set)(int slot, const char *id);
};

/* Build under parent (the shell's content area, already at its launcher
 * size). apps is the shell's registry. keepout is the status cluster's
 * widest box in parent's coordinates (DS §36), which the header keeps clear
 * of; NULL: none. Returns the launcher's root: the object the shell shows
 * and hides, which holds the launcher's page and an open folder's. */
lv_obj_t *home_create(lv_obj_t *parent, const struct pocketos_app *const *apps, size_t napps,
                      bool landscape, const lv_area_t *keepout, const struct home_actions *actions);
/* Delete everything home_create made and release its art: for the tests,
 * which build a launcher many times in one process. */
void home_destroy(void);

/* The header's time ("07:30" or "--:--") and date ("" when unset). */
void home_set_time(const char *hm, const char *date);

/* The keys (see above): attach when home shows, detach when something
 * covers it. Attach joins the focus group and takes the focus; both are
 * safe to repeat. */
void home_keys_attach(void);
void home_keys_detach(void);

/* ---- favorites ------------------------------------------------------------- *
 *
 * The first three cells of the launcher's page (home_layout.h, favorites).
 * An empty slot shows a dimmed empty portal with a plus; a tap on it, or a
 * long press, opens the picker. A tap on a slot holding an installed app
 * opens the app; a long press opens the picker. The picker is a page like a
 * folder's: a way back, "Favorite N", and a panel with - when the slot holds
 * something - a Clear cell first, then every installed app the other slots
 * do not hold, each once. Choosing one sets the slot (and keeps it through
 * favorite_set) and comes back to the launcher's page, the focus on the
 * slot. The way back, Esc or Backspace leave the slot as it was.
 *
 * The long press is LVGL's own (LV_EVENT_LONG_PRESSED, 400 ms, never sent
 * once the finger has started to scroll the page), and the finger is then
 * ignored until it lifts (lv_indev_wait_release), so the release neither
 * opens the app nor lands on the picker. Keys: Enter on an empty slot opens
 * the picker, and E on any slot does (the keys' long press).
 *
 * Slots are 0-based. */

/* Give slot the installed app id, or clear it (id NULL or ""). 0; -1 when
 * the slot is out of range or id is not an installed app; -2 when another
 * slot holds id. Closes the picker if it is open. Not from inside an LVGL
 * event. */
int home_favorite_set(int slot, const char *id);
/* The installed app slot holds, or NULL (empty, or its app is not here). */
const char *home_favorite_id(int slot);
/* What slot holds as stored, installed or not, or NULL when empty. */
const char *home_favorite_stored(int slot);
/* Open the picker for slot at once (not from inside an LVGL event). False
 * when there is nothing to offer or no launcher. */
bool home_favorite_pick(int slot);
/* The slot the picker is open for, or -1. */
int home_favorite_picking(void);
/* The slot's cell on the screen while the launcher's page shows. */
bool home_favorite_area(int slot, lv_area_t *out);

/* ---- folders --------------------------------------------------------------- */

/* Open the folder with this id at once (not from inside an LVGL event: the
 * launcher's own taps and keys defer it). False, and nothing changes, when
 * there is no such folder or none of its apps is installed. Opening the
 * folder that is open does nothing and is true. */
bool home_folder_open(const char *id);
/* Back to the launcher's own page, the focus on the folder's cell (or the
 * favorite's, from the picker, which this closes too). Nothing when
 * neither is open. */
void home_folder_close(void);
/* The open folder's id, or NULL. */
const char *home_folder_current(void);

/* For shell.info: how the launcher came out. */
struct home_info {
    int groups;
    int apps;            /* installed apps, in folders or not */
    int cells;           /* cells on the launcher's own page: apps and folders */
    int folders;         /* folder cells on it */
    int favorites;       /* favorite slots on it, before those cells (HOME_FAVORITES) */
    int favorites_set;   /* of them, holding an installed app now */
    int icons_art;       /* cells drawn with their portal icon file */
    int icons_fallback;  /* cells drawn on the empty frame or as text */
    bool scrolls;
    bool wrapped;
    int cell_w;
};
void home_info(struct home_info *out);

/* The screen rectangle of an app's cell on the page showing - the
 * launcher's, or the open folder's - for tests and the bench: false when
 * the app is not on it. */
bool home_cell_area(const char *app_id, lv_area_t *out);
/* The same for a folder's cell on the launcher's page. */
bool home_folder_area(const char *folder_id, lv_area_t *out);
/* The open folder's way back, on the screen: false when none is open. */
bool home_folder_back_area(lv_area_t *out);
/* A folder's size: its installed apps, or -1 for an unknown id. */
int home_folder_size(const char *folder_id);
/* What the keys would act on: the focused cell's app or folder id,
 * "favorite-N" for a favorite's slot (N from 1), "clear" for the picker's
 * Clear, or NULL; and whether a key has been used, so the mark is drawn. */
const char *home_focus_id(bool *shown);

/* The screen rectangles the header's time and date labels take now, for
 * tests and the bench: false before the launcher is built. */
bool home_header_area(lv_area_t *time, lv_area_t *date);

#endif
