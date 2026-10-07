/*
 * PocketOS logical key input (DS v0.1 §17.2 and §17.4).
 *
 * One stream, one sink. The touch keyboard, the simulator's SDL keyboard and
 * the physical keyboard that comes later are *sources*: each hands its keys
 * to pos_input_push_key(), and one LVGL keypad device delivers them to the
 * focused object of one focus group. There is no second path, and an app
 * cannot tell which source a key came from - that is the point of §17.4, and
 * the reason a physical keyboard will be a driver rather than a redesign.
 *
 * A key is LVGL's: a printable character is its Unicode code point, anything
 * else is an LV_KEY_* constant. No parallel vocabulary (§17.4).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POS_INPUT_H
#define POS_INPUT_H

#include "lvgl.h"

#include <stdbool.h>

/* One logical key: a Unicode code point, or an LV_KEY_* constant. */
typedef uint32_t pos_key_t;

/* How many keys may wait to be delivered. A finger cannot outrun this; it
 * exists so a source that misbehaves cannot grow the queue without bound. */
#define POS_INPUT_QUEUE 32

/* Build the stream: the queue, the keypad device that delivers it, and the
 * focus group every focusable object joins. Call once, after lv_init().
 * Calling it twice is a no-op, so a test may call it per case. */
void pos_input_init(void);
/* Release the device, both groups and the sink. Safe when not initialised. */
void pos_input_deinit(void);

/* A source offers one key. False when the queue is full: the new key is
 * dropped rather than an older one, so what the user typed first survives.
 * False also when the stream is not initialised. */
bool pos_input_push_key(pos_key_t key);

/* Adopt an existing LVGL keypad device as a source. Its keys stop being
 * delivered by LVGL and enter this stream instead, which is how the SDL
 * keyboard reaches a text field and how a physical keyboard will.
 *
 * LVGL's SDL driver finds its own device by comparing read callbacks, so the
 * callback is left alone; the device is given a private group holding one
 * sink object instead, and the sink forwards what it receives. False when
 * src is NULL, is not a keypad device, or the stream is not initialised. */
bool pos_input_add_source(lv_indev_t *src);

/* The focus group. Exactly one object in it is focused at a time (§17.2). */
lv_group_t *pos_input_group(void);
/* The one device that delivers the stream. */
lv_indev_t *pos_input_indev(void);

/* Join the focus group. Next/Prev visit objects in the order they joined,
 * and LVGL focuses the first object added to an empty group - which is what
 * DS §17.6 wants of the Notes editor, so it is left alone. */
void pos_input_add_obj(lv_obj_t *obj);

/* Move focus to obj. NULL is ignored: an LVGL group has no unfocused state,
 * and nothing needs one. Focus clears itself when the focused object is
 * deleted or leaves the group, both of which LVGL already handles. */
void pos_input_focus(lv_obj_t *obj);

/* The focused object, or NULL when the group is empty or has just lost it. */
lv_obj_t *pos_input_focused(void);

/* ---- one-deep group redirection (DS §18.8) ----------------------------- *
 *
 * A system alert must take every key while it is up. The app underneath keeps
 * its objects, its text and its focus, and receives nothing.
 *
 * The stream is not filtered to achieve that - filtering would mean deciding,
 * key by key, what an app may still see, and getting one wrong is a leak.
 * Instead the delivering device is pointed at another group for as long as
 * the alert owns the panel, and pointed back afterwards. A group that is not
 * delivered to cannot receive anything, whatever the key.
 *
 * One deep, deliberately: §18.6 allows exactly one active alert, so a stack
 * would be dead code that invites stacking. A second push is refused and a
 * pop with nothing pushed does nothing, so a repeated tick cannot unbalance
 * it.
 *
 * The shell owns the alert and owns these. Apps must not call them. */
bool pos_input_push_group(lv_group_t *group);
void pos_input_pop_group(void);
/* True while a redirection is in effect. For the shell, and for tests that
 * check the depth never exceeds one. */
bool pos_input_group_redirected(void);

/* Keys still waiting. For tests, and for a diagnostic that wants to know
 * whether the queue is draining. */
unsigned pos_input_queued(void);

/* ---- modifiers and the raw key target ---------------------------------- *
 *
 * A source that knows which modifiers were held when a key was pressed (the
 * physical keyboard) says so with the key. Nothing an ordinary field
 * receives changes: the modifiers are dropped on delivery, so Ctrl+C still
 * types a 'c' into a note, exactly as before.
 *
 * One object may instead ask for keys raw: a terminal, which has to send
 * Tab, Esc and Ctrl+C to the program it hosts rather than let them move
 * focus, cancel or type a letter. While that object is the focused object of
 * the group the device delivers to, every key reaches it as LV_EVENT_KEY
 * carrying a flagged value - NEXT, PREV, ENTER and ESC included, which LVGL
 * would otherwise act on itself - and pos_input_raw_decode() gives back the
 * key and the modifiers. The moment it is not focused (an alert took the
 * keys, DS §18.8, or it left the group) delivery is ordinary again, so the
 * raw target can never take a key meant for anything else. */
#define POS_INPUT_MOD_SHIFT 0x1u
#define POS_INPUT_MOD_CTRL 0x2u
#define POS_INPUT_MOD_ALT 0x4u

bool pos_input_push_key_mods(pos_key_t key, unsigned mods);

/* Name the raw target (NULL: none). Cleared by itself when the object is
 * deleted. */
void pos_input_set_raw_target(lv_obj_t *obj);
lv_obj_t *pos_input_raw_target(void);

/* In the raw target's LV_EVENT_KEY handler: the key and the modifiers, from
 * the value lv_event_get_param() points at. False when the value is not a
 * raw one (an ordinary delivery). */
bool pos_input_raw_decode(uint32_t delivered, pos_key_t *key, unsigned *mods);

/* True while the raw target is the focused object of the group the device
 * delivers to - the moment a raw key would reach it. */
bool pos_input_raw_focused(void);

/* ---- raw-only keys: the function row ----------------------------------- *
 *
 * A function key has no LVGL constant and no character, and a field must
 * never receive one. Doors keeps the bare function row for its own shortcuts
 * (ui/shell/hw_actions.h); with Fn held, the keyboard pushes F1..F12 as the
 * codes below instead, and only a raw target (the Terminal) may receive
 * them. Anything else they would reach is skipped at delivery, so a focus
 * change between push and delivery cannot type one into a note. They sit in
 * Unicode's Supplementary Private Use Area-A, inside the raw key mask. */
#define POS_KEY_F_BASE 0xF0000u
#define POS_KEY_F(n) (POS_KEY_F_BASE + (pos_key_t)(n))

/* 1..12 for POS_KEY_F(1..12), else 0. */
static inline int pos_key_function(pos_key_t key)
{
    return key > POS_KEY_F_BASE && key <= POS_KEY_F_BASE + 12u ? (int)(key - POS_KEY_F_BASE) : 0;
}

#endif
