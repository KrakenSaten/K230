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
#include <time.h>

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
    int bus_gone;   /* the base was unplugged: nothing can be claimed */
    unsigned resets;
    unsigned claim_fails;
} chip;

static int fake_claim(void *ctx)
{
    (void)ctx;
    if (chip.bus_gone) {
        chip.claim_fails++;
        return -1;
    }
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
    if (reg == 0x02) {
        /* INT_STAT is write-1-to-clear. Storing what was written instead
         * left OVR_FLOW_INT standing after the driver's own clear, so every
         * drain reported an overflow and the Shift case below passed without
         * ever exercising the overflow it injected. */
        chip.reg[reg] = (uint8_t)(chip.reg[reg] & ~value);
        return 0;
    }
    chip.reg[reg] = value;
    return 0;
}

static int fake_reset(void *ctx)
{
    (void)ctx;
    if (chip.bus_gone) {
        return -1;
    }
    /* A real reset empties the controller, which is exactly why anything
     * believed about held modifiers is worthless afterwards. */
    chip.resets++;
    chip.n = 0;
    chip.i = 0;
    memset(chip.reg, 0, sizeof(chip.reg));
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

/* The driver's unconditional sweep and its retry backoff are both measured on
 * CLOCK_MONOTONIC, not on LVGL ticks, so lv_tick_inc() cannot reach either:
 * these waits take as long as the driver really takes. Returns 1 once the
 * counter has moved, 0 if it never did. */
static int wait_for_count(const unsigned *counter, unsigned before, int max_50ms)
{
    struct timespec ts = { 0, 50 * 1000 * 1000L };
    int i;

    for (i = 0; i < max_50ms && *counter == before; i++) {
        nanosleep(&ts, NULL);
        settle();
    }
    return *counter > before;
}

/* Raw bytes measured on unit A. */
#define W_PRESS 0xA7
#define W_RELEASE 0x27
#define SHIFT_PRESS 0x87
#define SHIFT_RELEASE 0x07
#define F1_PRESS 0xB2 /* code 50, reserved rather than mapped */
#define CTRL_PRESS 0x97 /* code 23 (CTRL) pressed; its release never arrives */
#define CTRL_RELEASE 0x17
#define C_PRESS 0x90 /* code 16 */
#define C_RELEASE 0x10
#define UP_PRESS 0x96  /* code 22 */
#define TAB_PRESS 0x86 /* code 6 */

/* What a raw key target (the Terminal) received. */
static pos_key_t raw_keys[8];
static unsigned raw_mods[8];
static int raw_count;

static void on_raw_key(lv_event_t *e)
{
    const uint32_t *v = lv_event_get_param(e);
    pos_key_t key;
    unsigned mods;

    if (v && raw_count < 8 && pos_input_raw_decode(*v, &key, &mods)) {
        raw_keys[raw_count] = key;
        raw_mods[raw_count] = mods;
        raw_count++;
    }
}

int main(void)
{
    lv_display_t *disp;
    lv_obj_t *screen;
    lv_obj_t *field;
    unsigned resets_before;
    unsigned fails_before;

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

    /* ---- 5. an overflow drops what is believed about Shift -------------- *
     *
     * The controller's CFG sets OVR_FLOW_M, so a full FIFO overwrites its
     * oldest entries: what survives is the newest events and what was thrown
     * away is older - which is where a Shift release goes missing. The batch
     * that arrives carrying the overflow flag must therefore be translated
     * with the modifier state already dropped, not with the stale one.
     *
     * Both events are fed before settling so they land in a single drain,
     * which is the case that matters: the reset has to happen inside that
     * drain, ahead of the events, rather than after the poll returns. With
     * the reset one batch late this reads "w__" - the W arrives shifted,
     * because a Shift the controller never released is still believed. */

    feed(SHIFT_PRESS);
    settle();
    check_str("Shift is held and nothing has been typed by it",
              lv_textarea_get_text(field), "w_");
    chip.reg[0x02] = 0x08; /* OVR_FLOW_INT: the controller lost events, and
                            * the Shift release was among them */
    feed(W_PRESS);
    settle();
    check_str("the batch arriving with an overflow is not shifted",
              lv_textarea_get_text(field), "w_w");

    /* And the state really was dropped rather than merely skipped once: the
     * next key is unshifted too, with no release ever delivered. */
    feed(W_PRESS);
    feed(W_RELEASE);
    settle();
    check_str("and Shift stays dropped afterwards",
              lv_textarea_get_text(field), "w_ww");

    /* ---- 5b. a modifier release lost to a bus failure (cold review F5) --
     *
     * The bus goes away with Shift held. The release happens while it is
     * gone, so the controller never reports it and, when it comes back, it
     * has been reset and its FIFO flushed: there is no release left to
     * deliver and never will be. Anything this layer still believes about
     * Shift is therefore wrong, and would apply the orange legend to
     * everything typed from then on.
     *
     * The recovery also has to be a real one. The driver throttles its
     * retries, so most polls during an outage return 0 without touching the
     * bus; reading that as "answering again" both logged a recovery that had
     * not happened and, once the reset became the thing that clears the
     * modifiers, would have cleared them at the wrong moment. */

    feed(SHIFT_PRESS);
    settle();
    check_str("Shift is held again", lv_textarea_get_text(field), "w_ww");

    chip.bus_gone = 1;          /* the base is unplugged, mid-chord */
    resets_before = chip.resets;
    fails_before = chip.claim_fails;
    /* The INT gate means an idle keyboard never touches the bus, so the
     * outage is not noticed until the next unconditional sweep. */
    check("the outage is noticed on the next sweep",
          wait_for_count(&chip.claim_fails, fails_before, 60));
    check("and nothing was reset while the bus was gone",
          chip.resets == resets_before);

    chip.bus_gone = 0;          /* plugged back in */
    check("the controller was reset and reconfigured on recovery",
          wait_for_count(&chip.resets, resets_before, 160));

    /* The Shift release was never delivered and never will be. */
    feed(W_PRESS);
    feed(W_RELEASE);
    settle();
    check_str("a key typed after recovery is not stuck shifted",
              lv_textarea_get_text(field), "w_www");

    /* Ctrl and Alt are the same key map state, so the same loss applies to
     * them: a chord broken by the outage must not leave the field taking
     * control characters instead of letters. */
    feed(CTRL_PRESS);
    settle();
    chip.bus_gone = 1;
    resets_before = chip.resets;
    fails_before = chip.claim_fails;
    check("the second outage is noticed too",
          wait_for_count(&chip.claim_fails, fails_before, 60));
    chip.bus_gone = 0;
    check("the controller recovered from the second outage too",
          wait_for_count(&chip.resets, resets_before, 160));
    feed(W_PRESS);
    feed(W_RELEASE);
    settle();
    check_str("and Ctrl is not stuck either",
              lv_textarea_get_text(field), "w_wwww");

    /* ---- 5c. the modifiers go with the key to a raw target --------------
     *
     * The Terminal (pos_input.h, raw key target) needs Ctrl+C as a chord,
     * Tab as a key and Shift+Up as Shift and Up. The driver sends the
     * modifiers it holds with each key; an ordinary field never sees them. */
    {
        lv_obj_t *raw = lv_obj_create(screen);

        pos_input_add_obj(raw);
        pos_input_focus(raw);
        pos_input_set_raw_target(raw);
        lv_obj_add_event_cb(raw, on_raw_key, LV_EVENT_KEY, NULL);
        settle();

        feed(CTRL_PRESS);
        feed(C_PRESS);
        feed(C_RELEASE);
        feed(CTRL_RELEASE);
        settle();
        check("Ctrl+C reaches a raw target as c with Ctrl",
              raw_count == 1 && raw_keys[0] == 'c' && raw_mods[0] == POS_INPUT_MOD_CTRL);
        feed(SHIFT_PRESS);
        feed(UP_PRESS);
        feed(SHIFT_RELEASE);
        feed(TAB_PRESS);
        settle();
        check("Shift+Up as Up with Shift, then Tab as a key with nothing held",
              raw_count == 3 && raw_keys[1] == LV_KEY_UP && raw_mods[1] == POS_INPUT_MOD_SHIFT &&
              raw_keys[2] == LV_KEY_NEXT && raw_mods[2] == 0);
        check("and Tab did not move the focus", pos_input_focused() == raw);

        pos_input_set_raw_target(NULL);
        lv_obj_delete(raw);
        pos_input_focus(field);
        settle();
        feed(CTRL_PRESS);
        feed(W_PRESS);
        feed(W_RELEASE);
        feed(CTRL_RELEASE);
        settle();
        check_str("a field still gets the letter for a Ctrl chord",
                  lv_textarea_get_text(field), "w_wwwww");
    }

    /* ---- 6. destroy stops the keyboard --------------------------------- */

    shell_kbd_destroy();
    feed(W_PRESS);
    feed(W_RELEASE);
    settle();
    check_str("nothing arrives once the keyboard is destroyed",
              lv_textarea_get_text(field), "w_wwwww");

    printf("shell_kbd_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
