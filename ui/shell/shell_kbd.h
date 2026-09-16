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
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_SHELL_KBD_H
#define POCKETOS_SHELL_KBD_H

#include "kbd_bus.h"

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

#endif
