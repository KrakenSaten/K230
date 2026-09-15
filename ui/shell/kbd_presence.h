/*
 * Whether a physical keyboard is attached, as the rest of the shell may know
 * it: present, absent, or unknown. Pure C.
 *
 * This is only the state and its one listener. Nothing in the shell detects
 * a keyboard to publish here yet: the physical keyboard's transport and
 * detection are not complete (docs/hardware/KEYBOARD_DRIVER_DESIGN_2026-09-12.md),
 * and a controller answering on a bus is not taken as proof that a keyboard
 * is in use. So the state stays unknown in production, and whatever consumes
 * it - automatic rotation - must treat unknown safely. A future, verified
 * keyboard driver publishes here and nothing that consumes it changes.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_KBD_PRESENCE_H
#define POCKETOS_KBD_PRESENCE_H

enum kbd_presence {
    KBD_PRESENCE_UNKNOWN = 0,
    KBD_PRESENCE_ABSENT,
    KBD_PRESENCE_PRESENT,
};

typedef void (*kbd_presence_listener_t)(enum kbd_presence now, void *user);

enum kbd_presence kbd_presence_get(void);
/* Set the state; the listener is called when it changed. */
void kbd_presence_publish(enum kbd_presence state);
/* One listener (the shell). NULL removes it. */
void kbd_presence_set_listener(kbd_presence_listener_t cb, void *user);

/* "unknown" | "absent" | "present". parse returns -1 for anything else. */
const char *kbd_presence_name(enum kbd_presence state);
int kbd_presence_parse(const char *text, enum kbd_presence *out);

#endif
