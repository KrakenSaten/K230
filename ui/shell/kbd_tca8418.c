/*
 * TCA8418 presence, initialisation and FIFO drain. See kbd_tca8418.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "kbd_tca8418.h"

#include <stddef.h>

/* Registers, from the vendor driver and the part's map (DOCUMENTED). */
#define REG_CFG 0x01
#define REG_INT_STAT 0x02
#define REG_KEY_LCK_EC 0x03
#define REG_KEY_EVENT_A 0x04
#define REG_KP_GPIO1 0x1D
#define REG_KP_GPIO2 0x1E
#define REG_KP_GPIO3 0x1F
#define REG_DEBOUNCE_DIS1 0x29
#define REG_DEBOUNCE_DIS2 0x2A
#define REG_DEBOUNCE_DIS3 0x2B

/* KE_IEN | OVR_FLOW_IEN | OVR_FLOW_M: interrupt on a key event and on an
 * overflow, which is what makes the INT line usable as a gate. */
#define CFG_VALUE 0x29

/* KP_GPIO1..3 claim 7 rows and 10 columns for the matrix. */
#define KP_GPIO1_VALUE 0x7F
#define KP_GPIO2_VALUE 0xFF
#define KP_GPIO3_VALUE 0x03

/* Debounce is left enabled: 0 means "do not disable". */
#define DEBOUNCE_ENABLED 0x00

#define INT_STAT_K_INT 0x01
#define INT_STAT_OVERFLOW 0x08
/* K_INT | K_LCK_INT | OVR_FLOW_INT, the three this driver ever sets. */
#define INT_STAT_CLEAR 0x0D
/* K_INT | OVR_FLOW_INT, what the startup flush clears. */
#define INT_STAT_CLEAR_FLUSH 0x09

#define EVENT_COUNT_MASK 0x0F
#define EVENT_PRESSED 0x80
#define EVENT_CODE 0x7F

#define FLUSH_MAX 16

/* After a failure: 2 s (the vendor's own throttle), then 5 s, then 30 s, so
 * a board with no keyboard does not run an I2C transaction every 2 s for
 * the life of the shell. */
#define RETRY_1_US 2000000ULL
#define RETRY_2_US 5000000ULL
#define RETRY_MAX_US 30000000ULL

/* Even when the gate says idle, drain unconditionally this often. A line
 * stuck high would otherwise make the keyboard look dead for ever. */
#define SWEEP_INTERVAL_US 500000ULL

/* How many times the gate may claim "events pending" and be wrong before it
 * is switched off. A line stuck low would otherwise cost a full I2C poll
 * every tick, which is the cost the gate exists to avoid. */
#define GATE_FALSE_LIMIT 5

static void bus_release(struct kbd_tca8418 *k)
{
    k->bus->release(k->bus->ctx);
}

static int read_reg(struct kbd_tca8418 *k, uint8_t reg, uint8_t *value)
{
    return k->bus->read_reg(k->bus->ctx, reg, value);
}

static int write_reg(struct kbd_tca8418 *k, uint8_t reg, uint8_t value)
{
    return k->bus->write_reg(k->bus->ctx, reg, value);
}

/* Drain whatever the FIFO holds at startup and clear the status, so the
 * first poll reports what the owner pressed rather than what was left over
 * from the vendor launcher or from a previous run. */
static void flush_events(struct kbd_tca8418 *k)
{
    uint8_t event;
    int i;

    for (i = 0; i < FLUSH_MAX; i++) {
        if (read_reg(k, REG_KEY_EVENT_A, &event) != 0 || event == 0) {
            break;
        }
    }
    (void)write_reg(k, REG_INT_STAT, INT_STAT_CLEAR_FLUSH);
}

/* The vendor's sequence, with every return value checked. The bus is held
 * for the whole of it and released on every path. */
static int configure(struct kbd_tca8418 *k)
{
    uint8_t probe;

    if (k->bus->claim(k->bus->ctx) != 0) {
        return -1;
    }
    if (k->bus->reset_pulse(k->bus->ctx) != 0) {
        goto fail;
    }
    /* The presence probe. Nothing below matters if this does not answer. */
    if (read_reg(k, REG_KEY_LCK_EC, &probe) != 0) {
        goto fail;
    }
    if (write_reg(k, REG_KP_GPIO1, KP_GPIO1_VALUE) != 0 ||
        write_reg(k, REG_KP_GPIO2, KP_GPIO2_VALUE) != 0 ||
        write_reg(k, REG_KP_GPIO3, KP_GPIO3_VALUE) != 0) {
        goto fail;
    }
    if (write_reg(k, REG_DEBOUNCE_DIS1, DEBOUNCE_ENABLED) != 0 ||
        write_reg(k, REG_DEBOUNCE_DIS2, DEBOUNCE_ENABLED) != 0 ||
        write_reg(k, REG_DEBOUNCE_DIS3, DEBOUNCE_ENABLED) != 0) {
        goto fail;
    }
    if (write_reg(k, REG_CFG, CFG_VALUE) != 0) {
        goto fail;
    }
    flush_events(k);
    bus_release(k);
    return 0;

fail:
    bus_release(k);
    return -1;
}

static void schedule_retry(struct kbd_tca8418 *k, uint64_t now_us)
{
    uint64_t wait;

    if (k->retry_count == 0) {
        wait = RETRY_1_US;
    } else if (k->retry_count == 1) {
        wait = RETRY_2_US;
    } else {
        wait = RETRY_MAX_US;
    }
    if (k->retry_count < 2) {
        k->retry_count++;
    }
    k->next_retry_us = now_us + wait;
    k->state = KBD_TCA8418_FAILED;
}

int kbd_tca8418_init(struct kbd_tca8418 *k, const struct kbd_bus *bus,
                     uint64_t now_us)
{
    if (!k || !bus) {
        return 0;
    }
    k->bus = bus;
    k->state = KBD_TCA8418_ABSENT;
    k->gate = false;
    k->gate_false_asserted = 0;
    k->next_sweep_us = now_us;
    k->next_retry_us = 0;
    k->retry_count = 0;
    k->overflow_count = 0;
    k->unknown_count = 0;
    k->events_total = 0;
    k->overflow_pending = false;

    if (configure(k) != 0) {
        return 0; /* absent is normal, not an error (design §9) */
    }
    k->state = KBD_TCA8418_READY;
    /* Use the INT line as a gate only if it can be read at all. Whether it
     * tells the truth is settled at runtime, not assumed here: GPIO42 has
     * never been verified on a connected base. */
    k->gate = bus->irq_level(bus->ctx) >= 0;
    /* The first unconditional sweep belongs one interval away. Scheduling it
     * at now would make the very next poll touch the bus whatever the gate
     * said, which is the cost the gate exists to avoid. */
    k->next_sweep_us = now_us + SWEEP_INTERVAL_US;
    return 1;
}

/* The FIFO drain of design §6. The bus is already claimed. */
static int drain(struct kbd_tca8418 *k, kbd_tca8418_event_fn on_event,
                 void *user)
{
    uint8_t int_stat = 0;
    uint8_t count_reg = 0;
    unsigned count;
    unsigned limit;
    int handled = 0;
    unsigned i;

    /* A failed status read is not fatal: it costs the overflow check and
     * the clear decision, not the events. */
    int int_stat_valid = read_reg(k, REG_INT_STAT, &int_stat) == 0;

    if (read_reg(k, REG_KEY_LCK_EC, &count_reg) != 0) {
        return -1;
    }
    count = count_reg & EVENT_COUNT_MASK;

    if (int_stat_valid && (int_stat & INT_STAT_OVERFLOW)) {
        /* The controller dropped events. Whatever the key map believes
         * about held modifiers may now be wrong, so the caller is told to
         * forget it; then the drain continues, as the vendor's does. */
        k->overflow_count++;
        k->overflow_pending = true;
    }

    /* When the count register reads zero the FIFO may still hold events, so
     * one read is always attempted; the zero byte below ends it. */
    limit = count > 0 ? count : 1;
    if (limit > KBD_TCA8418_DRAIN_MAX) {
        limit = KBD_TCA8418_DRAIN_MAX;
    }
    for (i = 0; i < limit; i++) {
        uint8_t event;
        uint8_t code;

        if (read_reg(k, REG_KEY_EVENT_A, &event) != 0) {
            return handled > 0 ? handled : -1; /* keep what was read */
        }
        if (event == 0) {
            break; /* the FIFO is empty */
        }
        code = (uint8_t)(event & EVENT_CODE);
        if (code < KBD_TCA8418_CODE_MIN || code > KBD_TCA8418_CODE_MAX) {
            k->unknown_count++;
            continue; /* a position this matrix cannot produce */
        }
        k->events_total++;
        handled++;
        if (on_event) {
            on_event(user, event);
        }
    }

    if (handled > 0 || (int_stat_valid && (int_stat & INT_STAT_CLEAR))) {
        (void)write_reg(k, REG_INT_STAT, INT_STAT_CLEAR);
    }
    return handled;
}

int kbd_tca8418_poll(struct kbd_tca8418 *k, uint64_t now_us,
                     kbd_tca8418_event_fn on_event, void *user)
{
    bool swept;
    int level;
    int handled;

    if (!k || k->state == KBD_TCA8418_ABSENT) {
        return 0;
    }
    if (k->state == KBD_TCA8418_FAILED) {
        if (now_us < k->next_retry_us) {
            return 0;
        }
        if (configure(k) != 0) {
            schedule_retry(k, now_us);
            return -1;
        }
        k->state = KBD_TCA8418_READY;
        k->retry_count = 0;
        k->next_sweep_us = now_us + SWEEP_INTERVAL_US;
        return 0;
    }

    swept = now_us >= k->next_sweep_us;
    if (k->gate && !swept) {
        level = k->bus->irq_level(k->bus->ctx);
        if (level < 0) {
            k->gate = false; /* the line went away; poll unconditionally */
        } else if (level != 0) {
            return 0; /* idle, and the bus is never touched */
        }
    }
    if (swept) {
        k->next_sweep_us = now_us + SWEEP_INTERVAL_US;
    }

    if (k->bus->claim(k->bus->ctx) != 0) {
        schedule_retry(k, now_us);
        return -1;
    }
    handled = drain(k, on_event, user);
    bus_release(k);

    if (handled < 0) {
        schedule_retry(k, now_us);
        return -1;
    }
    /* Two ways the gate can lie, and both switch it off. A line stuck low
     * asks for an I2C poll that finds nothing, every tick; a line stuck
     * high hides events until the sweep finds them. */
    if (k->gate) {
        if (swept && handled > 0) {
            k->gate = false;
        } else if (!swept && handled == 0) {
            k->gate_false_asserted++;
            if (k->gate_false_asserted > GATE_FALSE_LIMIT) {
                k->gate = false;
            }
        } else if (handled > 0) {
            k->gate_false_asserted = 0;
        }
    }
    return handled;
}

bool kbd_tca8418_take_overflow(struct kbd_tca8418 *k)
{
    bool pending;

    if (!k) {
        return false;
    }
    pending = k->overflow_pending;
    k->overflow_pending = false;
    return pending;
}

bool kbd_tca8418_ready(const struct kbd_tca8418 *k)
{
    return k && k->state == KBD_TCA8418_READY;
}

bool kbd_tca8418_gated(const struct kbd_tca8418 *k)
{
    return k && k->gate;
}
