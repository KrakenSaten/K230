/*
 * Status chrome policy (DS §30): the resolver and the geometry that follows
 * from it. See chrome.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "chrome.h"

enum pocketos_chrome chrome_resolve(enum pocketos_chrome declared, bool landscape, bool home)
{
    if (home) {
        return POCKETOS_CHROME_FULL;
    }
    /* DS §30.4, stage 1: portrait keeps the bar every screen has had,
     * whatever an app declares, so every portrait screen stays what
     * v0.0.10 drew. This line is the whole of that rule; lifting it is
     * lifting this line, and the checks in tests/chrome_test.c that hold
     * it. */
    if (!landscape) {
        return POCKETOS_CHROME_FULL;
    }
    switch (declared) {
    case POCKETOS_CHROME_FULL:
    case POCKETOS_CHROME_COMPACT:
    case POCKETOS_CHROME_NONE:
        return declared;
    case POCKETOS_CHROME_DEFAULT:
    default:
        return POCKETOS_CHROME_COMPACT;
    }
}

int32_t chrome_height(enum pocketos_chrome effective)
{
    switch (effective) {
    case POCKETOS_CHROME_COMPACT:
        return POCKETOS_CHROME_COMPACT_H;
    case POCKETOS_CHROME_NONE:
        return 0;
    case POCKETOS_CHROME_FULL:
    case POCKETOS_CHROME_DEFAULT:
    default:
        return POCKETOS_CHROME_FULL_H;
    }
}

const char *chrome_name(enum pocketos_chrome c)
{
    switch (c) {
    case POCKETOS_CHROME_FULL:
        return "full";
    case POCKETOS_CHROME_COMPACT:
        return "compact";
    case POCKETOS_CHROME_NONE:
        return "none";
    case POCKETOS_CHROME_DEFAULT:
    default:
        return "default";
    }
}

struct chrome_box chrome_content_box(enum pocketos_chrome effective, int32_t display_h,
                                     int32_t reserve)
{
    struct chrome_box b;

    if (reserve < 0) {
        reserve = 0;
    }
    b.y = chrome_height(effective);
    b.height = display_h - b.y - reserve;
    if (b.height < 0) {
        b.height = 0;
    }
    return b;
}
