/*
 * The power key's press timing. See power_key.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "power_key.h"

#include <string.h>

void power_key_init(struct power_key *k)
{
    memset(k, 0, sizeof(*k));
    k->state = POWER_KEY_UP;
}

static bool reached(const struct power_key *k, uint32_t t_ms)
{
    /* Wrap-safe; a time before the press (a clamp the caller missed) reads
     * as no time at all rather than as a huge one. */
    int32_t held = (int32_t)(t_ms - k->down_ms);

    return held >= (int32_t)POWER_KEY_LONG_MS;
}

enum power_key_press power_key_input(struct power_key *k, int value, uint32_t t_ms)
{
    if (value == 1) {
        if (k->state != POWER_KEY_UP) {
            k->ignored++; /* a second press with no release between */
            return POWER_KEY_NONE;
        }
        k->state = POWER_KEY_DOWN;
        k->down_ms = t_ms;
        return POWER_KEY_NONE;
    }
    if (value != 0) {
        k->ignored++; /* auto-repeat, or a value evdev does not send */
        return POWER_KEY_NONE;
    }
    switch (k->state) {
    case POWER_KEY_DOWN:
        k->state = POWER_KEY_UP;
        if (reached(k, t_ms)) {
            k->longs++;
            return POWER_KEY_LONG;
        }
        k->shorts++;
        return POWER_KEY_SHORT;
    case POWER_KEY_LONG_FIRED:
    case POWER_KEY_SWALLOW:
        k->state = POWER_KEY_UP;
        return POWER_KEY_NONE;
    case POWER_KEY_UP:
    default:
        k->ignored++; /* a release with no press */
        return POWER_KEY_NONE;
    }
}

enum power_key_press power_key_poll(struct power_key *k, uint32_t now_ms)
{
    if (k->state == POWER_KEY_DOWN && reached(k, now_ms)) {
        k->state = POWER_KEY_LONG_FIRED;
        k->longs++;
        return POWER_KEY_LONG;
    }
    return POWER_KEY_NONE;
}

void power_key_lost(struct power_key *k)
{
    if (k->state == POWER_KEY_DOWN) {
        k->lost++;
    }
    k->state = POWER_KEY_UP;
}

void power_key_resync(struct power_key *k, bool down_now)
{
    if (down_now) {
        if (k->state == POWER_KEY_UP) {
            k->state = POWER_KEY_SWALLOW;
        }
        return;
    }
    power_key_lost(k);
}

bool power_key_is_down(const struct power_key *k)
{
    return k->state != POWER_KEY_UP;
}

const char *power_key_state_name(enum power_key_state s)
{
    switch (s) {
    case POWER_KEY_DOWN:
        return "down";
    case POWER_KEY_LONG_FIRED:
        return "long";
    case POWER_KEY_SWALLOW:
        return "swallow";
    case POWER_KEY_UP:
    default:
        return "up";
    }
}
