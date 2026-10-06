/*
 * The power key's press timing: what one press of the board's power key
 * means - a short press, or a hold of POWER_KEY_LONG_MS - from its press and
 * release alone.
 *
 * The key (SW2 "0", the PMU's INT0, kernel driver k230-pmu-pwrkey,
 * KEY_POWER) reports exactly one press and one release per push and never
 * repeats (docs/hardware/K230_BUTTONS.md §4, VERIFIED on unit B), so the hold
 * is timed here, from the two edges and the clock:
 *
 *   - a release before POWER_KEY_LONG_MS is a short press;
 *   - reaching POWER_KEY_LONG_MS while still held is a long press, reported
 *     once, at that moment - not at the release, which then means nothing;
 *   - a release that arrives at or past the threshold without the long press
 *     having been seen (the shell's loop was busy) is the long press.
 *
 * Nothing here can stop what the kernel does with the same key: its own
 * 5 s hold powers the device off (orderly_poweroff) whatever userspace does.
 * The threshold is well short of that and there is no "cancel".
 *
 * Input that cannot be trusted ends a press with no action: the device gone,
 * events dropped by the kernel, a release with no press, a second press
 * without a release, an auto-repeat. After a loss the real key state is
 * unknown until it is read back (power_key_resync); a key found already down
 * then is swallowed until it comes up, since when it went down is not known.
 *
 * Times are milliseconds on a monotonic clock, compared wrap-safe. Pure C,
 * no LVGL, no devices (tests/power_key_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef DOORS_POWER_KEY_H
#define DOORS_POWER_KEY_H

#include <stdbool.h>
#include <stdint.h>

/* A hold this long opens the power menu. About one second, well under the
 * kernel's 5 s power-off. */
#define POWER_KEY_LONG_MS 1000u

enum power_key_press {
    POWER_KEY_NONE = 0,
    POWER_KEY_SHORT,
    POWER_KEY_LONG
};

enum power_key_state {
    POWER_KEY_UP = 0,
    POWER_KEY_DOWN,       /* held, under the threshold */
    POWER_KEY_LONG_FIRED, /* held, the long press already reported */
    POWER_KEY_SWALLOW     /* held since before we could see it: ignored until up */
};

struct power_key {
    enum power_key_state state;
    uint32_t down_ms;
    /* For shell.info and the tests. */
    unsigned shorts;
    unsigned longs;
    unsigned ignored; /* repeats, duplicate presses, releases with no press */
    unsigned lost;    /* presses ended by lost input */
};

void power_key_init(struct power_key *k);

/* One EV_KEY event for the key: value 1 press, 0 release, 2 auto-repeat
 * (ignored), at t_ms. */
enum power_key_press power_key_input(struct power_key *k, int value, uint32_t t_ms);

/* The clock moved on: the long press, once, when a hold reaches the
 * threshold. */
enum power_key_press power_key_poll(struct power_key *k, uint32_t now_ms);

/* The input went away (device gone, read error): whatever was held ends
 * with no action. */
void power_key_lost(struct power_key *k);

/* The key's real state read back after a loss or dropped events. A press
 * still held is kept; a release missed ends it with no action; a key found
 * down that was not known to be is swallowed until it is released. */
void power_key_resync(struct power_key *k, bool down_now);

bool power_key_is_down(const struct power_key *k);
const char *power_key_state_name(enum power_key_state s);

#endif
