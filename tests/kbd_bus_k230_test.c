/*
 * The K230 keyboard bus one layer below the chip driver: the bit-banged I2C
 * itself, driven against a fake libgpiod so a GPIO that fails can be injected
 * at the only place it actually happens - the individual line access.
 *
 * tests/kbd_tca8418_test.c fakes read_reg/write_reg, so it can say what the
 * chip layer does with a failed register access but nothing at all about how
 * one comes to be reported. That is where cold review F6 lives: a failed GPIO
 * read used to be folded into "line low", a low SDA is an acknowledge, and a
 * bus that had gone away therefore acknowledged every byte and handed back
 * zeroes. The chip layer saw a present, configured, permanently silent
 * keyboard, and none of its retry or reporting ran.
 *
 * The translation unit is included rather than linked so the static bit
 * banger can be reached directly. Nothing here touches /dev/mem, /proc or a
 * real gpiochip: only the line accesses are faked, and everything between
 * them is the shipped code.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "kbd_bus_k230.c"

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* ---- the fake gpiod ---------------------------------------------------- */

/* What SDA reads, sample by sample, in the order the bit banger samples it.
 * Scripting the line rather than modelling a slave keeps the fake honest:
 * it says only what a logic analyser would have seen, and every sample the
 * shipped code takes consumes exactly one entry.
 *
 * A transaction samples SDA once per byte written (the acknowledge) and once
 * per bit read. Anything past the end of the script reads high, which is the
 * idle line - a NACK, never an accidental acknowledge. */
#define FAKE_MAX_SAMPLES 64

static struct fake_gpio {
    int fail_reads_after;   /* -1 never; otherwise samples before failing */
    int fail_writes_after;  /* -1 never; otherwise stores before failing */
    unsigned reads;
    unsigned writes;
    int level[FAKE_MAX_SAMPLES];
    unsigned levels;
    unsigned next;
} g;

static void fake_reset(void)
{
    memset(&g, 0, sizeof(g));
    g.fail_reads_after = -1;
    g.fail_writes_after = -1;
}

/* The slave holds SDA down for one sample: an acknowledge. */
static void push_ack(void)
{
    g.level[g.levels++] = 0;
}

/* The slave leaves SDA up for one sample: no acknowledge. */
static void push_nack(void)
{
    g.level[g.levels++] = 1;
}

/* Eight samples, most significant bit first. */
static void push_byte(uint8_t v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        g.level[g.levels++] = (v >> i) & 1;
    }
}

int gpiod_line_request_set_value(struct gpiod_line_request *request,
                                 unsigned int offset, enum gpiod_line_value value)
{
    (void)request;
    (void)offset;
    (void)value;
    g.writes++;
    if (g.fail_writes_after >= 0 && g.writes > (unsigned)g.fail_writes_after) {
        return -1;
    }
    return 0;
}

enum gpiod_line_value gpiod_line_request_get_value(struct gpiod_line_request *request,
                                                   unsigned int offset)
{
    (void)request;
    (void)offset;
    g.reads++;
    if (g.fail_reads_after >= 0 && g.reads > (unsigned)g.fail_reads_after) {
        return GPIOD_LINE_VALUE_ERROR;
    }
    if (g.next < g.levels) {
        return g.level[g.next++] ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    }
    return GPIOD_LINE_VALUE_ACTIVE; /* the idle line, pulled up */
}

/* The rest of libgpiod is never reached by these tests; they exist so the
 * translation unit links. */
struct gpiod_chip *gpiod_chip_open(const char *path) { (void)path; return NULL; }
void gpiod_chip_close(struct gpiod_chip *chip) { (void)chip; }
struct gpiod_line_request *gpiod_chip_request_lines(struct gpiod_chip *c,
                                                    struct gpiod_request_config *r,
                                                    struct gpiod_line_config *l)
{ (void)c; (void)r; (void)l; return NULL; }
struct gpiod_line_settings *gpiod_line_settings_new(void) { return NULL; }
void gpiod_line_settings_free(struct gpiod_line_settings *s) { (void)s; }
int gpiod_line_settings_set_direction(struct gpiod_line_settings *s, enum gpiod_line_direction d)
{ (void)s; (void)d; return 0; }
int gpiod_line_settings_set_drive(struct gpiod_line_settings *s, enum gpiod_line_drive d)
{ (void)s; (void)d; return 0; }
int gpiod_line_settings_set_bias(struct gpiod_line_settings *s, enum gpiod_line_bias b)
{ (void)s; (void)b; return 0; }
int gpiod_line_settings_set_output_value(struct gpiod_line_settings *s, enum gpiod_line_value v)
{ (void)s; (void)v; return 0; }
struct gpiod_line_config *gpiod_line_config_new(void) { return NULL; }
void gpiod_line_config_free(struct gpiod_line_config *c) { (void)c; }
int gpiod_line_config_add_line_settings(struct gpiod_line_config *c, const unsigned int *o,
                                        size_t n, struct gpiod_line_settings *s)
{ (void)c; (void)o; (void)n; (void)s; return 0; }
struct gpiod_request_config *gpiod_request_config_new(void) { return NULL; }
void gpiod_request_config_free(struct gpiod_request_config *c) { (void)c; }
void gpiod_request_config_set_consumer(struct gpiod_request_config *c, const char *s)
{ (void)c; (void)s; }
void gpiod_line_request_release(struct gpiod_line_request *r) { (void)r; }

/* ---- a bus that looks claimed, without claiming anything ---------------- */

static struct k230_bus *test_bus(void)
{
    struct k230_bus *b = &the_bus;

    memset(b, 0, sizeof(*b));
    /* Never dereferenced: the fake takes the request pointer and ignores it. */
    b->i2c = (struct gpiod_line_request *)(void *)&g;
    b->claimed = true;
    return b;
}

int main(void)
{
    struct k230_bus *b;
    uint8_t value;

    /* ---- 1. the honest case still works -------------------------------- */

    /* A register read writes three bytes (address, register, address again)
     * and then clocks eight bits out, so the line is sampled three times for
     * the acknowledges and eight times for the data. */
    b = test_bus();
    fake_reset();
    push_ack(); push_ack(); push_ack();
    push_byte(0xA5);
    check("a register read succeeds when the bus answers",
          k230_read_reg(b, 0x04, &value) == 0);
    check("and returns what the slave clocked out", value == 0xA5);

    b = test_bus();
    fake_reset();
    push_ack(); push_ack(); push_ack();
    check("a register write succeeds when the bus answers",
          k230_write_reg(b, 0x02, 0x09) == 0);

    /* ---- 2. a slave that does not answer -------------------------------- */

    b = test_bus();
    fake_reset();
    push_nack();
    check("a register read fails when nothing acknowledges",
          k230_read_reg(b, 0x04, &value) != 0);

    b = test_bus();
    fake_reset();
    push_nack();
    check("a register write fails when nothing acknowledges",
          k230_write_reg(b, 0x02, 0x09) != 0);

    /* ---- 3. the finding: a GPIO read that fails -------------------------
     *
     * GPIOD_LINE_VALUE_ERROR is -1, and the comparison that decided the level
     * was `v == GPIOD_LINE_VALUE_ACTIVE`, so an error came back false, which
     * is "low", which is an acknowledge. A bus that had gone away therefore
     * acknowledged every byte. */

    b = test_bus();
    fake_reset();
    g.fail_reads_after = 0; /* the line cannot be read at all */
    value = 0x11;
    check("a register read fails when SDA cannot be read",
          k230_read_reg(b, 0x04, &value) != 0);
    check("and the caller's byte is left alone", value == 0x11);

    b = test_bus();
    fake_reset();
    g.fail_reads_after = 0;
    check("a register write fails when SDA cannot be read",
          k230_write_reg(b, 0x02, 0x09) != 0);

    /* Every read failing must not produce a plausible 0x00 either: that is a
     * legal value for the event and lock registers and reads as "no events",
     * which is exactly how a dead bus used to look like a quiet keyboard. */
    b = test_bus();
    fake_reset();
    push_ack(); push_ack(); push_ack(); /* the address is acknowledged... */
    g.fail_reads_after = 3;             /* ...then the line dies mid-byte */
    value = 0x11;
    check("a read that dies part way through fails",
          k230_read_reg(b, 0x04, &value) != 0);
    check("rather than reporting an invented 0x00", value != 0x00);

    /* ---- 4. a GPIO write that fails ------------------------------------- */

    b = test_bus();
    fake_reset();
    push_ack(); push_ack(); push_ack();
    g.fail_writes_after = 4; /* the clock stops being driven */
    check("a register write fails when a line cannot be driven",
          k230_write_reg(b, 0x02, 0x09) != 0);

    b = test_bus();
    fake_reset();
    push_ack(); push_ack(); push_ack();
    push_byte(0xA5);
    g.fail_writes_after = 4;
    check("a register read fails when a line cannot be driven",
          k230_read_reg(b, 0x04, &value) != 0);

    /* A stop that could not be driven still fails the transaction: the bus is
     * left in an unknown state and the next one would start from there. */
    b = test_bus();
    fake_reset();
    push_ack(); push_ack(); push_ack();
    check("counting the stores of a good transaction",
          k230_write_reg(b, 0x02, 0x09) == 0);
    {
        unsigned good = g.writes;

        b = test_bus();
        fake_reset();
        push_ack(); push_ack(); push_ack();
        g.fail_writes_after = (int)good - 1; /* only the final stop fails */
        check("a transaction whose stop could not be driven fails",
              k230_write_reg(b, 0x02, 0x09) != 0);
    }

    /* ---- 5. the error does not leak into the next transaction ----------- */

    b = test_bus();
    fake_reset();
    g.fail_reads_after = 0;
    check("a failing transaction fails", k230_write_reg(b, 0x02, 0x09) != 0);
    fake_reset();
    push_ack(); push_ack(); push_ack();
    check("and the next one, on a bus that works, succeeds",
          k230_write_reg(b, 0x02, 0x09) == 0);

    /* ---- 6. a bus nobody claimed ---------------------------------------- */

    b = test_bus();
    b->claimed = false;
    fake_reset();
    check("a read on an unclaimed bus fails", k230_read_reg(b, 0x04, &value) != 0);
    check("a write on an unclaimed bus fails", k230_write_reg(b, 0x02, 0x09) != 0);

    printf("kbd_bus_k230_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
