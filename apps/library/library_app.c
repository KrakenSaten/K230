/*
 * Library: an offline knowledge collection (a Kiwix .zim archive) on the
 * unit - search it, read its articles and pictures, follow its links, keep
 * bookmarks (docs/apps/LIBRARY.md, a feasibility prototype).
 *
 * This file is the app and nothing else: the screen is Browser's
 * (apps/browser/browser_app.h) in the view's Library mode, and everything
 * that touches the archive - opening it, decompressing, searching, the HTML
 * reader, the picture decoders - is in pos-library, a helper process started
 * with the first page and ended when the screen goes. The helper links no
 * network code at all.
 *
 * Remembered state (bookmarks and recent articles) is Browser's format in a
 * file of its own, <state>/library/state; the two apps never share it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"
#include "browser_app.h"
#include "pocketpaths.h"

#include <stdio.h>

static const struct browser_profile library_profile = {
    .log = "library",
    .library = true,
    .helper_env = "POCKETOS_LIBRARY_HELPER",
    .helper_default = "/usr/bin/pos-library",
    .field_hint = "Search the collection",
};

static void *library_create(lv_obj_t *root)
{
    char path[POCKETOS_PATH_MAX];

    snprintf(path, sizeof(path), "%s/library/state", pocketos_state_dir());
    return browser_app_create(root, &library_profile, path);
}

const struct pocketos_app app_library = {
    .id = "library",
    .name = "Library",
    .icon = LV_SYMBOL_FILE,
    /* No first-party icon yet: the launcher draws the glyph. */
    .icon_mask = NULL,
    .create = library_create,
    .tick = NULL,
    .destroy = browser_app_destroy,
    .chrome = POCKETOS_CHROME_NONE,
};
