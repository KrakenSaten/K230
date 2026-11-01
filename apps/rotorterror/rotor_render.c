/* Original RGB565 pixel artwork: the Ember Coast and the Kestrel gunship.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "rotor.h"
#include <math.h>
#include <stdlib.h>

struct ink { struct rt_frame *f; int shake_x, shake_y, mirror_at; bool mirrored; };
struct point { int x, y; };

static uint16_t rgb(unsigned c)
{
    return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x7e0) | ((c >> 3) & 31));
}

static unsigned mix(unsigned a, unsigned b, int t)
{
    int r=(int)(a>>16)+((int)(b>>16)-(int)(a>>16))*t/256;
    int g=(int)((a>>8)&255)+((int)((b>>8)&255)-(int)((a>>8)&255))*t/256;
    int blue=(int)(a&255)+((int)(b&255)-(int)(a&255))*t/256;
    return (unsigned)((r<<16)|(g<<8)|blue);
}

static void pixel(struct ink *d, int x, int y, unsigned color)
{
    if(d->mirrored)x=2*d->mirror_at-x;
    x+=d->shake_x;y+=d->shake_y;
    if((unsigned)x<RT_W && (unsigned)y<(unsigned)d->f->height)
        d->f->pixels[y*RT_W+x]=rgb(color);
}

static void rect(struct ink *d, int x, int y, int w, int h, unsigned color)
{
    if(d->mirrored)x=2*d->mirror_at-x-w+1;
    x+=d->shake_x;y+=d->shake_y;
    int x1=x<0?0:x, y1=y<0?0:y, x2=x+w>RT_W?RT_W:x+w;
    int y2=y+h>d->f->height?d->f->height:y+h;
    uint16_t c=rgb(color);
    for(int row=y1;row<y2;row++)for(int col=x1;col<x2;col++)d->f->pixels[row*RT_W+col]=c;
}

static void line(struct ink *d, int x, int y, int x2, int y2, unsigned c)
{
    int dx=abs(x2-x),sx=x<x2?1:-1,dy=-abs(y2-y),sy=y<y2?1:-1,e=dx+dy;
    for(;;) {
        pixel(d,x,y,c);
        if(x==x2 && y==y2)break;
        int e2=e*2;
        if(e2>=dy) { e+=dy;x+=sx; }
        if(e2<=dx) { e+=dx;y+=sy; }
    }
}

static void ellipse(struct ink *d, int x, int y, int rx, int ry, unsigned c)
{
    if(rx<1 || ry<1)return;
    for(int j=-ry;j<=ry;j++) {
        int half=(int)(rx*sqrtf(fmaxf(0,1-(float)(j*j)/(ry*ry))));
        rect(d,x-half,y+j,half*2+1,1,c);
    }
}

static void ring(struct ink *d, int x, int y, int rx, int ry, unsigned color, int gaps)
{
    for(int i=0;i<96;i++) {
        if(gaps && (i/8)%3==0)continue;
        float a=i*.06545f;
        pixel(d,x+(int)(cosf(a)*rx),y+(int)(sinf(a)*ry),color);
    }
}

static void polygon(struct ink *d, const struct point *p, int count, unsigned color)
{
    int min=p[0].y,max=min;
    for(int i=1;i<count;i++) { if(p[i].y<min)min=p[i].y;if(p[i].y>max)max=p[i].y; }
    if(min<0)min=0;
    if(max>d->f->height)max=d->f->height;
    for(int y=min;y<=max;y++) {
        int crosses[12],n=0;
        for(int j=0,k=count-1;j<count;k=j++) {
            if((p[j].y<=y && p[k].y>y)||(p[k].y<=y && p[j].y>y))
                crosses[n++]=p[j].x+(p[k].x-p[j].x)*(y-p[j].y)/(p[k].y-p[j].y);
        }
        for(int i=1;i<n;i++)for(int j=i;j>0 && crosses[j]<crosses[j-1];j--) {
            int v=crosses[j];crosses[j]=crosses[j-1];crosses[j-1]=v;
        }
        for(int i=0;i+1<n;i+=2)rect(d,crosses[i],y,crosses[i+1]-crosses[i]+1,1,color);
    }
}

static struct point transform(float cx,float cy,float x,float y,float scale,float angle)
{
    float c=cosf(angle),s=sinf(angle);
    return (struct point){(int)(cx+(x*c-y*s)*scale),(int)(cy+(x*s+y*c)*scale)};
}

static void part(struct ink *d,float x,float y,float scale,float angle,
                 const int coords[][2],int n,unsigned color)
{
    struct point p[10];
    for(int i=0;i<n;i++)p[i]=transform(x,y,coords[i][0],coords[i][1],scale,angle);
    polygon(d,p,n,color);
}

static void stroke(struct ink *d,float x,float y,float scale,float angle,
                   int x1,int y1,int x2,int y2,unsigned c)
{
    struct point a=transform(x,y,x1,y1,scale,angle),b=transform(x,y,x2,y2,scale,angle);
    line(d,a.x,a.y,b.x,b.y,c);
}

static void helicopter(struct ink *d,float x,float y,float scale,float angle,
                       unsigned phase,bool hostile,bool hit)
{
    if(hostile) { d->mirrored=true;d->mirror_at=(int)x; }
    static const int tail[][2]={{-49,-9},{-13,-5},{-14,5},{-47,-3}};
    static const int fin[][2]={{-49,-18},{-40,-16},{-39,-4},{-48,-4}};
    static const int body[][2]={{-23,-8},{-12,-14},{11,-12},{29,-2},{30,5},{18,12},{-18,10},{-25,4}};
    static const int armor[][2]={{-23,-3},{-7,-3},{-3,9},{-18,9},{-25,3}};
    static const int cabin[][2]={{1,-11},{12,-10},{25,-2},{23,2},{4,0}};
    static const int cabin2[][2]={{-6,-11},{-1,-11},{2,0},{-8,-1}};
    static const int belly[][2]={{-17,8},{20,8},{14,13},{-11,12}};
    static const int engine[][2]={{-21,-12},{-12,-18},{0,-17},{3,-12}};
    unsigned main=hit?0xfff3cf:hostile?0xbc4d64:0x43d7cd;
    unsigned light=hit?0xffffff:hostile?0xff977e:0xa2fff0;
    unsigned dark=hostile?0x652e54:0x1e717c;
    /* Dark silhouette, lit upper armor and separate cockpit glazing keep the
     * aircraft readable over both bright water and dark industrial scenery. */
    part(d,x,y+2*scale,scale*1.04f,angle,body,8,0x091725);
    part(d,x,y,scale,angle,tail,4,dark);
    part(d,x,y,scale,angle,fin,4,main);
    part(d,x,y,scale,angle,body,8,main);
    part(d,x,y,scale,angle,armor,5,dark);
    part(d,x,y,scale,angle,belly,4,0x163d53);
    part(d,x,y,scale,angle,engine,4,0x274759);
    part(d,x,y,scale,angle,cabin,5,0x173349);
    part(d,x,y,scale,angle,cabin2,4,0x1c4a5b);
    stroke(d,x,y,scale,angle,3,-10,13,-8,0xbaf9e7);
    stroke(d,x,y,scale,angle,14,-8,22,-3,0x4b9eac);
    stroke(d,x,y,scale,angle,-19,-8,-10,-12,light);
    stroke(d,x,y,scale,angle,-16,1,-8,1,light);
    stroke(d,x,y,scale,angle,-10,-15,-10,-23,0xbbe8dc);
    stroke(d,x,y,scale,angle,-9,-22,24,-22,0x263d53);
    int rotor=phase%3==0?40:phase%3==1?29:46;
    stroke(d,x,y,scale,angle,-10-rotor,-23,-10+rotor,-23,0x83c5c0);
    stroke(d,x,y,scale,angle,-10-rotor,-24,-10+rotor,-24,0xd1f3dc);
    stroke(d,x,y,scale,angle,-15,12,-19,19,0x90bec4);
    stroke(d,x,y,scale,angle,15,12,13,19,0x90bec4);
    stroke(d,x,y,scale,angle,-26,19,26,19,0x345768);
    stroke(d,x,y,scale,angle,-26,18,26,18,0x83b0b8);
    stroke(d,x,y,scale,angle,16,9,35,9,0x111f37);
    stroke(d,x,y,scale,angle,18,10,34,10,0xb6c7b7);
    stroke(d,x,y,scale,angle,-22,-11,-28,-11,0xffb65f);
    stroke(d,x,y,scale,angle,-22,-9,-29,-9,phase%2?0xfc705a:0xfdd896);
    static const int wing[][2]={{-14,3},{8,3},{15,9},{-18,8}};
    part(d,x,y,scale,angle,wing,4,hostile?0xb36970:0x308e9b);
    stroke(d,x,y,scale,angle,-19,10,17,11,0x081c2c);
    stroke(d,x,y,scale,angle,-18,-13,-2,-13,light);
    stroke(d,x,y,scale,angle,5,-8,18,-3,hostile?0xffc2a5:0x73d5e2);
    stroke(d,x,y,scale,angle,6,-7,17,-4,0x32728a);
    stroke(d,x,y,scale,angle,7,-1,10,5,dark);
    for(int i=0;i<4;i++) {
        stroke(d,x,y,scale,angle,-19+i*3,-12,-20+i*3,-8,0x102a3d);
        stroke(d,x,y,scale,angle,-14+i*6,7,-13+i*6,7,light);
    }
    stroke(d,x,y,scale,angle,-43,-14,-42,-9,0xffd59a);
    stroke(d,x,y,scale,angle,-11,-24,-8,-24,0xffffff);
    stroke(d,x,y,scale,angle,3,8,12,8,0x142638);
    stroke(d,x,y,scale,angle,4,9,12,9,0xe4bb7b);
    if(phase%24<12)stroke(d,x,y,scale,angle,-46,-18,-44,-18,0xff7067);
    float spin=phase*.65f;
    int tx=(int)(cosf(spin)*8),ty=(int)(sinf(spin)*8);
    stroke(d,x,y,scale,angle,-45-tx,-7-ty,-45+tx,-7+ty,light);
    stroke(d,x,y,scale,angle,-45+ty,-7-tx,-45-ty,-7+tx,light);
    stroke(d,x,y,scale,angle,-5,5,4,5,0xf6e5b4);
    stroke(d,x,y,scale,angle,-5,6,-2,6,0xf6e5b4);
    d->mirrored=false;
}

static uint32_t hash(unsigned n)
{
    n=(n^61u)^(n>>16);n*=9;n^=n>>4;n*=0x27d4eb2du;n^=n>>15;
    return n;
}

static void background(struct ink *d,const struct rt_game *g,bool title)
{
    int h=d->f->height, horizon=h*71/100;
    unsigned zone=(g->wave-1)/3%3;
    static const unsigned top[]={0x111b35,0x18172e,0x0b283b};
    static const unsigned sky[]={0x80506e,0x684369,0x426979};
    static const unsigned glow[]={0xf69c7d,0xd88199,0xa6b6aa};
    for(int y=0;y<h;y++) {
        unsigned c=y<horizon?mix(top[zone],sky[zone],y*256/horizon):mix(sky[zone],0x16283b,(y-horizon)*256/(h-horizon));
        if(y>horizon-50 && y<horizon)c=mix(c,glow[zone],(y-horizon+50)*2);
        rect(d,0,y,RT_W,1,c);
    }
    int sx=title?430:475,sy=horizon-70,sr=title?69:47;
    ellipse(d,sx,sy,sr+9,sr+9,mix(sky[zone],glow[zone],86));
    for(int y=-sr;y<=sr;y++) {
        if(y>4 && ((y+sr)%9>5))continue;
        int hw=(int)sqrtf((float)(sr*sr-y*y));
        rect(d,sx-hw,sy+y,hw*2,1,mix(0xffe6a5,0xff7d78,(y+sr)*256/(sr*2)));
    }
    for(unsigned i=0;i<42;i++) {
        unsigned r=hash(i+3);
        int x=(int)(r%RT_W),y=(int)((r>>10)%(unsigned)(horizon-55));
        if(y<horizon/2)pixel(d,x,y,i%5==0?0xc7c7c3:0x626379);
    }
    /* Soft cloud silhouettes and distant ridges move independently of the
     * foreground. All scenery is deterministic and allocation-free. */
    for(int i=0;i<7;i++) {
        int x=((int)(hash(i+80)%820)-(int)fmodf(g->distance*.12f,820))%820;
        if(x< -130)x+=820;
        int y=28+(int)(hash(i+41)%(unsigned)(horizon-65));
        unsigned c=mix(sky[zone],0xaab4c4,35);
        ellipse(d,x,y,56,5,c);ellipse(d,x+31,y-4,34,7,c);
        line(d,x-44,y+5,x+75,y+5,mix(c,glow[zone],40));
    }
    for(int layer=0;layer<3;layer++) {
        int step=layer==0?110:layer==1?76:49;
        float speed=layer==0?.10f:layer==1?.25f:.52f;
        int offset=(int)(g->distance*speed),base=horizon+layer*17;
        unsigned c=layer==0?mix(sky[zone],0x24334b,135):layer==1?0x29374d:0x142b3c;
        for(int i=-2;i<RT_W/step+3;i++) {
            int index=i+offset/step,x=i*step-offset%step;
            unsigned r=hash((unsigned)(index+100000+layer*900));
            int bh=22+(int)(r%57)+layer*9,bw=step-7;
            if(zone!=2 && (layer<2 || zone==1)) {
                int peak=x+step/2;
                struct point ridge[]={{x-30,base},{x+6,base-bh/2},
                    {peak,base-bh},{x+step-9,base-bh/3},{x+step+35,base}};
                polygon(d,ridge,5,c);
                struct point face[]={{peak,base-bh},{peak+9,base-bh/2},
                    {x+step+35,base},{x+step-9,base-bh/3}};
                polygon(d,face,4,mix(c,0x101e34,65));
                line(d,peak,base-bh,peak-12,base-bh+15,mix(c,glow[zone],80));
                if(zone==1 && layer==0) {
                    struct point snow[]={{peak,base-bh},{peak-12,base-bh+17},
                        {peak-3,base-bh+12},{peak+9,base-bh+18}};
                    polygon(d,snow,4,0xa898b0);
                }
            } else {
                rect(d,x,base-bh,bw,bh,c);
                rect(d,x+2,base-bh,bw-4,2,mix(c,glow[zone],60));
                rect(d,x+bw-6,base-bh+2,6,bh-2,0x112536);
                if((r&3)==0) {
                    rect(d,x+7,base-bh-10,bw-14,10,c);
                    line(d,x+bw/2,base-bh-27,x+bw/2,base-bh,c);
                    pixel(d,x+bw/2,base-bh-27,0xffa584);
                }
                for(int wy=0;wy<bh-10;wy+=9)
                    for(int wx=5;wx<bw-7;wx+=7)
                        if(hash((unsigned)(wx+wy*300+index))%3==0)
                            rect(d,x+wx,base-bh+6+wy,2,3,layer==2?0xc49b76:0x777187);
                if(layer==2 && (r&3)==0) {
                    rect(d,x+5,base-bh+12,3,18,0x397681);
                    rect(d,x+6,base-bh+14,1,14,0x83e5cf);
                }
            }
        }
    }
    int water=horizon+33;
    for(int y=water;y<h-19;y++) {
        rect(d,0,y,RT_W,1,mix(0x284458,0x0c2132,(y-water)*256/(h-water)));
        if(y%3==0) {
            unsigned r=hash((unsigned)y);
            int width=18+(y-water)*2;
            int x=sx-width/2+(int)(r%19)-9;
            rect(d,x,y,width,1,mix(0x495667,glow[zone],50+(int)(r%60)));
        }
    }
    for(int i=0;i<70;i++) {
        unsigned r=hash((unsigned)i+901);
        int y=water+(int)(r%(unsigned)(h-water-18));
        int x=((int)((r>>10)%760)-(int)(g->distance*(.35f+(float)(y-water)/h)))%760;
        if(x<0)x+=760;
        rect(d,x,y,3+(int)(r%15),1,0x456274);
    }
    /* Near-field seawall: fast parallax provides a tangible sense of speed. */
    rect(d,0,h-19,RT_W,19,0x081722);
    rect(d,0,h-19,RT_W,2,0x517077);
    rect(d,0,h-15,RT_W,2,0x243e4b);
    int offset=(int)(g->distance*.9f)%80;
    for(int x=-offset;x<RT_W;x+=80) {
        rect(d,x,h-13,2,13,0x31444e);
        rect(d,x+10,h-10,23,3,0x182d39);
        rect(d,x+45,h-18,12,2,0xe4b47c);
        rect(d,x+48,h-16,6,2,0x775947);
    }
}

static void enemy(struct ink *d,const struct rt_enemy *e,unsigned tick)
{
    int x=(int)e->x,y=(int)e->y;
    bool flash=e->hit_ms>0;
    unsigned coral=flash?0xfff2c3:0xff8b7b,dark=0x753851;
    if(e->kind==RT_INTERCEPTOR) {
        helicopter(d,x,y,1.05f,sinf(e->age*.003f)*.1f,tick,true,flash);
    } else if(e->kind==RT_BOSS) {
        static const int hull[][2]={{-64,-5},{-47,-20},{29,-19},{58,-9},
            {66,6},{40,19},{-36,19},{-60,9}};
        static const int deck[][2]={{-35,-20},{-20,-29},{12,-27},{28,-19}};
        static const int armor[][2]={{-28,-6},{54,-6},{43,13},{-25,13}};
        part(d,x+2,y+3,1.03f,0,hull,8,0x091927);
        part(d,x,y,1,0,hull,8,dark);
        part(d,x,y,1,0,deck,4,0x3d5061);
        part(d,x,y,1,0,armor,4,flash?0xfff2c3:0xb15a6b);
        line(d,x-45,y-19,x+28,y-19,coral);
        line(d,x-22,y-5,x+50,y-5,0xef9c89);
        for(int j=0;j<3;j++) {
            rect(d,x-52+j*10,y-12,8,9,0x102b3e);
            rect(d,x-51+j*10,y-11,6,2,0xffd49b);
        }
        for(int i=0;i<5;i++) {
            rect(d,x-15+i*12,y+1,7,6,0x412c43);
            rect(d,x-14+i*12,y+2,3,2,0xffc488);
            line(d,x-15+i*12,y+11,x-10+i*12,y+11,0xde8891);
        }
        for(int side=-1;side<=1;side+=2) {
            int xx=x+side*32;
            rect(d,xx-12,y+17,24,8,0x17293b);
            rect(d,xx-8,y+24,16,3,0xc58070);
            rect(d,xx-4,y+25,8,2,0xffe0a5);
            line(d,xx,y-19,xx,y-35,0x9baeba);
            int r=tick%2?37:28;
            line(d,xx-r,y-35,xx+r,y-35,0x6f8497);
            line(d,xx-r+5,y-36,xx+r-5,y-36,0xd1d6ca);
            rect(d,xx-3,y-37,6,3,0xf4b58e);
        }
        rect(d,x-66,y+8,24,5,0x0c1c2c);
        line(d,x-66,y+8,x-44,y+8,0xd5a18c);
    } else if(e->kind==RT_TURRET) {
        rect(d,x-19,y+3,39,18,0x263c50);rect(d,x-15,y-7,29,17,dark);
        rect(d,x-22,y-5,21,6,coral);rect(d,x-12,y-10,13,4,0xffbf8e);
        for(int i=-14;i<18;i+=8)rect(d,x+i,y+15,4,3,0xc39685);
    } else if(e->kind==RT_MINE) {
        ring(d,x,y,21,21,0x88546e,1);
        for(int i=0;i<8;i++) {
            float a=i*.7854f+tick*.015f;
            line(d,x+(int)(cosf(a)*10),y+(int)(sinf(a)*10),x+(int)(cosf(a)*17),y+(int)(sinf(a)*17),coral);
        }
        ellipse(d,x,y,10,10,0x332b4d);ellipse(d,x,y,5,5,tick%20<10?0xffb793:0xff5c79);
        pixel(d,x-2,y-2,0xffecbb);
    } else {
        ellipse(d,x,y,13,6,dark);rect(d,x-7,y-5,14,10,coral);
        rect(d,x-18,y-5,9,2,0x9c7890);rect(d,x+9,y-5,9,2,0x9c7890);
        line(d,x-19,y-8,x-6,y-8,0xd0a7ac);line(d,x+6,y-8,x+19,y-8,0xd0a7ac);
        rect(d,x-3,y-1,6,3,0xffecab);rect(d,x-5,y+6,10,2,0x814a64);
    }
    if(e->kind!=RT_MINE && e->age<650)ring(d,x,y,24,24,0xf4b782,1);
    if(e->fire_ms<550 && e->x>0 && e->x<RT_W && e->kind!=RT_MINE) {
        ring(d,x-18,y,5,5,0xffbc88,0);
        line(d,x-23,y,x-29,y,0xffca9a);
    }
    if(e->hp<e->max_hp && e->kind!=RT_BOSS) {
        rect(d,x-15,y-30,30,2,0x3c314c);
        rect(d,x-15,y-30,30*e->hp/e->max_hp,2,0xffa67d);
    }
}

void rt_render(const struct rt_game *g,struct rt_frame *frame,bool title)
{
    frame->height=g->height;
    struct ink d={.f=frame};
    background(&d,g,title);
    if(title) {
        int x=frame->height>400?RT_W/2+30:RT_W*70/100;
        int y=frame->height>400?frame->height*49/100:frame->height*49/100;
        float scale=frame->height>400?3.4f:2.7f;
        ring(&d,x,y,115,86,0x5d7484,1);ring(&d,x,y,126,97,0x405369,1);
        line(&d,x-152,y,x-130,y,0xa3aeb1);line(&d,x+132,y,x+155,y,0xa3aeb1);
        helicopter(&d,x,y,scale,-.10f,g->steps/2,false,false);
        /* Foreground vapour and running lights under the hero silhouette. */
        for(int i=0;i<12;i++) {
            int xx=(x-180+i*47)%RT_W,yy=y+90+(i%3)*8;
            rect(&d,xx,yy,20+i%4*8,1,0x5f6979);
        }
        return;
    }
    if(g->shake_ms>0 && !g->reduced_motion) {
        d.shake_x=(int)(hash(g->steps)%5)-2;
        d.shake_y=(int)(hash(g->steps+40)%3)-1;
    }
    for(int i=0;i<RT_PICKUPS;i++) {
        const struct rt_pickup *p=&g->pickups[i];
        if(!p->active)continue;
        int x=(int)p->x,y=(int)p->y;
        unsigned c=p->kind==0?0xffdc92:p->kind==1?0x8ef7dc:0xf58baf;
        ring(&d,x,y,10,10,p->kind==0?0x92745f:0x477a86,1);
        struct point diamond[]={{x,y-6},{x+5,y},{x,y+6},{x-5,y}};
        polygon(&d,diamond,4,c);line(&d,x,y-4,x,y+3,0xfff1c7);
        if(p->kind>0)line(&d,x-3,y,x+3,y,0x26384f);
    }
    for(int i=0;i<RT_PARTICLES;i++) {
        const struct rt_particle *p=&g->particles[i];
        if(p->life<=0)continue;
        static const unsigned colors[]={0xfff4c8,0xffa568,0xffdc92,0xb17b81,0x8cf5df};
        int t=256-p->life*256/p->max_life;
        unsigned color=mix(colors[p->color%5],0x35374f,t);
        int x=(int)p->x,y=(int)p->y;
        if(p->color==1 && p->size>=3) {
            int radius=(int)p->size+(p->max_life-p->life)/85;
            ellipse(&d,x,y,radius,radius,mix(0x783a4b,0x30364a,t));
            ellipse(&d,x,y,(int)p->size,(int)p->size,color);
            if(t<100)ellipse(&d,x-1,y-1,2,2,0xffedb5);
        } else {
            line(&d,x-(int)(p->vx*.025f),y-(int)(p->vy*.025f),x,y,color);
            rect(&d,x,y,(int)p->size,(int)p->size,color);
        }
    }
    for(int i=0;i<RT_ENEMIES;i++)if(g->enemies[i].active)enemy(&d,&g->enemies[i],g->steps);
    for(int i=0;i<RT_SHOTS;i++) {
        const struct rt_shot *s=&g->shots[i];
        if(!s->active)continue;
        int x=(int)s->x,y=(int)s->y;
        if(s->rocket) {
            float angle=atan2f(s->vy,s->vx);
            int tail=10+(g->steps%3)*3;
            stroke(&d,x,y,1,angle,-tail,0,-5,0,0xff875b);
            stroke(&d,x,y,1,angle,-9,0,-4,0,0xffedb1);
            stroke(&d,x,y,1,angle,-4,-1,4,-1,0x86d6d7);
            stroke(&d,x,y,1,angle,-4,0,6,0,0xf3fff1);
            stroke(&d,x,y,1,angle,-4,1,3,1,0x3d7b91);
            stroke(&d,x,y,1,angle,-3,-3,-1,3,0xb7cfcc);
        } else if(s->hostile) {
            ellipse(&d,x,y,4,4,0x90435e);ellipse(&d,x,y,2,2,0xffa47f);pixel(&d,x,y,0xffe0ac);
        } else {
            line(&d,x-(int)(s->vx*.013f),y-(int)(s->vy*.013f),x,y,0xffcd83);
            line(&d,x-3,y,x+2,y,0xfff8d9);
        }
    }
    if(g->phase!=RT_OVER || g->elapsed_ms%160<80)
        helicopter(&d,g->x,g->y,1,g->angle,g->steps,false,g->hit_ms>140);
    if(g->shield>0 && g->phase!=RT_OVER) {
        unsigned c=g->hit_ms>0?0xc7ffeb:g->shield<25?0xb18aab:0x4c8b9b;
        int charged=(int)(96*g->shield/g->shield_max);
        for(int i=0;i<charged;i++) {
            if(g->hit_ms<=0 && (i/8)%3==0)continue;
            float angle=i*.06545f;
            pixel(&d,(int)g->x-5+(int)(cosf(angle)*48),
                     (int)g->y-2+(int)(sinf(angle)*35),c);
        }
        if(g->hit_ms>0) {
            ring(&d,(int)g->x-5,(int)g->y-2,52,39,0x6fb8b7,1);
            for(int i=0;i<6;i++) {
                float a=i*1.0472f;
                int x=(int)g->x+(int)(cosf(a)*43),y=(int)g->y+(int)(sinf(a)*32);
                ring(&d,x,y,6,6,0xd1fff0,1);
            }
        }
    }
    if(g->gun_ms>75 && g->phase==RT_FLIGHT) {
        struct point muzzle=transform(g->x,g->y,35,8,1,g->angle);
        line(&d,muzzle.x,muzzle.y,muzzle.x+10,muzzle.y,0xfff5cf);
        line(&d,muzzle.x+4,muzzle.y-3,muzzle.x+5,muzzle.y+3,0xffc385);
    }
    /* Wave progress is intentionally independent from the score HUD. */
    d.shake_x=d.shake_y=0;
    int progress=g->wave_ms<RT_WAVE_MS?g->wave_ms:RT_WAVE_MS;
    rect(&d,0,frame->height-2,RT_W,2,0x254354);
    rect(&d,0,frame->height-2,progress*RT_W/RT_WAVE_MS,2,0x86e3cc);
    for(int i=0;i<RT_ENEMIES;i++) {
        const struct rt_enemy *e=&g->enemies[i];
        if(e->active && e->kind==RT_BOSS) {
            rect(&d,RT_W/2-96,49,192,5,0x472e48);
            rect(&d,RT_W/2-96,49,192*e->hp/e->max_hp,5,0xff957c);
            break;
        }
    }
}
