/*
 * Settings' pages but Network: Display, Appearance, Sound, Keyboard, Power &
 * Sleep, Time & Region with its time zone list, and Developer (DS §52).
 *
 * Every value here is the shell's (app.h), read when the page is painted and
 * changed through the shell, which applies it and stores it; nothing here
 * keeps a value of its own between ticks.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "brightness.h"
#include "power_policy.h"
#include "settings_internal.h"
#include "tz_zones.h"
#include "volume.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* The keyboard base's light: 0 (off) to 100 in steps of 10 (kbd_light.h). */
#define SETTINGS_LIGHT_STEP 10

const char *settings_page_title(enum settings_page page)
{
    static const char *const titles[PAGE_COUNT] = {
        [PAGE_ROOT] = "Settings",       [PAGE_DISPLAY] = "Display",       [PAGE_APPEARANCE] = "Appearance",
        [PAGE_SOUND] = "Sound",         [PAGE_KEYBOARD] = "Keyboard",     [PAGE_POWER] = "Power & Sleep",
        [PAGE_TIME] = "Time & Region",  [PAGE_NETWORK] = "Network",       [PAGE_SYSTEM] = "System",
        [PAGE_DEVELOPER] = "Developer", [PAGE_ZONES] = "Time zone",       [PAGE_SHEET] = "Wi-Fi network",
    };

    return page >= 0 && page < PAGE_COUNT && titles[page] ? titles[page] : "Settings";
}

static const char *rotation_word(int mode)
{
    return mode == 1 ? "Portrait" : mode == 2 ? "Landscape" : "Automatic";
}

static const char *text_size_word(enum pos_text_size s)
{
    return s == POS_TEXT_SIZE_LARGE ? "Large" : s == POS_TEXT_SIZE_MEDIUM ? "Medium" : "Small";
}

static const char *mode_word(enum pos_mode m)
{
    return m == POS_MODE_NIGHT ? "Night" : m == POS_MODE_OUTDOOR ? "Outdoor" : "Normal";
}

static void volume_view(struct sv_volume *v)
{
    sv_volume_apply(v, pocketos_shell_volume_get(), pocketos_shell_volume_muted(),
                    pocketos_shell_volume_available(), VOLUME_MIN_PCT, VOLUME_MAX_PCT);
}

static int keyboard_state(void)
{
    struct pocketos_orientation o;

    pocketos_shell_orientation(&o);
    return (int)o.keyboard;
}

void settings_summary(struct settings_app *a, enum settings_page page, char *out, size_t n)
{
    const struct pos_theme_def *theme = pos_theme_current_def();
    const struct tz_zone *zone = tz_zone_find(pocketos_shell_timezone());
    struct sv_volume v;
    char state[24];

    switch (page) {
    case PAGE_DISPLAY:
        if (a->bright.supported) {
            snprintf(out, n, "%s \xc2\xb7 %s \xc2\xb7 %s text", a->bright.value, rotation_word(a->rot.selected),
                     text_size_word(pocketos_shell_text_size()));
        } else {
            snprintf(out, n, "%s \xc2\xb7 %s text", rotation_word(a->rot.selected),
                     text_size_word(pocketos_shell_text_size()));
        }
        break;
    case PAGE_APPEARANCE:
        snprintf(out, n, "%s \xc2\xb7 %s", theme ? theme->name : "", mode_word(pos_theme_current_mode()));
        break;
    case PAGE_SOUND:
        volume_view(&v);
        snprintf(out, n, "%s", v.summary);
        break;
    case PAGE_KEYBOARD:
        sv_keyboard_text(keyboard_state(), pocketos_shell_keyboard_light(), state, sizeof(state), out, n);
        break;
    case PAGE_POWER:
        sv_power_summary(pocketos_shell_screen_off_after(), pocketos_shell_lock_after(), out, n);
        break;
    case PAGE_TIME:
        snprintf(out, n, "%s", zone ? zone->place : "UTC");
        break;
    case PAGE_NETWORK:
        snprintf(out, n, "%s", a->wifi.headline);
        break;
    case PAGE_SYSTEM:
        snprintf(out, n, "About, status, diagnostics, restart and power off");
        break;
    case PAGE_DEVELOPER:
        snprintf(out, n, "Debug overlay %s", pocketos_shell_debug_overlay() ? "on" : "off");
        break;
    default:
        out[0] = '\0';
        break;
    }
}

/* ---- Display: brightness, rotation, text size -------------------------------------- */

static void brightness_step(struct settings_app *a, int direction)
{
    if (!a->bright.supported) {
        return;
    }
    pocketos_shell_brightness_set(sv_brightness_step(a->bright.percent, direction, BRIGHTNESS_MIN_PCT,
                                                     BRIGHTNESS_MAX_PCT, BRIGHTNESS_STEP_PCT));
    settings_poll_brightness(a);
    settings_repaint(a);
}

static void on_bright_down(lv_event_t *e)
{
    brightness_step(lv_event_get_user_data(e), -1);
}

static void on_bright_up(lv_event_t *e)
{
    brightness_step(lv_event_get_user_data(e), 1);
}

static void on_rotation(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));

    if (i >= 0 && i < SV_ROTATION_MODES) {
        pocketos_shell_set_rotation_mode((enum pocketos_rotation_mode)i);
    }
    settings_poll_rotation(a);
    settings_repaint(a);
}

/* Text size (DS §46): the shell applies it live and stores it. The fonts of
 * everything here change at once through the shared styles and the panels
 * are flex, so they lay themselves out again; only the marks are repainted. */
static void on_text_size(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));

    if (i >= 0 && i < POS_TEXT_SIZE_COUNT) {
        pocketos_shell_set_text_size((enum pos_text_size)i);
    }
    settings_repaint(a);
}

/* A row of choices, the current one accented (repaint), each flex-grown. */
static void choices(lv_obj_t *panel, const char *const *labels, int n, lv_event_cb_t cb, void *user, lv_obj_t **out)
{
    lv_obj_t *r = settings_hrow(panel, SETTINGS_BTN_H);
    int i;

    for (i = 0; i < n; i++) {
        out[i] = settings_button(r, labels[i], cb, user, 0);
        lv_obj_set_flex_grow(out[i], 1);
        lv_obj_set_user_data(out[i], (void *)(intptr_t)i);
    }
}

static void build_display(struct settings_app *a)
{
    static const char *const rot_labels[SV_ROTATION_MODES] = { "AUTOMATIC", "PORTRAIT", "LANDSCAPE" };
    static const char *const size_labels[POS_TEXT_SIZE_COUNT] = { "SMALL", "MEDIUM", "LARGE" };
    lv_obj_t *p;
    int i;

    p = settings_panel(a, "BRIGHTNESS", COL_LEFT);
    settings_stepper(p, "Brightness", on_bright_down, on_bright_up, a, &a->w.bright_down, &a->w.bright_value,
                     &a->w.bright_up);
    a->w.bright_note = settings_wrap_label(p, "", POS_STYLE_TEXT_MUTED);

    /* Rotation: three modes, the stored one accented; the note carries the
     * words (DS §2). */
    p = settings_panel(a, "ROTATION", COL_RIGHT);
    choices(p, rot_labels, SV_ROTATION_MODES, on_rotation, a, a->w.rot_btn);
    a->w.rot_note = settings_wrap_label(p, "", POS_STYLE_TEXT_SECONDARY);

    /* Each choice in its own size, so the difference is seen before it is
     * chosen. */
    p = settings_panel(a, "TEXT SIZE", COL_LEFT);
    choices(p, size_labels, POS_TEXT_SIZE_COUNT, on_text_size, a, a->w.size_btn);
    for (i = 0; i < POS_TEXT_SIZE_COUNT; i++) {
        lv_obj_add_style(lv_obj_get_child(a->w.size_btn[i], 0), pos_style_text_size_sample((enum pos_text_size)i), 0);
    }
}

static void repaint_display(struct settings_app *a)
{
    int i;

    lv_label_set_text(a->w.bright_value, a->bright.value);
    settings_set_enabled(a->w.bright_down, a->bright.can_down);
    settings_set_enabled(a->w.bright_up, a->bright.can_up);
    lv_label_set_text(a->w.bright_note, a->bright.note);
    settings_set_hidden(a->w.bright_note, a->bright.note[0] == '\0');
    for (i = 0; i < SV_ROTATION_MODES; i++) {
        settings_accent(a->w.rot_btn[i], i == a->rot.selected);
    }
    lv_label_set_text(a->w.rot_note, a->rot.note);
    for (i = 0; i < POS_TEXT_SIZE_COUNT; i++) {
        settings_accent(a->w.size_btn[i], i == (int)pocketos_shell_text_size());
    }
}

/* ---- Appearance: theme and display mode ------------------------------------------------ */

/* The selection is the shell's: it applies it live, stores it and announces
 * it, exactly as shell.theme does. Everything on screen follows through the
 * shared styles; only the selected marks are repainted here. */
static void on_theme(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    const struct pos_theme_def *d = pos_theme_at((int)i);

    if (d) {
        pocketos_shell_set_appearance(d->id, NULL);
    }
    settings_repaint(a);
}

static void on_mode(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));

    if (i >= 0 && i < POS_MODE_COUNT) {
        pocketos_shell_set_appearance(NULL, pos_mode_name((enum pos_mode)i));
    }
    settings_repaint(a);
}

/* A list of rows that go two to a line in the wide shape (settings_page_shape). */
static lv_obj_t *two_up_list(lv_obj_t *panel)
{
    lv_obj_t *l = lv_obj_create(panel);

    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(l, 8, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/* A row of such a list: a title, a caption under it when there is one, and
 * the SELECTED chip on the current one. */
static lv_obj_t *choice_row(lv_obj_t *list, const char *title, const char *caption, lv_event_cb_t cb, void *user,
                            intptr_t index, lv_obj_t **chip)
{
    lv_obj_t *row = settings_hrow(list, POCKETUI_ROW_H + 8);
    lv_obj_t *left;
    lv_obj_t *lb;

    pos_style_add(row, POS_STYLE_DIVIDER, 0);
    pos_style_add(row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_user_data(row, (void *)index);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user);
    left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lb = pocketui_label(left, title, POS_STYLE_ROW_TITLE);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lb, LV_PCT(100));
    if (caption) {
        pocketui_label_fit(lb, 1);
        lb = pocketui_label(left, caption, POS_STYLE_CAPTION);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lb, LV_PCT(100));
        pocketui_label_fit(lb, 1);
    } else {
        pocketui_label_fit(lb, 2);
    }
    *chip = lv_label_create(row);
    pos_style_add(*chip, POS_STYLE_CHIP, 0);
    pos_style_add(*chip, POS_STYLE_CHIP_ACTIVE, 0);
    lv_label_set_text(*chip, "SELECTED");
    return row;
}

static void build_appearance(struct settings_app *a)
{
    static const char *const mode_labels[POS_MODE_COUNT] = { "NORMAL", "OUTDOOR", "NIGHT" };
    lv_obj_t *p = settings_panel(a, "THEME", COL_LEFT);
    int i;

    a->w.theme_list = two_up_list(p);
    for (i = 0; i < pos_theme_count() && i < SETTINGS_THEMES_MAX; i++) {
        choice_row(a->w.theme_list, pos_theme_at(i)->name, NULL, on_theme, a, i, &a->w.theme_chip[i]);
    }
    pocketui_label(p, "DISPLAY MODE", POS_STYLE_CAPTION);
    choices(p, mode_labels, POS_MODE_COUNT, on_mode, a, a->w.mode_btn);
}

static void repaint_appearance(struct settings_app *a)
{
    const struct pos_theme_def *cur = pos_theme_current_def();
    enum pos_mode mode = pos_theme_current_mode();
    int i;

    for (i = 0; i < pos_theme_count() && i < SETTINGS_THEMES_MAX; i++) {
        settings_set_hidden(a->w.theme_chip[i], !cur || pos_theme_at(i) != cur);
    }
    for (i = 0; i < POS_MODE_COUNT; i++) {
        settings_accent(a->w.mode_btn[i], i == (int)mode);
    }
}

/* ---- Sound: volume and mute -------------------------------------------------------------- */

static void volume_step(struct settings_app *a, int direction)
{
    pocketos_shell_volume_set(sv_brightness_step(pocketos_shell_volume_get(), direction, VOLUME_MIN_PCT,
                                                 VOLUME_MAX_PCT, VOLUME_STEP_PCT));
    settings_repaint(a);
}

static void on_vol_down(lv_event_t *e)
{
    volume_step(lv_event_get_user_data(e), -1);
}

static void on_vol_up(lv_event_t *e)
{
    volume_step(lv_event_get_user_data(e), 1);
}

static void on_mute(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);

    pocketos_shell_volume_set_muted(!pocketos_shell_volume_muted());
    settings_repaint(a);
}

static void build_sound(struct settings_app *a)
{
    lv_obj_t *p = settings_panel(a, "VOLUME", COL_LEFT);

    settings_stepper(p, "Volume", on_vol_down, on_vol_up, a, &a->w.vol_down, &a->w.vol_value, &a->w.vol_up);
    a->w.mute_btn = settings_switch_row(p, "Mute", on_mute, a);
    a->w.vol_note = settings_wrap_label(p, "", POS_STYLE_TEXT_MUTED);
}

static void repaint_sound(struct settings_app *a)
{
    struct sv_volume v;

    volume_view(&v);
    lv_label_set_text(a->w.vol_value, v.value);
    settings_set_enabled(a->w.vol_down, v.can_down);
    settings_set_enabled(a->w.vol_up, v.can_up);
    settings_switch_paint(a->w.mute_btn, v.muted);
    lv_label_set_text(a->w.vol_note, v.note);
    settings_set_hidden(a->w.vol_note, v.note[0] == '\0');
}

/* ---- Keyboard: the base and its light ------------------------------------------------------ */

static void light_step(struct settings_app *a, int direction)
{
    int cur = pocketos_shell_keyboard_light();
    int want = cur + direction * SETTINGS_LIGHT_STEP;

    if (cur < 0) {
        return;
    }
    want = want < 0 ? 0 : want > 100 ? 100 : want - want % SETTINGS_LIGHT_STEP;
    pocketos_shell_set_keyboard_light(want);
    settings_repaint(a);
}

static void on_light_down(lv_event_t *e)
{
    light_step(lv_event_get_user_data(e), -1);
}

static void on_light_up(lv_event_t *e)
{
    light_step(lv_event_get_user_data(e), 1);
}

static void build_keyboard(struct settings_app *a)
{
    lv_obj_t *p = settings_panel(a, "KEYBOARD BASE", COL_LEFT);
    lv_obj_t *r = settings_hrow(p, SETTINGS_BTN_H);

    pocketui_label(r, "Keyboard base", POS_STYLE_ROW_TITLE);
    a->w.kb_state = lv_label_create(r);
    pos_style_add(a->w.kb_state, POS_STYLE_CHIP, 0);
    pos_style_add(a->w.kb_state, POS_STYLE_CHIP_OFF, 0);
    settings_stepper(p, "Key light", on_light_down, on_light_up, a, &a->w.light_down, &a->w.light_value,
                     &a->w.light_up);
    a->w.light_note = settings_wrap_label(p, "", POS_STYLE_TEXT_MUTED);
}

static void repaint_keyboard(struct settings_app *a)
{
    int light = pocketos_shell_keyboard_light();
    int kb = keyboard_state();
    char state[24];
    char summary[SV_TEXT];
    char value[16];

    sv_keyboard_text(kb, light, state, sizeof(state), summary, sizeof(summary));
    lv_label_set_text(a->w.kb_state, state);
    lv_obj_remove_style(a->w.kb_state, pos_style(POS_STYLE_CHIP_ACTIVE), 0);
    lv_obj_remove_style(a->w.kb_state, pos_style(POS_STYLE_CHIP_OFF), 0);
    pos_style_add(a->w.kb_state, kb == POCKETOS_KEYBOARD_PRESENT ? POS_STYLE_CHIP_ACTIVE : POS_STYLE_CHIP_OFF, 0);
    if (light < 0) {
        snprintf(value, sizeof(value), "--");
    } else if (light == 0) {
        snprintf(value, sizeof(value), "Off");
    } else {
        snprintf(value, sizeof(value), "%d %%", light);
    }
    lv_label_set_text(a->w.light_value, value);
    settings_set_enabled(a->w.light_down, light > 0);
    settings_set_enabled(a->w.light_up, light >= 0 && light < 100);
    lv_label_set_text(a->w.light_note, light < 0 ? "This device has no keyboard light it can drive." : "");
    settings_set_hidden(a->w.light_note, light >= 0);
}

/* ---- Power & Sleep -------------------------------------------------------------------------- */

static int timer_get(int which)
{
    return which == POWER_TIMER_LOCK ? pocketos_shell_lock_after() : pocketos_shell_screen_off_after();
}

static void timer_step(lv_event_t *e, int dir)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t which = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    int next = power_option_step((enum power_timer)which, timer_get((int)which), dir);

    if (which == POWER_TIMER_LOCK) {
        pocketos_shell_set_lock_after(next);
    } else {
        pocketos_shell_set_screen_off_after(next);
    }
    settings_repaint(a);
}

static void on_timer_down(lv_event_t *e)
{
    timer_step(e, -1);
}

static void on_timer_up(lv_event_t *e)
{
    timer_step(e, 1);
}

static void on_lock_start(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);

    pocketos_shell_set_lock_at_start(!pocketos_shell_lock_at_start());
    settings_repaint(a);
}

static void build_power(struct settings_app *a)
{
    static const char *const titles[2] = { "Screen off after", "Lock after" };
    lv_obj_t *p = settings_panel(a, "SCREEN AND LOCK", COL_LEFT);
    int i;

    for (i = 0; i < 2; i++) {
        settings_stepper(p, titles[i], on_timer_down, on_timer_up, a, &a->w.timer_down[i], &a->w.timer_value[i],
                         &a->w.timer_up[i]);
        lv_obj_set_user_data(a->w.timer_down[i], (void *)(intptr_t)i);
        lv_obj_set_user_data(a->w.timer_up[i], (void *)(intptr_t)i);
    }
    a->w.lock_start_btn = settings_switch_row(p, "Lock when Doors starts", on_lock_start, a);

    /* Three things, never one (power_policy.h): said here in as many words. */
    p = settings_panel(a, "WHAT EACH ONE DOES", COL_RIGHT);
    settings_wrap_label(p, "Screen off: the screen goes dark and Doors keeps running. A touch or a key turns it "
                           "back on and does nothing else.",
                        POS_STYLE_TEXT_SECONDARY);
    settings_wrap_label(p, "Lock: the lock screen covers the apps until it is swiped open. It has no code.",
                        POS_STYLE_TEXT_SECONDARY);
    settings_wrap_label(p, "Sleep: not available. This device does not suspend, and cannot wake itself on a timer.",
                        POS_STYLE_TEXT_SECONDARY);
    settings_wrap_label(p, "Neither happens while an alarm rings, or in Video, Camera, Vision or DeskBuddy.",
                        POS_STYLE_TEXT_MUTED);
}

static void repaint_power(struct settings_app *a)
{
    int i;

    for (i = 0; i < 2; i++) {
        struct sv_timer t;

        sv_timer_apply(&t, i, timer_get(i));
        lv_label_set_text(a->w.timer_value[i], t.value);
        settings_set_enabled(a->w.timer_down[i], t.can_down);
        settings_set_enabled(a->w.timer_up[i], t.can_up);
    }
    settings_switch_paint(a->w.lock_start_btn, pocketos_shell_lock_at_start());
}

/* ---- Time & Region, and the time zone list ------------------------------------------------- */

static void on_change_zone(lv_event_t *e)
{
    settings_go(lv_event_get_user_data(e), PAGE_ZONES);
}

static void build_time(struct settings_app *a)
{
    lv_obj_t *p = settings_panel(a, "TIME ZONE", COL_LEFT);
    lv_obj_t *b;

    a->w.zone_place = settings_wrap_label(p, "", POS_STYLE_TITLE);
    a->w.zone_detail = settings_wrap_label(p, "", POS_STYLE_CAPTION);
    a->w.local_time = settings_wrap_label(p, "", POS_STYLE_VALUE);
    b = settings_button(p, "CHANGE TIME ZONE", on_change_zone, a, 0);
    lv_obj_set_width(b, LV_PCT(100));

    p = settings_panel(a, "CLOCK", COL_RIGHT);
    a->w.clock_state = settings_wrap_label(p, "", POS_STYLE_TEXT_SECONDARY);
}

static void repaint_time(struct settings_app *a)
{
    const struct tz_zone *z = tz_zone_find(pocketos_shell_timezone());
    char line[SV_TEXT];
    int set = pocketos_shell_system_day() >= 0;

    if (!z) {
        z = tz_zone_at(0);
    }
    lv_label_set_text(a->w.zone_place, z->place);
    snprintf(line, sizeof(line), "%s \xc2\xb7 %s", z->id, z->offset);
    lv_label_set_text(a->w.zone_detail, line);
    if (set) {
        time_t now = time(NULL);
        struct tm tm;

        if (localtime_r(&now, &tm)) {
            snprintf(line, sizeof(line), "Local time %02d:%02d", tm.tm_hour, tm.tm_min);
        } else {
            snprintf(line, sizeof(line), "Local time --:--");
        }
    } else {
        snprintf(line, sizeof(line), "Local time --:--");
    }
    if (strcmp(lv_label_get_text(a->w.local_time), line) != 0) {
        lv_label_set_text(a->w.local_time, line);
    }
    lv_label_set_text(a->w.clock_state,
                      set ? "Set from the network. The time is right; the zone decides how it is shown."
                          : "Not set yet. This device has no clock that runs while it is off: the time comes "
                            "from the network once Wi-Fi or Ethernet is connected.");
}

static void on_zone(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    const struct tz_zone *z = tz_zone_at((int)i);

    if (z && pocketos_shell_set_timezone(z->id) == 0) {
        settings_go(a, PAGE_TIME);
    }
}

static void build_zones(struct settings_app *a)
{
    lv_obj_t *p = settings_panel(a, "CHOOSE A TIME ZONE", COL_LEFT);
    const char *cur = pocketos_shell_timezone();
    int i;

    a->w.zone_list = two_up_list(p);
    for (i = 0; i < tz_zone_count() && i < SETTINGS_ZONES_MAX; i++) {
        const struct tz_zone *z = tz_zone_at(i);

        choice_row(a->w.zone_list, z->place, z->offset, on_zone, a, i, &a->w.zone_chip[i]);
        settings_set_hidden(a->w.zone_chip[i], strcmp(z->id, cur) != 0);
    }
}

/* ---- Developer ------------------------------------------------------------------------------- */

static void on_overlay(lv_event_t *e)
{
    struct settings_app *a = lv_event_get_user_data(e);

    pocketos_shell_set_debug_overlay(!pocketos_shell_debug_overlay());
    settings_repaint(a);
}

static void build_developer(struct settings_app *a)
{
    lv_obj_t *p = settings_panel(a, "DEBUG OVERLAY", COL_LEFT);

    a->w.overlay_btn = settings_switch_row(p, "Debug overlay", on_overlay, a);
    settings_wrap_label(p, "One line at the foot of every screen: CPU, memory, temperature, network traffic and, "
                           "while the radio answers, LoRa packets. Refreshed every 2 seconds. It takes no touch and "
                           "shows no addresses or names.",
                        POS_STYLE_TEXT_SECONDARY);
    settings_wrap_label(p, "Off unless turned on here; it stays as it was left after a restart.", POS_STYLE_TEXT_MUTED);
}

static void repaint_developer(struct settings_app *a)
{
    settings_switch_paint(a->w.overlay_btn, pocketos_shell_debug_overlay());
}

/* ---- dispatch --------------------------------------------------------------------------------- */

void settings_page_build(struct settings_app *a)
{
    switch (a->page) {
    case PAGE_DISPLAY:
        build_display(a);
        break;
    case PAGE_APPEARANCE:
        build_appearance(a);
        break;
    case PAGE_SOUND:
        build_sound(a);
        break;
    case PAGE_KEYBOARD:
        build_keyboard(a);
        break;
    case PAGE_POWER:
        build_power(a);
        break;
    case PAGE_TIME:
        build_time(a);
        break;
    case PAGE_ZONES:
        build_zones(a);
        break;
    case PAGE_DEVELOPER:
        build_developer(a);
        break;
    default:
        break;
    }
}

void settings_page_repaint(struct settings_app *a)
{
    switch (a->page) {
    case PAGE_DISPLAY:
        repaint_display(a);
        break;
    case PAGE_APPEARANCE:
        repaint_appearance(a);
        break;
    case PAGE_SOUND:
        repaint_sound(a);
        break;
    case PAGE_KEYBOARD:
        repaint_keyboard(a);
        break;
    case PAGE_POWER:
        repaint_power(a);
        break;
    case PAGE_TIME:
        repaint_time(a);
        break;
    case PAGE_DEVELOPER:
        repaint_developer(a);
        break;
    default:
        break;
    }
}

/* The theme and zone lists: one row to a line when tall; two when wide,
 * sharing the panel's width with an 8 px gap. The panel is the page's one
 * column across the frame, so its width is known from the layout without
 * measuring anything mid-pass: the frame's content, less the panel's padding
 * and border. A pixel is given up so that rounding never sends the second
 * row of a pair to a line of its own. */
void settings_page_shape(struct settings_app *a)
{
    lv_obj_t *list = a->page == PAGE_APPEARANCE ? a->w.theme_list : a->page == PAGE_ZONES ? a->w.zone_list : NULL;
    lv_obj_t *panel;
    int32_t w = LV_PCT(100);
    uint32_t i;

    if (!list) {
        return;
    }
    panel = lv_obj_get_parent(list);
    if (a->wide) {
        w = (a->full_w - lv_obj_get_style_pad_left(panel, LV_PART_MAIN) -
             lv_obj_get_style_pad_right(panel, LV_PART_MAIN) - 2 * lv_obj_get_style_border_width(panel, LV_PART_MAIN) -
             8) / 2 - 1;
    }
    for (i = 0; i < lv_obj_get_child_count(list); i++) {
        lv_obj_set_width(lv_obj_get_child(list, i), w);
    }
}
