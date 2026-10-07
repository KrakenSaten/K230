/*
 * Camera's gallery screen. See camera_gallery_screen.h; what it does is
 * camera_gallery.c, where it goes is camera_layout.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"
#include "camera_gallery.h"
#include "camera_gallery_screen.h"
#include "camera_layout.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"

#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: not in lvgl.h */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Leaving: time for the library helper to go before it is killed (it holds
 * no device, so it goes at once unless it is in the middle of a decode). */
#define GALLERY_LEAVE_GRACE_MS 300
#define GALLERY_INFO_LINES 3

struct gpic {
    lv_image_dsc_t dsc;
    uint16_t *px;
    uint32_t cap_w;      /* what the buffer holds */
    uint32_t cap_h;
};

struct gallery_ui {
    lv_obj_t *frame;
    lv_obj_t *status;
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *detail;
    lv_obj_t *cells[GALLERY_PAGE_MAX];
    lv_obj_t *cell_img[GALLERY_PAGE_MAX];
    lv_obj_t *cell_note[GALLERY_PAGE_MAX];
    lv_obj_t *photo_box;
    lv_obj_t *photo_img;
    lv_obj_t *info[GALLERY_INFO_LINES];
    lv_obj_t *show_box;
    lv_obj_t *show_img;
    lv_obj_t *newer;
    lv_obj_t *older;
    lv_obj_t *left;
    lv_obj_t *middle;
    lv_obj_t *right;

    struct gpic thumbs[GALLERY_PAGE_MAX];
    struct gpic photo;
    struct gpic slides[2];
    int front;           /* which of slides is shown */

    struct pocketui_layout_guard guard;
    struct camera_gallery_layout lay;
    bool laid_out;
    bool active;
    bool want_camera;
    bool standalone;       /* the Photo app's: no CAMERA */
    uint32_t list_total;   /* photos in the folder, from the last LISTED */
    struct camera_session *session;
    struct camera_gallery model;
    char names[CAMERA_LIBRARY_MAX][CAMERA_NAME_MAX]; /* a list, on its way into the model */
};

static int64_t now_ms(void)
{
    return (int64_t)lv_tick_get();
}

/* ---- small things ---------------------------------------------------------------- */

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

static void label_text(lv_obj_t *lb, const char *text)
{
    if (strcmp(lv_label_get_text(lb), text) != 0) {
        lv_label_set_text(lb, text);
    }
}

static lv_obj_t *button(lv_obj_t *parent, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = pocketui_button(parent, "", cb, user);

    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_CLIP);
    return b;
}

/* A button's word, role and whether it takes taps (as camera_app.c's). A NULL
 * word hides it. */
static void button_set(lv_obj_t *b, const char *text, bool primary, bool enabled)
{
    intptr_t role = enabled ? (primary ? 1 : 2) : 3;

    set_hidden(b, text == NULL);
    if (!text) {
        return;
    }
    label_text(lv_obj_get_child(b, 0), text);
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

static lv_obj_t *slab(lv_obj_t *parent, bool clickable)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    pos_style_add(o, POS_STYLE_SLAB, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    if (clickable) {
        pos_style_add(o, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    }
    return o;
}

static lv_obj_t *picture_in(lv_obj_t *box)
{
    lv_obj_t *img = lv_image_create(box);

    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}

/* ---- pictures -------------------------------------------------------------------- */

static void gpic_free(struct gpic *p)
{
    if (p->px) {
        lv_image_cache_drop(&p->dsc);
        free(p->px);
    }
    memset(p, 0, sizeof(*p));
}

static bool gpic_alloc(struct gpic *p, uint32_t w, uint32_t h)
{
    if (p->px && p->cap_w == w && p->cap_h == h) {
        return true;
    }
    gpic_free(p);
    p->px = calloc((size_t)w * h, sizeof(uint16_t));
    if (!p->px) {
        LOG_ERROR("camera: no memory for a %ux%u gallery picture", w, h);
        return false;
    }
    p->cap_w = w;
    p->cap_h = h;
    p->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    p->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    p->dsc.data = (const uint8_t *)p->px;
    return true;
}

/* Copy the picture in slot into p, shown in img. False when it could not. */
static bool gpic_take(struct gallery_ui *u, struct gpic *p, int slot, lv_obj_t *img)
{
    uint32_t w = 0;
    uint32_t h = 0;

    if (!p->px) {
        camera_session_take_picture(u->session, slot, NULL, 0, 0, NULL, NULL);
        return false;
    }
    /* The pixels change under the descriptor: LVGL must not draw a cached copy. */
    lv_image_cache_drop(&p->dsc);
    if (camera_session_take_picture(u->session, slot, p->px, p->cap_w, p->cap_h, &w, &h) != 1) {
        return false;
    }
    p->dsc.header.w = w;
    p->dsc.header.h = h;
    p->dsc.header.stride = w * 2;
    p->dsc.data_size = w * h * 2;
    if (img) {
        lv_image_set_src(img, NULL);
        lv_image_set_src(img, &p->dsc);
        lv_obj_center(img);
        lv_obj_invalidate(img);
    }
    return true;
}

/* A buffer of exactly w x h for img, which is let go of first when the old
 * one has to be replaced. */
static bool gpic_ensure(struct gpic *p, lv_obj_t *img, uint32_t w, uint32_t h)
{
    if (p->px && (p->cap_w != w || p->cap_h != h) && img) {
        lv_image_set_src(img, NULL);
    }
    return gpic_alloc(p, w, h);
}

static void free_slides(struct gallery_ui *u)
{
    lv_image_set_src(u->show_img, NULL);
    gpic_free(&u->slides[0]);
    gpic_free(&u->slides[1]);
    u->front = 0;
}

static void free_pictures(struct gallery_ui *u)
{
    int i;

    for (i = 0; i < GALLERY_PAGE_MAX; i++) {
        lv_image_set_src(u->cell_img[i], NULL);
        gpic_free(&u->thumbs[i]);
    }
    lv_image_set_src(u->photo_img, NULL);
    gpic_free(&u->photo);
    free_slides(u);
}

/* ---- the screen -------------------------------------------------------------------- */

static void repaint(struct gallery_ui *u)
{
    struct camera_gallery *m = &u->model;
    struct gallery_screen s;
    bool photo_view = m->view == GALLERY_PHOTO;
    int c;

    camera_gallery_screen(m, &s);
    set_hidden(u->panel, !s.show_panel);
    if (s.show_panel) {
        label_text(u->title, m->title);
        label_text(u->detail, m->detail);
        set_hidden(u->detail, m->detail[0] == '\0');
    }
    for (c = 0; c < GALLERY_PAGE_MAX; c++) {
        bool on = s.show_grid && c < u->lay.rows * u->lay.cols &&
                  camera_gallery_cell_index(m, c) >= 0;

        set_hidden(u->cells[c], !on);
        if (on) {
            set_hidden(u->cell_img[c], m->cells[c] != GALLERY_CELL_SHOWN);
            set_hidden(u->cell_note[c], m->cells[c] != GALLERY_CELL_BAD);
        }
    }

    set_hidden(u->photo_box, !s.show_photo);
    set_hidden(u->photo_img, !(s.show_photo && m->photo == GALLERY_PIC_SHOWN));
    {
        char name[CAMERA_NAME_MAX];
        char when[64];
        char what[96];
        const char *lines[GALLERY_INFO_LINES] = { name, when, what };
        int i;

        camera_gallery_info(m, name, sizeof(name), when, sizeof(when), what, sizeof(what));
        for (i = 0; i < GALLERY_INFO_LINES; i++) {
            set_hidden(u->info[i], !s.show_photo);
            label_text(u->info[i], lines[i]);
        }
    }

    set_hidden(u->show_box, !s.show_slideshow);
    set_hidden(u->show_img, !(s.show_slideshow && m->slide_front == GALLERY_PIC_SHOWN));
    if (!s.show_slideshow && (u->slides[0].px || u->slides[1].px)) {
        free_slides(u); /* only a running slideshow holds its two pictures */
    }

    label_text(u->status, s.status);
    lv_obj_remove_style(u->status, pos_style(s.status_warn ? POS_STYLE_TEXT_SECONDARY
                                                           : POS_STYLE_STATUS_WARN_TEXT), 0);
    pos_style_add(u->status, s.status_warn ? POS_STYLE_STATUS_WARN_TEXT : POS_STYLE_TEXT_SECONDARY,
                  0);
    place(u->status, s.show_slideshow ? &u->lay.show_status : &u->lay.status);

    button_set(u->newer, s.show_newer ? "NEWER" : NULL, false, s.newer_enabled);
    button_set(u->older, s.show_older ? "OLDER" : NULL, false, s.older_enabled);
    button_set(u->left, s.left, false, s.left_enabled);
    button_set(u->middle, s.middle, s.middle_primary, s.middle_enabled);
    button_set(u->right, s.right, s.right_primary, s.right_enabled);
    place(u->newer, photo_view ? &u->lay.p_newer : &u->lay.newer);
    place(u->older, photo_view ? &u->lay.p_older : &u->lay.older);
    place(u->left, photo_view ? &u->lay.p_left : &u->lay.left);
    place(u->middle, photo_view ? &u->lay.p_middle : &u->lay.middle);
    place(u->right, &u->lay.p_right);
}

/* ---- doing what the model asks ------------------------------------------------------ */

static void start_helper(struct gallery_ui *u)
{
    struct camera_session_config cfg = { 0 };
    char err[CAMERA_EVENT_TEXT_MAX - 8];

    camera_session_abandon(u->session, GALLERY_LEAVE_GRACE_MS);
    cfg.library = true;
    if (camera_session_start(u->session, &cfg, now_ms(), err, sizeof(err)) != 0) {
        struct camera_event ev;

        LOG_WARN("camera: could not start the library helper: %s", err);
        memset(&ev, 0, sizeof(ev));
        ev.kind = CAMERA_EV_ERROR;
        snprintf(ev.text, sizeof(ev.text), "start %s", err);
        camera_gallery_event(&u->model, &ev, now_ms());
    }
}

static void ask_for_pictures(struct gallery_ui *u)
{
    struct gallery_request req;

    while (camera_session_pictures_free(u->session) > 0 &&
           camera_gallery_next_request(&u->model, &req)) {
        int slot;

        if (req.purpose == GALLERY_FOR_SLIDE &&
            (!gpic_ensure(&u->slides[0], u->show_img, req.w, req.h) ||
             !gpic_ensure(&u->slides[1], u->show_img, req.w, req.h))) {
            free_slides(u);
            break;
        }
        if (req.purpose == GALLERY_FOR_PHOTO && !gpic_ensure(&u->photo, u->photo_img, req.w, req.h)) {
            break;
        }
        slot = camera_session_request_picture(u->session, req.name, req.w, req.h, req.cover,
                                              now_ms());
        camera_gallery_requested(&u->model, &req, slot);
        if (slot < 0) {
            break;
        }
    }
}

static void do_actions(struct gallery_ui *u, unsigned acts)
{
    int64_t now = now_ms();
    const char *name;

    if (acts & GALLERY_DO_START) {
        start_helper(u);
    }
    if (acts & GALLERY_DO_LIST) {
        camera_session_list(u->session, now);
    }
    if (acts & GALLERY_DO_TAKE_LIST) {
        int n = camera_session_take_list(u->session, u->names, CAMERA_LIBRARY_MAX);

        acts |= camera_gallery_set_list(&u->model, u->names, n, u->list_total);
    }
    if (acts & GALLERY_DO_DELETE) {
        name = camera_gallery_current_name(&u->model);
        if (!name || camera_session_delete(u->session, name, now) != 0) {
            struct camera_event ev;

            memset(&ev, 0, sizeof(ev));
            ev.kind = CAMERA_EV_DELFAIL;
            camera_gallery_event(&u->model, &ev, now);
        }
    }
    if (acts & GALLERY_DO_EXPORT) {
        name = camera_gallery_current_name(&u->model);
        if (!name || camera_session_export(u->session, name, now) != 0) {
            struct camera_event ev;

            memset(&ev, 0, sizeof(ev));
            ev.kind = CAMERA_EV_EXPFAIL;
            ev.reason = CAMERA_EXPFAIL_IO;
            camera_gallery_event(&u->model, &ev, now);
        }
    }
    if (acts & GALLERY_DO_SWAP) {
        u->front = !u->front;
        lv_image_set_src(u->show_img, NULL);
        if (u->slides[u->front].px) {
            lv_image_set_src(u->show_img, &u->slides[u->front].dsc);
            lv_obj_center(u->show_img);
        }
    }
    if (acts & GALLERY_DO_CAMERA) {
        u->want_camera = true;
    }
    if (acts & (GALLERY_DO_PICTURES | GALLERY_DO_TAKE_LIST)) {
        ask_for_pictures(u);
    }
}

/* An answered picture into the buffer the model says, or back to the helper. */
static void arrived(struct gallery_ui *u, const struct camera_event *ev, unsigned *acts)
{
    int cell = -1;
    int64_t now = now_ms();
    enum gallery_dest dest = camera_gallery_arrived(&u->model, ev, now, &cell, acts);
    bool ok = true;

    switch (dest) {
    case GALLERY_DEST_THUMB:
        ok = gpic_take(u, &u->thumbs[cell], ev->value, u->cell_img[cell]);
        break;
    case GALLERY_DEST_PHOTO:
        ok = gpic_take(u, &u->photo, ev->value, u->photo_img);
        break;
    case GALLERY_DEST_SLIDE_FRONT:
        ok = gpic_take(u, &u->slides[u->front], ev->value, u->show_img);
        break;
    case GALLERY_DEST_SLIDE_BACK:
        /* Not shown yet: the image object keeps pointing at the front one. */
        ok = gpic_take(u, &u->slides[!u->front], ev->value, NULL);
        break;
    case GALLERY_DEST_DROP:
        camera_session_take_picture(u->session, ev->value, NULL, 0, 0, NULL, NULL);
        break;
    }
    if (!ok) {
        camera_gallery_lost(&u->model, dest, cell, now);
    }
}

bool gallery_ui_poll(struct gallery_ui *u)
{
    struct camera_event ev;
    int64_t now = now_ms();
    unsigned acts = 0;
    bool changed;

    if (!u->active) {
        return false;
    }
    changed = camera_gallery_tick(&u->model, now, &acts);
    while (camera_session_poll(u->session, &ev, now)) {
        if (ev.kind == CAMERA_EV_IMAGE || ev.kind == CAMERA_EV_IMGFAIL) {
            arrived(u, &ev, &acts);
        } else {
            if (ev.kind == CAMERA_EV_LISTED) {
                u->list_total = (uint32_t)ev.bytes;
            }
            if (ev.kind == CAMERA_EV_EXITED && ev.reason != CAMERA_EXIT_NORMAL) {
                LOG_WARN("camera: library helper ended (%d), reason %d", ev.value, ev.reason);
            }
            acts |= camera_gallery_event(&u->model, &ev, now);
        }
        changed = true;
    }
    if (acts) {
        do_actions(u, acts);
        changed = true;
    }
    if (changed) {
        repaint(u);
    }
    return u->want_camera;
}

/* ---- taps --------------------------------------------------------------------------- */

static void act(struct gallery_ui *u, unsigned acts)
{
    do_actions(u, acts);
    repaint(u);
}

static void on_cell(lv_event_t *e)
{
    struct gallery_ui *u = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_current_target(e);
    int c;

    for (c = 0; c < GALLERY_PAGE_MAX; c++) {
        if (u->cells[c] == target) {
            act(u, camera_gallery_tap_cell(&u->model, c));
            return;
        }
    }
}

static void on_newer(lv_event_t *e)
{
    struct gallery_ui *u = lv_event_get_user_data(e);

    act(u, camera_gallery_newer(&u->model));
}

static void on_older(lv_event_t *e)
{
    struct gallery_ui *u = lv_event_get_user_data(e);

    act(u, camera_gallery_older(&u->model));
}

static void on_left(lv_event_t *e)
{
    struct gallery_ui *u = lv_event_get_user_data(e);

    act(u, camera_gallery_left(&u->model));
}

static void on_middle(lv_event_t *e)
{
    struct gallery_ui *u = lv_event_get_user_data(e);

    act(u, camera_gallery_middle(&u->model, now_ms()));
}

static void on_right(lv_event_t *e)
{
    struct gallery_ui *u = lv_event_get_user_data(e);

    act(u, camera_gallery_right(&u->model));
}

static void on_show(lv_event_t *e)
{
    struct gallery_ui *u = lv_event_get_user_data(e);

    act(u, camera_gallery_tap_slideshow(&u->model));
}

/* ---- layout ------------------------------------------------------------------------ */

static void layout(struct gallery_ui *u)
{
    struct pos_insets in;
    const lv_area_t *area;
    uint32_t cell;
    int c;
    int i;

    if (!u->active || !pocketui_layout_begin(&u->guard, u->frame, &in)) {
        return;
    }
    area = &u->guard.area;
    if (camera_gallery_layout_compute(&u->lay, lv_area_get_width(area), lv_area_get_height(area),
                                      in.left, in.top, in.right, in.bottom) != 0) {
        LOG_WARN("camera: body %dx%d is too small for the gallery", (int)lv_area_get_width(area),
                 (int)lv_area_get_height(area));
        return;
    }
    u->laid_out = true;
    cell = (uint32_t)u->lay.cell;
    for (c = 0; c < GALLERY_PAGE_MAX; c++) {
        if (c < u->lay.rows * u->lay.cols) {
            struct camera_rect r = camera_gallery_cell_rect(&u->lay, c);

            place(u->cells[c], &r);
            if (u->thumbs[c].cap_w != cell) {
                lv_image_set_src(u->cell_img[c], NULL);
                if (gpic_alloc(&u->thumbs[c], cell, cell)) {
                    u->thumbs[c].dsc.header.w = 0; /* nothing in it yet */
                }
            }
            lv_obj_set_width(u->cell_note[c], r.w - 8);
        } else {
            lv_image_set_src(u->cell_img[c], NULL);
            gpic_free(&u->thumbs[c]);
        }
    }
    place(u->panel, &u->lay.panel);
    lv_obj_set_width(u->detail, u->lay.panel.w - 2 * POCKETUI_PAD);
    place(u->photo_box, &u->lay.photo);
    for (i = 0; i < GALLERY_INFO_LINES; i++) {
        struct camera_rect r = { u->lay.info.x, u->lay.info.y + i * (GALLERY_INFO_H / 3),
                                 u->lay.info.w, GALLERY_INFO_H / 3 };

        place(u->info[i], &r);
    }
    /* The box takes the whole body but the status line, so a tap beside the
     * picture stops the slideshow too; the picture (lay.show, what a slot
     * holds) is centred in it. */
    place(u->show_box, &u->lay.show_touch);
    /* A new photo or slideshow size is asked for again; its buffer follows
     * the request (gpic_ensure). */
    do_actions(u, camera_gallery_set_sizes(&u->model, u->lay.rows * u->lay.cols, cell,
                                           (uint32_t)u->lay.photo.w, (uint32_t)u->lay.photo.h,
                                           (uint32_t)u->lay.show.w, (uint32_t)u->lay.show.h));
    repaint(u);
}

static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

/* ---- the gallery --------------------------------------------------------------------- */

struct gallery_ui *gallery_ui_create(lv_obj_t *root, struct camera_session *session)
{
    struct gallery_ui *u = calloc(1, sizeof(*u));
    int c;
    int i;

    if (!u) {
        return NULL;
    }
    u->session = session;
    camera_gallery_init(&u->model);
    u->frame = lv_obj_create(root);
    lv_obj_remove_style_all(u->frame);
    lv_obj_set_size(u->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(u->frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(u->frame, LV_OBJ_FLAG_HIDDEN);

    u->panel = lv_obj_create(u->frame);
    lv_obj_remove_style_all(u->panel);
    lv_obj_set_flex_flow(u->panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(u->panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(u->panel, 12, 0);
    lv_obj_clear_flag(u->panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    u->title = pocketui_label(u->panel, "", POS_STYLE_TITLE);
    u->detail = pocketui_label(u->panel, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(u->detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(u->detail, LV_TEXT_ALIGN_CENTER, 0);

    for (c = 0; c < GALLERY_PAGE_MAX; c++) {
        u->cells[c] = slab(u->frame, true);
        lv_obj_add_event_cb(u->cells[c], on_cell, LV_EVENT_CLICKED, u);
        lv_obj_add_flag(u->cells[c], LV_OBJ_FLAG_HIDDEN);
        u->cell_img[c] = picture_in(u->cells[c]);
        u->cell_note[c] = pocketui_label(u->cells[c], "Cannot show", POS_STYLE_CAPTION);
        lv_label_set_long_mode(u->cell_note[c], LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(u->cell_note[c], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(u->cell_note[c]);
        lv_obj_add_flag(u->cell_note[c], LV_OBJ_FLAG_HIDDEN);
    }

    u->photo_box = slab(u->frame, false);
    lv_obj_add_flag(u->photo_box, LV_OBJ_FLAG_HIDDEN);
    u->photo_img = picture_in(u->photo_box);
    for (i = 0; i < GALLERY_INFO_LINES; i++) {
        u->info[i] = pocketui_label(u->frame, "",
                                    i == 0 ? POS_STYLE_TEXT_PRIMARY : POS_STYLE_TEXT_SECONDARY);
        lv_label_set_long_mode(u->info[i], LV_LABEL_LONG_DOT);
        lv_obj_add_flag(u->info[i], LV_OBJ_FLAG_HIDDEN);
    }

    u->show_box = slab(u->frame, true);
    lv_obj_add_event_cb(u->show_box, on_show, LV_EVENT_CLICKED, u);
    lv_obj_add_flag(u->show_box, LV_OBJ_FLAG_HIDDEN);
    u->show_img = picture_in(u->show_box);

    u->status = pocketui_label(u->frame, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(u->status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(u->status, LV_TEXT_ALIGN_CENTER, 0);

    u->newer = button(u->frame, on_newer, u);
    u->older = button(u->frame, on_older, u);
    u->left = button(u->frame, on_left, u);
    u->middle = button(u->frame, on_middle, u);
    u->right = button(u->frame, on_right, u);
    lv_obj_add_event_cb(u->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, u);
    return u;
}

void gallery_ui_set_standalone(struct gallery_ui *u, bool standalone)
{
    if (u) {
        u->standalone = standalone;
        camera_gallery_set_standalone(&u->model, standalone);
    }
}

void gallery_ui_enter(struct gallery_ui *u)
{
    if (!u || u->active) {
        return;
    }
    u->active = true;
    u->want_camera = false;
    camera_gallery_set_standalone(&u->model, u->standalone);
    set_hidden(u->frame, false);
    /* Laid out afresh: the buffers were freed when the gallery was last left. */
    pocketui_layout_guard_reset(&u->guard);
    do_actions(u, camera_gallery_open(&u->model));
    lv_obj_update_layout(u->frame);
    layout(u);
    repaint(u);
}

void gallery_ui_leave(struct gallery_ui *u)
{
    if (!u || !u->active) {
        return;
    }
    /* The helper first: nothing may write the shared memory while pictures
     * are still being copied out of it (none are: every copy is done in the
     * tick that learns of it). */
    camera_session_abandon(u->session, GALLERY_LEAVE_GRACE_MS);
    free_pictures(u);
    camera_gallery_init(&u->model);
    u->active = false;
    u->laid_out = false;
    set_hidden(u->frame, true);
}

bool gallery_ui_active(const struct gallery_ui *u)
{
    return u && u->active;
}

void gallery_ui_destroy(struct gallery_ui *u)
{
    if (!u) {
        return;
    }
    lv_obj_remove_event_cb_with_user_data(u->frame, on_frame_size, u);
    free_pictures(u);
    free(u);
}
