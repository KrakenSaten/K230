/*
 * DOORS runtime art: the shell's backgrounds and launcher icons, read from
 * files at the moment a screen needs them (art_format.h for the files).
 *
 * The art is not compiled in. Six full-screen backgrounds are 8.4 MB of
 * RGB565, which would sit in the shell's binary whether or not a screen is
 * showing; as files, only the screen in front is in memory, and the rest
 * cost the root filesystem and nothing else. A missing or damaged file is
 * not a failure of the shell: the loader logs why and returns NULL, and
 * every caller has a drawn fallback (a plain background, the app's own
 * mask on an empty frame), so a bench unit without the art still boots to
 * a working launcher.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_ART_H
#define DOORS_ART_H

#include "lvgl.h"

#include <stdbool.h>

/* Where the files are: POCKETOS_ART_DIR from the environment when set (the
 * bench, the tests), else the build's default - /usr/share/doors/ui on the
 * device, the source tree's ui/assets/doors in the simulator. */
const char *art_dir(void);

/* Read <art_dir>/<name>.bin into memory. NULL (and one log line) when it is
 * missing or not a valid art file. The image must be released with
 * art_free() once nothing draws it any more. */
lv_image_dsc_t *art_load(const char *name);
void art_free(lv_image_dsc_t *img);

/* A background for this screen and orientation, e.g. ("home", true) reads
 * bg-home-landscape. */
lv_image_dsc_t *art_load_background(const char *screen, bool landscape);

/* Totals since start, for shell.info and the log: files read, bytes held now. */
unsigned art_loads(void);
size_t art_bytes_held(void);

#endif
