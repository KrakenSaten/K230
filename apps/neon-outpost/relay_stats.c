/* Opt-in frame timing: the app's own timer, game step and scene render, and
 * the display's refresh split into LVGL drawing, flush and flush wait.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "relay_stats.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define RS_CAP 512
static const char *const names[RS_COUNT]={"interval","game","scene","refresh","draw","flush","wait"};
struct rn_stats {
    FILE *out;lv_display_t *display;uint64_t opened,window,refr,render,flush,wait,mark;bool rendered;
    uint32_t sample[RS_COUNT][RS_CAP];unsigned n[RS_COUNT],frames,playing,stalls,pauses;
};
uint64_t rn_stats_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static void put(struct rn_stats *s,enum rn_stat k,uint64_t us){if(s->n[k]<RS_CAP)s->sample[k][s->n[k]++]=us>UINT32_MAX?UINT32_MAX:(uint32_t)us;}
void rn_stats_add(struct rn_stats *s,enum rn_stat k,uint64_t start){if(s)put(s,k,rn_stats_now()-start);}
/* LVGL sends FLUSH and FLUSH_WAIT inside RENDER, so drawing is what is left. */
static void display_event(lv_event_t *e)
{
    struct rn_stats *s=lv_event_get_user_data(e);uint64_t now=rn_stats_now();
    switch(lv_event_get_code(e)){
    case LV_EVENT_REFR_START:s->refr=now;s->flush=s->wait=0;s->rendered=false;break;
    case LV_EVENT_RENDER_START:s->render=now;s->rendered=true;break;
    case LV_EVENT_FLUSH_START:case LV_EVENT_FLUSH_WAIT_START:s->mark=now;break;
    case LV_EVENT_FLUSH_FINISH:s->flush+=now-s->mark;break;
    case LV_EVENT_FLUSH_WAIT_FINISH:s->wait+=now-s->mark;break;
    case LV_EVENT_REFR_READY:if(s->rendered&&s->refr){uint64_t all=now-s->refr,render=now-s->render;put(s,RS_REFRESH,all);put(s,RS_FLUSH,s->flush);put(s,RS_WAIT,s->wait);put(s,RS_DRAW,render>s->flush+s->wait?render-s->flush-s->wait:0);}break;
    default:break;
    }
}
static int order(const void *a,const void *b){uint32_t x=*(const uint32_t *)a,y=*(const uint32_t *)b;return x<y?-1:x>y;}
static void report(struct rn_stats *s,const struct rn_game *g,uint64_t now)
{
    fprintf(s->out,"t=%.1f frames=%u playing=%u stalls_100ms=%u auto_pauses=%u",(now-s->opened)/1e6,s->frames,s->playing,s->stalls,s->pauses);
    if(g)fprintf(s->out," state=%d paused=%d room=%d pos=%.2f,%.2f,%.2f yaw=%.2f weapon=%d ammo=%d/%d+%d hp=%d kills=%d",g->state,g->paused,g->sectors[g->sector].room,g->x,g->y,g->z,g->angle,g->weapon,g->mag,g->loaded,g->shells,g->health,g->kills);
    for(int k=0;k<RS_COUNT;k++){unsigned n=s->n[k];if(!n){fprintf(s->out," %s=-",names[k]);continue;}qsort(s->sample[k],n,sizeof(uint32_t),order);
        uint64_t sum=0;for(unsigned i=0;i<n;i++)sum+=s->sample[k][i];
        fprintf(s->out," %s_ms=%.2f/%.2f/%.2f/%.2f",names[k],sum/1000.0/n,s->sample[k][n/2]/1000.0,s->sample[k][(n*95)/100]/1000.0,s->sample[k][n-1]/1000.0);}
    fputc('\n',s->out);fflush(s->out);
    memset(s->n,0,sizeof(s->n));s->frames=s->playing=s->stalls=s->pauses=0;s->window=now;
}
struct rn_stats *rn_stats_open(void)
{
    const char *path=getenv("DOORS_RELAY_STATS");if(!path||!*path)return NULL;
    struct rn_stats *s=calloc(1,sizeof(*s));if(!s)return NULL;
    if(!(s->out=fopen(path,"a"))){free(s);return NULL;}
    s->opened=s->window=rn_stats_now();s->display=lv_display_get_default();
    fprintf(s->out,"# relay-stats v1 fields: mean/p50/p95/max per window\n");fflush(s->out);
    if(s->display)lv_display_add_event_cb(s->display,display_event,LV_EVENT_ALL,s);
    return s;
}
void rn_stats_frame(struct rn_stats *s,const struct rn_game *g,bool playing,bool auto_pause)
{
    if(!s)return;
    s->frames++;if(playing)s->playing++;if(auto_pause)s->pauses++;
    if(s->n[RS_INTERVAL]&&s->sample[RS_INTERVAL][s->n[RS_INTERVAL]-1]>=100000)s->stalls++;
    uint64_t now=rn_stats_now();if(now-s->window>=5000000)report(s,g,now);
}
void rn_stats_close(struct rn_stats *s)
{
    if(!s)return;
    if(s->display)lv_display_remove_event_cb_with_user_data(s->display,display_event,s);
    if(s->n[RS_INTERVAL])report(s,NULL,rn_stats_now());
    fprintf(s->out,"# closed\n");fclose(s->out);free(s);
}
