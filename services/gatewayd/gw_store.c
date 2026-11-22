/*
 * gatewayd: the paired clients. See gw_store.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gw_store.h"

#include "gw_policy.h"
#include "gw_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HEADER "doors-gatewayd-paired 1"
#define FILE_MAX 8192

int gw_store_dir(const char *dir)
{
    struct stat sb;

    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        return -1;
    }
    if (lstat(dir, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        return -1;
    }
    /* A directory made by hand or restored from a backup is not left open. */
    if ((sb.st_mode & 0777) != 0700 && chmod(dir, 0700) != 0) {
        return -1;
    }
    return 0;
}

static int path_of(char *out, size_t n, const char *dir, const char *name)
{
    int w = snprintf(out, n, "%s/%s", dir, name);

    return (w > 0 && (size_t)w < n) ? 0 : -1;
}

bool gw_store_exists(const char *dir)
{
    char path[GW_STORE_PATH_MAX];
    struct stat sb;

    return path_of(path, sizeof(path), dir, GW_STORE_FILE) == 0 && lstat(path, &sb) == 0;
}

/* One "client ..." line into *c. 0, or -1. */
static int parse_client(char *line, struct gw_client *c)
{
    char *save = NULL;
    char *word = strtok_r(line, " ", &save);
    char *fp = strtok_r(NULL, " ", &save);
    char *scopes = strtok_r(NULL, " ", &save);
    char *wall = strtok_r(NULL, " ", &save);
    char *name = strtok_r(NULL, " ", &save);
    char *end;
    unsigned long sc;
    long long w;
    size_t nlen;

    if (word == NULL || strcmp(word, "client") != 0 || fp == NULL || scopes == NULL || wall == NULL ||
        name == NULL || strtok_r(NULL, " ", &save) != NULL) {
        return -1;
    }
    memset(c, 0, sizeof(*c));
    if (gw_unhex(fp, c->fp, GW_FP_LEN) != 0) {
        return -1;
    }
    errno = 0;
    sc = strtoul(scopes, &end, 16);
    if (errno != 0 || *end != '\0' || end == scopes || !gw_scopes_valid((unsigned)sc) || sc > GW_S_ALL) {
        return -1;
    }
    c->scopes = (unsigned)sc;
    errno = 0;
    w = strtoll(wall, &end, 10);
    if (errno != 0 || *end != '\0' || end == wall || w < 0) {
        return -1;
    }
    c->paired_wall_s = (int64_t)w;
    nlen = strlen(name);
    if (nlen < 2 || nlen > 2 * GW_NAME_MAX || nlen % 2 != 0 ||
        gw_unhex(name, (uint8_t *)c->name, nlen / 2) != 0) {
        return -1;
    }
    c->name[nlen / 2] = '\0';
    /* A NUL inside the decoded name would hide the rest of it. */
    return (strlen(c->name) == nlen / 2 && gw_name_ok(c->name)) ? 0 : -1;
}

int gw_store_load(struct gw_store *st, const char *dir)
{
    char path[GW_STORE_PATH_MAX];
    char buf[FILE_MAX + 1];
    char *line, *save = NULL;
    struct stat sb;
    ssize_t got;
    size_t len = 0;
    int fd, first = 1;

    memset(st, 0, sizeof(*st));
    if (strlen(dir) >= sizeof(st->dir) || path_of(path, sizeof(path), dir, GW_STORE_FILE) != 0) {
        return -1;
    }
    memcpy(st->dir, dir, strlen(dir) + 1);
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? 0 : -1;
    }
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size > FILE_MAX) {
        close(fd);
        return -1;
    }
    while (len < FILE_MAX) {
        got = read(fd, buf + len, FILE_MAX - len);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (got == 0) {
            break;
        }
        len += (size_t)got;
    }
    close(fd);
    /* Text ending in a newline, with no NUL: anything else is damage. */
    if (len == 0 || buf[len - 1] != '\n' || memchr(buf, '\0', len) != NULL) {
        return -1;
    }
    buf[len] = '\0';
    for (line = strtok_r(buf, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        if (first) {
            if (strcmp(line, HEADER) != 0) {
                goto bad;
            }
            first = 0;
            continue;
        }
        if (st->n >= GW_MAX_CLIENTS || parse_client(line, &st->c[st->n]) != 0 ||
            gw_store_find(st, st->c[st->n].fp) >= 0) {
            goto bad;
        }
        st->n++;
    }
    if (first) {
        goto bad;
    }
    return 0;
bad:
    st->n = 0;
    memset(st->c, 0, sizeof(st->c));
    return -1;
}

static int write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int gw_store_save(const struct gw_store *st)
{
    char path[GW_STORE_PATH_MAX], tmp[GW_STORE_PATH_MAX];
    char buf[FILE_MAX];
    size_t len = 0;
    int i, fd, dfd, w;

    if (path_of(path, sizeof(path), st->dir, GW_STORE_FILE) != 0 ||
        path_of(tmp, sizeof(tmp), st->dir, "." GW_STORE_FILE ".tmp") != 0) {
        return -1;
    }
    w = snprintf(buf, sizeof(buf), "%s\n", HEADER);
    len = (size_t)w;
    for (i = 0; i < st->n; i++) {
        char fp[GW_FP_HEX + 1], name[2 * GW_NAME_MAX + 1];

        gw_hex(st->c[i].fp, GW_FP_LEN, fp);
        gw_hex((const uint8_t *)st->c[i].name, strlen(st->c[i].name), name);
        w = snprintf(buf + len, sizeof(buf) - len, "client %s %x %lld %s\n", fp, st->c[i].scopes,
                     (long long)st->c[i].paired_wall_s, name);
        if (w < 0 || (size_t)w >= sizeof(buf) - len) {
            return -1;
        }
        len += (size_t)w;
    }
    (void)unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    if (write_all(fd, buf, len) != 0 || fsync(fd) != 0) {
        close(fd);
        (void)unlink(tmp);
        return -1;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return -1;
    }
    /* The rename is not on storage until the directory is. */
    dfd = open(st->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        return -1;
    }
    w = fsync(dfd);
    close(dfd);
    return w == 0 ? 0 : -1;
}

int gw_store_find(const struct gw_store *st, const uint8_t fp[GW_FP_LEN])
{
    int i, found = -1;

    for (i = 0; i < st->n; i++) {
        if (gw_ct_eq(st->c[i].fp, fp, GW_FP_LEN)) {
            found = i;
        }
    }
    return found;
}

int gw_store_add(struct gw_store *st, const uint8_t fp[GW_FP_LEN], const char *name, unsigned scopes,
                 int64_t wall_s)
{
    struct gw_client *c;

    if (st->n >= GW_MAX_CLIENTS || gw_store_find(st, fp) >= 0 || !gw_name_ok(name) ||
        !gw_scopes_valid(scopes)) {
        return -1;
    }
    c = &st->c[st->n];
    memset(c, 0, sizeof(*c));
    memcpy(c->fp, fp, GW_FP_LEN);
    memcpy(c->name, name, strlen(name) + 1);
    c->scopes = scopes;
    c->paired_wall_s = wall_s > 0 ? wall_s : 0;
    st->n++;
    return 0;
}

void gw_store_remove(struct gw_store *st, int idx)
{
    if (idx < 0 || idx >= st->n) {
        return;
    }
    memmove(&st->c[idx], &st->c[idx + 1], (size_t)(st->n - idx - 1) * sizeof(st->c[0]));
    st->n--;
    memset(&st->c[st->n], 0, sizeof(st->c[0]));
}
