/*
 * PocketTimber replay: the action vocabulary, the log a session writes,
 * and playing a log back into a fresh run.
 *
 * A run is reproduced by (seed, the ordered list of player actions at
 * their ticks). This file makes that list explicit. Every way the player
 * can touch the engine is one action: select, deselect, test, one tick of
 * pull travel, place. A live session drives the engine through
 * timber_log_act(), which records the action and applies it in one step,
 * so the log is exactly what happened, refusals included: a refused action
 * changes nothing on replay either, and a pull the stiction absorbed is not
 * a refusal, it moved the grip.
 *
 * timber_log_play() builds a run from the log's seed, starts it, and for
 * each action ticks the run up to the action's tick and applies it. The
 * result is bit-identical to the session that wrote the log, collapse
 * included, which is what tests/timber_replay_test.c asserts and what a
 * bug report can carry.
 *
 * Pure C, no LVGL and no I/O: the log is a struct in memory. Writing it
 * anywhere is the app's business, and not part of v0.1.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETTIMBER_REPLAY_H
#define POCKETTIMBER_REPLAY_H

#include "timber_rules.h"

enum timber_action_type {
    TIMBER_ACTION_NONE = 0,
    TIMBER_ACTION_SELECT,       /* arg: block id */
    TIMBER_ACTION_DESELECT,
    TIMBER_ACTION_TEST,
    TIMBER_ACTION_PULL,         /* arg: travel, Q8.8 widths, signed */
    TIMBER_ACTION_PLACE,        /* arg: slot */
    TIMBER_ACTION_COUNT
};

struct timber_action {
    uint32_t tick;
    uint8_t type;               /* enum timber_action_type */
    int16_t arg;
};

/* A five-minute run drags for perhaps two of them at one pull action a
 * tick, so a few thousand actions; the log holds more, and counts what it
 * had to drop past that. */
#define TIMBER_LOG_MAX 6144

struct timber_log {
    uint32_t seed;
    uint32_t count;
    uint32_t dropped;
    struct timber_action actions[TIMBER_LOG_MAX];
};

/* Display name, or "?" for an out-of-range type. */
const char *timber_action_name(enum timber_action_type type);

/* Apply one action to a run. Returns what the rules returned, or -1 for
 * an unknown action. */
int timber_run_apply(struct timber_run *run, const struct timber_action *a);
/* Tick a run up to a tick count, stopping early if it ends. */
void timber_run_advance(struct timber_run *run, uint32_t tick);

void timber_log_init(struct timber_log *log, uint32_t seed);
/* Append an action. Returns 0, or -1 when the log is full, in which case
 * the action is counted as dropped. */
int timber_log_record(struct timber_log *log, uint32_t tick, enum timber_action_type type, int arg);
/* Record an action at the run's current tick and apply it: how a live
 * session drives the engine. Returns what the rules returned. */
int timber_log_act(struct timber_log *log, struct timber_run *run, enum timber_action_type type,
                   int arg);
/* Build a run from the log's seed, start it, and replay every action at
 * its tick. Returns the number of actions applied, or -1 for a NULL
 * argument. Stops early if the run ends. */
int timber_log_play(const struct timber_log *log, struct timber_run *run);

#endif
