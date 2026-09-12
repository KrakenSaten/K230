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

/* And the same in the other direction: how many sweeps may find events while
 * the line reads idle before it is disbelieved. This is not symmetry for its
 * own sake. A sweep drains whatever the line says, so between reading the
 * level and reading the FIFO there is a window - a claim plus one or two
 * register reads, one to two milliseconds on this bus - in which a key
 * genuinely pressed just now arrives and is found. Blaming the line for that
 * single coincidence would switch the gate off during ordinary typing: at
 * eight events per second and two sweeps per second it happens within a
 * minute or two, and the gate never comes back. Requiring the evidence to
 * repeat, and clearing it below the moment the line is seen asserted,
 * separates a line stuck high - which never reads asserted at all, so it
 * reaches the limit in under two seconds - from that coincidence. */
#define GATE_FALSE_IDLE_LIMIT 2

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
 * from the vendor launcher or from a previous run.
 *
 * Returns 0, or -1 when the bus would not answer. A failure here is a failure
 * of the configure: it means the leftovers are still in the FIFO and the
 * status still stands, so the first poll would report a keypress from before
 * this process started, and the comment above configure() claiming every
 * return value is checked would not have been true. */
static int flush_events(struct kbd_tca8418 *k)
{
    uint8_t event;
    int i;

    for (i = 0; i < FLUSH_MAX; i++) {
        if (read_reg(k, REG_KEY_EVENT_A, &event) != 0) {
            return -1;
        }
        if (event == 0) {
            break;
        }
    }
    return write_reg(k, REG_INT_STAT, INT_STAT_CLEAR_FLUSH) != 0 ? -1 : 0;
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
    if (flush_events(k) != 0) {
        goto fail;
    }
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
    k->gate_false_idle = 0;
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
                 kbd_tca8418_overflow_fn on_overflow, void *user)
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
         * forget it - here, before a single event of this batch is handed
         * over, because the survivors are exactly the ones whose modifier
         * context was destroyed. Then the drain continues, as the vendor's
         * does. */
        k->overflow_count++;
        k->overflow_pending = true;
        if (on_overflow) {
            on_overflow(user);
        }
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
                     kbd_tca8418_event_fn on_event,
                     kbd_tca8418_overflow_fn on_overflow, void *user)
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
    /* The level is read on the sweep too, and not only when it decides
     * whether to drain. A sweep drains either way, so the only thing the
     * reading is wanted for there is the verdict below: a sweep that finds
     * events can only be evidence against the line if the line was claiming
     * to be idle at the time. Without this the verdict had nothing to go on
     * and blamed the line for every sweep that happened to coincide with a
     * keypress, which is most of them once someone is typing. */
    level = -1;
    if (k->gate) {
        level = k->bus->irq_level(k->bus->ctx);
        if (level < 0) {
            k->gate = false; /* the line went away; poll unconditionally */
        } else if (level == 0) {
            /* Asserted: the line is doing its job. Whatever idle sweeps were
             * counted against it were coincidences, not a stuck line. */
            k->gate_false_idle = 0;
        } else if (!swept) {
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
    handled = drain(k, on_event, on_overflow, user);
    bus_release(k);

    if (handled < 0) {
        schedule_retry(k, now_us);
        return -1;
    }
    /* Two ways the gate can lie, and both switch it off. A line stuck low
     * asks for an I2C poll that finds nothing, every tick; a line stuck high
     * hides events until the sweep finds them.
     *
     * The stuck-high verdict requires the line to have actually read idle
     * (level > 0) while the sweep found events. A line that read asserted
     * told the truth and is left alone however many events turned up - that
     * is the ordinary case while someone is typing, and treating it as a
     * lie is what used to switch the gate off within seconds of first use. */
    if (k->gate) {
        if (swept && handled > 0 && level > 0) {
            k->gate_false_idle++;
            if (k->gate_false_idle > GATE_FALSE_IDLE_LIMIT) {
                k->gate = false;
            }
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
