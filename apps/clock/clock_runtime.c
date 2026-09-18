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
static int handoff_result = 1;

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
     * not paint a zeroed clock for a tick - and before the handoff, which is
     * checked against it and may have a countdown in it that is already
     * over (clock_store.h). */
    clock_runtime_read();
    /* After the settings file and not before it: the handoff describes the
     * alarms by position, so they have to be there to be described. */
    handoff_result = clock_handoff_load(&engine, &now);
    /* last_ringing stays NONE even when the handoff brought a ring back, so
     * the first step tells the shell about it through the one path that
     * announces any other ring. */
    return load_result;
}

int clock_runtime_handoff_result(void)
{
    return handoff_result;
}

void clock_runtime_deinit(void)
{
    clock_engine_init(&engine);
    memset(&now, 0, sizeof(now));
    ring_change_cb = NULL;
    last_ringing = CLOCK_RING_NONE;
    inited = 0;
    load_result = 0;
    handoff_result = 1;
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

int clock_runtime_handoff_save(void)
{
    /* The reading this engine was last advanced or read at, which is at most
     * a tick old and is the one every instant in the engine is measured
     * against. Taking a fresh one here would be reading a clock, which this
     * file does not do (tests/clock_lint.sh). */
    return clock_handoff_save(&engine, &now);
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
