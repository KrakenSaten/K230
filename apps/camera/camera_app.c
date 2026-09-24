/*
 * Camera: a live picture, a shutter, and a look at the photo just taken.
 *
 * This file is the screen. What state the camera is in and what each tap
 * does is camera_state.c; where things go is camera_layout.c; the camera
 * itself is not here at all. It lives in a pos-camera helper process
 * (camera_session.c, ADR-006 PROPOSED), polled from an LVGL timer that only
 * ever makes non-blocking calls. The only waits on the LVGL thread are
 * destroy() and Try again giving a running helper CAMERA_DESTROY_GRACE_MS to
 * close the camera before it is killed.
 *
 * PICTURES. The helper converts every preview frame to RGB565 at exactly the
 * size of the picture box and puts it in shared memory; the timer copies the
 * newest one into this app's own buffer and hands the slot back in the same
 * tick. The image object only ever points at memory this app owns, so no
 * picture can be drawn from memory the helper or a finished session still
 * controls, and nothing is freed while an image still points at it.
 *
 * FULLSCREEN (DS section 30.8): the app declares NONE; the shell's header
 * carries the back button and whatever the hint says (SIMULATED under the
 * fake backend, so a made-up picture is never mistaken for a camera).
 *
 * KEYBOARD (not implemented, documented in docs/apps/CAMERA.md): Space or
 * Enter for the shutter, K and D in review.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "camera_layout.h"
#include "camera_session.h"
#include "camera_state.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"

#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: not in lvgl.h */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAMERA_POLL_MS 33
/* destroy() and Try again: time for a running helper to close the camera. */
#define CAMERA_DESTROY_GRACE_MS 300
/* The last photo's thumbnail inside its 72 px button. */
#define CAMERA_THUMB_PIX 64

struct camera_picture {
    lv_image_dsc_t dsc;
    uint16_t *px;
    uint32_t w;
    uint32_t h;
};

struct camera_app {
    lv_obj_t *frame;
    lv_obj_t *box;         /* the picture's place: a slab */
    lv_obj_t *img;
    lv_obj_t *panel;       /* title and detail, in the picture's place */
    lv_obj_t *title;
    lv_obj_t *detail;
    lv_obj_t *status;
    lv_obj_t *shutter;
    lv_obj_t *last;
    lv_obj_t *last_img;
    lv_obj_t *btn_a;
    lv_obj_t *btn_b;

    struct pocketui_layout_guard guard;
    struct camera_layout lay;
    uint32_t lay_still_w;  /* the still the layout was chosen for */
    uint32_t lay_still_h;
    bool laid_out;
    lv_timer_t *timer;

    struct camera_model model;
    struct camera_session session;
    struct camera_picture preview;
    struct camera_picture review;
    struct camera_picture thumb;
    const lv_image_dsc_t *shown;  /* what img shows now, or NULL */
    const char *hint_shown;
};

static int64_t now_ms(void)
{
    return (int64_t)lv_tick_get();
}

static bool portrait_now(void)
{
    struct pocketos_orientation o;

    pocketos_shell_orientation(&o);
    return !o.landscape;
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden != lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        if (hidden) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void place(lv_obj_t *obj, const struct camera_rect *r)
{
    lv_obj_set_pos(obj, r->x, r->y);
    lv_obj_set_size(obj, r->w, r->h);
}

/* ---- pictures ------------------------------------------------------------------ */

static void picture_free(struct camera_picture *p)
{
    if (p->px) {
        lv_image_cache_drop(&p->dsc);
        free(p->px);
    }
    memset(p, 0, sizeof(*p));
}

static bool picture_alloc(struct camera_picture *p, uint32_t w, uint32_t h)
{
    picture_free(p);
    p->px = calloc((size_t)w * h, sizeof(uint16_t));
    if (!p->px) {
        return false;
    }
    p->w = w;
    p->h = h;
    p->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    p->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    p->dsc.header.w = w;
    p->dsc.header.h = h;
    p->dsc.header.stride = w * 2;
    p->dsc.data = (const uint8_t *)p->px;
    p->dsc.data_size = w * h * 2;
    return true;
}

/* The pixels behind a picture changed: LVGL must not draw a cached copy. */
static void picture_changed(struct camera_app *a, struct camera_picture *p)
{
    lv_image_cache_drop(&p->dsc);
    if (a->shown == &p->dsc) {
        lv_obj_invalidate(a->img);
    }
}

/* The review picture, shrunk to cover the thumbnail. */
static void make_thumb(struct camera_app *a)
{
    const struct camera_picture *r = &a->review;
    uint32_t side = r->w < r->h ? r->w : r->h;
    uint32_t ox = (r->w - side) / 2;
    uint32_t oy = (r->h - side) / 2;
    uint32_t x;
    uint32_t y;

    if (!a->thumb.px || !r->px || side == 0) {
        return;
    }
    for (y = 0; y < CAMERA_THUMB_PIX; y++) {
        for (x = 0; x < CAMERA_THUMB_PIX; x++) {
            a->thumb.px[y * CAMERA_THUMB_PIX + x] =
                r->px[(oy + y * side / CAMERA_THUMB_PIX) * r->w + ox + x * side / CAMERA_THUMB_PIX];
        }
    }
    lv_image_cache_drop(&a->thumb.dsc);
    lv_obj_invalidate(a->last_img);
}

static void show(struct camera_app *a, const lv_image_dsc_t *dsc)
{
    if (a->shown != dsc) {
        a->shown = dsc;
        lv_image_set_src(a->img, dsc);
    }
}

/* ---- buttons ------------------------------------------------------------------- */

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = pocketui_button(parent, text, cb, user);

    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_CLIP);
    return b;
}

/* Primary or secondary, and enabled or not (the disabled role of DS section 9).
 * Only a change is applied: the timer repaints often. The role last applied
 * is kept in the user data, offset by one so 0 means "not yet". */
static void button_style(lv_obj_t *b, bool primary, bool enabled)
{
    intptr_t role = enabled ? (primary ? 1 : 2) : 3;

    if ((intptr_t)lv_obj_get_user_data(b) == role) {
        return;
    }
    lv_obj_set_user_data(b, (void *)role);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    lv_obj_remove_style(b, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_DISABLED), 0);
    if (!enabled) {
        pos_style_add(b, POS_STYLE_BUTTON_DISABLED, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    } else {
        if (primary) {
            pos_style_add(b, POS_STYLE_BUTTON_PRIMARY, 0);
            pos_style_add(b, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
        } else {
            pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
            pos_style_add(b, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        }
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_invalidate(b);
}

static void button_text(lv_obj_t *b, const char *text)
{
    lv_obj_t *lb = lv_obj_get_child(b, 0);

    if (strcmp(lv_label_get_text(lb), text) != 0) {
        lv_label_set_text(lb, text);
    }
}

static void label_text(lv_obj_t *lb, const char *text)
{
    if (strcmp(lv_label_get_text(lb), text) != 0) {
        lv_label_set_text(lb, text);
    }
}

/* ---- the screen ------------------------------------------------------------------ */

static void repaint(struct camera_app *a)
{
    struct camera_screen s;
    enum camera_state st = a->model.state;

    camera_model_screen(&a->model, &s);
    if (s.show_picture) {
        const struct camera_picture *p = s.picture_is_review ? &a->review : &a->preview;

        show(a, p->px ? &p->dsc : NULL);
    }
    set_hidden(a->img, !s.show_picture);
    set_hidden(a->panel, !s.show_panel);
    label_text(a->title, a->model.title);
    label_text(a->detail, a->model.detail);
    set_hidden(a->detail, a->model.detail[0] == '\0');
    label_text(a->status, s.status);
    if (s.status[0] && (st == CAMERA_CAPTURING || a->model.stalled || a->model.note[0])) {
        lv_obj_remove_style(a->status, pos_style(POS_STYLE_TEXT_SECONDARY), 0);
        pos_style_add(a->status, POS_STYLE_STATUS_WARN_TEXT, 0);
    } else {
        lv_obj_remove_style(a->status, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
        pos_style_add(a->status, POS_STYLE_TEXT_SECONDARY, 0);
    }

    set_hidden(a->shutter, !s.show_shutter);
    button_style(a->shutter, true, s.shutter_enabled);
    set_hidden(a->last, !s.show_last);

    set_hidden(a->btn_a, !(s.show_review_buttons || s.show_confirm));
    set_hidden(a->btn_b, !(s.show_review_buttons || s.show_confirm || s.show_retry));
    if (s.show_confirm) {
        button_text(a->btn_a, "CANCEL");
        button_text(a->btn_b, "DELETE");
        button_style(a->btn_a, false, !a->model.deleting);
        button_style(a->btn_b, true, !a->model.deleting);
    } else if (s.show_review_buttons) {
        button_text(a->btn_a, "DELETE");
        button_text(a->btn_b, "KEEP");
        button_style(a->btn_a, false, true);
        button_style(a->btn_b, true, true);
    } else if (s.show_retry) {
        button_text(a->btn_b, st == CAMERA_NO_DEVICE ? "CHECK AGAIN" : "TRY AGAIN");
        button_style(a->btn_b, true, true);
    }
    /* In the wide shape the retry button sits where the shutter was. */
    if (s.show_retry) {
        place(a->btn_b, a->lay.wide ? &a->lay.btn_a : &a->lay.shutter);
    } else {
        place(a->btn_b, &a->lay.btn_b);
    }

    /* Only a change: writing the hint repaints the header. */
    if (a->hint_shown != s.hint && (!a->hint_shown || strcmp(a->hint_shown, s.hint) != 0)) {
        pocketos_shell_set_status_hint(s.hint);
    }
    a->hint_shown = s.hint;
}

/* ---- doing what the model asks ---------------------------------------------------------- */

static void do_actions(struct camera_app *a, unsigned acts);
static void layout(struct camera_app *a);

static void start_session(struct camera_app *a)
{
    struct camera_session_config cfg = { 0 };
    char err[CAMERA_EVENT_TEXT_MAX - 8];

    camera_session_abandon(&a->session, CAMERA_DESTROY_GRACE_MS);
    /* The fake's fault script goes to the fake only; the real backend reads
     * its own settings from the environment the helper inherits. */
    if (strcmp(camera_session_backend(), "fake") == 0) {
        cfg.fake = getenv("POCKETOS_CAMERA_FAKE");
    }
    if (camera_session_start(&a->session, &cfg, now_ms(), err, sizeof(err)) != 0) {
        struct camera_event ev;

        LOG_WARN("camera: could not start the helper: %s", err);
        memset(&ev, 0, sizeof(ev));
        ev.kind = CAMERA_EV_ERROR;
        snprintf(ev.text, sizeof(ev.text), "start %s", err);
        do_actions(a, camera_model_event(&a->model, &ev, now_ms()));
    }
}

static void do_actions(struct camera_app *a, unsigned acts)
{
    int64_t now = now_ms();

    if (acts & CAMERA_DO_START_SESSION) {
        start_session(a);
    }
    if (acts & CAMERA_DO_TAKE_REVIEW) {
        bool copied = camera_session_take_review(&a->session, a->review.px, a->review.w,
                                                 a->review.h) == 1;

        a->model.review_picture = copied;
        if (copied) {
            picture_changed(a, &a->review);
            make_thumb(a);
        }
    }
    if (acts & CAMERA_DO_START_PREVIEW) {
        camera_session_view(&a->session, a->preview.w, a->preview.h, portrait_now());
        camera_session_preview(&a->session, true, now);
    }
    if (acts & CAMERA_DO_STOP_PREVIEW) {
        camera_session_preview(&a->session, false, now);
    }
    if (acts & CAMERA_DO_CAPTURE) {
        camera_session_capture(&a->session, portrait_now(), now);
    }
    if (acts & CAMERA_DO_DELETE) {
        if (camera_session_delete(&a->session, a->model.review_name, now) != 0) {
            struct camera_event ev;

            memset(&ev, 0, sizeof(ev));
            ev.kind = CAMERA_EV_DELFAIL;
            camera_model_event(&a->model, &ev, now);
        }
    }
}

static void on_poll(lv_timer_t *t)
{
    struct camera_app *a = lv_timer_get_user_data(t);
    struct camera_event ev;
    int64_t now = now_ms();
    bool changed = camera_model_tick(&a->model, now);

    while (camera_session_poll(&a->session, &ev, now)) {
        if (ev.kind == CAMERA_EV_FRAME) {
            /* A picture only counts once it is on screen. */
            if (a->model.state != CAMERA_PREVIEW ||
                camera_session_take_frame(&a->session, a->preview.px, a->preview.w,
                                          a->preview.h) != 1) {
                continue;
            }
            picture_changed(a, &a->preview);
            if (a->model.live && !a->model.stalled) {
                continue; /* the picture changed, nothing else did */
            }
        } else if (ev.kind == CAMERA_EV_EXITED && ev.reason != CAMERA_EXIT_NORMAL) {
            LOG_WARN("camera: helper ended (%d), reason %d", ev.value, ev.reason);
        }
        {
            unsigned acts = camera_model_event(&a->model, &ev, now);

            /* A still of another shape than the one laid out for: the
             * picture box takes the photo's shape before the preview starts. */
            if (ev.kind == CAMERA_EV_READY &&
                (a->model.still_w != a->lay_still_w || a->model.still_h != a->lay_still_h)) {
                pocketui_layout_guard_reset(&a->guard);
                layout(a);
            }
            do_actions(a, acts);
        }
        changed = true;
    }
    if (changed) {
        repaint(a);
    }
}

/* ---- taps ------------------------------------------------------------------------ */

static void act(struct camera_app *a, unsigned acts)
{
    do_actions(a, acts);
    repaint(a);
}

static void on_shutter(lv_event_t *e)
{
    struct camera_app *a = lv_event_get_user_data(e);

    act(a, camera_model_shutter(&a->model));
}

static void on_last(lv_event_t *e)
{
    struct camera_app *a = lv_event_get_user_data(e);

    act(a, camera_model_show_last(&a->model));
}

static void on_btn_a(lv_event_t *e)
{
    struct camera_app *a = lv_event_get_user_data(e);

    act(a, a->model.confirm_delete ? camera_model_delete_cancel(&a->model)
                                   : camera_model_delete(&a->model));
}

static void on_btn_b(lv_event_t *e)
{
    struct camera_app *a = lv_event_get_user_data(e);

    if (a->model.state == CAMERA_ERROR || a->model.state == CAMERA_NO_DEVICE) {
        act(a, camera_model_retry(&a->model));
    } else if (a->model.confirm_delete) {
        act(a, camera_model_delete_confirm(&a->model));
    } else {
        act(a, camera_model_keep(&a->model));
    }
}

/* ---- layout ---------------------------------------------------------------------- */

static void layout(struct camera_app *a)
{
    struct pos_insets in;
    const lv_area_t *area;
    uint32_t pw;
    uint32_t ph;

    /* Nothing to lay out in, or nothing the layout is chosen from changed:
     * PocketUI decides (pocketui.h) and gives the corner clearance. */
    if (!pocketui_layout_begin(&a->guard, a->frame, &in)) {
        return;
    }
    area = &a->guard.area;
    a->lay_still_w = a->model.still_w;
    a->lay_still_h = a->model.still_h;
    /* The photo as the owner holds the unit: the still turned upright. */
    if (portrait_now()) {
        pw = a->model.still_h;
        ph = a->model.still_w;
    } else {
        pw = a->model.still_w;
        ph = a->model.still_h;
    }
    if (camera_layout_compute(&a->lay, lv_area_get_width(area), lv_area_get_height(area), in.left,
                              in.top, in.right, in.bottom, pw, ph) != 0) {
        LOG_WARN("camera: body %dx%d is too small for the camera screen",
                 (int)lv_area_get_width(area), (int)lv_area_get_height(area));
        return;
    }
    a->laid_out = true;
    place(a->box, &a->lay.picture);
    place(a->status, &a->lay.status);
    place(a->shutter, &a->lay.shutter);
    place(a->last, &a->lay.last);
    place(a->btn_a, &a->lay.btn_a);
    place(a->btn_b, &a->lay.btn_b);
    lv_obj_set_width(a->detail, a->lay.picture.w - 2 * POCKETUI_PAD);
    if ((uint32_t)a->lay.picture.w != a->preview.w || (uint32_t)a->lay.picture.h != a->preview.h) {
        uint32_t w = (uint32_t)a->lay.picture.w;
        uint32_t h = (uint32_t)a->lay.picture.h;

        /* New buffers, so nothing is shown from the old ones meanwhile. */
        show(a, NULL);
        if (!picture_alloc(&a->preview, w, h) || !picture_alloc(&a->review, w, h)) {
            LOG_ERROR("camera: no memory for a %ux%u picture", w, h);
            picture_free(&a->preview);
            picture_free(&a->review);
        }
        /* The review picture was of the old size: it is gone. */
        a->model.review_picture = false;
        if (camera_session_active(&a->session) && a->model.state == CAMERA_PREVIEW) {
            camera_session_view(&a->session, w, h, portrait_now());
        }
    }
    repaint(a);
}

static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

/* ---- building ---------------------------------------------------------------------- */

static void build(struct camera_app *a, lv_obj_t *root)
{
    a->frame = lv_obj_create(root);
    lv_obj_remove_style_all(a->frame);
    /* Exactly the body's content box, so the shape is chosen from the room
     * the shell gives and never from what the shape itself put there. */
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(a->frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    a->box = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->box);
    pos_style_add(a->box, POS_STYLE_SLAB, 0);
    lv_obj_clear_flag(a->box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    a->img = lv_image_create(a->box);
    lv_obj_set_pos(a->img, 0, 0);
    lv_obj_add_flag(a->img, LV_OBJ_FLAG_HIDDEN);

    a->panel = lv_obj_create(a->box);
    lv_obj_remove_style_all(a->panel);
    lv_obj_set_size(a->panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_center(a->panel);
    lv_obj_set_flex_flow(a->panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(a->panel, 12, 0);
    lv_obj_clear_flag(a->panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    a->title = pocketui_label(a->panel, "", POS_STYLE_TITLE);
    a->detail = pocketui_label(a->panel, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(a->detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(a->detail, LV_TEXT_ALIGN_CENTER, 0);

    a->status = pocketui_label(a->frame, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(a->status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(a->status, LV_TEXT_ALIGN_CENTER, 0);

    a->shutter = button(a->frame, "TAKE PHOTO", on_shutter, a);

    a->last = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->last);
    pos_style_add(a->last, POS_STYLE_SLAB, 0);
    pos_style_add(a->last, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_clear_flag(a->last, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(a->last, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->last, on_last, LV_EVENT_CLICKED, a);
    a->last_img = lv_image_create(a->last);
    lv_obj_center(a->last_img);
    if (picture_alloc(&a->thumb, CAMERA_THUMB_PIX, CAMERA_THUMB_PIX)) {
        lv_image_set_src(a->last_img, &a->thumb.dsc);
    }
    lv_obj_clear_flag(a->last_img, LV_OBJ_FLAG_CLICKABLE);

    a->btn_a = button(a->frame, "DELETE", on_btn_a, a);
    a->btn_b = button(a->frame, "KEEP", on_btn_b, a);
}

/* ---- the app ------------------------------------------------------------------------ */

static void *camera_create(lv_obj_t *root)
{
    struct camera_app *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    camera_session_init(&a->session);
    camera_model_init(&a->model);
    build(a, root);
    pocketos_shell_set_status_hint("");
    a->hint_shown = "";
    a->timer = lv_timer_create(on_poll, CAMERA_POLL_MS, a);
    /* Only now: building lays objects out as it goes, and the layout step
     * shapes objects that must all exist. */
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    layout(a);
    /* The camera starts with the screen and never outlives it. */
    do_actions(a, camera_model_open(&a->model));
    repaint(a);
    return a;
}

static void camera_destroy(void *priv)
{
    struct camera_app *a = priv;

    if (!a) {
        return;
    }
    /* The frame outlives this by a moment, until the shell deletes the app's
     * objects; nothing may call back into a freed app in between. */
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    /* The helper first: once it is gone nothing writes the shared memory the
     * session is about to unmap. Leaving the app ends the camera. */
    camera_session_abandon(&a->session, CAMERA_DESTROY_GRACE_MS);
    /* No image may point at a buffer being freed. */
    lv_image_set_src(a->img, NULL);
    lv_image_set_src(a->last_img, NULL);
    picture_free(&a->preview);
    picture_free(&a->review);
    picture_free(&a->thumb);
    free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_camera);

const struct pocketos_app app_camera = {
    .id = "camera",
    .name = "Camera",
    /* The launcher draws the Doors icon (DS section 20); LVGL's symbol font
     * has no camera, and the image glyph is the nearest thing. */
    .icon = LV_SYMBOL_IMAGE,
    .icon_mask = &pos_app_icon_camera,
    .create = camera_create,
    .tick = NULL,
    .destroy = camera_destroy,
    /* Fullscreen (DS section 30.8): no status bar, the picture takes the
     * height. The hint shows in the header. */
    .chrome = POCKETOS_CHROME_NONE,
};
