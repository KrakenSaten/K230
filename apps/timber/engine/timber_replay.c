/*
 * PocketTimber replay. See timber_replay.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "timber_replay.h"

#include <string.h>

static const char *const action_names[TIMBER_ACTION_COUNT] = {
    "NONE", "SELECT", "DESELECT", "TEST", "PULL", "PLACE"
};

const char *timber_action_name(enum timber_action_type type)
{
    return (unsigned)type < TIMBER_ACTION_COUNT ? action_names[type] : "?";
}

int timber_run_apply(struct timber_run *run, const struct timber_action *a)
{
    if (!run || !a) {
        return -1;
    }
    switch (a->type) {
    case TIMBER_ACTION_SELECT:
        return timber_run_select(run, a->arg);
    case TIMBER_ACTION_DESELECT:
        return timber_run_deselect(run);
    case TIMBER_ACTION_TEST:
        return timber_run_test(run);
    case TIMBER_ACTION_PULL:
        return timber_run_pull(run, a->arg);
    case TIMBER_ACTION_PLACE:
        return timber_run_place(run, a->arg);
    default:
        return -1;
    }
}

void timber_run_advance(struct timber_run *run, uint32_t tick)
{
    if (!run) {
        return;
    }
    while (run->ticks < tick && !timber_run_is_over(run)) {
        timber_run_tick(run);
    }
}

void timber_log_init(struct timber_log *log, uint32_t seed)
{
    if (!log) {
        return;
    }
    memset(log, 0, sizeof(*log));
    log->seed = seed;
}

int timber_log_record(struct timber_log *log, uint32_t tick, enum timber_action_type type, int arg)
{
    struct timber_action *a;

    if (!log) {
        return -1;
    }
    if (log->count >= TIMBER_LOG_MAX) {
        log->dropped++;
        return -1;
    }
    if (arg > 32767) {
        arg = 32767;
    }
    if (arg < -32768) {
        arg = -32768;
    }
    a = &log->actions[log->count++];
    a->tick = tick;
    a->type = (uint8_t)type;
    a->arg = (int16_t)arg;
    return 0;
}

int timber_log_act(struct timber_log *log, struct timber_run *run, enum timber_action_type type,
                   int arg)
{
    struct timber_action a;

    if (!run) {
        return -1;
    }
    a.tick = run->ticks;
    a.type = (uint8_t)type;
    a.arg = (int16_t)(arg > 32767 ? 32767 : arg < -32768 ? -32768 : arg);
    timber_log_record(log, a.tick, type, a.arg);
    return timber_run_apply(run, &a);
}

int timber_log_play(const struct timber_log *log, struct timber_run *run)
{
    uint32_t i;
    int applied = 0;

    if (!log || !run) {
        return -1;
    }
    timber_run_new(run, log->seed);
    timber_run_start(run);
    for (i = 0; i < log->count; i++) {
        const struct timber_action *a = &log->actions[i];

        timber_run_advance(run, a->tick);
        if (timber_run_is_over(run)) {
            break;
        }
        timber_run_apply(run, a);
        applied++;
    }
    return applied;
}
