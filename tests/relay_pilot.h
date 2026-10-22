/* Bounded input-only test driver: never teleports or edits production state.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef RELAY_PILOT_H
#define RELAY_PILOT_H
#include "relay.h"
#include <math.h>
static struct rn_input relay_pilot(const struct rn_game *g,double gx,double gy)
{
    struct rn_input in={0};double tx=gx,ty=gy,best=10;bool fight=false;
    for(int i=0;i<g->enemy_count;i++){
        const struct rn_actor *e=&g->enemy[i];double d=hypot(e->x-g->x,e->y-g->y),tz=fmax(e->z+.15,fmin(e->z+(e->kind?.52:1.32),g->z+RN_EYE));
        if(e->hp>0&&d<best&&fabs(atan2(tz-g->z-RN_EYE,d))<1.03&&rn_los(g,g->sector,g->x,g->y,g->z+RN_EYE,e->x,e->y,tz)){tx=e->x;ty=e->y;best=d;fight=true;}
    }
    if(!fight){
        int goal=rn_locate(g,gx,gy),prev[RN_SECTORS],q[RN_SECTORS],h=0,t=0;for(int i=0;i<RN_SECTORS;i++)prev[i]=-1;
        q[t++]=g->sector;prev[g->sector]=g->sector;
        while(h<t&&goal>=0&&prev[goal]<0){int at=q[h++];const struct rn_sector *s=&g->sectors[at];
            for(int j=s->first;j<s->first+s->count;j++){int n=g->edges[j].neighbor;
                if(n>=0&&prev[n]<0&&g->sectors[n].floor-g->sectors[at].floor<=.201&&g->sectors[n].ceiling-fmax(g->sectors[n].floor,g->sectors[at].floor)>=RN_BODY){prev[n]=at;q[t++]=n;}
            }
        }
        if(goal>=0&&prev[goal]>=0&&goal!=g->sector){int n=goal;while(prev[n]!=g->sector)n=prev[n];
            const struct rn_sector *s=&g->sectors[g->sector];
            for(int j=s->first;j<s->first+s->count;j++)if(g->edges[j].neighbor==n){const struct rn_edge *e=&g->edges[j];tx=(e->ax+e->bx)/2;ty=(e->ay+e->by)/2;double cx=(g->sectors[n].x0+g->sectors[n].x1)/2,cy=(g->sectors[n].y0+g->sectors[n].y1)/2,l=hypot(cx-tx,cy-ty);if(l>.001){tx+=(cx-tx)/l*.25;ty+=(cy-ty)/l*.25;}break;}
        }
    }
    double diff=remainder(atan2(ty-g->y,tx-g->x)-g->angle,2*RN_PI);
    in.turn=fabs(diff)>.015?fmax(-1,fmin(1,diff/.024)):0;
    bool want_shotgun=g->shotgun&&(g->loaded||g->shells)&&fight&&best<6.5;
    in.switch_weapon=(g->weapon==RN_SHOTGUN)!=want_shotgun;
    if(fight)in.fire=fabs(diff)<.07&&!in.switch_weapon;
    else in.forward=fabs(diff)<.12?1:0;
    in.reload=g->weapon==RN_SHOTGUN?g->loaded==0:g->mag==0;in.use=g->steps%20==0;
    return in;
}
static const double relay_tour[][2]={{6.5,13.3},{1.5,9.5},{-2,10.4},{-4.7,10.25},{-7,11},{-4,7.8},{-4,1.2},{-4,-5.5},{9.5,-5.7},{9.5,3.9},{13.5,10.5},{13,15.5}};
#define RELAY_GOALS (sizeof(relay_tour)/sizeof(relay_tour[0]))
static bool relay_goal_done(const struct rn_game *g,unsigned goal)
{return hypot(g->x-relay_tour[goal][0],g->y-relay_tour[goal][1])<.35&&(goal!=8||g->relay);}
#endif
