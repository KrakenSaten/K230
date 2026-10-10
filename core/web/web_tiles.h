/*
 * Map tiles for RIFT's basemap: the line protocol between RIFT (in the
 * shell) and `pos-browser tiles`, and the helper's disk cache. The fetching,
 * the cache and the decoding are the helper's; the shell links none of it
 * (tests/browser_lint.sh), and receives only decoded pixels.
 *
 * THE PROTOCOL. One line per message, tab-separated, every line well within
 * WEB_LINE_MAX. Coordinates are a tile's z, x and y in the slippy-map
 * scheme (0 <= x, y < 2^z, y = 0 at the north edge).
 *
 *   Shell to helper:
 *
 *     want  <gen> <z/x/y> <z/x/y> ...   the tiles in view now, nearest the
 *                                       centre first, at most WEB_TILE_WANT_MAX;
 *                                       none at all is a cancel. Replaces any
 *                                       want before it.
 *     quit
 *
 *   Helper to shell:
 *
 *     hello  <proto> <features>         "net" or "fake", ",img:png" when it decodes
 *     tile   <gen> <z> <x> <y> <w> <h> <file> <source> <why>
 *            a picture of w x h RGB565 pixels in <file> (web_rx_file_name_ok)
 *            in the session's directory. source: cache (fresh, nothing was
 *            asked), valid (asked, 304), net (fetched), stale (an expired
 *            copy, because asking failed: why says how; "-" otherwise)
 *     notile <gen> <z> <x> <y> <why>    no picture for it: offline, dns,
 *            connect, timeout, tls, clock, http-<status>, broken, ...
 *     idle   <gen>                      every tile of gen is answered
 *
 * The helper works through a want in its order, one tile at a time. A new
 * want between two tiles replaces the rest; one that arrives during a
 * transfer stops that transfer at once unless the tile is in the new want
 * too, in which case it finishes and is answered under the new gen. An
 * answer always carries the gen it was asked under, so the shell can drop
 * whatever a view it has left asked for.
 *
 * FRESHNESS follows the tile server's own headers (the OpenStreetMap tile
 * usage policy asks for exactly that): Cache-Control max-age, no-cache and
 * no-store; else Expires, read against the answer's Date; else
 * WEB_TILE_FALLBACK_S, the policy's seven days. An expired tile is asked for
 * again with If-None-Match / If-Modified-Since, and a 304 renews it. A tile
 * whose renewal fails (no network, a server error) is shown as it is,
 * marked stale: the cache is what keeps the map usable offline.
 *
 * THE CACHE is a directory the shell made, 0700, holding "<z>-<x>-<y>.png"
 * (the server's bytes) and "<z>-<x>-<y>.meta" (the validators and the
 * expiry) for each tile kept. A tile's mtime is when it was last shown; the
 * least recently shown go first once the cache passes its byte or file
 * limit, and nothing new is written while the file system has less than
 * WEB_TILE_DISK_RESERVE free. Every file is written to a temporary name and
 * renamed, never through a link; a pair that does not agree is not a tile.
 * Nothing is fetched that was not asked for, and only one tile at a time.
 *
 * Pure C apart from the files: tests/web_tiles_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_WEB_TILES_H
#define POCKETOS_WEB_TILES_H

#include "web/web_fetch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEB_TILE_PROTO_VERSION 1
#define WEB_TILE_SIZE 256
#define WEB_TILE_ZOOM_MAX 19
/* The most tiles one want names: more than any map area on this panel
 * shows (a full 1232 x 568 screen is 6 x 4 tiles at worst). */
#define WEB_TILE_WANT_MAX 48
/* The most a tile may be when fetched; a tile server's are a few tens of KB. */
#define WEB_TILE_BYTES_MAX (256u * 1024u)
#define WEB_TILE_CONNECT_TIMEOUT_MS 10000
#define WEB_TILE_TIMEOUT_MS 20000
/* Kept when the answer said nothing about how long (the OSM tile policy's
 * "at least 7 days"). */
#define WEB_TILE_FALLBACK_S (7 * 24 * 3600)
/* The longest a max-age is believed. */
#define WEB_TILE_MAX_AGE_S (365 * 24 * 3600)
/* A wall clock before this has not been set (no RTC: 1970 until NTP). */
#define WEB_TILE_CLOCK_MIN 1700000000
/* The cache's limits, and the free space it always leaves. */
#define WEB_TILE_CACHE_BYTES (32u * 1024u * 1024u)
#define WEB_TILE_CACHE_FILES 2048u
#define WEB_TILE_DISK_RESERVE (64ull * 1024u * 1024u)

struct web_tile_meta {
    int64_t fetched;        /* wall clock, seconds */
    int64_t expires;
    char etag[WEB_HDR_LONG];
    char last_modified[WEB_HDR_SHORT];
    size_t size;            /* of the .png beside it */
};

struct web_tile_cache {
    char dir[512];
    bool ok;                /* the directory was accepted */
    uint64_t bytes;         /* what the tiles in it take, as counted */
    unsigned files;         /* tiles in it */
    uint64_t max_bytes;
    unsigned max_files;
    uint64_t reserve;       /* free space never used */
    unsigned pruned;        /* tiles removed for room, for the tests */
};

/* A tile's coordinates are in range. */
bool web_tile_valid(int z, int x, int y);

/* Use dir (which must exist, be a directory and not a link, and be reachable
 * by its owner alone) with these limits; 0 for a limit is its default. Counts
 * what is there and removes temporary and unpaired files. -1 when the
 * directory is refused: c->ok is false and nothing is ever written. */
int web_tile_cache_open(struct web_tile_cache *c, const char *dir, uint64_t max_bytes, unsigned max_files);

/* The kept copy: 0 with *body malloc'd (NUL after len), or -1 when there is
 * none (or its two files do not agree, which removes them). */
int web_tile_cache_read(struct web_tile_cache *c, int z, int x, int y, struct web_tile_meta *m, char **body,
                        size_t *len);
/* Keep a copy, then make room. 0, or -1 when it was not written (no room on
 * the disk, an I/O error); a copy that was there before is gone then. */
int web_tile_cache_write(struct web_tile_cache *c, int z, int x, int y, const struct web_tile_meta *m,
                         const char *body, size_t len);
/* A 304: new expiry and validators, the same picture. */
int web_tile_cache_renew(struct web_tile_cache *c, int z, int x, int y, const struct web_tile_meta *m);
/* Shown now (its mtime). */
void web_tile_cache_touch(struct web_tile_cache *c, int z, int x, int y);
void web_tile_cache_remove(struct web_tile_cache *c, int z, int x, int y);
/* Remove the least recently shown tiles until the cache is at most these
 * (0: nine tenths of its limits). */
void web_tile_cache_prune(struct web_tile_cache *c, uint64_t keep_bytes, unsigned keep_files);

/* When an answer received at now expires, by its headers (see above). *store
 * is false for no-store. */
int64_t web_tile_expiry(const struct web_cache_hdrs *h, int64_t now, bool *store);
/* An HTTP-date (IMF-fixdate, RFC 850 or asctime) as seconds since 1970, or
 * -1 when it is not one. */
int64_t web_http_date(const char *s);

/* ---- one tile -------------------------------------------------------------------- */

enum web_tile_source {
    WEB_TILE_NONE = 0,      /* no picture */
    WEB_TILE_CACHE,         /* kept and fresh: nothing was asked */
    WEB_TILE_VALID,         /* kept, expired, and the server said 304 */
    WEB_TILE_NET,           /* fetched */
    WEB_TILE_STALE          /* kept and expired, and asking failed (or could not be done) */
};

struct web_tile_net {
    struct web_fetcher *f;
    const char *url_template;   /* https://host/{z}/{x}/{y}.png */
    const char *user_agent;
    const char *ca_file;
    /* false: do not ask now (the helper is backing off); kept copies only,
     * and why_not says why for the rest. */
    bool allow;
    char why_not[24];
    int (*abort_cb)(void *ctx);
    void *ctx;
};

struct web_tile_result {
    enum web_tile_source source;
    char *body;             /* malloc'd, the server's bytes; NULL with NONE */
    size_t len;
    bool asked;             /* a request went out */
    bool stopped;           /* asked to stop: nothing to answer */
    long http;              /* the final status, when one came */
    enum web_fail fail;     /* when asking failed */
    char why[24];           /* for the line: "-" or what went wrong */
};

/* The template filled in, or -1 when it is not an https template with all
 * three of {z}, {x} and {y}. */
int web_tile_url(const char *tmpl, int z, int x, int y, char *out, size_t cap);

/* Get one tile, from the cache or the network, by the rules above. now is
 * the wall clock in seconds. */
void web_tile_get(struct web_tile_cache *c, struct web_tile_net *net, int z, int x, int y, int64_t now,
                  struct web_tile_result *r);
void web_tile_result_free(struct web_tile_result *r);

#endif
