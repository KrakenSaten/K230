/* Sector adjacency, plane/segment tracing, support-aware movement and combat.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "relay.h"
#include <math.h>
#include <string.h>
#include <float.h>

static int rect(struct rn_game *g,double x0,double y0,double x1,double y1,double z,double ceil,int room)
{
    if (g->sector_count >= RN_SECTORS) return -1;
    int i=g->sector_count++;
    g->sectors[i]=(struct rn_sector){x0,y0,x1,y1,z,ceil,0,0,room,room==4?220:room==1?205:185,false}; return i;
}
static bool contains(const struct rn_sector *s,double x,double y)
{ return x>=s->x0-1e-6 && x<=s->x1+1e-6 && y>=s->y0-1e-6 && y<=s->y1+1e-6; }
int rn_locate(const struct rn_game *g,double x,double y)
{
    if(!g || !isfinite(x)||!isfinite(y)) return -1;
    for(int i=0;i<g->sector_count && i<RN_SECTORS;i++) if(contains(&g->sectors[i],x,y)) return i;
    return -1;
}
static int door_between(int a,int b)
{
    if((a==7&&b==8)||(b==7&&a==8)) return 4;
    if((a==0&&b==1)||(b==0&&a==1)) return 0;
    if((a==2&&b==3)||(b==2&&a==3)) return 1;
    if((a==1&&b==5)||(b==1&&a==5)) return 2;
    if((a==5&&b==6)||(b==5&&a==6)) return 3;
    return -1;
}
/* Split every authored rectangle side at all adjacent-sector endpoints. This
 * produces explicit directed portal segments, not a collision/raycast grid. */
static bool build_edges(struct rn_game *g)
{
    for(int i=0;i<g->sector_count;i++) {
        struct rn_sector *s=&g->sectors[i]; s->first=g->edge_count;
        for(int side=0;side<4;side++) {
            bool vertical=side==1||side==3;
            double fixed=side==0?s->y0:side==1?s->x1:side==2?s->y1:s->x0;
            double start=vertical?s->y0:s->x0,end=vertical?s->y1:s->x1;
            double points[RN_SECTORS*2+2]; int n=0; points[n++]=start; points[n++]=end;
            for(int j=0;j<g->sector_count;j++) if(j!=i) {
                struct rn_sector *t=&g->sectors[j];
                double other=side==0?t->y1:side==1?t->x0:side==2?t->y0:t->x1;
                if(fabs(fixed-other)>.0001) continue;
                double a=fmax(start,vertical?t->y0:t->x0),b=fmin(end,vertical?t->y1:t->x1);
                if(b>a+.0001) { points[n++]=a;points[n++]=b; }
            }
            for(int a=1;a<n;a++) {double v=points[a];int b=a;while(b&&points[b-1]>v){points[b]=points[b-1];b--;}points[b]=v;}
            for(int a=0;a<n-1;a++) if(points[a+1]-points[a]>.0001) {
                if(g->edge_count>=RN_EDGES) return false;
                double p=points[a],q=points[a+1],mid=(p+q)/2; int neighbor=-1;
                double tx=vertical?fixed+(side==1?.001:-.001):mid;
                double ty=vertical?mid:fixed+(side==2?.001:-.001);
                for(int j=0;j<g->sector_count;j++) if(j!=i&&contains(&g->sectors[j],tx,ty)){neighbor=j;break;}
                struct rn_edge e={0};
                e.ax=vertical?fixed:p;e.ay=vertical?p:fixed;e.bx=vertical?fixed:q;e.by=vertical?q:fixed;
                if(side>=2){float v=e.ax;e.ax=e.bx;e.bx=v;v=e.ay;e.ay=e.by;e.by=v;}
                e.neighbor=neighbor;e.door=neighbor<0?-1:door_between(s->room,g->sectors[neighbor].room);
                e.material=e.door==4?0:e.door>=0?2:(s->room==1?0:s->room==2?1:0);
                g->edges[g->edge_count++]=e;
            }
        }
        s->count=g->edge_count-s->first;
    }
    return true;
}
static void enemy(struct rn_game *g,double x,double y,int kind)
{
    if(g->enemy_count>=RN_ENEMIES) return;
    int s=rn_locate(g,x,y);if(s<0)return;
    struct rn_actor *e=&g->enemy[g->enemy_count++];
    *e=(struct rn_actor){.x=x,.y=y,.z=g->sectors[s].floor,.sector=s,.hp=kind?50:100,.kind=kind,.cooldown=600,.angle=RN_PI/2};
}
static void item(struct rn_game *g,double x,double y,int kind)
{
    int s=rn_locate(g,x,y);if(s<0||g->item_count>=RN_ITEMS)return;
    g->item[g->item_count++]=(struct rn_item){x,y,g->sectors[s].floor,s,kind,false};
}
bool rn_restart(struct rn_game *g,bool slice)
{
    if(!g)return false;
    memset(g,0,sizeof(*g));g->slice=slice;g->health=100;g->mag=24;g->reserve=24;
    /* A service lock, B pump hall split around a solid pump, west stairs C,
     * D compressor bays, east stairs E, separate descending return J and F. */
    rect(g,4,12,9,18,0,2.8,0); /* A */
    rect(g,0,4,12,8,0,5,1);rect(g,0,8,4,12,0,5,1);
    rect(g,8,8,12,12,0,5,1);rect(g,4,10.5,8,12,0,5,1);
    int pump=rect(g,4,8,6,10.5,.9,5,1);g->sectors[pump].prop=true;
    pump=rect(g,6,8,8,10.5,2.7,5,1);g->sectors[pump].prop=true; /* solid-topped machinery plinth; too high to step */
    for(int i=0;i<6;i++) rect(g,-.4*(i+1),5,-.4*i,8,.125*(i+1),5,2);
    rect(g,-5.4,0,-2.4,9,.75,3.5,2); /* C */
    if(slice) {
        g->x=2;g->y=6.5;g->angle=RN_PI;enemy(g,-3.5,6.5,0);
    } else {
        rect(g,-5.4,-7,-1.8,0,.75,4,3); /* D, around solid compressor */
        rect(g,-1.8,-7,.8,-5.6,.75,4,3);
        rect(g,.8,-7,2.6,0,.75,4,3);
        rect(g,-1.8,-2.3,.8,0,.75,4,3);
        int compressor=rect(g,-1.8,-5.6,.8,-2.3,2.2,4,3);g->sectors[compressor].prop=true;
        /* Solid compressor recess, a low wall across a nonwalkable plinth. */
        for(int i=0;i<6;i++) rect(g,2.6+.4*i,-6,3+.4*i,-3,.875+.125*i,4.5,3);
        rect(g,5,-7,11,-1,1.5,4.2,4); /* E */
        for(int i=0;i<12;i++) rect(g,8,-1+.4*i,11,-.6+.4*i,1.375-.125*i,5,5);
        rect(g,8,3.8,15,4,0,5,5); /* return link */
        rect(g,12,4,15,12,0,3,5); /* J adjacent to B: K-controlled shortcut */
        rect(g,11,12,15,17,0,2.8,6); /* F */
        /* Optional maintenance loop joins west pump hall to the gallery's
         * landing through six additional real steps. A scored service panel
         * on its west wall hides a supply recess. No main-route gate. */
        rect(g,-4,9,0,12,0,3.2,7);
        for(int i=0;i<6;i++) rect(g,-5.4,12-.5*(i+1),-4,12-.5*i,.125*(i+1),3.5,7);
        rect(g,-8,10,-5.4,12,.5,3.2,8);
        g->x=6.5;g->y=16;g->angle=-RN_PI/2;
        enemy(g,7,13.2,0);enemy(g,9.5,6,0);enemy(g,2,10,1);enemy(g,-3.8,11.6,1); /* maintenance ambush, lighter pump-hall opening */
        enemy(g,-4,2,0);enemy(g,-3,-4,1);enemy(g,1,-5,1);enemy(g,9,-5,0);enemy(g,13.5,8,1);
        item(g,1.5,9.5,0);item(g,-4,7.8,0);item(g,-4,1.2,1);item(g,-4,-5.5,0);item(g,13.5,10.5,1);
        item(g,-2,10.4,RN_GUN);item(g,-3,11,RN_SHELLS);
        item(g,-7,11,RN_SHELLS);item(g,-7,10.7,RN_MED);
    }
    if(!build_edges(g))return false;
    g->sector=rn_locate(g,g->x,g->y);if(g->sector<0)return false;g->z=g->sectors[g->sector].floor;
    for(int i=0;i<RN_SECTORS;i++)g->route[i]=-1;
    return rn_validate(g);
}
bool rn_validate(const struct rn_game *g)
{
    if(!g||g->sector_count<1||g->sector_count>RN_SECTORS||g->edge_count>RN_EDGES||g->edge_count<4)return false;
    /* Check every range before reciprocal-edge validation dereferences it. */
    for(int i=0;i<g->sector_count;i++) {
        const struct rn_sector *s=&g->sectors[i];
        if(!isfinite(s->x0+s->y0+s->x1+s->y1)||s->first<0||s->count<4||s->first>g->edge_count-s->count)return false;
    }
    for(int i=0;i<g->sector_count;i++) {
        const struct rn_sector *s=&g->sectors[i];
        if(!isfinite(s->floor)||!isfinite(s->ceiling)||s->x0>=s->x1||s->y0>=s->y1||s->ceiling-s->floor<RN_BODY||s->first<0||s->count<4||s->first+s->count>g->edge_count)return false;
        for(int j=i+1;j<g->sector_count;j++) {const struct rn_sector *t=&g->sectors[j];if(fmin(s->x1,t->x1)>fmax(s->x0,t->x0)+.0001&&fmin(s->y1,t->y1)>fmax(s->y0,t->y0)+.0001)return false;}
        for(int j=s->first;j<s->first+s->count;j++) {
            const struct rn_edge *e=&g->edges[j];if(e->neighbor>=g->sector_count||e->neighbor< -1||e->door>=5||e->door< -1)return false;
            if(e->neighbor>=0) {
                const struct rn_sector *n=&g->sectors[e->neighbor];bool reciprocal=false;
                for(int k=n->first;k<n->first+n->count;k++){const struct rn_edge *r=&g->edges[k];if(r->neighbor==i&&fabs(r->ax-e->bx)<.001&&fabs(r->ay-e->by)<.001&&fabs(r->bx-e->ax)<.001&&fabs(r->by-e->ay)<.001&&r->door==e->door)reciprocal=true;}
                if(!reciprocal)return false;
            }
        }
    }
    bool seen[RN_SECTORS]={0};int q[RN_SECTORS],h=0,t=0;
    if(g->sector<0||g->sector>=g->sector_count)return false;
    q[t++]=g->sector;seen[g->sector]=true;
    while(h<t){int s=q[h++];for(int i=g->sectors[s].first;i<g->sectors[s].first+g->sectors[s].count;i++){int n=g->edges[i].neighbor;if(n>=0&&!seen[n]&&fabs(g->sectors[n].floor-g->sectors[s].floor)<=.201){seen[n]=true;q[t++]=n;}}}
    for(int i=0;i<g->sector_count;i++)if(!seen[i]&&!g->sectors[i].prop)return false;
    for(int i=0;i<g->enemy_count;i++)if(rn_locate(g,g->enemy[i].x,g->enemy[i].y)!=g->enemy[i].sector)return false;
    return true;
}
void rn_opening(const struct rn_game *g,int s,const struct rn_edge *e,double *lo,double *hi)
{
    *lo=1;*hi=0;if(e->neighbor<0||e->neighbor>=g->sector_count)return;
    *lo=fmax(g->sectors[s].floor,g->sectors[e->neighbor].floor);
    *hi=fmin(g->sectors[s].ceiling,g->sectors[e->neighbor].ceiling);
    if(e->door>=0)*hi=*lo+(*hi-*lo)*g->door[e->door];
}
static double cross(double ax,double ay,double bx,double by){return ax*by-ay*bx;}
int rn_boundary(const struct rn_game *g,int s,double x,double y,double dx,double dy,double after,double *t,double *u)
{
    int best=-1;*t=DBL_MAX;
    if(!g||s<0||s>=g->sector_count||g->sector_count>RN_SECTORS||g->edge_count<0||g->edge_count>RN_EDGES)return -1;
    if(g->sectors[s].first<0||g->sectors[s].count<0||g->sectors[s].first+g->sectors[s].count>g->edge_count)return -1;
    const struct rn_sector *sec=&g->sectors[s];
    for(int i=sec->first;i<sec->first+sec->count;i++) {
        const struct rn_edge *e=&g->edges[i];double ex=e->bx-e->ax,ey=e->by-e->ay,den=cross(dx,dy,ex,ey);
        if(fabs(den)<1e-10)continue;
        double v=cross(e->ax-x,e->ay-y,ex,ey)/den,w=cross(e->ax-x,e->ay-y,dx,dy)/den;
        if(v>after+1e-6&&v<*t&&w>=-.00001&&w<=1.00001){best=i;*t=v;*u=w;}
    }
    return best;
}
struct rn_hit rn_trace(const struct rn_game *g,int s,double x,double y,double z,double dx,double dy,double dz,double limit)
{
    struct rn_hit hit={.t=0,.edge=-1,.sector=s};
    if(!g||s<0||s>=g->sector_count||!isfinite(x+y+z+dx+dy+dz+limit)||limit<=0)return hit;
    double after=-.00001;
    for(int k=0;k<RN_SECTORS;k++) {
        double t,u;int i=rn_boundary(g,s,x,y,dx,dy,after,&t,&u);
        double plane=limit;
        if(dz< -1e-9)plane=(g->sectors[s].floor-z)/dz;
        if(dz>1e-9)plane=(g->sectors[s].ceiling-z)/dz;
        if(plane>after+1e-6&&plane<t&&plane<limit){hit=(struct rn_hit){plane,x+dx*plane,y+dy*plane,z+dz*plane,0,-1,s,true};return hit;}
        if(i<0||t>=limit)return(struct rn_hit){limit,x+dx*limit,y+dy*limit,z+dz*limit,0,-1,s,false};
        const struct rn_edge *e=&g->edges[i];double lo,hi;rn_opening(g,s,e,&lo,&hi);double hz=z+dz*t;
        if(hz<=lo+.00001||hz>=hi-.00001)return(struct rn_hit){t,x+dx*t,y+dy*t,hz,u,i,s,false};
        after=t;s=e->neighbor;
    }
    return hit; /* A malformed/cyclic traversal stops solid, never leaks shots. */
}
bool rn_los(const struct rn_game *g,int s,double x,double y,double z,double tx,double ty,double tz)
{double d=hypot(tx-x,ty-y);if(d<.0001)return fabs(tz-z)<.1;return rn_trace(g,s,x,y,z,(tx-x)/d,(ty-y)/d,(tz-z)/d,d).t>=d-.001;}
static bool passable(const struct rn_game *g,int s,const struct rn_edge *e,double z)
{
    if(e->neighbor<0 || g->sectors[e->neighbor].prop)return false;
    double lo,hi;rn_opening(g,s,e,&lo,&hi);
    return lo-z<=.201&&hi-fmax(lo,z)>=RN_BODY;
}
static bool fits(const struct rn_game *g,int sector,double x,double y,double z)
{
    for(int i=g->sectors[sector].first;i<g->sectors[sector].first+g->sectors[sector].count;i++) {
        const struct rn_edge *e=&g->edges[i];if(passable(g,sector,e,z))continue;
        double ex=e->bx-e->ax,ey=e->by-e->ay,len=ex*ex+ey*ey;
        double u=fmax(0,fmin(1,((x-e->ax)*ex+(y-e->ay)*ey)/len));
        if(hypot(x-e->ax-u*ex,y-e->ay-u*ey)<RN_RADIUS-.0001)return false;
    }
    return true;
}
static void move(struct rn_game *g,double *x,double *y,double *z,int *sector,double dx,double dy)
{
    for(int axis=0;axis<2;axis++) {
        double nx=*x+(axis?0:dx),ny=*y+(axis?dy:0);int ns=rn_locate(g,nx,ny);
        if(ns<0)continue;
        bool can=ns==*sector;
        if(!can)for(int i=g->sectors[*sector].first;i<g->sectors[*sector].first+g->sectors[*sector].count;i++)if(g->edges[i].neighbor==ns&&passable(g,*sector,&g->edges[i],*z))can=true;
        if(can&&fits(g,*sector,nx,ny,*z)&&fits(g,ns,nx,ny,*z)) {
            *x=nx;*y=ny;*sector=ns;if(g->sectors[ns].floor>*z)*z=g->sectors[ns].floor;
            /* Step-downs follow support; larger legal drops settle by fixed-step gravity. */
            if(*z-g->sectors[ns].floor<=.201)*z=g->sectors[ns].floor;
        }
    }
}
static void navigate(struct rn_game *g)
{
    for(int i=0;i<g->sector_count;i++)g->route[i]=-1;
    int q[RN_SECTORS],h=0,t=0;q[t++]=g->sector;g->route[g->sector]=0;
    while(h<t){int at=q[h++];for(int s=0;s<g->sector_count;s++)if(g->route[s]<0)for(int j=g->sectors[s].first;j<g->sectors[s].first+g->sectors[s].count;j++){
        const struct rn_edge *e=&g->edges[j];if(e->neighbor==at&&hypot(e->bx-e->ax,e->by-e->ay)>RN_RADIUS*2+.02&&passable(g,s,e,g->sectors[s].floor)){g->route[s]=g->route[at]+1;q[t++]=s;break;}
    }}
}
static void notice(struct rn_game *g,int id){g->notice=id;g->notice_ms=3000;}
void rn_switch_weapon(struct rn_game *g)
{
    if(!g||g->paused||g->state!=RN_PLAY)return;
    if(g->shotgun){g->weapon=1-g->weapon;g->reloading=0;g->flash=0;}
    else notice(g,2);
}
static void fire(struct rn_game *g)
{
    if(g->cooldown||g->reloading)return;
    bool scatter=g->weapon==RN_SHOTGUN;
    int *ammo=scatter?&g->loaded:&g->mag;
    g->cooldown=scatter?850:250;
    if(!*ammo){g->sounds[RN_EMPTY]++;return;}
    (*ammo)--;g->flash=scatter?100:70;g->sounds[scatter?RN_SCATTER:RN_SHOT]++;
    /* Five deterministic pellet bearings. Every pellet chooses its nearest
     * visible body and traces the same 3D portal geometry as the carbine. */
    for(int pellet=0;pellet<(scatter?5:1);pellet++){
        double angle=g->angle+(scatter?(pellet-2)*.035:0);
        double dx=cos(angle),dy=sin(angle),best=scatter?7:40;int target=-1;
        for(int i=0;i<g->enemy_count;i++) {
            struct rn_actor *e=&g->enemy[i];if(e->hp<=0)continue;
            double rx=e->x-g->x,ry=e->y-g->y,along=rx*dx+ry*dy,across=rx*dy-ry*dx;
            double height=e->kind?.65:1.65,tz=fmax(e->z+.15,fmin(e->z+height*.8,g->z+RN_EYE));
            if((scatter&&hypot(rx,ry)>=7)||along<=.1||fabs(across)>.32||fabs(atan2(tz-(g->z+RN_EYE),along))>1.05)continue;
            if(along<best&&rn_los(g,g->sector,g->x,g->y,g->z+RN_EYE,e->x,e->y,tz)){best=along;target=i;}
        }
        if(target>=0){struct rn_actor *e=&g->enemy[target];
            int damage=scatter?(best<3?24:best<5?14:7):25;
            e->hp-=damage;e->hit=160;e->awake=true;g->hitmark=130;
            if(e->hp<=0){e->death=500;e->windup=0;e->flash=0;g->kills++;}
            g->sounds[RN_IMPACT]++;
        }
    }
    struct rn_hit h=rn_trace(g,g->sector,g->x,g->y,g->z+RN_EYE,cos(g->angle),sin(g->angle),0,scatter?7:40);
    g->impact_x=h.x;g->impact_y=h.y;g->impact_z=h.z;g->impact_ms=100;
}
static void use(struct rn_game *g)
{
    const struct rn_sector *s=&g->sectors[g->sector];
    for(int i=s->first;i<s->first+s->count;i++) {
        struct rn_edge *e=&g->edges[i];if(e->door<0)continue;
        double ex=e->bx-e->ax,ey=e->by-e->ay,l=ex*ex+ey*ey;
        double u=fmax(0,fmin(1,((g->x-e->ax)*ex+(g->y-e->ay)*ey)/l));
        if(hypot(g->x-e->ax-u*ex,g->y-e->ay-u*ey)<1.5 && (e->door<2||e->door==4||g->relay)) {if(!g->door_want[e->door]){g->door_want[e->door]=true;g->sounds[RN_DOOR]++;if(e->door==4){g->secret=true;notice(g,4);}}}
    }
    if(!g->relay&&s->room==4&&hypot(g->x-9.5,g->y+5.7)<1.5){notice(g,5);g->relay=true;g->door_want[2]=true;g->door_want[3]=true;g->sounds[RN_DOOR]++;}
    if(s->room==6&&g->relay&&g->y>14.5)g->state=RN_WON;
}
static int dec(int x){return x>10?x-10:0;}
static double clamp(double x){return isfinite(x)?fmax(-1,fmin(1,x)):0;}
void rn_pause(struct rn_game *g,bool p){if(g){g->paused=p;g->accumulator=0;g->use_down=g->reload_down=g->switch_down=false;}}
static void step(struct rn_game *g,struct rn_input in)
{
    g->steps++;g->cooldown=dec(g->cooldown);g->flash=dec(g->flash);g->hurt=dec(g->hurt);g->impact_ms=dec(g->impact_ms);g->notice_ms=dec(g->notice_ms);g->hitmark=dec(g->hitmark);
    if(g->reloading){g->reloading=dec(g->reloading);if(!g->reloading){
        int *ammo=g->weapon==RN_SHOTGUN?&g->loaded:&g->mag;
        int *reserve=g->weapon==RN_SHOTGUN?&g->shells:&g->reserve;
        int n=(g->weapon==RN_SHOTGUN?2:24)-*ammo;if(n>*reserve)n=*reserve;*ammo+=n;*reserve-=n;
    }}
    if(in.switch_weapon&&!g->switch_down)rn_switch_weapon(g);
    g->switch_down=in.switch_weapon;
    for(int i=0;i<5;i++)if(g->door_want[i]&&g->door[i]<1){g->door[i]+= .0125f;if(g->door[i]>1)g->door[i]=1;}
    g->angle=remainder(g->angle+clamp(in.turn)*.024,2*RN_PI);
    double f=clamp(in.forward),s=clamp(in.strafe),n=fmax(1,hypot(f,s));
    move(g,&g->x,&g->y,&g->z,&g->sector,(cos(g->angle)*f-sin(g->angle)*s)*.028/n,(sin(g->angle)*f+cos(g->angle)*s)*.028/n);
    double floor=g->sectors[g->sector].floor;
    if(g->z>floor){g->velocity_z-=.098;g->z+=g->velocity_z*.01;if(g->z<=floor){g->z=floor;g->velocity_z=0;}}
    int ammo=g->weapon==RN_SHOTGUN?g->loaded:g->mag,reserve=g->weapon==RN_SHOTGUN?g->shells:g->reserve;
    if(in.reload&&!g->reload_down&&!g->reloading&&ammo<(g->weapon==RN_SHOTGUN?2:24)&&reserve){
        g->reloading=g->weapon==RN_SHOTGUN?1500:900;g->sounds[g->weapon==RN_SHOTGUN?RN_LOAD_SHELL:RN_RELOAD]++;
    }
    g->reload_down=in.reload;if(in.fire)fire(g);
    if(in.use&&!g->use_down)use(g);
    g->use_down=in.use;if(g->state!=RN_PLAY)return;
    for(int i=0;i<g->item_count;i++){struct rn_item *p=&g->item[i];if(p->taken||fabs(p->z-g->z)>.3||hypot(p->x-g->x,p->y-g->y)>.65)continue;
        if(p->kind==RN_MED&&g->health<100){g->health+=30;if(g->health>100)g->health=100;p->taken=true;}
        if(!p->kind&&g->reserve<120){g->reserve+=24;if(g->reserve>120)g->reserve=120;p->taken=true;}
        if(p->kind==RN_GUN&&!g->shotgun){g->shotgun=true;g->loaded=2;g->shells+=4;if(g->shells>20)g->shells=20;g->weapon=RN_SHOTGUN;g->reloading=0;g->flash=0;p->taken=true;notice(g,3);}
        if(p->kind==RN_SHELLS&&g->shells<20){g->shells+=4;if(g->shells>20)g->shells=20;p->taken=true;notice(g,6);}
        if(p->taken)g->sounds[RN_PICKUP]++;
    }
    if(g->nav_clock<=0){navigate(g);g->nav_clock=200;}g->nav_clock-=10;
    for(int i=0;i<g->enemy_count;i++) {
        struct rn_actor *e=&g->enemy[i];e->hit=dec(e->hit);e->death=dec(e->death);e->flash=dec(e->flash);if(e->hp<=0)continue;e->cooldown=dec(e->cooldown);
        double d=hypot(e->x-g->x,e->y-g->y),eye=e->z+(e->kind?.4:1.3);
        bool los=rn_los(g,e->sector,e->x,e->y,eye,g->x,g->y,g->z+1.0);
        if(los&&d<11)e->awake=true;
        if(!e->awake)continue;
        if(e->windup){e->windup=dec(e->windup);if(!e->windup){g->sounds[RN_ATTACK]++;e->flash=120;double vx=g->x-e->x,vy=g->y-e->y;
            double error=fabs(remainder(atan2(vy,vx)-atan2(e->aimy-e->y,e->aimx-e->x),2*RN_PI));
            if(los&&d<(e->kind?1.15:9)&&error<.15){g->health-=e->kind?12:8;g->hurt=150;if(g->health<=0){g->health=0;g->state=RN_DEAD;return;}}
            e->cooldown=e->kind?900:1600;
        }continue;}
        if(los&&d<(e->kind?.85:7)){e->angle=atan2(g->y-e->y,g->x-e->x);if(!e->cooldown){e->windup=e->kind?260:350;e->aimx=g->x;e->aimy=g->y;e->aimz=g->z+1;g->sounds[RN_WINDUP]++;}continue;}
        double tx=g->x,ty=g->y;
        if(e->sector!=g->sector){int best=-1;const struct rn_sector *sec=&g->sectors[e->sector];for(int j=sec->first;j<sec->first+sec->count;j++){struct rn_edge *ed=&g->edges[j];int ns=ed->neighbor;
            if(ns>=0&&g->route[ns]>=0&&(best<0||g->route[ns]<g->route[g->edges[best].neighbor])&&passable(g,e->sector,ed,e->z))best=j;}
            if(best<0)continue;
            struct rn_edge *ed=&g->edges[best];tx=(ed->ax+ed->bx)/2;ty=(ed->ay+ed->by)/2;
            const struct rn_sector *next=&g->sectors[ed->neighbor];double cx=(next->x0+next->x1)/2,cy=(next->y0+next->y1)/2,l=hypot(cx-tx,cy-ty);if(l>.001){tx+=(cx-tx)/l*.35;ty+=(cy-ty)/l*.35;}
        }
        double len=hypot(tx-e->x,ty-e->y);if(len>.05){e->angle=atan2(ty-e->y,tx-e->x);double speed=e->kind?.016:.009;e->walk++;move(g,&e->x,&e->y,&e->z,&e->sector,(tx-e->x)/len*speed,(ty-e->y)/len*speed);e->z=g->sectors[e->sector].floor;}
    }
}
void rn_advance(struct rn_game *g,struct rn_input in,unsigned ms)
{
    if(!g||g->paused||g->state!=RN_PLAY||g->sector<0||g->sector>=g->sector_count)return;
    if(ms>250)ms=250;
    g->accumulator+=(int)ms;while(g->accumulator>=10&&g->state==RN_PLAY){g->accumulator-=10;step(g,in);}
}
const char *rn_room_name(int room){static const char *n[]={"SERVICE LOCK","PUMP HALL","UPPER GALLERY","COMPRESSOR","RELAY CONTROL","RETURN","EXIT LOCK","MAINTENANCE","SUPPLY CACHE"};return room>=0&&room<9?n[room]:"STATION 9";}

const char *rn_notice(const struct rn_game *g)
{
    static const char *n[]={"", "", "Maintenance bay: close-range equipment.",
        "BREACH-2 acquired. Q / WEAPON changes guns.", "Secret supply cache found.",
        "Relay online. Return stairs lead to EXIT.", "Shotgun shells recovered."};
    return g->notice_ms&&g->notice>=0&&g->notice<7?n[g->notice]:"";
}
