/*
 * Display orientation policy. See orientation.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "orientation.h"

#include <stddef.h>
#include <string.h>

const char *orientation_mode_name(enum orientation_mode m)
{
    switch (m) {
    case ORIENTATION_PORTRAIT: return "portrait";
    case ORIENTATION_LANDSCAPE: return "landscape";
    case ORIENTATION_AUTOMATIC:
    default: return "automatic";
    }
}

int orientation_mode_parse(const char *text, enum orientation_mode *out)
{
    if (!text) {
        return -1;
    }
    if (strcmp(text, "automatic") == 0) {
        *out = ORIENTATION_AUTOMATIC;
    } else if (strcmp(text, "portrait") == 0) {
        *out = ORIENTATION_PORTRAIT;
    } else if (strcmp(text, "landscape") == 0) {
        *out = ORIENTATION_LANDSCAPE;
    } else {
        return -1;
    }
    return 0;
}

enum orientation_mode orientation_mode_from_setting(const char *stored, bool *valid)
{
    enum orientation_mode m = ORIENTATION_AUTOMATIC;

    if (!stored) {
        *valid = true;
        return ORIENTATION_AUTOMATIC;
    }
    *valid = orientation_mode_parse(stored, &m) == 0;
    return *valid ? m : ORIENTATION_AUTOMATIC;
}

enum pos_rotation orientation_resolve(enum orientation_mode m, enum kbd_presence keyboard)
{
    switch (m) {
    case ORIENTATION_PORTRAIT:
        return POS_ROTATION_0;
    case ORIENTATION_LANDSCAPE:
        return ORIENTATION_LANDSCAPE_ROTATION;
    case ORIENTATION_AUTOMATIC:
    default:
        /* Only a keyboard known to be there turns the display. Unknown is not
         * a guess in either direction: it is portrait, the orientation the
         * panel boots in. */
        return keyboard == KBD_PRESENCE_PRESENT ? ORIENTATION_LANDSCAPE_ROTATION : POS_ROTATION_0;
    }
}
