/*
 * Wave persistence. See wave_store.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "wave_store.h"

#include "pocketpaths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int join(char *out, size_t n, const char *dir, const char *name)
{
    int w = snprintf(out, n, "%s/%s", dir, name);

    return w < 0 || (size_t)w >= n ? -1 : 0;
}

int wave_store_dir(char *out, size_t n)
{
    return join(out, n, pocketos_state_dir(), WAVE_STORE_SUBDIR);
}

static int file_path(char *out, size_t n, const char *name)
{
    char dir[POCKETOS_PATH_MAX];

    return wave_store_dir(dir, sizeof(dir)) == 0 ? join(out, n, dir, name) : -1;
}

/* The whole file, NUL-terminated, into buf. Its length, -2 when there is no
 * file, -1 when it cannot be read or is larger than the buffer. */
static long read_file(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    size_t got = 0;

    if (fd < 0) {
        return errno == ENOENT ? -2 : -1;
    }
    for (;;) {
        ssize_t r;

        if (got + 1 >= n) {
            char probe;

            /* Full: a file exactly this long is fine, a longer one is not. */
            r = read(fd, &probe, 1);
            close(fd);
            return r == 0 ? (buf[got] = '\0', (long)got) : -1;
        }
        r = read(fd, buf + got, n - 1 - got);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (r == 0) {
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    return (long)got;
}

/* Temporary file, fsync, rename; 0600 in a 0700 directory. */
static int write_file(const char *name, const char *data, size_t len)
{
    char dir[POCKETOS_PATH_MAX];
    char path[POCKETOS_PATH_MAX];
    char tmp[POCKETOS_PATH_MAX];
    size_t off = 0;
    int fd;

    if (wave_store_dir(dir, sizeof(dir)) != 0 || join(path, sizeof(path), dir, name) != 0 ||
        snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        return -1;
    }
    if (pocketos_mkdir_p(dir, 0700) != 0) {
        return -1;
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);

        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---- preferences -------------------------------------------------------- */

int wave_store_load_prefs(struct wave_prefs *p)
{
    char path[POCKETOS_PATH_MAX];
    char buf[1024];
    char *line;
    char *save = NULL;
    long n;

    memset(p, 0, sizeof(*p));
    if (file_path(path, sizeof(path), WAVE_PREFS_FILE) != 0) {
        return -1;
    }
    n = read_file(path, buf, sizeof(buf));
    if (n == -2) {
        return 1;
    }
    if (n < 0) {
        return -1;
    }
    line = strtok_r(buf, "\n", &save);
    if (!line || strcmp(line, WAVE_PREFS_MAGIC) != 0) {
        return -1;
    }
    while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
        if (strncmp(line, "preset=", 7) == 0 && strlen(line + 7) < sizeof(p->preset)) {
            snprintf(p->preset, sizeof(p->preset), "%s", line + 7);
        }
    }
    return 0;
}

int wave_store_save_prefs(const struct wave_prefs *p)
{
    char buf[128];
    int w = snprintf(buf, sizeof(buf), "%s\npreset=%s\n", WAVE_PREFS_MAGIC, p->preset);

    if (w < 0 || (size_t)w >= sizeof(buf) || strchr(p->preset, '\n')) {
        return -1;
    }
    return write_file(WAVE_PREFS_FILE, buf, (size_t)w);
}

/* ---- history ------------------------------------------------------------ */

int wave_store_load_history(struct wave_history *h, int *skipped)
{
    char path[POCKETOS_PATH_MAX];
    char *buf;
    long n;
    int rc;

    wave_history_init(h);
    if (skipped) {
        *skipped = 0;
    }
    if (file_path(path, sizeof(path), WAVE_HISTORY_FILE) != 0) {
        return -1;
    }
    buf = malloc(WAVE_STORE_FILE_MAX);
    if (!buf) {
        return -1;
    }
    n = read_file(path, buf, WAVE_STORE_FILE_MAX);
    if (n == -2) {
        free(buf);
        return 1;
    }
    rc = n < 0 ? -1 : wave_history_parse(h, buf, (size_t)n, skipped);
    free(buf);
    return rc;
}

int wave_store_save_history(const struct wave_history *h)
{
    char path[POCKETOS_PATH_MAX];
    char *buf;
    long n;
    int rc;

    if (wave_history_count(h) == 0) {
        if (file_path(path, sizeof(path), WAVE_HISTORY_FILE) != 0) {
            return -1;
        }
        return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
    }
    buf = malloc(WAVE_HISTORY_TEXT_MAX);
    if (!buf) {
        return -1;
    }
    n = wave_history_format(h, buf, WAVE_HISTORY_TEXT_MAX);
    rc = n < 0 ? -1 : write_file(WAVE_HISTORY_FILE, buf, (size_t)n);
    free(buf);
    return rc;
}

/* ---- the capture -------------------------------------------------------- */

static int capture_dir(char *out, size_t n)
{
    return join(out, n, pocketos_runtime_dir(), WAVE_STORE_SUBDIR);
}

int wave_store_capture_path(char *out, size_t n)
{
    char dir[POCKETOS_PATH_MAX];

    if (capture_dir(dir, sizeof(dir)) != 0 || pocketos_mkdir_p(dir, 0700) != 0) {
        return -1;
    }
    return join(out, n, dir, WAVE_CAPTURE_FILE);
}

void wave_store_capture_remove(void)
{
    char dir[POCKETOS_PATH_MAX];
    char path[POCKETOS_PATH_MAX];

    if (capture_dir(dir, sizeof(dir)) == 0 && join(path, sizeof(path), dir, WAVE_CAPTURE_FILE) == 0) {
        unlink(path);
    }
}
