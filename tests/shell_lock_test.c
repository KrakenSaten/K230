/*
 * The DOORS lock screen (ui/shell/shell_lock.h, DS §31.4) under a real LVGL
 * pointer and the shell's real input stream:
 *
 *   - engaged, it covers the screen: a tap on it reaches nothing below, and
 *     it takes the keys - a character typed or NEXT pressed does not reach
 *     the focused control underneath, and focus cannot walk off it;
 *   - a tap alone and a short drag do not open it, and a short drag springs
 *     back; a swipe past the open distance does, and so does Enter;
 *   - opening plays the door sequence and ends open, with the keys handed
 *     back to what had them, and every background it read released;
 *   - engaging again during the sequence wins; reduced motion opens at once;
 *   - with no art at all it still covers, still opens.
 *
 * Built by ui/shell (CMake, simulator only); run by tests/doors_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "art.h"
#include "pocketui.h"
#include "pos_input.h"
#include "shell_lock.h"

#include <stdio.h>
#include <stdlib.h>
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
    } else {
        printf("ok   %s\n", what);
    }
}

/* ---- what the shell would provide ------------------------------------------ */

static int reduced_motion;

int pocketos_shell_reduced_motion(void)
{
    return reduced_motion;
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

static void finger(int x, int y, bool down)
{
    finger_point.x = x;
    finger_point.y = y;
    finger_state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    pump(40);
}

/* A drag from (x, y0) to (x, y1) in steps, then letting go. */
static void swipe(int x, int y0, int y1)
{
    int steps = 8;
    int k;

    finger(x, y0, true);
    for (k = 1; k <= steps; k++) {
        finger(x, y0 + (y1 - y0) * k / steps, true);
    }
    finger(x, y1, false);
}

/* ---- what is below the lock ---------------------------------------------- */

static int below_clicks;
static int below_keys;
static int engaged_calls;
static int opened_calls;
static int revealing_calls;

static void on_below_click(lv_event_t *e)
{
    (void)e;
    below_clicks++;
}

static void on_below_key(lv_event_t *e)
{
    (void)e;
    below_keys++;
}

static void on_engaged(void)
{
    engaged_calls++;
}

static void on_opened(void)
{
    opened_calls++;
}

/* The shell hides a fullscreen app's status bar here (shell.c
 * status_bar_fit): still locked, but the app is starting to show. */
static void on_revealing(void)
{
    revealing_calls++;
    check("revealing is told while the lock is still opening",
          shell_lock_is_opening() && shell_lock_is_revealing());
}

static lv_obj_t *lock_root(void)
{
    /* The lock is the last child created on the screen in this test. */
    lv_obj_t *s = lv_screen_active();

    return lv_obj_get_child(s, (int32_t)lv_obj_get_child_count(s) - 1);
}

int main(void)
{
    static const struct shell_lock_hooks hooks = { on_engaged, on_opened, on_revealing };
    struct pos_display_geometry g;
    struct pos_panel panel;
    lv_display_t *disp;
    lv_indev_t *pointer;
    lv_obj_t *button;
    size_t held_before;

    setvbuf(stdout, NULL, _IOLBF, 0);
    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_cb);
    memset(&panel, 0, sizeof(panel));
    panel.width = PANEL_W;
    panel.height = PANEL_H;
    pos_display_geometry_init(&g, &panel, POS_ROTATION_0);
    pocketui_set_display_geometry(&g);
    pos_input_init();
    pocketui_init();

    /* Something an app would have: a focused control that counts. */
    button = lv_button_create(lv_screen_active());
    lv_obj_set_size(button, 200, 100);
    lv_obj_set_pos(button, 100, 500);
    lv_obj_add_event_cb(button, on_below_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(button, on_below_key, LV_EVENT_KEY, NULL);
    pos_input_add_obj(button);
    pos_input_focus(button);
    pump(50);
    finger(200, 550, true);
    finger(200, 550, false);
    check("the control below takes a tap before the lock exists", below_clicks == 1);
    below_clicks = 0;

    shell_lock_create(lv_screen_active(), false, &hooks);
    check("created, the lock is not engaged", !shell_lock_is_locked());
    held_before = art_bytes_held();

    /* ---- engaged ---- */
    shell_lock_engage("test");
    pump(100);
    check("engaged, the lock says so and told the shell once", shell_lock_is_locked() && engaged_calls == 1);
    check("engaged, its background was read (bytes held grew)", art_bytes_held() > held_before);
    check("the lock took the keys: it is the focused object", pos_input_focused() == lock_root());
    finger(200, 550, true);
    finger(200, 550, false);
    check("a tap on the lock reaches nothing below it", below_clicks == 0);
    check("and does not open it", shell_lock_is_locked());
    pos_input_push_key('a');
    pos_input_push_key(LV_KEY_NEXT);
    pump(100);
    check("a character typed does not reach the control below", below_keys == 0);
    check("NEXT does not walk focus off the lock", pos_input_focused() == lock_root());
    check("and neither opens it", shell_lock_is_locked());
    shell_lock_engage("again");
    check("engaging an engaged lock changes nothing", engaged_calls == 1 && shell_lock_engage_count() == 1);

    /* ---- short drag ---- */
    swipe(284, 900, 900 - shell_lock_open_distance() / 2);
    pump(300);
    check("a drag short of the open distance does not open it", shell_lock_is_locked() && !shell_lock_is_opening());
    check("and what followed the finger has sprung back",
          lv_obj_get_y(lv_obj_get_child(lock_root(), (int32_t)lv_obj_get_child_count(lock_root()) - 1)) == 0);

    /* ---- swipe ---- */
    swipe(284, 1000, 1000 - shell_lock_open_distance() - 40);
    check("a swipe past the open distance starts the door sequence", shell_lock_is_opening() && shell_lock_is_locked());
    check("the open door covers what is below at first: not yet revealing",
          !shell_lock_is_revealing() && revealing_calls == 0);
    pump(1500);
    check("and ends open", !shell_lock_is_locked() && opened_calls == 1 && shell_lock_open_count() == 1);
    check("having told the shell it was revealing exactly once, before it was open",
          revealing_calls == 1 && !shell_lock_is_revealing());
    check("the lock is hidden", lv_obj_has_flag(lock_root(), LV_OBJ_FLAG_HIDDEN));
    check("every background the lock read was released", art_bytes_held() == held_before);
    pump(50);
    check("the keys went back to the control that had them", pos_input_focused() == button);
    finger(200, 550, true);
    finger(200, 550, false);
    check("and taps reach it again", below_clicks == 1);

    /* ---- Enter ---- */
    shell_lock_engage("test");
    pump(100);
    pos_input_push_key(LV_KEY_ENTER);
    pump(1500);
    check("Enter opens it", !shell_lock_is_locked() && shell_lock_open_count() == 2);
    check("Enter did not reach the control below", below_keys == 0);

    /* ---- engage during the sequence ---- */
    shell_lock_engage("test");
    pump(100);
    shell_lock_open(true, "test");
    pump(150);
    check("mid-sequence the lock is still locked", shell_lock_is_opening());
    shell_lock_engage("during");
    pump(1500);
    check("engaging during the sequence wins: locked, not opening",
          shell_lock_is_locked() && !shell_lock_is_opening() && shell_lock_open_count() == 2);
    check("and not revealing", !shell_lock_is_revealing());
    shell_lock_open(false, "direct");
    pump(50);
    check("a direct open is immediate", !shell_lock_is_locked() && shell_lock_open_count() == 3);

    /* ---- reduced motion ---- */
    reduced_motion = 1;
    shell_lock_engage("test");
    pump(100);
    shell_lock_open(true, "test");
    check("with reduced motion the door sequence is skipped", !shell_lock_is_locked());
    reduced_motion = 0;

    /* ---- no art ---- */
    setenv("POCKETOS_ART_DIR", "/nonexistent-doors-art", 1);
    held_before = art_bytes_held();
    shell_lock_engage("no art");
    pump(100);
    check("with no art the lock still engages", shell_lock_is_locked() && art_bytes_held() == held_before);
    finger(200, 550, true);
    finger(200, 550, false);
    check("and still covers what is below", below_clicks == 1);
    revealing_calls = 0;
    swipe(284, 1000, 1000 - shell_lock_open_distance() - 40);
    check("with no open door, what is below shows at once: revealing from the start",
          shell_lock_is_revealing() && revealing_calls == 1);
    pump(1500);
    check("and still opens", !shell_lock_is_locked() && !shell_lock_is_revealing());

    /* ---- many times ---- */
    unsetenv("POCKETOS_ART_DIR");
    held_before = art_bytes_held();
    for (int k = 0; k < 20; k++) {
        shell_lock_engage("repeat");
        pump(20);
        shell_lock_open(k % 2 == 0, "repeat");
        pump(1200);
    }
    check("twenty lock/open rounds end open, holding no more art than before",
          !shell_lock_is_locked() && art_bytes_held() == held_before);
    printf("shell_lock_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
