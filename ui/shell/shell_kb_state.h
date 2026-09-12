/*
 * Whether the one touch keyboard may be shown at all.
 *
 * The shell owns the keyboard (DS §17.3, §17.4). While a system alert holds
 * the panel, §18.5 says the keyboard is dismissed and deliberately not given
 * back, and §18.8 requires that nothing underneath the alert can bring it up
 * again - an app that asks is not misbehaving, it simply cannot see that an
 * alert exists, and must not be able to.
 *
 * That is a policy about a single boolean, so it lives here rather than
 * inside the shell's LVGL code: shell_alarm.c sets it, the shell's
 * pocketos_shell_keyboard_show() asks it, and a test can link this one file
 * and exercise the real rule instead of a copy of it.
 *
 * Deliberately not in app.h: an app must never learn that an alert exists.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_SHELL_KB_STATE_H
#define POCKETOS_SHELL_KB_STATE_H

/* Raised while an alert owns the panel, lowered when it lets go. */
void pocketos_shell_keyboard_set_suppressed(int on);
int pocketos_shell_keyboard_suppressed(void);

/* The predicate every caller of the keyboard's show path must ask. Hiding is
 * never suppressed: taking the keyboard away has to work at any time. */
int pocketos_shell_keyboard_may_show(void);

#endif
