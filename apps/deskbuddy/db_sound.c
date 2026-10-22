/*
 * DeskBuddy's visit sound: the backend seam. See db_sound.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "db_sound.h"

static int none_available(void)
{
    return 0;
}

static const struct db_sound_backend none = {
    .name = "none",
    .available = none_available,
    .play = NULL,
    .stop = NULL,
    .why = "No sound backend: a visit is shown and logged, not heard.",
};

static const struct db_sound_backend *current = &db_sound_pos_record;

void db_sound_set_backend(const struct db_sound_backend *backend)
{
    current = backend ? backend : &none;
}

const char *db_sound_backend_name(void)
{
    return current->name ? current->name : "";
}

const char *db_sound_why(void)
{
    return current->why ? current->why : "";
}

int db_sound_available(void)
{
    return current->available && current->play && current->available() ? 1 : 0;
}

int db_sound_play(int volume_percent)
{
    if (!db_sound_available() || volume_percent <= 0) {
        return -1;
    }
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    return current->play(volume_percent) == 0 ? 0 : -1;
}

void db_sound_stop(void)
{
    if (current->stop) {
        current->stop();
    }
}
