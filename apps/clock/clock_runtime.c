/*
 * The one clock runtime. See clock_runtime.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "clock_runtime.h"

#include "clock_store.h"
#include "clock_time.h"

#include <stddef.h>
#include <string.h>

static struct clock_engine engine;
static struct clock_now now;
static void (*ring_change_cb)(void);
static uint8_t last_ringing;
static int inited;
static int load_result;

/* Everything that can change what is ringing goes through here, so the shell
 * hears about it once and only when it actually changed. */
static void settle(void)
{
    if (engine.ringing == last_ringing) {
        return;
    }
    last_ringing = engine.ringing;
    if (ring_change_cb) {
        ring_change_cb();
    }
}

int clock_runtime_init(void (*on_ring_change)(void))
{
    if (inited) {
        ring_change_cb = on_ring_change;
        return load_result;
    }
    clock_engine_init(&engine);
    load_result = clock_store_load(&engine);
    memset(&now, 0, sizeof(now));
    ring_change_cb = on_ring_change;
    last_ringing = CLOCK_RING_NONE;
    inited = 1;
    /* One reading before anything else runs, so the first view to ask does
     * not paint a zeroed clock for a tick. */
    clock_runtime_read();
    return load_result;
}

void clock_runtime_deinit(void)
{
    clock_engine_init(&engine);
    memset(&now, 0, sizeof(now));
    ring_change_cb = NULL;
    last_ringing = CLOCK_RING_NONE;
    inited = 0;
    load_result = 0;
}

void clock_runtime_read(void)
{
    clock_now_read(&now);
}

void clock_runtime_step(void)
{
    clock_now_read(&now);
    clock_engine_step(&engine, &now);
    settle();
}

void clock_runtime_step_at(const struct clock_now *at)
{
    if (!at) {
        return;
    }
    now = *at;
    clock_engine_step(&engine, &now);
    settle();
}

struct clock_engine *clock_runtime_engine(void)
{
    return &engine;
}

const struct clock_now *clock_runtime_now(void)
{
    return &now;
}

int clock_runtime_save(void)
{
    return clock_store_save(&engine);
}

void clock_runtime_stop_ringing(void)
{
    if (engine.ringing == CLOCK_RING_ALARM) {
        clock_alarm_acknowledge(&engine, &now);
        /* A one-shot alarm has just switched itself off, and that is a
         * setting: it has to survive the next reboot. */
        clock_runtime_save();
    } else if (engine.ringing == CLOCK_RING_TIMER) {
        clock_timer_acknowledge(&engine);
    }
    settle();
}

void clock_runtime_snooze(void)
{
    clock_alarm_snooze(&engine, &now);
    settle();
}
