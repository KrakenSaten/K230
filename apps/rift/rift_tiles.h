/*
 * MAP's basemap, the shell's half: a `pos-browser tiles` helper for as long
 * as the basemap is on and MAP's screen exists, and the few decoded tiles
 * the map is drawing. The protocol, the disk cache and the rules of
 * freshness are core/web/web_tiles.h; the reasons are docs/apps/RIFT.md
 * ("MAP") and ADR-009 Amendment 1.
 *
 * NOTHING UNTRUSTED IS HANDLED HERE. The network, TLS, the tile server's
 * bytes and the PNG decoder are all the helper's; this file reads lines
 * from a socket that already has data (MSG_DONTWAIT) and files of exactly
 * 256 x 256 RGB565 pixels the helper finished writing, by names
 * web_rx_file_name_ok accepts, never through a link, and removes them.
 *
 * WHAT IS ASKED FOR is what MAP draws: rift_tiles_want() takes the tiles
 * that intersect the map area now (rift_map_tiles) and sends the ones not
 * already held, under a new generation, when that set changed. A tile that
 * could not be had, or was shown stale, is not asked for again within
 * RIFT_TILES_RETRY_MS. An answer is taken only under the generation in force
 * and only for a tile still wanted: anything a view the reader has left
 * asked for is dropped, so it can never replace what is on screen now.
 *
 * MEMORY: at most RIFT_TILES_MEM_MAX tiles of 128 KB, the least recently
 * drawn of those out of view going first. The map is told before a tile's
 * pixels are freed (evict), so it can let go of anything that points at
 * them.
 *
 * LIFETIME: the helper leaves on rift_tiles_stop (quit, SIGTERM, then
 * SIGKILL, bounded), with the shell (PR_SET_PDEATHSIG), and on an exec (the
 * socket is close-on-exec). A helper that is silent past its deadlines is
 * killed; one that stops is started again after RIFT_TILES_RESTART_MS, at
 * most RIFT_TILES_RESTARTS_MAX times, and then the basemap says it is
 * unavailable until it is turned off and on.
 *
 * No LVGL and no clock of its own (now is passed in): tests/rift_tiles_test.c
 * runs it against the real helper on the fake network.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_TILES_H
#define RIFT_TILES_H

#include "rift_map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The OpenStreetMap Foundation's standard tile layer, under its tile usage
 * policy (https://operations.osmfoundation.org/policies/tiles/): https
 * only, a User-Agent of the app's own (the helper's), the server's cache
 * headers honoured, no bulk download, prefetch or offline packs, and the
 * attribution on the map whenever its tiles may be. */
#define RIFT_TILE_URL_OSM "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
#define RIFT_TILE_ATTRIBUTION "\xC2\xA9 OpenStreetMap contributors"
/* The fake network's tile server (core/web/web_fake.c). */
#define RIFT_TILE_URL_FAKE "https://tiles.doors.test/{z}/{x}/{y}.png"

#define RIFT_TILES_MEM_MAX 32
#define RIFT_TILES_HELLO_MS 3000
/* Silence while tiles are owed: more than a tile's own timeouts allow. */
#define RIFT_TILES_SILENCE_MS 45000
#define RIFT_TILES_RETRY_MS 30000
#define RIFT_TILES_RESTART_MS 10000
#define RIFT_TILES_RESTARTS_MAX 3
#define RIFT_TILES_FAILED_MAX 64
#define RIFT_TILES_PATH_MAX 512

/* What the basemap is doing, in the words MAP shows (rift_tiles_status). */
enum rift_tiles_status {
    RIFT_TILES_OFF = 0,     /* not started */
    RIFT_TILES_LOADING,     /* the helper is starting, or tiles are owed */
    RIFT_TILES_SHOWN,       /* every tile in view answered */
    RIFT_TILES_OFFLINE,     /* no network: kept tiles only */
    RIFT_TILES_NO_CLOCK,    /* the clock is not set: kept tiles only */
    RIFT_TILES_ERROR,       /* the server or a tile failed: why says how */
    RIFT_TILES_UNAVAILABLE  /* no helper, or it kept stopping */
};

struct rift_tiles_config {
    const char *helper;     /* NULL: $POCKETOS_BROWSER_HELPER, else /usr/bin/pos-browser */
    bool fake;              /* the fake network */
    const char *url;        /* NULL: RIFT_TILE_URL_OSM (RIFT_TILE_URL_FAKE when fake) */
    const char *ca_file;    /* NULL: the system store */
    const char *runtime;    /* NULL: pocketos_runtime_dir() */
    const char *state;      /* NULL: pocketos_state_dir() */
};

struct rift_tile_slot {
    int z;
    int x;
    int y;
    uint16_t *px;           /* RIFT_MAP_TILE_PX squared, or NULL: free */
    bool stale;
    bool in_view;
    unsigned used;
    int64_t got_ms;
};

struct rift_tile_key {
    int z;
    int x;
    int y;
};

struct rift_tiles {
    /* the helper */
    pid_t pid;
    int fd;
    bool running;
    bool eof;
    bool killed;
    bool hello;
    int64_t hello_by;
    int64_t last_line_ms;
    char line[1024];
    size_t line_len;
    bool overlong;
    char out_dir[RIFT_TILES_PATH_MAX];
    struct rift_tiles_config cfg;
    char cfg_url[RIFT_TILES_PATH_MAX];
    char cfg_helper[RIFT_TILES_PATH_MAX];
    bool on;                /* started, and not stopped: restarts allowed */
    unsigned restarts;
    int64_t restart_at;     /* 0: none due */
    bool gave_up;
    char err[96];

    /* what is asked */
    int gen;
    struct rift_tile_key want[RIFT_MAP_TILES_MAX];
    bool done[RIFT_MAP_TILES_MAX]; /* answered */
    int nwant;
    int pending;            /* of gen, not yet answered */
    struct rift_tile_key failed[RIFT_TILES_FAILED_MAX];
    int64_t failed_ms[RIFT_TILES_FAILED_MAX];
    int nfailed;
    char why[24];           /* the last trouble under gen, or "" */

    /* what is held */
    struct rift_tile_slot slot[RIFT_TILES_MEM_MAX];
    unsigned use_clock;
    void (*evict)(void *ctx, int slot);
    void *evict_ctx;

    /* for the tests and the log */
    unsigned answers;
    unsigned taken;
    unsigned dropped;       /* answers for an old generation, or not wanted */
    unsigned starts;
};

void rift_tiles_init(struct rift_tiles *t);
/* Start the helper. 0, or -1 with t->err (the status is UNAVAILABLE). */
int rift_tiles_start(struct rift_tiles *t, const struct rift_tiles_config *cfg, int64_t now_ms);
/* The tiles in view now (rift_map_tiles, nearest the centre first); sends a
 * want when the set of tiles still to get changed. */
void rift_tiles_want(struct rift_tiles *t, const struct rift_map_tile *tiles, int n, int64_t now_ms);
/* Read what the helper wrote, enforce the deadlines, restart a helper that
 * stopped when one is due. Never blocks. Returns true when a tile arrived
 * or the status changed (the map should be drawn again). */
bool rift_tiles_poll(struct rift_tiles *t, int64_t now_ms);
/* A held tile's pixels, or NULL; *slot is its place (for the map's own
 * bookkeeping) and *stale whether it is an expired copy. Marks it used. */
const uint16_t *rift_tiles_pixels(struct rift_tiles *t, int z, int x, int y, int *slot, bool *stale);
/* End the helper (bounded: about 250 ms at most), remove its directory and
 * free every tile, telling evict first. Idle afterwards. */
void rift_tiles_stop(struct rift_tiles *t);
bool rift_tiles_running(const struct rift_tiles *t);
enum rift_tiles_status rift_tiles_status(const struct rift_tiles *t, const char **why);
/* How many tiles are held. */
int rift_tiles_held(const struct rift_tiles *t);

#endif
