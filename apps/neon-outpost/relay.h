/* Relay Nine: original bounded sector-world simulation. No UI or hardware.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef RELAY_NINE_H
#define RELAY_NINE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define RN_SECTORS 96
#define RN_EDGES 512
#define RN_ENEMIES 16
#define RN_ITEMS 16
#define RN_W 320
#define RN_H 200
#define RN_PI 3.14159265358979323846
#define RN_EYE 1.45
#define RN_BODY 1.65
#define RN_RADIUS .22

enum rn_state { RN_PLAY, RN_DEAD, RN_WON };
enum rn_sound { RN_SHOT, RN_IMPACT, RN_EMPTY, RN_RELOAD, RN_WINDUP, RN_ATTACK, RN_DOOR, RN_PICKUP, RN_SCATTER, RN_LOAD_SHELL, RN_SOUNDS };
enum rn_weapon_id { RN_CARBINE, RN_SHOTGUN };
enum rn_pickup { RN_AMMO, RN_MED, RN_GUN, RN_SHELLS };
struct rn_edge { float ax,ay,bx,by; int neighbor,door,material; };
struct rn_sector { float x0,y0,x1,y1,floor,ceiling; int first,count,room,light; bool prop; };
struct rn_actor { double x,y,z,angle; int sector,hp,kind,cooldown,windup,hit,death,flash,walk; double aimx,aimy,aimz; bool awake; };
struct rn_item { double x,y,z; int sector,kind; bool taken; };
struct rn_input { double forward,strafe,turn; bool fire,use,reload,switch_weapon; };
struct rn_hit { double t,x,y,z,u; int edge,sector; bool floor; };
struct rn_game {
    struct rn_sector sectors[RN_SECTORS]; struct rn_edge edges[RN_EDGES];
    int sector_count,edge_count;
    double x,y,z,angle,velocity_z;
    int sector,health,mag,reserve,kills,cooldown,reloading,flash,hurt,accumulator,nav_clock;
    enum rn_state state; bool paused,use_down,reload_down,switch_down,relay,slice,reduced_motion,shotgun,secret;
    int weapon,shells,loaded,notice,notice_ms,hitmark;
    float door[5]; bool door_want[5];
    struct rn_actor enemy[RN_ENEMIES]; int enemy_count;
    struct rn_item item[RN_ITEMS]; int item_count;
    int16_t route[RN_SECTORS];
    unsigned steps,sounds[RN_SOUNDS],ray_caps;
    double impact_x,impact_y,impact_z; int impact_ms;
};
struct rn_frame { uint16_t pixels[RN_W*RN_H],depth[RN_W*RN_H]; unsigned columns,portals,sprite_pixels,cap_hits; };
bool rn_restart(struct rn_game *g, bool slice);
bool rn_validate(const struct rn_game *g);
int rn_locate(const struct rn_game *g,double x,double y);
void rn_opening(const struct rn_game *g,int sector,const struct rn_edge *e,double *lo,double *hi);
int rn_boundary(const struct rn_game *g,int s,double x,double y,double dx,double dy,double after,double *t,double *u);
struct rn_hit rn_trace(const struct rn_game *g,int sector,double x,double y,double z,double dx,double dy,double dz,double limit);
bool rn_los(const struct rn_game *g,int sector,double x,double y,double z,double tx,double ty,double tz);
void rn_switch_weapon(struct rn_game *g);
void rn_pause(struct rn_game *g,bool pause);
void rn_advance(struct rn_game *g,struct rn_input in,unsigned ms);
bool rn_render(const struct rn_game *g,struct rn_frame *frame);
const char *rn_room_name(int room);
uint16_t rn_texture(int material,int x,int y,int light);
uint16_t rn_sprite(int kind,int x,int y,int phase,int facing,int state);
uint16_t rn_weapon(int x,int y,int flash,int reload,int weapon,bool reduced_motion);
void rn_title(struct rn_frame *frame);
const char *rn_notice(const struct rn_game *g);
#endif
