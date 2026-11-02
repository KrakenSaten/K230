/* RotorTerror: automatic forward flight, aimed weapons, waves and salvage.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "rotor.h"
#include <math.h>
#include <string.h>

static float clampf(float x, float low, float high)
{
    return x < low ? low : x > high ? high : x;
}

static uint32_t random32(struct rt_game *g)
{
    uint32_t n = g->rng;
    n ^= n << 13;
    n ^= n >> 17;
    n ^= n << 5;
    return g->rng = n;
}

static float randomf(struct rt_game *g, float low, float high)
{
    return low + (random32(g) & 65535u) * ((high - low) / 65535.0f);
}

static void sound(struct rt_game *g, enum rt_sound id) { g->sounds[id]++; }

const char *rt_sector(unsigned wave)
{
    static const char *const names[] = { "EMBER COAST", "VIOLET RIDGE", "ION CITY" };
    return names[((wave ? wave : 1) - 1) / 3 % 3];
}

const char *rt_difficulty(unsigned difficulty)
{
    static const char *const names[] = { "PILOT", "ACE", "TERROR" };
    return names[difficulty < 3 ? difficulty : 0];
}

void rt_init(struct rt_game *g, uint32_t seed)
{
    memset(g, 0, sizeof(*g));
    g->phase = RT_TITLE;
    g->rng = seed ? seed : 0x524f544fu;
    g->height = 284;
    g->hull = 100;
    g->shield_max = 100;
    g->shield = 100;
    g->x = 128;
    g->y = 142;
    g->wave = 1;
    g->rocket_max = g->rockets = 3;
    g->combo = 1;
}

void rt_start(struct rt_game *g, unsigned difficulty, uint32_t seed)
{
    int height = g->height;
    bool reduced_motion = g->reduced_motion;
    rt_init(g, seed);
    g->phase = RT_FLIGHT;
    g->difficulty = difficulty < 3 ? difficulty : 0;
    g->height = height;
    g->y = height * .5f;
    g->reduced_motion = reduced_motion;
    g->spawn_ms = 1100;
    g->pickup_ms = 900;
    g->banner_ms = 2300;
}

void rt_resize(struct rt_game *g, int height)
{
    if (height < 240) height = 240;
    if (height > RT_MAX_H) height = RT_MAX_H;
    if (height == g->height) return;
    float scale = (height - 100.0f) / (g->height - 100.0f);
    g->y = 50 + (g->y - 50) * scale;
    for (int i = 0; i < RT_ENEMIES; i++) {
        g->enemies[i].y = 50 + (g->enemies[i].y - 50) * scale;
        g->enemies[i].origin_y = 50 + (g->enemies[i].origin_y - 50) * scale;
    }
    for (int i = 0; i < RT_SHOTS; i++) g->shots[i].y = 50 + (g->shots[i].y - 50) * scale;
    for (int i = 0; i < RT_PICKUPS; i++) g->pickups[i].y = 50 + (g->pickups[i].y - 50) * scale;
    memset(g->particles, 0, sizeof(g->particles));
    g->height = height;
}

static void particle(struct rt_game *g, float x, float y, float vx, float vy,
                     unsigned color, int life, unsigned size)
{
    struct rt_particle *p = &g->particles[g->particles_next++ % RT_PARTICLES];
    *p = (struct rt_particle){ x, y, vx, vy, life, life, color, size };
}

static void explosion(struct rt_game *g, float x, float y, bool large)
{
    unsigned count = g->reduced_motion ? 10 : large ? 60 : 28;
    for (unsigned i = 0; i < count; i++) {
        float angle = randomf(g, -3.141593f, 3.141593f);
        float speed = randomf(g, 20, large ? 175 : 100);
        particle(g, x, y, cosf(angle) * speed - 25, sinf(angle) * speed,
                 i % 5, (int)randomf(g, 240, large ? 1100 : 650), 1 + i % 4);
    }
    g->shake_ms = g->reduced_motion ? 0 : large ? 230 : 90;
    sound(g, RT_BOOM);
}

void rt_damage(struct rt_game *g, int damage)
{
    if (g->phase != RT_FLIGHT || g->safe_ms > 0 || damage <= 0) return;
    float blocked = fminf(g->shield, (float)damage);
    g->shield -= blocked;
    g->hull -= (int)ceilf(damage - blocked);
    g->hurt_ms = 2500;
    g->safe_ms = 260;
    g->hit_ms = 240;
    g->combo = 1;
    g->combo_ms = 0;
    sound(g, RT_DEFLECT);
    if (g->hull <= 0) {
        g->hull = 0;
        explosion(g, g->x, g->y, true);
        g->phase = RT_OVER;
    }
}

static void add_pickup(struct rt_game *g, float x, float y, unsigned kind)
{
    for (int i = 0; i < RT_PICKUPS; i++) if (!g->pickups[i].active) {
        g->pickups[i] = (struct rt_pickup){ x, y, kind, 0, true };
        return;
    }
}

static void kill_enemy(struct rt_game *g, struct rt_enemy *e)
{
    e->active = false;
    explosion(g, e->x, e->y, e->kind == RT_BOSS);
    g->kills++;
    if (g->combo_ms && g->combo < 8) g->combo++;
    g->combo_ms = 2800;
    unsigned value = e->kind == RT_BOSS ? 1000 : e->kind == RT_INTERCEPTOR ? 120 : 60;
    g->score += value * (unsigned)g->combo * (g->difficulty + 1);
    add_pickup(g, e->x, e->y, 0);
    if (e->kind == RT_BOSS) {
        g->credits += 160;
        add_pickup(g, e->x - 20, e->y - 24, 1);
        add_pickup(g, e->x + 20, e->y + 24, 2);
    } else if (random32(g) % 6 == 0) {
        add_pickup(g, e->x + 18, e->y - 12, 1 + random32(g) % 2);
    }
}

static void shot(struct rt_game *g, float x, float y, float angle,
                 bool hostile, bool rocket, int damage)
{
    for (int i = 0; i < RT_SHOTS; i++) if (!g->shots[i].active) {
        float speed = hostile ? 145 + 28 * g->difficulty : rocket ? 290 : 590;
        g->shots[i] = (struct rt_shot){ x, y, cosf(angle) * speed, sinf(angle) * speed,
                                      damage, 0, true, hostile, rocket };
        return;
    }
}

static void spawn_enemy(struct rt_game *g, unsigned kind, float x, float y)
{
    for (int i = 0; i < RT_ENEMIES; i++) if (!g->enemies[i].active) {
        struct rt_enemy *e = &g->enemies[i];
        memset(e, 0, sizeof(*e));
        e->active = true;
        e->kind = kind;
        e->x = x;
        e->y = e->origin_y = y;
        e->serial = g->serial++;
        e->vx = -(55 + 12 * g->difficulty + fminf(g->wave, 20) * 3);
        e->hp = kind == RT_BOSS ? 500 + 95 * g->wave :
                kind == RT_INTERCEPTOR ? 42 + 2 * g->wave : kind == RT_TURRET ? 65 : 18;
        e->max_hp = e->hp;
        e->radius = kind == RT_BOSS ? 56 : kind == RT_INTERCEPTOR ? 23 : 14;
        e->fire_ms = kind == RT_BOSS ? 800 : 1800 + (int)(random32(g) % 1000);
        return;
    }
}

static bool near(float ax, float ay, float bx, float by, float radius)
{
    float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy < radius * radius;
}

static void advance_shots(struct rt_game *g)
{
    for (int i = 0; i < RT_SHOTS; i++) {
        struct rt_shot *s = &g->shots[i];
        if (!s->active) continue;
        float oldx = s->x, oldy = s->y;
        if (s->rocket && !s->hostile) {
            struct rt_enemy *target = NULL;
            float distance = 1000000;
            for (int j = 0; j < RT_ENEMIES; j++) {
                struct rt_enemy *e = &g->enemies[j];
                float d = (e->x-s->x)*(e->x-s->x) + (e->y-s->y)*(e->y-s->y);
                if (e->active && e->x > s->x && d < distance) { target = e; distance = d; }
            }
            if (target) {
                float a = atan2f(target->y-s->y, target->x-s->x);
                s->vx += (cosf(a)*360 - s->vx)*.065f;
                s->vy += (sinf(a)*360 - s->vy)*.065f;
            }
            if (!g->reduced_motion || g->steps % 3 == 0)
                particle(g, s->x-5, s->y, -35, randomf(g,-12,12), 3, 340, 2);
        }
        s->x += s->vx * .016f;
        s->y += s->vy * .016f;
        s->age += RT_STEP_MS;
        if (s->x < -30 || s->x > RT_W+40 || s->y < 25 || s->y > g->height-20 || s->age > 4000) {
            s->active = false;
            continue;
        }
        if (s->hostile) {
            if (near(s->x,s->y,g->x,g->y,22)) { s->active=false; rt_damage(g,s->damage); }
            continue;
        }
        /* Swept segment collision keeps fast cannon rounds from tunnelling. */
        for (int j = 0; j < RT_ENEMIES; j++) {
            struct rt_enemy *e = &g->enemies[j];
            if (!e->active) continue;
            float dx=s->x-oldx, dy=s->y-oldy, den=dx*dx+dy*dy;
            float t=den>0?clampf(((e->x-oldx)*dx+(e->y-oldy)*dy)/den,0,1):0;
            if (!near(oldx+t*dx,oldy+t*dy,e->x,e->y,e->radius)) continue;
            s->active=false;
            e->hp-=s->damage;
            e->hit_ms=90;
            particle(g,s->x,s->y,-30,-25,0,150,3);
            if (s->rocket) {
                explosion(g,s->x,s->y,false);
                for (int k=0;k<RT_ENEMIES;k++) {
                    struct rt_enemy *other=&g->enemies[k];
                    if (other!=e && other->active && near(e->x,e->y,other->x,other->y,65)) {
                        other->hp-=s->damage/2;
                        if (other->hp<=0) kill_enemy(g,other);
                    }
                }
            }
            if (e->hp<=0) kill_enemy(g,e);
            break;
        }
    }
}

static void tick(struct rt_game *g, struct rt_input in)
{
    g->steps++;
    g->elapsed_ms += RT_STEP_MS;
    g->wave_ms += RT_STEP_MS;
    g->distance += (70 + g->difficulty*15) * .016f;
    if (g->hurt_ms>0) g->hurt_ms-=RT_STEP_MS;
    if (g->safe_ms>0) g->safe_ms-=RT_STEP_MS;
    if (g->hit_ms>0) g->hit_ms-=RT_STEP_MS;
    if (g->banner_ms>0) g->banner_ms-=RT_STEP_MS;
    if (g->shake_ms>0) g->shake_ms-=RT_STEP_MS;
    if (g->combo_ms>0) { g->combo_ms-=RT_STEP_MS; if(g->combo_ms<=0)g->combo=1; }
    if (g->hurt_ms<=0) g->shield=fminf(g->shield_max,g->shield+(5+g->level[0]*2)*.016f);
    in.lift=clampf(in.lift,-1,1);
    in.move=clampf(in.move,-1,1);
    in.aim=clampf(in.aim,-1,1);
    g->vy+=(in.lift*195-g->vy)*.15f;
    g->y=clampf(g->y+g->vy*.016f,62,g->height-58);
    g->x=clampf(g->x+in.move*155*.016f,70,RT_W*.55f);
    float target=in.aim*.72f+in.lift*.26f;
    g->angle+=(target-g->angle)*.12f;
    g->gun_ms-=RT_STEP_MS;
    if (g->gun_ms<=0) {
        g->gun_ms=115-(int)g->level[1]*14;
        shot(g,g->x+29,g->y+6,g->angle,false,false,9+g->level[1]*3);
        if (g->level[1]>=3) shot(g,g->x+26,g->y-2,g->angle-.055f,false,false,9);
        sound(g,RT_GUN);
    }
    g->rocket_ms-=RT_STEP_MS;
    if (in.rocket && g->rockets>0 && g->rocket_ms<=0) {
        shot(g,g->x+20,g->y+13,g->angle,false,true,85+g->level[2]*25);
        g->rockets--;
        g->rocket_ms=500;
        sound(g,RT_LAUNCH);
    }
    if(g->rockets<g->rocket_max) {
        g->reload_ms+=RT_STEP_MS;
        if(g->reload_ms>=3100-(int)g->level[2]*350) { g->rockets++;g->reload_ms=0; }
    } else g->reload_ms=0;
    g->rotor_ms-=RT_STEP_MS;
    if(g->rotor_ms<=0) { g->rotor_ms=384;sound(g,RT_ROTOR); }
    g->music_ms-=RT_STEP_MS;
    if(g->music_ms<=0) { g->music_ms=1536;sound(g,RT_MUSIC); }
    if(g->steps%3==0 && !g->reduced_motion)
        particle(g,g->x-29,g->y+3,-60,randomf(g,-6,6),4,260,2);

    g->spawn_ms-=RT_STEP_MS;
    if(g->spawn_ms<=0 && g->wave_ms<RT_WAVE_MS-3000) {
        unsigned r=random32(g)%100;
        unsigned kind=r<48?RT_DRONE:r<73?RT_INTERCEPTOR:r<89?RT_MINE:RT_TURRET;
        if(g->wave==1 && g->wave_ms<5000)kind=RT_DRONE;
        float y=kind==RT_TURRET?g->height-55:randomf(g,75,g->height-85);
        spawn_enemy(g,kind,RT_W+32,y);
        if(kind==RT_DRONE && g->wave_ms>4000 && random32(g)%3==0) {
            spawn_enemy(g,kind,RT_W+82,clampf(y-35,70,g->height-80));
            spawn_enemy(g,kind,RT_W+132,clampf(y+35,70,g->height-80));
        }
        g->spawn_ms=(int)fmaxf(330,1300-g->difficulty*270-fminf(g->wave,18)*38);
    }
    if(g->wave%3==0 && !g->boss_spawned && g->wave_ms>=12500) {
        spawn_enemy(g,RT_BOSS,RT_W+100,g->height*.43f);
        g->boss_spawned=true;
        g->banner_ms=2600;
    }
    bool boss_alive=false;
    for(int i=0;i<RT_ENEMIES;i++) {
        struct rt_enemy *e=&g->enemies[i];
        if(!e->active)continue;
        e->age+=RT_STEP_MS;
        if(e->hit_ms>0)e->hit_ms-=RT_STEP_MS;
        if(e->kind==RT_BOSS) {
            boss_alive=true;
            e->x+=((RT_W-115)-e->x)*.012f;
            e->y=g->height*.43f+sinf(e->age*.0014f)*(g->height*.20f);
        } else {
            e->x+=e->vx*.016f;
            if(e->kind==RT_DRONE || e->kind==RT_INTERCEPTOR)
                e->y=e->origin_y+sinf(e->age*.0025f+e->serial)*18;
        }
        if(e->x < -100) { e->active=false;continue; }
        if(e->kind!=RT_MINE) {
            e->fire_ms-=RT_STEP_MS;
            if(e->fire_ms<=0 && e->x<RT_W-20 && e->x>g->x+40 && e->age>750) {
                float a=atan2f(g->y-e->y,g->x-e->x);
                shot(g,e->x-20,e->y,a,true,false,12+g->difficulty*5);
                if(e->kind==RT_BOSS) {
                    shot(g,e->x-20,e->y,a-.22f,true,false,18);
                    shot(g,e->x-20,e->y,a+.22f,true,false,18);
                }
                e->fire_ms=e->kind==RT_BOSS?700:2500-(int)g->difficulty*400;
            }
        }
        if(e->age>650 && near(g->x,g->y,e->x,e->y,e->radius+18)) {
            rt_damage(g,e->kind==RT_MINE?36:25);
            if(e->kind!=RT_BOSS) { e->hp=0;kill_enemy(g,e); }
        }
    }
    advance_shots(g);
    if(g->phase!=RT_FLIGHT)return;
    g->pickup_ms-=RT_STEP_MS;
    if(g->pickup_ms<=0 && g->wave_ms<RT_WAVE_MS-1500) {
        float y=randomf(g,80,g->height-80);
        for(int i=0;i<5;i++)add_pickup(g,RT_W+25+i*24,y+sinf(i*.75f)*24,0);
        g->pickup_ms=5100;
    }
    for(int i=0;i<RT_PICKUPS;i++) {
        struct rt_pickup *p=&g->pickups[i];
        if(!p->active)continue;
        p->x-=70*.016f;
        p->age+=RT_STEP_MS;
        if(p->x< -20 || p->age>16000) { p->active=false;continue; }
        float range=90+g->level[0]*8;
        if(near(p->x,p->y,g->x,g->y,range)) {
            p->x+=(g->x-p->x)*.075f;
            p->y+=(g->y-p->y)*.075f;
        }
        if(near(p->x,p->y,g->x,g->y,25)) {
            p->active=false;
            if(p->kind==0) { g->credits+=12;g->score+=25*(g->difficulty+1); }
            else if(p->kind==1)g->shield=fminf(g->shield_max,g->shield+32);
            else g->rockets=g->rocket_max;
            sound(g,RT_SALVAGE);
            for(int j=0;j<6;j++)particle(g,p->x,p->y,randomf(g,-35,35),randomf(g,-45,20),p->kind==0?2:4,300,2);
        }
    }
    for(int i=0;i<RT_PARTICLES;i++) {
        struct rt_particle *p=&g->particles[i];
        if(p->life<=0)continue;
        p->life-=RT_STEP_MS;
        p->x+=p->vx*.016f;p->y+=p->vy*.016f;
        p->vx*=.985f;
        p->vy+=5*.016f;
    }
    if(g->wave_ms>=RT_WAVE_MS && !boss_alive) {
        g->phase=RT_HANGAR;
        g->credits+=90+g->wave*10;
        g->score+=500*g->wave*(g->difficulty+1);
        g->shield=fminf(g->shield_max,g->shield+25);
        g->paused=false;
        memset(g->shots,0,sizeof(g->shots));
        sound(g,RT_SALVAGE);
    }
}

void rt_advance(struct rt_game *g, struct rt_input input, unsigned ms)
{
    if(g->phase!=RT_FLIGHT || g->paused)return;
    /* A stalled UI never catches up with seconds of invisible gameplay. */
    if(ms>250) { g->paused=true;g->accumulator=0;return; }
    g->accumulator+=(int)ms;
    while(g->accumulator>=RT_STEP_MS && g->phase==RT_FLIGHT && !g->paused) {
        g->accumulator-=RT_STEP_MS;
        tick(g,input);
    }
}

unsigned rt_price(const struct rt_game *g, enum rt_upgrade item)
{
    if(item<RT_SHIELD || item>=RT_UPGRADES)return 0;
    if(item==RT_REPAIR)return g->hull<100?65:0;
    if(g->level[item]>=4)return 0;
    return 80+g->level[item]*65;
}

bool rt_buy(struct rt_game *g, enum rt_upgrade item)
{
    if(g->phase!=RT_HANGAR || item<RT_SHIELD || item>=RT_UPGRADES)return false;
    unsigned cost=rt_price(g,item);
    if(!cost || g->credits<cost)return false;
    g->credits-=cost;
    if(item==RT_REPAIR)g->hull=100;
    else {
        g->level[item]++;
        if(item==RT_SHIELD) { g->shield_max+=30;g->shield=g->shield_max; }
        if(item==RT_ROCKETS) { g->rocket_max++;g->rockets=g->rocket_max; }
    }
    sound(g,RT_SALVAGE);
    return true;
}

void rt_next_wave(struct rt_game *g)
{
    if(g->phase!=RT_HANGAR)return;
    g->phase=RT_FLIGHT;
    if(g->wave<999)g->wave++;
    g->wave_ms=0;
    g->spawn_ms=650;
    g->boss_spawned=false;
    g->banner_ms=2300;
    g->safe_ms=1200;
    g->accumulator=0;
    g->rocket_ms=0;
    g->rockets=g->rocket_max;
    memset(g->enemies,0,sizeof(g->enemies));
    memset(g->pickups,0,sizeof(g->pickups));
}
