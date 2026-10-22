/* Original procedural neon panels, geometric robots and supplies; no imported assets.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "neon.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

static uint16_t rgb(int r, int g, int b) { return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3)); }
static void pixel(uint16_t *p, int x, int y, uint16_t c)
{
    if (x >= 0 && y >= 0 && x < NEON_VIEW_W && y < NEON_VIEW_H) p[y*NEON_VIEW_W+x] = c;
}
static void box(uint16_t *p, int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y+h; ++j) for (int i = x; i < x+w; ++i) pixel(p,i,j,c);
}
struct sprite { double x,y,depth; int kind, hp; };
bool neon_render(const struct neon_game *g, uint16_t *p, size_t n)
{
    if (!g || !p || n != NEON_VIEW_W*NEON_VIEW_H) return false;
    memset(p,0,n*sizeof(*p));
    if (!isfinite(g->x) || !isfinite(g->y) || !isfinite(g->angle) || g->x < 1 || g->y < 1 ||
        g->x >= NEON_W-1 || g->y >= NEON_H-1) return false;
    double z[NEON_VIEW_W];
    double ca = cos(g->angle), sa = sin(g->angle);
    for (int y = 0; y < NEON_VIEW_H; ++y) {
        int d = abs(y-NEON_VIEW_H/2);
        uint16_t c = y < NEON_VIEW_H/2 ? rgb(10+d/8,12,30+d/5) : rgb(14+d/5,16+d/6,26+d/4);
        for (int x = 0; x < NEON_VIEW_W; ++x) p[y*NEON_VIEW_W+x] = c;
    }
    for (int x = 0; x < NEON_VIEW_W; ++x) {
        double camera = (2*(x+.5)/NEON_VIEW_W-1)*.66;
        struct neon_ray r = neon_cast(g,g->x,g->y,ca-sa*camera,sa+ca*camera);
        z[x] = r.distance;
        double dist = fmax(.08,r.distance);
        int height = (int)fmin(2000,NEON_VIEW_H/dist), top = (NEON_VIEW_H-height)/2;
        int u = (int)(r.fraction*32);
        for (int y = top < 0 ? 0 : top; y < top+height && y < NEON_VIEW_H; ++y) {
            int v = (y-top)*32/(height ? height : 1);
            bool seam = u == 0 || u == 31 || v == 0 || v == 31;
            bool strip = v == 7 || v == 24 || (r.cell == 'D' && (u == 9 || u == 22));
            int shade = (int)(170/(1+dist*.15)) - r.side*20;
            if (r.cell == 'D') p[y*NEON_VIEW_W+x] = strip ? rgb(240,70,170) : rgb(shade/2,shade/5,shade/2);
            else if (strip) p[y*NEON_VIEW_W+x] = rgb(20,shade+50,shade+55);
            else p[y*NEON_VIEW_W+x] = seam ? rgb(8,12,25) : rgb(shade/4,shade/3,shade/2+15+(u^v)%8);
        }
    }
    struct sprite s[NEON_ENEMIES+NEON_PICKUPS+1];
    int count = 0;
    for (int i = 0; i < NEON_ENEMIES; ++i) if (g->enemy[i].hp > 0)
        s[count++] = (struct sprite){g->enemy[i].x,g->enemy[i].y,0,g->enemy[i].kind,g->enemy[i].hp};
    for (int i = 0; i < NEON_PICKUPS; ++i) if (!g->pickup[i].taken)
        s[count++] = (struct sprite){g->pickup[i].x,g->pickup[i].y,0,2+g->pickup[i].kind,0};
    for (int i = 0; i < NEON_CELLS; ++i) if (g->map[i] == 'E') {
        if (count < NEON_ENEMIES+NEON_PICKUPS+1) s[count++] = (struct sprite){i%NEON_W+.5,i/NEON_W+.5,0,4,0};
        break;
    }
    for (int i = 0; i < count; ++i) s[i].depth = (s[i].x-g->x)*ca+(s[i].y-g->y)*sa;
    for (int i = 1; i < count; ++i) {
        struct sprite v = s[i]; int j = i;
        while (j && s[j-1].depth < v.depth) { s[j] = s[j-1]; --j; }
        s[j] = v;
    }
    for (int i = 0; i < count; ++i) {
        struct sprite v = s[i];
        if (!isfinite(v.depth) || v.depth < .15 || !isfinite(v.x) || !isfinite(v.y)) continue;
        double across = -(v.x-g->x)*sa+(v.y-g->y)*ca;
        double screen = NEON_VIEW_W/2.0+across/v.depth*(NEON_VIEW_W/1.32);
        if (!isfinite(screen) || fabs(screen) > 4096) continue;
        int cx = (int)screen;
        int size = (int)fmin(300,NEON_VIEW_H/v.depth*(v.kind < 2 ? .65 : .4));
        if (size < 2) continue;
        int cy = NEON_VIEW_H/2+(v.kind >= 2 && v.kind != 4 ? size/2 : 0);
        for (int y = 0; y < NEON_VIEW_H; ++y) for (int x = cx-size/2; x <= cx+size/2; ++x) {
            if (x < 0 || x >= NEON_VIEW_W || v.depth >= z[x]) continue;
            double u = (x-cx)*2.0/size, t = (y-cy)*2.0/size;
            bool draw = false; uint16_t c = 0;
            if (v.kind == 0) { /* diamond crawler, split eye */
                draw = fabs(u)+fabs(t) < 1;
                c = fabs(t) < .13 && fabs(u) > .12 ? rgb(255,240,200) : rgb(240,75,60);
            } else if (v.kind == 1) { /* triangular sentry and square core */
                draw = t > -.9 && t < .9 && fabs(u) < (t+1)*.48;
                c = fabs(u) < .23 && fabs(t-.2) < .22 ? rgb(250,245,220) : rgb(180,90,250);
            } else if (v.kind == 2) {
                draw = fabs(u) < .8 && fabs(t) < .65;
                c = (fabs(u-.35) < .12 || fabs(u+.35) < .12) ? rgb(255,245,170) : rgb(210,150,30);
            } else if (v.kind == 3) {
                draw = (fabs(u) < .25 && fabs(t) < .85) || (fabs(t) < .25 && fabs(u) < .85);
                c = rgb(100,250,185);
            } else {
                draw = fabs(t) < .95 && ((fabs(u) > .65 && fabs(u) < .85) || t < -.7 || t > .7 ||
                    (fabs(t) < .15 && u > -.4 && u < .45) || (u > .2 && fabs(t) < .4-fabs(u-.2)));
                c = rgb(100,255,170);
            }
            if (draw) pixel(p,x,y,c);
        }
    }
    /* Fixed weapon silhouette and crosshair; no shake, flicker or full-screen flashes. */
    box(p,117,133,22,27,rgb(35,50,68)); box(p,121,124,14,26,rgb(70,100,120));
    box(p,125,121,6,24,rgb(20,230,220));
    if (g->flash > 0) { box(p,120,116,16,4,rgb(255,225,130)); box(p,126,110,4,14,rgb(255,245,170)); }
    box(p,123,80,4,1,rgb(210,255,250)); box(p,130,80,4,1,rgb(210,255,250));
    box(p,128,75,1,4,rgb(210,255,250)); box(p,128,82,1,4,rgb(210,255,250));
    if (g->hurt > 0) { box(p,0,0,3,NEON_VIEW_H,rgb(220,90,70)); box(p,253,0,3,NEON_VIEW_H,rgb(220,90,70)); }
    return true;
}
