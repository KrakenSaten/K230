/*
 * Whether a physical keyboard is attached, as the rest of the shell may know
 * it: present, absent, or unknown. Pure C.
 *
 * This is the state, its one listener and the debounce every provider goes
 * through. The provider is ui/shell/shell_kbd.c: the TCA8418 on the keyboard
 * base answering its probe is "present", the controller not answering is
 * "absent", and no transport at all - the simulator, or a board whose lines
 * cannot be taken - is "unknown". Whatever consumes this, automatic rotation
 * above all, must treat unknown as safely as absent: it resolves to portrait.
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

/* How many consecutive observations of the same new state kbd_presence_observe
 * needs before it publishes. A keyboard base being mated or unmated makes and
 * breaks contact several times on the way (KEYBOARD_BRINGUP §0 is what a
 * partial contact looks like), and each published change costs the display
 * being opened again, so a single reading never moves the state.
 *
 * Three, not two, and the reason is sampling rather than caution: a provider
 * that looks once a second cannot tell a keyboard that is there from one
 * flickering at about that rate, and two readings a second apart can both
 * land in the same flicker. Three consecutive readings mean the state held
 * for two whole seconds, which contact bounce does not. The cost is the
 * second or two before the display turns. */
#define KBD_PRESENCE_STABLE 3

typedef void (*kbd_presence_listener_t)(enum kbd_presence now, void *user);

enum kbd_presence kbd_presence_get(void);
/* Set the state; the listener is called when it changed. For a provider's
 * first, decisive reading - the probe at start-up - and for tests. */
void kbd_presence_publish(enum kbd_presence state);
/* One observation from a provider that is watching. Publishes only after
 * KBD_PRESENCE_STABLE consecutive observations agree on a state other than
 * the published one, so a reading that flickers changes nothing. */
void kbd_presence_observe(enum kbd_presence observed);
/* Forget any half-finished run of observations. The published state stays. */
void kbd_presence_forget_observations(void);
/* One listener (the shell). NULL removes it. */
void kbd_presence_set_listener(kbd_presence_listener_t cb, void *user);

/* "unknown" | "absent" | "present". parse returns -1 for anything else. */
const char *kbd_presence_name(enum kbd_presence state);
int kbd_presence_parse(const char *text, enum kbd_presence *out);

#endif
