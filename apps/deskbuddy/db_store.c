/*
 * DeskBuddy's files. See db_store.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "db_store.h"

#include "pocketpaths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir_buf[POCKETOS_PATH_MAX];

const char *db_store_dir(void)
{
    snprintf(dir_buf, sizeof(dir_buf), "%s/%s", pocketos_state_dir(), DB_STORE_SUBDIR);
    return dir_buf;
}

static void path_of(const char *file, char *out, size_t len)
{
    snprintf(out, len, "%s/%s", db_store_dir(), file);
}

/* The whole file into buf (NUL-terminated). 0, 1 when absent, -1 otherwise.
 * A file longer than buf is not one this build wrote. */
static int read_file(const char *file, char *buf, size_t len)
{
    char path[POCKETOS_PATH_MAX + 32];
    size_t got;
    FILE *f;

    path_of(file, path, sizeof(path));
    f = fopen(path, "r");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }
    got = fread(buf, 1, len - 1, f);
    if (ferror(f) || (got == len - 1 && fgetc(f) != EOF)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[got] = '\0';
    return 0;
}

static int write_file(const char *file, const char *text, size_t n)
{
    char path[POCKETOS_PATH_MAX + 32];
    char tmp[POCKETOS_PATH_MAX + 40];
    int fd;

    if (pocketos_mkdir_p(db_store_dir(), 0700) != 0) {
        return -1;
    }
    path_of(file, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    if (write(fd, text, n) != (ssize_t)n || fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int db_store_load_prefs(struct db_prefs *p)
{
    char text[DB_PREFS_TEXT_MAX * 2];
    int rc;

    if (!p) {
        return -1;
    }
    db_prefs_defaults(p);
    rc = read_file(DB_STORE_PREFS, text, sizeof(text));
    if (rc == 0) {
        db_prefs_parse(p, text);
    }
    return rc;
}

int db_store_save_prefs(const struct db_prefs *p)
{
    char text[DB_PREFS_TEXT_MAX];
    int n = db_prefs_format(p, text, sizeof(text));

    return n < 0 ? -1 : write_file(DB_STORE_PREFS, text, (size_t)n);
}

int db_store_load_guard(struct db_guard_log *log)
{
    char *text;
    int rc;

    if (!log) {
        return -1;
    }
    db_guard_init(log);
    text = malloc(DB_GUARD_TEXT_MAX * 2);
    if (!text) {
        return -1;
    }
    rc = read_file(DB_STORE_GUARD, text, DB_GUARD_TEXT_MAX * 2);
    if (rc == 0) {
        db_guard_parse(log, text);
    }
    free(text);
    return rc;
}

int db_store_save_guard(const struct db_guard_log *log)
{
    char *text = malloc(DB_GUARD_TEXT_MAX);
    int n;
    int rc = -1;

    if (!text) {
        return -1;
    }
    n = db_guard_format(log, text, DB_GUARD_TEXT_MAX);
    if (n >= 0) {
        rc = write_file(DB_STORE_GUARD, text, (size_t)n);
    }
    free(text);
    return rc;
}
