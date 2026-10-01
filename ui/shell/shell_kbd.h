/*
 * The physical keyboard, as a source of the one logical key stream.
 *
 * It owns the poll timer, the controller and the bus, and it hands every
 * key to pos_input_push_key() exactly as the touch keyboard does: an app
 * sees a focused field and the characters that arrive in it, and cannot
 * tell which source they came from (DS v0.1 §17.4).
 *
 * The keyboard is not adopted as an LVGL input device. That path loses
 * LV_KEY_NEXT and LV_KEY_PREV in the source's private group
 * (docs/KNOWN_ISSUES.md), so a physical keyboard must push.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_SHELL_KBD_H
#define POCKETOS_SHELL_KBD_H

#include "hw_actions.h"
#include "kbd_bus.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Take the bus and ask the controller whether it is there, before LVGL and
 * the display exist: the orientation policy needs the answer to decide which
 * way to open the display (shell_display.h), and a keyboard found after the
 * display was opened would cost a restart. Publishes the answer through
 * kbd_presence.h - present, absent, or unknown when this board has no
 * transport at all. Returns 1 when the controller answered. */
int shell_kbd_probe(void);

/* Start polling the keyboard found by shell_kbd_probe(), and start watching
 * for one being attached or removed while the shell runs. Returns 1 when a
 * keyboard is being polled and 0 when none is attached. Absence is normal and
 * is not an error: the shell runs on touch alone, and keeps watching. Call
 * after pos_input_init(), which is the stream this pushes into. */
int shell_kbd_create(void);

/* Stop polling, release the lines and put the pin mux back. Must be called
 * on the shell's exit path: it is what restores the mux. */
void shell_kbd_destroy(void);

/* Drive the driver from a bus the caller supplies, instead of the board's.
 * This is how tests/shell_kbd_test.c exercises the real glue - the poll
 * timer, the presence watch, the key path and the overflow recovery - with no
 * hardware. Returns 1 when the controller on that bus answered. */
int shell_kbd_attach(const struct kbd_bus *bus);

/* ---- the keys that type nothing (hw_actions.h) ------------------------- *
 *
 * The function row, the microphone key and the LILYGO key never reach a
 * field. Their press becomes a semantic action, handed to fn after the
 * controller's FIFO has been drained and the bus released - never from
 * inside the drain, where opening an app would hold the bus for as long as
 * the app takes to build. Only presses act: a release carries nothing, and
 * the controller does not auto-repeat, so a held key is one action. (A
 * later long press would be timed between the two, which this leaves room
 * for: the release still passes through on_event.)
 *
 * With Fn held, a function key goes to a raw key target instead (the
 * Terminal, pos_input.h), as the F-key itself; with no raw target focused it
 * is the shortcut as usual. */
void shell_kbd_on_action(void (*fn)(enum hw_action action, void *user), void *user);

/* The microphone and camera LEDs (kbd_leds.h KBD_LED_MIC | KBD_LED_CAMERA).
 * The Caps LED follows the keyboard's own Caps state and is not set here.
 * Written at once when it changes; nothing when no base is attached. */
void shell_kbd_set_indicators(unsigned mask);

/* Feed raw controller bytes (bit 7 press, bits 0-6 the matrix code) through
 * the same path a key takes: the key map, the modifiers, the stream and the
 * actions. For the bench (shell.key) and the tests - the one way to exercise
 * a key nobody can press remotely. Returns how many bytes were taken. */
size_t shell_kbd_inject(const uint8_t *raw, size_t n);

/* Give the keyboard light's pin its PWM function (kbd_light.h). -1 when
 * there is no bus to do it through. */
int shell_kbd_light_mux(void);

struct shell_kbd_status {
    bool present;
    bool caps;
    bool leds;           /* the LED expander answered */
    unsigned leds_shown; /* KBD_LED_* as shown */
    unsigned delivered;
    unsigned dropped;
    unsigned reserved;   /* presses of keys that type nothing */
    unsigned actions;    /* of those, handed on as an action */
    unsigned led_failures;
    int port_read;       /* 0 when the two registers below were read back */
    uint8_t output0;     /* XL9555 OUTPUT0 as read now (LED pins active low) */
    uint8_t config0;     /* XL9555 CONFIG0 as read now (0 bits are outputs) */
};
void shell_kbd_status(struct shell_kbd_status *out);

#endif
