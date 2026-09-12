/*
 * The physical keyboard end to end: a raw TCA8418 FIFO byte on one side, a
 * character in a focused text field on the other, through the real poll
 * timer, the real key map and the real pos_input stream. Only the bus is
 * fake, so nothing between the controller and the field is stubbed out.
 *
 * The events are the ones measured on unit A (KEYBOARD_BRINGUP §5.1), which
 * is what makes this test mean something: Shift+W is an underscore because
 * the keycap says so, and this is where that claim reaches a field.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/kbd_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "kbd_bus.h"
#include "pocketui.h"
#include "pos_input.h"
#include "shell_kbd.h"

#include <stdio.h>
#include <string.h>

#define PANEL_W 568
#define PANEL_H 1232

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

static void check_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)",
               want);
    }
}

/* ---- a display that draws nowhere ------------------------------------- */

static uint8_t buf[PANEL_W * 40 * 2];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

/* ---- a controller that exists only here -------------------------------- */

#define FIFO_MAX 32

static struct {
    uint8_t reg[0x40];
    uint8_t fifo[FIFO_MAX];
    unsigned n;
    unsigned i;
    int claimed;
} chip;

static int fake_claim(void *ctx)
{
    (void)ctx;
    chip.claimed = 1;
    return 0;
}

static void fake_release(void *ctx)
{
    (void)ctx;
    chip.claimed = 0;
}

static int fake_read(void *ctx, uint8_t reg, uint8_t *value)
{
    (void)ctx;
    if (reg == 0x03) {
        unsigned left = chip.n - chip.i;

        *value = (uint8_t)(left > 0x0F ? 0x0F : left);
        return 0;
    }
    if (reg == 0x04) {
        *value = chip.i < chip.n ? chip.fifo[chip.i++] : 0;
        return 0;
    }
    *value = chip.reg[reg];
    return 0;
}

static int fake_write(void *ctx, uint8_t reg, uint8_t value)
{
    (void)ctx;
    chip.reg[reg] = value;
    return 0;
}

static int fake_reset(void *ctx)
{
    (void)ctx;
    return 0;
}

static int fake_irq(void *ctx)
{
    (void)ctx;
    return chip.i < chip.n ? 0 : 1; /* asserted while the FIFO has events */
}

static const struct kbd_bus fake_bus = {
    fake_claim, fake_release, fake_read, fake_write, fake_reset, fake_irq, NULL
};

static void feed(uint8_t raw)
{
    if (chip.n < FIFO_MAX) {
        chip.fifo[chip.n++] = raw;
    }
}

/* ---- driving LVGL ------------------------------------------------------ */

static void settle(void)
{
    int i;

    for (i = 0; i < 40; i++) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

/* Raw bytes measured on unit A. */
#define W_PRESS 0xA7
#define W_RELEASE 0x27
#define SHIFT_PRESS 0x87
#define SHIFT_RELEASE 0x07
#define F1_PRESS 0xB2 /* code 50, reserved rather than mapped */

int main(void)
{
    lv_display_t *disp;
    lv_obj_t *screen;
    lv_obj_t *field;

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    pos_input_init();
    pocketui_init();
    screen = lv_screen_active();
    pocketui_style_screen(screen);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    field = pocketui_text_field(screen, "Note", false);
    pos_input_focus(field);
    settle();

    check("the keyboard attaches to a controller that answers",
          shell_kbd_attach(&fake_bus) == 1);
    check("the field starts empty",
          strcmp(lv_textarea_get_text(field), "") == 0);

    /* ---- 1. a key becomes a character --------------------------------- */

    feed(W_PRESS);
    feed(W_RELEASE);
    settle();
    check_str("W types a w", lv_textarea_get_text(field), "w");

    /* ---- 2. Shift reaches the keycap's orange legend ------------------- */

    feed(SHIFT_PRESS);
    feed(W_PRESS);
    feed(W_RELEASE);
    feed(SHIFT_RELEASE);
    settle();
    check_str("Shift+W is the underscore printed on the keycap",
              lv_textarea_get_text(field), "w_");

    /* ---- 3. a release delivers nothing of its own ---------------------- */

    feed(W_RELEASE);
    settle();
    check_str("a bare release types nothing", lv_textarea_get_text(field), "w_");

    /* ---- 4. the function row is reserved, not guessed at ---------------- */

    feed(F1_PRESS);
    settle();
    check_str("F1 types nothing", lv_textarea_get_text(field), "w_");

    /* ---- 5. an overflow drops what is believed about Shift -------------- */

    feed(SHIFT_PRESS);
    settle();
    chip.reg[0x02] = 0x08; /* OVR_FLOW_INT: the controller lost events */
    feed(W_PRESS);
    settle();
    chip.reg[0x02] = 0x00;
    check_str("after an overflow a held Shift is no longer believed",
              lv_textarea_get_text(field), "w_w");

    /* ---- 6. destroy stops the keyboard --------------------------------- */

    shell_kbd_destroy();
    feed(W_PRESS);
    feed(W_RELEASE);
    settle();
    check_str("nothing arrives once the keyboard is destroyed",
              lv_textarea_get_text(field), "w_w");

    printf("shell_kbd_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
