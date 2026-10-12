/* Runtime token provisioning. Permissions only, not encryption.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "ninja_api.h"
#include "pocketpaths.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int token_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n >= 4096) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
            !strchr("-._~+/=", c)) return 0;
    }
    return 1;
}
static int directory(char *path, size_t cap, int create)
{
    struct stat st;
    int fd;
    if (snprintf(path, cap, "%s/ninjaone", pocketos_runtime_dir()) >= (int)cap) return -1;
    if (create && mkdir(path, 0700) && errno != EEXIST) return -1;
    fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0700) {
        close(fd); return -1;
    }
    return fd;
}
int ninja_token_read(char *token, size_t cap)
{
    char path[POCKETOS_PATH_MAX];
    struct stat st;
    int dir = directory(path, sizeof(path), 0), fd;
    ssize_t n;
    memset(token, 0, cap);
    if (dir < 0) return -1;
    fd = openat(dir, "access-token", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    close(dir);
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 0777) != 0600 || st.st_nlink != 1 || st.st_size < 1 || st.st_size >= (off_t)cap) {
        close(fd); return -1;
    }
    n = read(fd, token, cap - 1); close(fd);
    if (n != st.st_size || !token_ok(token)) { explicit_bzero(token, cap); return -1; }
    return 0;
}
int ninja_token_write(const char *token)
{
    char path[POCKETOS_PATH_MAX];
    int dir, fd, rc = -1;
    size_t n;
    if (!token_ok(token)) return -1;
    dir = directory(path, sizeof(path), 1);
    if (dir < 0) return -1;
    fd = openat(dir, "access-token.new", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { close(dir); return -1; }
    n = strlen(token);
    if (!fchmod(fd, 0600) && write(fd, token, n) == (ssize_t)n && !fsync(fd) &&
        !renameat(dir, "access-token.new", dir, "access-token") && !fsync(dir)) rc = 0;
    close(fd);
    if (rc) unlinkat(dir, "access-token.new", 0);
    close(dir); return rc;
}
