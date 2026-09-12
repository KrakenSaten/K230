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
#include "kbd_tca8418.h"
#include "pocketlog/pocketlog.h"
#include "pos_input.h"
#include "pos_keymap.h"

#include <stdbool.h>
#include <time.h>

/* The INT line is read on this period and the bus is touched only when it
 * says something is waiting, so an idle keyboard costs microseconds rather
 * than a millisecond of I2C (design doc §14). */
#define KBD_POLL_MS 15

static struct {
    struct kbd_bus bus;
    struct kbd_tca8418 chip;
    struct pos_keymap map;
    lv_timer_t *timer;
    bool present;
    bool failing;      /* so a dead keyboard is logged once, not per tick */
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
        if (pos_input_push_key(key)) {
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

static void on_poll(lv_timer_t *timer)
{
    int handled;

    (void)timer;
    handled = kbd_tca8418_poll(&kbd.chip, now_us(), on_event, NULL);

    if (kbd_tca8418_take_overflow(&kbd.chip)) {
        /* The controller dropped events, which may have included the
         * release of a held modifier. What this layer believes about Shift
         * is therefore no longer trustworthy. */
        pos_keymap_reset(&kbd.map);
        LOG_WARN("keyboard: controller overflow, modifier state dropped");
    }
    if (handled < 0 && !kbd.failing) {
        kbd.failing = true;
        LOG_WARN("keyboard: the controller stopped answering; retrying");
    } else if (handled >= 0 && kbd.failing) {
        kbd.failing = false;
        LOG_INFO("keyboard: the controller is answering again");
    }
}

int shell_kbd_attach(const struct kbd_bus *bus)
{
    if (!bus || kbd.present) {
        return kbd.present ? 1 : 0;
    }
    kbd.bus = *bus;
    kbd.delivered = 0;
    kbd.dropped = 0;
    kbd.reserved = 0;
    kbd.failing = false;
    pos_keymap_reset(&kbd.map);
    if (kbd_tca8418_init(&kbd.chip, &kbd.bus, now_us()) != 1) {
        return 0;
    }
    kbd.timer = lv_timer_create(on_poll, KBD_POLL_MS, NULL);
    if (!kbd.timer) {
        return 0;
    }
    kbd.present = true;
    return 1;
}

int shell_kbd_create(void)
{
    struct kbd_bus bus;
    char why[96];

    if (kbd.present) {
        return 1;
    }
    if (kbd_bus_k230_create(&bus, why, sizeof(why)) != 0) {
        LOG_INFO("keyboard: none (%s)", why);
        return 0;
    }
    if (!shell_kbd_attach(&bus)) {
        LOG_INFO("keyboard: the controller did not answer; touch only");
        kbd_bus_k230_destroy(&bus);
        return 0;
    }
    LOG_INFO("keyboard: TCA8418 ready, polling every %d ms (%s)", KBD_POLL_MS,
             kbd_tca8418_gated(&kbd.chip) ? "INT-gated" : "unconditional");
    return 1;
}

void shell_kbd_destroy(void)
{
    if (kbd.timer) {
        lv_timer_delete(kbd.timer);
        kbd.timer = NULL;
    }
    if (kbd.present) {
        LOG_INFO("keyboard: %u key(s) delivered, %u dropped, %u reserved",
                 kbd.delivered, kbd.dropped, kbd.reserved);
    }
    /* This is what puts the pin mux back, so it runs whether or not a
     * keyboard was ever found. */
    kbd_bus_k230_destroy(&kbd.bus);
    kbd.present = false;
    kbd.failing = false;
}
