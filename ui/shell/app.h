/*
 * PocketOS application API (in-process variant, ADR-002).
 * An app gets a content root below the status bar, is created when opened,
 * ticked every second while visible, and destroyed when the user leaves.
 * Apps never touch hardware; they talk to services over pocketipc.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_APP_H
#define POCKETOS_APP_H

#include "lvgl.h"

#define POCKETOS_APP_API_VERSION 0

struct pocketos_app {
    const char *id;       /* stable identifier, e.g. "radio" */
    const char *name;     /* launcher label */
    const char *icon;     /* LV_SYMBOL_* or short text */
    /* Build the UI under root. Return a private pointer or NULL. */
    void *(*create)(lv_obj_t *root);
    /* Called once per second while the app is visible. May be NULL. */
    void (*tick)(void *priv);
    /* Release resources; LVGL objects under root are deleted by the shell. */
    void (*destroy)(void *priv);
};

/* Shell services available to apps. */
void pocketos_shell_set_status_hint(const char *text); /* short text in the status bar */
void pocketos_shell_go_home(void);

#endif
