/*
 * PocketFleet save file. See fleet_store.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "fleet_store.h"

#include "fleet_save.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORE_PATH_MAX 512

static char dir_buf[STORE_PATH_MAX];
static char path_buf[STORE_PATH_MAX + 32];

const char *fleet_store_dir(void)
{
    const char *base = getenv("POCKETOS_STATE_DIR");

    snprintf(dir_buf, sizeof(dir_buf), "%s/%s",
             (base && *base) ? base : FLEET_STORE_DEFAULT_DIR, FLEET_STORE_SUBDIR);
    return dir_buf;
}

const char *fleet_store_path(void)
{
    snprintf(path_buf, sizeof(path_buf), "%s/%s", fleet_store_dir(), FLEET_STORE_FILE);
    return path_buf;
}

/* Create every missing component of the save directory. Best effort: a
 * failure here surfaces as a failed write, which the caller already
 * tolerates. */
static void make_dirs(const char *dir)
{
    char work[STORE_PATH_MAX];
    size_t i;

    snprintf(work, sizeof(work), "%s", dir);
    for (i = 1; work[i]; i++) {
        if (work[i] != '/') {
            continue;
        }
        work[i] = '\0';
        mkdir(work, 0755);
        work[i] = '/';
    }
    mkdir(work, 0755);
}

int fleet_store_save(const struct fleet_game *game)
{
    uint8_t blob[FLEET_SAVE_SIZE];
    char tmp[sizeof(path_buf) + 8];
    const char *path;
    FILE *f;
    int written;

    if (!game) {
        return -1;
    }
    written = fleet_save_encode(game, blob, sizeof(blob));
    if (written < 0) {
        return -1;
    }
    make_dirs(fleet_store_dir());
    path = fleet_store_path();
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(blob, 1, (size_t)written, f) != (size_t)written ||
        fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int fleet_store_load(struct fleet_game *game)
{
    uint8_t blob[FLEET_SAVE_SIZE + 1];
    FILE *f;
    size_t got;

    if (!game) {
        return -1;
    }
    f = fopen(fleet_store_path(), "rb");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }
    got = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    /* A save is exactly FLEET_SAVE_SIZE bytes; anything else is truncated,
     * padded or from another version. */
    if (got != FLEET_SAVE_SIZE) {
        return -1;
    }
    return fleet_save_decode(game, blob, got);
}

int fleet_store_clear(void)
{
    if (unlink(fleet_store_path()) == 0 || errno == ENOENT) {
        return 0;
    }
    return -1;
}

int fleet_store_has_save(void)
{
    struct fleet_game probe;

    return fleet_store_load(&probe) == 0;
}

/* ---- multiplayer --------------------------------------------------------- */

static char match_buf[STORE_PATH_MAX + 32];

const char *fleet_store_match_path(void)
{
    snprintf(match_buf, sizeof(match_buf), "%s/%s", fleet_store_dir(), FLEET_STORE_MATCH_FILE);
    return match_buf;
}

int fleet_store_match_save(const uint8_t *blob, size_t n)
{
    char path[sizeof(match_buf)];
    char tmp[sizeof(match_buf) + 8];
    FILE *f;
    int dir;
    int rc;

    if (!blob || n == 0) {
        return -1;
    }
    make_dirs(fleet_store_dir());
    snprintf(path, sizeof(path), "%s", fleet_store_match_path());
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(blob, 1, n, f) != n || fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    /* The rename is only durable once the directory entry is. */
    dir = open(fleet_store_dir(), O_RDONLY | O_DIRECTORY);
    if (dir < 0) {
        return -1;
    }
    rc = fsync(dir);
    close(dir);
    return rc == 0 ? 0 : -1;
}

int fleet_store_match_load(uint8_t *buf, size_t max)
{
    FILE *f;
    size_t got;

    if (!buf || max == 0) {
        return -1;
    }
    f = fopen(fleet_store_match_path(), "rb");
    if (!f) {
        return errno == ENOENT ? 0 : -1;
    }
    got = fread(buf, 1, max, f);
    if (ferror(f) || (got == max && fgetc(f) != EOF)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return (int)got;
}
