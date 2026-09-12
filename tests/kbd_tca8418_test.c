/*
 * The TCA8418 state machine against a fake bus: no hardware, no /dev/mem,
 * no GPIO, no LVGL. This is the whole of the driver that can be tested
 * without the keyboard attached, which is why the chip layer was kept free
 * of everything else (design doc §11).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "kbd_tca8418.h"

#include <stdio.h>
#include <string.h>

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

/* ---- the fake bus ------------------------------------------------------ */

#define FIFO_MAX 40
#define WLOG_MAX 64

struct fake {
    uint8_t reg[0x40];
    uint8_t fifo[FIFO_MAX];
    unsigned fifo_n;
    unsigned fifo_i;

    unsigned claims;
    unsigned releases;
    unsigned reads;
    int claimed;
    int claim_rc;
    int reset_rc;
    int resets;
    int irq;             /* what irq_level returns */
    int fail_read;       /* register whose read fails, -1 for none */
    int fail_write;      /* register whose write fails, -1 for none */
    int read_outside;    /* a read or write happened with the bus released */

    struct { uint8_t reg; uint8_t val; } wlog[WLOG_MAX];
    unsigned wlog_n;
};

static uint8_t fifo_pop(struct fake *f)
{
    if (f->fifo_i >= f->fifo_n) {
        return 0; /* the controller reports an empty FIFO as a zero byte */
    }
    return f->fifo[f->fifo_i++];
}

static unsigned fifo_left(const struct fake *f)
{
    return f->fifo_n - f->fifo_i;
}

static int fake_claim(void *ctx)
{
    struct fake *f = ctx;

    f->claims++;
    if (f->claim_rc != 0) {
        return f->claim_rc;
    }
    f->claimed = 1;
    return 0;
}

static void fake_release(void *ctx)
{
    struct fake *f = ctx;

    f->releases++;
    f->claimed = 0;
}

static int fake_read(void *ctx, uint8_t reg, uint8_t *value)
{
    struct fake *f = ctx;

    if (!f->claimed) {
        f->read_outside = 1;
    }
    f->reads++;
    if (f->fail_read >= 0 && reg == (uint8_t)f->fail_read) {
        return -1;
    }
    if (reg == 0x03) {
        unsigned left = fifo_left(f);

        *value = (uint8_t)(left > 0x0F ? 0x0F : left);
        return 0;
    }
    if (reg == 0x04) {
        *value = fifo_pop(f);
        return 0;
    }
    *value = f->reg[reg];
    return 0;
}

static int fake_write(void *ctx, uint8_t reg, uint8_t value)
{
    struct fake *f = ctx;

    if (!f->claimed) {
        f->read_outside = 1;
    }
    if (f->wlog_n < WLOG_MAX) {
        f->wlog[f->wlog_n].reg = reg;
        f->wlog[f->wlog_n].val = value;
        f->wlog_n++;
    }
    if (f->fail_write >= 0 && reg == (uint8_t)f->fail_write) {
        return -1;
    }
    if (reg == 0x02) {
        /* INT_STAT is write-1-to-clear on the real part, and modelling it as
         * an ordinary register is not a harmless simplification: the driver
         * clears it by writing 0x09 and 0x0D, both of which carry
         * OVR_FLOW_INT, so a store-what-was-written fake reads that bit back
         * on the very next drain and reports an overflow that never
         * happened - on every drain, for the life of the test. */
        f->reg[reg] = (uint8_t)(f->reg[reg] & ~value);
        return 0;
    }
    f->reg[reg] = value;
    return 0;
}

static int fake_reset(void *ctx)
{
    struct fake *f = ctx;

    f->resets++;
    return f->reset_rc;
}

static int fake_irq(void *ctx)
{
    struct fake *f = ctx;

    return f->irq;
}

static void fake_init(struct fake *f, struct kbd_bus *bus)
{
    memset(f, 0, sizeof(*f));
    f->fail_read = -1;
    f->fail_write = -1;
    f->irq = 0; /* asserted by default, so a drain is attempted */

    bus->claim = fake_claim;
    bus->release = fake_release;
    bus->read_reg = fake_read;
    bus->write_reg = fake_write;
    bus->reset_pulse = fake_reset;
    bus->irq_level = fake_irq;
    bus->ctx = f;
}

static void fifo_push(struct fake *f, uint8_t raw)
{
    if (f->fifo_n < FIFO_MAX) {
        f->fifo[f->fifo_n++] = raw;
    }
}

/* ---- what the events looked like --------------------------------------- */

#define SEEN_MAX 64
static uint8_t seen[SEEN_MAX];
static unsigned seen_n;

static void on_event(void *user, uint8_t raw)
{
    (void)user;
    if (seen_n < SEEN_MAX) {
        seen[seen_n++] = raw;
    }
}

static void seen_reset(void)
{
    seen_n = 0;
    memset(seen, 0, sizeof(seen));
}

/* The order the two callbacks arrived in, as a string: 'O' for the overflow
 * announcement and 'E' for one delivered event. What matters about an
 * overflow is not that it was reported but that it was reported first, and a
 * sequence is the shortest way to assert that without reaching inside the
 * driver. */
#define ORDER_MAX 16
static char order[ORDER_MAX];
static unsigned order_n;

static void order_put(char c)
{
    if (order_n + 1 < ORDER_MAX) {
        order[order_n++] = c;
        order[order_n] = '\0';
    }
}

static void order_on_event(void *user, uint8_t raw)
{
    (void)user;
    (void)raw;
    order_put('E');
}

static void order_on_overflow(void *user)
{
    (void)user;
    order_put('O');
}

static void order_reset(void)
{
    order_n = 0;
    memset(order, 0, sizeof(order));
}

/* Codes measured on unit A (KEYBOARD_BRINGUP §5.1), used here so the test
 * speaks in real events rather than invented ones. */
#define A_PRESS 0x9D
#define A_RELEASE 0x1D
#define W_PRESS 0xA7
#define SHIFT_PRESS 0x87
#define SHIFT_RELEASE 0x07

/* The driver's unconditional sweep interval. Mirrored rather than exported:
 * the tests below need to step across sweep boundaries, and a test that has
 * to be told the number is better than a header that exposes it. */
#define SWEEP_US 500000ULL

int main(void)
{
    struct kbd_bus bus;
    struct fake f;
    struct kbd_tca8418 k;
    int rc;
    unsigned i;

    /* ---- 1. the init sequence ------------------------------------------ */

    fake_init(&f, &bus);
    rc = kbd_tca8418_init(&k, &bus, 0);
    check("a controller that answers is present", rc == 1);
    check("and the driver is ready", kbd_tca8418_ready(&k));
    check("the reset line was pulsed", f.resets == 1);
    check("the bus was released after init", f.claimed == 0);
    check("no register was touched with the bus released", !f.read_outside);

    {
        static const struct { uint8_t reg; uint8_t val; } want[] = {
            { 0x1D, 0x7F }, { 0x1E, 0xFF }, { 0x1F, 0x03 },
            { 0x29, 0x00 }, { 0x2A, 0x00 }, { 0x2B, 0x00 },
            { 0x01, 0x29 }, { 0x02, 0x09 },
        };
        int ok = f.wlog_n == sizeof(want) / sizeof(want[0]);

        for (i = 0; ok && i < f.wlog_n; i++) {
            ok = f.wlog[i].reg == want[i].reg && f.wlog[i].val == want[i].val;
        }
        check("init writes exactly the documented sequence, in order", ok);
    }

    /* ---- 2. absence is not an error ------------------------------------- */

    fake_init(&f, &bus);
    f.fail_read = 0x03; /* the presence probe does not answer */
    rc = kbd_tca8418_init(&k, &bus, 0);
    check("a controller that does not answer is absent", rc == 0);
    check("absent is not ready", !kbd_tca8418_ready(&k));
    check("and nothing was configured", f.wlog_n == 0);
    check("and the bus was still released", f.releases == f.claims);

    fake_init(&f, &bus);
    f.fail_write = 0x1E; /* the matrix configuration fails half way */
    rc = kbd_tca8418_init(&k, &bus, 0);
    check("a configuration failure leaves the driver absent", rc == 0);
    check("and releases the bus", f.releases == f.claims);

    fake_init(&f, &bus);
    f.reset_rc = -1;
    rc = kbd_tca8418_init(&k, &bus, 0);
    check("a reset that fails leaves the driver absent", rc == 0);

    /* The start-up FIFO flush and the status clear that ends it (cold review
     * F5). Their results used to be discarded, so a bus that died between the
     * last configuration write and the flush produced a driver that called
     * itself ready over a controller still holding whatever the vendor
     * launcher left in its FIFO - and the comment over configure() claiming
     * every return value is checked was not true. */
    fake_init(&f, &bus);
    f.fail_read = 0x04; /* the FIFO read the flush uses */
    rc = kbd_tca8418_init(&k, &bus, 0);
    check("a flush that cannot read the FIFO leaves the driver absent", rc == 0);
    check("and absent is not ready", !kbd_tca8418_ready(&k));
    check("and the bus was still released", f.releases == f.claims);

    fake_init(&f, &bus);
    f.fail_write = 0x02; /* the status clear at the end of the flush */
    rc = kbd_tca8418_init(&k, &bus, 0);
    check("a flush whose status clear fails leaves the driver absent", rc == 0);
    check("and that bus was released too", f.releases == f.claims);

    /* ---- 3. draining ---------------------------------------------------- */

    fake_init(&f, &bus);
    (void)kbd_tca8418_init(&k, &bus, 0);
    seen_reset();
    fifo_push(&f, SHIFT_PRESS);
    fifo_push(&f, A_PRESS);
    fifo_push(&f, A_RELEASE);
    rc = kbd_tca8418_poll(&k, 0, on_event, NULL, NULL);
    check("every queued event is delivered", rc == 3 && seen_n == 3);
    check("the raw bytes arrive untouched",
          seen[0] == SHIFT_PRESS && seen[1] == A_PRESS && seen[2] == A_RELEASE);
    check("the bus is released after a drain", f.claimed == 0);
    check("the status register is cleared with 0x0D",
          f.wlog_n > 0 && f.wlog[f.wlog_n - 1].reg == 0x02 &&
              f.wlog[f.wlog_n - 1].val == 0x0D);

    /* A zero byte means the FIFO is empty, whatever the count register
     * claims; the vendor's own drain ends the same way. */
    fake_init(&f, &bus);
    (void)kbd_tca8418_init(&k, &bus, 0);
    seen_reset();
    fifo_push(&f, A_PRESS);
    fifo_push(&f, A_RELEASE);
    f.reg[0x03] = 0x0F; /* ignored: the fake answers from the queue */
    rc = kbd_tca8418_poll(&k, 0, on_event, NULL, NULL);
    check("a zero byte ends the drain", rc == 2 && seen_n == 2);

    /* The cap bounds how long one poll may hold the UI thread. */
    fake_init(&f, &bus);
    (void)kbd_tca8418_init(&k, &bus, 0);
    seen_reset();
    for (i = 0; i < 30; i++) {
        fifo_push(&f, A_PRESS);
    }
    rc = kbd_tca8418_poll(&k, 0, on_event, NULL, NULL);
    check("one poll delivers no more than the cap",
          rc > 0 && rc <= KBD_TCA8418_DRAIN_MAX);
    check("and the rest waits for the next poll", fifo_left(&f) > 0);

    /* ---- 4. codes the matrix cannot produce ----------------------------- */

    fake_init(&f, &bus);
    (void)kbd_tca8418_init(&k, &bus, 0);
    seen_reset();
    fifo_push(&f, 0xFF);    /* code 127, past the matrix */
    fifo_push(&f, A_PRESS); /* and a real one behind it */
    rc = kbd_tca8418_poll(&k, 0, on_event, NULL, NULL);
    check("a code past the matrix is not delivered", seen_n == 1);
    check("but the drain continues past it", seen[0] == A_PRESS);
    check("and it is counted", k.unknown_count == 1);

    /* ---- 5. overflow ---------------------------------------------------- */

    fake_init(&f, &bus);
    (void)kbd_tca8418_init(&k, &bus, 0);
    seen_reset();
    f.reg[0x02] = 0x08; /* OVR_FLOW_INT */
    fifo_push(&f, W_PRESS);
    rc = kbd_tca8418_poll(&k, 0, on_event, NULL, NULL);
    check("an overflow is reported once", kbd_tca8418_take_overflow(&k));
    check("and not twice", !kbd_tca8418_take_overflow(&k));
    check("the drain continues through an overflow", rc == 1 && seen_n == 1);
    check("and the overflow is counted", k.overflow_count == 1);

    /* An ordinary drain must not report an overflow at all. This is what the
     * fake's write-1-to-clear INT_STAT buys: while it stored what was
     * written, the driver's own clear left OVR_FLOW_INT standing and every
     * drain reported an overflow, so the cases here and in shell_kbd_test
     * could not fail. */
    fake_init(&f, &bus);
    (void)kbd_tca8418_init(&k, &bus, 0);
    seen_reset();
    for (i = 0; i < 6; i++) {
        fifo_push(&f, A_PRESS);
        (void)kbd_tca8418_poll(&k, (uint64_t)(i + 1) * 15000, on_event, NULL,
                               NULL);
        check("a drain with no overflow reports none",
              !kbd_tca8418_take_overflow(&k));
    }
    check("and none was counted", k.overflow_count == 0);

    /* The ordering the caller depends on. CFG sets OVR_FLOW_M, so the events
     * that survive an overflow are the *newest* and the ones thrown away are
     * the older ones that would have released a held modifier. The caller is
     * therefore told to forget its modifier state before it is handed a
     * single event of that batch - not after the poll returns, which is one
     * batch too late and is exactly how a dropped Shift release used to turn
     * the survivors into symbols. */
    fake_init(&f, &bus);
    (void)kbd_tca8418_init(&k, &bus, 0);
    f.reg[0x02] = 0x08; /* OVR_FLOW_INT */
    fifo_push(&f, SHIFT_RELEASE);
    fifo_push(&f, W_PRESS);
    order_reset();
    (void)kbd_tca8418_poll(&k, 0, order_on_event, order_on_overflow, NULL);
    check("the overflow is announced before the batch it invalidated",
          strcmp(order, "OEE") == 0);

    /* Without an overflow there is no announcement to order. */
    fake_init(&f, &bus);
    (void)kbd_tca8418_init(&k, &bus, 0);
    fifo_push(&f, W_PRESS);
    order_reset();
    (void)kbd_tca8418_poll(&k, 0, order_on_event, order_on_overflow, NULL);
    check("and is not announced when nothing overflowed",
          strcmp(order, "E") == 0);

    /* ---- 6. failures and the retry throttle ----------------------------- */

    fake_init(&f, &bus);
    (void)kbd_tca8418_init(&k, &bus, 0);
    f.fail_read = 0x03; /* the count register stops answering */
    rc = kbd_tca8418_poll(&k, 1000, on_event, NULL, NULL);
    check("a failed transaction is reported", rc == -1);
    check("and the driver stops claiming to be ready", !kbd_tca8418_ready(&k));
    check("and the bus was released anyway", f.claimed == 0);

    {
        unsigned claims_before = f.claims;

        rc = kbd_tca8418_poll(&k, 1000 + 500000ULL, on_event, NULL, NULL);
        check("a poll inside the throttle does nothing at all",
              rc == 0 && f.claims == claims_before);

        f.fail_read = -1; /* the keyboard comes back */
        rc = kbd_tca8418_poll(&k, 1000 + 2000000ULL, on_event, NULL, NULL);
        check("after the throttle it is reconfigured", f.claims > claims_before);
        check("and it is ready again", kbd_tca8418_ready(&k));
    }

    /* A read that fails part way through keeps what it already read. */
    fake_init(&f, &bus);
    (void)kbd_tca8418_init(&k, &bus, 0);
    seen_reset();
    f.fail_read = 0x04;
    fifo_push(&f, A_PRESS);
    rc = kbd_tca8418_poll(&k, 0, on_event, NULL, NULL);
    check("an event read that fails reports the failure", rc == -1);

    /* ---- 7. the INT line as a gate -------------------------------------- */

    fake_init(&f, &bus);
    f.irq = 1; /* idle */
    (void)kbd_tca8418_init(&k, &bus, 0);
    check("the gate is used when the line can be read", kbd_tca8418_gated(&k));
    {
        unsigned claims_before = f.claims;

        rc = kbd_tca8418_poll(&k, 1, on_event, NULL, NULL);
        check("an idle line costs no bus traffic at all",
              rc == 0 && f.claims == claims_before);

        f.irq = 0; /* asserted */
        fifo_push(&f, A_PRESS);
        seen_reset();
        rc = kbd_tca8418_poll(&k, 2, on_event, NULL, NULL);
        check("an asserted line drains", rc == 1 && seen_n == 1);
    }

    /* A line stuck low asks for a drain that finds nothing, every tick. The
     * gate switches itself off rather than paying that for ever. */
    fake_init(&f, &bus);
    f.irq = 0;
    (void)kbd_tca8418_init(&k, &bus, 0);
    for (i = 0; i < 10; i++) {
        (void)kbd_tca8418_poll(&k, 10 + i, on_event, NULL, NULL);
    }
    check("a line that lies about pending events stops being trusted",
          !kbd_tca8418_gated(&k));

    /* The regression this section exists for. A sweep drains whatever the
     * line says, so a sweep will sooner or later coincide with a key the
     * owner really did press - and that is the ordinary case, not evidence
     * of anything. The gate must survive it. Before the fix the sweep drew
     * no distinction and switched itself off at the first such coincidence,
     * which on the bench meant within about half a second of typing, after
     * which every tick paid a full I2C drain for the rest of the session. */
    fake_init(&f, &bus);
    f.irq = 0; /* asserted, and telling the truth: there really is an event */
    (void)kbd_tca8418_init(&k, &bus, 0);
    seen_reset();
    fifo_push(&f, A_PRESS);
    rc = kbd_tca8418_poll(&k, 600000, on_event, NULL, NULL); /* past the sweep */
    check("a sweep that coincides with a real key still drains it",
          rc == 1 && seen_n == 1);
    check("and an honest line is still trusted afterwards",
          kbd_tca8418_gated(&k));

    /* The same again, sustained: a truthful line under continuous typing
     * crosses many sweep boundaries with events pending and must keep the
     * gate through all of them. */
    fake_init(&f, &bus);
    f.irq = 1;
    (void)kbd_tca8418_init(&k, &bus, 0);
    for (i = 0; i < 400; i++) {
        uint64_t now = (uint64_t)(i + 1) * 15000; /* the shell's poll period */

        /* A key waiting at every single poll, so that every sweep boundary
         * is crossed with events pending - the coincidence itself, made
         * certain rather than left to the arithmetic of the two periods.
         * The fake's queue is recycled once drained so it stays bounded. */
        if (f.fifo_i == f.fifo_n) {
            f.fifo_i = f.fifo_n = 0;
        }
        fifo_push(&f, A_PRESS);
        f.irq = fifo_left(&f) > 0 ? 0 : 1; /* the line never lies */
        (void)kbd_tca8418_poll(&k, now, on_event, NULL, NULL);
    }
    check("six seconds of typing across twelve sweeps does not drop the gate",
          kbd_tca8418_gated(&k));

    /* A line stuck high would hide every event, so the unconditional sweep
     * finds them and the gate is dropped. It takes more than one sweep now:
     * a single one cannot tell a stuck line from the coincidence above, and
     * a line that is genuinely stuck never reads asserted, so it never gets
     * the reprieve that clears the count. */
    fake_init(&f, &bus);
    f.irq = 1; /* idle, and lying: there are events waiting */
    (void)kbd_tca8418_init(&k, &bus, 0);
    seen_reset();
    fifo_push(&f, A_PRESS);
    rc = kbd_tca8418_poll(&k, 600000, on_event, NULL, NULL);
    check("the sweep finds events the gate hid", rc == 1 && seen_n == 1);
    check("one such sweep alone is not yet a verdict", kbd_tca8418_gated(&k));
    for (i = 0; i < 4; i++) {
        fifo_push(&f, A_PRESS);
        (void)kbd_tca8418_poll(&k, 600000 + (uint64_t)(i + 1) * SWEEP_US,
                               on_event, NULL, NULL);
    }
    check("but a line that keeps hiding events is dropped",
          !kbd_tca8418_gated(&k));

    /* No line at all is not an error: poll unconditionally instead. */
    fake_init(&f, &bus);
    f.irq = -1;
    (void)kbd_tca8418_init(&k, &bus, 0);
    check("no usable INT line means no gate", !kbd_tca8418_gated(&k));
    seen_reset();
    fifo_push(&f, A_PRESS);
    rc = kbd_tca8418_poll(&k, 1, on_event, NULL, NULL);
    check("and the keyboard still works", rc == 1 && seen_n == 1);

    /* ---- 8. a bus that cannot be claimed -------------------------------- */

    fake_init(&f, &bus);
    (void)kbd_tca8418_init(&k, &bus, 0);
    f.claim_rc = -1;
    rc = kbd_tca8418_poll(&k, 1, on_event, NULL, NULL);
    check("a bus that cannot be claimed is a failure", rc == -1);
    check("and is retried rather than repeated", !kbd_tca8418_ready(&k));

    /* ---- 9. an absent keyboard never polls the bus ---------------------- */

    fake_init(&f, &bus);
    f.fail_read = 0x03;
    (void)kbd_tca8418_init(&k, &bus, 0);
    {
        unsigned claims_before = f.claims;

        for (i = 0; i < 50; i++) {
            (void)kbd_tca8418_poll(&k, 1000000ULL * i, on_event, NULL, NULL);
        }
        check("an absent keyboard costs nothing for ever",
              f.claims == claims_before);
    }

    printf("kbd_tca8418_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
