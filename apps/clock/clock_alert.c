/*
 * The alert seam. See clock_alert.h for the hardware and its evidence.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "clock_alert.h"

#include <stddef.h>

/* The screen, and nothing else. begin and end are NULL because the view
 * already draws the ringing state; this backend exists to state the
 * capability honestly, not to do work twice. */
static const struct clock_alert_backend screen_only = {
    .name = "screen",
    .channels = CLOCK_ALERT_VISUAL,
    .why = "This board has no buzzer and no vibration motor, so an alert "
           "appears on screen and makes no sound.",
    .begin = NULL,
    .end = NULL,
};

static const struct clock_alert_backend *current = &screen_only;
static bool active;
static enum clock_alert_kind active_kind;

void clock_alert_set_backend(const struct clock_alert_backend *backend)
{
    /* Changing the backend under a running alert would leave the old one
     * holding whatever it had started, so end it first. */
    if (active) {
        clock_alert_end();
    }
    current = backend ? backend : &screen_only;
}

unsigned clock_alert_channels(void)
{
    return current->channels;
}

const char *clock_alert_why(void)
{
    return current->why ? current->why : "";
}

const char *clock_alert_backend_name(void)
{
    return current->name ? current->name : "";
}

void clock_alert_begin(enum clock_alert_kind kind)
{
    if (active) {
        return;
    }
    active = true;
    active_kind = kind;
    if (current->begin) {
        current->begin(kind);
    }
}

void clock_alert_end(void)
{
    if (!active) {
        return;
    }
    active = false;
    if (current->end) {
        current->end();
    }
}

bool clock_alert_active(void)
{
    return active;
}

enum clock_alert_kind clock_alert_kind_active(void)
{
    return active_kind;
}
