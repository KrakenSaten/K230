/*
 * The owner's choice for the radio. See rf_state.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "rf_state.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int rf_state_path(const char *state_dir, char *out, size_t len)
{
    int n;

    if (!state_dir || !out || len == 0) {
        return -1;
    }
    n = snprintf(out, len, "%s/%s/%s", state_dir, RF_STATE_DIR, RF_STATE_FILE);
    return (n < 0 || (size_t)n >= len) ? -1 : 0;
}

enum rf_state_load rf_state_load(const char *path, char *backend, size_t blen)
{
    FILE *f = fopen(path, "r");
    char line[128];
    enum rf_state_load found = RF_STATE_INVALID;

    if (backend && blen > 0) {
        backend[0] = '\0';
    }
    if (!f) {
        return errno == ENOENT ? RF_STATE_ABSENT : RF_STATE_INVALID;
    }
    /* The last well-formed enabled= line wins, as in every key=value store
     * here; anything else on a line of its own is ignored. A file with no
     * such line - empty, truncated by hand, overwritten with something else -
     * says nothing about the owner's choice and is reported as invalid, so
     * the caller falls back to the default rather than guessing one. */
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);

        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) {
            line[--n] = '\0';
        }
        if (strcmp(line, "enabled=0") == 0) {
            found = RF_STATE_OFF;
        } else if (strcmp(line, "enabled=1") == 0) {
            found = RF_STATE_ON;
        } else if (strncmp(line, "backend=", 8) == 0 && backend && blen > 0) {
            snprintf(backend, blen, "%s", line + 8);
        }
    }
    fclose(f);
    return found;
}

bool rf_state_applies(enum rf_state_load stored, const char *stored_backend, const char *backend)
{
    if (stored != RF_STATE_ON && stored != RF_STATE_OFF) {
        return false;
    }
    if (stored_backend && stored_backend[0]) {
        return backend && strcmp(stored_backend, backend) == 0;
    }
    /* Untagged (before 0.3.5): off is always safe to keep; on only for the
     * mock, which transmits nothing. */
    return stored == RF_STATE_OFF || (backend && strcmp(backend, "mock") == 0);
}

int rf_state_store(const char *path, bool enabled, const char *backend, char *err, size_t errlen)
{
    char dir[512];
    char tmp[576];
    char body[192];
    char *slash;
    int fd;
    int n;
    size_t blen;

    n = snprintf(body, sizeof(body),
                 "# radiod: the owner's radio on/off choice (radio.set_enabled), on this backend\n"
                 "enabled=%d\nbackend=%s\n",
                 enabled ? 1 : 0, backend ? backend : "");
    if (n < 0 || (size_t)n >= sizeof(body) || !backend || !backend[0] || strchr(backend, '\n')) {
        snprintf(err, errlen, "no usable backend name");
        return -EINVAL;
    }
    blen = (size_t)n;

    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir) ||
        snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        snprintf(err, errlen, "path too long");
        return -ENAMETOOLONG;
    }
    slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
            n = errno;
            snprintf(err, errlen, "cannot create %s: %s", dir, strerror(n));
            return -n;
        }
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        n = errno;
        snprintf(err, errlen, "cannot write %s: %s", tmp, strerror(n));
        return -n;
    }
    if (write(fd, body, blen) != (ssize_t)blen || fsync(fd) < 0) {
        n = errno ? errno : EIO;
        close(fd);
        unlink(tmp);
        snprintf(err, errlen, "cannot write %s: %s", tmp, strerror(n));
        return -n;
    }
    if (close(fd) < 0 || rename(tmp, path) < 0) {
        n = errno;
        unlink(tmp);
        snprintf(err, errlen, "cannot replace %s: %s", path, strerror(n));
        return -n;
    }
    /* The rename is only durable once the directory entry is. */
    if (slash && slash != dir) {
        fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
    }
    return 0;
}
