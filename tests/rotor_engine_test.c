/* Combat, progression, timing, deterministic replays, score integrity and audio.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "rotor.h"
#include "rotor_store.h"
#include "audio_effects/effects.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { checks++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);abort();} } while(0)
static void quiet(struct rt_game *g)
{
    g->spawn_ms=g->pickup_ms=100000;
    memset(g->enemies,0,sizeof(g->enemies));
}
static void run(struct rt_game *g,int ms,struct rt_input in)
{
    for(int t=0;t<ms;t+=16)rt_advance(g,in,16);
}
static void start(struct rt_game *g) { rt_init(g,12);rt_start(g,0,12);quiet(g); }

static void combat(void)
{
    struct rt_game g;start(&g);
    rt_damage(&g,35);CHECK(g.shield==65 && g.hull==100);
    rt_damage(&g,35);CHECK(g.shield==65);
    run(&g,2200,(struct rt_input){0});CHECK(g.shield==65);
    run(&g,1300,(struct rt_input){0});CHECK(g.shield>69 && g.shield<72);
    g.safe_ms=0;rt_damage(&g,90);CHECK(g.shield==0 && g.hull<100 && g.hull>70);
    g.safe_ms=0;rt_damage(&g,200);CHECK(g.phase==RT_OVER && g.hull==0);
    CHECK(g.sounds[RT_DEFLECT]==3 && g.sounds[RT_BOOM]);

    start(&g);float y=g.y;
    run(&g,200,(struct rt_input){.lift=-1,.aim=-1,.rocket=true});
    CHECK(g.y<y && g.angle<-.4f && g.rockets==2);
    bool aimed=false;
    for(int i=0;i<RT_SHOTS;i++)if(g.shots[i].active && !g.shots[i].rocket && g.shots[i].vy< -100)aimed=true;
    CHECK(aimed);
    run(&g,3300,(struct rt_input){0});CHECK(g.rockets==3);
    run(&g,3000,(struct rt_input){.lift=-1});CHECK(g.y==62);
    run(&g,3000,(struct rt_input){.lift=1});CHECK(g.y==g.height-58);

    start(&g);
    g.enemies[0]=(struct rt_enemy){.active=true,.kind=RT_MINE,.x=260,.y=g.y+6,.hp=18,.max_hp=18,.radius=15,.age=1000};
    run(&g,800,(struct rt_input){0});CHECK(g.kills==1 && g.score>=60 && !g.enemies[0].active);
    run(&g,1700,(struct rt_input){0});CHECK(g.credits>=12);
    g.pickups[0]=(struct rt_pickup){g.x+10,g.y,1,0,true};g.shield=20;
    rt_advance(&g,(struct rt_input){0},16);CHECK(g.shield>=52);
    g.pickups[0]=(struct rt_pickup){g.x+10,g.y,2,0,true};g.rockets=0;
    rt_advance(&g,(struct rt_input){0},16);CHECK(g.rockets==g.rocket_max);
}

static void progression(void)
{
    struct rt_game g;start(&g);
    CHECK(!rt_buy(&g,RT_SHIELD));
    g.wave_ms=RT_WAVE_MS-16;
    rt_advance(&g,(struct rt_input){0},16);
    CHECK(g.phase==RT_HANGAR && g.credits==100 && g.score==500);
    CHECK(!rt_buy(&g,(enum rt_upgrade)-1) && !rt_price(&g,(enum rt_upgrade)-1));
    CHECK(rt_buy(&g,RT_SHIELD) && g.shield_max==130 && g.shield==130 && g.credits==20);
    CHECK(!rt_buy(&g,RT_CANNON));
    g.credits=5000;g.hull=12;CHECK(rt_buy(&g,RT_REPAIR) && g.hull==100);
    CHECK(!rt_buy(&g,RT_REPAIR));
    for(int k=0;k<3;k++)while(g.level[k]<4)CHECK(rt_buy(&g,(enum rt_upgrade)k));
    CHECK(!rt_buy(&g,RT_SHIELD) && !rt_price(&g,RT_SHIELD));
    CHECK(g.rocket_max==7);
    rt_next_wave(&g);CHECK(g.wave==2 && g.phase==RT_FLIGHT && g.level[0]==4);
    g.wave=3;g.wave_ms=12496;g.spawn_ms=100000;
    rt_advance(&g,(struct rt_input){0},16);CHECK(g.boss_spawned);
    int boss=-1;for(int i=0;i<RT_ENEMIES;i++)if(g.enemies[i].active && g.enemies[i].kind==RT_BOSS)boss=i;
    CHECK(boss>=0);
    g.wave_ms=RT_WAVE_MS;rt_advance(&g,(struct rt_input){0},16);CHECK(g.phase==RT_FLIGHT);
    struct rt_enemy *e=&g.enemies[boss];e->x=g.x+50;e->y=g.y;e->hp=1;
    g.shots[0]=(struct rt_shot){.x=e->x-1,.y=e->y,.vx=100,.damage=100,.active=true};
    rt_advance(&g,(struct rt_input){0},16);rt_advance(&g,(struct rt_input){0},16);
    CHECK(!e->active && g.phase==RT_HANGAR && g.kills==1);
    CHECK(!strcmp(rt_sector(1),"EMBER COAST") && !strcmp(rt_sector(4),"VIOLET RIDGE") && !strcmp(rt_sector(7),"ION CITY"));
}

static void timing(void)
{
    struct rt_game a,b;start(&a);start(&b);
    for(int i=0;i<80;i++)rt_advance(&a,(struct rt_input){.lift=.25f,.aim=-.5f,.rocket=true},16);
    for(int i=0;i<40;i++)rt_advance(&b,(struct rt_input){.lift=.25f,.aim=-.5f,.rocket=true},32);
    CHECK(a.x==b.x && a.y==b.y && a.angle==b.angle && a.steps==b.steps && a.rng==b.rng);
    CHECK(!memcmp(a.shots,b.shots,sizeof(a.shots)));
    unsigned steps=a.steps;rt_advance(&a,(struct rt_input){0},1000);CHECK(a.paused && a.steps==steps);
    rt_advance(&a,(struct rt_input){0},16);CHECK(a.steps==steps);
    a.paused=false;rt_resize(&a,720);CHECK(a.height==720 && a.y>=62 && a.y<720-58);
    rt_resize(&a,10000);CHECK(a.height==RT_MAX_H);
    rt_resize(&a,0);CHECK(a.height==240);
    rt_start(&a,99,0);CHECK(a.difficulty==0 && a.rng!=0);
}

static void endurance(void)
{
    struct rt_game g;rt_init(&g,42);rt_start(&g,2,42);
    unsigned hangars=0,bosses=0;
    for(int step=0;step<40000;step++) {
        if(g.phase==RT_HANGAR) {
            hangars++;
            for(int i=0;i<3;i++)rt_buy(&g,(enum rt_upgrade)i);
            rt_next_wave(&g);
        }
        g.safe_ms=1000;
        float target_y=g.height*.5f,min_x=10000;
        for(int i=0;i<RT_ENEMIES;i++) {
            struct rt_enemy *e=&g.enemies[i];
            if(e->active && e->x>g.x+30 && e->x<min_x) { min_x=e->x;target_y=e->y; }
        }
        struct rt_input in={.lift=fmaxf(-1,fminf(1,(target_y-g.y)*.02f)),.rocket=true};
        if(min_x<10000)in.aim=fmaxf(-1,fminf(1,atan2f(target_y-g.y,min_x-g.x)/.72f));
        rt_advance(&g,in,16);
        if(g.wave%3==0 && g.wave_ms<13000 && g.boss_spawned)bosses++;
        CHECK(g.hull==100 && isfinite(g.y) && isfinite(g.angle) && g.shield>=0 && g.shield<=g.shield_max);
        CHECK(g.rockets>=0 && g.rockets<=g.rocket_max && g.phase!=RT_OVER);
        if(step%2000==0) { rt_resize(&g,g.height==720?284:720); }
    }
    CHECK(hangars>=10 && bosses>0 && g.kills>100);
    printf("  endurance: wave %u, %u kills, %u hangars\n",g.wave,g.kills,hangars);
}

static void scores(void)
{
    char dir[]="/tmp/rotor-score-XXXXXX";CHECK(mkdtemp(dir));setenv("POCKETOS_STATE_DIR",dir,1);
    uint32_t best[]={1234,5678,9012},read[3];
    CHECK(!rt_scores_load(read) && !read[0]);
    CHECK(!rt_scores_save(best) && !rt_scores_load(read) && !memcmp(read,best,sizeof(best)));
    char file[200];snprintf(file,sizeof(file),"%s/rotorterror/scores.bin",dir);
    FILE *f=fopen(file,"r+b");CHECK(f);fputc('X',f);fclose(f);
    CHECK(rt_scores_load(read)<0 && !read[0] && !read[1] && !read[2]);
    CHECK(!rt_scores_save(best));
    f=fopen(file,"ab");CHECK(f);fputc(0,f);fclose(f);CHECK(rt_scores_load(read)<0);
    setenv("POCKETOS_STATE_DIR",file,1);CHECK(rt_scores_save(best)<0);
    unlink(file);snprintf(file,sizeof(file),"%s/rotorterror",dir);rmdir(file);rmdir(dir);
}

static void effects(void)
{
    struct audio_effects *m=malloc(sizeof(*m));CHECK(m);audio_effects_init(m);
    for(unsigned k=RT_GUN;k<RT_SOUNDS;k++) {
        CHECK(m->length[k]>0 && m->length[k]<=AUDIO_EFFECT_FRAMES);
        unsigned nonzero=0;
        for(unsigned i=0;i<m->length[k];i++)if(m->bank[k][i])nonzero++;
        CHECK(nonzero>1000);
        audio_effects_play(m,k);
    }
    int16_t pcm[960];bool audible=false;
    for(int b=0;b<24;b++) {
        audio_effects_mix(m,pcm,960);
        for(int i=0;i<960;i++) { CHECK(abs(pcm[i])<=8192);if(pcm[i])audible=true; }
    }
    CHECK(audible);free(m);
}

static void renderer(void)
{
    struct guarded { uint32_t before;struct rt_frame frame;uint32_t after; };
    struct guarded *buf=malloc(sizeof(*buf));CHECK(buf);
    buf->before=0xabcdef12;buf->after=0x12345678;
    struct rt_game g;start(&g);
    for(int i=0;i<8;i++) {
        rt_resize(&g,i%2?720:284);g.wave=1+(i%3)*3;
        rt_render(&g,&buf->frame,i%2!=0);
        CHECK(buf->before==0xabcdef12 && buf->after==0x12345678);
        CHECK(buf->frame.height==g.height);
        unsigned changes=0;uint16_t last=buf->frame.pixels[0];
        for(int j=1;j<RT_W*g.height;j++)if(last!=buf->frame.pixels[j]) { changes++;last=buf->frame.pixels[j]; }
        CHECK(changes>3000);
    }
    /* Exercise clipped geometry, minimum-height scenery, every enemy and
     * velocity-aligned rockets under ASan/UBSan, not only empty backdrops. */
    for(int zone=0;zone<3;zone++)for(int h=240;h<=720;h+=240) {
        rt_resize(&g,h);g.wave=1+zone*3;g.distance=123456.0f;
        for(int side=0;side<2;side++) {
            for(int kind=RT_DRONE;kind<=RT_BOSS;kind++) {
                g.enemies[kind]=(struct rt_enemy){.active=true,.kind=kind,
                    .x=side?RT_W-2:2,.y=3+kind*40,.hp=1,.max_hp=10};
            }
            for(int i=0;i<8;i++) {
                float angle=i*.785398f;
                g.shots[i]=(struct rt_shot){.active=true,.rocket=true,
                    .x=side?RT_W-1:1,.y=1+i*30,
                    .vx=cosf(angle)*100,.vy=sinf(angle)*100};
                g.particles[i]=(struct rt_particle){.x=i*85,.y=h-1,
                    .life=80,.max_life=120,.color=1,.size=5};
            }
            rt_render(&g,&buf->frame,false);
            CHECK(buf->before==0xabcdef12 && buf->after==0x12345678);
        }
    }
    free(buf);
}

int main(void)
{
    combat();progression();timing();endurance();scores();effects();renderer();
    printf("rotor_engine_test: %u checks passed\n",checks);
    return 0;
}
