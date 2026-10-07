/*
 * sysd_power implementation. See sysd_power.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "sysd_power.h"

#include "pocketlog/pocketlog.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SYSD_REBOOT_PATH "/sbin/reboot"
#define SYSD_POWEROFF_PATH "/sbin/poweroff"

const char *sysd_power_name(enum sysd_power_action action)
{
    switch (action) {
    case SYSD_POWER_REBOOT:
        return "reboot";
    case SYSD_POWER_POWEROFF:
        return "poweroff";
    case SYSD_POWER_NONE:
    default:
        return "none";
    }
}

#ifdef SYSD_TEST_HOOKS
/* Test builds only. $SYSD_REBOOT_COMMAND and $SYSD_POWEROFF_COMMAND stand in
 * for the real commands so tests/sysd_test.sh can watch an action be chosen
 * and run without restarting the build host. Two variables rather than one:
 * a test has to be able to prove that reboot picked the reboot command and
 * poweroff picked the other, not merely that something ran.
 *
 * The same rule as POCKETSYS_TEST_HOOKS applies and is why this is a build
 * option and not an environment switch: a service that can be told from its
 * environment to run a different program instead of rebooting the machine is
 * a service whose behaviour is decided by whoever sets that environment. The
 * shipped binary is compiled without this and does not contain the variable
 * names at all, which tests/sysd_test.sh checks. */
const char *sysd_power_command(enum sysd_power_action action)
{
    const char *override = NULL;

    if (action == SYSD_POWER_REBOOT) {
        override = getenv("SYSD_REBOOT_COMMAND");
        return override ? override : SYSD_REBOOT_PATH;
    }
    override = getenv("SYSD_POWEROFF_COMMAND");
    return override ? override : SYSD_POWEROFF_PATH;
}
#else
const char *sysd_power_command(enum sysd_power_action action)
{
    return action == SYSD_POWER_REBOOT ? SYSD_REBOOT_PATH : SYSD_POWEROFF_PATH;
}
#endif

int sysd_power_run(enum sysd_power_action action)
{
    const char *cmd;
    pid_t child;
    int status = 0;

    if (action != SYSD_POWER_REBOOT && action != SYSD_POWER_POWEROFF) {
        return -1;
    }
    cmd = sysd_power_command(action);
    LOG_INFO("%s: running %s", sysd_power_name(action), cmd);
    child = fork();
    if (child < 0) {
        LOG_ERROR("%s: fork: %s", sysd_power_name(action), strerror(errno));
        return -1;
    }
    if (child == 0) {
        execl(cmd, cmd, (char *)NULL);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            LOG_ERROR("%s: waitpid: %s", sysd_power_name(action), strerror(errno));
            return -1;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        /* The client was told the action was accepted before this ran, so
         * there is nobody left to tell. Say so where an operator will find
         * it: the reply means accepted, not completed. */
        LOG_ERROR("%s: %s did not succeed (status %d)", sysd_power_name(action), cmd, status);
        return -1;
    }
    return 0;
}
