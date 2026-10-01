/*
 * Keyboard indicator LED test (ui/shell/kbd_leds.c) against a model of the
 * XL9555 expander on a fake bus: the pins that are touched and the ones that
 * are not, active-low, off-before-output, writes only on change, a failing
 * expander, recovery, and the exit path leaving nothing lit.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kbd_leds.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

/* ---- an XL9555 at 0x20 ----------------------------------------------------- */

struct xl {
    uint8_t reg[8];       /* in0 in1 out0 out1 pol0 pol1 cfg0 cfg1 */
    bool present;
    bool claimed;
    int claims, releases;
    int writes;           /* register writes that landed */
    int fail_writes_after; /* -1: never; else fail every write once this many landed */
    bool fail_claim;
    /* The order of events, for the off-before-output rule. */
    int out_written_at, cfg_written_at, step;
    uint8_t other_addr_touched;
};

static struct xl x;

static int claim(void *ctx)
{
    (void)ctx;
    if (x.fail_claim) {
        return -1;
    }
    x.claims++;
    x.claimed = true;
    return 0;
}

static void release(void *ctx)
{
    (void)ctx;
    x.releases++;
    x.claimed = false;
}

static int rd_at(void *ctx, uint8_t addr, uint8_t reg, uint8_t *v)
{
    (void)ctx;
    if (!x.claimed) {
        return -1;
    }
    if (addr != KBD_LEDS_XL9555_ADDR) {
        x.other_addr_touched = addr;
        return -1;
    }
    if (!x.present || reg > 7) {
        return -1;
    }
    *v = x.reg[reg];
    return 0;
}

static int wr_at(void *ctx, uint8_t addr, uint8_t reg, uint8_t v)
{
    (void)ctx;
    if (!x.claimed) {
        return -1;
    }
    if (addr != KBD_LEDS_XL9555_ADDR) {
        x.other_addr_touched = addr;
        return -1;
    }
    if (!x.present || reg > 7) {
        return -1;
    }
    if (x.fail_writes_after >= 0 && x.writes >= x.fail_writes_after) {
        return -1;
    }
    x.reg[reg] = v;
    x.writes++;
    x.step++;
    if (reg == KBD_LEDS_REG_OUTPUT0) {
        x.out_written_at = x.step;
    }
    if (reg == KBD_LEDS_REG_CONFIG0) {
        x.cfg_written_at = x.step;
    }
    return 0;
}

static const struct kbd_bus bus = {
    .claim = claim,
    .release = release,
    .read_reg_at = rd_at,
    .write_reg_at = wr_at,
};

static void reset(void)
{
    memset(&x, 0, sizeof(x));
    x.present = true;
    x.fail_writes_after = -1;
    /* Power-on state: every pin an input (cfg 0xFF), outputs latched high.
     * P00 and P07 carry something else on some base: an output driven low
     * and one driven high, which the LED code must leave exactly so. */
    x.reg[KBD_LEDS_REG_OUTPUT0] = 0xFE;
    x.reg[KBD_LEDS_REG_CONFIG0] = 0x7E; /* P00 and P07 outputs */
    x.reg[3] = 0xAA;
    x.reg[7] = 0x55;
}

/* Is the LED on that pin lit? Active low, and only if the pin is an output. */
static bool lit(uint8_t pin)
{
    return !(x.reg[KBD_LEDS_REG_CONFIG0] & pin) && !(x.reg[KBD_LEDS_REG_OUTPUT0] & pin);
}

static bool others_untouched(void)
{
    return (x.reg[KBD_LEDS_REG_OUTPUT0] & 0xC7) == (0xFE & 0xC7) &&
           (x.reg[KBD_LEDS_REG_CONFIG0] & 0xC7) == (0x7E & 0xC7) && x.reg[3] == 0xAA && x.reg[7] == 0x55;
}

static bool balanced(void)
{
    return x.claims == x.releases && !x.claimed;
}

int main(void)
{
    struct kbd_leds l;
    struct kbd_bus nobus;
    int k;
    int before;

    /* The pure port arithmetic. */
    check("all off: three pins high", kbd_leds_port_value(0x00, 0) == KBD_LEDS_PINS);
    check("caps: P03 low", kbd_leds_port_value(0xFF, KBD_LED_CAPS) == (uint8_t)(0xFF & ~KBD_LEDS_PIN_CAPS));
    check("mic: P04 low", kbd_leds_port_value(0xFF, KBD_LED_MIC) == (uint8_t)(0xFF & ~KBD_LEDS_PIN_MIC));
    check("camera: P05 low", kbd_leds_port_value(0xFF, KBD_LED_CAMERA) ==
                                 (uint8_t)(0xFF & ~KBD_LEDS_PIN_CAMERA));
    check("the other bits pass through", (kbd_leds_port_value(0x81, KBD_LED_ALL) & 0xC7) == 0x81);

    /* Bring-up. */
    reset();
    memset(&l, 0, sizeof(l));
    check("init answers", kbd_leds_init(&l, &bus) == 1 && l.ready);
    check("all three are outputs",
          !(x.reg[KBD_LEDS_REG_CONFIG0] & KBD_LEDS_PINS));
    check("and all three dark", !lit(KBD_LEDS_PIN_CAPS) && !lit(KBD_LEDS_PIN_MIC) && !lit(KBD_LEDS_PIN_CAMERA));
    check("off was written before the pins became outputs", x.out_written_at < x.cfg_written_at);
    check("nothing else on the expander moved", others_untouched());
    check("only 0x20 was spoken to", x.other_addr_touched == 0);
    check("every claim released", balanced());

    /* Caps, microphone, camera - independently. */
    check("caps on", kbd_leds_set(&l, KBD_LED_CAPS) == 0 && lit(KBD_LEDS_PIN_CAPS) && !lit(KBD_LEDS_PIN_MIC) &&
                         !lit(KBD_LEDS_PIN_CAMERA));
    check("caps off", kbd_leds_set(&l, 0) == 0 && !lit(KBD_LEDS_PIN_CAPS));
    check("mic on alone", kbd_leds_set(&l, KBD_LED_MIC) == 0 && lit(KBD_LEDS_PIN_MIC) &&
                              !lit(KBD_LEDS_PIN_CAPS) && !lit(KBD_LEDS_PIN_CAMERA));
    check("camera joins", kbd_leds_set(&l, KBD_LED_MIC | KBD_LED_CAMERA) == 0 && lit(KBD_LEDS_PIN_MIC) &&
                              lit(KBD_LEDS_PIN_CAMERA));
    check("mic released, camera stays", kbd_leds_set(&l, KBD_LED_CAMERA) == 0 && !lit(KBD_LEDS_PIN_MIC) &&
                                            lit(KBD_LEDS_PIN_CAMERA));
    check("shown mask follows", kbd_leds_shown(&l) == KBD_LED_CAMERA);
    check("still nothing else moved", others_untouched());

    /* No bus traffic for a state already shown. */
    before = x.claims;
    for (k = 0; k < 50; k++) {
        kbd_leds_set(&l, KBD_LED_CAMERA);
    }
    check("the same state 50 times: no bus traffic", x.claims == before);

    /* Repeated open and close leaves them right. */
    for (k = 0; k < 100; k++) {
        kbd_leds_set(&l, (k & 1) ? KBD_LED_MIC : 0);
        kbd_leds_set(&l, (k & 1) ? KBD_LED_MIC | KBD_LED_CAMERA : KBD_LED_CAPS);
    }
    kbd_leds_set(&l, 0);
    check("100 cycles end dark", !lit(KBD_LEDS_PIN_CAPS) && !lit(KBD_LEDS_PIN_MIC) && !lit(KBD_LEDS_PIN_CAMERA));
    check("and balanced", balanced() && others_untouched());

    /* An expander that stops answering: not ready, no pretending. */
    kbd_leds_set(&l, KBD_LED_MIC);
    x.fail_writes_after = x.writes;
    check("a failed write reports failure", kbd_leds_set(&l, KBD_LED_MIC | KBD_LED_CAMERA) == -1);
    check("and the LEDs are no longer ready", !l.ready && kbd_leds_shown(&l) == 0 && l.failures == 1);
    check("the claim was still released", balanced());
    check("further sets refuse without touching the bus", kbd_leds_set(&l, 0) == -1);
    /* It comes back: init writes everything from scratch, all dark. */
    x.fail_writes_after = -1;
    check("re-init after recovery", kbd_leds_init(&l, &bus) == 1);
    check("recovery starts dark, the stale mic LED out",
          !lit(KBD_LEDS_PIN_MIC) && !lit(KBD_LEDS_PIN_CAMERA) && !lit(KBD_LEDS_PIN_CAPS));

    /* The bench's read-back is the expander's own word. */
    {
        uint8_t out = 0;
        uint8_t cfg = 0;

        kbd_leds_set(&l, KBD_LED_CAMERA);
        check("read-back answers", kbd_leds_read_port(&l, &out, &cfg) == 0);
        check("and shows P05 low, P03/P04 high, all three outputs",
              (out & KBD_LEDS_PINS) == (KBD_LEDS_PIN_CAPS | KBD_LEDS_PIN_MIC) && !(cfg & KBD_LEDS_PINS));
        check("read-back balanced", balanced());
    }

    /* The exit path. */
    kbd_leds_set(&l, KBD_LED_ALL);
    kbd_leds_off(&l);
    check("exit: all three dark", !lit(KBD_LEDS_PIN_CAPS) && !lit(KBD_LEDS_PIN_MIC) && !lit(KBD_LEDS_PIN_CAMERA));
    check("exit: forgotten", !l.ready && kbd_leds_shown(&l) == 0);
    check("exit: balanced", balanced());

    /* A base without the expander, and a bus that cannot reach one. */
    reset();
    x.present = false;
    check("no expander: init says so", kbd_leds_init(&l, &bus) == 0 && !l.ready);
    check("no expander: set refuses", kbd_leds_set(&l, KBD_LED_CAPS) == -1);
    check("no expander: balanced", balanced());
    memset(&nobus, 0, sizeof(nobus));
    nobus.claim = claim;
    nobus.release = release;
    check("a bus without addressed access has no LEDs", kbd_leds_init(&l, &nobus) == 0);
    reset();
    x.fail_claim = true;
    check("a bus that cannot be claimed has no LEDs", kbd_leds_init(&l, &bus) == 0);
    kbd_leds_off(NULL);
    check("NULL is harmless", kbd_leds_set(NULL, 1) == 0 && kbd_leds_shown(NULL) == 0);

    printf("kbd_leds_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
