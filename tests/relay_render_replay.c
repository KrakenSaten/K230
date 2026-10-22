/* Deterministic renderer comparison and fixed-state CPU benchmark.
 * Build with RN_REFERENCE to compare a separately linked reference renderer/art.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "relay.h"
#include "relay_pilot.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef RN_REFERENCE
bool rn_render_reference(const struct rn_game *,struct rn_frame *);
uint16_t rn_texture_reference(int,int,int,int);
#endif
static struct rn_frame frame;
static unsigned frames;
static uint64_t hash=UINT64_C(14695981039346656037);
static double now(void){struct timespec t;clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t);return t.tv_sec*1000.0+t.tv_nsec/1000000.0;}
static int compare(const void *a,const void *b){double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y);}
static void render(const struct rn_game *g)
{
    if(!rn_render(g,&frame)||frame.cap_hits){fprintf(stderr,"render/cap failure frame %u\n",frames);exit(1);}
#ifdef RN_REFERENCE
    static struct rn_frame ref;
    if(!rn_render_reference(g,&ref)||memcmp(&ref,&frame,sizeof(frame))){fprintf(stderr,"reference mismatch frame %u\n",frames);exit(1);}
#endif
    const unsigned char *p=(const unsigned char *)&frame;
    for(size_t i=0;i<sizeof(frame);i++){hash^=p[i];hash*=UINT64_C(1099511628211);}
    frames++;
}
static void bench(const struct rn_game *state,const char *name,const char *dir)
{
    struct rn_game g=*state;double times[400];
    for(int i=0;i<420;i++){
        /* Identical, frozen gameplay state; the same twenty view bearings. */
        g.angle=state->angle+(i%20-10)*.025;
        double begin=now();if(!rn_render(&g,&frame)||frame.cap_hits)exit(1);
        if(i>=20)times[i-20]=now()-begin;
    }
    qsort(times,400,sizeof(*times),compare);
    printf("%s,p50=%.6f,p95=%.6f,p99=%.6f\n",name,times[200],times[380],times[396]);
    if(dir){
        g=*state;if(!rn_render(&g,&frame))exit(1);char path[512];
        snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);FILE *f=fopen(path,"wb");if(!f)exit(1);
        fprintf(f,"P6\n%d %d\n255\n",RN_W,RN_H);
        for(int i=0;i<RN_W*RN_H;i++){uint16_t c=frame.pixels[i];unsigned char p[3]={(c>>11)*255/31,((c>>5)&63)*255/63,(c&31)*255/31};fwrite(p,1,3,f);}fclose(f);
    }
}
int main(int argc,char **argv)
{
    static struct rn_game g,slice,spawn,shotgun,gallery,upper;
    const char *dir=argc==2?argv[1]:NULL;
#ifdef RN_REFERENCE
    /* Every material texel and light, including clamped and wrapped inputs. */
    for(int m=-1;m<=6;m++)for(int l=-1;l<=256;l++)for(int y=0;y<64;y++)for(int x=0;x<64;x++)
        if(rn_texture(m,x-64,y+64,l)!=rn_texture_reference(m,x-64,y+64,l)){fprintf(stderr,"texture mismatch\n");return 1;}
#endif
    if(!rn_restart(&slice,true)||!rn_restart(&g,false))return 1;
    spawn=g;bool got_shot=false,got_gallery=false,got_upper=false;unsigned goal=0;
    for(int t=0;t<60000&&g.state==RN_PLAY;t++){
        if(relay_goal_done(&g,goal)&&goal+1<RELAY_GOALS)goal++;
        rn_advance(&g,relay_pilot(&g,relay_tour[goal][0],relay_tour[goal][1]),10);
        if(t%10==0)render(&g);
        if(!got_shot&&g.weapon==RN_SHOTGUN&&g.flash){shotgun=g;got_shot=true;}
        if(!got_gallery&&g.z>.74){gallery=g;got_gallery=true;}
        if(!got_upper&&g.z>1.49){upper=g;got_upper=true;}
    }
    if(g.state!=RN_WON||g.kills!=9||!g.secret||!got_shot||!got_gallery||!got_upper)return 1;
    printf("tour: steps=%u hp=%d ammo=%d/%d kills=%d\n",g.steps,g.health,g.mag,g.reserve,g.kills);
    for(int scene=0;scene<2;scene++){
        g=scene?upper:slice;
        for(int i=0;i<720;i++){g.angle=i*RN_PI/360;render(&g);}
    }
    /* Weapon poses are frozen at simulation timers, including reduced motion. */
    for(int weapon=0;weapon<2;weapon++)for(int reduced=0;reduced<2;reduced++){
        g=slice;g.weapon=weapon;g.reduced_motion=reduced;
        for(int t=0;t<=1500;t+=50){g.reloading=t;g.flash=t<150?t:0;render(&g);}
    }
    printf("frames=%u frame-fnv1a=%016" PRIx64 " game=%zu frame=%zu bytes\n",frames,hash,sizeof(g),sizeof(frame));
    bench(&slice,"slice",dir);bench(&spawn,"closed-door",dir);bench(&shotgun,"shotgun",dir);bench(&gallery,"gallery",dir);bench(&upper,"upper",dir);
    g=slice;g.reloading=450;bench(&g,"carbine-reload",dir);
    g.weapon=RN_SHOTGUN;g.reloading=750;bench(&g,"shotgun-reload",dir);
    return 0;
}
