/*
 * The Browser screen for another app: the Library (docs/apps/LIBRARY.md)
 * is this screen with its own helper, its own remembered state and the
 * view's Library mode. Browser itself is the profile below with nothing set.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BROWSER_APP_H
#define BROWSER_APP_H

#include "lvgl.h"

#include <stdbool.h>

struct browser_profile {
    const char *log;            /* the word log lines start with: "browser", "library" */
    bool library;               /* browser_view_set_library() */
    const char *helper_env;     /* names the helper in the environment; NULL: Browser's */
    const char *helper_default; /* else this one */
    const char *field_hint;     /* the field's placeholder */
};

/* store_path: the remembered state, NULL for Browser's own file. */
void *browser_app_create(lv_obj_t *root, const struct browser_profile *p, const char *store_path);
void browser_app_destroy(void *priv);

#endif
