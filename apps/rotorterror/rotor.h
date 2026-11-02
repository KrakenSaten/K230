/* RotorTerror: bounded, fixed-step arcade simulation; no UI, I/O or hardware.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef ROTORTERROR_H
#define ROTORTERROR_H

#include <stdbool.h>
#include <stdint.h>

#define RT_W 616
#define RT_MAX_H 720
#define RT_ENEMIES 40
#define RT_SHOTS 224
#define RT_PICKUPS 64
#define RT_PARTICLES 384
#define RT_SOUNDS 17
#define RT_STEP_MS 16
#define RT_WAVE_MS 26000

enum rt_phase { RT_TITLE, RT_FLIGHT, RT_HANGAR, RT_OVER };
enum rt_enemy_kind { RT_DRONE, RT_INTERCEPTOR, RT_MINE, RT_TURRET, RT_BOSS };
enum rt_upgrade { RT_SHIELD, RT_CANNON, RT_ROCKETS, RT_REPAIR, RT_UPGRADES };
/* First ten IDs belong to the existing effects bank. Its recipes stay intact. */
enum rt_sound { RT_GUN = 10, RT_LAUNCH, RT_BOOM, RT_SALVAGE, RT_DEFLECT,
                RT_ROTOR, RT_MUSIC };

struct rt_input { float lift, move, aim; bool rocket; };
struct rt_enemy {
    float x, y, origin_y, vx, radius;
    int hp, max_hp, age, fire_ms, hit_ms;
    unsigned kind, serial;
    bool active;
};
struct rt_shot {
    float x, y, vx, vy;
    int damage, age;
    bool active, hostile, rocket;
};
struct rt_pickup { float x, y; unsigned kind; int age; bool active; };
struct rt_particle {
    float x, y, vx, vy;
    int life, max_life;
    unsigned color, size;
};
struct rt_game {
    enum rt_phase phase;
    bool paused, reduced_motion, boss_spawned;
    unsigned difficulty, wave, level[3], rng, serial;
    uint32_t score, credits, kills, steps;
    int height, hull, shield_max, rockets, rocket_max;
    float x, y, vy, angle, shield, distance;
    int elapsed_ms, wave_ms, accumulator, gun_ms, rocket_ms, reload_ms;
    int spawn_ms, pickup_ms, hurt_ms, safe_ms, hit_ms, combo_ms, combo;
    int rotor_ms, music_ms, banner_ms, shake_ms;
    unsigned sounds[RT_SOUNDS], particles_next;
    struct rt_enemy enemies[RT_ENEMIES];
    struct rt_shot shots[RT_SHOTS];
    struct rt_pickup pickups[RT_PICKUPS];
    struct rt_particle particles[RT_PARTICLES];
};

void rt_init(struct rt_game *g, uint32_t seed);
void rt_start(struct rt_game *g, unsigned difficulty, uint32_t seed);
void rt_resize(struct rt_game *g, int height);
void rt_advance(struct rt_game *g, struct rt_input input, unsigned ms);
bool rt_buy(struct rt_game *g, enum rt_upgrade item);
unsigned rt_price(const struct rt_game *g, enum rt_upgrade item);
void rt_next_wave(struct rt_game *g);
void rt_damage(struct rt_game *g, int damage);
const char *rt_sector(unsigned wave);
const char *rt_difficulty(unsigned difficulty);

struct rt_frame { uint16_t pixels[RT_W * RT_MAX_H]; int height; };
void rt_render(const struct rt_game *g, struct rt_frame *frame, bool title);

#endif
