/*
 * Keyboard presence state. See kbd_presence.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kbd_presence.h"

#include <stddef.h>
#include <string.h>

static enum kbd_presence presence = KBD_PRESENCE_UNKNOWN;
static kbd_presence_listener_t listener;
static void *listener_user;
/* The run of observations that has not been published: what they agree on,
 * and how many of them there have been. */
static enum kbd_presence candidate = KBD_PRESENCE_UNKNOWN;
static unsigned agreed;

static enum kbd_presence sane(enum kbd_presence state)
{
    if (state != KBD_PRESENCE_UNKNOWN && state != KBD_PRESENCE_ABSENT && state != KBD_PRESENCE_PRESENT) {
        return KBD_PRESENCE_UNKNOWN;
    }
    return state;
}

enum kbd_presence kbd_presence_get(void)
{
    return presence;
}

void kbd_presence_publish(enum kbd_presence state)
{
    state = sane(state);
    if (state == presence) {
        kbd_presence_forget_observations();
        return;
    }
    presence = state;
    kbd_presence_forget_observations();
    if (listener) {
        listener(presence, listener_user);
    }
}

void kbd_presence_observe(enum kbd_presence observed)
{
    observed = sane(observed);
    if (observed == presence) {
        /* Agreement with what is published ends any run against it: a single
         * good reading between two bad ones is enough to keep the state. */
        kbd_presence_forget_observations();
        return;
    }
    if (observed != candidate) {
        candidate = observed;
        agreed = 0;
    }
    if (++agreed < KBD_PRESENCE_STABLE) {
        return;
    }
    kbd_presence_publish(observed);
}

void kbd_presence_forget_observations(void)
{
    candidate = presence;
    agreed = 0;
}

void kbd_presence_set_listener(kbd_presence_listener_t cb, void *user)
{
    listener = cb;
    listener_user = user;
}

const char *kbd_presence_name(enum kbd_presence state)
{
    switch (state) {
    case KBD_PRESENCE_ABSENT: return "absent";
    case KBD_PRESENCE_PRESENT: return "present";
    case KBD_PRESENCE_UNKNOWN:
    default: return "unknown";
    }
}

int kbd_presence_parse(const char *text, enum kbd_presence *out)
{
    if (!text) {
        return -1;
    }
    if (strcmp(text, "unknown") == 0) {
        *out = KBD_PRESENCE_UNKNOWN;
    } else if (strcmp(text, "absent") == 0) {
        *out = KBD_PRESENCE_ABSENT;
    } else if (strcmp(text, "present") == 0) {
        *out = KBD_PRESENCE_PRESENT;
    } else {
        return -1;
    }
    return 0;
}
