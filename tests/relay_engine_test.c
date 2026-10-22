/* Focused sector geometry, production run, rendering and sound synthesis checks.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "relay.h"
#include "relay_pilot.h"
#include "audio_effects/effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static unsigned checks;
static int compare(const void *a,const void *b){double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y);}
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);abort();}}while(0)
static void quiet(struct rn_game *g){CHECK(rn_restart(g,false));for(int i=0;i<g->enemy_count;i++)g->enemy[i].hp=0;}
static void ppm(const struct rn_game *g,const char *name)
{
    const char *dir=getenv("RELAY_RENDER_DIR");if(!dir)return;static struct rn_frame f;CHECK(rn_render(g,&f));CHECK(!f.cap_hits);
    char path[512];snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);FILE *file=fopen(path,"wb");CHECK(file);fprintf(file,"P6\n%d %d\n255\n",RN_W,RN_H);
    for(int i=0;i<RN_W*RN_H;i++){unsigned char p[3]={(f.pixels[i]>>11)*255/31,((f.pixels[i]>>5)&63)*255/63,(f.pixels[i]&31)*255/31};fwrite(p,1,3,file);}fclose(file);
}
int main(void)
{
    struct rn_game g,a,b;static struct rn_frame frame;
    CHECK(rn_restart(&g,true));CHECK(rn_validate(&g));ppm(&g,"slice-stairs");
    double start=g.z;
    for(int i=0;i<200;i++)rn_advance(&g,(struct rn_input){.forward=1},10);
    CHECK(g.z>start+.7&&g.sectors[g.sector].room==2);ppm(&g,"slice-gallery");
    CHECK(rn_restart(&g,false));CHECK(g.sector_count==41&&g.enemy_count==9);CHECK(rn_validate(&g));
    a=g;a.edges[0].neighbor=1000;CHECK(!rn_validate(&a));a=g;a.sectors[0].floor=NAN;CHECK(!rn_validate(&a));
    a=g;a.sectors[0].x0=NAN;CHECK(!rn_validate(&a));a=g;a.sectors[1].first=100000;CHECK(!rn_validate(&a));
    a=g;a.sectors[0].ceiling=.5;CHECK(!rn_validate(&a));CHECK(rn_locate(&g,NAN,3)==-1);
    quiet(&g);g.x=6.5;g.y=12.5;g.angle=-RN_PI/2;g.sector=rn_locate(&g,g.x,g.y);
    for(int i=0;i<50;i++)rn_advance(&g,(struct rn_input){.forward=1},10);
    CHECK(g.y>12.2);
    rn_advance(&g,(struct rn_input){.use=true},10);
    CHECK(g.door_want[0]&&g.door[0]<.1);
    for(int i=0;i<100;i++)rn_advance(&g,(struct rn_input){.forward=1},10);
    CHECK(g.y<12&&g.door[0]==1);
    quiet(&g);g.x=2;g.y=9;g.sector=rn_locate(&g,g.x,g.y);g.angle=0;
    for(int i=0;i<300;i++)rn_advance(&g,(struct rn_input){.forward=1},10);
    CHECK(g.x<3.8&&g.z==0);
    /* Low wall blocks a low ray but the upper opening remains visible. */
    g.x=5;g.y=7.5;g.sector=rn_locate(&g,g.x,g.y);
    CHECK(rn_trace(&g,g.sector,g.x,g.y,.3,0,1,0,10).t<1);
    CHECK(rn_trace(&g,g.sector,g.x,g.y,1.5,0,1,0,10).t>4);
    /* Per-pixel depth, not one distance for the entire column: a robot
     * beyond a low plinth has visible head/shoulders and hidden legs. */
    quiet(&g);g.x=5;g.y=7.5;g.angle=RN_PI/2;g.sector=rn_locate(&g,g.x,g.y);
    static struct rn_frame before;CHECK(rn_render(&g,&before));
    g.enemy[0]=(struct rn_actor){.x=5,.y=11.3,.sector=4,.hp=100};CHECK(rn_render(&g,&frame));
    unsigned top_diff=0,leg_diff=0;
    for(int y=90;y<125;y++)for(int x=145;x<175;x++)top_diff+=frame.pixels[y*RN_W+x]!=before.pixels[y*RN_W+x];
    for(int y=165;y<185;y++)for(int x=145;x<175;x++)leg_diff+=frame.pixels[y*RN_W+x]!=before.pixels[y*RN_W+x];
    CHECK(top_diff>10&&leg_diff==0);
    quiet(&g);g.mag=1;g.reserve=3;rn_advance(&g,(struct rn_input){.fire=true},10);
    CHECK(g.mag==0&&g.sounds[RN_SHOT]==1);
    rn_advance(&g,(struct rn_input){.reload=true},10);
    CHECK(g.reloading>0);
    for(int i=0;i<100;i++)rn_advance(&g,(struct rn_input){.reload=true},10);
    CHECK(g.mag==3&&g.reserve==0);
    unsigned shots=g.sounds[RN_SHOT];rn_advance(&g,(struct rn_input){.fire=true},250);CHECK(g.sounds[RN_SHOT]==shots+1&&g.mag==2);
    quiet(&g);g.enemy_count=2;g.angle=-RN_PI/2;g.enemy[0]=(struct rn_actor){.x=g.x,.y=g.y-1.5,.z=0,.sector=0,.hp=25};g.enemy[1]=(struct rn_actor){.x=g.x,.y=g.y-2.5,.z=0,.sector=0,.hp=100};
    rn_advance(&g,(struct rn_input){.fire=true},10);
    CHECK(g.enemy[0].hp==0&&g.enemy[1].hp==100&&g.kills==1);
    quiet(&g);g.enemy_count=1;g.enemy[0]=(struct rn_actor){.x=g.x,.y=g.y-2,.z=0,.sector=0,.hp=100,.awake=true};
    rn_advance(&g,(struct rn_input){0},10);
    CHECK(g.enemy[0].windup>0&&g.health==100);
    for(int i=0;i<36;i++)rn_advance(&g,(struct rn_input){0},10);
    CHECK(g.health==92);
    g.health=1;for(int i=0;i<300&&g.state==RN_PLAY;i++)rn_advance(&g,(struct rn_input){0},10);
    CHECK(g.state==RN_DEAD&&g.health==0);
    a=g;rn_advance(&g,(struct rn_input){.fire=true,.use=true},250);CHECK(!memcmp(&a,&g,sizeof(g)));
    quiet(&a);b=a;for(int i=0;i<500;i++)rn_advance(&a,(struct rn_input){.forward=.4,.turn=.3,.fire=true},10);for(int i=0;i<50;i++)rn_advance(&b,(struct rn_input){.forward=.4,.turn=.3,.fire=true},100);CHECK(!memcmp(&a,&b,sizeof(a)));
    rn_pause(&a,true);b=a;rn_advance(&a,(struct rn_input){.forward=1},100000);CHECK(!memcmp(&a,&b,sizeof(a)));rn_pause(&a,false);CHECK(a.accumulator==0);
    CHECK(rn_render(&a,&frame));CHECK(frame.columns==RN_W&&!frame.cap_hits);a.x=NAN;CHECK(!rn_render(&a,&frame));a.x=1e100;CHECK(!rn_render(&a,&frame));CHECK(!rn_render(NULL,&frame));
    quiet(&g);g.x=0;g.y=6.5;g.angle=RN_PI;g.sector=rn_locate(&g,g.x,g.y);CHECK(rn_render(&g,&frame)&&!frame.cap_hits);
    CHECK(rn_restart(&g,false));unsigned goal=0;bool high=false,mid=false,combat=false,relay=false;clock_t begin=clock();double times[6000];unsigned timed=0,max_portals=0;
    for(int t=0;t<60000&&g.state==RN_PLAY;t++){
        if(hypot(g.x-relay_tour[goal][0],g.y-relay_tour[goal][1])<.35&&(goal!=5||g.relay)&&goal<8)goal++;
        struct rn_input in=relay_pilot(&g,relay_tour[goal][0],relay_tour[goal][1]);rn_advance(&g,in,10);
        if(!combat&&g.flash){ppm(&g,"combat");combat=true;}
        if(!mid&&g.z>.7){ppm(&g,"gallery");mid=true;}
        if(!high&&g.z>1.49){ppm(&g,"upper-control");high=true;}
        if(!relay&&g.relay){ppm(&g,"relay-restored");relay=true;}
        if(t%10==0){clock_t before=clock();CHECK(rn_render(&g,&frame));times[timed++]=1000.0*(clock()-before)/CLOCKS_PER_SEC;if(frame.portals>max_portals)max_portals=frame.portals;CHECK(!frame.cap_hits);}
    }
    CHECK(g.state==RN_WON&&g.kills==9&&g.relay&&mid&&high);CHECK(g.health>0&&g.mag>=0&&g.reserve>=0);
    printf("production full run: %u steps, %.2fs simulation, hp %d, ammo %d/%d, kills %d; host loop CPU %.3fs including sampled renders\n",g.steps,g.steps*.01,g.health,g.mag,g.reserve,g.kills,(double)(clock()-begin)/CLOCKS_PER_SEC);
    qsort(times,timed,sizeof(*times),compare);printf("host renderer: %u frames at 320x200, p50 %.3fms p95 %.3fms p99 %.3fms; max column portal transitions/frame %u; excludes LVGL/flush and does not predict K230\n",timed,times[timed/2],times[timed*95/100],times[timed*99/100],max_portals);
    a=g;rn_advance(&g,(struct rn_input){.fire=true},250);CHECK(!memcmp(&a,&g,sizeof(g)));
    static struct audio_effects mix;audio_effects_init(&mix);int16_t pcm[960];
    for(int i=0;i<8;i++){CHECK(mix.length[i]>0&&mix.length[i]<=AUDIO_EFFECT_FRAMES);audio_effects_play(&mix,i);}
    CHECK(mix.dropped>0);bool nonzero=false;for(int t=0;t<30;t++){audio_effects_mix(&mix,pcm,960);for(int i=0;i<960;i++){CHECK(pcm[i]>=-8192&&pcm[i]<=8192);nonzero|=pcm[i]!=0;}}
    CHECK(nonzero);for(int i=0;i<4;i++)CHECK(!mix.voice[i].active);
    printf("relay_engine_test: %u checks passed\n",checks);return 0;
}
