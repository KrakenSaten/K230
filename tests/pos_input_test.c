/*
 * The logical key stream and the text field, DS v0.1 §17.1, §17.2 and §17.4.
 *
 * The claim §17.4 makes is testable, and it is the one worth testing before
 * the touch keyboard exists: every source converges on one stream, the field
 * cannot tell them apart, and an app has nothing to bind to but a focused
 * object. So the same characters are delivered twice - once pushed directly,
 * the way the touch keyboard will in M4, and once through a second LVGL
 * keypad device adopted as a source, the way the SDL keyboard is on the host
 * and a physical keyboard will be - and the field must answer identically.
 *
 * The adopted-source case uses a stub keypad device rather than SDL's,
 * because a headless test cannot post SDL events. What it does exercise is
 * the mechanism the SDL keyboard actually goes through: pos_input_add_source
 * gives the device a private group whose focused sink forwards into the
 * queue. Nothing about that path is SDL-specific.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/pos_input_test.sh. A display with a
 * no-op flush is enough: nothing is rendered.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketui.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

/* ---- a display that draws nowhere ------------------------------------- */

static uint8_t buf[PANEL_W * 40 * 2];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

/* ---- a stub keypad device, standing in for SDL's ----------------------- */

static uint32_t stub_queue[16];
static unsigned stub_head, stub_tail;
static bool stub_release_pending;
static uint32_t stub_last;

static void stub_read(lv_indev_t *dev, lv_indev_data_t *data)
{
    (void)dev;
    if (stub_release_pending) {
        stub_release_pending = false;
        data->key = stub_last;
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = stub_head != stub_tail;
        return;
    }
    if (stub_head == stub_tail) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    stub_last = stub_queue[stub_tail % 16];
    stub_tail++;
    data->key = stub_last;
    data->state = LV_INDEV_STATE_PRESSED;
    stub_release_pending = true;
    data->continue_reading = true;
}

static void stub_push(uint32_t key)
{
    stub_queue[stub_head % 16] = key;
    stub_head++;
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

static void type(const char *s)
{
    for (; *s; s++) {
        pos_input_push_key((pos_key_t)(unsigned char)*s);
    }
    settle();
}

/* A layout that never settles never returns from lv_timer_handler(), so no
 * check could see it; the alarm says so instead of hanging the run. */
static void on_hang(int sig)
{
    static const char msg[] = "FAIL a field's layout never settled (stopped after 10 s)\n";

    (void)sig;
    (void)!write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

/* Whether a field's error caption is shown below it and inside the wrapper,
 * which clips it. */
static int caption_inside(lv_obj_t *field)
{
    lv_obj_t *wrap = lv_obj_get_parent(field);
    lv_obj_t *lb;
    lv_area_t f;
    lv_area_t w;
    lv_area_t c;

    if (lv_obj_get_child_count(wrap) < 2) {
        return 0;
    }
    lb = lv_obj_get_child(wrap, 1);
    lv_obj_update_layout(lb);
    lv_obj_get_coords(field, &f);
    lv_obj_get_coords(wrap, &w);
    lv_obj_get_coords(lb, &c);
    return !lv_obj_has_flag(lb, LV_OBJ_FLAG_HIDDEN) && lv_area_get_height(&c) > 0 &&
           c.y1 > f.y2 && c.y1 >= w.y1 && c.y2 <= w.y2;
}

int main(void)
{
    lv_display_t *disp;
    lv_obj_t *screen;
    lv_obj_t *field;
    lv_obj_t *second;
    lv_indev_t *stub;
    unsigned i;
    unsigned accepted;

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    pos_input_init();
    pocketui_init();
    screen = lv_screen_active();
    pocketui_style_screen(screen);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

    /* ---- 1. the stream exists and is empty ---------------------------- */

    check("init creates the focus group", pos_input_group() != NULL);
    check("init creates one delivering device", pos_input_indev() != NULL);
    check("the delivering device is a keypad",
          lv_indev_get_type(pos_input_indev()) == LV_INDEV_TYPE_KEYPAD);
    check("the queue starts empty", pos_input_queued() == 0);
    check("nothing is focused yet", pos_input_focused() == NULL);
    pos_input_init();
    check("a second init is a no-op", pos_input_indev() != NULL);

    /* A key pushed while the group holds nothing focusable must drain and
     * go nowhere. This is the real no-focus case: LVGL groups have no
     * unfocused state, so it can only be seen before anything joins. */
    pos_input_push_key('z');
    settle();
    check("a key with nothing focusable drains harmlessly", pos_input_queued() == 0);

    /* ---- 2. the field, and what a push does to it --------------------- */

    field = pocketui_text_field(screen, "Note", false);
    check("the field is a text area", field != NULL && lv_obj_check_type(field, &lv_textarea_class));
    check("the field joined the group", lv_obj_get_group(field) == pos_input_group());
    check("the field is wrapped for its error caption",
          lv_obj_get_parent(field) != NULL && lv_obj_get_parent(field) != screen);
    check_str("an empty field holds nothing", lv_textarea_get_text(field), "");
    /* LVGL focuses the first object added to an empty group, so the first
     * field on a screen is focused by construction. That is what DS §17.6
     * wants of the Notes editor, so it is kept rather than undone. */
    check("the first field added takes focus", pos_input_focused() == field);
    check("focusing NULL is ignored rather than pretending",
          (pos_input_focus(NULL), pos_input_focused() == field));

    pos_input_focus(field);
    settle();
    check("focus landed on the field", pos_input_focused() == field);
    check("the focused field carries LVGL's focused state",
          lv_obj_has_state(field, LV_STATE_FOCUSED));

    type("hi");
    check_str("pushed keys reach the focused field", lv_textarea_get_text(field), "hi");
    check("the queue drained", pos_input_queued() == 0);

    /* ---- 3. editing keys --------------------------------------------- */

    pos_input_push_key(LV_KEY_BACKSPACE);
    settle();
    check_str("backspace deletes one character", lv_textarea_get_text(field), "h");

    pos_input_push_key(LV_KEY_BACKSPACE);
    pos_input_push_key(LV_KEY_BACKSPACE);
    settle();
    check_str("backspace on an empty field is inert", lv_textarea_get_text(field), "");

    type("ab");
    pos_input_push_key(LV_KEY_LEFT);
    settle();
    type("c");
    check_str("the caret moves and inserts where it is",
              lv_textarea_get_text(field), "acb");

    /* Multi-line takes a newline; DS §17.3 gives it Enter rather than Done. */
    pos_input_push_key(LV_KEY_ENTER);
    settle();
    type("z");
    check("a multi-line field takes a newline",
          strchr(lv_textarea_get_text(field), '\n') != NULL);

    /* ---- 4. the vocabulary boundary: code points in, UTF-8 out --------
     *
     * DS §17.4 says the stream speaks code points; LVGL's device layer packs
     * UTF-8 bytes into the same uint32_t. The two agree for ASCII, so only a
     * character above U+007F proves the conversion is there - and C9 has put
     * exactly such characters on the symbol layer.
     */

    lv_textarea_set_text(field, "");
    settle();
    pos_input_push_key(0x00E6); /* æ */
    pos_input_push_key(0x00F8); /* ø */
    pos_input_push_key(0x00E5); /* å */
    settle();
    check_str("a code point above U+007F arrives as its character",
              lv_textarea_get_text(field), "\xC3\xA6\xC3\xB8\xC3\xA5");

    lv_textarea_set_text(field, "");
    settle();
    pos_input_push_key(0x20AC); /* three-byte: € */
    settle();
    check_str("a three-byte code point survives the boundary",
              lv_textarea_get_text(field), "\xE2\x82\xAC");

    lv_textarea_set_text(field, "");
    settle();
    type("a");
    check_str("ASCII is unaffected by the conversion", lv_textarea_get_text(field), "a");

    /* ---- 5. a second source converges on the same stream -------------- */

    stub = lv_indev_create();
    lv_indev_set_type(stub, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(stub, stub_read);
    check("a keypad device is adopted as a source", pos_input_add_source(stub));
    check("the delivering device refuses to be its own source",
          !pos_input_add_source(pos_input_indev()));
    check("a NULL source is refused", !pos_input_add_source(NULL));
    check("the source does not join the app focus group",
          lv_indev_get_group(stub) != pos_input_group());

    lv_textarea_set_text(field, "");
    settle();
    stub_push('q');
    stub_push('r');
    settle();
    check_str("keys from an adopted source reach the same field",
              lv_textarea_get_text(field), "qr");

    /* The field cannot tell the sources apart: the same sequence, delivered
     * the other way, must produce the same text. That is §17.4's claim. */
    lv_textarea_set_text(field, "");
    settle();
    type("qr");
    check_str("a pushed sequence produces the same text as a sourced one",
              lv_textarea_get_text(field), "qr");

    /* Interleaved, order is preserved: one queue, not two. */
    lv_textarea_set_text(field, "");
    settle();
    pos_input_push_key('1');
    stub_push('2');
    settle();
    pos_input_push_key('3');
    settle();
    check_str("sources share one ordered queue", lv_textarea_get_text(field), "123");

    /* An LVGL source hands over UTF-8 bytes packed into the key, the way the
     * SDL driver does. Harvesting must decode them, or the same letter would
     * mean one thing pushed and another sourced. */
    lv_textarea_set_text(field, "");
    settle();
    {
        const uint8_t ae[4] = { 0xC3, 0xA6, 0, 0 }; /* æ as LVGL packs it */
        uint32_t packed;

        memcpy(&packed, ae, sizeof(packed));
        stub_push(packed);
    }
    settle();
    check_str("a source's packed UTF-8 becomes the same character",
              lv_textarea_get_text(field), "\xC3\xA6");

    /* ---- 6. focus moves between fields -------------------------------- */

    second = pocketui_text_field(screen, NULL, true);
    check("a single-line field is one row tall",
          lv_obj_get_style_height(second, LV_PART_MAIN) == POCKETUI_ROW_H);
    lv_textarea_set_text(field, "");
    lv_textarea_set_text(second, "");
    settle();

    pos_input_focus(second);
    settle();
    type("s");
    check_str("keys follow the focus", lv_textarea_get_text(second), "s");
    check_str("the unfocused field is untouched", lv_textarea_get_text(field), "");

    pos_input_push_key(LV_KEY_NEXT);
    settle();
    check("LV_KEY_NEXT moves focus rather than typing",
          pos_input_focused() != second);
    check_str("NEXT left no character behind", lv_textarea_get_text(second), "s");

    /* ---- 7. disabled --------------------------------------------------- */

    pos_input_focus(second);
    settle();
    pocketui_text_field_set_enabled(second, false);
    settle();
    check("a disabled field carries the disabled state",
          lv_obj_has_state(second, LV_STATE_DISABLED));
    check("a disabled field gives up focus", pos_input_focused() != second);
    check("a disabled field leaves the group", lv_obj_get_group(second) == NULL);
    type("n");
    check_str("a disabled field takes no text", lv_textarea_get_text(second), "s");

    pocketui_text_field_set_enabled(second, true);
    pos_input_focus(second);
    settle();
    type("y");
    check_str("re-enabling restores editing", lv_textarea_get_text(second), "sy");

    /* ---- 8. the error state carries a caption ------------------------- */

    check("a field starts with no error caption",
          lv_obj_get_child_count(lv_obj_get_parent(second)) == 1);
    pocketui_text_field_set_error(second, "Name is required");
    settle();
    check("an error adds the caption", lv_obj_get_child_count(lv_obj_get_parent(second)) == 2);
    check_str("the caption says what is wrong",
              lv_label_get_text(lv_obj_get_child(lv_obj_get_parent(second), 1)),
              "Name is required");
    check("the caption is visible",
          !lv_obj_has_flag(lv_obj_get_child(lv_obj_get_parent(second), 1), LV_OBJ_FLAG_HIDDEN));

    pocketui_text_field_set_error(second, NULL);
    settle();
    check("clearing the error hides the caption",
          lv_obj_has_flag(lv_obj_get_child(lv_obj_get_parent(second), 1), LV_OBJ_FLAG_HIDDEN));
    check("clearing does not delete the caption",
          lv_obj_get_child_count(lv_obj_get_parent(second)) == 2);
    pocketui_text_field_set_error(second, "");
    check("an empty message clears rather than shows",
          lv_obj_has_flag(lv_obj_get_child(lv_obj_get_parent(second), 1), LV_OBJ_FLAG_HIDDEN));

    /* ---- 8b. a multi-line field makes room for its caption ------------ */

    /* However its wrapper gets its height, the caption goes below the field
     * and inside the wrapper. A field at 100% of its wrapper left no room:
     * grown into a body, as the Notes editor is, the caption was laid out
     * past the wrapper's edge and clipped away; sized by its content, as
     * `field` is here, the field chased the caption's height and LVGL never
     * came back - hence the alarm. */
    signal(SIGALRM, on_hang);
    alarm(10);
    {
        lv_obj_t *body;
        lv_obj_t *grown;
        int32_t h;

        lv_obj_update_layout(field);
        h = lv_obj_get_height(field);
        pocketui_text_field_set_error(field, "Too long");
        settle();
        check("a multi-line caption is below the field, inside its wrapper",
              caption_inside(field));
        check("a wrapper sized by its content keeps the field's height",
              lv_obj_get_height(field) == h);
        pocketui_text_field_set_error(field, NULL);
        settle();
        lv_obj_update_layout(field);
        check("clearing shrinks that wrapper back to the field",
              lv_obj_get_height(lv_obj_get_parent(field)) == h);

        body = lv_obj_create(screen);
        lv_obj_remove_style_all(body);
        lv_obj_set_size(body, PANEL_W, 600);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        grown = pocketui_text_field(body, NULL, false);
        lv_obj_set_flex_grow(lv_obj_get_parent(grown), 1);
        settle();
        lv_obj_update_layout(grown);
        check("a field in a grown wrapper fills it", lv_obj_get_height(grown) == 600);
        pocketui_text_field_set_error(grown, "Not saved");
        settle();
        check("its caption takes its room from the field, inside the wrapper",
              caption_inside(grown));
        pocketui_text_field_set_error(grown, NULL);
        settle();
        lv_obj_update_layout(grown);
        check("clearing gives the field all of it back", lv_obj_get_height(grown) == 600);
        lv_obj_delete(body);
        settle();
    }
    alarm(0);

    /* ---- 9. the queue is bounded and keeps what came first ------------ */

    lv_textarea_set_text(field, "");
    pos_input_focus(field);
    settle();
    accepted = 0;
    for (i = 0; i < POS_INPUT_QUEUE; i++) {
        if (pos_input_push_key('a')) {
            accepted++;
        }
    }
    check("the queue accepts up to its bound", accepted == POS_INPUT_QUEUE);
    check("one past the bound is refused", !pos_input_push_key('b'));
    check("the queue reports itself full", pos_input_queued() == POS_INPUT_QUEUE);
    settle();
    check("everything queued was delivered", pos_input_queued() == 0);
    check("the dropped key was the newest, not the oldest",
          strchr(lv_textarea_get_text(field), 'b') == NULL);

    /* ---- 10. deleting a focused field leaves no dangling focus -------- */

    pos_input_focus(field);
    settle();
    lv_obj_delete(lv_obj_get_parent(field));
    settle();
    check("deleting the focused field clears focus", pos_input_focused() != field);
    type("q"); /* must not crash */
    check("the stream survives its focused object being deleted", 1);

    pos_input_deinit();
    check("deinit releases the group", pos_input_group() == NULL);
    check("deinit releases the device", pos_input_indev() == NULL);
    check("a push without a stream is refused", !pos_input_push_key('a'));

    /* ---- 11. group redirection, and what it must survive (DS §18.8) --- *
     *
     * The alert redirects the whole stream rather than filtering it. Two
     * things have to hold for that to be safe: a redirection that cannot be
     * established must report so rather than half-succeed, because the
     * caller ties the keyboard's suppression to that answer; and a
     * redirection must never outlive the stream, or the next init would
     * start out believing it was still in effect. */

    check("a redirection without a stream is refused",
          !pos_input_push_group(NULL) && !pos_input_group_redirected());

    pos_input_init();
    check("a fresh stream is not redirected", !pos_input_group_redirected());

    {
        lv_group_t *alt = lv_group_create();
        lv_group_t *other = lv_group_create();

        check("a NULL group is refused", !pos_input_push_group(NULL));
        check("and refusing it redirects nothing",
              !pos_input_group_redirected());

        check("a real group is taken", pos_input_push_group(alt));
        check("and the stream says so", pos_input_group_redirected());
        /* One deep: §18.6 allows one alert, so a second push is refused
         * rather than stacked, and the first redirection stands. */
        check("a second push is refused", !pos_input_push_group(other));
        check("and the first redirection still stands",
              pos_input_group_redirected());

        pos_input_pop_group();
        check("popping gives the stream back", !pos_input_group_redirected());
        pos_input_pop_group();
        check("an unmatched pop does nothing",
              !pos_input_group_redirected());

        /* The regression: a deinit taken while a redirection is in effect,
         * with nobody to pop it. The next init must be indistinguishable
         * from a fresh process start. */
        check("redirected again", pos_input_push_group(alt));
        pos_input_deinit();
        pos_input_init();
        check("a redirection does not survive deinit and init",
              !pos_input_group_redirected());

        /* ...and the stream still works afterwards, rather than merely
         * reporting the right flag. */
        {
            lv_group_t *third = lv_group_create();

            check("a push after the cycle is accepted",
                  pos_input_push_group(third));
            check("the stream is redirected again",
                  pos_input_group_redirected());
            pos_input_pop_group();
            check("and pops cleanly", !pos_input_group_redirected());
            lv_group_delete(third);
        }
        lv_group_delete(alt);
        lv_group_delete(other);
    }
    pos_input_deinit();

    printf("pos_input_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
