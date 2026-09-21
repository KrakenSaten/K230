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

#include "chrome.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#define POCKETOS_APP_API_VERSION 0

struct pocketos_app {
    const char *id;       /* stable identifier, e.g. "radio" */
    const char *name;     /* launcher label */
    const char *icon;     /* LV_SYMBOL_* or short text; see icon_mask */
    /* Build the UI under root. Return a private pointer or NULL. */
    void *(*create)(lv_obj_t *root);
    /* Called once per second while the app is visible. May be NULL. */
    void (*tick)(void *priv);
    /* Release resources; LVGL objects under root are deleted by the shell. */
    void (*destroy)(void *priv);
    /* The app's launcher icon: an LVGL A8 alpha mask with no colour of its
     * own, e.g. one of ui/pocketui/pos_app_icons.c, which the launcher draws
     * in accent_primary (DS §20). May be NULL: the launcher then draws `icon`
     * as text, as it always did. Appended, so an app that does not set it is
     * unchanged and the API version stays. */
    const lv_image_dsc_t *icon_mask;
    /* The status chrome the app needs above its body (DS §30, chrome.h):
     * DEFAULT, FULL, COMPACT or NONE. A declaration, not a command: the
     * shell resolves it against the orientation and the rollout stage
     * before create() runs, and the app is created in the body that
     * results. It never learns the bar's height and never touches the bar.
     * Appended and zero, so an app that says nothing is DEFAULT and the
     * API version stays. */
    enum pocketos_chrome chrome;
};

/* Shell services available to apps. */
void pocketos_shell_set_status_hint(const char *text); /* short text in the status bar */
void pocketos_shell_go_home(void);
/* Platform-wide reduced-motion preference (DS §12): settings key
 * "reduced_motion" = 0|1 in /etc/pocketos/settings.conf, default 0.
 * When 1, every animation must apply its end state immediately. */
int pocketos_shell_reduced_motion(void);
/* The system date as one number, YYYYMMDD local, or -1 when the wall clock
 * is not set. It comes from the reading the shell's own once-a-second tick
 * already took, through PocketClock's one clock reader and its validity rule
 * (clock_wall): an app that wants the date reads it here rather than calling
 * a clock itself, so there is one answer to what day it is and one place
 * that decides whether the board knows.
 *
 * -1 is not 1970. This board has no clock that survives a power cut, so an
 * unset clock is the ordinary state after boot, and a caller must show that
 * it does not know rather than draw the epoch as a date. */
int64_t pocketos_shell_system_day(void);

/* The radio state the status bar's own once-a-second poll last saw: "rx",
 * "tx", "idle" and so on, or NULL while radiod is not answering. An app that
 * wants to show the radio reads this instead of polling radiod again - the
 * shell is already asking, and a second timer on the same service would
 * double the IPC on the LVGL thread for no new information. The pointer is
 * valid until the next status tick; copy it if you keep it. */
const char *pocketos_shell_radio_state(void);

/* ---- display brightness ------------------------------------------------ *
 *
 * The panel belongs to the shell, and so does its brightness: an app asks
 * here and never opens the backlight device itself. Percentages, with the
 * floor, ceiling and step in brightness.h (BRIGHTNESS_MIN_PCT and friends).
 *
 * _get: the current level, or -1 when this display has no brightness
 * control (the simulator, the HDMI variant) or the level cannot be read.
 * _set: clamps into the allowed range, applies it, and remembers it for the
 * next start only once the panel took it. Returns the level applied, or -1
 * when unsupported or the write failed - in which case nothing is stored. */
int pocketos_shell_brightness_get(void);
int pocketos_shell_brightness_set(int percent);

/* Select the Design System theme and/or display mode (either may be NULL to
 * keep the current one), live, and remember it - the same path as
 * shell.theme over IPC, so the stored selection and the shell.theme event
 * follow. The current selection is pos_theme_current_def() and
 * pos_theme_current_mode(). Returns 0, or -1 when the request was not a
 * valid theme or mode and the DS §8 fallback was applied instead. */
int pocketos_shell_set_appearance(const char *theme_id, const char *mode_name);

/* ---- display orientation (DS §21) --------------------------------------- *
 *
 * The shell owns the orientation; an app never rotates anything and lays out
 * in whatever size its body is given. Settings chooses the mode here.
 *
 * The mode takes effect when the shell next starts, not at once: the display
 * is rotated when it is opened. _orientation says what this run is, what the
 * stored mode gives now (with the keyboard as it is now), and whether those
 * differ. _set stores the mode; returns 0, or -1 when the mode is not one of
 * the three or could not be stored. */
enum pocketos_rotation_mode {
    POCKETOS_ROTATION_AUTOMATIC = 0, /* keyboard present -> landscape, else portrait */
    POCKETOS_ROTATION_PORTRAIT,
    POCKETOS_ROTATION_LANDSCAPE,
};

enum pocketos_keyboard {
    POCKETOS_KEYBOARD_UNKNOWN = 0,
    POCKETOS_KEYBOARD_ABSENT,
    POCKETOS_KEYBOARD_PRESENT,
};

struct pocketos_orientation {
    enum pocketos_rotation_mode mode; /* stored (automatic when nothing or nonsense is stored) */
    bool mode_valid;                  /* false: the stored value was not a mode */
    bool landscape;                   /* this run */
    bool next_landscape;              /* what the mode and the keyboard now ask for */
    bool applying;                    /* the two differ: the shell is restarting itself to apply it */
    enum pocketos_keyboard keyboard;
};

void pocketos_shell_orientation(struct pocketos_orientation *out);
int pocketos_shell_set_rotation_mode(enum pocketos_rotation_mode mode);

/* ---- the touch keyboard (DS §17.3, §17.4) ------------------------------ *
 *
 * There is exactly one keyboard and the shell owns it. An app asks for it
 * here; it never creates one, never holds a pointer to one, and never learns
 * whether a character was tapped on it, typed on the host keyboard or, later,
 * on a physical one. Characters arrive in the focused text field either way.
 *
 * Showing it shrinks the app's body by the sheet's height so the field above
 * stays usable, and hiding it gives that space back. Neither moves focus.
 */
enum pocketos_kb_return {
    POCKETOS_KB_DONE,   /* single-line field: commits, and on_done is called */
    POCKETOS_KB_NEWLINE /* multi-line field: inserts a line break */
};

/* Show the keyboard. on_done may be NULL and is only ever called for
 * POCKETOS_KB_DONE: the app decides what committing means, and whether to
 * hide the keyboard afterwards. */
void pocketos_shell_keyboard_show(enum pocketos_kb_return ret,
                                  void (*on_done)(void *user), void *user);
void pocketos_shell_keyboard_hide(void);
int pocketos_shell_keyboard_visible(void);

/* v0.1 lifecycle limitation: an app is created when opened and destroyed
 * when left; there is no pause/resume/suspend and no background state.
 * Apps that need continuity persist their own state on each change
 * (ADR-002 lists the fuller lifecycle as a later step). */

#endif
