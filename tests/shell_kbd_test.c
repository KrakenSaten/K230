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
#include "hw_actions.h"
#include "kbd_bus.h"
#include "kbd_leds.h"
#include "pocketui.h"
#include "pos_input.h"
#include "pos_keymap.h"
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

/* The XL9555 behind the indicator LEDs, on the same fake bus. */
static uint8_t xl[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0xFF, 0xFF };

static int fake_read_at(void *ctx, uint8_t addr, uint8_t reg, uint8_t *value)
{
    (void)ctx;
    if (!chip.claimed || addr != 0x20 || reg > 7) {
        return -1;
    }
    *value = xl[reg];
    return 0;
}

static int fake_write_at(void *ctx, uint8_t addr, uint8_t reg, uint8_t value)
{
    (void)ctx;
    if (!chip.claimed || addr != 0x20 || reg > 7) {
        return -1;
    }
    xl[reg] = value;
    return 0;
}

/* Lit: an output driven low (active low). */
static int led_lit(uint8_t pin)
{
    return !(xl[6] & pin) && !(xl[2] & pin);
}

static const struct kbd_bus fake_bus = {
    fake_claim, fake_release, fake_read, fake_write, fake_reset, fake_irq, NULL,
    fake_read_at, fake_write_at
};

static void feed(uint8_t raw)
{
    /* A drained FIFO starts again at the front, as the real one does; the
     * array would otherwise fill up over a long test and drop events. */
    if (chip.i == chip.n) {
        chip.n = 0;
        chip.i = 0;
    }
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

#define FN_PRESS 0x89     /* code 9 */
#define FN_RELEASE 0x09
#define F1_RELEASE 0x32
#define F2_PRESS 0xBC     /* code 60 */
#define F5_PRESS 0xC3     /* code 67 */
#define F5_RELEASE 0x43
#define F8_PRESS 0xC0     /* code 64 */
#define F8_RELEASE 0x40
#define MIC_PRESS 0x8B    /* code 11 */
#define MIC_RELEASE 0x0B
#define LILYGO_PRESS 0x88 /* code 8 */
#define LILYGO_RELEASE 0x08
#define CAPS_PRESS 0x8A   /* code 10 */
#define CAPS_RELEASE 0x0A

/* The actions the driver handed on. */
static enum hw_action acted[16];
static int acted_n;

static void on_action(enum hw_action a, void *user)
{
    (void)user;
    if (acted_n < 16) {
        acted[acted_n] = a;
    }
    acted_n++;
}

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

/* The matrix code whose key is named name, or 0. */
static uint8_t code_named(const char *name)
{
    unsigned c;

    for (c = 1; c <= POS_KEYMAP_MAX_CODE; c++) {
        const char *n = pos_keymap_name((uint8_t)c);

        if (n && strcmp(n, name) == 0) {
            return (uint8_t)c;
        }
    }
    return 0;
}

static void tap_code(uint8_t code)
{
    feed((uint8_t)(POS_KEYMAP_EVENT_PRESSED | code));
    feed(code);
}

/* Type UTF-8 text the way a person would on the keyboard base: a-z bare,
 * A-Z under Caps (Shift would give most letters' orange legend: Shift+T is
 * '='), space on the space bar, and æ ø å through the Fn layer (Fn+E, Fn+O,
 * Fn+A), with Shift for the capitals. One character per drain, so the
 * 32-deep FIFO never fills. False for a character with no key. */
static int type_text(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    while (*p) {
        char name[2] = { 0, 0 };
        int shift = 0;
        int caps = 0;
        int fn = 0;
        uint8_t code;

        if (*p == ' ') {
            code = 5; /* one of the two space bars */
            p++;
        } else if (*p >= 'a' && *p <= 'z') {
            name[0] = (char)(*p++ - 'a' + 'A');
            code = code_named(name);
        } else if (*p >= 'A' && *p <= 'Z') {
            name[0] = (char)*p++;
            caps = 1;
            code = code_named(name);
        } else if (p[0] == 0xC3 && p[1] != 0) {
            /* U+00C0..U+00FF: the three the Fn layer has, either case. */
            switch (p[1]) {
            case 0xA5: name[0] = 'A'; break;            /* å */
            case 0x85: name[0] = 'A'; shift = 1; break; /* Å */
            case 0xB8: name[0] = 'O'; break;            /* ø */
            case 0x98: name[0] = 'O'; shift = 1; break; /* Ø */
            case 0xA6: name[0] = 'E'; break;            /* æ */
            case 0x86: name[0] = 'E'; shift = 1; break; /* Æ */
            default: return 0;
            }
            fn = 1;
            code = code_named(name);
            p += 2;
        } else {
            return 0;
        }
        if (code == 0) {
            return 0;
        }
        if (caps) {
            feed(CAPS_PRESS);
            feed(CAPS_RELEASE);
        }
        if (fn) {
            feed(FN_PRESS);
        }
        if (shift) {
            feed(SHIFT_PRESS);
        }
        tap_code(code);
        if (shift) {
            feed(SHIFT_RELEASE);
        }
        if (fn) {
            feed(FN_RELEASE);
        }
        if (caps) {
            feed(CAPS_PRESS);
            feed(CAPS_RELEASE);
        }
        settle();
    }
    return 1;
}

/* Well-formed UTF-8 of at most two bytes a character, which is all a field
 * here can hold: ASCII and U+0080..U+07FF, no overlong forms. */
static int utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    while (*p) {
        if (*p < 0x80) {
            p++;
        } else if (*p >= 0xC2 && *p <= 0xDF && (p[1] & 0xC0) == 0x80) {
            p += 2;
        } else {
            return 0;
        }
    }
    return 1;
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

    /* ---- 5d. the keys that type nothing: actions ------------------------ *
     *
     * The function row, the microphone key and the LILYGO key become the
     * semantic actions of hw_actions.h, handed on after the drain, on the
     * press only. Nothing of them reaches a field. */
    shell_kbd_on_action(on_action, NULL);
    feed(F1_PRESS);
    settle();
    check("F1 hands on Home", acted_n == 1 && acted[0] == HW_ACTION_HOME);
    feed(F1_RELEASE);
    settle();
    check("its release hands on nothing", acted_n == 1);
    feed(F8_PRESS);
    feed(F8_RELEASE);
    feed(MIC_PRESS);
    feed(MIC_RELEASE);
    feed(LILYGO_PRESS);
    feed(LILYGO_RELEASE);
    settle();
    check("F8, mic and LILYGO in one drain: Terminal, Wave, Terminal, in order",
          acted_n == 4 && acted[1] == HW_ACTION_TERMINAL && acted[2] == HW_ACTION_WAVE &&
              acted[3] == HW_ACTION_TERMINAL);
    feed(F8_PRESS); /* held: the controller does not repeat */
    settle();
    settle();
    check("a held key is one action", acted_n == 5);
    feed(F8_RELEASE);
    settle();
    check_str("none of them typed anything", lv_textarea_get_text(field), "w_wwwww");

    /* Ordinary typing still types, and hands on no action. */
    feed(W_PRESS);
    feed(W_RELEASE);
    settle();
    check_str("W still types", lv_textarea_get_text(field), "w_wwwwww");
    check("and is no action", acted_n == 5);

    /* Fn with a function key over a field (no raw target): the shortcut,
     * and no character. */
    feed(FN_PRESS);
    feed(F5_PRESS);
    feed(F5_RELEASE);
    feed(FN_RELEASE);
    settle();
    check("Fn+F5 over a field: the volume shortcut", acted_n == 6 && acted[5] == HW_ACTION_VOLUME_DOWN);
    check_str("and nothing typed", lv_textarea_get_text(field), "w_wwwwww");

    /* A raw-only key can never be typed into a field, whoever pushes it. */
    pos_input_push_key(POS_KEY_F(3));
    settle();
    check_str("a raw F-key pushed at a field is dropped", lv_textarea_get_text(field), "w_wwwwww");

    /* With the Terminal (a raw target) focused, Fn+F5 is the key itself. */
    {
        lv_obj_t *raw = lv_obj_create(screen);

        raw_count = 0;
        pos_input_add_obj(raw);
        pos_input_focus(raw);
        pos_input_set_raw_target(raw);
        lv_obj_add_event_cb(raw, on_raw_key, LV_EVENT_KEY, NULL);
        settle();
        feed(FN_PRESS);
        feed(F5_PRESS);
        feed(F5_RELEASE);
        feed(FN_RELEASE);
        settle();
        check("Fn+F5 at the Terminal: F5 delivered raw", raw_count == 1 && raw_keys[0] == POS_KEY_F(5));
        check("and no action", acted_n == 6);
        feed(F5_PRESS);
        feed(F5_RELEASE);
        settle();
        check("bare F5 at the Terminal: still the shortcut",
              acted_n == 7 && acted[6] == HW_ACTION_VOLUME_DOWN && raw_count == 1);
        feed(CTRL_PRESS);
        feed(C_PRESS);
        feed(C_RELEASE);
        feed(CTRL_RELEASE);
        settle();
        check("Ctrl+C still reaches the Terminal as a chord",
              raw_count == 2 && raw_keys[1] == 'c' && raw_mods[1] == POS_INPUT_MOD_CTRL);
        pos_input_set_raw_target(NULL);
        lv_obj_delete(raw);
        pos_input_focus(field);
        settle();
    }

    /* The bench's path: raw bytes through the same map. */
    {
        const uint8_t f2[2] = { F2_PRESS, 0x3C };
        const uint8_t w[2] = { W_PRESS, W_RELEASE };

        check("inject takes both bytes", shell_kbd_inject(f2, 2) == 2);
        check("an injected F2 hands on Settings", acted_n == 8 && acted[7] == HW_ACTION_SETTINGS);
        shell_kbd_inject(w, 2);
        settle();
        check_str("an injected W types", lv_textarea_get_text(field), "w_wwwwwww");
    }

    /* ---- 5e. the indicator LEDs ------------------------------------------ */
    check("the LEDs came up with the keyboard", xl[6] == (0xFF & ~KBD_LEDS_PINS));
    check("all dark at the start",
          !led_lit(KBD_LEDS_PIN_CAPS) && !led_lit(KBD_LEDS_PIN_MIC) && !led_lit(KBD_LEDS_PIN_CAMERA));
    feed(CAPS_PRESS);
    feed(CAPS_RELEASE);
    settle();
    check("Caps on: its LED lit", led_lit(KBD_LEDS_PIN_CAPS));
    feed(W_PRESS);
    feed(W_RELEASE);
    settle();
    check_str("and W types a capital", lv_textarea_get_text(field), "w_wwwwwwwW");
    feed(CAPS_PRESS);
    feed(CAPS_RELEASE);
    settle();
    check("Caps off: dark", !led_lit(KBD_LEDS_PIN_CAPS));
    shell_kbd_set_indicators(KBD_LED_MIC);
    check("microphone in use: its LED alone",
          led_lit(KBD_LEDS_PIN_MIC) && !led_lit(KBD_LEDS_PIN_CAMERA) && !led_lit(KBD_LEDS_PIN_CAPS));
    shell_kbd_set_indicators(KBD_LED_MIC | KBD_LED_CAMERA);
    check("camera too", led_lit(KBD_LEDS_PIN_MIC) && led_lit(KBD_LEDS_PIN_CAMERA));
    shell_kbd_set_indicators(KBD_LED_CAPS | KBD_LED_CAMERA);
    check("the shell cannot set Caps; the microphone released",
          !led_lit(KBD_LEDS_PIN_CAPS) && !led_lit(KBD_LEDS_PIN_MIC) && led_lit(KBD_LEDS_PIN_CAMERA));
    feed(CAPS_PRESS);
    settle();
    check("Caps on again", led_lit(KBD_LEDS_PIN_CAPS));
    chip.reg[0x02] = 0x08; /* an overflow drops the modifier state, Caps too */
    feed(W_PRESS);
    settle();
    check("an overflow that drops Caps puts its LED out", !led_lit(KBD_LEDS_PIN_CAPS));
    feed(W_RELEASE);
    settle();

    /* ---- 5b. Norwegian letters, as UTF-8, in the field RIFT uses ------- *
     *
     * No keycap carries æ ø å: the Fn layer does (pos_keymap.h). Typed key by
     * key from raw controller bytes into a one-line field - the kind every
     * RIFT text input is - and compared byte for byte, so a code point that
     * reached the field unpacked, or packed twice, cannot pass. */
    {
        lv_obj_t *line = pocketui_text_field(screen, "Message", true);
        static const struct {
            const char *text;
            const char *bytes; /* the same, spelled out as UTF-8 */
            size_t len;
        } cases[] = {
            { "hei på deg", "hei p\xC3\xA5 deg", 11 },
            { "blåbær og øl", "bl\xC3\xA5" "b\xC3\xA6r og \xC3\xB8l", 15 },
            { "ÆØÅ æøå", "\xC3\x86\xC3\x98\xC3\x85 \xC3\xA6\xC3\xB8\xC3\xA5", 13 },
            { "Tromsø Ærlig Åsen", "Troms\xC3\xB8 \xC3\x86rlig \xC3\x85sen", 20 },
        };
        size_t c;

        pos_input_focus(line);
        settle();
        check("the one-line field takes the focus", pos_input_focused() == line);
        for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            const char *got;

            lv_textarea_set_text(line, "");
            check("the case is spelled the same twice", strcmp(cases[c].text, cases[c].bytes) == 0);
            check("typing it sends no key the map does not know", type_text(cases[c].text));
            got = lv_textarea_get_text(line);
            check_str("the field holds it, byte for byte", got, cases[c].bytes);
            check("and its length in bytes is UTF-8's", got && strlen(got) == cases[c].len);
            check("and is valid UTF-8", got && utf8_valid(got));
        }
        /* Caps instead of Shift, and Fn with a key outside the layer. */
        lv_textarea_set_text(line, "");
        feed(CAPS_PRESS);
        feed(CAPS_RELEASE);
        feed(FN_PRESS);
        feed(0x80 | 43); /* O */
        feed(43);
        feed(FN_RELEASE);
        feed(CAPS_PRESS);
        feed(CAPS_RELEASE);
        feed(FN_PRESS);
        feed(W_PRESS);
        feed(W_RELEASE);
        feed(FN_RELEASE);
        settle();
        check_str("Caps+Fn+O is \xC3\x98 and Fn+W is still w", lv_textarea_get_text(line),
                  "\xC3\x98w");
        /* Backspace takes a whole letter, never half of its two bytes. */
        feed(FN_PRESS);
        feed(0x80 | 29); /* A */
        feed(29);
        feed(FN_RELEASE);
        feed(0x80 | 41); /* DEL */
        feed(41);
        settle();
        check_str("Backspace removes all of \xC3\xA5", lv_textarea_get_text(line), "\xC3\x98w");
        feed(0x80 | 41);
        feed(41);
        feed(0x80 | 41);
        feed(41);
        settle();
        check_str("and of \xC3\x98, leaving nothing behind", lv_textarea_get_text(line), "");
        pos_input_focus(field);
        settle();
    }

    /* ---- 6. destroy stops the keyboard --------------------------------- */

    shell_kbd_destroy();
    check("destroy leaves every LED dark",
          !led_lit(KBD_LEDS_PIN_CAPS) && !led_lit(KBD_LEDS_PIN_MIC) && !led_lit(KBD_LEDS_PIN_CAMERA));

    feed(W_PRESS);
    feed(W_RELEASE);
    settle();
    check_str("nothing arrives once the keyboard is destroyed",
              lv_textarea_get_text(field), "w_wwwwwwwWw");

    printf("shell_kbd_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
