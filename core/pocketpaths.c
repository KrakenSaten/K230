/*
 * PocketOS filesystem roots. See pocketpaths.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketpaths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *dir_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);

    return (v && *v) ? v : fallback;
}

const char *pocketos_runtime_dir(void)
{
    return dir_or("POCKETOS_RUNTIME_DIR", POCKETOS_RUNTIME_DIR_DEFAULT);
}

const char *pocketos_config_dir(void)
{
    return dir_or("POCKETOS_CONFIG_DIR", POCKETOS_CONFIG_DIR_DEFAULT);
}

const char *pocketos_state_dir(void)
{
    return dir_or("POCKETOS_STATE_DIR", POCKETOS_STATE_DIR_DEFAULT);
}

const char *pocketos_log_dir(void)
{
    return dir_or("POCKETOS_LOG_DIR", POCKETOS_LOG_DIR_DEFAULT);
}

/* mkdir, but an existing directory is success and an existing non-directory
 * is not. The stores' own make_dirs() treated both as success, which turned a
 * file in the way into a write that failed later and further from the cause. */
static int make_one(const char *path, mode_t mode)
{
    struct stat st;

    if (mkdir(path, mode) == 0) {
        return 0;
    }
    if (errno != EEXIST) {
        return -1;
    }
    if (stat(path, &st) < 0) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = EEXIST;
        return -1;
    }
    return 0;
}

int pocketos_mkdir_p(const char *path, mode_t mode)
{
    char work[POCKETOS_PATH_MAX];
    size_t i;

    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    if (snprintf(work, sizeof(work), "%s", path) >= (int)sizeof(work)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (i = 1; work[i]; i++) {
        if (work[i] != '/') {
            continue;
        }
        work[i] = '\0';
        if (make_one(work, mode) < 0) {
            return -1;
        }
        work[i] = '/';
    }
    /* A trailing slash means the last component was already created above. */
    if (i > 1 && work[i - 1] == '/') {
        return 0;
    }
    return make_one(work, mode);
}
