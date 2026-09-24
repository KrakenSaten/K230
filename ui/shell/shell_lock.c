/*
 * The DOORS lock screen. See shell_lock.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "shell_lock.h"

#include "app.h"
#include "art.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pos_glyphs.h"

#include <string.h>

/* The door sequence (DS §31.4): closed door out, open door held, open door
 * out. Short enough that nobody waits for it, long enough to be seen. */
#define OPEN_REVEAL_MS 300
#define OPEN_HOLD_MS 220
#define OPEN_FADE_MS 320
#define SPRING_BACK_MS 160
#define DRAG_FOLLOW_MAX 140   /* the content follows a drag this far, at half speed */

enum lock_state { LOCK_OPEN, LOCK_ENGAGED, LOCK_OPENING };

static struct {
    enum lock_state state;
    bool landscape;
    struct shell_lock_hooks hooks;
    lv_obj_t *root;
    lv_obj_t *open_img;
    lv_obj_t *lock_img;
    lv_obj_t *content;
    lv_obj_t *clock;
    lv_obj_t *date;
    lv_obj_t *hint;
    lv_obj_t *tagline;       /* on the open door, as the package sets it */
    lv_image_dsc_t *lock_bg;
    lv_image_dsc_t *open_bg;
    lv_obj_t *prev_focus;    /* what had the keys before the lock took them */
    int32_t press_y;
    bool dragging;
    unsigned engaged_n;
    unsigned opened_n;
    bool hold_at_door;       /* tests: stop the sequence on the open door */
    bool revealing;          /* opening, and what is below shows through */
} lk;

void shell_lock_test_hold_at_door(bool hold)
{
    lk.hold_at_door = hold;
}

int32_t shell_lock_open_distance(void)
{
    return lk.landscape ? 100 : 140;
}

bool shell_lock_is_locked(void)
{
    return lk.state != LOCK_OPEN;
}

bool shell_lock_is_revealing(void)
{
    return lk.state == LOCK_OPENING && lk.revealing;
}

bool shell_lock_is_opening(void)
{
    return lk.state == LOCK_OPENING;
}

unsigned shell_lock_engage_count(void)
{
    return lk.engaged_n;
}

unsigned shell_lock_open_count(void)
{
    return lk.opened_n;
}

/* ---- keys ---------------------------------------------------------------- *
 *
 * While engaged the lock is the focused object of the shell's group, and the
 * group is frozen, so NEXT and PREV cannot walk off it into the app below
 * and nothing typed reaches a field. Focus is moved from a timer, never from
 * inside the event that asked for the change (docs: LVGL focus gotchas).
 */

static void prev_focus_deleted(lv_event_t *e)
{
    (void)e;
    lk.prev_focus = NULL;
}

static void take_keys(void *unused)
{
    lv_group_t *g = pos_input_group();

    (void)unused;
    if (!g || lk.state == LOCK_OPEN) {
        return;
    }
    if (lv_obj_get_group(lk.root) != g) {
        lk.prev_focus = lv_group_get_focused(g);
        if (lk.prev_focus) {
            lv_obj_add_event_cb(lk.prev_focus, prev_focus_deleted, LV_EVENT_DELETE, NULL);
        }
        lv_group_focus_freeze(g, false);
        lv_group_add_obj(g, lk.root);
    }
    lv_group_focus_obj(lk.root);
    lv_group_focus_freeze(g, true);
}

static void give_keys_back(void)
{
    lv_group_t *g = pos_input_group();

    if (!g || lv_obj_get_group(lk.root) != g) {
        return;
    }
    lv_group_focus_freeze(g, false);
    lv_group_remove_obj(lk.root);
    if (lk.prev_focus) {
        lv_obj_remove_event_cb(lk.prev_focus, prev_focus_deleted);
        lv_group_focus_obj(lk.prev_focus);
        lk.prev_focus = NULL;
    }
}

/* ---- the door sequence --------------------------------------------------- */

static void release_art(void)
{
    lv_image_set_src(lk.lock_img, NULL);
    lv_image_set_src(lk.open_img, NULL);
    art_free(lk.lock_bg);
    art_free(lk.open_bg);
    lk.lock_bg = NULL;
    lk.open_bg = NULL;
}

/* What is under the lock starts to show: tell the shell once, so whatever
 * it draws over the lock for the lock's sake can go before the app is seen
 * with it (a fullscreen app's status bar, shell.c status_bar_fit). */
static void begin_reveal(void)
{
    if (lk.revealing) {
        return;
    }
    lk.revealing = true;
    if (lk.hooks.revealing) {
        lk.hooks.revealing();
    }
}

static void finish_open(void)
{
    lv_anim_delete(lk.lock_img, NULL);
    lv_anim_delete(lk.open_img, NULL);
    lv_obj_add_flag(lk.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lk.tagline, LV_OBJ_FLAG_HIDDEN);
    release_art();
    lk.state = LOCK_OPEN;
    lk.revealing = false;
    lk.opened_n++;
    give_keys_back();
    LOG_INFO("lock: open (%u)", lk.opened_n);
    if (lk.hooks.opened) {
        lk.hooks.opened();
    }
}

static void anim_image_opa(void *obj, int32_t v)
{
    lv_obj_set_style_image_opa(obj, (lv_opa_t)v, 0);
}

static void fade_done(lv_anim_t *a)
{
    (void)a;
    finish_open();
}

static void fade_open_door(void)
{
    lv_anim_t a;

    /* The lock's own ground goes first, so what is underneath shows through
     * as the open door fades. */
    lv_obj_remove_style(lk.root, pos_style(POS_STYLE_SCREEN), 0);
    lv_obj_add_flag(lk.tagline, LV_OBJ_FLAG_HIDDEN);
    begin_reveal();
    lv_anim_init(&a);
    lv_anim_set_var(&a, lk.open_img);
    lv_anim_set_exec_cb(&a, anim_image_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, OPEN_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, fade_done);
    lv_anim_start(&a);
}

static void hold_done(lv_anim_t *a)
{
    (void)a;
    if (lk.hold_at_door) {
        LOG_INFO("lock: held on the open door (test hook)");
        return;
    }
    fade_open_door();
}

static void reveal_done(lv_anim_t *a)
{
    lv_anim_t hold;

    (void)a;
    /* A pause on the open door: an anim that changes nothing, for its end. */
    lv_anim_init(&hold);
    lv_anim_set_var(&hold, lk.open_img);
    lv_anim_set_exec_cb(&hold, NULL);
    lv_anim_set_values(&hold, 0, 1);
    lv_anim_set_duration(&hold, OPEN_HOLD_MS);
    lv_anim_set_completed_cb(&hold, hold_done);
    lv_anim_start(&hold);
}

void shell_lock_open(bool animate, const char *why)
{
    lv_anim_t a;

    if (lk.state != LOCK_ENGAGED) {
        if (lk.state == LOCK_OPENING && !animate) {
            finish_open(); /* a direct open overtakes a running sequence */
        }
        return;
    }
    LOG_INFO("lock: opening (%s)", why ? why : "?");
    lk.state = LOCK_OPENING;
    lk.dragging = false;
    lv_obj_add_flag(lk.content, LV_OBJ_FLAG_HIDDEN);
    if (!animate || pocketos_shell_reduced_motion()) {
        finish_open();
        return;
    }
    lk.open_bg = art_load_background("open", lk.landscape);
    lv_image_set_src(lk.open_img, lk.open_bg);
    lv_obj_set_style_image_opa(lk.open_img, LV_OPA_COVER, 0);
    if (!lk.open_bg) {
        /* No open door to show: the closed one fades straight to home. */
        lv_obj_remove_style(lk.root, pos_style(POS_STYLE_SCREEN), 0);
        begin_reveal();
    } else {
        lv_obj_remove_flag(lk.tagline, LV_OBJ_FLAG_HIDDEN);
    }
    lv_anim_init(&a);
    lv_anim_set_var(&a, lk.lock_img);
    lv_anim_set_exec_cb(&a, anim_image_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, OPEN_REVEAL_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, lk.open_bg ? reveal_done : fade_done);
    lv_anim_start(&a);
}

void shell_lock_engage(const char *why)
{
    if (!lk.root) {
        return;
    }
    if (lk.state == LOCK_OPENING) {
        lv_anim_delete(lk.lock_img, NULL);
        lv_anim_delete(lk.open_img, NULL);
        lv_image_set_src(lk.open_img, NULL);
        art_free(lk.open_bg);
        lk.open_bg = NULL;
    } else if (lk.state == LOCK_ENGAGED) {
        return;
    }
    lk.state = LOCK_ENGAGED;
    lk.revealing = false;
    lk.engaged_n++;
    if (!lk.lock_bg) {
        lk.lock_bg = art_load_background("lock", lk.landscape);
        lv_image_set_src(lk.lock_img, lk.lock_bg);
    }
    lv_obj_set_style_image_opa(lk.lock_img, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(lk.open_img, LV_OPA_COVER, 0);
    /* Without its photograph the lock still covers what is below it. */
    lv_obj_remove_style(lk.root, pos_style(POS_STYLE_SCREEN), 0);
    if (!lk.lock_bg) {
        pos_style_add(lk.root, POS_STYLE_SCREEN, 0);
    }
    lv_obj_set_y(lk.content, 0);
    lv_obj_remove_flag(lk.content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(lk.root, LV_OBJ_FLAG_HIDDEN);
    lv_async_call(take_keys, NULL);
    LOG_INFO("lock: engaged (%s)", why ? why : "?");
    if (lk.hooks.engaged) {
        lk.hooks.engaged();
    }
}

/* ---- touch ---------------------------------------------------------------- */

static void spring_y(void *obj, int32_t v)
{
    lv_obj_set_y(obj, v);
}

static void hint_opa(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void on_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = { 0, 0 };

    if (lk.state != LOCK_ENGAGED) {
        return;
    }
    if (indev) {
        lv_indev_get_point(indev, &p);
    }
    if (code == LV_EVENT_PRESSED) {
        lk.press_y = p.y;
        lk.dragging = false;
        lv_anim_delete(lk.content, spring_y);
    } else if (code == LV_EVENT_PRESSING) {
        int32_t up = lk.press_y - p.y;

        if (up > 8) {
            lk.dragging = true;
        }
        if (lk.dragging) {
            int32_t follow = up > 0 ? up / 2 : 0;

            lv_obj_set_y(lk.content, -(follow > DRAG_FOLLOW_MAX ? DRAG_FOLLOW_MAX : follow));
        }
    } else if (code == LV_EVENT_RELEASED) {
        int32_t up = lk.press_y - p.y;

        if (lk.dragging && up >= shell_lock_open_distance()) {
            shell_lock_open(true, "swipe");
        } else if (lk.dragging) {
            lv_anim_t a;

            lv_anim_init(&a);
            lv_anim_set_var(&a, lk.content);
            lv_anim_set_exec_cb(&a, spring_y);
            lv_anim_set_values(&a, lv_obj_get_y(lk.content), 0);
            lv_anim_set_duration(&a, SPRING_BACK_MS);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        } else {
            /* A tap: say how to open, by lifting the hint for a moment. */
            lv_anim_t a;

            lv_anim_init(&a);
            lv_anim_set_var(&a, lk.hint);
            lv_anim_set_exec_cb(&a, hint_opa);
            lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
            lv_anim_set_duration(&a, 420);
            lv_anim_start(&a);
        }
        lk.dragging = false;
    }
}

static void open_by_key(void *unused)
{
    (void)unused;
    shell_lock_open(true, "key");
}

static void on_key(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);

    if (lk.state == LOCK_ENGAGED && (key == LV_KEY_ENTER || key == ' ' || key == LV_KEY_UP)) {
        lv_async_call(open_by_key, NULL);
    }
}

/* ---- building -------------------------------------------------------------- */

static void set_if_changed(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void shell_lock_set_time(const char *hm, const char *date)
{
    set_if_changed(lk.clock, hm ? hm : "--:--");
    set_if_changed(lk.date, (date && date[0]) ? date : "Time not set");
}

static lv_obj_t *full(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    return o;
}

static lv_obj_t *picture(lv_obj_t *parent)
{
    lv_obj_t *img = lv_image_create(parent);

    lv_obj_set_pos(img, 0, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

void shell_lock_create(lv_obj_t *screen, bool landscape, const struct shell_lock_hooks *hooks)
{
    lv_obj_t *glyph;
    lv_obj_t *text;
    /* The package's anchors (base_ui_layout.json, lock): the clock's
     * baseline 294 / 210 at 98 / 108 px, the date 42 below, the hint near
     * the foot - here as the tops of the Doors fonts' boxes. */
    int32_t clock_y = landscape ? 72 : 164;
    int32_t hint_y = landscape ? 430 : 1036;

    memset(&lk, 0, sizeof(lk));
    lk.landscape = landscape;
    if (hooks) {
        lk.hooks = *hooks;
    }
    lk.root = full(screen);
    lv_obj_add_flag(lk.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lk.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(lk.root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(lk.root, on_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(lk.root, on_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(lk.root, on_touch, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(lk.root, on_key, LV_EVENT_KEY, NULL);

    lk.open_img = picture(lk.root);
    pos_style_add(lk.open_img, POS_STYLE_ENV_BG, 0);
    lk.lock_img = picture(lk.root);
    pos_style_add(lk.lock_img, POS_STYLE_ENV_BG, 0);

    lk.tagline = lv_label_create(lk.root);
    pos_style_add(lk.tagline, POS_STYLE_ENV_TEXT_SECONDARY, 0);
    lv_label_set_text(lk.tagline, "Open. Explore. Connect.");
    lv_obj_align(lk.tagline, LV_ALIGN_BOTTOM_MID, 0, landscape ? -22 : -24);
    lv_obj_add_flag(lk.tagline, LV_OBJ_FLAG_HIDDEN);

    lk.content = full(lk.root);
    lv_obj_remove_flag(lk.content, LV_OBJ_FLAG_CLICKABLE);
    lk.clock = lv_label_create(lk.content);
    pos_style_add(lk.clock, POS_STYLE_ENV_CLOCK_LARGE, 0);
    lv_label_set_text(lk.clock, "--:--");
    lv_obj_align(lk.clock, LV_ALIGN_TOP_MID, 0, clock_y);
    lk.date = lv_label_create(lk.content);
    pos_style_add(lk.date, POS_STYLE_ENV_TEXT_SECONDARY, 0);
    lv_label_set_text(lk.date, "");
    lv_obj_align(lk.date, LV_ALIGN_TOP_MID, 0, clock_y + 108);

    lk.hint = lv_obj_create(lk.content);
    lv_obj_remove_style_all(lk.hint);
    lv_obj_remove_flag(lk.hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(lk.hint, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lk.hint, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lk.hint, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(lk.hint, 10, 0);
    lv_obj_set_y(lk.hint, hint_y);
    glyph = lv_image_create(lk.hint);
    lv_obj_remove_style_all(glyph);
    pos_style_add(glyph, POS_STYLE_ENV_GLYPH, 0);
    lv_image_set_src(glyph, &pos_glyph_lock);
    text = lv_label_create(lk.hint);
    pos_style_add(text, POS_STYLE_ENV_TEXT_SECONDARY, 0);
    lv_label_set_text(text, "Swipe up to open");
}
