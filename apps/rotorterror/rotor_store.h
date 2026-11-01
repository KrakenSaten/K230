/* Separate high score for each difficulty; upgrades belong to the current run.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef ROTOR_STORE_H
#define ROTOR_STORE_H
#include <stdint.h>
int rt_scores_load(uint32_t best[3]);
int rt_scores_save(const uint32_t best[3]);
#endif
