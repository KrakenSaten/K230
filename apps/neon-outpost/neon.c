/* Original grid simulation and ray/grid intersections from elementary geometry.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "neon.h"
#include <math.h>
#include <string.h>

static const char level[] =
    "################"
    "#......#.......#"
    "#......D.......#"
    "#......#.......#"
    "#......#.......#"
    "###D########D###"
    "#......#.......#"
    "#......D.......#"
    "#......#.......#"
    "#......#......E#"
    "#......#.......#"
    "################";
_Static_assert(sizeof(level) == NEON_CELLS + 1, "map dimensions");
static bool inside(int x, int y) { return x >= 0 && y >= 0 && x < NEON_W && y < NEON_H; }
static bool position(double x, double y)
{
    return isfinite(x) && isfinite(y) && x >= 1 && y >= 1 && x < NEON_W-1 && y < NEON_H-1;
}
bool neon_validate(const char *m, size_t n, double x, double y)
{
    if (!m || n != NEON_CELLS || !position(x,y)) return false;
    unsigned exits = 0;
    for (int j = 0; j < NEON_H; ++j) for (int i = 0; i < NEON_W; ++i) {
        char c = m[j*NEON_W+i];
        if (c != '#' && c != '.' && c != 'D' && c != 'E') return false;
        if ((!i || !j || i == NEON_W-1 || j == NEON_H-1) && c != '#') return false;
        exits += c == 'E';
        if (c == 'D') {
            bool ns = m[(j-1)*NEON_W+i] == '#' && m[(j+1)*NEON_W+i] == '#';
            bool ew = m[j*NEON_W+i-1] == '#' && m[j*NEON_W+i+1] == '#';
            bool clear_ew = m[j*NEON_W+i-1] == '.' && m[j*NEON_W+i+1] == '.';
            bool clear_ns = m[(j-1)*NEON_W+i] == '.' && m[(j+1)*NEON_W+i] == '.';
            if (!((ns && clear_ew) || (ew && clear_ns))) return false;
        }
    }
    if (exits != 1 || m[(int)y*NEON_W+(int)x] != '.') return false;
    for (int j = -1; j <= 1; j += 2) for (int i = -1; i <= 1; i += 2)
        if (m[(int)(y+j*.21)*NEON_W+(int)(x+i*.21)] != '.') return false;
    uint8_t seen[NEON_CELLS] = {0};
    int q[NEON_CELLS], head = 0, tail = 0;
    int start = (int)y*NEON_W+(int)x;
    q[tail++] = start; seen[start] = 1;
    const int d[] = {-1,1,-NEON_W,NEON_W};
    while (head < tail) {
        int p = q[head++];
        for (int k = 0; k < 4; ++k) {
            int v = p + d[k];
            if (v >= 0 && v < NEON_CELLS && m[v] != '#' && !seen[v]) {
                seen[v] = 1; q[tail++] = v;
            }
        }
    }
    for (int i = 0; i < NEON_CELLS; ++i) if (m[i] != '#' && !seen[i]) return false;
    return true;
}
bool neon_solid(const struct neon_game *g, int x, int y)
{
    if (!g || !inside(x,y)) return true;
    char c = g->map[y*NEON_W+x];
    return c != '.' && c != 'E';
}
struct neon_ray neon_cast(const struct neon_game *g, double x, double y, double dx, double dy)
{
    struct neon_ray r = {0,0,'#',0};
    if (!g || !position(x,y) || !isfinite(dx) || !isfinite(dy) ||
        fabs(dx) > 2 || fabs(dy) > 2 || (fabs(dx)+fabs(dy) < 1e-9)) return r;
    int cx = (int)x, cy = (int)y, sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (neon_solid(g,cx,cy)) return r;
    double stepx = fabs(dx) < 1e-9 ? 1e30 : fabs(1/dx);
    double stepy = fabs(dy) < 1e-9 ? 1e30 : fabs(1/dy);
    double nx = (dx < 0 ? x-cx : cx+1-x)*stepx;
    double ny = (dy < 0 ? y-cy : cy+1-y)*stepy;
    for (int i = 0; i < NEON_CELLS; ++i) {
        if (nx < ny) { r.distance = nx; nx += stepx; cx += sx; r.side = 0; }
        else { r.distance = ny; ny += stepy; cy += sy; r.side = 1; }
        if (neon_solid(g,cx,cy)) {
            r.cell = inside(cx,cy) ? g->map[cy*NEON_W+cx] : '#';
            double hit = r.side ? x+dx*r.distance : y+dy*r.distance;
            r.fraction = hit-floor(hit);
            return r;
        }
    }
    return r;
}
bool neon_visible(const struct neon_game *g, double x, double y, double tx, double ty)
{
    if (!position(x,y) || !position(tx,ty)) return false;
    double d = hypot(tx-x,ty-y);
    if (d < 1e-6) return true;
    return neon_cast(g,x,y,(tx-x)/d,(ty-y)/d).distance > d-0.001;
}
static bool fits(const struct neon_game *g, double x, double y)
{
    const double r = .21;
    return position(x,y) && !neon_solid(g,(int)(x-r),(int)(y-r)) &&
        !neon_solid(g,(int)(x+r),(int)(y-r)) && !neon_solid(g,(int)(x-r),(int)(y+r)) &&
        !neon_solid(g,(int)(x+r),(int)(y+r));
}
static void move(const struct neon_game *g, double *x, double *y, double dx, double dy)
{
    if (fits(g,*x+dx,*y)) *x += dx;
    if (fits(g,*x,*y+dy)) *y += dy;
}
static void navigate(struct neon_game *g)
{
    int q[NEON_CELLS], head = 0, tail = 0;
    for (int i = 0; i < NEON_CELLS; ++i) g->distance[i] = -1;
    int p = (int)g->y*NEON_W+(int)g->x;
    q[tail++] = p; g->distance[p] = 0;
    const int d[] = {-1,1,-NEON_W,NEON_W};
    while (head < tail) {
        p = q[head++];
        for (int k = 0; k < 4; ++k) {
            int v = p+d[k];
            if (v >= 0 && v < NEON_CELLS && g->distance[v] < 0 && !neon_solid(g,v%NEON_W,v/NEON_W)) {
                g->distance[v] = g->distance[p]+1; q[tail++] = v;
            }
        }
    }
}
void neon_restart(struct neon_game *g)
{
    if (!g) return;
    memset(g,0,sizeof(*g)); memcpy(g->map,level,NEON_CELLS);
    g->x = 2.5; g->y = 2.5; g->health = 100; g->ammo = 36;
    const struct neon_actor enemies[NEON_ENEMIES] = {
        {5.5,3.5,2,0,1000}, {10.5,2.5,3,1,1500}, {13.5,4.5,2,0,1500},
        {3.5,8.5,2,0,1000}, {10.5,8.5,3,1,1500}, {13.5,9.5,2,0,1000}
    };
    const struct neon_pickup pickups[NEON_PICKUPS] = {
        {5.5,2.5,0,false}, {9.5,3.5,1,false}, {3.5,6.5,0,false},
        {5.5,9.5,1,false}, {13.5,6.5,0,false}, {9.5,9.5,1,false}
    };
    memcpy(g->enemy,enemies,sizeof(enemies)); memcpy(g->pickup,pickups,sizeof(pickups));
    navigate(g);
}
void neon_pause(struct neon_game *g, bool paused)
{
    if (!g) return;
    g->paused = paused; g->accumulator = 0; g->use_down = false;
}
static void shoot(struct neon_game *g)
{
    if (g->cooldown || !g->ammo) return;
    --g->ammo; g->cooldown = 280; g->flash = 90;
    double dx = cos(g->angle), dy = sin(g->angle);
    double best = neon_cast(g,g->x,g->y,dx,dy).distance;
    int target = -1;
    for (int i = 0; i < NEON_ENEMIES; ++i) {
        struct neon_actor *e = &g->enemy[i];
        if (e->hp <= 0) continue;
        double rx = e->x-g->x, ry = e->y-g->y;
        double along = rx*dx+ry*dy, across = rx*dy-ry*dx;
        double r = .30;
        if (along <= 0 || fabs(across) >= r) continue;
        double hit = along-sqrt(r*r-across*across);
        if (hit < best && neon_visible(g,g->x,g->y,e->x,e->y)) { best = hit; target = i; }
    }
    if (target >= 0 && --g->enemy[target].hp == 0) ++g->kills;
}
static void use(struct neon_game *g)
{
    double dx = cos(g->angle), dy = sin(g->angle);
    /* Sample only the first solid cell, so a farther door cannot be opened through a wall. */
    struct neon_ray r = neon_cast(g,g->x,g->y,dx,dy);
    if (r.cell == 'D' && r.distance < 1.25) {
        int x = (int)(g->x+dx*(r.distance+.001)), y = (int)(g->y+dy*(r.distance+.001));
        if (inside(x,y)) g->map[y*NEON_W+x] = '.';
        g->nav_clock = 0;
    }
    if (g->map[(int)g->y*NEON_W+(int)g->x] == 'E') g->state = NEON_WON;
}
static int axis(int v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }
static void step(struct neon_game *g, struct neon_input in)
{
    ++g->steps;
    if (g->cooldown > 0) g->cooldown -= 10;
    if (g->flash > 0) g->flash -= 10;
    if (g->hurt > 0) g->hurt -= 10;
    g->angle = remainder(g->angle+axis(in.turn)*.022,2*NEON_PI);
    double f = axis(in.forward), s = axis(in.strafe), scale = f && s ? .70710678 : 1;
    move(g,&g->x,&g->y,(cos(g->angle)*f-sin(g->angle)*s)*.024*scale,
                       (sin(g->angle)*f+cos(g->angle)*s)*.024*scale);
    if (in.fire) shoot(g);
    if (in.use && !g->use_down) use(g);
    g->use_down = in.use;
    if (g->state != NEON_PLAY) return;
    for (int i = 0; i < NEON_PICKUPS; ++i) {
        struct neon_pickup *p = &g->pickup[i];
        if (p->taken || hypot(g->x-p->x,g->y-p->y) > .45) continue;
        if (!p->kind && g->ammo < 99) { g->ammo += 12; if (g->ammo > 99) g->ammo = 99; p->taken = true; }
        if (p->kind && g->health < 100) { g->health += 30; if (g->health > 100) g->health = 100; p->taken = true; }
    }
    if (g->nav_clock <= 0) { navigate(g); g->nav_clock = 200; }
    g->nav_clock -= 10;
    for (int i = 0; i < NEON_ENEMIES; ++i) {
        struct neon_actor *e = &g->enemy[i];
        if (e->hp <= 0) continue;
        if (e->cooldown > 0) e->cooldown -= 10;
        double d = hypot(g->x-e->x,g->y-e->y);
        bool los = neon_visible(g,e->x,e->y,g->x,g->y);
        /* Triangle sentries shoot at range; diamond crawlers must close in. */
        if (los && d < (e->kind ? 5.0 : .85)) {
            if (!e->cooldown) {
                g->health -= e->kind ? 9 : 12; g->hurt = 160;
                e->cooldown = e->kind ? 1600 : 950;
                if (g->health <= 0) { g->health = 0; g->state = NEON_DEAD; return; }
            }
            continue;
        }
        double tx = g->x, ty = g->y;
        if (!los) {
            int cell = (int)e->y*NEON_W+(int)e->x, best = cell;
            const int offsets[] = {-1,1,-NEON_W,NEON_W};
            for (int k = 0; k < 4; ++k) {
                int v = cell+offsets[k];
                if (v >= 0 && v < NEON_CELLS && g->distance[v] >= 0 &&
                    (g->distance[best] < 0 || g->distance[v] < g->distance[best])) best = v;
            }
            if (g->distance[best] < 0) continue;
            /* Collision slides along the inside corner toward the next cell centre. */
            tx = best%NEON_W+.5; ty = best/NEON_W+.5;
        }
        double length = hypot(tx-e->x,ty-e->y);
        if (length > .05) {
            double speed = e->kind ? .008 : .012;
            move(g,&e->x,&e->y,(tx-e->x)/length*speed,(ty-e->y)/length*speed);
        }
    }
}
void neon_advance(struct neon_game *g, struct neon_input in, unsigned ms)
{
    if (!g || g->paused || g->state != NEON_PLAY || !position(g->x,g->y) || !isfinite(g->angle)) return;
    if (ms > 250) ms = 250;
    g->accumulator += (int)ms;
    while (g->accumulator >= 10 && g->state == NEON_PLAY) {
        g->accumulator -= 10; step(g,in);
    }
}
