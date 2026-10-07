/*
 * The touch keyboard (DS v0.1 §17.3) typing into a real text field, end to
 * end, under a real LVGL pointer device.
 *
 * Nothing here calls the keyboard's internals to make a character appear. A
 * synthetic finger presses the key at its place on the glass, the keyboard
 * pushes a code point into the logical stream, the stream's keypad device
 * delivers it to the focused object, and the field is read back. If any link
 * in that chain is wrong the text is wrong, which is the only way to be sure
 * the keyboard is a source of the one stream (§17.4) rather than a second
 * path into a text area.
 *
 * The keyboard is never given the field, and the field never hears of the
 * keyboard. That is the property under test.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/pos_keyboard_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketui.h"
#include "pos_keyboard.h"

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
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want);
    }
}

/* ---- a display that draws nowhere, and one finger ---------------------- */

static uint8_t draw_buf[PANEL_W * 40 * 2];
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = finger_state;
    data->point = finger_point;
}

static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

/* A key press queues a code point; the stream's own device is read on its
 * own period, so a check placed straight after a tap can run before the
 * character has been delivered. Wait for the queue to empty, with a bound so
 * a broken stream fails the test instead of hanging it. */
static void drain(void)
{
    int t;

    for (t = 0; t < 300 && pos_input_queued() > 0; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
    pump(40);
}

static void centre_of(lv_obj_t *obj, int *x, int *y)
{
    lv_area_t a;

    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    *x = a.x1 + lv_area_get_width(&a) / 2;
    *y = a.y1 + lv_area_get_height(&a) / 2;
}

static void press_at(int x, int y)
{
    finger_point.x = (int32_t)x;
    finger_point.y = (int32_t)y;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
}

static void release(void)
{
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
}

static void tap_at(int x, int y)
{
    press_at(x, y);
    release();
    drain();
}

/* Press the key drawn with this label, the way a finger would. */
static bool tap_key(lv_obj_t *kb, const char *label)
{
    lv_obj_t *key = pos_keyboard_key(kb, label);
    int x;
    int y;

    if (!key) {
        printf("FAIL no key labelled \"%s\"\n", label);
        failed++;
        checks++;
        return false;
    }
    centre_of(key, &x, &y);
    tap_at(x, y);
    return true;
}

static void tap_all(lv_obj_t *kb, const char *labels)
{
    char one[2] = { 0, 0 };

    for (; *labels; labels++) {
        one[0] = *labels;
        tap_key(kb, one);
    }
}

/* ---- the Done callback the owner would supply -------------------------- */

static int done_calls;
static int ready_calls;

static void on_done(void *user)
{
    (void)user;
    done_calls++;
}

/* What a single-line field emits when it is committed. */
static void on_ready(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        ready_calls++;
    }
}

int main(void)
{
    lv_display_t *disp;
    lv_obj_t *screen;
    lv_obj_t *field;
    lv_obj_t *single;
    lv_obj_t *kb;
    lv_obj_t *shift;
    lv_area_t a;
    int x;
    int y;

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_indev_set_read_cb(lv_indev_create(), read_cb);
    lv_indev_set_type(lv_indev_get_next(NULL), LV_INDEV_TYPE_POINTER);

    pos_input_init();
    pocketui_init();
    screen = lv_screen_active();
    pocketui_style_screen(screen);

    /* The editor above, the keyboard below - the arrangement DS §17.6
     * describes for Notes, built here without Notes. */
    {
        lv_obj_t *body = lv_obj_create(screen);

        lv_obj_remove_style_all(body);
        lv_obj_set_size(body, PANEL_W, PANEL_H - POS_KB_H);
        lv_obj_set_pos(body, 0, 0);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(body, POCKETUI_PAD, 0);
        field = pocketui_text_field(body, "Note", false);
    }
    kb = pos_keyboard_create(screen);
    check("the keyboard is created", kb != NULL);
    pos_keyboard_set_done_cb(kb, on_done, NULL);
    pump(60);

    /* ---- 1. geometry, DEV-1 included ---------------------------------- */

    lv_obj_update_layout(kb);
    lv_obj_get_coords(kb, &a);
    check("the sheet is the panel's width", lv_area_get_width(&a) == POS_KB_W);
    check("the sheet is 296 tall (DS §17.3)", lv_area_get_height(&a) == POS_KB_H);
    check("the sheet sits at the bottom", a.y2 == PANEL_H - 1);

    lv_obj_get_coords(pos_keyboard_key(kb, "q"), &a);
    check("a letter key is 52 wide (DEV-1)", lv_area_get_width(&a) == POS_KB_KEY_W);
    check("a letter key is 64 tall, so the minimum is met in height",
          lv_area_get_height(&a) == POS_KB_KEY_H);
    lv_obj_get_coords(pos_keyboard_key(kb, "SHIFT"), &a);
    check("a wide key is 80", lv_area_get_width(&a) == POS_KB_WIDE_W);
    lv_obj_get_coords(pos_keyboard_key(kb, "SPACE"), &a);
    check("Space fills the row's remainder (388)", lv_area_get_width(&a) == 388);

    /* ---- 2. visibility, and that it leaves focus alone ---------------- */

    check("the keyboard starts hidden", !pos_keyboard_is_shown(kb));
    pos_input_focus(field);
    pump(60);
    check("the field is focused before the keyboard opens", pos_input_focused() == field);
    pos_keyboard_show(kb);
    pump(60);
    check("the keyboard shows", pos_keyboard_is_shown(kb));
    check("opening the keyboard does not steal focus (DS §17.2)",
          pos_input_focused() == field);

    /* ---- 3. alpha typing, end to end ---------------------------------- */

    tap_all(kb, "hello");
    check_str("tapping letters types them into the field",
              lv_textarea_get_text(field), "hello");
    check("tapping a key does not steal focus", pos_input_focused() == field);

    /* ---- 4. Space ----------------------------------------------------- */

    tap_key(kb, "SPACE");
    check_str("Space inserts a space", lv_textarea_get_text(field), "hello ");

    /* ---- 5. Shift: one-shot ------------------------------------------- */

    shift = pos_keyboard_key(kb, "SHIFT");
    check("Shift starts off", pos_keyboard_shift(kb) == POS_KB_SHIFT_OFF);
    check("keys read lower case while Shift is off",
          pos_keyboard_key(kb, "w") != NULL && pos_keyboard_key(kb, "W") == NULL);

    centre_of(shift, &x, &y);
    tap_at(x, y);
    check("one tap arms Shift for the next character",
          pos_keyboard_shift(kb) == POS_KB_SHIFT_ONCE);
    check("the letters change case, so the state is legible without colour",
          pos_keyboard_key(kb, "W") != NULL && pos_keyboard_key(kb, "w") == NULL);

    tap_key(kb, "W");
    check_str("a one-shot Shift produces one capital",
              lv_textarea_get_text(field), "hello W");
    check("Shift falls back to off after one character",
          pos_keyboard_shift(kb) == POS_KB_SHIFT_OFF);
    tap_key(kb, "e");
    check_str("the character after it is lower case",
              lv_textarea_get_text(field), "hello We");

    /* ---- 6. Shift: lock, and leaving it ------------------------------- */

    centre_of(shift, &x, &y);
    tap_at(x, y);
    tap_at(x, y); /* quickly again: locks */
    check("a quick second tap locks Shift", pos_keyboard_shift(kb) == POS_KB_SHIFT_LOCK);

    lv_textarea_set_text(field, "");
    pump(60);
    tap_all(kb, "AB");
    check_str("a locked Shift holds across characters",
              lv_textarea_get_text(field), "AB");
    check("and stays locked", pos_keyboard_shift(kb) == POS_KB_SHIFT_LOCK);

    centre_of(shift, &x, &y);
    tap_at(x, y);
    check("a further tap releases the lock", pos_keyboard_shift(kb) == POS_KB_SHIFT_OFF);

    /* A slow second tap means "I changed my mind", not "lock". */
    tap_at(x, y);
    check("armed again", pos_keyboard_shift(kb) == POS_KB_SHIFT_ONCE);
    pump(POS_KB_SHIFT_LOCK_MS + 200);
    tap_at(x, y);
    check("a slow second tap disarms rather than locking",
          pos_keyboard_shift(kb) == POS_KB_SHIFT_OFF);

    /* ---- 7. the symbol layer, and C9's letters ------------------------ */

    lv_textarea_set_text(field, "");
    pump(60);
    check("the alpha layer is showing", pos_keyboard_layer(kb) == POS_KB_LAYER_ALPHA);
    tap_key(kb, "?123");
    check("?123 switches to the symbol layer",
          pos_keyboard_layer(kb) == POS_KB_LAYER_SYMBOL);
    check("the layer key now offers the way back", pos_keyboard_key(kb, "ABC") != NULL);

    tap_all(kb, "42");
    check_str("digits type from the symbol layer", lv_textarea_get_text(field), "42");
    tap_key(kb, "@");
    check_str("punctuation types too", lv_textarea_get_text(field), "42@");

    lv_textarea_set_text(field, "");
    pump(60);
    tap_key(kb, "\xC3\xA6");
    tap_key(kb, "\xC3\xB8");
    tap_key(kb, "\xC3\xA5");
    check_str("æ ø å type from the symbol layer (C9)",
              lv_textarea_get_text(field), "\xC3\xA6\xC3\xB8\xC3\xA5");

    /* Shift reaches them, as DS §17.3 says it does for any letter. */
    lv_textarea_set_text(field, "");
    pump(60);
    centre_of(pos_keyboard_key(kb, "SHIFT"), &x, &y);
    tap_at(x, y);
    tap_key(kb, "\xC3\x86");
    check_str("Shift capitalises them", lv_textarea_get_text(field), "\xC3\x86");

    tap_key(kb, "ABC");
    check("ABC returns to the alpha layer", pos_keyboard_layer(kb) == POS_KB_LAYER_ALPHA);

    /* ---- 8. Backspace, and its repeat --------------------------------- */

    lv_textarea_set_text(field, "abcdef");
    pump(60);
    tap_key(kb, "BKSP");
    check_str("Backspace deletes one character", lv_textarea_get_text(field), "abcde");

    /* Held: one on the press, nothing until 400 ms, then one per 60. Elapsed
     * time is tracked explicitly because press_at() itself advances the
     * clock, and the whole point of this section is when things happen. */
    centre_of(pos_keyboard_key(kb, "BKSP"), &x, &y);
    lv_textarea_set_text(field, "0123456789abcdefghij");
    pump(60);
    press_at(x, y); /* 60 ms held */
    drain();        /* +40: still well inside the 400 ms delay */
    check_str("the press deletes exactly one",
              lv_textarea_get_text(field), "0123456789abcdefghi");
    pump(200); /* ~300 ms held */
    check_str("nothing repeats before the delay is up",
              lv_textarea_get_text(field), "0123456789abcdefghi");
    pump(200); /* ~500 ms held: the first repeat has landed */
    drain();
    check("the repeat starts after the delay",
          strlen(lv_textarea_get_text(field)) < 19);
    {
        size_t after_first = strlen(lv_textarea_get_text(field));

        pump(240); /* about four more at 60 ms */
        drain();
        check("and then repeats about every 60 ms",
              strlen(lv_textarea_get_text(field)) + 1 < after_first);
    }
    release();
    drain();
    {
        size_t at_release = strlen(lv_textarea_get_text(field));

        pump(300);
        drain();
        check("releasing stops the repeat",
              strlen(lv_textarea_get_text(field)) == at_release);
    }

    /* Hiding the sheet mid-hold must not leave a timer deleting text. */
    lv_textarea_set_text(field, "xxxxxxxx");
    pump(60);
    press_at(x, y);
    pump(POS_KB_REPEAT_DELAY_MS + 100);
    pos_keyboard_hide(kb);
    release();
    drain();
    {
        size_t at_hide = strlen(lv_textarea_get_text(field));

        pump(400);
        drain();
        check("hiding the keyboard stops the repeat",
              strlen(lv_textarea_get_text(field)) == at_hide);
    }
    pos_keyboard_show(kb);
    pump(60);

    /* ---- 9. Enter on a multi-line field ------------------------------- */

    lv_textarea_set_text(field, "one");
    pump(60);
    pos_keyboard_set_return(kb, POS_KB_RETURN_NEWLINE);
    check("a multi-line field gets ENTER", pos_keyboard_key(kb, "ENTER") != NULL);
    check("and not DONE", pos_keyboard_key(kb, "DONE") == NULL);
    tap_key(kb, "ENTER");
    tap_key(kb, "t");
    check_str("Enter inserts a line break", lv_textarea_get_text(field), "one\nt");
    check("Enter does not call the owner's done", done_calls == 0);

    /* ---- 10. Done on a single-line field ------------------------------ *
     *
     * Done is only ever set for a single-line field (DS §17.3), so it is
     * tested against one. Both send the same logical key; what differs is
     * that a single-line field turns it into "ready" rather than a line
     * break, and that the owner is told so it can commit and hide.
     */

    single = pocketui_text_field(lv_obj_get_parent(lv_obj_get_parent(field)), NULL, true);
    lv_obj_add_event_cb(single, on_ready, LV_EVENT_READY, NULL);
    pos_input_focus(single);
    pump(60);
    check("the single-line field is focused", pos_input_focused() == single);
    tap_all(kb, "ab");
    check_str("it types like any other field", lv_textarea_get_text(single), "ab");

    pos_keyboard_set_return(kb, POS_KB_RETURN_DONE);
    check("a single-line field gets DONE", pos_keyboard_key(kb, "DONE") != NULL);
    tap_key(kb, "DONE");
    check("Done tells the owner", done_calls == 1);
    check_str("Done leaves no line break in a single-line field",
              lv_textarea_get_text(single), "ab");
    check("Done makes the field report itself ready", ready_calls == 1);
    check("the keyboard did not hide itself: that is the owner's call",
          pos_keyboard_is_shown(kb));
    pos_input_focus(field);
    pump(60);

    /* ---- 11. reduced motion ------------------------------------------- *
     *
     * The keyboard has no motion to reduce - DS §12's table has no keyboard
     * row and none was invented - but the Backspace repeat is function, not
     * decoration (§17.3), so it must survive the setting rather than being
     * mistaken for an animation.
     */

    pocketui_set_reduced_motion(true);
    lv_textarea_set_text(field, "reduced-motion-sample");
    pump(60);
    centre_of(pos_keyboard_key(kb, "BKSP"), &x, &y);
    press_at(x, y);
    pump(POS_KB_REPEAT_DELAY_MS + 240);
    drain();
    release();
    drain();
    check("Backspace still repeats under reduced motion",
          strlen(lv_textarea_get_text(field)) < 19);
    pocketui_set_reduced_motion(false);

    /* ---- 12. a full queue drops keys safely --------------------------- *
     *
     * pos_input drops the newest key when the queue is full rather than
     * losing what was typed first. The keyboard ignores that return value
     * because there is nothing useful it could do; what matters is that a
     * burst neither crashes nor corrupts the text.
     */

    lv_textarea_set_text(field, "");
    pump(60);
    {
        unsigned n;
        const char *got;

        /* Overfill deliberately: more keys than the queue can hold, with no
         * chance to drain in between. */
        for (n = 0; n < POS_INPUT_QUEUE + 16; n++) {
            pos_input_push_key('a');
        }
        check("the queue never grows past its bound",
              pos_input_queued() == POS_INPUT_QUEUE);
        drain();
        got = lv_textarea_get_text(field);
        check("an overfilled queue delivers exactly what it held",
              strlen(got) == POS_INPUT_QUEUE);
        check("and delivers it undamaged",
              strspn(got, "a") == strlen(got));

        /* And the keyboard's own burst: taps as fast as the finger model
         * allows, with no settling between them. */
        lv_textarea_set_text(field, "");
        pump(60);
        for (n = 0; n < 12; n++) {
            lv_obj_t *key = pos_keyboard_key(kb, "b");

            centre_of(key, &x, &y);
            press_at(x, y);
            release();
        }
        drain();
        got = lv_textarea_get_text(field);
        check("a burst of taps types every one of them", strlen(got) == 12);
        check("with nothing garbled", strspn(got, "b") == strlen(got));
    }

    /* ---- 13. the keyboard is a source, not a text-area client --------- */

    check("focus never left the field", pos_input_focused() == field);
    /* Every character above arrived through the queue. If the keyboard had
     * written to the text area directly, the field would still show text
     * with the stream torn down - so tear it down and check nothing types. */
    {
        const char *before;

        lv_textarea_set_text(field, "keep");
        pump(60);
        pos_input_deinit();
        before = lv_textarea_get_text(field);
        check_str("stream down: the field holds what it had", before, "keep");
        tap_all(kb, "zzz");
        check_str("with the stream gone the keyboard types nothing",
                  lv_textarea_get_text(field), "keep");
    }

    printf("pos_keyboard_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
