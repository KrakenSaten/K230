/*
 * DeskBuddy: a small companion for the desk. A pair of eyes that wake when
 * somebody arrives, are glad to see the owner and wary of a stranger; a desk
 * guard that notes who came by while the owner was away; a calm night clock
 * that still blinks. docs/apps/DESKBUDDY.md.
 *
 * This file is the screen and nothing else. What DeskBuddy does is the
 * brain's (db_brain.c), what the eyes look like is db_face.c's, what vision
 * says arrives through a provider (db_vision.h), and the files are
 * db_store.c's. There is no vision in v0.1: without $DESKBUDDY_SIM the
 * provider is db_vision_none_ops and DeskBuddy runs blind - Night mode and
 * the idle face work exactly the same.
 *
 * TIME. One lv_timer, owned here and deleted in destroy(). Its period is not
 * fixed: after every run it is set to when the brain or the provider next
 * has something to do (a blink, a timeout, a scripted event), at most a
 * second away. A sleeping face with no vision wakes once a second and draws
 * nothing. Nothing ever blocks: a provider hands over what it has.
 *
 * DRAWING. The eyes are plain filled objects in role styles (DS §4) - a
 * rounded white in the accent, a pupil, a lid and an arch cut-out in the
 * background colour - restyled only when db_face_eyes() gives a different
 * shape. Night dims them by opacity, not by a colour of their own. There is
 * no tweening: an expression is a step, a blink is two.
 * Reduced motion stops the idle behaviour altogether (DS §12).
 *
 * LAYOUT. Portrait stacks the face over the text and the controls;
 * landscape puts them side by side. The size handler only sets a flag and
 * the timer lays out, outside LVGL's layout pass.
 *
 * LIFETIME. destroy() stops the provider, saves what changed, deletes the
 * timer and then this app's root - so no callback can arrive after the app
 * is freed, whatever the shell deletes later.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "deskbuddy_app.h"

#include "db_brain.h"
#include "db_face.h"
#include "db_guard.h"
#include "db_prefs.h"
#include "db_store.h"
#include "db_vision.h"
#include "db_vision_mock.h"

#include "app.h"
#include "pocketui.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BUTTON_H 64
_Static_assert(BUTTON_H >= POCKETUI_TOUCH_MIN, "a button must meet the DS 64 px touch minimum");
#define GAP 12
#define SIDE_W 400          /* the text and controls column in landscape */
#define TIMER_MIN_MS 20
#define TIMER_MAX_MS 1000
#define LOG_LINES 3
#define DIM_OPA LV_OPA_40   /* the night face: the accent, quietened */

struct deskbuddy_eye {
    lv_obj_t *box;
    lv_obj_t *white;        /* the accent (a role style: no colour here) */
    lv_obj_t *pupil;        /* the background colour, as are the two cut-outs */
    lv_obj_t *lid;
    lv_obj_t *arch;
    struct db_eye_shape drawn;
};

struct deskbuddy_app {
    struct db_brain brain;
    struct db_guard_log log;
    struct db_vision_queue queue;
    struct db_vision_provider provider;
    struct db_vision_pipeline_cfg pipeline_cfg;
    struct db_vision_mock mock;
    int64_t provider_next;       /* -1: nothing scheduled */
    bool sim;                    /* $DESKBUDDY_SIM: scripted vision, dev keys, nothing saved */
    int64_t mono;                /* the app's monotonic ms, from lv_tick */
    uint32_t last_tick;
    unsigned prefs_saved;        /* brain.prefs_rev last written */
    unsigned log_saved;
    bool save_failed;
    bool layout_pending;
    int face_w;
    int face_h;
    bool landscape;
    int eye_box;
    struct db_face face_drawn;
    bool face_valid;
    unsigned face_paints;
    unsigned steps;              /* timer runs, for the wake-up budget test */
    enum db_state shown_state;
    enum db_seen shown_seen;
    unsigned shown_log_rev;
    unsigned shown_prefs_rev;
    bool shown_valid;
    lv_timer_t *timer;
    lv_obj_t *root;
    lv_obj_t *face;
    struct deskbuddy_eye eye[2];
    lv_obj_t *side;
    lv_obj_t *clock;
    lv_obj_t *caption;
    lv_obj_t *note;
    lv_obj_t *log_line[LOG_LINES];
    lv_obj_t *action;
    lv_obj_t *modes;
    lv_obj_t *mode_btn[DB_MODE_COUNT];
    lv_obj_t *settings_btn;
    lv_obj_t *panel;
    lv_obj_t *toggle[DB_PREF_TOGGLE_COUNT];
    lv_obj_t *done;
};

enum action_cmd { ACT_NONE = 0, ACT_ARM, ACT_DISARM, ACT_ACK };

/* ---- time ---------------------------------------------------------------------- */

static int64_t app_now(struct deskbuddy_app *a)
{
    uint32_t t = lv_tick_get();

    a->mono += (uint32_t)(t - a->last_tick); /* wraps cleanly every 49 days */
    a->last_tick = t;
    return a->mono;
}

/* The wall clock when the board knows it (pocketos_shell_system_day), else 0. */
static int64_t wall_now(void)
{
    return pocketos_shell_system_day() < 0 ? 0 : (int64_t)time(NULL);
}

static void hhmm(int64_t wall, char *out, size_t len)
{
    struct tm tm;
    time_t t = (time_t)wall;

    if (wall <= 0 || !localtime_r(&t, &tm)) {
        snprintf(out, len, "--:--");
        return;
    }
    snprintf(out, len, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

/* ---- small helpers ---------------------------------------------------------------- */

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj && lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) != hidden) {
        if (hidden) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void style_button(lv_obj_t *btn, bool primary)
{
    lv_obj_remove_style(btn, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(btn, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(btn, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    lv_obj_remove_style(btn, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
    if (primary) {
        pos_style_add(btn, POS_STYLE_BUTTON_PRIMARY, 0);
        pos_style_add(btn, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    } else {
        pos_style_add(btn, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(btn, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
}

/* Restyle only on a change, so a refresh does not invalidate a button
 * nobody touched. The primary state is kept in the object's user flag. */
static void set_primary(lv_obj_t *btn, bool primary)
{
    if (lv_obj_has_flag(btn, LV_OBJ_FLAG_USER_1) != primary) {
        style_button(btn, primary);
        if (primary) {
            lv_obj_add_flag(btn, LV_OBJ_FLAG_USER_1);
        } else {
            lv_obj_remove_flag(btn, LV_OBJ_FLAG_USER_1);
        }
    }
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *label = lv_label_create(btn);

    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, BUTTON_H);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    /* A tap must not move focus off the root, or the next key goes nowhere
     * (DS 17.2). */
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_style_all(label);
    pos_style_add(label, POS_STYLE_BUTTON_LABEL, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    style_button(btn, false);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    return btn;
}

/* ---- persistence -------------------------------------------------------------------- */

static void save_changes(struct deskbuddy_app *a)
{
    bool failed = false;

    if (a->sim) {
        /* A simulation writes nothing: the demo must not become the owner's
         * log or preferences. */
        a->prefs_saved = a->brain.prefs_rev;
        a->log_saved = a->brain.log_rev;
        return;
    }
    if (a->prefs_saved != a->brain.prefs_rev) {
        if (db_store_save_prefs(&a->brain.prefs) == 0) {
            a->prefs_saved = a->brain.prefs_rev;
        } else {
            failed = true;
        }
    }
    if (a->log_saved != a->brain.log_rev) {
        if (db_store_save_guard(&a->log) == 0) {
            a->log_saved = a->brain.log_rev;
        } else {
            failed = true;
        }
    }
    a->save_failed = failed;
}

/* ---- the face --------------------------------------------------------------------- */

static void paint_eye(struct deskbuddy_eye *e, const struct db_eye_shape *s, bool dim)
{
    lv_obj_set_size(e->white, s->w, s->h);
    lv_obj_set_style_radius(e->white, s->radius, 0);
    lv_obj_set_style_bg_opa(e->white, dim ? DIM_OPA : LV_OPA_COVER, 0);
    lv_obj_align(e->white, LV_ALIGN_CENTER, 0, s->dy);
    set_hidden(e->pupil, s->pupil_d == 0);
    if (s->pupil_d > 0) {
        lv_obj_set_size(e->pupil, s->pupil_d, s->pupil_d);
        lv_obj_align(e->pupil, LV_ALIGN_CENTER, s->pupil_dx, s->pupil_dy);
    }
    set_hidden(e->lid, s->lid == 0);
    if (s->lid > 0) {
        lv_obj_set_size(e->lid, s->w, s->lid);
        lv_obj_set_pos(e->lid, 0, 0);
    }
    set_hidden(e->arch, s->arch_d == 0);
    if (s->arch_d > 0) {
        lv_obj_set_size(e->arch, s->arch_d, s->arch_d);
        lv_obj_set_pos(e->arch, (s->w - s->arch_d) / 2, s->arch_y);
    }
}

static void apply_face(struct deskbuddy_app *a, bool force)
{
    struct db_face f;
    struct db_eye_shape s[2];
    int k;

    if (a->eye_box <= 0) {
        return;
    }
    db_brain_face(&a->brain, &f);
    db_face_eyes(&f, a->eye_box, a->eye_box, &s[0], &s[1]);
    if (!force && a->face_valid && f.dim == a->face_drawn.dim && db_eye_shape_equal(&s[0], &a->eye[0].drawn) &&
        db_eye_shape_equal(&s[1], &a->eye[1].drawn)) {
        return;
    }
    for (k = 0; k < 2; k++) {
        paint_eye(&a->eye[k], &s[k], f.dim);
        a->eye[k].drawn = s[k];
    }
    a->face_drawn = f;
    a->face_valid = true;
    a->face_paints++;
}

/* ---- text and controls ----------------------------------------------------------- */

static const char *caption_for(const struct deskbuddy_app *a)
{
    const struct db_brain *b = &a->brain;
    static char buf[64];
    unsigned n;

    switch (b->state) {
    case DB_ST_OWNER_GREETING:
        return b->prefs.on[DB_PREF_GREETING] ? "HELLO" : "";
    case DB_ST_UNKNOWN_REACTION:
        return "HM. WHO'S THIS?";
    case DB_ST_RECOGNIZING:
        return "...";
    case DB_ST_GUARD_DISARMED:
        return "GUARD OFF";
    case DB_ST_GUARD_ARMING:
        return "ARMED \xC2\xB7 GO AHEAD, I'LL WATCH";
    case DB_ST_GUARD_ARMED:
        return "WATCHING THE DESK";
    case DB_ST_GUARD_PERSON_DETECTED:
        return "SOMEONE'S HERE";
    case DB_ST_GUARD_UNKNOWN:
        return "VISITOR NOTED";
    case DB_ST_GUARD_OWNER:
        return b->prefs.on[DB_PREF_GREETING] ? "WELCOME BACK" : "";
    case DB_ST_GUARD_ALERT_PENDING:
        n = db_guard_unacknowledged(b->log);
        snprintf(buf, sizeof(buf), "%u VISIT%s WHILE YOU WERE AWAY", n, n == 1 ? "" : "S");
        return buf;
    default:
        return "";
    }
}

static const char *note_for(const struct deskbuddy_app *a)
{
    const struct db_brain *b = &a->brain;
    enum db_mode mode = db_state_mode(b->state);

    if (a->save_failed) {
        return "NOT SAVED";
    }
    if (mode == DB_MODE_COMPANION && !b->prefs.on[DB_PREF_COMPANION]) {
        return "RESTING";
    }
    if (a->sim) {
        return "SIMULATED VISION";
    }
    if (b->seen == DB_SEEN_UNAVAILABLE && mode != DB_MODE_NIGHT) {
        return "NO VISION YET";
    }
    return "";
}

static void refresh_log(struct deskbuddy_app *a, bool show)
{
    int i;

    for (i = 0; i < LOG_LINES; i++) {
        const struct db_guard_event *e = db_guard_at(&a->log, (unsigned)i);
        char t[8];
        char line[48];

        if (!show || !e) {
            set_hidden(a->log_line[i], true);
            continue;
        }
        hhmm(e->wall_s, t, sizeof(t));
        snprintf(line, sizeof(line), "%s  %s%s", t,
                 e->subject == DB_SUBJECT_OWNER ? "YOU, BACK"
                                                : (e->subject == DB_SUBJECT_UNKNOWN ? "UNKNOWN" : "SOMEONE"),
                 e->acknowledged || e->subject == DB_SUBJECT_OWNER ? "" : "  NEW");
        set_text(a->log_line[i], line);
        set_hidden(a->log_line[i], false);
    }
}

static void refresh_controls(struct deskbuddy_app *a)
{
    const struct db_brain *b = &a->brain;
    enum db_mode mode = db_state_mode(b->state);
    enum action_cmd cmd = ACT_NONE;
    const char *text = "";
    int m;

    for (m = 0; m < DB_MODE_COUNT; m++) {
        set_hidden(a->mode_btn[m], !db_prefs_mode_allowed(&b->prefs, (enum db_mode)m));
        set_primary(a->mode_btn[m], m == (int)mode && db_prefs_mode_allowed(&b->prefs, (enum db_mode)m));
    }
    if (mode == DB_MODE_GUARD) {
        if (b->state == DB_ST_GUARD_ALERT_PENDING) {
            cmd = ACT_ACK;
            text = "SEEN IT";
        } else if (db_state_armed(b->state)) {
            cmd = ACT_DISARM;
            text = "DISARM";
        } else if (b->prefs.on[DB_PREF_GUARD]) {
            cmd = ACT_ARM;
            text = "ARM";
        }
    }
    set_hidden(a->action, cmd == ACT_NONE);
    if (cmd != ACT_NONE) {
        set_text(lv_obj_get_child(a->action, 0), text);
        lv_obj_set_user_data(a->action, (void *)(intptr_t)cmd);
        set_primary(a->action, cmd != ACT_DISARM);
    }
}

static void refresh_clock(struct deskbuddy_app *a)
{
    char t[8];

    hhmm(wall_now(), t, sizeof(t));
    set_text(a->clock, t);
}

static void refresh_ui(struct deskbuddy_app *a, bool force)
{
    const struct db_brain *b = &a->brain;
    enum db_mode mode = db_state_mode(b->state);

    if (!force && a->shown_valid && a->shown_state == b->state && a->shown_seen == b->seen &&
        a->shown_log_rev == b->log_rev && a->shown_prefs_rev == b->prefs_rev) {
        return;
    }
    set_text(a->caption, caption_for(a));
    set_text(a->note, note_for(a));
    set_hidden(a->clock, mode != DB_MODE_NIGHT);
    if (mode == DB_MODE_NIGHT) {
        refresh_clock(a);
    }
    refresh_log(a, mode == DB_MODE_GUARD);
    refresh_controls(a);
    a->shown_state = b->state;
    a->shown_seen = b->seen;
    a->shown_log_rev = b->log_rev;
    a->shown_prefs_rev = b->prefs_rev;
    a->shown_valid = true;
}

static void refresh_toggles(struct deskbuddy_app *a)
{
    int k;

    for (k = 0; k < DB_PREF_TOGGLE_COUNT; k++) {
        bool on = a->brain.prefs.on[k];

        set_text(lv_obj_get_child(a->toggle[k], 0), on ? "ON" : "OFF");
        set_primary(a->toggle[k], on);
    }
}

/* ---- layout ------------------------------------------------------------------------- */

static void apply_layout(struct deskbuddy_app *a)
{
    int32_t w;
    int32_t h;
    int32_t box;
    bool landscape;
    int k;

    a->layout_pending = false;
    /* Measure a settled root: on the first run it has not been laid out
     * yet and reads 0 x 0, which is "portrait" on any display. This runs
     * from the timer or create(), never inside LVGL's layout pass. */
    lv_obj_update_layout(a->root);
    landscape = lv_obj_get_content_width(a->root) > lv_obj_get_content_height(a->root);
    if (landscape != a->landscape || a->face_w == 0) {
        a->landscape = landscape;
        lv_obj_set_flex_flow(a->root, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
        /* Landscape: a full-height column with the controls at its foot.
         * Portrait: a column as tall as its content - which LVGL measures
         * from the top, so it must be packed from the top too (packed from
         * the foot it measured nothing above its first row, and the face
         * grew over the buttons). */
        if (landscape) {
            lv_obj_set_size(a->face, 1, LV_PCT(100));
            lv_obj_set_size(a->side, SIDE_W, LV_PCT(100));
        } else {
            lv_obj_set_size(a->face, LV_PCT(100), 1);
            lv_obj_set_size(a->side, LV_PCT(100), LV_SIZE_CONTENT);
        }
        lv_obj_set_flex_align(a->side, landscape ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_flex_grow(a->face, 1);
    }
    lv_obj_update_layout(a->root);
    w = lv_obj_get_width(a->face);
    h = lv_obj_get_height(a->face);
    if (w == a->face_w && h == a->face_h && a->eye_box > 0) {
        return;
    }
    a->face_w = w;
    a->face_h = h;
    /* Two eyes side by side with a gap of a third of one: 2 boxes + gap in
     * the width, one box in the height, never bigger than looks friendly. */
    box = w * 3 / 7;
    if (box > h * 4 / 5) {
        box = h * 4 / 5;
    }
    if (box > 240) {
        box = 240;
    }
    if (box < 16) {
        box = 16;
    }
    a->eye_box = box;
    for (k = 0; k < 2; k++) {
        lv_obj_set_size(a->eye[k].box, box, box);
        lv_obj_set_pos(a->eye[k].box, w / 2 + (k == 0 ? -box - box / 6 : box / 6), (h - box) / 2);
    }
    apply_face(a, true);
}

static void on_size(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);

    /* Never lay out inside LVGL's layout pass: the timer does it. */
    a->layout_pending = true;
    if (a->timer) {
        lv_timer_ready(a->timer);
    }
}

/* ---- the timer --------------------------------------------------------------------- */

static void pump_vision(struct deskbuddy_app *a, int64_t now)
{
    struct db_vision_event ev;
    int n = 0;

    if (a->provider.ops && a->provider_next >= 0 && a->provider_next <= now) {
        a->provider_next = a->provider.ops->poll(a->provider.ctx, now, &a->queue);
    }
    while (n++ < DB_VISION_QUEUE_CAP && db_vision_queue_pop(&a->queue, &ev)) {
        db_brain_vision(&a->brain, &ev, now);
    }
}

static void step(struct deskbuddy_app *a)
{
    int64_t now = app_now(a);
    int64_t next;
    int64_t delay;

    a->steps++;
    if (a->layout_pending) {
        apply_layout(a);
    }
    pump_vision(a, now);
    db_brain_tick(&a->brain, now);
    apply_face(a, false);
    refresh_ui(a, false);
    next = db_brain_next_ms(&a->brain);
    if (a->provider_next >= 0 && a->provider_next < next) {
        next = a->provider_next;
    }
    delay = next == DB_NEVER ? TIMER_MAX_MS : next - now;
    if (delay < TIMER_MIN_MS) {
        delay = TIMER_MIN_MS;
    } else if (delay > TIMER_MAX_MS) {
        delay = TIMER_MAX_MS;
    }
    lv_timer_set_period(a->timer, (uint32_t)delay);
}

static void on_timer(lv_timer_t *t)
{
    step(lv_timer_get_user_data(t));
}

/* ---- input --------------------------------------------------------------------------- */

static void on_mode(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);
    int m;

    for (m = 0; m < DB_MODE_COUNT; m++) {
        if (a->mode_btn[m] == btn) {
            db_brain_set_mode(&a->brain, (enum db_mode)m, app_now(a));
        }
    }
    lv_timer_ready(a->timer);
}

static void on_action(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);
    int64_t now = app_now(a);

    switch ((enum action_cmd)(intptr_t)lv_obj_get_user_data(a->action)) {
    case ACT_ARM:
        db_brain_arm(&a->brain, now);
        break;
    case ACT_DISARM:
        db_brain_disarm(&a->brain, now);
        break;
    case ACT_ACK:
        db_brain_acknowledge(&a->brain, now);
        break;
    default:
        break;
    }
    lv_timer_ready(a->timer);
}

static void on_face(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);

    db_brain_poke(&a->brain, app_now(a));
    lv_timer_ready(a->timer);
}

static void show_settings(struct deskbuddy_app *a, bool show)
{
    if (show) {
        refresh_toggles(a);
    }
    set_hidden(a->panel, !show);
}

static void on_settings(lv_event_t *e)
{
    show_settings(lv_event_get_user_data(e), true);
}

static void on_done(lv_event_t *e)
{
    show_settings(lv_event_get_user_data(e), false);
}

static void on_toggle(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);
    struct db_prefs p = a->brain.prefs;
    int k;

    for (k = 0; k < DB_PREF_TOGGLE_COUNT; k++) {
        if (a->toggle[k] == btn) {
            p.on[k] = !p.on[k];
        }
    }
    db_brain_set_prefs(&a->brain, &p, app_now(a));
    refresh_toggles(a);
    lv_timer_ready(a->timer);
}

/* Developer keys, only with $DESKBUDDY_SIM set (docs/apps/DESKBUDDY.md):
 * 0 nobody, 1 a person, 2 the owner, 3 a stranger, 4 vision lost. Esc
 * closes the settings panel in any build. */
static void on_key(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);

    if (key == LV_KEY_ESC && !lv_obj_has_flag(a->panel, LV_OBJ_FLAG_HIDDEN)) {
        show_settings(a, false);
        return;
    }
    if (a->sim && key >= '0' && key < '0' + DB_VISION_KIND_COUNT) {
        db_vision_mock_inject(&a->mock, (enum db_vision_kind)(key - '0'), app_now(a), &a->queue);
        lv_timer_ready(a->timer);
    }
}

/* ---- building ----------------------------------------------------------------------- */

static void build_eye(struct deskbuddy_eye *e, lv_obj_t *face)
{
    e->box = plain(face);
    /* The accent fill without a chip's geometry: CHIP_ACTIVE is colour
     * only. The cut-outs are the background: SCREEN. */
    e->white = plain(e->box);
    pos_style_add(e->white, POS_STYLE_CHIP_ACTIVE, 0);
    lv_obj_set_style_bg_opa(e->white, LV_OPA_COVER, 0);
    e->pupil = plain(e->white);
    pos_style_add(e->pupil, POS_STYLE_SCREEN, 0);
    lv_obj_set_style_radius(e->pupil, LV_RADIUS_CIRCLE, 0);
    e->lid = plain(e->white);
    pos_style_add(e->lid, POS_STYLE_SCREEN, 0);
    lv_obj_add_flag(e->lid, LV_OBJ_FLAG_HIDDEN);
    e->arch = plain(e->white);
    pos_style_add(e->arch, POS_STYLE_SCREEN, 0);
    lv_obj_set_style_radius(e->arch, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(e->arch, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *centred_label(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *l = pocketui_label(parent, "", role);

    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

static void build_settings(struct deskbuddy_app *a)
{
    lv_obj_t *list;
    int k;

    a->panel = plain(a->root);
    lv_obj_add_flag(a->panel, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(a->panel, LV_OBJ_FLAG_CLICKABLE); /* taps stop here, not on the face */
    lv_obj_set_size(a->panel, LV_PCT(100), LV_PCT(100));
    pos_style_add(a->panel, POS_STYLE_SCREEN, 0);
    lv_obj_set_flex_flow(a->panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(a->panel, GAP, 0);
    list = plain(a->panel);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_add_flag(list, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    pocketui_label(list, "DESKBUDDY SETTINGS", POS_STYLE_CAPTION);
    for (k = 0; k < DB_PREF_TOGGLE_COUNT; k++) {
        lv_obj_t *row = plain(list);
        lv_obj_t *label;

        lv_obj_set_size(row, LV_PCT(100), BUTTON_H);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, GAP, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        label = pocketui_label(row, db_pref_label((enum db_pref)k), POS_STYLE_ROW_TITLE);
        lv_obj_set_flex_grow(label, 1);
        a->toggle[k] = button(row, "ON", on_toggle, a);
        lv_obj_set_width(a->toggle[k], 112);
    }
    a->done = button(a->panel, "DONE", on_done, a);
    lv_obj_set_width(a->done, LV_PCT(100));
    style_button(a->done, true);
    lv_obj_add_flag(a->done, LV_OBJ_FLAG_USER_1);
    lv_obj_add_flag(a->panel, LV_OBJ_FLAG_HIDDEN);
}

static void build(struct deskbuddy_app *a, lv_obj_t *body)
{
    static const char *const mode_text[DB_MODE_COUNT] = { "BUDDY", "GUARD", "NIGHT" };
    lv_obj_t *row;
    int k;

    a->root = plain(body);
    lv_obj_set_width(a->root, LV_PCT(100));
    lv_obj_set_flex_grow(a->root, 1);
    lv_obj_add_flag(a->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(a->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(a->root, GAP, 0);
    lv_obj_set_style_pad_column(a->root, GAP, 0);

    a->face = plain(a->root);
    lv_obj_add_flag(a->face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->face, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    for (k = 0; k < 2; k++) {
        build_eye(&a->eye[k], a->face);
    }

    a->side = plain(a->root);
    lv_obj_set_flex_flow(a->side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->side, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(a->side, 8, 0);
    lv_obj_add_flag(a->side, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    a->clock = centred_label(a->side, POS_STYLE_HERO_48);
    a->caption = centred_label(a->side, POS_STYLE_ROW_TITLE);
    a->note = centred_label(a->side, POS_STYLE_CAPTION);
    for (k = 0; k < LOG_LINES; k++) {
        a->log_line[k] = centred_label(a->side, POS_STYLE_CAPTION);
    }
    a->action = button(a->side, "ARM", on_action, a);
    lv_obj_set_width(a->action, LV_PCT(100));
    row = plain(a->side);
    a->modes = row;
    lv_obj_set_size(row, LV_PCT(100), BUTTON_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    for (k = 0; k < DB_MODE_COUNT; k++) {
        a->mode_btn[k] = button(row, mode_text[k], on_mode, a);
        lv_obj_set_flex_grow(a->mode_btn[k], 1);
    }
    a->settings_btn = button(row, "SET", on_settings, a);
    lv_obj_set_width(a->settings_btn, 80);

    build_settings(a);

    lv_obj_add_event_cb(a->face, on_face, LV_EVENT_CLICKED, a);
    lv_obj_add_event_cb(a->root, on_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_add_event_cb(a->root, on_key, LV_EVENT_KEY, a);
    pos_input_add_obj(a->root);
    pos_input_focus(a->root);
}

/* The provider: the script in $DESKBUDDY_SIM ("keys" gives the developer
 * keys with nothing scripted); otherwise the Vision pipeline - the camera
 * and the KPU in Vision's helper - unless $DESKBUDDY_VISION is "none". */
static void start_provider(struct deskbuddy_app *a, int64_t now)
{
    const char *sim = getenv("DESKBUDDY_SIM");
    const char *vision = getenv("DESKBUDDY_VISION");

    a->provider.ops = &db_vision_none_ops;
    a->provider.ctx = NULL;
    if (!(sim && *sim) && !(vision && strcmp(vision, "none") == 0)) {
        a->pipeline_cfg.display_rotation = pos_rotation_degrees(pocketui_display_geometry()->rotation);
        a->provider.ops = &db_vision_pipeline_ops;
        a->provider.ctx = &a->pipeline_cfg;
    }
    if (sim && *sim) {
        a->sim = true;
        if (strcmp(sim, "keys") == 0 || db_vision_mock_load(&a->mock, sim) > 0) {
            a->provider.ops = &db_vision_mock_ops;
            a->provider.ctx = &a->mock;
        }
    }
    a->provider_next = a->provider.ops->start(a->provider.ctx, now, &a->queue) == 0 ? now : -1;
}

/* $DESKBUDDY_MODE (simulation only): start in companion, guard, night or
 * armed, whatever the preferences say. */
static void sim_mode(struct deskbuddy_app *a, int64_t now)
{
    const char *want = getenv("DESKBUDDY_MODE");
    int m;

    if (!a->sim || !want) {
        return;
    }
    if (strcmp(want, "armed") == 0) {
        db_brain_set_mode(&a->brain, DB_MODE_GUARD, now);
        db_brain_arm(&a->brain, now);
        return;
    }
    for (m = 0; m < DB_MODE_COUNT; m++) {
        if (strcmp(want, db_mode_name((enum db_mode)m)) == 0) {
            db_brain_set_mode(&a->brain, (enum db_mode)m, now);
        }
    }
}

static void *deskbuddy_create(lv_obj_t *body)
{
    struct deskbuddy_app *a = lv_malloc_zeroed(sizeof(*a));
    struct db_prefs prefs;
    int64_t now;

    if (!a) {
        return NULL;
    }
    a->last_tick = lv_tick_get();
    now = app_now(a);
    db_vision_queue_init(&a->queue);
    start_provider(a, now);
    if (a->sim) {
        db_prefs_defaults(&prefs);
        db_guard_init(&a->log);
    } else {
        db_store_load_prefs(&prefs);
        db_store_load_guard(&a->log);
    }
    db_brain_init(&a->brain, &prefs, &a->log, (uint32_t)time(NULL) ^ lv_tick_get() ^ 0xDB0Bu, now);
    db_brain_set_wall(&a->brain, wall_now());
    db_brain_set_reduced_motion(&a->brain, pocketos_shell_reduced_motion() != 0, now);
    sim_mode(a, now);

    build(a, body);
    a->layout_pending = true;
    a->timer = lv_timer_create(on_timer, TIMER_MIN_MS, a);
    step(a);
    refresh_ui(a, true);
    return a;
}

static void deskbuddy_tick(void *priv)
{
    struct deskbuddy_app *a = priv;
    int64_t now;

    if (!a) {
        return;
    }
    now = app_now(a);
    db_brain_set_wall(&a->brain, wall_now());
    db_brain_set_reduced_motion(&a->brain, pocketos_shell_reduced_motion() != 0, now);
    if (db_state_mode(a->brain.state) == DB_MODE_NIGHT) {
        refresh_clock(a);
    }
    save_changes(a);
    /* The eyes take their colours from role styles, so a theme change needs
     * nothing here. */
    if (a->save_failed) {
        refresh_ui(a, true);
    }
}

static void deskbuddy_destroy(void *priv)
{
    struct deskbuddy_app *a = priv;

    if (!a) {
        return;
    }
    if (a->provider.ops) {
        a->provider.ops->stop(a->provider.ctx);
    }
    save_changes(a);
    if (a->timer) {
        lv_timer_delete(a->timer);
        a->timer = NULL;
    }
    /* Our objects go now, with their callbacks, rather than when the shell
     * deletes the body a moment after this app is freed. */
    lv_obj_delete(a->root);
    lv_free(a);
}

/* ---- for tests (deskbuddy_app.h) --------------------------------------------------- */

const struct db_brain *deskbuddy_app_brain(void *priv)
{
    return priv ? &((struct deskbuddy_app *)priv)->brain : NULL;
}

lv_timer_t *deskbuddy_app_timer(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->timer : NULL;
}

void deskbuddy_app_inject(void *priv, enum db_vision_kind kind)
{
    struct deskbuddy_app *a = priv;
    struct db_vision_event ev = { .kind = kind, .confidence_pm = DB_CONF_NONE };

    if (!a) {
        return;
    }
    ev.mono_ms = app_now(a);
    db_vision_queue_push(&a->queue, &ev);
    step(a);
}

void deskbuddy_app_pump(void *priv)
{
    if (priv) {
        step(priv);
    }
}

lv_obj_t *deskbuddy_app_mode_button(void *priv, enum db_mode mode)
{
    return priv && (int)mode >= 0 && mode < DB_MODE_COUNT ? ((struct deskbuddy_app *)priv)->mode_btn[mode] : NULL;
}

lv_obj_t *deskbuddy_app_action_button(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->action : NULL;
}

lv_obj_t *deskbuddy_app_settings_button(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->settings_btn : NULL;
}

lv_obj_t *deskbuddy_app_settings_panel(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->panel : NULL;
}

lv_obj_t *deskbuddy_app_toggle(void *priv, enum db_pref pref)
{
    return priv && (int)pref >= 0 && pref < DB_PREF_TOGGLE_COUNT ? ((struct deskbuddy_app *)priv)->toggle[pref]
                                                                 : NULL;
}

lv_obj_t *deskbuddy_app_settings_done(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->done : NULL;
}

lv_obj_t *deskbuddy_app_face(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->face : NULL;
}

lv_obj_t *deskbuddy_app_caption(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->caption : NULL;
}

lv_obj_t *deskbuddy_app_clock(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->clock : NULL;
}

lv_obj_t *deskbuddy_app_eye(void *priv, int index)
{
    return priv && (index == 0 || index == 1) ? ((struct deskbuddy_app *)priv)->eye[index].white : NULL;
}

unsigned deskbuddy_app_face_paints(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->face_paints : 0;
}

unsigned deskbuddy_app_steps(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->steps : 0;
}

/* The first-party launcher mask (docs/design/doors-app-icons); .icon stays
 * the text fallback for a shell without the mask. */
LV_IMAGE_DECLARE(pos_app_icon_deskbuddy);

/* The Back action (app.h `back`, hw_actions.h): the settings panel closes, as
 * Esc closes it; the face is the top level. */
static int deskbuddy_back(void *priv)
{
    struct deskbuddy_app *a = priv;

    if (!a || !a->panel || lv_obj_has_flag(a->panel, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    show_settings(a, false);
    return 1;
}

const struct pocketos_app app_deskbuddy = {
    .id = "deskbuddy",
    .name = "DeskBuddy",
    .icon = LV_SYMBOL_EYE_OPEN,
    .icon_mask = &pos_app_icon_deskbuddy,
    .create = deskbuddy_create,
    .tick = deskbuddy_tick,
    .destroy = deskbuddy_destroy,
    /* Fullscreen, like the games (DS §36): no status cluster over the face
     * or the night clock; the shell's header keeps the way back. */
    .chrome = POCKETOS_CHROME_NONE,
    .back = deskbuddy_back,
};
