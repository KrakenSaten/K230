/*
 * RIFT's map tiles, the helper's half: the disk cache and the rules for
 * one tile. See web_tiles.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "web/web_tiles.h"
#include "web/web_proto.h"
#include "web/web_url.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define META_MAGIC "doors-tile 1"
#define META_MAX 1024
/* What a .meta is counted as, beside its picture. */
#define META_BYTES 256u

bool web_tile_valid(int z, int x, int y)
{
    return z >= 0 && z <= WEB_TILE_ZOOM_MAX && x >= 0 && y >= 0 && x < (1 << z) && y < (1 << z);
}

/* ---- files ---------------------------------------------------------------------- */

static void path_of(const struct web_tile_cache *c, int z, int x, int y, const char *ext, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%d-%d-%d.%s", c->dir, z, x, y, ext);
}

static int read_file(const char *path, size_t max, char **out, size_t *len)
{
    struct stat st;
    char *buf;
    size_t got = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

    *out = NULL;
    *len = 0;
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size > max ||
        !(buf = malloc((size_t)st.st_size + 1))) {
        close(fd);
        return -1;
    }
    while (got < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);

        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    if (got != (size_t)st.st_size) {
        free(buf);
        return -1;
    }
    buf[got] = '\0';
    *out = buf;
    *len = got;
    return 0;
}

/* Write to a temporary name in the directory and rename it into place. */
static int write_file(const struct web_tile_cache *c, const char *path, const char *data, size_t len)
{
    char tmp[600];
    const char *p = data;
    size_t left = len;
    int fd;

    snprintf(tmp, sizeof(tmp), "%s/.tmp-%ld", c->dir, (long)getpid());
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    while (left > 0) {
        ssize_t w = write(fd, p, left);

        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            close(fd);
            unlink(tmp);
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    /* No fsync: this is a cache. A pair torn by a power cut does not agree
     * (the .meta names the picture's size) and is simply not a tile. */
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int format_meta(const struct web_tile_meta *m, char *out, size_t cap)
{
    int n = snprintf(out, cap, META_MAGIC "\nfetched %lld\nexpires %lld\nsize %zu\netag %s\nmodified %s\n",
                     (long long)m->fetched, (long long)m->expires, m->size, m->etag, m->last_modified);

    return n > 0 && (size_t)n < cap ? n : -1;
}

/* A line's value, if it is printable ASCII and fits. */
static bool value(const char *v, size_t n, char *out, size_t cap)
{
    size_t i;

    if (n >= cap) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if ((unsigned char)v[i] < 0x20 || (unsigned char)v[i] > 0x7e) {
            return false;
        }
    }
    memcpy(out, v, n);
    out[n] = '\0';
    return true;
}

static int parse_meta(const char *text, struct web_tile_meta *m)
{
    const char *p = text;
    int seen = 0;

    memset(m, 0, sizeof(*m));
    if (strncmp(p, META_MAGIC "\n", strlen(META_MAGIC) + 1) != 0) {
        return -1;
    }
    p += strlen(META_MAGIC) + 1;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char num[32];

        if (n > 8 && strncmp(p, "fetched ", 8) == 0 && value(p + 8, n - 8, num, sizeof(num))) {
            m->fetched = strtoll(num, NULL, 10);
            seen |= 1;
        } else if (n > 8 && strncmp(p, "expires ", 8) == 0 && value(p + 8, n - 8, num, sizeof(num))) {
            m->expires = strtoll(num, NULL, 10);
            seen |= 2;
        } else if (n > 5 && strncmp(p, "size ", 5) == 0 && value(p + 5, n - 5, num, sizeof(num))) {
            m->size = (size_t)strtoull(num, NULL, 10);
            seen |= 4;
        } else if (n >= 5 && strncmp(p, "etag ", 5) == 0) {
            if (!value(p + 5, n - 5, m->etag, sizeof(m->etag))) {
                m->etag[0] = '\0';
            }
        } else if (n >= 9 && strncmp(p, "modified ", 9) == 0) {
            if (!value(p + 9, n - 9, m->last_modified, sizeof(m->last_modified))) {
                m->last_modified[0] = '\0';
            }
        }
        p += n + (nl ? 1 : 0);
    }
    return seen == 7 ? 0 : -1;
}

/* "<z>-<x>-<y>.<ext>" with ext png or meta; the coordinates when it is. */
static bool tile_name(const char *name, int *z, int *x, int *y, bool *png)
{
    char ext[8];
    char check[64];

    if (sscanf(name, "%d-%d-%d.%7s", z, x, y, ext) != 4 || !web_tile_valid(*z, *x, *y)) {
        return false;
    }
    *png = strcmp(ext, "png") == 0;
    if (!*png && strcmp(ext, "meta") != 0) {
        return false;
    }
    snprintf(check, sizeof(check), "%d-%d-%d.%s", *z, *x, *y, ext);
    return strcmp(check, name) == 0;
}

/* ---- the cache -------------------------------------------------------------------- */

struct entry {
    int z;
    int x;
    int y;
    int64_t used;
    uint64_t bytes;
};

/* Every tile in the directory (pictures with a .meta beside them); the
 * rest - temporary files, a half of a pair, anything else of ours - is
 * removed. Returns the count, entries in *out (malloc'd), or -1. */
static int scan(struct web_tile_cache *c, struct entry **out)
{
    DIR *d = opendir(c->dir);
    struct dirent *e;
    struct entry *v = NULL;
    size_t n = 0;
    size_t cap = 0;
    int dfd;

    *out = NULL;
    if (!d) {
        return -1;
    }
    dfd = dirfd(d);
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        struct stat ms;
        char other[64];
        int z;
        int x;
        int y;
        bool png;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (strncmp(e->d_name, ".tmp-", 5) == 0) {
            unlinkat(dfd, e->d_name, 0);
            continue;
        }
        if (!tile_name(e->d_name, &z, &x, &y, &png)) {
            continue; /* not ours: left alone */
        }
        snprintf(other, sizeof(other), "%d-%d-%d.%s", z, x, y, png ? "meta" : "png");
        if (fstatat(dfd, other, &ms, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(ms.st_mode)) {
            unlinkat(dfd, e->d_name, 0); /* half a pair */
            continue;
        }
        if (!png) {
            continue; /* counted with its picture */
        }
        if (fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 256;
            struct entry *nv = realloc(v, nc * sizeof(*v));

            if (!nv) {
                break;
            }
            v = nv;
            cap = nc;
        }
        v[n].z = z;
        v[n].x = x;
        v[n].y = y;
        v[n].used = (int64_t)st.st_mtime;
        v[n].bytes = (uint64_t)st.st_size + META_BYTES;
        n++;
    }
    closedir(d);
    *out = v;
    return (int)n;
}

int web_tile_cache_open(struct web_tile_cache *c, const char *dir, uint64_t max_bytes, unsigned max_files)
{
    struct stat st;
    struct entry *v;
    int n;
    int i;

    memset(c, 0, sizeof(*c));
    c->max_bytes = max_bytes ? max_bytes : WEB_TILE_CACHE_BYTES;
    c->max_files = max_files ? max_files : WEB_TILE_CACHE_FILES;
    c->reserve = WEB_TILE_DISK_RESERVE;
    if (!dir || strlen(dir) + 64 >= sizeof(c->dir) || lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) ||
        (st.st_mode & 077) != 0 || st.st_uid != geteuid()) {
        return -1;
    }
    web_copy(c->dir, sizeof(c->dir), dir);
    n = scan(c, &v);
    if (n < 0) {
        c->dir[0] = '\0';
        return -1;
    }
    for (i = 0; i < n; i++) {
        c->bytes += v[i].bytes;
    }
    c->files = (unsigned)n;
    free(v);
    c->ok = true;
    if (c->bytes > c->max_bytes || c->files > c->max_files) {
        web_tile_cache_prune(c, 0, 0);
    }
    return 0;
}

int web_tile_cache_read(struct web_tile_cache *c, int z, int x, int y, struct web_tile_meta *m, char **body,
                        size_t *len)
{
    char path[600];
    char *text;
    size_t tlen;

    *body = NULL;
    *len = 0;
    if (!c->ok || !web_tile_valid(z, x, y)) {
        return -1;
    }
    path_of(c, z, x, y, "meta", path, sizeof(path));
    if (read_file(path, META_MAX, &text, &tlen) != 0) {
        return -1;
    }
    if (parse_meta(text, m) != 0) {
        free(text);
        web_tile_cache_remove(c, z, x, y);
        return -1;
    }
    free(text);
    path_of(c, z, x, y, "png", path, sizeof(path));
    if (read_file(path, WEB_TILE_BYTES_MAX, body, len) != 0 || *len != m->size || *len == 0) {
        free(*body);
        *body = NULL;
        *len = 0;
        web_tile_cache_remove(c, z, x, y);
        return -1;
    }
    return 0;
}

static bool room_on_disk(const struct web_tile_cache *c, size_t len)
{
    struct statvfs fs;

    if (statvfs(c->dir, &fs) != 0) {
        return false;
    }
    return (uint64_t)fs.f_bavail * (uint64_t)fs.f_frsize >= c->reserve + len + META_BYTES;
}

int web_tile_cache_write(struct web_tile_cache *c, int z, int x, int y, const struct web_tile_meta *m,
                         const char *body, size_t len)
{
    char path[600];
    char meta[META_MAX];
    struct web_tile_meta mm = *m;
    struct stat st;
    bool had;
    int n;

    if (!c->ok || !web_tile_valid(z, x, y) || len == 0 || len > WEB_TILE_BYTES_MAX) {
        return -1;
    }
    path_of(c, z, x, y, "png", path, sizeof(path));
    had = lstat(path, &st) == 0 && S_ISREG(st.st_mode);
    if (had) {
        /* The old pair goes first: a new picture beside the old .meta would
         * be a pair that agrees by accident. */
        web_tile_cache_remove(c, z, x, y);
    }
    if (!room_on_disk(c, len)) {
        return -1;
    }
    mm.size = len;
    n = format_meta(&mm, meta, sizeof(meta));
    if (n < 0 || write_file(c, path, body, len) != 0) {
        return -1;
    }
    path_of(c, z, x, y, "meta", path, sizeof(path));
    if (write_file(c, path, meta, (size_t)n) != 0) {
        path_of(c, z, x, y, "png", path, sizeof(path));
        unlink(path);
        return -1;
    }
    c->bytes += (uint64_t)len + META_BYTES;
    c->files++;
    if (c->bytes > c->max_bytes || c->files > c->max_files) {
        web_tile_cache_prune(c, 0, 0);
    }
    return 0;
}

int web_tile_cache_renew(struct web_tile_cache *c, int z, int x, int y, const struct web_tile_meta *m)
{
    char path[600];
    char meta[META_MAX];
    int n;

    if (!c->ok || !web_tile_valid(z, x, y)) {
        return -1;
    }
    n = format_meta(m, meta, sizeof(meta));
    path_of(c, z, x, y, "meta", path, sizeof(path));
    if (n < 0 || write_file(c, path, meta, (size_t)n) != 0) {
        return -1;
    }
    web_tile_cache_touch(c, z, x, y);
    return 0;
}

void web_tile_cache_touch(struct web_tile_cache *c, int z, int x, int y)
{
    char path[600];

    if (!c->ok || !web_tile_valid(z, x, y)) {
        return;
    }
    path_of(c, z, x, y, "png", path, sizeof(path));
    utimensat(AT_FDCWD, path, NULL, AT_SYMLINK_NOFOLLOW);
}

void web_tile_cache_remove(struct web_tile_cache *c, int z, int x, int y)
{
    char path[600];
    struct stat st;

    if (!c->dir[0] || !web_tile_valid(z, x, y)) {
        return;
    }
    path_of(c, z, x, y, "png", path, sizeof(path));
    if (lstat(path, &st) == 0) {
        uint64_t b = (S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0) + META_BYTES;

        if (unlink(path) == 0 && c->files > 0) {
            c->files--;
            c->bytes = c->bytes > b ? c->bytes - b : 0;
        }
    }
    path_of(c, z, x, y, "meta", path, sizeof(path));
    unlink(path);
}

static int by_use(const void *a, const void *b)
{
    const struct entry *ea = a;
    const struct entry *eb = b;

    return ea->used < eb->used ? -1 : ea->used > eb->used ? 1 : 0;
}

void web_tile_cache_prune(struct web_tile_cache *c, uint64_t keep_bytes, unsigned keep_files)
{
    struct entry *v;
    uint64_t bytes = 0;
    unsigned files;
    int n;
    int i;

    if (!c->ok) {
        return;
    }
    if (!keep_bytes) {
        keep_bytes = c->max_bytes - c->max_bytes / 10;
    }
    if (!keep_files) {
        keep_files = c->max_files - c->max_files / 10;
    }
    n = scan(c, &v);
    if (n < 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        bytes += v[i].bytes;
    }
    files = (unsigned)n;
    qsort(v, (size_t)n, sizeof(*v), by_use);
    for (i = 0; i < n && (bytes > keep_bytes || files > keep_files); i++) {
        char path[600];

        path_of(c, v[i].z, v[i].x, v[i].y, "png", path, sizeof(path));
        unlink(path);
        path_of(c, v[i].z, v[i].x, v[i].y, "meta", path, sizeof(path));
        unlink(path);
        bytes -= v[i].bytes;
        files--;
        c->pruned++;
    }
    free(v);
    c->bytes = bytes;
    c->files = files;
}

/* ---- freshness ------------------------------------------------------------------- */

static int lower(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
}

/* Whether the comma-separated Cache-Control list has this directive; its
 * "=value" into *arg when it has one. */
static bool directive(const char *cc, const char *name, long long *arg)
{
    const char *p = cc;
    size_t k = strlen(name);

    while (p && *p) {
        size_t i;

        while (*p == ' ' || *p == ',' || *p == '\t') {
            p++;
        }
        for (i = 0; i < k && p[i] && lower((unsigned char)p[i]) == name[i]; i++) {
        }
        if (i == k && (p[k] == '\0' || p[k] == ',' || p[k] == ' ' || p[k] == '=' || p[k] == '\t')) {
            if (arg) {
                const char *q = p + k;

                *arg = -1;
                while (*q == ' ') {
                    q++;
                }
                if (*q == '=') {
                    char *end;
                    long long v;

                    q++;
                    if (*q == '"') {
                        q++;
                    }
                    v = strtoll(q, &end, 10);
                    if (end != q && v >= 0) {
                        *arg = v;
                    }
                }
            }
            return true;
        }
        p = strchr(p, ',');
    }
    return false;
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    int64_t era;
    unsigned yoe;
    unsigned doy;
    unsigned doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int month_of(const char *s)
{
    static const char *const names[12] = { "jan", "feb", "mar", "apr", "may", "jun",
                                           "jul", "aug", "sep", "oct", "nov", "dec" };
    int i;

    for (i = 0; i < 12; i++) {
        if (lower((unsigned char)s[0]) == names[i][0] && lower((unsigned char)s[1]) == names[i][1] &&
            lower((unsigned char)s[2]) == names[i][2]) {
            return i + 1;
        }
    }
    return 0;
}

int64_t web_http_date(const char *s)
{
    char mon[8];
    int d;
    int y;
    int hh;
    int mm;
    int ss;
    int m;
    const char *comma;

    if (!s || !*s) {
        return -1;
    }
    comma = strchr(s, ',');
    if (comma) {
        /* "Sun, 06 Nov 1994 08:49:37 GMT" or "Sunday, 06-Nov-94 08:49:37 GMT" */
        if (sscanf(comma + 1, " %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6 &&
            sscanf(comma + 1, " %d-%3s-%d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) {
            return -1;
        }
        if (y < 100) {
            y += y < 70 ? 2000 : 1900;
        }
    } else if (sscanf(s, "%*3s %3s %d %d:%d:%d %d", mon, &d, &hh, &mm, &ss, &y) != 6) {
        return -1; /* "Sun Nov  6 08:49:37 1994" */
    }
    m = month_of(mon);
    if (!m || d < 1 || d > 31 || y < 1970 || y > 9999 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 ||
        ss > 60) {
        return -1;
    }
    return days_from_civil(y, (unsigned)m, (unsigned)d) * 86400 + hh * 3600 + mm * 60 + ss;
}

int64_t web_tile_expiry(const struct web_cache_hdrs *h, int64_t now, bool *store)
{
    long long age = -1;

    *store = true;
    if (h->cache_control[0]) {
        if (directive(h->cache_control, "no-store", NULL)) {
            *store = false;
            return now;
        }
        if (directive(h->cache_control, "no-cache", NULL)) {
            return now;
        }
        if (directive(h->cache_control, "max-age", &age) && age >= 0) {
            return now + (age > WEB_TILE_MAX_AGE_S ? WEB_TILE_MAX_AGE_S : age);
        }
    }
    if (h->expires[0]) {
        int64_t exp = web_http_date(h->expires);
        int64_t date = web_http_date(h->date);

        if (exp < 0) {
            return now; /* "0" or anything unreadable: already expired */
        }
        /* Read against the server's own clock where it said what that was:
         * this board's may be off by minutes after NTP, or by more. */
        age = (long long)(exp - (date >= 0 ? date : now));
        if (age < 0) {
            age = 0;
        }
        return now + (age > WEB_TILE_MAX_AGE_S ? WEB_TILE_MAX_AGE_S : age);
    }
    return now + WEB_TILE_FALLBACK_S;
}

/* ---- one tile ---------------------------------------------------------------------- */

int web_tile_url(const char *tmpl, int z, int x, int y, char *out, size_t cap)
{
    size_t o = 0;
    const char *p;
    int seen = 0;

    if (!tmpl || strncmp(tmpl, "https://", 8) != 0 || !web_tile_valid(z, x, y)) {
        return -1;
    }
    for (p = tmpl; *p; p++) {
        int v = -1;
        char num[16];
        int k;

        if (strncmp(p, "{z}", 3) == 0) {
            v = z;
            seen |= 1;
        } else if (strncmp(p, "{x}", 3) == 0) {
            v = x;
            seen |= 2;
        } else if (strncmp(p, "{y}", 3) == 0) {
            v = y;
            seen |= 4;
        }
        if (v >= 0) {
            k = snprintf(num, sizeof(num), "%d", v);
            if (o + (size_t)k + 1 > cap) {
                return -1;
            }
            memcpy(out + o, num, (size_t)k);
            o += (size_t)k;
            p += 2;
            continue;
        }
        if ((unsigned char)*p <= 0x20 || (unsigned char)*p > 0x7e || o + 2 > cap) {
            return -1;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
    return seen == 7 ? (int)o : -1;
}

void web_tile_result_free(struct web_tile_result *r)
{
    if (r) {
        free(r->body);
        r->body = NULL;
        r->len = 0;
    }
}

static void why(struct web_tile_result *r, const char *s)
{
    web_copy(r->why, sizeof(r->why), s);
}

/* The kept copy as the answer, expired, with why. */
static void stale(struct web_tile_result *r, char *body, size_t len, const char *reason)
{
    r->source = WEB_TILE_STALE;
    r->body = body;
    r->len = len;
    why(r, reason);
}

static void none(struct web_tile_result *r, char *body, const char *reason)
{
    free(body);
    r->source = WEB_TILE_NONE;
    why(r, reason);
}

void web_tile_get(struct web_tile_cache *c, struct web_tile_net *net, int z, int x, int y, int64_t now,
                  struct web_tile_result *r)
{
    struct web_tile_meta m;
    struct web_fetch_req req;
    struct web_fetch_resp resp;
    char url[WEB_URL_MAX];
    char *body = NULL;
    size_t len = 0;
    bool have;
    bool clock_ok = now >= WEB_TILE_CLOCK_MIN;

    memset(r, 0, sizeof(*r));
    why(r, "-");
    if (!web_tile_valid(z, x, y)) {
        none(r, NULL, "range");
        return;
    }
    have = web_tile_cache_read(c, z, x, y, &m, &body, &len) == 0;
    if (have && clock_ok && m.expires > now) {
        web_tile_cache_touch(c, z, x, y);
        r->source = WEB_TILE_CACHE;
        r->body = body;
        r->len = len;
        return;
    }
    /* With no clock nothing can be asked: a certificate cannot be checked,
     * and an expiry cannot be read. The kept copy is shown as it is. */
    if (!clock_ok) {
        if (have) {
            web_tile_cache_touch(c, z, x, y);
            stale(r, body, len, "clock");
        } else {
            none(r, body, "clock");
        }
        return;
    }
    if (!net || !net->f || !net->allow) {
        const char *w = net && net->why_not[0] ? net->why_not : "offline";

        if (have) {
            stale(r, body, len, w);
        } else {
            none(r, body, w);
        }
        return;
    }
    if (web_tile_url(net->url_template, z, x, y, url, sizeof(url)) < 0) {
        none(r, body, "url");
        return;
    }
    memset(&req, 0, sizeof(req));
    req.url = url;
    req.max_bytes = WEB_TILE_BYTES_MAX;
    req.keep_cut = false;
    req.accept = "image/png,image/jpeg;q=0.9";
    req.ca_file = net->ca_file;
    req.connect_timeout_ms = WEB_TILE_CONNECT_TIMEOUT_MS;
    req.timeout_ms = WEB_TILE_TIMEOUT_MS;
    req.user_agent = net->user_agent;
    req.abort_cb = net->abort_cb;
    req.ctx = net->ctx;
    if (have) {
        req.if_none_match = m.etag;
        req.if_modified_since = m.last_modified;
    }
    r->asked = true;
    if (web_fetch(net->f, &req, &resp) != 0) {
        r->fail = resp.fail;
        web_fetch_resp_free(&resp);
        if (resp.fail == WEB_FAIL_STOPPED) {
            r->stopped = true;
            none(r, body, "stopped");
        } else if (have) {
            stale(r, body, len, web_fail_name(resp.fail));
        } else {
            none(r, body, web_fail_name(resp.fail));
        }
        return;
    }
    r->http = resp.status;
    if (resp.status == 304 && have) {
        bool store;
        struct web_tile_meta nm = m;

        nm.fetched = now;
        nm.expires = web_tile_expiry(&resp.cache, now, &store);
        /* A 304 may carry new validators; it may also carry none. */
        if (resp.cache.etag[0]) {
            web_copy(nm.etag, sizeof(nm.etag), resp.cache.etag);
        }
        if (resp.cache.last_modified[0]) {
            web_copy(nm.last_modified, sizeof(nm.last_modified), resp.cache.last_modified);
        }
        web_fetch_resp_free(&resp);
        if (store) {
            web_tile_cache_renew(c, z, x, y, &nm);
        } else {
            web_tile_cache_remove(c, z, x, y);
        }
        r->source = WEB_TILE_VALID;
        r->body = body;
        r->len = len;
        return;
    }
    if (resp.status == 200 && resp.len > 0) {
        bool store;
        struct web_tile_meta nm;

        memset(&nm, 0, sizeof(nm));
        nm.fetched = now;
        nm.expires = web_tile_expiry(&resp.cache, now, &store);
        web_copy(nm.etag, sizeof(nm.etag), resp.cache.etag);
        web_copy(nm.last_modified, sizeof(nm.last_modified), resp.cache.last_modified);
        free(body);
        if (store) {
            web_tile_cache_write(c, z, x, y, &nm, resp.body, resp.len);
        } else {
            web_tile_cache_remove(c, z, x, y);
        }
        r->source = WEB_TILE_NET;
        r->body = resp.body; /* handed over */
        r->len = resp.len;
        return;
    }
    {
        char w[24];

        snprintf(w, sizeof(w), "http-%ld", resp.status > 0 && resp.status < 1000 ? resp.status : 0);
        web_fetch_resp_free(&resp);
        if (have) {
            stale(r, body, len, w);
        } else {
            none(r, body, w);
        }
    }
}
