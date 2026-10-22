/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef NEON_PILOT_H
#define NEON_PILOT_H
/* A bounded test driver chooses inputs, never edits the run's game state. */
static struct neon_input pilot(const struct neon_game *g, double goalx, double goaly)
{
    struct neon_input in = {0};
    double tx = goalx, ty = goaly, nearest = 8;
    bool fight = false;
    for (int i = 0; i < NEON_ENEMIES; ++i) {
        const struct neon_actor *e = &g->enemy[i];
        double d = hypot(e->x-g->x,e->y-g->y);
        if (e->hp > 0 && d < nearest && neon_visible(g,g->x,g->y,e->x,e->y)) {
            tx = e->x; ty = e->y; nearest = d; fight = true;
        }
    }
    if (!fight) {
        int q[NEON_CELLS], prev[NEON_CELLS], head = 0, tail = 0;
        for (int i = 0; i < NEON_CELLS; ++i) prev[i] = -1;
        int start = (int)g->y*NEON_W+(int)g->x, goal = (int)goaly*NEON_W+(int)goalx;
        q[tail++] = start; prev[start] = start;
        const int ds[] = {-1,1,-NEON_W,NEON_W};
        while (head < tail && prev[goal] < 0) {
            int p = q[head++];
            for (int k = 0; k < 4; ++k) {
                int n = p+ds[k];
                if (n >= 0 && n < NEON_CELLS && prev[n] < 0 && g->map[n] != '#') {
                    prev[n] = p; q[tail++] = n;
                }
            }
        }
        if (prev[goal] >= 0 && goal != start) {
            int n = goal;
            while (prev[n] != start) n = prev[n];
            tx = n%NEON_W+.5; ty = n/NEON_W+.5;
        }
    }
    double diff = remainder(atan2(ty-g->y,tx-g->x)-g->angle,2*NEON_PI);
    if (fabs(diff) > .025) in.turn = diff > 0 ? 1 : -1;
    if (fight) in.fire = fabs(diff) < .07;
    else { in.forward = fabs(diff) < .15; in.use = g->steps%20 == 0; }
    return in;
}
#endif
