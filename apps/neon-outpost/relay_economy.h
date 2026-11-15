/* Relay Nine's economy-mode controller: when to shed optional scene detail
 * and when to try full detail again. Pure and header-only, so the engine test
 * drives it without LVGL.
 *
 * Economy is entered on four scene renders over 24 ms in a row, or eight
 * frame intervals over 45 ms in a row (the second catches LVGL's draw and
 * flush cost, which the scene timer does not see). It is left after a quiet
 * run of frames: intervals of at most 40 ms and renders under 14 ms. The
 * interval half matters: renders in economy are cheap by construction, so a
 * render-only exit returns to full detail every quiet run even when full
 * detail is what made the intervals long. When economy comes back soon after
 * such a probe (RN_ECONOMY_RELAPSE frames), the quiet run needed next time
 * doubles, up to RN_ECONOMY_QUIET_MAX; a long stretch of full detail
 * (RN_ECONOMY_SETTLED frames) puts it back to RN_ECONOMY_QUIET.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef RELAY_ECONOMY_H
#define RELAY_ECONOMY_H
#include <stdbool.h>
#define RN_ECONOMY_SLOW_RENDER_US 24000u
#define RN_ECONOMY_SLOW_RENDERS 4u
#define RN_ECONOMY_LAG_MS 45u
#define RN_ECONOMY_LAGS 8u
#define RN_ECONOMY_QUIET_RENDER_US 14000u
#define RN_ECONOMY_QUIET_MS 40u
#define RN_ECONOMY_QUIET 180u
#define RN_ECONOMY_QUIET_MAX 2880u
#define RN_ECONOMY_RELAPSE 300u
#define RN_ECONOMY_SETTLED 900u
struct rn_economy {
    bool on;
    unsigned slow,lag,quiet,quiet_need,since_change,entries,exits;
};
static inline void rn_economy_init(struct rn_economy *e)
{
    *e=(struct rn_economy){.quiet_need=RN_ECONOMY_QUIET,.since_change=RN_ECONOMY_SETTLED};
}
/* Not playing (menu, pause, title): runs restart, the state and the backoff stay. */
static inline void rn_economy_idle(struct rn_economy *e)
{
    e->slow=e->lag=e->quiet=0;
}
/* One played frame: its interval and the scene render it drew (0: none).
 * Returns whether the next frame renders in economy. */
static inline bool rn_economy_frame(struct rn_economy *e,unsigned interval_ms,unsigned render_us)
{
    if(e->since_change<0x7fffffffu)e->since_change++;
    if(!e->on){
        e->slow=render_us>RN_ECONOMY_SLOW_RENDER_US?e->slow+1:0;
        e->lag=interval_ms>RN_ECONOMY_LAG_MS?e->lag+1:0;
        if(e->since_change>=RN_ECONOMY_SETTLED)e->quiet_need=RN_ECONOMY_QUIET;
        if(e->slow>=RN_ECONOMY_SLOW_RENDERS||e->lag>=RN_ECONOMY_LAGS){
            if(e->exits&&e->since_change<RN_ECONOMY_RELAPSE)
                e->quiet_need=e->quiet_need*2>RN_ECONOMY_QUIET_MAX?RN_ECONOMY_QUIET_MAX:e->quiet_need*2;
            e->on=true;e->entries++;e->since_change=0;e->slow=e->lag=e->quiet=0;
        }
    }else{
        bool quiet=interval_ms<=RN_ECONOMY_QUIET_MS&&render_us<RN_ECONOMY_QUIET_RENDER_US;
        e->quiet=quiet?e->quiet+1:0;
        if(e->quiet>=e->quiet_need){e->on=false;e->exits++;e->since_change=0;e->slow=e->lag=e->quiet=0;}
    }
    return e->on;
}
#endif
