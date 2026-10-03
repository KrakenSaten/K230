/*
 * The seam RIFT's message sounds go through. See rift_sound.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_sound.h"

#include <stddef.h>

/* The silent backend: what rift_sound_set_backend(NULL) installs. */
static const struct rift_sound_backend none = {
    .name = "none",
    .available = NULL,
    .play = NULL,
    .stop = NULL,
    .why = "No system notification sound in this build of Doors: a new message is shown, "
           "not heard.",
};

static const struct rift_sound_backend *current = &rift_sound_pos_record;

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

int rift_sound_play(enum rift_sound_kind kind, int volume_percent)
{
    if (!rift_sound_available() || volume_percent <= 0 || kind < 0 || kind >= RIFT_SOUND_KINDS) {
        return -1;
    }
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    return current->play(kind, volume_percent) == 0 ? 0 : -1;
}

void rift_sound_stop(void)
{
    if (current->stop) {
        current->stop();
    }
}
