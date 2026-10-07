/*
 * The K230's keyboard bus: bit-banged I2C on GPIO46/47. See kbd_bus_k230.h
 * and docs/hardware/KEYBOARD_DRIVER_DESIGN_2026-09-12.md §4 and §8.
 *
 * No hardware I2C controller reaches this keyboard: I2C4 is muxed to the
 * camera, and nothing in the device tree claims io46/io47 (VERIFIED,
 * KEYBOARD_BRINGUP_2026-09-10.md §1). The only route is the vendor's: mux
 * the two pins to plain GPIO through /dev/mem, drive them open-drain with
 * pull-ups through libgpiod v2, and clock the bus by hand.
 *
 * Two things here are deliberately not the vendor's. It re-muxes, requests
 * the lines and restores on *every single register access*, so one FIFO
 * drain performs a mux flip per event; this maps /dev/mem once, holds the
 * line request for the life of the shell, and reduces claim and release to
 * two register stores. And it restores the mux words it actually read at
 * start-up rather than a constant, because the board reads back 0x800019D1
 * where the vendor writes 0x000019D1 (VERIFIED) and bit 31 looks read-only.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kbd_bus_k230.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Pins, from the vendor pin map and its driver (DOCUMENTED). The kernel
 * numbers them from 0 across two chips of 32, so the offset within
 * /dev/gpiochip1 is pin - 32. */
#define PIN_SCL 46
#define PIN_SDA 47
#define PIN_IRQ 42
#define PIN_RST 43
/* The keyboard light's PWM output (vendor pin map: GPIO52 / PWM4). */
#define PIN_KBD_LIGHT 52
#define GPIO_CHIP "/dev/gpiochip1"
#define LINES_PER_CHIP 32u
#define OFF_SCL (PIN_SCL - LINES_PER_CHIP)
#define OFF_SDA (PIN_SDA - LINES_PER_CHIP)
#define OFF_IRQ (PIN_IRQ - LINES_PER_CHIP)
#define OFF_RST (PIN_RST - LINES_PER_CHIP)

/* The iomux block. Each pin is one 32-bit word at pin * 4. */
#define IOMUX_BASE 0x91105000UL
#define IOMUX_SIZE 0x1000UL
#define IOMUX_OFF(pin) ((pin) * 4u)

/* Function 0 with the drive and pull bits the vendor uses: plain GPIO, the
 * state the bus is bit-banged in. */
#define IOMUX_BITBANG 0x000001D1UL
/* GPIO input with a pull-up, for the controller's INT line. */
#define IOMUX_IRQ_INPUT 0x00000344UL
/* io52 as PWM4, the vendor launcher's own word for the keyboard light
 * (k230_phone_ui ui_hardware.c, DOCUMENTED). The card boots it as a GPIO
 * that U-Boot drives low (0x18F read on units A and B, 2026-09-30). */
#define IOMUX_PWM4 0x00001191UL

#define TCA8418_ADDR 0x34u

/* The vendor's half-period. Eight microseconds gives roughly 50-60 kHz,
 * which this bus has been running at since the vendor shipped it. */
#define BUS_DELAY_NS 8000L

/* The reset pulse, held rather than pulsed and released (design §5). */
#define RESET_LOW_MS 3
#define RESET_HIGH_MS 12

struct k230_bus {
    volatile uint32_t *iomux;
    uint32_t saved_scl;
    uint32_t saved_sda;
    uint32_t saved_irq;
    bool mux_saved;
    uint32_t saved_light; /* io52 before the keyboard light took it */
    bool light_muxed;
    struct gpiod_chip *chip;
    struct gpiod_line_request *i2c; /* SCL and SDA in one request */
    struct gpiod_line_request *irq;
    bool claimed;
    /* Set by the first GPIO access of a transaction that failed, cleared when
     * the next one starts. A bus that cannot be driven or read has no levels
     * to report, and must not be allowed to look like a talkative chip. */
    bool io_error;
};

/* The shell owns exactly one keyboard, so one instance avoids an allocation
 * failure path in code that already has enough of them. */
static struct k230_bus the_bus;

static void say(char *why, size_t why_len, const char *fmt, ...)
{
    va_list ap;

    if (!why || why_len == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(why, why_len, fmt, ap);
    va_end(ap);
}

/* ---- the pin mux ------------------------------------------------------- */

static uint32_t mux_read(struct k230_bus *b, unsigned pin)
{
    return b->iomux[IOMUX_OFF(pin) / 4u];
}

static void mux_write(struct k230_bus *b, unsigned pin, uint32_t value)
{
    b->iomux[IOMUX_OFF(pin) / 4u] = value;
}

/* ---- bit timing -------------------------------------------------------- */

/* A busy wait rather than a sleep. This runs on the LVGL thread, where a
 * nanosleep of eight microseconds costs a scheduler round trip and turns a
 * one-millisecond register read into several; spinning keeps the cost of a
 * poll where the design says it is. */
static void bus_delay(void)
{
    struct timespec start;
    struct timespec now;
    long elapsed;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return;
    }
    for (;;) {
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return;
        }
        elapsed = (long)(now.tv_sec - start.tv_sec) * 1000000000L +
                  (now.tv_nsec - start.tv_nsec);
        if (elapsed >= BUS_DELAY_NS) {
            return;
        }
    }
}

/* Open drain: "high" releases the line to the pull-up, "low" drives it.
 *
 * Every GPIO access here can fail, and a failure is not a level. The result
 * used to be discarded on writes and folded into "low" on reads - and a low
 * SDA is an acknowledge, so a bus that had gone away (the base unplugged, the
 * gpiochip unbound) answered every byte with an ACK and returned zeroes. The
 * chip layer then saw a controller that was present, configured and simply
 * never had any events, and its retry and reporting never ran. So the first
 * failure of a transaction is remembered here and the transaction fails. */
static void io_failed(struct k230_bus *b)
{
    b->io_error = true;
}

static void line_set(struct k230_bus *b, unsigned offset, bool high)
{
    if (gpiod_line_request_set_value(b->i2c, offset,
                                     high ? GPIOD_LINE_VALUE_ACTIVE
                                          : GPIOD_LINE_VALUE_INACTIVE) < 0) {
        io_failed(b);
    }
    bus_delay();
}

static void scl(struct k230_bus *b, bool high)
{
    line_set(b, OFF_SCL, high);
}

static void sda(struct k230_bus *b, bool high)
{
    line_set(b, OFF_SDA, high);
}

/* 1 high, 0 low, -1 the line could not be read. Never collapse the last into
 * either of the first two: the caller reads low as an acknowledge. */
static int sda_read(struct k230_bus *b)
{
    enum gpiod_line_value v;

    bus_delay();
    v = gpiod_line_request_get_value(b->i2c, OFF_SDA);
    if (v == GPIOD_LINE_VALUE_ACTIVE) {
        return 1;
    }
    if (v == GPIOD_LINE_VALUE_INACTIVE) {
        return 0;
    }
    io_failed(b);
    return -1;
}

/* ---- I2C ---------------------------------------------------------------- */

static void i2c_start(struct k230_bus *b)
{
    sda(b, true);
    scl(b, true);
    sda(b, false);
    scl(b, false);
}

static void i2c_stop(struct k230_bus *b)
{
    sda(b, false);
    scl(b, true);
    sda(b, true);
}

/* Returns 0 when the slave acknowledged, -1 when it did not or when the bus
 * could not be driven or read. */
static int i2c_write_byte(struct k230_bus *b, uint8_t byte)
{
    int level;
    int i;

    for (i = 7; i >= 0; i--) {
        sda(b, (byte >> i) & 1u);
        scl(b, true);
        scl(b, false);
    }
    sda(b, true); /* release for the acknowledge */
    scl(b, true);
    level = sda_read(b);
    scl(b, false);
    /* Only a line that actually read low is an acknowledge. */
    return (level == 0 && !b->io_error) ? 0 : -1;
}

/* Returns 0 and fills *out, or -1 when any bit could not be read. */
static int i2c_read_byte(struct k230_bus *b, bool ack, uint8_t *out)
{
    uint8_t byte = 0;
    int i;

    sda(b, true);
    for (i = 7; i >= 0; i--) {
        int level;

        scl(b, true);
        level = sda_read(b);
        if (level < 0) {
            /* Finish the bit so the bus is left in a known state, then give
             * up: a byte with an unread bit in it is not a byte. */
            scl(b, false);
            sda(b, true);
            return -1;
        }
        if (level > 0) {
            byte |= (uint8_t)(1u << i);
        }
        scl(b, false);
    }
    sda(b, !ack);
    scl(b, true);
    scl(b, false);
    sda(b, true);
    if (b->io_error) {
        return -1;
    }
    *out = byte;
    return 0;
}

static int read_at(struct k230_bus *b, uint8_t addr, uint8_t reg, uint8_t *value)
{
    int rc = -1;

    if (!b->claimed) {
        return -1;
    }
    b->io_error = false;
    i2c_start(b);
    if (i2c_write_byte(b, (uint8_t)(addr << 1)) != 0) {
        goto out;
    }
    if (i2c_write_byte(b, reg) != 0) {
        goto out;
    }
    i2c_start(b); /* repeated start */
    if (i2c_write_byte(b, (uint8_t)((addr << 1) | 1u)) != 0) {
        goto out;
    }
    if (i2c_read_byte(b, false, value) != 0) {
        goto out;
    }
    rc = 0;
out:
    i2c_stop(b); /* the bus is released whatever happened */
    /* A stop that could not be driven still fails the transaction. */
    return b->io_error ? -1 : rc;
}

static int write_at(struct k230_bus *b, uint8_t addr, uint8_t reg, uint8_t value)
{
    int rc = -1;

    if (!b->claimed) {
        return -1;
    }
    b->io_error = false;
    i2c_start(b);
    if (i2c_write_byte(b, (uint8_t)(addr << 1)) != 0) {
        goto out;
    }
    if (i2c_write_byte(b, reg) != 0) {
        goto out;
    }
    if (i2c_write_byte(b, value) != 0) {
        goto out;
    }
    rc = 0;
out:
    i2c_stop(b); /* the bus is released whatever happened */
    return b->io_error ? -1 : rc;
}

static int k230_read_reg(void *ctx, uint8_t reg, uint8_t *value)
{
    return read_at(ctx, TCA8418_ADDR, reg, value);
}

static int k230_write_reg(void *ctx, uint8_t reg, uint8_t value)
{
    return write_at(ctx, TCA8418_ADDR, reg, value);
}

/* Another device on the same two lines (kbd_bus.h): the XL9555 expander.
 * Same process, same thread, same claim - the bus still has one owner
 * (design §0.2). */
static int k230_read_reg_at(void *ctx, uint8_t addr, uint8_t reg, uint8_t *value)
{
    return read_at(ctx, addr, reg, value);
}

static int k230_write_reg_at(void *ctx, uint8_t addr, uint8_t reg, uint8_t value)
{
    return write_at(ctx, addr, reg, value);
}

/* ---- claim, release, reset, INT ---------------------------------------- */

static int k230_claim(void *ctx)
{
    struct k230_bus *b = ctx;

    if (!b->iomux || !b->i2c) {
        return -1;
    }
    mux_write(b, PIN_SCL, IOMUX_BITBANG);
    mux_write(b, PIN_SDA, IOMUX_BITBANG);
    b->claimed = true;
    /* Both lines idle high before anyone starts a transaction. */
    sda(b, true);
    scl(b, true);
    return 0;
}

static void k230_release(void *ctx)
{
    struct k230_bus *b = ctx;

    if (!b->iomux) {
        return;
    }
    if (b->mux_saved) {
        mux_write(b, PIN_SCL, b->saved_scl);
        mux_write(b, PIN_SDA, b->saved_sda);
    }
    b->claimed = false;
}

static void sleep_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

/* The vendor requests the reset line, sets it, sleeps a millisecond and
 * releases, so the pin floats for most of the pulse (DOCUMENTED). This
 * holds the line for the whole pulse, which is what the part's timing
 * actually asks for. */
static int k230_reset_pulse(void *ctx)
{
    struct k230_bus *b = ctx;
    struct gpiod_line_settings *settings;
    struct gpiod_line_config *line_cfg;
    struct gpiod_request_config *req_cfg;
    struct gpiod_line_request *req = NULL;
    unsigned int offset = OFF_RST;
    int rc;

    if (!b->chip) {
        return -1;
    }
    settings = gpiod_line_settings_new();
    line_cfg = gpiod_line_config_new();
    req_cfg = gpiod_request_config_new();
    if (!settings || !line_cfg || !req_cfg) {
        goto out;
    }
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);
    gpiod_request_config_set_consumer(req_cfg, "pocketos-shell-kbd-rst");
    if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) == 0) {
        req = gpiod_chip_request_lines(b->chip, req_cfg, line_cfg);
    }
out:
    gpiod_line_settings_free(settings);
    gpiod_line_config_free(line_cfg);
    gpiod_request_config_free(req_cfg);
    if (!req) {
        return -1;
    }
    sleep_ms(RESET_LOW_MS);
    /* Releasing the line is what ends the pulse. If that store did not reach
     * the pin the part is still held in reset, and reporting a reset that did
     * not happen sends the chip layer on to configure a controller that
     * cannot answer. */
    rc = gpiod_line_request_set_value(req, offset, GPIOD_LINE_VALUE_ACTIVE) < 0 ? -1 : 0;
    sleep_ms(RESET_HIGH_MS);
    gpiod_line_request_release(req); /* released on both paths */
    return rc;
}

static int k230_irq_level(void *ctx)
{
    struct k230_bus *b = ctx;
    enum gpiod_line_value v;

    if (!b->irq) {
        return -1;
    }
    v = gpiod_line_request_get_value(b->irq, OFF_IRQ);
    if (v == GPIOD_LINE_VALUE_ACTIVE) {
        return 1; /* idle */
    }
    if (v == GPIOD_LINE_VALUE_INACTIVE) {
        return 0; /* asserted: events pending */
    }
    return -1;
}

/* ---- construction ------------------------------------------------------ */

/* Only one program may drive these two pins (VERIFIED). S90 already refuses
 * to start while the vendor launcher is enabled, and this is the second
 * line of defence: if the launcher is somehow running, the bus is left
 * alone rather than fought over. */
static bool launcher_running(void)
{
    DIR *proc = opendir("/proc");
    struct dirent *ent;
    bool found = false;

    if (!proc) {
        return false;
    }
    while (!found && (ent = readdir(proc)) != NULL) {
        char path[64];
        char comm[64];
        FILE *f;

        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
            continue;
        }
        /* The precision is not decoration: a directory entry may be 255
         * bytes, the compiler cannot see that this one is a pid, and
         * without a bound it warns that the path may be truncated. */
        snprintf(path, sizeof(path), "/proc/%.20s/comm", ent->d_name);
        f = fopen(path, "r");
        if (!f) {
            continue;
        }
        if (fgets(comm, sizeof(comm), f) && strstr(comm, "k230_phone_ui")) {
            found = true;
        }
        fclose(f);
    }
    closedir(proc);
    return found;
}

static int request_i2c_lines(struct k230_bus *b)
{
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    unsigned int offsets[2] = { OFF_SCL, OFF_SDA };
    int rc = -1;

    if (!settings || !line_cfg || !req_cfg) {
        goto out;
    }
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_drive(settings, GPIOD_LINE_DRIVE_OPEN_DRAIN);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_ACTIVE);
    gpiod_request_config_set_consumer(req_cfg, "pocketos-shell-kbd");
    if (gpiod_line_config_add_line_settings(line_cfg, offsets, 2, settings) != 0) {
        goto out;
    }
    b->i2c = gpiod_chip_request_lines(b->chip, req_cfg, line_cfg);
    rc = b->i2c ? 0 : -1;
out:
    gpiod_line_settings_free(settings);
    gpiod_line_config_free(line_cfg);
    gpiod_request_config_free(req_cfg);
    return rc;
}

/* The INT line is only ever read as a level (design §14), so it is
 * requested as a plain input: no edge detection, no event buffer, no
 * thread. */
static int request_irq_line(struct k230_bus *b)
{
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    unsigned int offset = OFF_IRQ;
    int rc = -1;

    if (!settings || !line_cfg || !req_cfg) {
        goto out;
    }
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
    gpiod_request_config_set_consumer(req_cfg, "pocketos-shell-kbd-irq");
    if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) != 0) {
        goto out;
    }
    b->irq = gpiod_chip_request_lines(b->chip, req_cfg, line_cfg);
    rc = b->irq ? 0 : -1;
out:
    gpiod_line_settings_free(settings);
    gpiod_line_config_free(line_cfg);
    gpiod_request_config_free(req_cfg);
    return rc;
}

static void teardown(struct k230_bus *b)
{
    if (b->i2c) {
        gpiod_line_request_release(b->i2c);
        b->i2c = NULL;
    }
    if (b->irq) {
        gpiod_line_request_release(b->irq);
        b->irq = NULL;
    }
    if (b->chip) {
        gpiod_chip_close(b->chip);
        b->chip = NULL;
    }
    if (b->iomux) {
        if (b->mux_saved) {
            mux_write(b, PIN_SCL, b->saved_scl);
            mux_write(b, PIN_SDA, b->saved_sda);
            mux_write(b, PIN_IRQ, b->saved_irq);
        }
        if (b->light_muxed) {
            mux_write(b, PIN_KBD_LIGHT, b->saved_light);
        }
        munmap((void *)b->iomux, IOMUX_SIZE);
        b->iomux = NULL;
    }
    b->mux_saved = false;
    b->light_muxed = false;
    b->claimed = false;
}

int kbd_bus_k230_light_mux(const struct kbd_bus *bus)
{
    struct k230_bus *b = bus ? bus->ctx : NULL;

    if (!b || !b->iomux) {
        return -1;
    }
    if (!b->light_muxed) {
        b->saved_light = mux_read(b, PIN_KBD_LIGHT);
        b->light_muxed = true;
    }
    mux_write(b, PIN_KBD_LIGHT, IOMUX_PWM4);
    return 0;
}

int kbd_bus_k230_create(struct kbd_bus *bus, char *why, size_t why_len)
{
    struct k230_bus *b = &the_bus;
    int fd;
    void *map;

    if (!bus) {
        return -1;
    }
    memset(b, 0, sizeof(*b));

    if (launcher_running()) {
        say(why, why_len, "the vendor launcher is running; it owns this bus");
        return -1;
    }

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        say(why, why_len, "/dev/mem: %s", strerror(errno));
        return -1;
    }
    map = mmap(NULL, IOMUX_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               (off_t)IOMUX_BASE);
    /* The mapping outlives the descriptor, exactly as the vendor does it. */
    close(fd);
    if (map == MAP_FAILED) {
        say(why, why_len, "iomux mmap: %s", strerror(errno));
        return -1;
    }
    b->iomux = map;

    /* Save what was there. Restoring the value read avoids having to decide
     * whether the board's read-back or the vendor's constant is right. */
    b->saved_scl = mux_read(b, PIN_SCL);
    b->saved_sda = mux_read(b, PIN_SDA);
    b->saved_irq = mux_read(b, PIN_IRQ);
    b->mux_saved = true;

    b->chip = gpiod_chip_open(GPIO_CHIP);
    if (!b->chip) {
        say(why, why_len, "%s: %s", GPIO_CHIP, strerror(errno));
        teardown(b);
        return -1;
    }
    if (request_i2c_lines(b) != 0) {
        say(why, why_len, "gpio %u/%u: %s", PIN_SCL, PIN_SDA, strerror(errno));
        teardown(b);
        return -1;
    }
    /* The INT line is optional: without it the chip layer polls the bus
     * unconditionally, which is slower but correct (design §14). */
    mux_write(b, PIN_IRQ, IOMUX_IRQ_INPUT);
    if (request_irq_line(b) != 0) {
        b->irq = NULL;
    }

    bus->claim = k230_claim;
    bus->release = k230_release;
    bus->read_reg = k230_read_reg;
    bus->write_reg = k230_write_reg;
    bus->reset_pulse = k230_reset_pulse;
    bus->irq_level = k230_irq_level;
    bus->ctx = b;
    bus->read_reg_at = k230_read_reg_at;
    bus->write_reg_at = k230_write_reg_at;
    return 0;
}

void kbd_bus_k230_destroy(struct kbd_bus *bus)
{
    if (!bus || !bus->ctx) {
        return;
    }
    teardown(bus->ctx);
    memset(bus, 0, sizeof(*bus));
}
