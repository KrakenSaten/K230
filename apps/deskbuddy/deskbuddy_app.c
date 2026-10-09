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
 * DRAWING. The character is a group - two eyes and a mouth - moved as one
 * by a lean, a hop or a breath. The eyes are plain filled objects in role
 * styles (DS §4) - a rounded white in the accent, a pupil, a lid and an
 * arch cut-out in the background colour - and the mouth is an accent body
 * with one background cut-out; both are restyled only when db_face gives a
 * different shape. Night dims them by opacity, not by a colour of their
 * own. In Companion a new face is tweened from the one drawn over a few
 * frames (TWEEN_MS; a blink faster), so expressions grow out of each other;
 * Guard and Night step from face to face as before. Under reduced motion it
 * is a step, and the idle behaviour stops (DS §12).
 *
 * TOUCH (Companion). The face area sorts a finger's path (db_gesture.h):
 * a tap pokes the character where it was touched, a gentle stroke pets it.
 * The buttons are siblings of the face, so a tap on one never reaches it.
 * FEED puts a snack beside the character; drag it to the mouth, or tap it
 * (or GIVE, or Enter) and it floats there. Let go anywhere else, or lose
 * the press, and it goes back to its place; Esc, Back, a mode change or 20
 * quiet seconds put it away. REST and WAKE send it to sleep and back. All
 * of it reaches the brain as a db_stimulus - the same door a future vision
 * gesture would use - and none of it touches the vision provider.
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
#include "db_gesture.h"
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
#define TWEEN_MS 160        /* one expression into the next */
#define BLINK_TWEEN_MS 70   /* into or out of shut eyes */
#define FRAME_MS 33         /* the timer's pace while something moves */
#define GLIDE_MS 320        /* a snack floating to the mouth, or home */
#define SNACK_BOX 72        /* the snack's touch area, over the DS 64 px minimum */
#define SNACK_D 52          /* the snack itself */
_Static_assert(SNACK_BOX >= POCKETUI_TOUCH_MIN, "the snack must meet the DS 64 px touch minimum");

struct deskbuddy_eye {
    lv_obj_t *box;
    lv_obj_t *white;        /* the accent (a role style: no colour here) */
    lv_obj_t *pupil;        /* the background colour, as are the two cut-outs */
    lv_obj_t *lid;
    lv_obj_t *arch;
    struct db_eye_shape drawn;
};

/* The look being drawn: both eyes, the mouth and the group's shift. */
struct deskbuddy_look {
    struct db_eye_shape eye[2];
    struct db_mouth_shape mouth;
    int off_x;
    int off_y;
};

struct deskbuddy_app {
    struct db_brain brain;
    struct db_guard_log log;
    struct db_vision_queue queue;
    struct db_vision_provider provider;
    struct db_vision_pipeline_cfg pipeline_cfg;
    struct db_vision_mock mock;
    int64_t provider_next;       /* -1: nothing scheduled */
    unsigned provider_starts;    /* once per open: play never starts one */
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
    int char_w;                  /* the character group, and where it rests */
    int char_h;
    int char_x;
    int char_y;
    struct db_face face_drawn;
    bool face_valid;
    unsigned face_paints;
    /* The tween: from what was drawn to the face the brain wants. */
    struct deskbuddy_look shown;     /* on screen now */
    struct deskbuddy_look tw_from;
    struct deskbuddy_look tw_to;
    int64_t tw_start;
    int tw_ms;
    bool tw_active;
    /* Touch on the face, and the snack. */
    struct db_gesture gesture;
    bool snack_out;
    bool snack_drag;
    bool snack_glide;
    bool snack_feed;             /* the glide ends at the mouth: eaten */
    lv_point_t snack_grab;       /* the finger's offset in the snack */
    lv_point_t snack_press;      /* where the press began, for a tap */
    int64_t snack_press_ms;
    lv_point_t snack_pos;        /* top-left, in the face */
    lv_point_t glide_from;
    lv_point_t glide_to;
    int64_t glide_start;
    unsigned steps;              /* timer runs, for the wake-up budget test */
    enum db_state shown_state;
    enum db_seen shown_seen;
    unsigned shown_log_rev;
    unsigned shown_prefs_rev;
    bool shown_valid;
    lv_timer_t *timer;
    lv_obj_t *root;
    lv_obj_t *face;
    lv_obj_t *character;         /* the eyes and the mouth, moved as one */
    struct deskbuddy_eye eye[2];
    lv_obj_t *mouth;             /* the box that clips the mouth's body */
    lv_obj_t *mouth_body;
    lv_obj_t *mouth_cut;
    lv_obj_t *snack;
    lv_obj_t *side;
    lv_obj_t *care;              /* FEED and REST, in Companion */
    lv_obj_t *feed_btn;
    lv_obj_t *rest_btn;
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

static void paint_mouth(struct deskbuddy_app *a, const struct db_mouth_shape *m, bool dim)
{
    set_hidden(a->mouth, m->w <= 0 || m->h <= 0);
    if (m->w <= 0 || m->h <= 0) {
        return;
    }
    lv_obj_set_size(a->mouth, m->w, m->h);
    lv_obj_set_pos(a->mouth, a->char_w / 2 + m->dx - m->w / 2,
                   a->eye_box / 2 + a->eye_box * DB_MOUTH_Y_PM / 1000 - m->h / 2);
    lv_obj_set_size(a->mouth_body, m->body_w, m->body_h);
    lv_obj_set_pos(a->mouth_body, m->body_x, m->body_y);
    lv_obj_set_style_radius(a->mouth_body, m->body_r, 0);
    lv_obj_set_style_bg_opa(a->mouth_body, dim ? DIM_OPA : LV_OPA_COVER, 0);
    set_hidden(a->mouth_cut, m->cut_w <= 0 || m->cut_h <= 0);
    if (m->cut_w > 0 && m->cut_h > 0) {
        lv_obj_set_size(a->mouth_cut, m->cut_w, m->cut_h);
        lv_obj_set_style_radius(a->mouth_cut, m->cut_r, 0);
        lv_obj_set_pos(a->mouth_cut, m->cut_x, m->cut_y);
    }
}

static bool look_equal(const struct deskbuddy_look *p, const struct deskbuddy_look *q)
{
    return db_eye_shape_equal(&p->eye[0], &q->eye[0]) && db_eye_shape_equal(&p->eye[1], &q->eye[1]) &&
           db_mouth_shape_equal(&p->mouth, &q->mouth) && p->off_x == q->off_x && p->off_y == q->off_y;
}

/* Put a look on the screen, restyling only what changed. */
static void draw_look(struct deskbuddy_app *a, const struct deskbuddy_look *l, bool dim, bool force)
{
    int k;

    if (!force && a->face_valid && dim == a->face_drawn.dim && look_equal(l, &a->shown)) {
        return;
    }
    for (k = 0; k < 2; k++) {
        if (force || !db_eye_shape_equal(&l->eye[k], &a->shown.eye[k]) || dim != a->face_drawn.dim) {
            paint_eye(&a->eye[k], &l->eye[k], dim);
        }
        a->eye[k].drawn = l->eye[k];
    }
    if (force || !db_mouth_shape_equal(&l->mouth, &a->shown.mouth) || dim != a->face_drawn.dim) {
        paint_mouth(a, &l->mouth, dim);
    }
    if (force || l->off_x != a->shown.off_x || l->off_y != a->shown.off_y) {
        lv_obj_set_pos(a->character, a->char_x + l->off_x, a->char_y + l->off_y);
    }
    a->shown = *l;
    a->face_paints++;
}

/* Eased: slow at both ends, per-mille in and out. */
static int ease(int p)
{
    int64_t q = p;

    return (int)(q * q * (3000 - 2 * q) / 1000000);
}

static void mix_look(const struct deskbuddy_look *p, const struct deskbuddy_look *q, int permille,
                     struct deskbuddy_look *out)
{
    int k;

    for (k = 0; k < 2; k++) {
        db_eye_shape_mix(&p->eye[k], &q->eye[k], permille, &out->eye[k]);
    }
    db_mouth_shape_mix(&p->mouth, &q->mouth, permille, &out->mouth);
    out->off_x = p->off_x + (q->off_x - p->off_x) * permille / 1000;
    out->off_y = p->off_y + (q->off_y - p->off_y) * permille / 1000;
}

/* The face the brain wants, drawn now or tweened toward from what is on the
 * screen. force: draw it at once (a new layout). */
static void apply_face(struct deskbuddy_app *a, bool force)
{
    struct db_face f;
    struct deskbuddy_look want;
    int64_t now = a->mono;

    if (a->eye_box <= 0) {
        return;
    }
    db_brain_face(&a->brain, &f);
    db_face_eyes(&f, a->eye_box, a->eye_box, &want.eye[0], &want.eye[1]);
    db_face_mouth(&f, a->eye_box, &want.mouth);
    want.off_x = f.off_x * a->eye_box / 1000;
    want.off_y = f.off_y * a->eye_box / 1000;
    /* Guard and Night step from face to face as they always did; only
     * Companion, where the character plays, is tweened. */
    if (force || !a->face_valid || a->brain.reduced_motion || db_state_mode(a->brain.state) != DB_MODE_COMPANION) {
        a->tw_active = false;
        a->tw_to = want;
        draw_look(a, &want, f.dim, force || !a->face_valid);
        a->face_drawn = f;
        a->face_valid = true;
        return;
    }
    if (!look_equal(&want, &a->tw_to) && !a->tw_active && db_eye_shape_equal(&want.eye[0], &a->shown.eye[0]) &&
        db_eye_shape_equal(&want.eye[1], &a->shown.eye[1]) && db_mouth_shape_equal(&want.mouth, &a->shown.mouth)) {
        /* Only the group moves - a breath, a hop, a shake: one step, one
         * repaint, which is what makes a shake a shake. */
        a->tw_to = want;
        draw_look(a, &want, f.dim, false);
        a->face_drawn = f;
        return;
    }
    if (!look_equal(&want, &a->tw_to)) {
        bool blink = f.expr == DB_EXPR_CLOSED || a->face_drawn.expr == DB_EXPR_CLOSED;

        a->tw_from = a->shown;
        a->tw_to = want;
        a->tw_start = now;
        a->tw_ms = blink ? BLINK_TWEEN_MS : TWEEN_MS;
        a->tw_active = true;
        a->face_drawn.expr = f.expr;
    }
    if (a->tw_active) {
        int p = (int)((now - a->tw_start) * 1000 / a->tw_ms);
        struct deskbuddy_look mid;

        if (p >= 1000) {
            a->tw_active = false;
            mid = a->tw_to;
        } else {
            mix_look(&a->tw_from, &a->tw_to, ease(p < 0 ? 0 : p), &mid);
        }
        draw_look(a, &mid, f.dim, false);
    } else if (f.dim != a->face_drawn.dim) {
        draw_look(a, &a->shown, f.dim, true);
    }
    a->face_drawn = f;
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

/* ---- stimuli and the snack --------------------------------------------------------- */

/* Tell the brain; a point is in the face's coordinates, and the brain gets
 * it from the middle of the eye line in per-mille of an eye box. */
static void stimulate(struct deskbuddy_app *a, enum db_stim_kind kind, enum db_stim_source src, lv_point_t at)
{
    struct db_stimulus s = { kind, src, 0, 0 };

    if (a->eye_box > 0) {
        s.x_pm = (int)((at.x - (a->char_x + a->char_w / 2)) * 1000 / a->eye_box);
        s.y_pm = (int)((at.y - (a->char_y + a->eye_box / 2)) * 1000 / a->eye_box);
    }
    db_brain_stimulus(&a->brain, &s, app_now(a));
    if (a->timer) {
        lv_timer_ready(a->timer);
    }
}

static lv_point_t eye_line_centre(const struct deskbuddy_app *a)
{
    lv_point_t p = { a->char_x + a->char_w / 2, a->char_y + a->eye_box / 2 };

    return p;
}

/* The finger, in the face's coordinates. */
static lv_point_t finger_in_face(const struct deskbuddy_app *a)
{
    lv_point_t p = { 0, 0 };
    lv_indev_t *indev = lv_indev_active();
    lv_area_t f;

    if (indev) {
        lv_indev_get_point(indev, &p);
    }
    lv_obj_get_coords(a->face, &f);
    p.x -= f.x1;
    p.y -= f.y1;
    return p;
}

static bool overlaps(int x, int y, int w, int h, int x2, int y2, int w2, int h2)
{
    return x < x2 + w2 && x2 < x + w && y < y2 + h2 && y2 < y + h;
}

/* The snack's place: a lower corner of the face, clear of the character. */
static lv_point_t snack_home(const struct deskbuddy_app *a)
{
    const int inset = 8;
    lv_point_t c[3] = {
        { a->face_w - SNACK_BOX - inset, a->face_h - SNACK_BOX - inset },
        { inset, a->face_h - SNACK_BOX - inset },
        { a->face_w - SNACK_BOX - inset, inset },
    };
    int k;

    for (k = 0; k < 3; k++) {
        if (!overlaps(c[k].x, c[k].y, SNACK_BOX, SNACK_BOX, a->char_x - inset, a->char_y - inset,
                      a->char_w + 2 * inset, a->char_h + 2 * inset)) {
            return c[k];
        }
    }
    return c[0];
}

/* Where the snack's top-left goes for it to be at the mouth. */
static lv_point_t snack_at_mouth(const struct deskbuddy_app *a)
{
    lv_point_t p = eye_line_centre(a);

    p.x -= SNACK_BOX / 2;
    p.y += a->eye_box * DB_MOUTH_Y_PM / 1000 - SNACK_BOX / 2;
    return p;
}

static lv_point_t snack_centre(const struct deskbuddy_app *a)
{
    lv_point_t p = { a->snack_pos.x + SNACK_BOX / 2, a->snack_pos.y + SNACK_BOX / 2 };

    return p;
}

static void snack_move(struct deskbuddy_app *a, lv_point_t to)
{
    to.x = LV_CLAMP(0, to.x, a->face_w - SNACK_BOX);
    to.y = LV_CLAMP(0, to.y, a->face_h - SNACK_BOX);
    a->snack_pos = to;
    lv_obj_set_pos(a->snack, to.x, to.y);
}

static void snack_put_away(struct deskbuddy_app *a, bool tell)
{
    bool was = a->snack_out;

    a->snack_out = false;
    a->snack_drag = false;
    a->snack_glide = false;
    set_hidden(a->snack, true);
    if (tell && was) {
        stimulate(a, DB_STIM_SNACK_GONE, DB_SRC_TOUCH, snack_centre(a));
    }
}

static void snack_show(struct deskbuddy_app *a, enum db_stim_source src)
{
    if (a->snack_out || !db_brain_playful(&a->brain) || a->eye_box <= 0) {
        return;
    }
    a->snack_out = true;
    snack_move(a, snack_home(a));
    set_hidden(a->snack, false);
    stimulate(a, DB_STIM_SNACK, src, snack_centre(a));
}

static void snack_glide(struct deskbuddy_app *a, lv_point_t to, bool feed)
{
    a->snack_drag = false;
    a->snack_glide = true;
    a->snack_feed = feed;
    a->glide_from = a->snack_pos;
    a->glide_to = to;
    a->glide_start = app_now(a);
    if (a->timer) {
        lv_timer_ready(a->timer);
    }
}

/* GIVE, Enter, or a tap on the snack: it floats to the mouth. */
static void snack_give(struct deskbuddy_app *a)
{
    if (a->snack_out && !a->snack_glide) {
        snack_glide(a, snack_at_mouth(a), true);
    }
}

/* From the timer: a glide's frame, and the brain's word on the snack. */
static void snack_step(struct deskbuddy_app *a, int64_t now)
{
    if (a->snack_out && !a->brain.snack) {
        /* Put away by the brain (a quiet spell, rest, another mode). */
        snack_put_away(a, false);
        return;
    }
    if (!a->snack_glide) {
        return;
    }
    if (a->brain.reduced_motion || now - a->glide_start >= GLIDE_MS) {
        a->snack_glide = false;
        snack_move(a, a->glide_to);
        stimulate(a, DB_STIM_SNACK, DB_SRC_TOUCH, snack_centre(a));
        if (a->snack_feed) {
            snack_put_away(a, false);
            stimulate(a, DB_STIM_FEED, DB_SRC_TOUCH, snack_centre(a));
        }
        return;
    }
    {
        int p = ease((int)((now - a->glide_start) * 1000 / GLIDE_MS));
        lv_point_t at = { a->glide_from.x + (a->glide_to.x - a->glide_from.x) * p / 1000,
                          a->glide_from.y + (a->glide_to.y - a->glide_from.y) * p / 1000 };

        snack_move(a, at);
        stimulate(a, DB_STIM_SNACK, DB_SRC_TOUCH, snack_centre(a));
    }
}

/* A new layout: a snack out goes back to its place in it. */
static void snack_relayout(struct deskbuddy_app *a)
{
    if (a->snack_out) {
        a->snack_drag = false;
        a->snack_glide = false;
        snack_move(a, snack_home(a));
        stimulate(a, DB_STIM_SNACK, DB_SRC_TOUCH, snack_centre(a));
    }
}

static void on_snack_press(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);
    lv_point_t p = finger_in_face(a);

    if (!a->snack_out) {
        return;
    }
    a->snack_glide = false;
    a->snack_drag = true;
    a->snack_grab.x = p.x - a->snack_pos.x;
    a->snack_grab.y = p.y - a->snack_pos.y;
    a->snack_press = p;
    a->snack_press_ms = app_now(a);
}

static void on_snack_pressing(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);
    lv_point_t p = finger_in_face(a);
    lv_point_t to;

    if (!a->snack_out || !a->snack_drag) {
        return;
    }
    to.x = p.x - a->snack_grab.x;
    to.y = p.y - a->snack_grab.y;
    if (to.x != a->snack_pos.x || to.y != a->snack_pos.y) {
        snack_move(a, to);
    }
    stimulate(a, DB_STIM_SNACK, DB_SRC_TOUCH, snack_centre(a));
}

static void on_snack_release(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);
    lv_point_t p = finger_in_face(a);
    int moved;

    if (!a->snack_out || !a->snack_drag) {
        return;
    }
    a->snack_drag = false;
    moved = LV_ABS(p.x - a->snack_press.x) + LV_ABS(p.y - a->snack_press.y);
    if (moved <= DB_TAP_SLOP_PX && app_now(a) - a->snack_press_ms <= DB_TAP_MS) {
        snack_give(a); /* a tap: it floats there */
        return;
    }
    stimulate(a, DB_STIM_SNACK, DB_SRC_TOUCH, snack_centre(a));
    if (a->brain.snack_near) {
        snack_put_away(a, false);
        stimulate(a, DB_STIM_FEED, DB_SRC_TOUCH, snack_centre(a));
    } else {
        snack_glide(a, snack_home(a), false); /* not at the mouth: back to its place */
    }
}

static void on_snack_lost(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);

    if (a->snack_out && a->snack_drag) {
        snack_glide(a, snack_home(a), false);
    }
}

/* FEED / GIVE and REST / WAKE, in Companion only. Cheap to call every
 * step: nothing is restyled unless it changed. */
static void refresh_care(struct deskbuddy_app *a)
{
    bool playful = db_brain_playful(&a->brain);

    set_hidden(a->care, !playful);
    if (playful) {
        set_text(lv_obj_get_child(a->feed_btn, 0), a->snack_out ? "GIVE" : "FEED");
        set_primary(a->feed_btn, a->snack_out);
        set_text(lv_obj_get_child(a->rest_btn, 0), db_brain_asleep(&a->brain) ? "WAKE" : "REST");
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
     * the width; the eyes and the mouth below them in the height, with room
     * for a hop; never bigger than looks friendly. */
    box = w * 3 / 7;
    if (box > h * 1000 / (DB_CHARACTER_H_PM + 120)) {
        box = h * 1000 / (DB_CHARACTER_H_PM + 120);
    }
    if (box > 240) {
        box = 240;
    }
    if (box < 16) {
        box = 16;
    }
    a->eye_box = box;
    a->char_w = 2 * box + box / 3;
    a->char_h = box * DB_CHARACTER_H_PM / 1000;
    a->char_x = (w - a->char_w) / 2;
    a->char_y = (h - a->char_h) / 2;
    lv_obj_set_size(a->character, a->char_w, a->char_h);
    for (k = 0; k < 2; k++) {
        lv_obj_set_size(a->eye[k].box, box, box);
        lv_obj_set_pos(a->eye[k].box, k == 0 ? 0 : box + box / 3, 0);
    }
    apply_face(a, true);
    snack_relayout(a);
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
    snack_step(a, now);
    apply_face(a, false);
    refresh_ui(a, false);
    refresh_care(a);
    next = db_brain_next_ms(&a->brain);
    if (a->provider_next >= 0 && a->provider_next < next) {
        next = a->provider_next;
    }
    if (a->tw_active || a->snack_glide) {
        next = now + FRAME_MS; /* something is moving: the next frame */
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

/* The face area: a finger's path, sorted into a poke or a stroke. */
static void on_face_press(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);
    lv_point_t p = finger_in_face(a);

    db_gesture_press(&a->gesture, p.x, p.y, app_now(a));
}

static void on_face_pressing(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);
    lv_point_t p = finger_in_face(a);

    if (db_gesture_move(&a->gesture, p.x, p.y, app_now(a)) == DB_GESTURE_STROKE) {
        stimulate(a, DB_STIM_PET, DB_SRC_TOUCH, p);
    }
}

static void on_face_release(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);
    lv_point_t p = finger_in_face(a);

    switch (db_gesture_release(&a->gesture, p.x, p.y, app_now(a))) {
    case DB_GESTURE_TAP:
        stimulate(a, DB_STIM_POKE, DB_SRC_TOUCH, p);
        break;
    case DB_GESTURE_STROKE:
        stimulate(a, DB_STIM_PET, DB_SRC_TOUCH, p);
        break;
    default:
        break;
    }
}

static void on_face_lost(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);

    db_gesture_cancel(&a->gesture);
}

/* FEED offers the snack; with one out it is GIVE. */
static void feed_or_give(struct deskbuddy_app *a, enum db_stim_source src)
{
    if (a->snack_out) {
        snack_give(a);
    } else {
        snack_show(a, src);
    }
}

static void rest_or_wake(struct deskbuddy_app *a, enum db_stim_source src)
{
    stimulate(a, db_brain_asleep(&a->brain) ? DB_STIM_WAKE : DB_STIM_REST, src, eye_line_centre(a));
}

static void on_feed(lv_event_t *e)
{
    feed_or_give(lv_event_get_user_data(e), DB_SRC_TOUCH);
}

static void on_rest(lv_event_t *e)
{
    rest_or_wake(lv_event_get_user_data(e), DB_SRC_TOUCH);
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
 * closes the settings panel in any build, else puts a snack away. In
 * Companion, F is FEED / GIVE, Enter gives a snack that is out, and R is
 * REST / WAKE - the keyboard's way to everything the buttons do. */
static void on_key(lv_event_t *e)
{
    struct deskbuddy_app *a = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    bool settings = !lv_obj_has_flag(a->panel, LV_OBJ_FLAG_HIDDEN);

    if (key == LV_KEY_ESC && settings) {
        show_settings(a, false);
        return;
    }
    if (key == LV_KEY_ESC && a->snack_out) {
        snack_put_away(a, true);
        return;
    }
    if (!settings && db_brain_playful(&a->brain)) {
        if (key == 'f' || key == 'F') {
            feed_or_give(a, DB_SRC_KEY);
            return;
        }
        if (key == LV_KEY_ENTER && a->snack_out) {
            snack_give(a);
            return;
        }
        if (key == 'r' || key == 'R') {
            rest_or_wake(a, DB_SRC_KEY);
            return;
        }
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

/* The mouth: a clipping box, an accent body in it and one cut-out, like
 * the eyes. */
static void build_mouth(struct deskbuddy_app *a)
{
    a->mouth = plain(a->character);
    a->mouth_body = plain(a->mouth);
    pos_style_add(a->mouth_body, POS_STYLE_CHIP_ACTIVE, 0);
    lv_obj_set_style_bg_opa(a->mouth_body, LV_OPA_COVER, 0);
    a->mouth_cut = plain(a->mouth_body);
    pos_style_add(a->mouth_cut, POS_STYLE_SCREEN, 0);
    lv_obj_add_flag(a->mouth, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a->mouth_cut, LV_OBJ_FLAG_HIDDEN);
}

/* The snack: a round biscuit - an accent rim and three accent crumbs on the
 * background, so it still reads as a biscuit over the open mouth - in a
 * touch area larger than itself. Hidden until FEED. */
static void build_snack(struct deskbuddy_app *a)
{
    static const lv_point_t crumbs[3] = { { 8, 8 }, { 22, 14 }, { 11, 23 } };
    lv_obj_t *biscuit;
    lv_obj_t *inside;
    int k;

    a->snack = plain(a->face);
    lv_obj_set_size(a->snack, SNACK_BOX, SNACK_BOX);
    lv_obj_add_flag(a->snack, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->snack, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(a->snack, LV_OBJ_FLAG_SCROLL_CHAIN);
    biscuit = plain(a->snack);
    lv_obj_set_size(biscuit, SNACK_D, SNACK_D);
    lv_obj_center(biscuit);
    pos_style_add(biscuit, POS_STYLE_CHIP_ACTIVE, 0);
    lv_obj_set_style_bg_opa(biscuit, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(biscuit, LV_RADIUS_CIRCLE, 0);
    inside = plain(biscuit);
    lv_obj_set_size(inside, SNACK_D - 14, SNACK_D - 14);
    lv_obj_center(inside);
    pos_style_add(inside, POS_STYLE_SCREEN, 0);
    lv_obj_set_style_radius(inside, LV_RADIUS_CIRCLE, 0);
    for (k = 0; k < 3; k++) {
        lv_obj_t *crumb = plain(inside);

        lv_obj_set_size(crumb, 7, 7);
        lv_obj_set_pos(crumb, crumbs[k].x, crumbs[k].y);
        pos_style_add(crumb, POS_STYLE_CHIP_ACTIVE, 0);
        lv_obj_set_style_bg_opa(crumb, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(crumb, LV_RADIUS_CIRCLE, 0);
    }
    lv_obj_add_event_cb(a->snack, on_snack_press, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->snack, on_snack_pressing, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(a->snack, on_snack_release, LV_EVENT_RELEASED, a);
    lv_obj_add_event_cb(a->snack, on_snack_lost, LV_EVENT_PRESS_LOST, a);
    lv_obj_add_flag(a->snack, LV_OBJ_FLAG_HIDDEN);
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
    /* A stroke is the face's own, not a scroll of what holds it. */
    lv_obj_remove_flag(a->face, LV_OBJ_FLAG_SCROLL_CHAIN);
    a->character = plain(a->face);
    for (k = 0; k < 2; k++) {
        build_eye(&a->eye[k], a->character);
    }
    build_mouth(a);
    build_snack(a);

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
    a->care = plain(a->side);
    lv_obj_set_size(a->care, LV_PCT(100), BUTTON_H);
    lv_obj_set_flex_flow(a->care, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(a->care, 8, 0);
    lv_obj_add_flag(a->care, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    a->feed_btn = button(a->care, "FEED", on_feed, a);
    lv_obj_set_flex_grow(a->feed_btn, 1);
    a->rest_btn = button(a->care, "REST", on_rest, a);
    lv_obj_set_flex_grow(a->rest_btn, 1);
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

    lv_obj_add_event_cb(a->face, on_face_press, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->face, on_face_pressing, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(a->face, on_face_release, LV_EVENT_RELEASED, a);
    lv_obj_add_event_cb(a->face, on_face_lost, LV_EVENT_PRESS_LOST, a);
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
    a->provider_starts++;
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

lv_obj_t *deskbuddy_app_feed_button(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->feed_btn : NULL;
}

lv_obj_t *deskbuddy_app_rest_button(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->rest_btn : NULL;
}

lv_obj_t *deskbuddy_app_snack(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->snack : NULL;
}

lv_obj_t *deskbuddy_app_mouth(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->mouth : NULL;
}

lv_obj_t *deskbuddy_app_character(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->character : NULL;
}

const char *deskbuddy_app_provider(void *priv)
{
    struct deskbuddy_app *a = priv;

    return a && a->provider.ops ? a->provider.ops->name : "";
}

unsigned deskbuddy_app_provider_starts(void *priv)
{
    return priv ? ((struct deskbuddy_app *)priv)->provider_starts : 0;
}

/* The first-party launcher mask (docs/design/doors-app-icons); .icon stays
 * the text fallback for a shell without the mask. */
LV_IMAGE_DECLARE(pos_app_icon_deskbuddy);

/* The Back action (app.h `back`, hw_actions.h): the settings panel closes, as
 * Esc closes it, then a snack is put away; the face is the top level. */
static int deskbuddy_back(void *priv)
{
    struct deskbuddy_app *a = priv;

    if (!a || !a->panel) {
        return 0;
    }
    if (!lv_obj_has_flag(a->panel, LV_OBJ_FLAG_HIDDEN)) {
        show_settings(a, false);
        return 1;
    }
    if (a->snack_out) {
        snack_put_away(a, true);
        return 1;
    }
    return 0;
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
