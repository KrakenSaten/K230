/* Original software sector/portal ray marcher with height-aware plane spans
 * and per-pixel depth. No grid map, GPU, engine import or external asset.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "relay.h"
#include <math.h>
#include <string.h>
#include <float.h>
#include <stdlib.h>
static const double focal=RN_W/1.32;
static uint16_t lit(uint16_t c,int room)
{
    int r=(c>>11)&31,g=(c>>5)&63,b=c&31;
    /* Authored sector lamp color: amber hall, neutral gallery, muted work
     * lights elsewhere. No theme or unbounded dynamic light/shadow pass. */
    if(room==1){r=r*108/100;g=g*98/100;b=b*82/100;}
    else if(room==3){r=r*94/100;b=b*90/100;}
    if(r>31)r=31;
    return (uint16_t)((r<<11)|(g<<5)|b);
}
static void put(struct rn_frame *f,int x,int y,double distance,uint16_t c)
{
    if(x<0||y<0||x>=RN_W||y>=RN_H||distance<=0||!isfinite(distance))return;
    unsigned d=(unsigned)fmin(65534,distance*512);int p=y*RN_W+x;
    if(d<f->depth[p]){f->depth[p]=(uint16_t)d;f->pixels[p]=c;}
}
static void wall(struct rn_frame *f,int x,int clip0,int clip1,double z0,double z1,double eye,double t,double u,int material,int light,int room)
{
    if(z1<=z0||t<=.001)return;
    double y0=RN_H/2+(eye-z1)*focal/t,y1=RN_H/2+(eye-z0)*focal/t;
    int start=(int)fmax(clip0,ceil(y0)),end=(int)fmin(clip1,floor(y1));
    for(int y=start;y<=end;y++){double z=eye-(y-RN_H/2)*t/focal;put(f,x,y,t,lit(rn_texture(material,(int)floor(u*32),(int)floor(z*32),light),room));}
}
static void sprite(const struct rn_game *g,struct rn_frame *f,double sx,double sy,double sz,double height,int kind,int phase,int facing,int state)
{
    double dx=sx-g->x,dy=sy-g->y,ca=cos(g->angle),sa=sin(g->angle);
    double t=dx*ca+dy*sa,across=-dx*sa+dy*ca;if(t<.15)return;
    double scale=height*focal/t;
    double cx=RN_W/2+across*focal/t,top=RN_H/2+(g->z+RN_EYE-sz-height)*focal/t;
    if(!isfinite(cx+top+scale)||scale>5000||cx+scale<0||cx-scale>RN_W)return;
    double width=scale*(kind==1?1.4:.7),left=cx-width/2;
    int x0=(int)fmax(0,ceil(left)),x1=(int)fmin(RN_W-1,floor(left+width));
    int y0=(int)fmax(0,ceil(top)),y1=(int)fmin(RN_H-1,floor(top+scale));
    for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++){
        uint16_t c=rn_sprite(kind,(int)((x-left)*64/width),(int)((y-top)*64/scale),phase,facing,state);
        if(c){put(f,x,y,t,c);f->sprite_pixels++;}
    }
}
bool rn_render(const struct rn_game *g,struct rn_frame *f)
{
    if(!g||!f||g->sector_count<1||g->sector_count>RN_SECTORS||g->edge_count>RN_EDGES||g->sector<0||g->sector>=g->sector_count||!isfinite(g->x+g->y+g->z+g->angle))return false;
    if(fabs(g->x)>128||fabs(g->y)>128||fabs(g->z)>16||rn_locate(g,g->x,g->y)!=g->sector)return false;
    for(int i=0;i<g->sector_count;i++){
        const struct rn_sector *s=&g->sectors[i];
        if(!isfinite(s->floor)||!isfinite(s->ceiling)||fabs(s->floor)>16||fabs(s->ceiling)>32||s->first<0||s->count<4||s->first+s->count>g->edge_count)return false;
    }
    memset(f,0,sizeof(*f));memset(f->depth,255,sizeof(f->depth));
    double ca=cos(g->angle),sa=sin(g->angle),eye=g->z+RN_EYE;
    for(int x=0;x<RN_W;x++){
        double camera=(2*(x+.5)/RN_W-1)*.66,dx=ca-sa*camera,dy=sa+ca*camera;
        int s=g->sector,low=0,high=RN_H-1;double after=-.00001;f->columns++;
        for(int k=0;k<RN_SECTORS;k++){
            const struct rn_sector *sec=&g->sectors[s];
            if(sec->first<0||sec->count<0||sec->first+sec->count>g->edge_count){f->cap_hits++;break;}
            double t,u;int index=rn_boundary(g,s,g->x,g->y,dx,dy,after,&t,&u);
            if(index<0||t>90){t=90;index=-1;}
            int light=(int)(sec->light/(1+t*.035));
            for(int y=low;y<=high;y++){
                double screen=y+.5-RN_H/2;if(fabs(screen)<.001)continue;
                double z=screen>0?sec->floor:sec->ceiling,plane=(eye-z)*focal/screen;
                if(plane>fmax(.02,after)&&plane<=t+.00001){
                    int tx=(int)floor((g->x+dx*plane)*32),ty=(int)floor((g->y+dy*plane)*32);
                    int material=screen>0?(sec->floor>0?4:3):5;
                    int shade=(int)(sec->light/(1+plane*.04));put(f,x,y,plane,lit(rn_texture(material,tx,ty,shade),sec->room));
                }
            }
            if(index<0)break;
            const struct rn_edge *e=&g->edges[index];double lo,hi;rn_opening(g,s,e,&lo,&hi);
            double texu=u*hypot(e->bx-e->ax,e->by-e->ay);
            if(hi<=lo||e->neighbor<0){wall(f,x,low,high,sec->floor,sec->ceiling,eye,t,texu,e->material,light,sec->room);break;}
            wall(f,x,low,high,hi,sec->ceiling,eye,t,texu,e->door==4?0:e->door>=0?2:e->material,light,sec->room);
            wall(f,x,low,high,sec->floor,lo,eye,t,texu,g->sectors[e->neighbor].prop?0:4,light,sec->room);
            /* A camera exactly on a portal edge has t=0. Clip projection
             * distance before integer conversion, while retaining true t
             * for traversal/depth. */
            double project=fmax(.001,t);
            int top=(int)ceil(RN_H/2+(eye-hi)*focal/project),bottom=(int)floor(RN_H/2+(eye-lo)*focal/project);
            if(top>low)low=top;
            if(bottom<high)high=bottom;
            if(low>high)break;
            s=e->neighbor;if(s<0||s>=g->sector_count){f->cap_hits++;break;}after=t;f->portals++;
            if(k==RN_SECTORS-1)f->cap_hits++;
        }
    }
    for(int i=0;i<g->enemy_count&&i<RN_ENEMIES;i++){
        const struct rn_actor *e=&g->enemy[i];if(e->hp<=0&&!e->death)continue;
        int facing=((int)floor((atan2(g->y-e->y,g->x-e->x)-e->angle)/(RN_PI/4)+.5)+16)&7;
        int state=e->hp<=0?3:e->hit>0?4:e->flash>0?2:e->windup>0?1:0;
        double height=e->kind?.65:1.7;
        sprite(g,f,e->x,e->y,e->z,height,e->kind,(e->walk/12)%4,facing,state);
    }
    for(int i=0;i<g->item_count&&i<RN_ITEMS;i++)if(!g->item[i].taken){const struct rn_item *p=&g->item[i];sprite(g,f,p->x,p->y,p->z,.9,p->kind==RN_GUN?7:p->kind==RN_SHELLS?8:2+p->kind,0,0,0);}
    if(!g->slice){
        sprite(g,f,6.5,12.04,1.9,.5,9,0,0,0);
        sprite(g,f,.04,5.65,1.6,.65,9,1,0,0);
        sprite(g,f,-5.36,10.25,1.4,.4,9,2,0,0);
        sprite(g,f,9.5,-5.7,1.5,1.2,4,0,0,0);sprite(g,f,13,16,0,2.0,5,0,0,0);}
    /* Impact is an opaque five-pixel spark, depth-tested against the wall. */
    if(g->impact_ms){double dx=g->impact_x-g->x,dy=g->impact_y-g->y,t=dx*ca+dy*sa;if(t>.1){double px=RN_W/2+(-dx*sa+dy*ca)*focal/t,py=RN_H/2+(eye-g->impact_z)*focal/t;if(fabs(px)<10000&&fabs(py)<10000)for(int a=-2;a<=2;a++){put(f,(int)px+a,(int)py,t-.01,0xff4c);put(f,(int)px,(int)py+a,t-.01,0xff4c);}}}
    int recoil=g->reduced_motion?0:g->flash/(g->weapon==RN_SHOTGUN?10:14);
    for(int y=0;y<96;y++)for(int x=0;x<128;x++){uint16_t c=rn_weapon(x,y,g->flash,g->reloading,g->weapon,g->reduced_motion);int xx=RN_W/2-35+x,yy=RN_H-96+y+recoil;if(c&&yy<RN_H)f->pixels[yy*RN_W+xx]=c;}
    for(int a=-4;a<=4;a++)if(abs(a)>1){f->pixels[(RN_H/2)*RN_W+RN_W/2+a]=0xce79;f->pixels[(RN_H/2+a)*RN_W+RN_W/2]=0xce79;}
    if(g->hitmark)for(int a=3;a<=6;a++)for(int sign=-1;sign<=1;sign+=2){f->pixels[(RN_H/2+a)*RN_W+RN_W/2+sign*a]=0xe609;f->pixels[(RN_H/2-a)*RN_W+RN_W/2+sign*a]=0xe609;}
    if(g->hurt)for(int y=0;y<RN_H;y++){f->pixels[y*RN_W]=0xb9e5;f->pixels[y*RN_W+RN_W-1]=0xb9e5;}
    return true;
}
