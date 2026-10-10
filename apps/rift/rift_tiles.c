/*
 * MAP's basemap client. See rift_tiles.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "rift_tiles.h"

#include "pocketlog/pocketlog.h"
#include "pocketpaths.h"
#include "web/web_proto.h"
#include "web/web_tiles.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HELPER_DEFAULT "/usr/bin/pos-browser"
#define CHILD_FD_SCAN_MAX 1024
#define TILE_BYTES ((size_t)RIFT_MAP_TILE_PX * RIFT_MAP_TILE_PX * sizeof(uint16_t))
/* rift_tiles_stop: from quit to SIGKILL, and after it, in naps. */
#define QUIT_NAPS 40
#define KILL_NAPS 10
#define NAP_NS (5 * 1000000L)
/* Lines read per poll, so a burst never holds a frame. */
#define POLL_ROUNDS 16

void rift_tiles_init(struct rift_tiles *t)
{
    memset(t, 0, sizeof(*t));
    t->pid = -1;
    t->fd = -1;
}

bool rift_tiles_running(const struct rift_tiles *t)
{
    return t && t->running;
}

/* web_tile_valid's rule; the cache module it lives in is the helper's, and
 * the shell does not link it. */
static bool tile_ok(int z, int x, int y)
{
    return z >= 0 && z <= WEB_TILE_ZOOM_MAX && x >= 0 && y >= 0 && x < (1 << z) && y < (1 << z);
}

static bool same(const struct rift_tile_key *k, int z, int x, int y)
{
    return k->z == z && k->x == x && k->y == y;
}

static int find_slot(const struct rift_tiles *t, int z, int x, int y)
{
    int i;

    for (i = 0; i < RIFT_TILES_MEM_MAX; i++) {
        if (t->slot[i].px && t->slot[i].z == z && t->slot[i].x == x && t->slot[i].y == y) {
            return i;
        }
    }
    return -1;
}

static void free_slot(struct rift_tiles *t, int i)
{
    if (!t->slot[i].px) {
        return;
    }
    if (t->evict) {
        t->evict(t->evict_ctx, i);
    }
    free(t->slot[i].px);
    memset(&t->slot[i], 0, sizeof(t->slot[i]));
}

int rift_tiles_held(const struct rift_tiles *t)
{
    int n = 0;
    int i;

    for (i = 0; i < RIFT_TILES_MEM_MAX; i++) {
        n += t->slot[i].px ? 1 : 0;
    }
    return n;
}

/* ---- the directory for the pictures ------------------------------------------------- */

static void remove_dir(struct rift_tiles *t)
{
    DIR *d;
    struct dirent *e;

    if (!t->out_dir[0]) {
        return;
    }
    d = opendir(t->out_dir);
    if (d) {
        int dfd = dirfd(d);

        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] != '.') {
                unlinkat(dfd, e->d_name, 0);
            }
        }
        closedir(d);
    }
    rmdir(t->out_dir);
    t->out_dir[0] = '\0';
}

static int make_dirs(struct rift_tiles *t, const struct rift_tiles_config *cfg, char *cache, size_t cache_len)
{
    const char *runtime = cfg->runtime ? cfg->runtime : pocketos_runtime_dir();
    const char *state = cfg->state ? cfg->state : pocketos_state_dir();
    char tmpl[RIFT_TILES_PATH_MAX];
    char rift[RIFT_TILES_PATH_MAX];
    int n;

    t->out_dir[0] = '\0';
    if (pocketos_mkdir_p(runtime, 0755) != 0) {
        return -1;
    }
    n = snprintf(tmpl, sizeof(tmpl), "%s/rift-tiles.XXXXXX", runtime);
    if (n < 0 || (size_t)n >= sizeof(tmpl) || !mkdtemp(tmpl)) {
        return -1;
    }
    snprintf(t->out_dir, sizeof(t->out_dir), "%s", tmpl);
    /* The kept tiles: the reader's own (they say where the map was looked
     * at), so only its owner reaches them. A directory that cannot be made
     * leaves the helper to run uncached; it refuses one others can reach. */
    cache[0] = '\0';
    n = snprintf(rift, sizeof(rift), "%s/rift", state);
    if (n > 0 && (size_t)n < sizeof(rift) && pocketos_mkdir_p(rift, 0755) == 0) {
        n = snprintf(cache, cache_len, "%s/tiles", rift);
        if (n < 0 || (size_t)n >= cache_len || (mkdir(cache, 0700) != 0 && errno != EEXIST)) {
            cache[0] = '\0';
        }
    }
    return 0;
}

/* ---- the helper -------------------------------------------------------------------- */

static void fail_start(struct rift_tiles *t, const char *what)
{
    snprintf(t->err, sizeof(t->err), "%s", what);
}

int rift_tiles_start(struct rift_tiles *t, const struct rift_tiles_config *cfg, int64_t now_ms)
{
    const char *env = getenv("POCKETOS_BROWSER_HELPER");
    char cache[RIFT_TILES_PATH_MAX];
    char *argv[14];
    int argc = 0;
    int sv[2];
    pid_t parent = getpid();
    pid_t pid;

    if (t->running) {
        return 0;
    }
    if (cfg != &t->cfg) {
        /* Kept for a restart: the strings are copied, the rest as given. */
        t->cfg = *cfg;
        snprintf(t->cfg_url, sizeof(t->cfg_url), "%s",
                 cfg->url ? cfg->url : cfg->fake ? RIFT_TILE_URL_FAKE : RIFT_TILE_URL_OSM);
        snprintf(t->cfg_helper, sizeof(t->cfg_helper), "%s",
                 cfg->helper ? cfg->helper : env && *env ? env : HELPER_DEFAULT);
        t->cfg.url = t->cfg_url;
        t->cfg.helper = t->cfg_helper;
        t->restarts = 0;
        t->gave_up = false;
    }
    t->on = true;
    t->err[0] = '\0';
    t->restart_at = 0;
    t->hello = false;
    t->eof = false;
    t->killed = false;
    t->line_len = 0;
    t->overlong = false;
    /* A fresh helper knows of no want: what is still to get is asked again. */
    t->nwant = 0;
    t->pending = 0;
    if (make_dirs(t, &t->cfg, cache, sizeof(cache)) != 0) {
        fail_start(t, "no runtime directory");
        t->gave_up = true;
        return -1;
    }
    argv[argc++] = (char *)t->cfg.helper;
    argv[argc++] = "tiles";
    argv[argc++] = "--out";
    argv[argc++] = t->out_dir;
    argv[argc++] = "--url";
    argv[argc++] = (char *)t->cfg.url;
    if (cache[0]) {
        argv[argc++] = "--cache";
        argv[argc++] = cache;
    }
    if (t->cfg.fake) {
        argv[argc++] = "--fake";
    }
    if (t->cfg.ca_file && *t->cfg.ca_file) {
        argv[argc++] = "--ca-file";
        argv[argc++] = (char *)t->cfg.ca_file;
    }
    argv[argc] = NULL;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        fail_start(t, "no socket for the helper");
        remove_dir(t);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        fail_start(t, "the helper could not be started");
        close(sv[0]);
        close(sv[1]);
        remove_dir(t);
        return -1;
    }
    if (pid == 0) {
        sigset_t none;
        int null;
        int fd;

        /* Leave with the shell; the check after closes the race where the
         * shell died before the prctl. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) {
            _exit(0);
        }
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (dup2(sv[1], 0) < 0 || dup2(sv[1], 1) < 0) {
            _exit(127);
        }
        null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, 2);
        }
        for (fd = 3; fd < CHILD_FD_SCAN_MAX; fd++) {
            close(fd);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    close(sv[1]);
    t->fd = sv[0];
    fcntl(t->fd, F_SETFL, fcntl(t->fd, F_GETFL) | O_NONBLOCK);
    t->pid = pid;
    t->running = true;
    t->hello_by = now_ms + RIFT_TILES_HELLO_MS;
    t->last_line_ms = now_ms;
    t->starts++;
    return 0;
}

static int send_line(struct rift_tiles *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static int send_line(struct rift_tiles *t, const char *fmt, ...)
{
    char line[WEB_TILE_WANT_MAX * 24 + 64];
    va_list ap;
    int n;

    if (!t->running || t->fd < 0) {
        return -1;
    }
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(line) - 1) {
        return -1;
    }
    line[n++] = '\n';
    /* Never waits; MSG_NOSIGNAL, so a helper that has just died cannot take
     * the shell with it. */
    return send(t->fd, line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) == n ? 0 : -1;
}

static void kill_helper(struct rift_tiles *t, const char *why)
{
    if (t->running && t->pid > 0 && !t->killed) {
        kill(t->pid, SIGKILL);
        t->killed = true;
        LOG_WARN("rift tiles: helper killed: %s", why);
    }
}

static void nap(void)
{
    struct timespec d = { 0, NAP_NS };

    nanosleep(&d, NULL);
}

static bool reap(struct rift_tiles *t, int naps)
{
    int status;
    int i;

    for (i = 0;; i++) {
        pid_t r = waitpid(t->pid, &status, WNOHANG);

        if (r == t->pid || (r < 0 && errno == ECHILD)) {
            return true;
        }
        if (i >= naps) {
            return false;
        }
        nap();
    }
}

void rift_tiles_stop(struct rift_tiles *t)
{
    int i;

    if (t->running && t->pid > 0) {
        send_line(t, "quit");
        if (!t->killed) {
            kill(t->pid, SIGTERM);
        }
        if (!reap(t, QUIT_NAPS)) {
            kill(t->pid, SIGKILL);
            /* Stuck in the kernel even so: left as a zombie rather than
             * holding the LVGL thread. */
            reap(t, KILL_NAPS);
        }
    }
    if (t->fd >= 0) {
        close(t->fd);
    }
    remove_dir(t);
    for (i = 0; i < RIFT_TILES_MEM_MAX; i++) {
        free_slot(t, i);
    }
    {
        void (*evict)(void *, int) = t->evict;
        void *ctx = t->evict_ctx;
        unsigned starts = t->starts;

        rift_tiles_init(t);
        t->evict = evict;
        t->evict_ctx = ctx;
        t->starts = starts;
    }
}

/* ---- what is asked ---------------------------------------------------------------- */

static bool failed_lately(const struct rift_tiles *t, int z, int x, int y, int64_t now)
{
    int i;

    for (i = 0; i < t->nfailed; i++) {
        if (same(&t->failed[i], z, x, y) && now - t->failed_ms[i] < RIFT_TILES_RETRY_MS) {
            return true;
        }
    }
    return false;
}

static void note_failed(struct rift_tiles *t, int z, int x, int y, int64_t now)
{
    int i;
    int oldest = 0;

    for (i = 0; i < t->nfailed; i++) {
        if (same(&t->failed[i], z, x, y)) {
            t->failed_ms[i] = now;
            return;
        }
        if (t->failed_ms[i] < t->failed_ms[oldest]) {
            oldest = i;
        }
    }
    i = t->nfailed < RIFT_TILES_FAILED_MAX ? t->nfailed++ : oldest;
    t->failed[i].z = z;
    t->failed[i].x = x;
    t->failed[i].y = y;
    t->failed_ms[i] = now;
}

void rift_tiles_want(struct rift_tiles *t, const struct rift_map_tile *tiles, int n, int64_t now_ms)
{
    struct rift_tile_key k[RIFT_MAP_TILES_MAX];
    char line[WEB_TILE_WANT_MAX * 24 + 32];
    size_t o = 0;
    int nk = 0;
    int i;
    int j;
    bool changed;

    for (i = 0; i < RIFT_TILES_MEM_MAX; i++) {
        t->slot[i].in_view = false;
    }
    for (i = 0; i < n && i < RIFT_MAP_TILES_MAX; i++) {
        int s = find_slot(t, tiles[i].z, tiles[i].x, tiles[i].y);
        bool dup = false;

        if (s >= 0) {
            t->slot[s].in_view = true;
            /* A stale copy is asked for again now and then: the network
             * may be back. */
            if (!t->slot[s].stale || now_ms - t->slot[s].got_ms < RIFT_TILES_RETRY_MS) {
                continue;
            }
        } else if (failed_lately(t, tiles[i].z, tiles[i].x, tiles[i].y, now_ms)) {
            continue;
        }
        for (j = 0; j < nk; j++) {
            dup = dup || same(&k[j], tiles[i].z, tiles[i].x, tiles[i].y);
        }
        if (!dup && tile_ok(tiles[i].z, tiles[i].x, tiles[i].y)) {
            k[nk].z = tiles[i].z;
            k[nk].x = tiles[i].x;
            k[nk].y = tiles[i].y;
            nk++;
        }
    }
    if (!t->running || !t->hello) {
        return; /* asked once the helper has said hello */
    }
    /* Still owed under the want in force is exactly what is still to get:
     * nothing to say (tiles arriving shrink both alike). Anything else - a
     * tile come into view, or one owed that has left it - is a new want. */
    changed = false;
    for (i = 0; !changed && i < nk; i++) {
        bool owed = false;

        for (j = 0; j < t->nwant && !owed; j++) {
            owed = !t->done[j] && same(&t->want[j], k[i].z, k[i].x, k[i].y);
        }
        changed = !owed;
    }
    for (j = 0; !changed && j < t->nwant; j++) {
        bool needed = false;

        for (i = 0; i < nk && !needed && !t->done[j]; i++) {
            needed = same(&t->want[j], k[i].z, k[i].x, k[i].y);
        }
        changed = !t->done[j] && !needed;
    }
    if (!changed) {
        return;
    }
    t->gen++;
    for (i = 0; i < nk && o + 24 < sizeof(line); i++) {
        o += (size_t)snprintf(line + o, sizeof(line) - o, "%s%d/%d/%d", i ? " " : "", k[i].z, k[i].x, k[i].y);
    }
    line[o] = '\0';
    if (send_line(t, "want\t%d\t%s", t->gen, line) != 0) {
        return;
    }
    memcpy(t->want, k, (size_t)nk * sizeof(k[0]));
    memset(t->done, 0, sizeof(t->done));
    t->nwant = nk;
    t->pending = nk;
    t->why[0] = '\0';
}

/* ---- answers ------------------------------------------------------------------------ */

/* The want entry this answers - in the want in force, not yet answered - or -1. */
static int owed(const struct rift_tiles *t, int z, int x, int y)
{
    int i;

    for (i = 0; i < t->nwant; i++) {
        if (!t->done[i] && same(&t->want[i], z, x, y)) {
            return i;
        }
    }
    return -1;
}

/* Exactly one tile of pixels from the file the helper named, which is
 * removed whatever is in it. */
static uint16_t *take_pixels(struct rift_tiles *t, const char *file)
{
    char path[RIFT_TILES_PATH_MAX + WEB_FILE_NAME_MAX + 2];
    struct stat st;
    uint16_t *px = NULL;
    size_t got = 0;
    int fd;

    if (!t->out_dir[0] || !web_rx_file_name_ok(file)) {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/%s", t->out_dir, file);
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    unlink(path);
    if (fd < 0) {
        return NULL;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size != TILE_BYTES ||
        !(px = malloc(TILE_BYTES))) {
        close(fd);
        return NULL;
    }
    while (got < TILE_BYTES) {
        ssize_t r = read(fd, (char *)px + got, TILE_BYTES - got);

        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    if (got != TILE_BYTES) {
        free(px);
        return NULL;
    }
    return px;
}

static void drop_file(struct rift_tiles *t, const char *file)
{
    char path[RIFT_TILES_PATH_MAX + WEB_FILE_NAME_MAX + 2];

    if (t->out_dir[0] && web_rx_file_name_ok(file)) {
        snprintf(path, sizeof(path), "%s/%s", t->out_dir, file);
        unlink(path);
    }
}

/* A place for a new tile: a free one, else the least recently drawn that is
 * out of view; -1 when every slot is in view. */
static int place_for(struct rift_tiles *t)
{
    int best = -1;
    int i;

    for (i = 0; i < RIFT_TILES_MEM_MAX; i++) {
        if (!t->slot[i].px) {
            return i;
        }
        if (!t->slot[i].in_view && (best < 0 || t->slot[i].used < t->slot[best].used)) {
            best = i;
        }
    }
    return best;
}

static void answered(struct rift_tiles *t, int entry)
{
    t->done[entry] = true;
    if (t->pending > 0) {
        t->pending--;
    }
}

static void keep_why(struct rift_tiles *t, const char *why)
{
    if (why && *why && strcmp(why, "-") != 0) {
        snprintf(t->why, sizeof(t->why), "%s", why);
    }
}

static int to_int(const char *s, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < -1000000000L || v > 1000000000L) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static bool line_in(struct rift_tiles *t, char *line, int64_t now)
{
    char *f[11];
    int n = 0;
    char *p = line;
    int gen;
    int z;
    int x;
    int y;

    t->last_line_ms = now;
    while (n < 11) {
        char *tab = strchr(p, '\t');

        f[n++] = p;
        if (!tab) {
            break;
        }
        *tab = '\0';
        p = tab + 1;
    }
    if (strcmp(f[0], "hello") == 0 && n >= 2) {
        int proto;

        if (to_int(f[1], &proto) != 0 || proto != WEB_TILE_PROTO_VERSION) {
            kill_helper(t, "protocol");
            return false;
        }
        t->hello = true;
        t->hello_by = 0;
        return true;
    }
    if (!t->hello) {
        return false; /* nothing counts before hello */
    }
    if (strcmp(f[0], "idle") == 0 && n == 2) {
        if (to_int(f[1], &gen) == 0 && gen == t->gen) {
            /* Whatever was not answered (a malformed entry the helper
             * stopped at) is not owed any more. */
            memset(t->done, 1, sizeof(t->done));
            t->pending = 0;
        }
        return true;
    }
    if (strcmp(f[0], "tile") == 0 && n == 10) {
        int w;
        int h;
        int e;
        uint16_t *px;
        int s;

        t->answers++;
        if (to_int(f[1], &gen) || to_int(f[2], &z) || to_int(f[3], &x) || to_int(f[4], &y) || to_int(f[5], &w) ||
            to_int(f[6], &h)) {
            drop_file(t, f[7]);
            return false;
        }
        /* Asked for by a view that has gone, answered already, or not
         * asked at all: never over what is on screen now. */
        e = gen == t->gen ? owed(t, z, x, y) : -1;
        if (e < 0 || w != RIFT_MAP_TILE_PX || h != RIFT_MAP_TILE_PX) {
            drop_file(t, f[7]);
            t->dropped++;
            return false;
        }
        answered(t, e);
        px = take_pixels(t, f[7]);
        if (!px) {
            note_failed(t, z, x, y, now);
            keep_why(t, "store");
            return true;
        }
        /* The same tile again (a stale copy renewed) takes its own place;
         * a new one a free place or the oldest out of view. */
        s = find_slot(t, z, x, y);
        if (s < 0) {
            s = place_for(t);
        }
        if (s < 0) {
            /* Every place holds a tile in view: this one waits. */
            free(px);
            note_failed(t, z, x, y, now);
            return false;
        }
        free_slot(t, s);
        t->slot[s].px = px;
        t->slot[s].z = z;
        t->slot[s].x = x;
        t->slot[s].y = y;
        t->slot[s].stale = strcmp(f[8], "stale") == 0;
        t->slot[s].in_view = true;
        t->slot[s].used = ++t->use_clock;
        t->slot[s].got_ms = now;
        if (t->slot[s].stale) {
            keep_why(t, f[9]);
        }
        t->taken++;
        return true;
    }
    if (strcmp(f[0], "notile") == 0 && n == 6) {
        t->answers++;
        if (to_int(f[1], &gen) || to_int(f[2], &z) || to_int(f[3], &x) || to_int(f[4], &y)) {
            return false;
        }
        {
            int e = gen == t->gen ? owed(t, z, x, y) : -1;

            if (e < 0) {
                t->dropped++;
                return false;
            }
            answered(t, e);
        }
        note_failed(t, z, x, y, now);
        keep_why(t, f[5]);
        return true;
    }
    return false;
}

static bool feed(struct rift_tiles *t, const char *buf, size_t n, int64_t now)
{
    bool changed = false;
    size_t i;

    for (i = 0; i < n; i++) {
        char c = buf[i];

        if (c == '\n') {
            if (t->overlong) {
                kill_helper(t, "line too long");
            } else {
                t->line[t->line_len] = '\0';
                changed |= line_in(t, t->line, now);
            }
            t->line_len = 0;
            t->overlong = false;
        } else if (!t->overlong) {
            if (t->line_len + 1 >= sizeof(t->line)) {
                t->overlong = true;
            } else {
                t->line[t->line_len++] = c;
            }
        }
    }
    return changed;
}

/* The helper has gone: tidy up, and say when to try again. */
static void finish(struct rift_tiles *t, int status, int64_t now)
{
    bool exec_failed = WIFEXITED(status) && WEXITSTATUS(status) == 127;
    bool asked = t->killed;

    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
    remove_dir(t);
    t->pid = -1;
    t->running = false;
    t->hello = false;
    t->nwant = 0;
    t->pending = 0;
    if (!t->on) {
        return;
    }
    if (exec_failed) {
        snprintf(t->err, sizeof(t->err), "no tile helper (%s)", t->cfg.helper);
        t->gave_up = true;
    } else if (++t->restarts > RIFT_TILES_RESTARTS_MAX) {
        snprintf(t->err, sizeof(t->err), "the tile helper kept stopping");
        t->gave_up = true;
    } else {
        snprintf(t->err, sizeof(t->err), "the tile helper %s", asked ? "did not answer" : "stopped");
        t->restart_at = now + RIFT_TILES_RESTART_MS;
    }
    LOG_WARN("rift tiles: %s", t->err);
}

bool rift_tiles_poll(struct rift_tiles *t, int64_t now_ms)
{
    bool changed = false;
    char buf[2048];
    int rounds = 0;
    int status;
    pid_t r;

    if (!t->running) {
        if (t->on && !t->gave_up && t->restart_at && now_ms >= t->restart_at) {
            changed = true;
            if (rift_tiles_start(t, &t->cfg, now_ms) != 0) {
                t->gave_up = true;
            }
        }
        return changed;
    }
    while (t->fd >= 0 && !t->eof && !t->killed && rounds++ < POLL_ROUNDS) {
        ssize_t n = recv(t->fd, buf, sizeof(buf), MSG_DONTWAIT);

        if (n > 0) {
            changed |= feed(t, buf, (size_t)n, now_ms);
        } else if (n == 0) {
            t->eof = true;
        } else if (errno == EINTR) {
            continue;
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                t->eof = true;
            }
            break;
        }
    }
    if (t->hello_by && now_ms >= t->hello_by) {
        kill_helper(t, "no hello");
    } else if (t->pending > 0 && now_ms - t->last_line_ms >= RIFT_TILES_SILENCE_MS) {
        kill_helper(t, "silent");
    }
    if (t->eof && !t->killed) {
        kill(t->pid, SIGTERM);
    }
    r = waitpid(t->pid, &status, WNOHANG);
    if (r == t->pid) {
        finish(t, status, now_ms);
        changed = true;
    } else if (r < 0 && errno == ECHILD) {
        finish(t, 255 << 8, now_ms);
        changed = true;
    }
    return changed;
}

const uint16_t *rift_tiles_pixels(struct rift_tiles *t, int z, int x, int y, int *slot, bool *stale)
{
    int s = find_slot(t, z, x, y);

    if (s < 0) {
        return NULL;
    }
    t->slot[s].used = ++t->use_clock;
    if (slot) {
        *slot = s;
    }
    if (stale) {
        *stale = t->slot[s].stale;
    }
    return t->slot[s].px;
}

enum rift_tiles_status rift_tiles_status(const struct rift_tiles *t, const char **why)
{
    const char *w = t->why;

    if (why) {
        *why = "";
    }
    if (t->gave_up || (!t->running && t->err[0] && !t->restart_at)) {
        if (why) {
            *why = t->err;
        }
        return RIFT_TILES_UNAVAILABLE;
    }
    if (!t->on) {
        return RIFT_TILES_OFF;
    }
    if (!t->running || !t->hello || t->pending > 0) {
        return RIFT_TILES_LOADING;
    }
    if (!w[0]) {
        return RIFT_TILES_SHOWN;
    }
    if (why) {
        *why = w;
    }
    if (strcmp(w, "offline") == 0 || strcmp(w, "dns") == 0 || strcmp(w, "connect") == 0 ||
        strcmp(w, "timeout") == 0) {
        return RIFT_TILES_OFFLINE;
    }
    if (strcmp(w, "clock") == 0) {
        return RIFT_TILES_NO_CLOCK;
    }
    return RIFT_TILES_ERROR;
}
