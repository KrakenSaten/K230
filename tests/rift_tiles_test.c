/*
 * MAP's basemap, without a display: the Web Mercator geometry the markers
 * and the tiles share (a marker lands on its own tile pixel), the tiles a
 * view covers and no others, the latitude limit and the antimeridian; and
 * the client (rift_tiles.h) against the real `pos-browser tiles` on the
 * fake network - tiles taken, an answer for a view that has gone dropped
 * and never shown, a cancel, offline with and without kept tiles, a server
 * error, the memory bound, and the helper's whole life: stopped, killed,
 * restarted, missing.
 *
 * Usage: rift_tiles_test <path to pos-browser>
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "rift_map.h"
#include "rift_tiles.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failed;
static const char *helper;
static char root[] = "/tmp/rift_tiles_test.XXXXXX";
static char runtime[600];
static char state[600];

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static int64_t now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void nap_ms(int ms)
{
    struct timespec d = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&d, NULL);
}

/* ---- geometry -------------------------------------------------------------------- */

static void geometry(void)
{
    static struct rift_model m;
    struct rift_map_view v;
    struct rift_map_tile t[RIFT_MAP_TILES_MAX];
    double x;
    double y;
    double lat;
    double lon;
    int n;
    int i;
    int ok;

    memset(&v, 0, sizeof(v));
    v.w = 500;
    v.h = 400;
    v.cx = 0.5;
    v.cy = 0.5;
    v.zoom = 3;
    rift_map_project(&v, 0, 0, &x, &y);
    check("0,0 is the middle of the world", fabs(x - 250) < 1e-9 && fabs(y - 200) < 1e-9);
    rift_map_project(&v, 85.05112878, 0, &y, &x);
    check("the Mercator limit is the top of the world", fabs(x - (200 - 0.5 * 2048)) < 1e-6);
    rift_map_project(&v, 89.9, 0, &x, &y);
    check("and nothing goes past it", fabs(y - (200 - 0.5 * 2048)) < 1e-6);

    /* Oslo, at zoom 14: the marker on the tile pixel of its position. */
    v.zoom = 14;
    v.w = 760;
    v.h = 330;
    lat = 59.9139;
    lon = 10.7522;
    v.cx = (lon + 180.0) / 360.0;
    v.cy = 0.5 - log((1 + sin(lat * M_PI / 180)) / (1 - sin(lat * M_PI / 180))) / (4 * M_PI);
    rift_map_pan(&v, 37, -21);
    rift_map_project(&v, lat, lon, &x, &y);
    n = rift_map_tiles(&v, t, RIFT_MAP_TILES_MAX);
    {
        double wx = (lon + 180.0) / 360.0 * (1 << 14);
        double wy = v.cy;
        int tx = (int)floor(wx);
        int ty;
        int found = 0;

        wy = (0.5 - log((1 + sin(lat * M_PI / 180)) / (1 - sin(lat * M_PI / 180))) / (4 * M_PI)) * (1 << 14);
        ty = (int)floor(wy);
        for (i = 0; i < n; i++) {
            if (t[i].x == tx && t[i].y == ty) {
                double ex = t[i].sx + (wx - tx) * 256;
                double ey = t[i].sy + (wy - ty) * 256;

                found = fabs(ex - x) <= 1.0 && fabs(ey - y) <= 1.0;
            }
        }
        check("a marker sits on its own tile pixel, within a pixel", found);
        check("that tile is the one OpenStreetMap numbers 14/8681/4765 for Oslo",
              tx == 8681 && ty == 4765);
    }
    rift_map_unproject(&v, x, y, &lat, &lon);
    check("and back again", fabs(lat - 59.9139) < 1e-7 && fabs(lon - 10.7522) < 1e-7);

    /* The tiles a view covers: those that touch it, every pixel of it, no
     * more, nearest the centre first. */
    ok = n > 0;
    for (i = 0; i < n; i++) {
        ok = ok && t[i].sx < v.w && t[i].sy < v.h && t[i].sx + 256 > 0 && t[i].sy + 256 > 0 && t[i].z == 14;
    }
    check("only tiles that intersect the map are named (no margin)", ok && n <= 12);
    {
        int px;
        int py;
        int covered = 1;

        for (py = 0; py < v.h; py += 7) {
            for (px = 0; px < v.w; px += 7) {
                int hit = 0;

                for (i = 0; i < n; i++) {
                    hit = hit || (px >= t[i].sx && px < t[i].sx + 256 && py >= t[i].sy && py < t[i].sy + 256);
                }
                covered = covered && hit;
            }
        }
        check("and together they cover all of it", covered);
    }
    {
        double d0 = pow(t[0].sx + 128 - v.w / 2.0, 2) + pow(t[0].sy + 128 - v.h / 2.0, 2);
        double dl = pow(t[n - 1].sx + 128 - v.w / 2.0, 2) + pow(t[n - 1].sy + 128 - v.h / 2.0, 2);

        check("nearest the centre first", d0 <= dl);
    }
    v.zoom = 2;
    v.w = 512;
    v.h = 512;
    v.cx = 0.5;
    v.cy = 0.5;
    n = rift_map_tiles(&v, t, RIFT_MAP_TILES_MAX);
    check("an area aligned to the grid is exactly its tiles", n == 4);

    /* The antimeridian and the poles. */
    v.zoom = 6;
    v.w = 600;
    v.h = 400;
    lon = 179.95;
    v.cx = (lon + 180.0) / 360.0;
    v.cy = 0.3;
    rift_map_project(&v, 60.0, -179.95, &x, &y);
    check("a node over the date line is drawn beside one this side of it", fabs(x - 300) < 10);
    n = rift_map_tiles(&v, t, RIFT_MAP_TILES_MAX);
    {
        int west = 0;
        int east = 0;

        for (i = 0; i < n; i++) {
            west = west || t[i].x == 0;
            east = east || t[i].x == 63;
        }
        check("and the tiles wrap round it", west && east);
    }
    rift_map_pan(&v, 0, 1e9);
    check("panning north stops at the top of the world", v.cy == 0.0);
    n = rift_map_tiles(&v, t, RIFT_MAP_TILES_MAX);
    ok = 1;
    for (i = 0; i < n; i++) {
        ok = ok && t[i].y >= 0 && t[i].y < 64;
    }
    check("and no tile past the pole is asked for", ok && n > 0);
    rift_map_pan(&v, 1e6 + 0.5, 0);
    check("longitude wraps however far the map is dragged", v.cx >= 0 && v.cx < 1);
    v.zoom = 1;
    v.w = 1192;
    v.h = 300;
    v.cy = 0.5;
    n = rift_map_tiles(&v, t, RIFT_MAP_TILES_MAX);
    check("a world narrower than the map is shown more than once", n > 4 && n <= RIFT_MAP_TILES_MAX);

    /* Fitting and zooming. */
    memset(&m, 0, sizeof(m)); /* only the nodes' places are read */
    check("no node: the whole world", rift_map_fit(&v, &m, 500, 400) == 0 && v.zoom == RIFT_MAP_ZOOM_MIN);
    m.node_count = 2;
    m.nodes[0].have_location = 1;
    m.nodes[0].lat = 59.90;
    m.nodes[0].lon = 10.70;
    m.nodes[1].have_location = 1;
    m.nodes[1].lat = 59.95;
    m.nodes[1].lon = 10.80;
    check("two nodes are fitted", rift_map_fit(&v, &m, 500, 400) == 2);
    rift_map_project(&v, 59.90, 10.70, &x, &y);
    ok = x >= RIFT_MAP_FIT_MARGIN - 1 && y <= 400 - RIFT_MAP_FIT_MARGIN + 1;
    rift_map_project(&v, 59.95, 10.80, &x, &y);
    ok = ok && x <= 500 - RIFT_MAP_FIT_MARGIN + 1 && y >= RIFT_MAP_FIT_MARGIN - 1 && x > 250 && y < 200;
    check("both in, clear of the edge, north up and east right", ok);
    {
        int z = v.zoom;

        rift_map_zoom(&v, 1);
        rift_map_project(&v, 59.95, 10.80, &x, &y);
        check("the closest whole level that holds them",
              z > RIFT_MAP_ZOOM_MIN && (x > 500 - RIFT_MAP_FIT_MARGIN || y < RIFT_MAP_FIT_MARGIN));
    }
    m.node_count = 1;
    rift_map_fit(&v, &m, 500, 400);
    check("one node alone is the closest level, not an infinite zoom", v.zoom == RIFT_MAP_ZOOM_MAX);
    rift_map_zoom(&v, 5);
    check("nor does + go past it", v.zoom == RIFT_MAP_ZOOM_MAX);
    rift_map_zoom(&v, -40);
    check("and - stops at the world", v.zoom == RIFT_MAP_ZOOM_MIN);
    rift_map_fit(&v, &m, 500, 400);
    {
        int32_t px;
        double metres = rift_map_scale(&v, 120, &px);

        /* At zoom 17 and 59.9 N a pixel is 0.6 m. */
        check("the scale bar is true at the centre's latitude",
              px > 0 && px <= 120 && fabs(metres / px - 1.1943 * cos(59.9 * M_PI / 180)) < 0.02);
    }
}

/* ---- the client against the real helper ------------------------------------------------ */

struct evicts {
    int n;
};

static void on_evict(void *ctx, int slot)
{
    (void)slot;
    ((struct evicts *)ctx)->n++;
}

static struct rift_tiles_config config(const char *url)
{
    struct rift_tiles_config c;

    memset(&c, 0, sizeof(c));
    c.helper = helper;
    c.fake = true;
    c.url = url;
    c.runtime = runtime;
    c.state = state;
    return c;
}

/* Poll until nothing is owed (or ms pass). */
static void settle(struct rift_tiles *t, const struct rift_map_tile *tiles, int n, int ms)
{
    int64_t end = now_ms() + ms;

    do {
        rift_tiles_poll(t, now_ms());
        rift_tiles_want(t, tiles, n, now_ms());
        nap_ms(5);
    } while (now_ms() < end && (rift_tiles_status(t, NULL) == RIFT_TILES_LOADING));
}

static int dirs_left(void)
{
    DIR *d = opendir(runtime);
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL) {
        n += strncmp(e->d_name, "rift-tiles.", 11) == 0;
    }
    if (d) {
        closedir(d);
    }
    return n;
}

static int files_in(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.';
    }
    if (d) {
        closedir(d);
    }
    return n;
}

static int gone(pid_t pid)
{
    return pid <= 0 || (kill(pid, 0) != 0 && errno == ESRCH);
}

static void row(struct rift_map_tile *t, int n, int z, int x0, int y)
{
    int i;

    for (i = 0; i < n; i++) {
        t[i].z = z;
        t[i].x = x0 + i;
        t[i].y = y;
        t[i].sx = i * 256;
        t[i].sy = 0;
    }
}

static void client(void)
{
    struct rift_tiles t;
    struct rift_tiles_config c = config(RIFT_TILE_URL_FAKE);
    struct rift_map_tile a[8];
    struct rift_map_tile b[8];
    struct evicts ev = { 0 };
    char cache[700];
    const char *why;
    bool stale;
    int slot;
    int gen;
    pid_t pid;
    int i;
    int ok;

    snprintf(cache, sizeof(cache), "%s/rift/tiles", state);
    rift_tiles_init(&t);
    t.evict = on_evict;
    t.evict_ctx = &ev;
    check("off until started", rift_tiles_status(&t, NULL) == RIFT_TILES_OFF && !rift_tiles_running(&t));
    check("the helper starts", rift_tiles_start(&t, &c, now_ms()) == 0 && rift_tiles_running(&t));
    row(a, 4, 12, 2170, 1190);
    settle(&t, a, 4, 3000);
    ok = 1;
    for (i = 0; i < 4; i++) {
        ok = ok && rift_tiles_pixels(&t, 12, 2170 + i, 1190, &slot, &stale) && !stale;
    }
    check("the tiles in view arrive, decoded", ok && rift_tiles_held(&t) == 4 && t.taken == 4);
    check("and the basemap says they are shown", rift_tiles_status(&t, NULL) == RIFT_TILES_SHOWN);
    check("the pictures do not pile up in the session's directory", files_in(t.out_dir) == 0);
    check("and they were kept in the cache, 0700", files_in(cache) == 8);
    {
        struct stat st;

        check("which only its owner reaches", stat(cache, &st) == 0 && (st.st_mode & 077) == 0);
    }
    gen = t.gen;
    rift_tiles_want(&t, a, 4, now_ms());
    check("the same view again asks for nothing", t.gen == gen);

    /* A view left before its tiles came: on the slow server, ask for one
     * row, then at once for another. */
    rift_tiles_stop(&t);
    c = config("https://tiles-slow.doors.test/{z}/{x}/{y}.png");
    rift_tiles_start(&t, &c, now_ms());
    row(a, 4, 13, 100, 50);
    row(b, 4, 13, 200, 50);
    settle(&t, a, 0, 2000); /* hello */
    rift_tiles_want(&t, a, 4, now_ms());
    nap_ms(450); /* the first of a is being fetched, or done */
    rift_tiles_poll(&t, now_ms());
    rift_tiles_want(&t, b, 4, now_ms());
    settle(&t, b, 4, 6000);
    ok = rift_tiles_held(&t) >= 4;
    for (i = 0; i < 4; i++) {
        ok = ok && rift_tiles_pixels(&t, 13, 200 + i, 50, NULL, NULL);
    }
    check("the new view's tiles all arrive", ok);
    ok = 1;
    for (i = 1; i < 4; i++) {
        ok = ok && !rift_tiles_pixels(&t, 13, 100 + i, 50, NULL, NULL);
    }
    check("the old view's were stopped, not fetched", ok);
    check("and nothing asked for under the old view was taken after it",
          t.taken == (unsigned)rift_tiles_held(&t));
    {
        unsigned answers;
        int64_t end;

        row(a, 6, 13, 300, 60);
        rift_tiles_want(&t, a, 6, now_ms());
        nap_ms(50);
        rift_tiles_poll(&t, now_ms());
        answers = t.answers;
        rift_tiles_want(&t, a, 0, now_ms());
        check("leaving the view cancels what it asked for", t.pending == 0);
        end = now_ms() + 1500;
        while (now_ms() < end) {
            rift_tiles_poll(&t, now_ms());
            nap_ms(10);
        }
        check("and the transfer in flight is stopped: nothing of it comes (six take 2.4 s)",
              t.answers == answers && !rift_tiles_pixels(&t, 13, 300, 60, NULL, NULL));
    }

    /* Offline: tiles never seen are not there; tiles seen before are. */
    rift_tiles_stop(&t);
    c = config("https://tiles-stale.doors.test/{z}/{x}/{y}.png");
    rift_tiles_start(&t, &c, now_ms());
    row(a, 2, 14, 500, 500);
    settle(&t, a, 2, 3000);
    rift_tiles_stop(&t);
    c = config("https://offline.doors.test/{z}/{x}/{y}.png");
    rift_tiles_start(&t, &c, now_ms());
    row(a, 3, 14, 500, 500);
    settle(&t, a, 3, 3000);
    check("offline, the tiles seen before are shown, stale",
          rift_tiles_pixels(&t, 14, 500, 500, NULL, &stale) && stale &&
              rift_tiles_pixels(&t, 14, 501, 500, NULL, NULL));
    check("the one never seen is not", !rift_tiles_pixels(&t, 14, 502, 500, NULL, NULL));
    check("and the basemap says offline",
          rift_tiles_status(&t, &why) == RIFT_TILES_OFFLINE && strcmp(why, "offline") == 0);
    gen = t.gen;
    rift_tiles_want(&t, a, 3, now_ms());
    check("a tile that could not be had is not asked for again at once", t.gen == gen);

    /* Fresh tiles need no network at all. */
    rift_tiles_stop(&t);
    row(a, 4, 12, 2170, 1190);
    rift_tiles_start(&t, &c, now_ms());
    settle(&t, a, 4, 3000);
    check("tiles still fresh in the cache are shown with no network",
          rift_tiles_held(&t) == 4 && rift_tiles_status(&t, NULL) == RIFT_TILES_SHOWN);

    /* A server that refuses. */
    rift_tiles_stop(&t);
    c = config("https://tiles-busy.doors.test/{z}/{x}/{y}.png");
    rift_tiles_start(&t, &c, now_ms());
    row(a, 2, 15, 7, 7);
    settle(&t, a, 2, 3000);
    check("a server error is said as one",
          rift_tiles_status(&t, &why) == RIFT_TILES_ERROR && strcmp(why, "http-503") == 0 &&
              rift_tiles_held(&t) == 0);

    /* The memory bound: more tiles in view than are held. */
    rift_tiles_stop(&t);
    c = config(RIFT_TILE_URL_FAKE);
    rift_tiles_start(&t, &c, now_ms());
    {
        struct rift_map_tile many[40];

        row(many, 40, 16, 1000, 1000);
        settle(&t, many, 40, 8000);
        check("never more than RIFT_TILES_MEM_MAX tiles in memory", rift_tiles_held(&t) == RIFT_TILES_MEM_MAX);
        row(a, 8, 16, 2000, 1000);
        settle(&t, a, 8, 4000);
        check("a new view takes the places of those out of view",
              rift_tiles_held(&t) == RIFT_TILES_MEM_MAX && rift_tiles_pixels(&t, 16, 2007, 1000, NULL, NULL) &&
                  ev.n >= 8);
    }

    /* The helper's life. */
    ev.n = 0;
    pid = t.pid;
    rift_tiles_stop(&t);
    check("stop ends the helper", gone(pid) && !rift_tiles_running(&t));
    check("removes its directory", dirs_left() == 0);
    check("and lets go of every tile, saying so first", rift_tiles_held(&t) == 0 && ev.n == RIFT_TILES_MEM_MAX);
    check("and is OFF again", rift_tiles_status(&t, NULL) == RIFT_TILES_OFF);
    ok = 1;
    for (i = 0; i < 20; i++) {
        rift_tiles_start(&t, &c, now_ms());
        if (i % 2) {
            settle(&t, a, 2, 2000);
        }
        pid = t.pid;
        rift_tiles_stop(&t);
        ok = ok && gone(pid);
    }
    check("20 starts and stops leave no helper", ok && waitpid(-1, NULL, WNOHANG) <= 0);
    check("and no directory", dirs_left() == 0);

    rift_tiles_start(&t, &c, now_ms());
    settle(&t, a, 2, 3000);
    pid = t.pid;
    kill(pid, SIGKILL);
    {
        int64_t at = now_ms();
        int seen = 0;

        while (now_ms() - at < 2000 && !seen) {
            rift_tiles_poll(&t, now_ms());
            seen = !rift_tiles_running(&t);
            nap_ms(5);
        }
        check("a helper that dies is noticed", seen && gone(pid) && dirs_left() == 0);
        check("and not started again at once", rift_tiles_status(&t, NULL) == RIFT_TILES_LOADING);
        rift_tiles_poll(&t, now_ms() + RIFT_TILES_RESTART_MS + 10);
        check("but after RIFT_TILES_RESTART_MS", rift_tiles_running(&t) && t.pid != pid);
    }
    rift_tiles_stop(&t);

    memset(&c, 0, sizeof(c));
    c = config(RIFT_TILE_URL_FAKE);
    c.helper = "/nonexistent/pos-browser";
    rift_tiles_start(&t, &c, now_ms());
    {
        int64_t at = now_ms();

        while (now_ms() - at < 2000 && rift_tiles_running(&t)) {
            rift_tiles_poll(&t, now_ms());
            nap_ms(5);
        }
    }
    check("no helper at all: the basemap is unavailable, and says why",
          rift_tiles_status(&t, &why) == RIFT_TILES_UNAVAILABLE && strstr(why, "no tile helper") != NULL);
    rift_tiles_poll(&t, now_ms() + 10 * RIFT_TILES_RESTART_MS);
    check("and it is not tried again until turned off and on", !rift_tiles_running(&t));
    rift_tiles_stop(&t);
    check("off clears it", rift_tiles_status(&t, NULL) == RIFT_TILES_OFF && dirs_left() == 0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: rift_tiles_test <pos-browser>\n");
        return 2;
    }
    helper = argv[1];
    if (helper[0] != '/') {
        static char abs[700];
        char cwd[512];

        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(abs, sizeof(abs), "%s/%s", cwd, helper);
            helper = abs;
        }
    }
    if (!mkdtemp(root)) {
        printf("FAIL a temporary directory\n");
        return 1;
    }
    snprintf(runtime, sizeof(runtime), "%s/run", root);
    snprintf(state, sizeof(state), "%s/state", root);
    mkdir(runtime, 0755);
    mkdir(state, 0755);
    setenv("POCKETOS_LOG_STDERR", "0", 1);
    geometry();
    client();
    {
        char cmd[700];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
        if (system(cmd) != 0) {
            printf("note: %s was not removed\n", root);
        }
    }
    printf("rift_tiles_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
