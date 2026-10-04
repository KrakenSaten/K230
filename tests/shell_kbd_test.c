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
#include "kbd_picker.h"
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

/* ---- the long-press letter picker (kbd_picker.h) ------------------------ */

/* A finger, so the picker's letters are tapped the way a person taps them:
 * through a real LVGL pointer device, not by sending events. */
static lv_point_t finger;
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;

static void finger_read(lv_indev_t *d, lv_indev_data_t *data)
{
    (void)d;
    data->point = finger;
    data->state = finger_state;
}

static void tap_at(int32_t x, int32_t y)
{
    finger.x = x;
    finger.y = y;
    finger_state = LV_INDEV_STATE_PRESSED;
    settle();
    finger_state = LV_INDEV_STATE_RELEASED;
    settle();
}

static void tap_obj(lv_obj_t *obj)
{
    lv_area_t a;

    if (!obj) {
        return;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    tap_at(a.x1 + lv_area_get_width(&a) / 2, a.y1 + lv_area_get_height(&a) / 2);
}

#define K_A 29
#define K_O 43
#define K_W 39
#define K_RIGHT 1
#define K_LEFT 2
#define K_ENTER 21
#define K_ESC 40
#define K_SPACE 5

static void key_down(uint8_t code)
{
    feed((uint8_t)(POS_KEYMAP_EVENT_PRESSED | code));
    settle();
}

static void key_up(uint8_t code)
{
    feed(code);
    settle();
}

static void tap_key(uint8_t code)
{
    feed((uint8_t)(POS_KEYMAP_EVENT_PRESSED | code));
    feed(code);
    settle();
}

/* The driver times a hold on CLOCK_MONOTONIC, which lv_tick_inc() cannot
 * reach, so a hold takes as long here as it does on the keyboard. */
static void hold_past_threshold(void)
{
    struct timespec ts = { 0, (long)(KBD_PICKER_HOLD_MS + 100) * 1000L * 1000L };

    nanosleep(&ts, NULL);
    settle();
}

/* Hold a key until the picker opens, let go, move right n times, Enter. */
static void pick(uint8_t code, int right)
{
    key_down(code);
    hold_past_threshold();
    key_up(code);
    while (right-- > 0) {
        tap_key(K_RIGHT);
    }
    tap_key(K_ENTER);
}

static uint8_t letter_code(char lower)
{
    unsigned c;

    for (c = 1; c <= POS_KEYMAP_MAX_CODE; c++) {
        const char *n = pos_keymap_name((uint8_t)c);

        if (n && n[1] == '\0' && n[0] == lower - 'a' + 'A') {
            return (uint8_t)c;
        }
    }
    return 0;
}

/* Type UTF-8 text as a person would: a-z and space tapped, and every
 * letter a picker offers chosen from it with the arrows. False for a
 * character the keyboard cannot give. */
static int type_with_picker(const char *text)
{
    static const struct {
        unsigned char second; /* the UTF-8 continuation byte after 0xC3 */
        uint8_t code;
        int index;
    } offered[] = {
        { 0xA5, K_A, 0 }, { 0x85, K_A, 1 }, { 0xA4, K_A, 2 }, { 0x84, K_A, 3 },
        { 0xA6, K_A, 4 }, { 0x86, K_A, 5 }, { 0xB8, K_O, 0 }, { 0x98, K_O, 1 },
        { 0xB6, K_O, 2 }, { 0x96, K_O, 3 },
    };
    const unsigned char *p = (const unsigned char *)text;

    while (*p) {
        if (*p == ' ') {
            tap_key(K_SPACE);
            p++;
        } else if (*p >= 'a' && *p <= 'z') {
            uint8_t code = letter_code((char)*p++);

            if (!code) {
                return 0;
            }
            tap_key(code);
        } else if (p[0] == 0xC3 && p[1]) {
            size_t i;

            for (i = 0; i < sizeof(offered) / sizeof(offered[0]); i++) {
                if (offered[i].second == p[1]) {
                    break;
                }
            }
            if (i == sizeof(offered) / sizeof(offered[0])) {
                return 0;
            }
            pick(offered[i].code, offered[i].index);
            p += 2;
        } else {
            return 0;
        }
    }
    return 1;
}

static const char *choice_text(unsigned i)
{
    lv_obj_t *b = kbd_picker_choice(i);
    lv_obj_t *lb = b ? lv_obj_get_child(b, 0) : NULL;

    return lb ? lv_label_get_text(lb) : NULL;
}

static void picker_section(lv_obj_t *screen)
{
    lv_obj_t *line = pocketui_text_field(screen, "Message", true);
    static const char *const a_offers[] = {
        "\xC3\xA5", "\xC3\x85", "\xC3\xA4", "\xC3\x84", "\xC3\xA6", "\xC3\x86",
    };
    static const char *const o_offers[] = { "\xC3\xB8", "\xC3\x98", "\xC3\xB6", "\xC3\x96" };
    static const struct {
        const char *text;
        const char *bytes;
        size_t len;
    } mixed[] = {
        { "blåbær og øl", "bl\xC3\xA5" "b\xC3\xA6r og \xC3\xB8l", 15 },
        { "smörgås", "sm\xC3\xB6rg\xC3\xA5s", 9 },
    };
    unsigned i;

    pos_input_focus(line);
    settle();
    check("the one-line field has the focus", pos_input_focused() == line);

    /* 1, 2. A quick tap types the letter - on the release, never before. */
    key_down(K_A);
    check_str("A pressed types nothing yet", lv_textarea_get_text(line), "");
    check("and opens nothing", !kbd_picker_is_open());
    key_up(K_A);
    check_str("released quickly, it types a", lv_textarea_get_text(line), "a");
    tap_key(K_O);
    check_str("a quick O types o", lv_textarea_get_text(line), "ao");
    tap_key(K_W);
    check_str("and W, which has no picker, types w", lv_textarea_get_text(line), "aow");
    feed(CAPS_PRESS);
    feed(CAPS_RELEASE);
    tap_key(K_A);
    tap_key(K_O);
    feed(CAPS_PRESS);
    feed(CAPS_RELEASE);
    settle();
    check_str("under Caps, A and O type capitals as ever", lv_textarea_get_text(line), "aowAO");
    feed(SHIFT_PRESS);
    key_down(K_A);
    check_str("Shift+A is its orange '~' at once, with no picker", lv_textarea_get_text(line),
              "aowAO~");
    key_up(K_A);
    feed(SHIFT_RELEASE);
    settle();
    /* Fast typing overlaps keys: W goes down before A comes up. */
    feed((uint8_t)(POS_KEYMAP_EVENT_PRESSED | K_A));
    feed((uint8_t)(POS_KEYMAP_EVENT_PRESSED | K_W));
    feed(K_A);
    feed(K_W);
    settle();
    check_str("overlapping keys keep their order", lv_textarea_get_text(line), "aowAO~aw");
    lv_textarea_set_text(line, "");

    /* 3. Held A: the picker, and no 'a' typed first. */
    key_down(K_A);
    hold_past_threshold();
    check("holding A opens the picker", kbd_picker_is_open());
    check_str("with nothing typed before it", lv_textarea_get_text(line), "");
    check("the field keeps the focus", pos_input_focused() == line);
    check("A offers six letters", kbd_picker_choice(5) && !kbd_picker_choice(6));
    for (i = 0; i < 6; i++) {
        check_str("each one shown as UTF-8", choice_text(i), a_offers[i]);
    }
    check("the first selected", kbd_picker_selected() == 0);
    key_up(K_A);
    check("letting go of A leaves it up", kbd_picker_is_open());
    check_str("and still types nothing", lv_textarea_get_text(line), "");

    /* 5. å by Enter, æ by the arrows, ä by a finger. */
    tap_key(K_ENTER);
    check_str("Enter types the selected \xC3\xA5", lv_textarea_get_text(line), "\xC3\xA5");
    check("and closes the picker", !kbd_picker_is_open());
    pick(K_A, 4);
    check_str("Right four times, Enter: \xC3\xA6", lv_textarea_get_text(line), "\xC3\xA5\xC3\xA6");
    key_down(K_A);
    hold_past_threshold();
    key_up(K_A);
    tap_key(K_LEFT);
    check("Left stops at the first", kbd_picker_selected() == 0);
    for (i = 0; i < 9; i++) {
        tap_key(K_RIGHT);
    }
    check("Right stops at the last", kbd_picker_selected() == 5);
    tap_obj(kbd_picker_choice(2));
    check_str("a finger on \xC3\xA4 types it", lv_textarea_get_text(line),
              "\xC3\xA5\xC3\xA6\xC3\xA4");
    check("and closes the picker", !kbd_picker_is_open());
    check("the field still has the focus after the tap", pos_input_focused() == line);

    /* 4, 6. Held O: ø and ö. */
    key_down(K_O);
    hold_past_threshold();
    check("holding O opens the picker", kbd_picker_is_open());
    check("O offers four letters", kbd_picker_choice(3) && !kbd_picker_choice(4));
    for (i = 0; i < 4; i++) {
        check_str("each one shown as UTF-8", choice_text(i), o_offers[i]);
    }
    key_up(K_O);
    tap_key(K_ENTER);
    pick(K_O, 2);
    check_str("\xC3\xB8 and \xC3\xB6 follow", lv_textarea_get_text(line),
              "\xC3\xA5\xC3\xA6\xC3\xA4\xC3\xB8\xC3\xB6");
    check("ten bytes: five two-byte letters",
          strlen(lv_textarea_get_text(line)) == 10);

    /* 7. Cancel: Esc, a tap outside, or another key, and nothing chosen. */
    lv_textarea_set_text(line, "x");
    key_down(K_A);
    hold_past_threshold();
    key_up(K_A);
    tap_key(K_ESC);
    check("Esc closes the picker", !kbd_picker_is_open());
    check_str("and types nothing, nor reaches the field", lv_textarea_get_text(line), "x");
    key_down(K_O);
    hold_past_threshold();
    key_up(K_O);
    tap_at(PANEL_W / 2, PANEL_H - 40);
    check("a tap outside it closes it", !kbd_picker_is_open());
    check_str("and types nothing", lv_textarea_get_text(line), "x");
    key_down(K_A);
    hold_past_threshold();
    key_up(K_A);
    tap_key(K_W);
    check("another key closes it", !kbd_picker_is_open());
    check_str("and then types as it always does", lv_textarea_get_text(line), "xw");

    /* 8. Mixed text, byte for byte. */
    for (i = 0; i < sizeof(mixed) / sizeof(mixed[0]); i++) {
        const char *got;

        lv_textarea_set_text(line, "");
        check("the phrase can be typed", type_with_picker(mixed[i].text));
        got = lv_textarea_get_text(line);
        check_str("and the field holds it, byte for byte", got, mixed[i].bytes);
        check("its length is UTF-8's", got && strlen(got) == mixed[i].len);
    }

    /* Nowhere to put a letter, no picker: held on anything but a text
     * field, A is just a key. */
    {
        lv_obj_t *button = lv_button_create(screen);

        pos_input_add_obj(button);
        pos_input_focus(button);
        settle();
        key_down(K_A);
        hold_past_threshold();
        check("held on a button, A opens no picker", !kbd_picker_is_open());
        key_up(K_A);
        lv_obj_delete(button);
    }
    lv_obj_delete(line);
    settle();
}

/* The Space bar's two contacts, in the byte orders unit A's controller gave
 * on 2026-10-04: a press near the middle closes both, the second while the
 * first is still down, and the two releases often share one drain. */
#define K_SPACE2 14

static void space_bar_section(lv_obj_t *screen)
{
    lv_obj_t *line = pocketui_text_field(screen, "Message", true);

    pos_input_focus(line);
    settle();
    tap_key(K_SPACE);
    tap_key(K_SPACE2);
    check_str("each end of the bar types one space", lv_textarea_get_text(line), "  ");

    lv_textarea_set_text(line, "");
    key_down(K_SPACE2);
    key_down(K_SPACE);
    feed(K_SPACE2);
    feed(K_SPACE);
    settle();
    check_str("the middle of the bar types one space", lv_textarea_get_text(line), " ");

    key_down(K_SPACE2);
    key_down(K_SPACE);
    key_up(K_SPACE);
    key_up(K_SPACE2);
    check_str("whichever contact lets go first", lv_textarea_get_text(line), "  ");

    key_down(K_SPACE);
    key_down(K_SPACE2);
    key_up(K_SPACE2);
    key_up(K_SPACE);
    check_str("and whichever closes first", lv_textarea_get_text(line), "   ");

    /* One drain holding the whole press, as a slow poll would see it. */
    feed((uint8_t)(POS_KEYMAP_EVENT_PRESSED | K_SPACE2));
    feed((uint8_t)(POS_KEYMAP_EVENT_PRESSED | K_SPACE));
    feed(K_SPACE);
    feed(K_SPACE2);
    settle();
    check_str("a whole press in one drain is still one space", lv_textarea_get_text(line), "    ");

    lv_obj_delete(line);
    settle();
}

int main(void)
{
    lv_display_t *disp;
    lv_obj_t *screen;
    lv_obj_t *field;
    unsigned resets_before;
    unsigned fails_before;
    lv_indev_t *pointer;

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    pos_input_init();
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, finger_read);
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

    /* ---- 5b. the long-press letter picker ------------------------------ */

    picker_section(screen);
    space_bar_section(screen);
    pos_input_focus(field);
    settle();

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
