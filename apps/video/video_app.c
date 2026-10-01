/*
 * Video: play a local MP4 file.
 *
 * This file is the screen. What each tap and each helper event does is
 * video_state.c; where things go is video_layout.c; the file list is
 * video_files.c. The video itself is not here at all: it is decoded by a
 * pos-video helper process per played file (video_session.c, ADR-012),
 * polled from an LVGL timer that only ever makes non-blocking calls. The only
 * waits on the LVGL thread are BACK and destroy() giving a running helper
 * VIDEO_DESTROY_GRACE_MS to close the sound card before it is killed, and the
 * folder listing (video_files.h).
 *
 * PICTURES. The helper scales and converts every picture to RGB565 at the
 * size that fits the frame and puts it in shared memory when it is due; the
 * timer copies the newest one into this app's own buffer and hands the slot
 * back in the same tick. The image object only ever points at memory this
 * app owns, so nothing is drawn from memory the helper writes, and nothing is
 * freed while an image points at it.
 *
 * SHAPE. The app takes the whole body (its padding is zeroed, as RIFT does)
 * and has no shell header in landscape (app.h NONE_LANDSCAPE), so the list
 * draws its own back slab there and fullscreen is the whole panel. Portrait
 * keeps the shell's header; fullscreen there is the body under it.
 * A tap on the picture plays or pauses; in fullscreen it leaves fullscreen.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"
#include "pocketlog/pocketlog.h"
#include "pocketpaths.h"
#include "pocketui.h"
#include "video_files.h"
#include "video_layout.h"
#include "video_session.h"
#include "video_state.h"

#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: not in lvgl.h */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pictures arrive at up to 30 a second; LVGL draws at most every 33 ms. */
#define VIDEO_POLL_MS 10
/* BACK and destroy(): time for a running helper to close the sound card. */
#define VIDEO_DESTROY_GRACE_MS 300
/* The progress bar's resolution: parts of the whole. */
#define VIDEO_SLIDER_RANGE 1000
/* How often the helper's statistics go to the log while playing. */
#define VIDEO_STATS_LOG_MS 10000
/* The progress bar's drawn track inside its VIDEO_LAYOUT_SLIDER_H rect. */
#define VIDEO_SLIDER_TRACK_H 10

struct video_picture {
    lv_image_dsc_t dsc;
    uint16_t *px;          /* room for VIDEO_VIEW_MAX_PIXELS while a file is open */
    uint32_t w;
    uint32_t h;
};

struct video_app {
    lv_obj_t *frame;
    /* the list */
    lv_obj_t *list_back;
    lv_obj_t *list_title;
    lv_obj_t *rescan;
    lv_obj_t *list;
    /* the player */
    lv_obj_t *box;
    lv_obj_t *img;
    lv_obj_t *panel;       /* a message in the picture's place */
    lv_obj_t *msg_title;
    lv_obj_t *msg_detail;
    lv_obj_t *status;
    lv_obj_t *slider;
    lv_obj_t *elapsed;
    lv_obj_t *duration;
    lv_obj_t *btn_back;
    lv_obj_t *btn_stop;
    lv_obj_t *btn_play;
    lv_obj_t *btn_full;

    struct pocketui_layout_guard guard;
    struct video_layout lay;
    bool laid_out;
    uint32_t view_w;       /* what the helper was last told */
    uint32_t view_h;
    lv_timer_t *timer;

    struct video_model model;
    struct video_session session;
    struct video_files files;
    char dir[VIDEO_PATH_MAX];
    struct video_picture pic;
    bool pic_shown;
    bool listed;           /* the folder has been read this visit */
    int64_t stats_logged_at;
};

static int64_t now_ms(void)
{
    return (int64_t)lv_tick_get();
}

static bool landscape_now(void)
{
    struct pocketos_orientation o;

    pocketos_shell_orientation(&o);
    return o.landscape;
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

static void place(lv_obj_t *obj, const struct video_rect *r)
{
    lv_obj_set_pos(obj, r->x, r->y);
    lv_obj_set_size(obj, r->w, r->h);
}

static void set_text(lv_obj_t *label, const char *text)
{
    const char *now = lv_label_get_text(label);

    if (!now || strcmp(now, text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* ---- the picture ---------------------------------------------------------------- */

static void picture_free(struct video_app *a)
{
    lv_image_set_src(a->img, NULL);
    a->pic_shown = false;
    if (a->pic.px) {
        lv_image_cache_drop(&a->pic.dsc);
        free(a->pic.px);
    }
    memset(&a->pic, 0, sizeof(a->pic));
}

static bool picture_alloc(struct video_app *a)
{
    picture_free(a);
    a->pic.px = malloc((size_t)VIDEO_VIEW_MAX_PIXELS * sizeof(uint16_t));
    return a->pic.px != NULL;
}

/* The newest picture from the helper into our buffer. */
static void take_picture(struct video_app *a)
{
    uint32_t w = 0;
    uint32_t h = 0;

    if (!a->pic.px || a->model.status == VIDEO_ST_ERROR || a->model.status == VIDEO_ST_LIST) {
        video_session_take_frame(&a->session, NULL, 0, NULL, NULL);
        return;
    }
    if (video_session_take_frame(&a->session, a->pic.px, VIDEO_VIEW_MAX_PIXELS, &w, &h) != 1) {
        return;
    }
    lv_image_cache_drop(&a->pic.dsc);
    if (!a->pic_shown || w != a->pic.w || h != a->pic.h) {
        a->pic.w = w;
        a->pic.h = h;
        a->pic.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        a->pic.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        a->pic.dsc.header.w = w;
        a->pic.dsc.header.h = h;
        a->pic.dsc.header.stride = w * 2;
        a->pic.dsc.data = (const uint8_t *)a->pic.px;
        a->pic.dsc.data_size = w * h * 2;
        lv_image_set_src(a->img, &a->pic.dsc);
        lv_obj_center(a->img);
        a->pic_shown = true;
    }
    lv_obj_invalidate(a->img);
}

/* ---- buttons -------------------------------------------------------------------- */

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = pocketui_button(parent, text, cb, user);

    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_CLIP);
    return b;
}

/* Primary or secondary, and enabled or not (DS section 9). Only a change is
 * applied; the role last applied is kept in the user data, offset by one. */
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
    set_text(lv_obj_get_child(b, 0), text);
}

/* ---- painting ------------------------------------------------------------------- */

static void repaint(struct video_app *a)
{
    const struct video_model *m = &a->model;
    bool player = m->status != VIDEO_ST_LIST;
    bool controls = player && !m->fullscreen;
    bool can = video_model_can_control(m);
    char t1[16];
    char t2[16];
    char line[VIDEO_FILES_NAME_MAX + 64];
    const char *audio;

    set_hidden(a->list_back, player || !a->lay.landscape);
    set_hidden(a->list_title, player);
    set_hidden(a->rescan, player);
    set_hidden(a->list, player);
    set_hidden(a->box, !player);
    set_hidden(a->status, !controls);
    set_hidden(a->slider, !controls);
    set_hidden(a->elapsed, !controls);
    set_hidden(a->duration, !controls);
    set_hidden(a->btn_back, !controls);
    set_hidden(a->btn_stop, !controls);
    set_hidden(a->btn_play, !controls);
    set_hidden(a->btn_full, !controls);
    if (!player) {
        return;
    }

    /* The picture, or a message in its place. */
    set_hidden(a->img, !a->pic_shown || m->status == VIDEO_ST_ERROR);
    if (m->status == VIDEO_ST_ERROR) {
        set_text(a->msg_title, m->error_title);
        set_text(a->msg_detail, m->error_detail);
        set_hidden(a->panel, false);
    } else if (!a->pic_shown) {
        set_text(a->msg_title, m->status == VIDEO_ST_OPENING ? "Opening..." : "");
        set_text(a->msg_detail, m->name);
        set_hidden(a->panel, false);
    } else {
        set_hidden(a->panel, true);
    }
    if (!controls) {
        return;
    }
    audio = video_model_audio_text(m);
    snprintf(line, sizeof(line), "%s\n%s%s%s", m->name, video_model_status_text(m), *audio ? " - " : "",
             audio);
    set_text(a->status, line);
    video_files_time_text(video_model_shown_pos(m), t1, sizeof(t1));
    video_files_time_text(m->duration_ms, t2, sizeof(t2));
    set_text(a->elapsed, t1);
    set_text(a->duration, m->duration_ms > 0 ? t2 : "--:--");
    if (!lv_slider_is_dragged(a->slider)) {
        int32_t v = m->duration_ms > 0
                        ? (int32_t)(video_model_shown_pos(m) * VIDEO_SLIDER_RANGE / m->duration_ms)
                        : 0;

        if (lv_slider_get_value(a->slider) != v) {
            lv_slider_set_value(a->slider, v, LV_ANIM_OFF);
        }
    }
    if (can && m->duration_ms > 0) {
        lv_obj_clear_state(a->slider, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(a->slider, LV_STATE_DISABLED);
    }
    button_text(a->btn_play, m->want_play ? "PAUSE" : "PLAY");
    button_style(a->btn_play, true, can);
    button_style(a->btn_stop, false, can);
    button_style(a->btn_full, false, true);
    button_style(a->btn_back, false, true);
}

/* ---- the list ------------------------------------------------------------------- */

static void act(struct video_app *a, unsigned acts);

static void on_row(lv_event_t *e)
{
    struct video_app *a = lv_event_get_user_data(e);
    intptr_t i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));

    if (i >= 0 && i < a->files.count) {
        act(a, video_model_choose(&a->model, a->files.items[i].name));
    }
}

static void rebuild_list(struct video_app *a)
{
    char text[VIDEO_PATH_MAX + 96];
    int i;

    video_files_scan(&a->files, a->dir);
    lv_obj_clean(a->list);
    if (a->files.count == 1) {
        snprintf(text, sizeof(text), "%s1 video", a->lay.landscape ? "Video - " : "");
    } else {
        snprintf(text, sizeof(text), "%s%d videos%s", a->lay.landscape ? "Video - " : "",
                 a->files.count, a->files.more ? " (more not shown)" : "");
    }
    set_text(a->list_title, text);
    if (a->files.count == 0) {
        lv_obj_t *l;

        snprintf(text, sizeof(text), "No videos yet.\nCopy MP4 files (H.264) to\n%s", a->dir);
        l = pocketui_label(a->list, text, POS_STYLE_TEXT_SECONDARY);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        return;
    }
    for (i = 0; i < a->files.count; i++) {
        lv_obj_t *row = lv_obj_create(a->list);
        lv_obj_t *name;
        lv_obj_t *size;
        char sz[24];

        lv_obj_remove_style_all(row);
        pos_style_add(row, POS_STYLE_SLAB, 0);
        pos_style_add(row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_size(row, LV_PCT(100), POCKETUI_ROW_H);
        lv_obj_set_style_pad_hor(row, VIDEO_LAYOUT_PAD, 0);
        lv_obj_set_style_pad_column(row, VIDEO_LAYOUT_GAP, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, a);
        name = pocketui_label(row, a->files.items[i].name, POS_STYLE_ROW_TITLE);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(name, 1);
        video_files_size_text(a->files.items[i].bytes, sz, sizeof(sz));
        size = pocketui_label(row, sz, POS_STYLE_TEXT_SECONDARY);
        (void)size;
    }
}

/* ---- actions ---------------------------------------------------------------------- */

static void layout(struct video_app *a);

static void send_view(struct video_app *a)
{
    uint32_t w = (uint32_t)a->lay.box.w;
    uint32_t h = (uint32_t)a->lay.box.h;

    if (w > VIDEO_VIEW_MAX_W) {
        w = VIDEO_VIEW_MAX_W;
    }
    if (h > VIDEO_VIEW_MAX_H) {
        h = VIDEO_VIEW_MAX_H;
    }
    if (!w || !h || !video_session_active(&a->session) || (w == a->view_w && h == a->view_h)) {
        return;
    }
    if (video_session_view(&a->session, w, h) == 0) {
        a->view_w = w;
        a->view_h = h;
    }
}

static void start(struct video_app *a)
{
    struct video_session_config cfg;
    char path[VIDEO_PATH_MAX];
    char err[128] = "";
    int64_t now = now_ms();
    int n;

    n = snprintf(path, sizeof(path), "%s/%s", a->dir, a->model.name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        video_model_start_failed(&a->model, "The file's path is too long.");
        return;
    }
    if (!picture_alloc(a)) {
        video_model_start_failed(&a->model, "Not enough memory for the picture.");
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.volume_percent = pocketos_shell_volume_effective();
    if (video_session_start(&a->session, &cfg, now, err, sizeof(err)) != 0) {
        LOG_WARN("video: the player could not start: %s", err);
        video_model_start_failed(&a->model, err);
        return;
    }
    a->view_w = a->view_h = 0;
    send_view(a);
    if (video_session_open(&a->session, path, now) != 0) {
        video_session_abandon(&a->session, VIDEO_DESTROY_GRACE_MS);
        video_model_start_failed(&a->model, "The file could not be handed to the player.");
    }
}

static void act(struct video_app *a, unsigned acts)
{
    int64_t now = now_ms();

    if (acts & VIDEO_ACT_ABANDON) {
        video_session_abandon(&a->session, VIDEO_DESTROY_GRACE_MS);
        picture_free(a);
    }
    if (acts & VIDEO_ACT_LAYOUT) {
        pocketui_layout_guard_reset(&a->guard);
        layout(a);
    }
    if (acts & VIDEO_ACT_START) {
        start(a);
    }
    if (acts & VIDEO_ACT_PLAY) {
        video_session_play(&a->session, now);
    }
    if (acts & VIDEO_ACT_PAUSE) {
        video_session_pause(&a->session, now);
    }
    if (acts & VIDEO_ACT_STOP) {
        video_session_stop(&a->session, now);
    }
    if (acts & VIDEO_ACT_SEEK) {
        video_session_seek(&a->session, a->model.seek_sent, now);
    }
    if (acts & VIDEO_ACT_RESCAN) {
        rebuild_list(a);
    }
    repaint(a);
    if (acts & VIDEO_ACT_HOME) {
        /* Last: this destroys the app. */
        pocketos_shell_go_home();
    }
}

/* ---- the timer ---------------------------------------------------------------------- */

static void on_poll(lv_timer_t *t)
{
    struct video_app *a = lv_timer_get_user_data(t);
    struct video_event ev;
    int64_t now = now_ms();
    bool changed = false;

    while (video_session_poll(&a->session, &ev, now)) {
        unsigned acts;

        if (ev.kind == VIDEO_EV_FRAME) {
            take_picture(a);
        } else if (ev.kind == VIDEO_EV_OPENED) {
            LOG_INFO("video: opened %ux%u %s, %lld ms, sound %s", ev.w, ev.h, ev.codec,
                     (long long)ev.ms, ev.word);
        } else if (ev.kind == VIDEO_EV_STATS) {
            if (now - a->stats_logged_at >= VIDEO_STATS_LOG_MS) {
                a->stats_logged_at = now;
                LOG_INFO("video: %u.%u fps, shown %llu dropped %llu late %llu, helper cpu %u%% "
                         "rss %u kB, xruns %u",
                         ev.stats.fps_x10 / 10, ev.stats.fps_x10 % 10,
                         (unsigned long long)ev.stats.shown, (unsigned long long)ev.stats.dropped,
                         (unsigned long long)ev.stats.late, ev.stats.cpu_pct, ev.stats.rss_kb,
                         ev.stats.xruns);
            }
        } else if (ev.kind == VIDEO_EV_EXITED && ev.reason != VIDEO_EXIT_NORMAL) {
            LOG_WARN("video: helper ended (%d), reason %d", ev.value, ev.reason);
        } else if (ev.kind == VIDEO_EV_ERROR || ev.kind == VIDEO_EV_OPENFAIL) {
            LOG_WARN("video: %s %s", ev.kind == VIDEO_EV_ERROR ? "error" : "openfail", ev.word);
        }
        acts = video_model_event(&a->model, &ev);
        if (acts) {
            act(a, acts);
        }
        changed = true;
    }
    if (changed) {
        repaint(a);
    }
}

/* ---- taps --------------------------------------------------------------------------- */

static void on_list_back(lv_event_t *e)
{
    struct video_app *a = lv_event_get_user_data(e);

    act(a, video_model_back(&a->model));
}

static void on_rescan(lv_event_t *e)
{
    struct video_app *a = lv_event_get_user_data(e);

    act(a, video_model_rescan(&a->model));
}

static void on_back(lv_event_t *e)
{
    struct video_app *a = lv_event_get_user_data(e);

    act(a, video_model_back(&a->model));
}

static void on_play(lv_event_t *e)
{
    struct video_app *a = lv_event_get_user_data(e);

    act(a, video_model_toggle(&a->model));
}

static void on_stop(lv_event_t *e)
{
    struct video_app *a = lv_event_get_user_data(e);

    act(a, video_model_stop(&a->model));
}

static void on_full(lv_event_t *e)
{
    struct video_app *a = lv_event_get_user_data(e);

    act(a, video_model_fullscreen(&a->model));
}

static void on_box(lv_event_t *e)
{
    struct video_app *a = lv_event_get_user_data(e);

    if (a->model.fullscreen) {
        act(a, video_model_fullscreen(&a->model));
    } else {
        act(a, video_model_toggle(&a->model));
    }
}

static int64_t slider_ms(struct video_app *a)
{
    return (int64_t)lv_slider_get_value(a->slider) * a->model.duration_ms / VIDEO_SLIDER_RANGE;
}

static void on_slider(lv_event_t *e)
{
    struct video_app *a = lv_event_get_user_data(e);

    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        act(a, video_model_drag_end(&a->model, slider_ms(a)));
    } else {
        video_model_drag(&a->model, slider_ms(a));
        repaint(a);
    }
}

/* ---- layout ---------------------------------------------------------------------------- */

static void layout(struct video_app *a)
{
    struct pos_insets in;
    const lv_area_t *area;
    bool was_landscape = a->lay.landscape;

    if (!pocketui_layout_begin(&a->guard, a->frame, &in)) {
        return;
    }
    area = &a->guard.area;
    if (video_layout_compute(&a->lay, lv_area_get_width(area), lv_area_get_height(area), in.left,
                             in.top, in.right, in.bottom, landscape_now(),
                             a->model.fullscreen) != 0) {
        LOG_WARN("video: body %dx%d is too small for the player", (int)lv_area_get_width(area),
                 (int)lv_area_get_height(area));
    }
    a->laid_out = true;
    place(a->list_back, &a->lay.back);
    place(a->list_title, &a->lay.title);
    place(a->rescan, &a->lay.rescan);
    place(a->list, &a->lay.list);
    place(a->box, &a->lay.box);
    place(a->status, &a->lay.status);
    lv_obj_set_pos(a->slider, a->lay.slider.x,
                   a->lay.slider.y + (a->lay.slider.h - VIDEO_SLIDER_TRACK_H) / 2);
    lv_obj_set_size(a->slider, a->lay.slider.w, a->lay.slider.h ? VIDEO_SLIDER_TRACK_H : 0);
    place(a->elapsed, &a->lay.elapsed);
    place(a->duration, &a->lay.duration);
    place(a->btn_back, &a->lay.btn_back);
    place(a->btn_stop, &a->lay.btn_stop);
    place(a->btn_play, &a->lay.btn_play);
    place(a->btn_full, &a->lay.btn_full);
    button_text(a->btn_full, a->lay.landscape ? "FULLSCREEN" : "FULL");
    lv_obj_set_width(a->msg_detail, a->lay.box.w > 2 * VIDEO_LAYOUT_PAD
                                        ? a->lay.box.w - 2 * VIDEO_LAYOUT_PAD
                                        : a->lay.box.w);
    if (a->pic_shown) {
        lv_obj_center(a->img);
    }
    if ((!a->listed || was_landscape != a->lay.landscape) && a->model.status == VIDEO_ST_LIST) {
        /* The first pass reads the folder; a turn rewrites the title, which
         * says "Video" only where there is no shell header. */
        rebuild_list(a);
        a->listed = true;
    }
    send_view(a);
    repaint(a);
}

static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

/* ---- building --------------------------------------------------------------------------- */

static lv_obj_t *label(lv_obj_t *parent, enum pos_style_role role, lv_text_align_t align)
{
    lv_obj_t *l = pocketui_label(parent, "", role);

    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

static void build(struct video_app *a, lv_obj_t *root)
{
    lv_obj_t *o;

    /* The whole body: the design's padding is put back by the layout. The
     * body is the shell's object for this app only, and goes with it. */
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    a->frame = lv_obj_create(root);
    lv_obj_remove_style_all(a->frame);
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(a->frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* the list */
    a->list_back = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->list_back);
    pos_style_add(a->list_back, POS_STYLE_SLAB, 0);
    pos_style_add(a->list_back, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_clear_flag(a->list_back, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(a->list_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->list_back, on_list_back, LV_EVENT_CLICKED, a);
    o = pocketui_label(a->list_back, LV_SYMBOL_LEFT, POS_STYLE_SYMBOL);
    pos_style_add(o, POS_STYLE_ACCENT_TEXT, 0);
    lv_obj_center(o);

    a->list_title = label(a->frame, POS_STYLE_TITLE, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_pad_top(a->list_title, 16, 0);
    a->rescan = button(a->frame, "RESCAN", on_rescan, a);
    button_style(a->rescan, false, true);

    a->list = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->list);
    lv_obj_set_flex_flow(a->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(a->list, VIDEO_LAYOUT_GAP, 0);
    lv_obj_add_flag(a->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->list, LV_DIR_VER);

    /* the player */
    a->box = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->box);
    pos_style_add(a->box, POS_STYLE_SLAB, 0);
    lv_obj_clear_flag(a->box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(a->box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->box, on_box, LV_EVENT_CLICKED, a);
    a->img = lv_image_create(a->box);
    lv_obj_clear_flag(a->img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->img, LV_OBJ_FLAG_HIDDEN);

    a->panel = lv_obj_create(a->box);
    lv_obj_remove_style_all(a->panel);
    lv_obj_set_size(a->panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_center(a->panel);
    lv_obj_set_flex_flow(a->panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(a->panel, 12, 0);
    lv_obj_clear_flag(a->panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    a->msg_title = pocketui_label(a->panel, "", POS_STYLE_TITLE);
    a->msg_detail = pocketui_label(a->panel, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(a->msg_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(a->msg_detail, LV_TEXT_ALIGN_CENTER, 0);

    a->status = pocketui_label(a->frame, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(a->status, LV_LABEL_LONG_WRAP);

    a->slider = lv_slider_create(a->frame);
    lv_obj_remove_style_all(a->slider);
    pos_style_add(a->slider, POS_STYLE_SLAB, 0);
    pos_style_add(a->slider, POS_STYLE_BUTTON_PRIMARY, LV_PART_INDICATOR);
    pos_style_add(a->slider, POS_STYLE_BUTTON_PRIMARY, LV_PART_KNOB);
    lv_obj_set_style_pad_all(a->slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(a->slider, 8, LV_PART_KNOB);
    lv_obj_set_style_radius(a->slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_opa(a->slider, LV_OPA_40, LV_STATE_DISABLED);
    lv_slider_set_range(a->slider, 0, VIDEO_SLIDER_RANGE);
    /* The track is thin (place_slider()); its whole layout rect, and the
     * DS minimum around the knob, is the touch target. */
    lv_obj_set_ext_click_area(a->slider, (VIDEO_LAYOUT_SLIDER_H - VIDEO_SLIDER_TRACK_H) / 2);
    lv_obj_add_event_cb(a->slider, on_slider, LV_EVENT_VALUE_CHANGED, a);
    lv_obj_add_event_cb(a->slider, on_slider, LV_EVENT_RELEASED, a);

    a->elapsed = label(a->frame, POS_STYLE_VALUE, LV_TEXT_ALIGN_LEFT);
    a->duration = label(a->frame, POS_STYLE_VALUE, LV_TEXT_ALIGN_RIGHT);

    a->btn_back = button(a->frame, "BACK", on_back, a);
    a->btn_stop = button(a->frame, "STOP", on_stop, a);
    a->btn_play = button(a->frame, "PLAY", on_play, a);
    a->btn_full = button(a->frame, "FULL", on_full, a);
}

/* ---- the app ------------------------------------------------------------------------------ */

static void *video_create(lv_obj_t *root)
{
    struct video_app *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    video_session_init(&a->session);
    video_model_init(&a->model);
    if (video_files_dir(a->dir, sizeof(a->dir)) != 0) {
        snprintf(a->dir, sizeof(a->dir), "/root/" VIDEO_FILES_SUBDIR);
    }
    /* The folder the list asks for: made on the first visit so there is
     * somewhere to copy videos to. Never written otherwise. */
    if (pocketos_mkdir_p(a->dir, 0755) != 0) {
        LOG_WARN("video: the videos folder could not be made");
    }
    build(a, root);
    pocketos_shell_set_status_hint(strcmp(video_session_backend(), "fake") == 0 ? "SIMULATED" : "");
    a->timer = lv_timer_create(on_poll, VIDEO_POLL_MS, a);
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    layout(a);
    if (!a->listed) {
        rebuild_list(a);
        a->listed = true;
    }
    repaint(a);
    return a;
}

static void video_destroy(void *priv)
{
    struct video_app *a = priv;

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
     * session is about to unmap, and the sound card is closed. */
    video_session_abandon(&a->session, VIDEO_DESTROY_GRACE_MS);
    /* No image may point at a buffer being freed. */
    picture_free(a);
    free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_video);

const struct pocketos_app app_video = {
    .id = "video",
    .name = "Video",
    .icon = LV_SYMBOL_VIDEO,
    .icon_mask = &pos_app_icon_video,
    .create = video_create,
    .tick = NULL,
    .destroy = video_destroy,
    /* Fullscreen (DS section 30.8): no status bar. In landscape no shell
     * header either (DS section 37.2): the list draws its own back slab, and
     * the picture can take the whole panel. */
    .chrome = POCKETOS_CHROME_NONE,
    .header = POCKETOS_HEADER_NONE_LANDSCAPE,
};
