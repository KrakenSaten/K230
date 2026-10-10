/*
 * MAP's basemap on screen: OpenStreetMap tiles under the graticule and the
 * markers, while the reader has BASEMAP on and MAP is the section shown.
 * The helper, the protocol and the tiles held are rift_tiles.h; this is
 * what LVGL needs on top - the tiles drawn as images, a scrim over them so
 * the theme's markers stay legible, and the words for what the basemap is
 * doing.
 *
 * The helper runs only while it is wanted: turning BASEMAP off, leaving MAP
 * for another section, leaving RIFT and CLOSE RIFT all stop it, free the
 * tiles held and remove its directory; nothing is asked for meanwhile.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_BASEMAP_H
#define RIFT_BASEMAP_H

#include "rift_map.h"
#include "rift_tiles.h"

#include "lvgl.h"

#include <stddef.h>
#include <stdint.h>

struct rift_basemap;

struct rift_basemap *rift_basemap_new(void);
/* Stops the helper and lets go of every tile. */
void rift_basemap_free(struct rift_basemap *b);
/* BASEMAP on or off. Off stops the helper at once. */
void rift_basemap_set(struct rift_basemap *b, int on);
int rift_basemap_on(const struct rift_basemap *b);
/* Every pass of the app's timer: the helper started or stopped for whether
 * the basemap is on and MAP is shown, the tiles in v's area asked for, and
 * the answers read. 1 when the map should be drawn again. */
int rift_basemap_pump(struct rift_basemap *b, const struct rift_map_view *v, int shown, int64_t now_ms);
/* Draw the tiles held for v's area, a the map's coordinates, and the scrim
 * over them. Returns how many tiles were drawn. */
int rift_basemap_draw(struct rift_basemap *b, lv_layer_t *layer, const lv_area_t *a,
                      const struct rift_map_view *v);
/* What the basemap is doing, in MAP's words: "" when there is nothing to
 * say (off is said by the BASEMAP button and the panel). detail gets the
 * longer reason, for the panel. */
enum rift_tiles_status rift_basemap_words(const struct rift_basemap *b, char *line, size_t len, char *detail,
                                          size_t dlen);
/* The client, for the tests. */
const struct rift_tiles *rift_basemap_tiles(const struct rift_basemap *b);

#endif
