/*
 * The seam RIFT's message sound goes through. See rift_sound.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_sound.h"

#include <stddef.h>

/* The built-in backend: this build of Doors has no notification sound for
 * an app to ask for, so there is none, and it says so. */
static const struct rift_sound_backend none = {
    .name = "none",
    .available = NULL,
    .play = NULL,
    .stop = NULL,
    .why = "No system notification sound in this build of Doors: a new direct message is "
           "shown, not heard.",
};

static const struct rift_sound_backend *current = &none;

void rift_sound_set_backend(const struct rift_sound_backend *backend)
{
    current = backend ? backend : &none;
}

const char *rift_sound_backend_name(void)
{
    return current->name ? current->name : "";
}

const char *rift_sound_why(void)
{
    return current->why ? current->why : "";
}

int rift_sound_available(void)
{
    return current->available && current->play && current->available() ? 1 : 0;
}

int rift_sound_play(int volume_percent)
{
    if (!rift_sound_available() || volume_percent <= 0) {
        return -1;
    }
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    return current->play(volume_percent) == 0 ? 0 : -1;
}

void rift_sound_stop(void)
{
    if (current->stop) {
        current->stop();
    }
}
