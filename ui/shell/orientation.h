/*
 * Display orientation policy: the user's rotation mode and the keyboard's
 * presence decide one effective rotation. Pure C. The shell owns the result;
 * apps never choose an orientation.
 *
 *   Portrait   rotation 0, whatever the keyboard
 *   Landscape  ORIENTATION_LANDSCAPE_ROTATION, whatever the keyboard
 *   Automatic  keyboard present -> Landscape; absent or unknown -> Portrait
 *
 * Landscape is one direction, not a choice of two: DRM rotation 270, the
 * rotation the vendor launcher runs at on this board with its keyboard in
 * use (docs/hardware/KEYBOARD_BRINGUP_2026-09-10.md §6). By the mapping in
 * pos_display.h, the portrait panel's left edge becomes the top and its
 * bottom edge the left: the device turned a quarter turn clockwise.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_ORIENTATION_H
#define POCKETOS_ORIENTATION_H

#include "kbd_presence.h"
#include "pos_display.h"

#include <stdbool.h>

/* The settings key (ui/shell/settings.h). */
#define ORIENTATION_SETTING "display_rotation"
#define ORIENTATION_LANDSCAPE_ROTATION POS_ROTATION_270

enum orientation_mode {
    ORIENTATION_AUTOMATIC = 0,
    ORIENTATION_PORTRAIT,
    ORIENTATION_LANDSCAPE,
};

/* "automatic" | "portrait" | "landscape"; parse returns -1 for anything else. */
const char *orientation_mode_name(enum orientation_mode m);
int orientation_mode_parse(const char *text, enum orientation_mode *out);

/* The mode a stored value means: absent is automatic (the default) and so is
 * anything unparsable, which also clears *valid so the caller can say so.
 * The stored value itself is never rewritten here (DS §8 fallback rule). */
enum orientation_mode orientation_mode_from_setting(const char *stored, bool *valid);

/* The one decision. */
enum pos_rotation orientation_resolve(enum orientation_mode m, enum kbd_presence keyboard);

/* And what an open app that runs in portrait only (app.h `orientation`)
 * makes of it: portrait while it holds, the decision above otherwise. A hold
 * does not change the mode; it only sets it aside until the app closes. */
enum pos_rotation orientation_hold(enum pos_rotation resolved, bool portrait_only);

#endif
