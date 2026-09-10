/*
 * system.reboot and system.poweroff: the machine actions of the system.* API
 * (docs/api/system.md).
 *
 * Both go through BusyBox init by running /sbin/reboot or /sbin/poweroff,
 * never through reboot(2). init is what runs rcK, stops S90, S60 and S50 in
 * reverse order, syncs and remounts the root filesystem read-only. Calling
 * reboot(2) from here would skip all of that: the shell would never release
 * the panel, nothing would be flushed, and the SD card would be cut off
 * mid-write.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef SYSD_POWER_H
#define SYSD_POWER_H

enum sysd_power_action {
    SYSD_POWER_NONE = 0,
    SYSD_POWER_REBOOT,
    SYSD_POWER_POWEROFF
};

/* The name of an action, for logs and for the response. */
const char *sysd_power_name(enum sysd_power_action action);

/* The command that action runs. Production: /sbin/reboot or /sbin/poweroff,
 * fixed at compile time. A build with -DSYSD_TEST_HOOKS=1 lets the
 * environment replace them so that a host test can prove which action was
 * chosen without rebooting the developer's machine; the shipped sysd is not
 * built with it and carries no such path (Makefile, tests/sysd-testhooks). */
const char *sysd_power_command(enum sysd_power_action action);

/* Spawn the command and wait for it to finish. Returns 0 when it ran and
 * exited 0, -1 otherwise. BusyBox reboot and poweroff signal init and return
 * at once; init does the rest, so this is not the caller's last instruction
 * and sysd keeps serving until rcK stops it like any other service. */
int sysd_power_run(enum sysd_power_action action);

#endif
