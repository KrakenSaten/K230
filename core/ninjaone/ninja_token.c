/* Runtime token provisioning. Permissions only, not encryption.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "ninja_api.h"
#include "ninja_auth.h"
#include <cjson/cJSON.h>
#include <math.h>
#include <sys/file.h>
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
    if (!rc && unlinkat(dir, "forgotten", 0) && errno != ENOENT) rc = -1;
    if (!rc && fsync(dir)) rc = -1;
    if (rc) unlinkat(dir, "access-token.new", 0);
    close(dir); return rc;
}

static int bounded(const char *s, size_t n)
{
    return memchr(s, 0, n) && token_ok(s);
}
int ninja_credentials_valid(const struct ninja_credentials *c)
{
    if (!bounded(c->access, sizeof(c->access)) || !bounded(c->refresh, sizeof(c->refresh)) ||
        !bounded(c->client, sizeof(c->client)) || !memchr(c->base, 0, sizeof(c->base)) ||
        !ninja_base_valid(c->base) || c->profile[24] || strlen(c->profile) != 24 ||
        strspn(c->profile, "0123456789abcdef") != 24 || c->expires <= 0 ||
        c->expires > INT64_C(4102444800) || c->retry_at < 0 || c->retry_at > INT64_C(4102444800) ||
        c->attempts < 0 || c->attempts > 3) return 0;
    return 1;
}
int ninja_credentials_decode(struct ninja_credentials *c, const char *json, size_t n)
{
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, n + 1, &end, 1), *v;
    int rc = -1;
    memset(c, 0, sizeof(*c));
    if (!root || !cJSON_IsObject(root) || end != json + n) goto done;
    const char *keys[] = {"access_token", "refresh_token", "base", "client_id", "profile"};
    char *dst[] = {c->access, c->refresh, c->base, c->client, c->profile};
    size_t caps[] = {sizeof(c->access), sizeof(c->refresh), sizeof(c->base), sizeof(c->client), sizeof(c->profile)};
    for (unsigned i = 0; i < 5; i++) {
        v = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= caps[i]) goto done;
        memcpy(dst[i], v->valuestring, strlen(v->valuestring) + 1);
    }
    v = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble <= 0 ||
        v->valuedouble > 4102444800. || floor(v->valuedouble) != v->valuedouble) goto done;
    c->expires = (int64_t)v->valuedouble;
    if (ninja_credentials_valid(c)) rc = 0;
done:
    /* cJSON owns copies of token strings too. */
    if (root) for (v = root->child; v; v = v->next)
        if (v->valuestring) explicit_bzero(v->valuestring, strlen(v->valuestring));
    cJSON_Delete(root);
    if (rc) explicit_bzero(c, sizeof(*c));
    return rc;
}
int ninja_credentials_lock(void)
{
    char path[POCKETOS_PATH_MAX];
    int dir = directory(path, sizeof(path), 1);
    if (dir >= 0 && flock(dir, LOCK_EX | LOCK_NB)) { close(dir); return -1; }
    return dir;
}
int ninja_credentials_read(int dir, struct ninja_credentials *c)
{
    struct stat st;
    int fd = openat(dir, "credentials", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    memset(c, 0, sizeof(*c));
    if (fd < 0) return -1;
    int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
        (st.st_mode & 0777) == 0600 && st.st_nlink == 1 && st.st_size == sizeof(*c) &&
        read(fd, c, sizeof(*c)) == sizeof(*c) && ninja_credentials_valid(c);
    close(fd);
    if (!ok) explicit_bzero(c, sizeof(*c));
    return ok ? 0 : -1;
}
int ninja_credentials_write(int dir, const struct ninja_credentials *c)
{
    if (!ninja_credentials_valid(c)) return -1;
    int fd = openat(dir, "credentials.new", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int rc = -1;
    if (!fchmod(fd, 0600) && write(fd, c, sizeof(*c)) == sizeof(*c) && !fsync(fd) &&
        !renameat(dir, "credentials.new", dir, "credentials") && !fsync(dir)) rc = 0;
    close(fd);
    if (!rc && unlinkat(dir, "forgotten", 0) && errno != ENOENT) rc = -1;
    if (!rc && fsync(dir)) rc = -1;
    if (rc) unlinkat(dir, "credentials.new", 0);
    return rc;
}
int ninja_credentials_pending(int dir)
{
    struct stat st;
    /* Any unfinished rotation or staged write is uncertain: never replay an
     * old refresh token after a crash that may have consumed it remotely. */
    if (!fstatat(dir, "renewing", &st, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) return 1;
    return !fstatat(dir, "credentials.new", &st, AT_SYMLINK_NOFOLLOW) || errno != ENOENT;
}
int ninja_credentials_begin(int dir)
{
    int fd = openat(dir, "renewing", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int rc = fsync(fd) || fsync(dir);
    close(fd); return rc ? -1 : 0;
}
int ninja_credentials_finish(int dir)
{
    return unlinkat(dir, "renewing", 0) || fsync(dir) ? -1 : 0;
}
int ninja_credentials_forget(int dir)
{
    const char *names[] = {"credentials", "credentials.new", "renewing", "access-token", "access-token.new"};
    int rc = 0;
    for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); i++)
        if (unlinkat(dir, names[i], 0) && errno != ENOENT) rc = -1;
    int fd = openat(dir, "forgotten", O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) rc = -1;
    else { if (fsync(fd)) rc = -1; close(fd); }
    if (fsync(dir)) rc = -1;
    return rc;
}
