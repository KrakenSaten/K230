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

#include "lvgl.h"

#define POCKETOS_APP_API_VERSION 0

struct pocketos_app {
    const char *id;       /* stable identifier, e.g. "radio" */
    const char *name;     /* launcher label */
    const char *icon;     /* LV_SYMBOL_* or short text */
    /* Build the UI under root. Return a private pointer or NULL. */
    void *(*create)(lv_obj_t *root);
    /* Called once per second while the app is visible. May be NULL. */
    void (*tick)(void *priv);
    /* Release resources; LVGL objects under root are deleted by the shell. */
    void (*destroy)(void *priv);
};

/* Shell services available to apps. */
void pocketos_shell_set_status_hint(const char *text); /* short text in the status bar */
void pocketos_shell_go_home(void);
/* Platform-wide reduced-motion preference (DS §12): settings key
 * "reduced_motion" = 0|1 in /etc/pocketos/settings.conf, default 0.
 * When 1, every animation must apply its end state immediately. */
int pocketos_shell_reduced_motion(void);
/* The radio state the status bar's own once-a-second poll last saw: "rx",
 * "tx", "idle" and so on, or NULL while radiod is not answering. An app that
 * wants to show the radio reads this instead of polling radiod again - the
 * shell is already asking, and a second timer on the same service would
 * double the IPC on the LVGL thread for no new information. The pointer is
 * valid until the next status tick; copy it if you keep it. */
const char *pocketos_shell_radio_state(void);

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
