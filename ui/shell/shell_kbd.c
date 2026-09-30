/*
 * The physical keyboard as a key source. See shell_kbd.h.
 *
 * Everything here runs on the LVGL thread, in one lv_timer. pos_input's
 * queue has no lock and the repository has no threads; a driver thread
 * would have to introduce both (design doc §3).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "shell_kbd.h"

#include "kbd_bus_k230.h"
#include "kbd_presence.h"
#include "kbd_tca8418.h"
#include "pocketlog/pocketlog.h"
#include "pos_input.h"
#include "pos_keymap.h"

#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

/* The INT line is read on this period and the bus is touched only when it
 * says something is waiting, so an idle keyboard costs microseconds rather
 * than a millisecond of I2C (design doc §14). */
#define KBD_POLL_MS 15

/* And the period once the gate is gone. Every tick then costs a full I2C
 * drain - about a millisecond of bit-banging on the UI thread - so design
 * §14 spends the latency to buy that back: 5 % of the thread at 20 ms
 * against 7 % at 15 ms. */
#define KBD_POLL_FALLBACK_MS 20

/* The presence watch. It costs one probe of the bus per period while no
 * keyboard is attached - a reset pulse and one register read, about a
 * millisecond - and nothing at all while one is, where it only reads the
 * state the poll timer already keeps. A second is short enough that
 * attaching the base and seeing the screen turn feels like one action, and
 * long enough that the idle cost is noise. */
#define KBD_WATCH_MS 1000

static struct {
    struct kbd_bus bus;
    struct kbd_tca8418 chip;
    struct pos_keymap map;
    lv_timer_t *timer;
    lv_timer_t *watch;
    bool bus_ok;       /* the transport exists: lines taken, mux ours */
    bool probed;
    bool present;
    bool ready;        /* the controller state this layer last saw, so a
                        * failure and a recovery are each logged once */
    bool gated;        /* the mode this layer last reported; one edge only */
    unsigned delivered;
    unsigned dropped;
    unsigned reserved;
} kbd;

static uint64_t now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* One raw FIFO byte. The translation, the modifier state and the decision
 * about what a key even means all belong to pos_keymap, which is tested on
 * its own; this only carries the result into the stream. */
static void on_event(void *user, uint8_t raw)
{
    enum pos_keymap_effect effect;
    pos_key_t key;

    (void)user;
    key = pos_keymap_event(&kbd.map, raw, &effect);
    if (effect == POS_KEYMAP_KEY && key != 0) {
        /* The modifiers held at this press go with the key. Only a raw key
         * target (the Terminal, pos_input.h) ever sees them; every field
         * gets the key exactly as it always did. */
        unsigned mods = (kbd.map.shift ? POS_INPUT_MOD_SHIFT : 0u) |
                        (kbd.map.ctrl ? POS_INPUT_MOD_CTRL : 0u) |
                        (kbd.map.alt ? POS_INPUT_MOD_ALT : 0u);

        if (pos_input_push_key_mods(key, mods)) {
            kbd.delivered++;
        } else {
            kbd.dropped++; /* the queue is full; the oldest keys survive */
        }
    } else if (effect == POS_KEYMAP_RESERVED) {
        /* The function row, Fn, the LILYGO key and the mic have no settled
         * meaning in PocketOS yet, so they are counted and dropped rather
         * than guessed at. */
        kbd.reserved++;
    }
}

/* The controller dropped events, which may have included the release of a
 * held modifier. What this layer believes about Shift is therefore no longer
 * trustworthy. This runs from inside the drain, before any event of the
 * batch that reported the overflow is translated - see kbd_tca8418.h. */
static void on_overflow(void *user)
{
    (void)user;
    pos_keymap_reset(&kbd.map);
}

static void on_poll(lv_timer_t *timer)
{
    bool ready;

    (void)timer;
    /* The count of events drained is not interesting here; what the keys did
     * is on_event's business and whether the controller is usable is asked
     * below, of the driver rather than of this return value. */
    (void)kbd_tca8418_poll(&kbd.chip, now_us(), on_event, on_overflow, NULL);

    if (kbd_tca8418_take_overflow(&kbd.chip)) {
        LOG_WARN("keyboard: controller overflow, modifier state dropped");
    }
    /* The gate can only be lost, never regained, so this is one edge for the
     * life of the process: one line in the log, one change of period. Design
     * §14 requires the fallback to be decided at runtime *and logged* - the
     * mode reported at start-up says nothing about what happened afterwards,
     * and without this line there is no way to tell from a log whether a
     * session kept the gate or quietly spent the whole time polling. */
    if (kbd.gated && !kbd_tca8418_gated(&kbd.chip)) {
        kbd.gated = false;
        if (kbd.timer) {
            lv_timer_set_period(kbd.timer, KBD_POLL_FALLBACK_MS);
        }
        LOG_WARN("keyboard: INT gate dropped, polling unconditionally every "
                 "%d ms", KBD_POLL_FALLBACK_MS);
    }
    /* Whether the controller is usable is the driver's state, not this poll's
     * return value. A retry that is still being throttled returns 0 without
     * touching the bus, and reading that as good news logged "answering
     * again" on the very next tick after a failure, once per backoff, while
     * the keyboard was still dead.
     *
     * The recovery edge is also where what this layer believes about held
     * modifiers has to go. Getting back to READY means the controller was
     * reset, reconfigured and its FIFO flushed: a Shift that was down when
     * the bus went away released itself unseen, and keeping it would apply
     * the orange legend to everything typed afterwards. The overflow path
     * (on_overflow) forgets for the same reason; this is the other way the
     * controller's state can be lost. */
    ready = kbd_tca8418_ready(&kbd.chip);
    if (kbd.ready && !ready) {
        LOG_WARN("keyboard: the controller stopped answering; retrying");
    } else if (!kbd.ready && ready) {
        pos_keymap_reset(&kbd.map);
        LOG_INFO("keyboard: the controller is answering again");
    }
    kbd.ready = ready;
}

/* ---- presence ----------------------------------------------------------- *
 *
 * The keyboard base answers on the bit-banged bus or it does not, and that is
 * the whole signal: the controller acknowledging its address at 0x34 is what
 * the vendor launcher calls the base being detected, and what
 * KEYBOARD_BRINGUP §0 and §5.3 verified in both directions on unit A - the
 * base unmated gives "the controller did not answer", remating restores
 * "TCA8418 ready". No pin on this board says "a keyboard is attached", so
 * nothing here pretends one does.
 *
 * A transport that cannot be taken at all - no /dev/mem, the lines held by
 * something else, the simulator - is neither presence nor absence: it is
 * unknown, which automatic rotation resolves to portrait.
 */

#if defined(POCKETOS_SHELL_TEST_HOOKS) && POCKETOS_SHELL_TEST_HOOKS
#include <stdio.h>
#include <string.h>

/* The simulator's stand-in for the base board: a test says what the keyboard
 * is, in the environment for the state at start-up and in a file it can
 * rewrite while the shell runs, which is how attaching and removing one - and
 * a provider that starts failing - are exercised without hardware. */
static bool test_presence(enum kbd_presence *out)
{
    const char *path = getenv("POCKETOS_TEST_KEYBOARD_FILE");
    const char *fixed = getenv("POCKETOS_TEST_KEYBOARD_PRESENCE");
    char buf[32];
    FILE *f;

    if (path) {
        f = fopen(path, "r");
        if (!f) {
            /* The provider cannot read its source: that is a failure to
             * detect, not a keyboard that went away. */
            *out = KBD_PRESENCE_UNKNOWN;
            return true;
        }
        if (!fgets(buf, sizeof(buf), f)) {
            buf[0] = '\0';
        }
        fclose(f);
        buf[strcspn(buf, "\r\n")] = '\0';
        if (kbd_presence_parse(buf, out) != 0) {
            *out = KBD_PRESENCE_UNKNOWN;
        }
        return true;
    }
    if (fixed && kbd_presence_parse(fixed, out) == 0) {
        return true;
    }
    return false;
}
#else
static bool test_presence(enum kbd_presence *out)
{
    (void)out;
    return false;
}
#endif

/* What this layer can say about the keyboard right now. */
static enum kbd_presence observed(void)
{
    enum kbd_presence test;

    if (test_presence(&test)) {
        return test;
    }
    if (!kbd.bus_ok) {
        return KBD_PRESENCE_UNKNOWN;
    }
    if (kbd_tca8418_ready(&kbd.chip)) {
        return KBD_PRESENCE_PRESENT;
    }
    return kbd_tca8418_bus_error(&kbd.chip) ? KBD_PRESENCE_UNKNOWN : KBD_PRESENCE_ABSENT;
}

static int bring_up(void)
{
    if (kbd_tca8418_init(&kbd.chip, &kbd.bus, now_us()) != 1) {
        return 0;
    }
    /* A board whose INT line was never usable starts in the fallback mode
     * rather than falling into it, so the period matches the mode in both
     * cases and the edge in on_poll is only ever the runtime transition. */
    kbd.gated = kbd_tca8418_gated(&kbd.chip);
    /* init() only returns 1 from READY, so the first poll's edge test starts
     * from the truth rather than from a default. */
    kbd.ready = kbd_tca8418_ready(&kbd.chip);
    pos_keymap_reset(&kbd.map);
    return 1;
}

static int start_polling(void)
{
    if (kbd.timer) {
        return 1;
    }
    kbd.timer = lv_timer_create(on_poll, kbd.gated ? KBD_POLL_MS : KBD_POLL_FALLBACK_MS, NULL);
    if (!kbd.timer) {
        return 0;
    }
    kbd.present = true;
    kbd.delivered = 0;
    kbd.dropped = 0;
    kbd.reserved = 0;
    /* The period follows the mode, so it is reported rather than assumed:
     * a log saying 15 ms while the driver polls at 20 would be worse than
     * saying nothing. */
    LOG_INFO("keyboard: TCA8418 ready, polling every %d ms (%s)",
             kbd.gated ? KBD_POLL_MS : KBD_POLL_FALLBACK_MS,
             kbd.gated ? "INT-gated" : "unconditional");
    return 1;
}

static void on_watch(lv_timer_t *timer)
{
    (void)timer;
    if (kbd.bus_ok && !kbd.present) {
        /* Nothing attached: this probe is the only thing here that touches
         * the bus, and a keyboard that answers it is taken into use at once -
         * typing works from this tick, whatever the debounce below decides
         * about the orientation. */
        if (bring_up()) {
            start_polling();
        }
    } else if (kbd.present && !kbd_tca8418_ready(&kbd.chip)) {
        /* Failing: retry on this cadence rather than at the end of the 30 s
         * backoff, so a keyboard that comes back is seen in a second. */
        kbd_tca8418_retry_now(&kbd.chip);
    }
    kbd_presence_observe(observed());
}

int shell_kbd_probe(void)
{
    enum kbd_presence test;
    char why[96];

    if (kbd.probed) {
        return kbd.bus_ok && kbd_tca8418_ready(&kbd.chip) ? 1 : 0;
    }
    kbd.probed = true;
    if (test_presence(&test)) {
        kbd_presence_publish(test);
        return test == KBD_PRESENCE_PRESENT ? 1 : 0;
    }
    if (kbd_bus_k230_create(&kbd.bus, why, sizeof(why)) != 0) {
        LOG_INFO("keyboard: none (%s)", why);
        kbd_presence_publish(KBD_PRESENCE_UNKNOWN);
        return 0;
    }
    kbd.bus_ok = true;
    if (!bring_up()) {
        LOG_INFO("keyboard: the controller did not answer; touch only");
        /* The bus is kept, unlike before: it is what the watch probes with,
         * and dropping it would mean a keyboard attached later was never
         * noticed. The lines are the shell's either way until it exits. */
        kbd_presence_publish(observed());
        return 0;
    }
    kbd_presence_publish(KBD_PRESENCE_PRESENT);
    return 1;
}

static void start_watching(void)
{
    /* The watch runs whether or not a keyboard was found: it is what notices
     * one being attached, and what notices the attached one going away. */
    if (!kbd.watch) {
        kbd.watch = lv_timer_create(on_watch, KBD_WATCH_MS, NULL);
    }
}

int shell_kbd_attach(const struct kbd_bus *bus)
{
    int answered;

    if (!bus || kbd.present) {
        return kbd.present ? 1 : 0;
    }
    kbd.bus = *bus;
    kbd.bus_ok = true;
    kbd.probed = true;
    answered = bring_up() && start_polling();
    kbd_presence_publish(answered ? KBD_PRESENCE_PRESENT : observed());
    start_watching();
    return answered;
}

int shell_kbd_create(void)
{
    if (!kbd.probed) {
        shell_kbd_probe();
    }
    if (kbd.bus_ok && kbd_tca8418_ready(&kbd.chip)) {
        start_polling();
    }
    start_watching();
    return kbd.present ? 1 : 0;
}

void shell_kbd_destroy(void)
{
    if (kbd.timer) {
        lv_timer_delete(kbd.timer);
        kbd.timer = NULL;
    }
    if (kbd.watch) {
        lv_timer_delete(kbd.watch);
        kbd.watch = NULL;
    }
    if (kbd.present) {
        LOG_INFO("keyboard: %u key(s) delivered, %u dropped, %u reserved",
                 kbd.delivered, kbd.dropped, kbd.reserved);
    }
    /* This is what puts the pin mux back, so it runs whether or not a
     * keyboard was ever found - but only when the lines were actually taken:
     * the bus is now kept across a failed probe, and destroying one that was
     * never created would restore a mux value nobody read. */
    if (kbd.bus_ok) {
        kbd_bus_k230_destroy(&kbd.bus);
    }
    kbd.bus_ok = false;
    kbd.probed = false;
    kbd.present = false;
    kbd.ready = false;
    kbd.gated = false;
}
