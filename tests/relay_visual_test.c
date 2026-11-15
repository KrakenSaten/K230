/* Rendering invariants for the cinematic presentation and its load fallback.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "relay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static struct rn_frame first, second;

int main(void)
{
    struct rn_game g, saved;
    for (int slice=0;slice<2;slice++) {
        CHECK(rn_restart(&g,slice));
        for (int detail=0;detail<2;detail++) {
            for (int bearing=0;bearing<36;bearing++) {
                g.angle=bearing*RN_PI/18;
                saved=g;
                CHECK(rn_render_quality(&g,&first,detail));
                CHECK(!first.cap_hits);
                CHECK(!memcmp(&g,&saved,sizeof(g)));
                CHECK(rn_render_quality(&g,&second,detail));
                CHECK(!memcmp(&first,&second,sizeof(first)));
            }
        }
    }
    /* Current may change color, but never visibility, depth or simulation. */
    CHECK(rn_restart(&g,true));
    for (int detail=0;detail<2;detail++) {
        g.reduced_motion=false;g.steps=0;
        CHECK(rn_render_quality(&g,&first,detail));
        g.steps=64;
        CHECK(rn_render_quality(&g,&second,detail));
        CHECK(memcmp(first.pixels,second.pixels,sizeof(first.pixels)));
        CHECK(!memcmp(first.depth,second.depth,sizeof(first.depth)));
        g.reduced_motion=true;g.steps=0;
        CHECK(rn_render_quality(&g,&first,detail));
        g.steps=64;
        CHECK(rn_render_quality(&g,&second,detail));
        CHECK(!memcmp(&first,&second,sizeof(first)));
    }
    /* Both quality paths preserve a low wall's occlusion of a sentinel. */
    CHECK(rn_restart(&g,false));
    for (int i=0;i<g.enemy_count;i++)g.enemy[i].hp=0;
    g.x=5;g.y=7.5;g.angle=RN_PI/2;g.sector=rn_locate(&g,g.x,g.y);
    for (int detail=0;detail<2;detail++) {
        g.enemy[0].hp=0;
        CHECK(rn_render_quality(&g,&first,detail));
        g.enemy[0]=(struct rn_actor){.x=5,.y=11.3,.sector=4,.hp=100};
        CHECK(rn_render_quality(&g,&second,detail));
        unsigned head=0,legs=0;
        for (int y=RN_H/2-10;y<RN_H/2+25;y++)for(int x=145;x<175;x++)
            head+=first.pixels[y*RN_W+x]!=second.pixels[y*RN_W+x];
        for (int y=RN_H/2+65;y<RN_H/2+85;y++)for(int x=145;x<175;x++)
            legs+=first.pixels[y*RN_W+x]!=second.pixels[y*RN_W+x];
        CHECK(head>10&&legs==0);
    }
    CHECK(!rn_render_quality(NULL,&first,true));
    printf("relay_visual_test: %u checks passed; deterministic, read-only, reduced motion, occlusion in both modes\n",checks);
    return 0;
}
