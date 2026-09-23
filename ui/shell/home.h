/*
 * The DOORS launcher (DS §31.3): the time and the date, the apps in glass
 * panels by group (home_layout.h), and two shell actions at the foot - Lock
 * and Controls. Built once per shell run, for the orientation of that run.
 *
 * The background is not the launcher's: the shell draws the home
 * photograph behind the status bar and the launcher alike (shell.c), so the
 * two read as one surface.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
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
};

/* Build under parent (the shell's content area, already at its launcher
 * size). apps is the shell's registry. Returns the launcher's root. */
lv_obj_t *home_create(lv_obj_t *parent, const struct pocketos_app *const *apps, size_t napps,
                      bool landscape, const struct home_actions *actions);

/* The header's time ("07:30" or "--:--") and date ("" when unset). */
void home_set_time(const char *hm, const char *date);

/* For shell.info: how the launcher came out. */
struct home_info {
    int groups;
    int apps;
    int icons_art;       /* apps drawn with their portal icon file */
    int icons_fallback;  /* apps drawn on the empty frame or as text */
    bool scrolls;
    bool wrapped;
    int cell_w;
};
void home_info(struct home_info *out);

/* The screen rectangle of an app's cell, for tests and the bench: false when
 * the app is not on the launcher. */
bool home_cell_area(const char *app_id, lv_area_t *out);

#endif
