/* Neon Outpost: bounded, deterministic simulation, no UI or hardware.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef NEON_ENGINE_H
#define NEON_ENGINE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NEON_W 16
#define NEON_H 12
#define NEON_CELLS (NEON_W * NEON_H)
#define NEON_ENEMIES 6
#define NEON_PICKUPS 6
#define NEON_VIEW_W 256
#define NEON_VIEW_H 160
#define NEON_PI 3.14159265358979323846

enum neon_state { NEON_PLAY, NEON_DEAD, NEON_WON };
struct neon_actor { double x, y; int hp, kind, cooldown; };
struct neon_pickup { double x, y; int kind; bool taken; };
struct neon_input { int forward, turn, strafe; bool fire, use; };
struct neon_game {
    char map[NEON_CELLS];
    double x, y, angle;
    int health, ammo, kills, cooldown, flash, hurt, accumulator, nav_clock;
    unsigned steps;
    bool paused, use_down;
    enum neon_state state;
    struct neon_actor enemy[NEON_ENEMIES];
    struct neon_pickup pickup[NEON_PICKUPS];
    int16_t distance[NEON_CELLS];
};
struct neon_ray { double distance, fraction; char cell; int side; };
bool neon_validate(const char *map, size_t length, double x, double y);
void neon_restart(struct neon_game *g);
void neon_pause(struct neon_game *g, bool paused);
/* At most 250 ms accepted per call; fixed 10 ms simulation, no backlog. */
void neon_advance(struct neon_game *g, struct neon_input in, unsigned elapsed_ms);
bool neon_solid(const struct neon_game *g, int x, int y);
struct neon_ray neon_cast(const struct neon_game *g, double x, double y, double dx, double dy);
bool neon_visible(const struct neon_game *g, double x, double y, double tx, double ty);
/* Fixed RGB565 output, exact size required. No heap or external assets. */
bool neon_render(const struct neon_game *g, uint16_t *pixels, size_t count);
#endif
