/*
 * How PocketClock gets your attention, and what this board can actually do
 * about it.
 *
 * THE HARDWARE, as far as it is known. The evidence classes are the ones the
 * project uses everywhere: VERIFIED means measured on the unit, DOCUMENTED
 * means stated in a vendor source, ASSUMED means inferred, UNRESOLVED means
 * nobody knows yet.
 *
 *   - Buzzer or piezo:      NONE.   DOCUMENTED. No buzzer, piezo or beeper
 *     appears anywhere in vendor/T-Display-K230/k230_bsp/docs/HARDWARE_PINMAP.md
 *     or in the board schematic. There is no pin assigned to one.
 *   - Vibration motor:      NONE.   DOCUMENTED. Same two sources, same
 *     answer. The K230 board has no haptic driver and no motor pad.
 *   - Audio out:            EXISTS, NEVER EXERCISED. DOCUMENTED. The K230
 *     has an internal INNO codec wired to the 3.5 mm headphone jack. No part
 *     of PocketOS has ever opened it, no ALSA device has been confirmed on
 *     the running image, and whether anything is plugged into the jack is
 *     not something software can know. Treating it as an alarm bell would be
 *     inventing an alert the owner may never hear.
 *   - MAX98357A amplifier:  NOT ON THE MAIN BOARD. DOCUMENTED. It is on the
 *     nRF52840 base board. Corrected 2026-09-13: a second unit has that
 *     board and a built-in speaker, and unit A is the same hardware
 *     (docs/hardware/AUDIO_HARDWARE_MAP_2026-09-13.md). This backend still
 *     claims no sound.
 *   - Whether the audio path works at all: UNRESOLVED. It has not been
 *     tried, and this milestone did not try it.
 *
 * So the alert this milestone ships is the screen, and the app says so in
 * plain words rather than letting the owner assume an alarm will make a
 * noise. The abstraction below is the seam: when the audio path is brought
 * up - or a base board with a buzzer turns out to exist - it becomes a
 * backend registered here, and nothing in the engine, the store or the views
 * changes.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETCLOCK_ALERT_H
#define POCKETCLOCK_ALERT_H

#include <stdbool.h>

enum clock_alert_kind {
    CLOCK_ALERT_ALARM = 0,
    CLOCK_ALERT_TIMER
};

/* The output channels an alert can use. Only the ones in
 * clock_alert_channels() exist on this hardware. */
enum clock_alert_channel {
    CLOCK_ALERT_VISUAL = 1 << 0,
    CLOCK_ALERT_SOUND = 1 << 1,
    CLOCK_ALERT_HAPTIC = 1 << 2
};

struct clock_alert_backend {
    const char *name;
    unsigned channels;                         /* enum clock_alert_channel */
    /* One line for the owner, saying what an alert will and will not do.
     * Shown in the app: an alarm that cannot make a sound has to say so. */
    const char *why;
    void (*begin)(enum clock_alert_kind kind); /* may be NULL */
    void (*end)(void);                         /* may be NULL */
};

/* Install a backend. NULL restores the built-in screen-only one. The pointer
 * is kept, so it must outlive the app - a static is the intended shape. */
void clock_alert_set_backend(const struct clock_alert_backend *backend);

unsigned clock_alert_channels(void);
const char *clock_alert_why(void);
const char *clock_alert_backend_name(void);

/* Begin and end are idempotent: beginning an alert that is already running
 * does nothing, and so does ending one that is not. The engine can be
 * stepped as often as the UI likes, so this has to hold. */
void clock_alert_begin(enum clock_alert_kind kind);
void clock_alert_end(void);
bool clock_alert_active(void);
enum clock_alert_kind clock_alert_kind_active(void);

#endif
