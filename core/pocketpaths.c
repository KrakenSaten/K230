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

int pocketos_release_read(const char *root, struct pocketos_release *rel)
{
    static const char *const names[] = { POCKETOS_RELEASE_FILE, POCKETOS_RELEASE_FILE_COMPAT };
    FILE *f = NULL;
    char line[256];
    size_t i;
    int lines = 0;
    int at_start = 1;
    int have_build_line = 0;

    memset(rel, 0, sizeof(*rel));
    for (i = 0; i < sizeof(names) / sizeof(names[0]) && !f; i++) {
        if (snprintf(rel->path, sizeof(rel->path), "%s%s", root ? root : "", names[i]) >=
            (int)sizeof(rel->path)) {
            rel->path[0] = '\0';
            errno = ENAMETOOLONG;
            return -1;
        }
        f = fopen(rel->path, "r");
        if (!f && errno != ENOENT) {
            return -1;
        }
    }
    if (!f) {
        rel->path[0] = '\0';
        errno = ENOENT;
        return -1;
    }
    /* A line longer than the buffer arrives in pieces; only the piece that
     * starts a line is looked at. */
    while (fgets(line, sizeof(line), f)) {
        size_t len = strcspn(line, "\n");
        int ends_line = line[len] == '\n';

        line[len] = '\0';
        if (at_start) {
            if (lines == 0) {
                snprintf(rel->version, sizeof(rel->version), "%s", line);
            } else if (!have_build_line && strncmp(line, "BUILD_ID=", 9) == 0) {
                snprintf(rel->build, sizeof(rel->build), "%s", line + 9);
                have_build_line = 1;
            }
            lines++;
        }
        at_start = ends_line;
    }
    fclose(f);
    return 0;
}
