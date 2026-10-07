/*
 * Vision's files. See vision_store.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_store.h"

#include "pocketpaths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir_buf[POCKETOS_PATH_MAX];

const char *vision_store_dir(void)
{
    snprintf(dir_buf, sizeof(dir_buf), "%s/%s", pocketos_state_dir(), VISION_STORE_SUBDIR);
    return dir_buf;
}

static void path_of(const char *file, char *out, size_t len)
{
    snprintf(out, len, "%s/%s", vision_store_dir(), file);
}

int vision_store_load(struct vision_settings *s)
{
    char path[POCKETOS_PATH_MAX + 32];
    char text[VISION_SETTINGS_TEXT_MAX * 2];
    size_t got;
    FILE *f;

    if (!s) {
        return -1;
    }
    vision_settings_defaults(s);
    path_of(VISION_STORE_SETTINGS, path, sizeof(path));
    f = fopen(path, "r");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }
    got = fread(text, 1, sizeof(text) - 1, f);
    /* A file longer than this build ever writes is not one it wrote. */
    if (ferror(f) || (got == sizeof(text) - 1 && fgetc(f) != EOF)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    text[got] = '\0';
    vision_settings_parse(s, text);
    return 0;
}

int vision_store_save(const struct vision_settings *s)
{
    char path[POCKETOS_PATH_MAX + 32];
    char tmp[POCKETOS_PATH_MAX + 40];
    char text[VISION_SETTINGS_TEXT_MAX];
    int n;
    int fd;

    if (!s) {
        return -1;
    }
    n = vision_settings_format(s, text, sizeof(text));
    if (n < 0 || pocketos_mkdir_p(vision_store_dir(), 0700) != 0) {
        return -1;
    }
    path_of(VISION_STORE_SETTINGS, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    if (write(fd, text, (size_t)n) != (ssize_t)n || fsync(fd) != 0) {
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
