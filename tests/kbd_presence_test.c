/*
 * Keyboard presence: what a provider's observations do to the published
 * state, and what the listener therefore sees.
 *
 * The provider itself is shell_kbd.c, which turns "the TCA8418 answered" into
 * one of these observations; everything that decides whether a keyboard being
 * mated, unmated, or flickering between the two changes the orientation is
 * here, where it can be run a thousand times without a screwdriver.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "kbd_presence.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* What the shell's listener would see. */
static enum kbd_presence heard;
static unsigned heard_count;

static void on_change(enum kbd_presence now, void *user)
{
    (void)user;
    heard = now;
    heard_count++;
}

/* Start every case from a known published state, without counting the
 * listener calls that getting there costs. */
static void start_from(enum kbd_presence state)
{
    kbd_presence_set_listener(NULL, NULL);
    kbd_presence_publish(state);
    heard_count = 0;
    heard = state;
    kbd_presence_set_listener(on_change, NULL);
}

static void observe_n(enum kbd_presence state, unsigned n)
{
    while (n--) {
        kbd_presence_observe(state);
    }
}

int main(void)
{
    enum kbd_presence parsed;

    /* ---- the state itself ------------------------------------------------ */
    check("nothing published yet is unknown", kbd_presence_get() == KBD_PRESENCE_UNKNOWN);
    start_from(KBD_PRESENCE_UNKNOWN);
    kbd_presence_publish(KBD_PRESENCE_PRESENT);
    check("a boot probe that answers publishes present at once",
          kbd_presence_get() == KBD_PRESENCE_PRESENT && heard == KBD_PRESENCE_PRESENT && heard_count == 1);
    kbd_presence_publish(KBD_PRESENCE_PRESENT);
    check("publishing the same state again tells nobody", heard_count == 1);
    kbd_presence_publish((enum kbd_presence)42);
    check("a state that is not one of the three is unknown", kbd_presence_get() == KBD_PRESENCE_UNKNOWN);

    /* ---- boot ------------------------------------------------------------ */
    start_from(KBD_PRESENCE_UNKNOWN);
    kbd_presence_publish(KBD_PRESENCE_ABSENT);
    check("boot with no keyboard: absent", kbd_presence_get() == KBD_PRESENCE_ABSENT && heard_count == 1);
    start_from(KBD_PRESENCE_UNKNOWN);
    kbd_presence_publish(KBD_PRESENCE_UNKNOWN);
    check("boot on a board with no transport: unknown, and no change to announce",
          kbd_presence_get() == KBD_PRESENCE_UNKNOWN && heard_count == 0);

    /* ---- attached while running ------------------------------------------ */
    start_from(KBD_PRESENCE_ABSENT);
    observe_n(KBD_PRESENCE_PRESENT, KBD_PRESENCE_STABLE - 1);
    check("readings short of the debounce do not move the state yet",
          kbd_presence_get() == KBD_PRESENCE_ABSENT && heard_count == 0);
    kbd_presence_observe(KBD_PRESENCE_PRESENT);
    check("the one that completes it publishes present",
          kbd_presence_get() == KBD_PRESENCE_PRESENT && heard == KBD_PRESENCE_PRESENT && heard_count == 1);
    observe_n(KBD_PRESENCE_PRESENT, 10);
    check("and a keyboard that stays attached is not announced again", heard_count == 1);

    /* ---- removed while running ------------------------------------------- */
    start_from(KBD_PRESENCE_PRESENT);
    kbd_presence_observe(KBD_PRESENCE_ABSENT);
    check("one failed reading is not a removal", kbd_presence_get() == KBD_PRESENCE_PRESENT && heard_count == 0);
    observe_n(KBD_PRESENCE_ABSENT, KBD_PRESENCE_STABLE - 1);
    check("the full run publishes absent", kbd_presence_get() == KBD_PRESENCE_ABSENT && heard_count == 1);
    observe_n(KBD_PRESENCE_ABSENT, 5);
    check("and staying away is not announced again", heard_count == 1);

    /* ---- noise ------------------------------------------------------------ */
    start_from(KBD_PRESENCE_ABSENT);
    kbd_presence_observe(KBD_PRESENCE_PRESENT);
    kbd_presence_observe(KBD_PRESENCE_ABSENT);
    kbd_presence_observe(KBD_PRESENCE_PRESENT);
    kbd_presence_observe(KBD_PRESENCE_ABSENT);
    kbd_presence_observe(KBD_PRESENCE_PRESENT);
    kbd_presence_observe(KBD_PRESENCE_ABSENT);
    check("contacts making and breaking never publish anything",
          kbd_presence_get() == KBD_PRESENCE_ABSENT && heard_count == 0);
    observe_n(KBD_PRESENCE_PRESENT, KBD_PRESENCE_STABLE);
    check("once they settle, the new state is published once",
          kbd_presence_get() == KBD_PRESENCE_PRESENT && heard_count == 1);

    /* A run against the published state that is interrupted by the published
     * state itself starts over: absent, present, absent would otherwise
     * accumulate
     * across a minute of good readings and remove a keyboard that is there. */
    start_from(KBD_PRESENCE_PRESENT);
    kbd_presence_observe(KBD_PRESENCE_ABSENT);
    kbd_presence_observe(KBD_PRESENCE_PRESENT);
    kbd_presence_observe(KBD_PRESENCE_ABSENT);
    check("a good reading between two bad ones keeps the keyboard",
          kbd_presence_get() == KBD_PRESENCE_PRESENT && heard_count == 0);

    /* A run that changes its mind starts over rather than counting towards
     * whichever state happens to arrive next. */
    start_from(KBD_PRESENCE_PRESENT);
    observe_n(KBD_PRESENCE_ABSENT, KBD_PRESENCE_STABLE - 1);
    kbd_presence_observe(KBD_PRESENCE_UNKNOWN);
    check("a run of absent ending in unknown publishes neither",
          kbd_presence_get() == KBD_PRESENCE_PRESENT && heard_count == 0);
    observe_n(KBD_PRESENCE_UNKNOWN, KBD_PRESENCE_STABLE - 1);
    check("the unknowns then count on their own",
          kbd_presence_get() == KBD_PRESENCE_UNKNOWN && heard_count == 1);

    /* ---- the provider failing --------------------------------------------- */
    start_from(KBD_PRESENCE_PRESENT);
    observe_n(KBD_PRESENCE_UNKNOWN, KBD_PRESENCE_STABLE);
    check("detection that stops working publishes unknown, not absent",
          kbd_presence_get() == KBD_PRESENCE_UNKNOWN && heard == KBD_PRESENCE_UNKNOWN && heard_count == 1);
    observe_n(KBD_PRESENCE_UNKNOWN, 20);
    check("and keeps quiet while it stays broken", heard_count == 1);
    observe_n(KBD_PRESENCE_PRESENT, KBD_PRESENCE_STABLE);
    check("recovery publishes present again", kbd_presence_get() == KBD_PRESENCE_PRESENT && heard_count == 2);

    /* ---- forgetting ------------------------------------------------------- */
    start_from(KBD_PRESENCE_PRESENT);
    kbd_presence_observe(KBD_PRESENCE_ABSENT);
    kbd_presence_forget_observations();
    kbd_presence_observe(KBD_PRESENCE_ABSENT);
    check("a run forgotten half way does not publish on its next reading",
          kbd_presence_get() == KBD_PRESENCE_PRESENT && heard_count == 0);
    observe_n(KBD_PRESENCE_ABSENT, KBD_PRESENCE_STABLE - 1);
    check("it takes a whole run again", kbd_presence_get() == KBD_PRESENCE_ABSENT && heard_count == 1);
    /* Publishing is what a boot probe does, and it must not leave a half-run
     * behind that a later observation completes. */
    start_from(KBD_PRESENCE_ABSENT);
    observe_n(KBD_PRESENCE_PRESENT, KBD_PRESENCE_STABLE - 1);
    kbd_presence_publish(KBD_PRESENCE_UNKNOWN);
    observe_n(KBD_PRESENCE_PRESENT, KBD_PRESENCE_STABLE - 1);
    check("a publish in the middle of a run starts the count again",
          kbd_presence_get() == KBD_PRESENCE_UNKNOWN && heard_count == 1);

    /* ---- the listener ------------------------------------------------------ */
    start_from(KBD_PRESENCE_ABSENT);
    kbd_presence_set_listener(NULL, NULL);
    observe_n(KBD_PRESENCE_PRESENT, KBD_PRESENCE_STABLE);
    check("no listener is not a crash and the state still changes",
          kbd_presence_get() == KBD_PRESENCE_PRESENT && heard_count == 0);

    /* ---- names and parsing -------------------------------------------------- */
    check("names", strcmp(kbd_presence_name(KBD_PRESENCE_UNKNOWN), "unknown") == 0 &&
                       strcmp(kbd_presence_name(KBD_PRESENCE_ABSENT), "absent") == 0 &&
                       strcmp(kbd_presence_name(KBD_PRESENCE_PRESENT), "present") == 0);
    check("parse", kbd_presence_parse("present", &parsed) == 0 && parsed == KBD_PRESENCE_PRESENT &&
                       kbd_presence_parse("absent", &parsed) == 0 && parsed == KBD_PRESENCE_ABSENT &&
                       kbd_presence_parse("unknown", &parsed) == 0 && parsed == KBD_PRESENCE_UNKNOWN);
    check("anything else is refused", kbd_presence_parse("maybe", &parsed) != 0 &&
                                          kbd_presence_parse("", &parsed) != 0 &&
                                          kbd_presence_parse(NULL, &parsed) != 0);
    check("the debounce is three readings, so two whole seconds of one state",
          KBD_PRESENCE_STABLE == 3);

    printf("kbd_presence_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
