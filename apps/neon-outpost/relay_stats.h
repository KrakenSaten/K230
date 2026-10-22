/* Opt-in frame timing for device measurement. Off unless DOORS_RELAY_STATS
 * names a file; then one summary line is appended per five seconds.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef RELAY_STATS_H
#define RELAY_STATS_H
#include <stdint.h>
#include "relay.h"
enum rn_stat { RS_INTERVAL, RS_GAME, RS_SCENE, RS_REFRESH, RS_DRAW, RS_FLUSH, RS_WAIT, RS_COUNT };
struct rn_stats;
struct rn_stats *rn_stats_open(void);
uint64_t rn_stats_now(void);
void rn_stats_add(struct rn_stats *s,enum rn_stat k,uint64_t start);
void rn_stats_frame(struct rn_stats *s,const struct rn_game *g,bool playing,bool auto_pause);
void rn_stats_close(struct rn_stats *s);
#endif
