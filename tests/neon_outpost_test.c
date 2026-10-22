/* Production simulation, ray and renderer regression checks, including a full input-driven run.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "neon.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); assert(x); } } while (0)
static struct neon_input none;
static void quiet(struct neon_game *g)
{
    neon_restart(g);
    for (int i = 0; i < NEON_ENEMIES; ++i) g->enemy[i].hp = 0;
}
#include "neon_pilot.h"
static void full_run(void)
{
    struct neon_game g; neon_restart(&g);
    const double tour[][2] = {{5.5,2.5},{3.5,8.5},{5.5,9.5},{10.5,7.5},{13.5,6.5},{10.5,2.5},{9.5,3.5},{14.5,9.5}};
    unsigned goal = 0;
    for (int t = 0; t < 40000 && g.state == NEON_PLAY; ++t) {
        if (goal < 7 && hypot(g.x-tour[goal][0],g.y-tour[goal][1]) < .35) ++goal;
        struct neon_input in = pilot(&g,tour[goal][0],tour[goal][1]);
        if (goal == 7 && hypot(g.x-14.5,g.y-9.5) < .35) in.use = true;
        neon_advance(&g,in,10);
    }
    printf("full run: state=%d hp=%d ammo=%d kills=%d steps=%u position=%.2f,%.2f goal=%u\n",
           g.state,g.health,g.ammo,g.kills,g.steps,g.x,g.y,goal);
    CHECK(g.state == NEON_WON); CHECK(g.kills == NEON_ENEMIES); CHECK(g.health > 0); CHECK(g.ammo >= 0);
}
int main(void)
{
    struct neon_game g,a,b; neon_restart(&g);
    CHECK(neon_validate(g.map,sizeof(g.map),g.x,g.y));
    for (int i = 0; i < NEON_ENEMIES; ++i) CHECK(!neon_solid(&g,(int)g.enemy[i].x,(int)g.enemy[i].y));
    for (int i = 0; i < NEON_PICKUPS; ++i) CHECK(!neon_solid(&g,(int)g.pickup[i].x,(int)g.pickup[i].y));
    CHECK(!neon_validate(NULL,NEON_CELLS,2,2)); CHECK(!neon_validate(g.map,1,2,2));
    CHECK(!neon_validate(g.map,NEON_CELLS,NAN,2)); CHECK(!neon_validate(g.map,NEON_CELLS,0,0));
    CHECK(!neon_validate(g.map,NEON_CELLS,1.01,1.01));
    a = g; a.map[2*NEON_W+6] = '#'; CHECK(!neon_validate(a.map,NEON_CELLS,2,2));
    a = g; a.map[0] = 'D'; CHECK(!neon_validate(a.map,NEON_CELLS,2,2));
    a = g; a.map[2*NEON_W+7] = '#'; a.map[5*NEON_W+12] = '#'; a.map[7*NEON_W+7] = '#';
    CHECK(!neon_validate(a.map,NEON_CELLS,2,2));
    quiet(&g); g.x = 6.5; g.y = 2.5;
    neon_advance(&g,(struct neon_input){.forward=1},2000); CHECK(g.x < 6.8);
    for (int i = 0; i < 20; ++i) neon_advance(&g,(struct neon_input){.forward=1},250);
    CHECK(g.x < 6.8);
    neon_advance(&g,(struct neon_input){.use=true},10); CHECK(g.map[2*NEON_W+7] == '.');
    for (int i = 0; i < 5; ++i) neon_advance(&g,(struct neon_input){.forward=1},250);
    CHECK(g.x > 8.0);
    quiet(&g); g.x = 6; g.y = 3; g.angle = NEON_PI/4;
    for (int i = 0; i < 15; ++i) neon_advance(&g,(struct neon_input){.forward=1},250);
    CHECK(g.x < 6.8 && g.y < 4.8);
    quiet(&g); g.enemy[0] = (struct neon_actor){9,2.5,2,1,0};
    neon_advance(&g,(struct neon_input){.fire=true},10); CHECK(g.enemy[0].hp == 2); CHECK(g.ammo == 35);
    CHECK(!neon_visible(&g,2.5,2.5,9,2.5));
    quiet(&g); g.enemy[0] = (struct neon_actor){4,2.5,1,0,1000}; g.enemy[1] = (struct neon_actor){5,2.5,2,0,1000};
    neon_advance(&g,(struct neon_input){.fire=true},10); CHECK(g.enemy[0].hp == 0 && g.enemy[1].hp == 2);
    quiet(&g); g.enemy[0] = (struct neon_actor){4,3,2,0,1000};
    neon_advance(&g,(struct neon_input){.fire=true},10); CHECK(g.enemy[0].hp == 2);
    quiet(&g); g.enemy[0] = (struct neon_actor){9,2.5,3,1,0};
    for (int i = 0; i < 20; ++i) neon_advance(&g,none,250);
    CHECK(g.health == 100);
    g.enemy[0] = (struct neon_actor){4,2.5,3,1,0}; neon_advance(&g,none,10); CHECK(g.health == 91);
    quiet(&g); g.enemy[0] = (struct neon_actor){4,2.5,2,0,0}; neon_advance(&g,none,10); CHECK(g.health == 100);
    for (int i = 0; i < 12; ++i) neon_advance(&g,none,250);
    CHECK(g.health < 100);
    /* An L-shaped pursuit through the actual doorway must make progress. */
    quiet(&g); g.map[7*NEON_W+7] = '.'; g.x = 9.5; g.y = 8.5;
    g.enemy[0] = (struct neon_actor){5.5,8.5,2,0,1000};
    for (int i = 0; i < 60; ++i) neon_advance(&g,none,250);
    CHECK(g.enemy[0].x > 7.5); CHECK(g.health < 100);
    quiet(&g); g.x = 5.5; g.y = 2.5; g.ammo = 98; neon_advance(&g,none,10);
    CHECK(g.ammo == 99 && g.pickup[0].taken); g.ammo = 0; neon_advance(&g,none,10); CHECK(g.ammo == 0);
    neon_advance(&g,(struct neon_input){.fire=true},250); CHECK(g.ammo == 0 && g.flash == 0);
    quiet(&g); g.x = 9.5; g.y = 3.5; g.health = 95; neon_advance(&g,none,10);
    CHECK(g.health == 100 && g.pickup[1].taken);
    quiet(&g); g.health = 1; g.enemy[0] = (struct neon_actor){3,2.5,2,0,0}; neon_advance(&g,none,10);
    CHECK(g.state == NEON_DEAD && g.health == 0); a = g; neon_advance(&g,(struct neon_input){.fire=true},250); CHECK(!memcmp(&g,&a,sizeof(g)));
    quiet(&g); g.x = 14.5; g.y = 9.5; neon_advance(&g,(struct neon_input){.use=true},10); CHECK(g.state == NEON_WON);
    a = g; neon_advance(&g,none,250);
    CHECK(!memcmp(&g,&a,sizeof(g))); neon_restart(&g); CHECK(g.state == NEON_PLAY && g.health == 100 && g.ammo == 36);
    quiet(&a); b = a;
    for (int i = 0; i < 100; ++i) neon_advance(&a,(struct neon_input){.forward=1,.turn=1},10);
    for (int i = 0; i < 10; ++i) neon_advance(&b,(struct neon_input){.forward=1,.turn=1},100);
    CHECK(!memcmp(&a,&b,sizeof(a)));
    neon_pause(&a,true); b = a; neon_advance(&a,(struct neon_input){.fire=true},50000); CHECK(!memcmp(&a,&b,sizeof(a)));
    neon_pause(&a,false); CHECK(a.accumulator == 0);
    static struct { uint16_t before; uint16_t pixels[NEON_VIEW_W*NEON_VIEW_H]; uint16_t after; } out;
    out.before = 0xa55a; out.after = 0x5aa5;
    CHECK(neon_render(&g,out.pixels,NEON_VIEW_W*NEON_VIEW_H));
    CHECK(out.before == 0xa55a && out.after == 0x5aa5); CHECK(!neon_render(&g,out.pixels,1)); CHECK(!neon_render(NULL,out.pixels,NEON_VIEW_W*NEON_VIEW_H));
    a = g; memset(a.map,'D',sizeof(a.map)); CHECK(neon_render(&a,out.pixels,NEON_VIEW_W*NEON_VIEW_H));
    a.x = NAN; CHECK(!neon_render(&a,out.pixels,NEON_VIEW_W*NEON_VIEW_H));
    CHECK(neon_cast(&g,2,2,NAN,0).distance == 0); CHECK(neon_cast(&g,2,2,0,0).distance == 0);
    clock_t start = clock();
    for (int i = 0; i < 300; ++i) { g.angle = i*.02; neon_render(&g,out.pixels,NEON_VIEW_W*NEON_VIEW_H); }
    printf("host renderer: %.3f ms/frame (300 frames, CPU clock; not K230 performance)\n",1000.0*(clock()-start)/CLOCKS_PER_SEC/300);
    full_run(); printf("neon_outpost_test: %u checks passed\n",checks);
}
