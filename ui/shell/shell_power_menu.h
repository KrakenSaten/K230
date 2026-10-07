/*
 * The power menu: what a one-second hold of the power key brings up
 * (shell_evkey.h). Restart and Power off, the same two machine actions
 * System's OVERVIEW offers, through the same sysd methods (system.reboot,
 * system.poweroff; docs/api/system.md), and Cancel.
 *
 * A §17.5 panel on the top layer, centred, over whatever is on screen -
 * an app, the launcher or the lock - with a scrim that takes every touch
 * outside it as Cancel. Cancel comes first and carries the accent: a
 * power-off on this board costs a walk to the device and a 3 s hold to
 * undo. Opening it does nothing; only Restart or Power off pressed does.
 *
 * It does not open the lock and is not refused by it: the lock is no
 * security (shell_lock.h), and the kernel's own 5 s hold powers the device
 * off from under the lock anyway. Closing the menu leaves the lock as it
 * was. The body says that holding on powers off; the menu cannot stop it
 * and does not pretend to.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef DOORS_SHELL_POWER_MENU_H
#define DOORS_SHELL_POWER_MENU_H

#include <stdbool.h>

/* Open it, or leave it as it is if it is already open. */
void shell_power_menu_open(void);
/* Close it, as Cancel does; nothing if it is closed. why is for the log. */
void shell_power_menu_close(const char *why);
bool shell_power_menu_visible(void);
/* The line under the buttons: "" normally, "Restarting..." once sysd took
 * the request, or the reason it was refused. For shell.info and the tests. */
const char *shell_power_menu_note(void);
unsigned shell_power_menu_open_count(void);

#endif
