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
/* Per frame, per screen row: the floor or ceiling plane of the last sector
 * cast on that row (sector -1: none yet). */
struct plane_row { double plane; int sector,shade; };
/* (int)floor(v) for the per-pixel texture coordinates, which are finite and
 * far inside int: on rv64gc floor() is a libm call, this is two instructions. */
static inline int ifloor(double v)
{
    int i=(int)v;
    return i-(i>v);
}
/* Current is authored in texture coordinates, so it follows surfaces rather
 * than sliding over the screen. The simulation clock freezes on pause. */
static uint16_t surface(int m,int x,int y,int light,int room,unsigned phase)
{
    uint16_t e,c=rn_texel(m,x,y,light,&e);
    int r=c>>11,g=(c>>5)&63,b=c&31;
    if(e){
        int pulse=((x+y+(int)phase)&63),dim=pulse<12?0:3;
        r-=((e>>11)*dim)>>3;g-=(((e>>5)&63)*dim)>>3;b-=((e&31)*dim)>>3;
    }
    /* Gallery violet, relay arctic blue; red/amber remain hazard colors. */
    if(room==2||room==3){r+=b/9;g=g*15/16;}
    if(room==4){g+=b/8;}
    return (uint16_t)(((r>31?31:r)<<11)|((g>63?63:g)<<5)|(b>31?31:b));
}
static void put(struct rn_frame *f,int x,int y,double distance,uint16_t c)
{
    if(x<0||y<0||x>=RN_W||y>=RN_H||distance<=0||!isfinite(distance))return;
    unsigned d=(unsigned)fmin(65534,distance*512);int p=y*RN_W+x;
    if(d<f->depth[p]){f->depth[p]=(uint16_t)d;f->pixels[p]=c;}
}
static void wall(struct rn_frame *f,int x,int clip0,int clip1,double z0,double z1,double eye,double t,double u,int material,int light,int room,unsigned phase)
{
    if(z1<=z0||t<=.001)return;
    double y0=RN_H/2+(eye-z1)*focal/t,y1=RN_H/2+(eye-z0)*focal/t;
    int start=(int)fmax(clip0,ceil(y0)),end=(int)fmin(clip1,floor(y1));
    /* Constant column depth and texture U; reject hidden pixels before shading. */
    unsigned depth=(unsigned)fmin(65534,t*512);
    int tx=(int)floor(u*32);
    for(int y=start;y<=end;y++){
        int p=y*RN_W+x;if(depth>=f->depth[p])continue;
        double z=eye-(y-RN_H/2)*t/focal;
        f->depth[p]=(uint16_t)depth;
        f->pixels[p]=surface(material,tx,ifloor(z*32),light,room,phase);
    }
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
static uint16_t add(uint16_t c,int r,int g,int b)
{
    r+=(c>>11);g+=(c>>5)&63;b+=c&31;
    return (uint16_t)(((r>31?31:r)<<11)|((g>63?63:g)<<5)|(b>31?31:b));
}
/* Local 4x4 light cells, three bounded taps per axis. No full-resolution blur
 * or heap allocation. Only bright sources cast a halo; depth stays untouched. */
#define BW (RN_W/4)
#define BH (RN_H/4)
static void atmosphere(const struct rn_game *g,struct rn_frame *f,bool economy)
{
    uint8_t light[BW*BH];
    memset(light,0,sizeof(light));
    for(int y=0;y<RN_H;y++)for(int x=0;x<RN_W;x++){
        int p=y*RN_W+x;uint16_t c=f->pixels[p];
        int r=c>>11,green=(c>>5)&63,b=c&31;
        if(!economy&&green>38&&b>20){int i=(y/4)*BW+x/4,v=(green-38)/3;if(v>light[i])light[i]=(uint8_t)v;}
        /* Distance haze: integer arithmetic on the already computed depth.
         * min(depth/3000,6) as six compares, no division. */
        unsigned d=f->depth[p];
        int fog=(d>=3000)+(d>=6000)+(d>=9000)+(d>=12000)+(d>=15000)+(d>=18000);
        int edge=x<18?(18-x)/6:x>RN_W-19?(x-RN_W+19)/6:0;
        /* Near and away from the side vignette the pixel is unchanged
         * (x*16/16 and +0): most of a room's floor, walls and sprites. */
        if(!fog&&!edge)continue;
        r=r*(16-fog)/16;green=green*(16-fog)/16+fog/2;b=b*(16-fog)/16+fog;
        r=r*(16-edge)/16;green=green*(16-edge)/16;b=b*(16-edge)/16;
        f->pixels[p]=(uint16_t)((r<<11)|(green<<5)|b);
    }
    for(int y=1;!economy&&y<BH-1;y++)for(int x=1;x<BW-1;x++){
        int i=y*BW+x,v=(light[i]*4+light[i-1]+light[i+1]+light[i-BW]+light[i+BW])/8;
        if(!v)continue;
        for(int yy=0;yy<4;yy++)for(int xx=0;xx<4;xx++){int p=(y*4+yy)*RN_W+x*4+xx;f->pixels[p]=add(f->pixels[p],v/3,v,v);}
    }
    /* Shot light is local to the emitter. It cannot hide a target's windup. */
    if(g->flash)for(int y=RN_H-110;y<RN_H;y++)for(int x=RN_W/2-66;x<RN_W/2+90;x++){
        int v=10-(abs(x-RN_W/2-18)+abs(y-RN_H+70))/10;
        if(v>0){int p=y*RN_W+x;f->pixels[p]=add(f->pixels[p],v/3,v,v);}
    }
}
static void sparks(const struct rn_game *g,struct rn_frame *f,double ca,double sa,double eye)
{
    if(!g->impact_ms)return;
    double dx=g->impact_x-g->x,dy=g->impact_y-g->y,t=dx*ca+dy*sa;
    if(t<=.1)return;
    double px=RN_W/2+(-dx*sa+dy*ca)*focal/t,py=RN_H/2+(eye-g->impact_z)*focal/t;
    if(fabs(px)>10000||fabs(py)>10000)return;
    int age=g->reduced_motion?3:(100-g->impact_ms)/6;if(age<0)age=0;if(age>20)age=20;
    for(int i=0;i<9;i++){
        int vx=(i*17%11)-5,vy=(i*13%9)-5;
        int x=(int)px+vx*age/4,y=(int)py+vy*age/4+age*age/32;
        put(f,x,y,t-.02,0xffb5);put(f,x-vx/3,y-vy/3,t-.02,0x4eff);
    }
}
bool rn_render_quality(const struct rn_game *g,struct rn_frame *f,bool economy)
{
    if(!g||!f||g->sector_count<1||g->sector_count>RN_SECTORS||g->edge_count>RN_EDGES||g->sector<0||g->sector>=g->sector_count||!isfinite(g->x+g->y+g->z+g->angle))return false;
    if(fabs(g->x)>128||fabs(g->y)>128||fabs(g->z)>16||rn_locate(g,g->x,g->y)!=g->sector)return false;
    for(int i=0;i<g->sector_count;i++){
        const struct rn_sector *s=&g->sectors[i];
        if(!isfinite(s->floor)||!isfinite(s->ceiling)||fabs(s->floor)>16||fabs(s->ceiling)>32||s->first<0||s->count<4||s->first+s->count>g->edge_count)return false;
    }
    memset(f,0,sizeof(*f));memset(f->depth,255,sizeof(f->depth));
    double ca=cos(g->angle),sa=sin(g->angle),eye=g->z+RN_EYE;
    unsigned phase=g->reduced_motion?0:g->steps/4;
    struct plane_row rows[RN_H];
    for(int y=0;y<RN_H;y++)rows[y].sector=-1;
    for(int x=0;x<RN_W;x+=economy?2:1){
        double camera=(2*(x+.5)/RN_W-1)*.66,dx=ca-sa*camera,dy=sa+ca*camera;
        int s=g->sector,low=0,high=RN_H-1;double after=-.00001;f->columns++;
        for(int k=0;k<RN_SECTORS;k++){
            const struct rn_sector *sec=&g->sectors[s];
            if(sec->first<0||sec->count<0||sec->first+sec->count>g->edge_count){f->cap_hits++;break;}
            double t,u;int index=rn_boundary(g,s,g->x,g->y,dx,dy,after,&t,&u);
            if(index<0||t>90){t=90;index=-1;}
            int light=(int)(sec->light/(1+t*.035));
            /* Plane distance is |eye-z|*focal/|screen|, monotonic per half, so
             * only rows that can land in (after,t] are walked (two spare rows
             * each side); the per-row test and its output are unchanged. */
            double near=fmax(.02,after);
            for(int half=0;half<2;half++){
                double a=half?eye-sec->floor:sec->ceiling-eye;if(a<=0||near>=t+.00001)continue;
                double s0=fmin(4*RN_H,a*focal/(t+.00001)),s1=fmin(4*RN_H,a*focal/near);
                int y0=half?(int)floor(RN_H/2-.5+s0)-2:(int)floor(RN_H/2-.5-s1)-2,y1=half?(int)ceil(RN_H/2-.5+s1)+2:(int)ceil(RN_H/2-.5-s0)+2;
                if(half){if(y0<RN_H/2)y0=RN_H/2;}else if(y1>RN_H/2-1)y1=RN_H/2-1;
                y0=y0<low?low:y0;y1=y1>high?high:y1;
                for(int y=y0;y<=y1;y++){
                    double screen=y+.5-RN_H/2;if(fabs(screen)<.001)continue;
                    /* A row's plane distance and shade depend only on the
                     * sector: computed once per run of columns, not per pixel. */
                    struct plane_row *row=&rows[y];
                    if(row->sector!=s){
                        double z=screen>0?sec->floor:sec->ceiling;
                        row->plane=(eye-z)*focal/screen;row->shade=(int)(sec->light/(1+row->plane*.04));row->sector=s;
                    }
                    double plane=row->plane;
                    if(plane>near&&plane<=t+.00001){
                        /* put()'s depth test before the texel is shaded; x, y
                         * and plane (> near > 0, <= t) are always in range. */
                        unsigned d=(unsigned)fmin(65534,plane*512);int p=y*RN_W+x;
                        if(d>=f->depth[p])continue;
                        int tx=ifloor((g->x+dx*plane)*32),ty=ifloor((g->y+dy*plane)*32);
                        int material=screen>0?(sec->floor>0?4:3):5;
                        f->depth[p]=(uint16_t)d;f->pixels[p]=surface(material,tx,ty,row->shade,sec->room,phase);
                    }
                }
            }
            if(index<0)break;
            const struct rn_edge *e=&g->edges[index];double lo,hi;rn_opening(g,s,e,&lo,&hi);
            double texu=u*hypot(e->bx-e->ax,e->by-e->ay);
            if(hi<=lo||e->neighbor<0){wall(f,x,low,high,sec->floor,sec->ceiling,eye,t,texu,e->material,light,sec->room,phase);break;}
            wall(f,x,low,high,hi,sec->ceiling,eye,t,texu,e->door==4?0:e->door>=0?2:e->material,light,sec->room,phase);
            wall(f,x,low,high,sec->floor,lo,eye,t,texu,g->sectors[e->neighbor].prop?0:4,light,sec->room,phase);
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
    if(economy)for(int y=0;y<RN_H;y++)for(int x=0;x<RN_W;x+=2){
        int p=y*RN_W+x;f->pixels[p+1]=f->pixels[p];f->depth[p+1]=f->depth[p];
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
    sparks(g,f,ca,sa,eye);
    atmosphere(g,f,economy);
    int recoil=g->reduced_motion?0:g->flash/(g->weapon==RN_SHOTGUN?10:14);
    for(int y=0;y<96;y++)for(int x=0;x<128;x++){uint16_t c=rn_weapon(x,y,g->flash,g->reloading,g->weapon,g->reduced_motion);int xx=RN_W/2-35+x,yy=RN_H-96+y+recoil;if(c&&yy<RN_H)f->pixels[yy*RN_W+xx]=c;}
    for(int a=-4;a<=4;a++)if(abs(a)>1){f->pixels[(RN_H/2)*RN_W+RN_W/2+a]=0x9f7e;f->pixels[(RN_H/2+a)*RN_W+RN_W/2]=0x9f7e;}
    if(g->hitmark)for(int a=3;a<=6;a++)for(int sign=-1;sign<=1;sign+=2){f->pixels[(RN_H/2+a)*RN_W+RN_W/2+sign*a]=0xff52;f->pixels[(RN_H/2-a)*RN_W+RN_W/2+sign*a]=0xff52;}
    if(g->hurt)for(int y=0;y<RN_H;y++)for(int x=0;x<8;x++){
        int v=(8-x)*(g->hurt>100?3:2);
        int p=y*RN_W+x,q=y*RN_W+RN_W-1-x;
        f->pixels[p]=add(f->pixels[p],v,0,v/4);f->pixels[q]=add(f->pixels[q],v,0,v/4);
    }
    return true;
}

bool rn_render(const struct rn_game *g,struct rn_frame *f)
{
    return rn_render_quality(g,f,false);
}
