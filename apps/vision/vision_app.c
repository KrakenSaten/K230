/*
 * Vision: the camera's live picture with what the KPU sees in it - boxes
 * with a class, a confidence and a persistent id - and one line across the
 * picture that counts what crosses it; in TRAFFIC mode, traffic only,
 * counted per class, with a direction on every box and a speed for what
 * crosses the two speed lines (docs/apps/VISION.md).
 *
 * This file is the screen. What state it is in is vision_model.c; where
 * things go is vision_layout.c; the camera and the detector are not here at
 * all: they live in a pos-vision helper process (vision_session.c, the same
 * shape as Camera's under ADR-006), polled from an LVGL timer that only
 * ever makes non-blocking calls. The only wait on the LVGL thread is
 * destroy() and Try again giving a running helper VISION_DESTROY_GRACE_MS
 * to close the camera before it is killed.
 *
 * PICTURES. As Camera: the helper converts every preview frame to RGB565 at
 * the picture box's size into shared memory; the timer copies the newest
 * one into this app's own buffer and hands the slot back in the same tick.
 * The image object only ever points at memory this app owns.
 *
 * BOXES. The helper says where every tracked object is, in the picture's
 * own pixels (pocketvision_proto.h); the timer moves a fixed set of
 * VISION_MAX_SHOWN outline objects onto them and hides the rest. Nothing is
 * created per frame.
 *
 * FULLSCREEN (DS §30.8): the app declares NONE; the shell's header carries
 * the back button and the hint (SIMULATED under the fake backend).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pocketvision/vision_labels.h"
#include "vision_layout.h"
#include "vision_model.h"
#include "vision_session.h"
#include "vision_store.h"
#include "vision_trails.h"

#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: not in lvgl.h */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VISION_POLL_MS 33
/* destroy() and Try again: time for the helper to close the camera and the
 * detector before it is killed. Camera's helper needs well under 300 ms;
 * Vision's took ~420 ms after SIGTERM on unit B (it unloads the model and
 * returns the KPU's shared pool), and a SIGKILL half way through closing the
 * camera and the KPU is exactly the kind of exit this helper must not get.
 * Bounded like every wait here; it only runs when the user leaves. */
#define VISION_DESTROY_GRACE_MS 1000
#define VISION_LABEL_MAX 48

struct vision_picture {
    lv_image_dsc_t dsc;
    uint16_t *px;
    uint32_t w;
    uint32_t h;
};

_Static_assert(VISION_SHEET_ROWS == VISION_SHEET_ROWS_MAX && VISION_SHEET_CELLS == VISION_SHEET_CELLS_MAX,
               "the model's sheet and the layout's are the same size");

struct vision_app;

/* Which sheet cell a button is, for its tap. */
struct vision_cell_ref {
    struct vision_app *a;
    int row;
    int cell;
};

struct vision_app {
    lv_obj_t *frame;
    lv_obj_t *box;        /* the picture's place: a slab */
    lv_obj_t *img;
    lv_obj_t *panel;      /* title and detail, in the picture's place */
    lv_obj_t *title;
    lv_obj_t *detail;
    lv_obj_t *outline[VISION_MAX_SHOWN];
    lv_obj_t *tag[VISION_MAX_SHOWN];
    /* Trails: VISION_TRAIL_POINTS dots per trail, made once and moved. */
    lv_obj_t *dot[VISION_TRAIL_TRACKS][VISION_TRAIL_POINTS];
    struct vision_trails trails;
    uint32_t trails_seq;  /* the det line the trails were last fed */
    lv_obj_t *line;       /* the counting line, drawn as a 2 px object */
    lv_obj_t *sline[2];   /* the speed lines A and B */
    lv_obj_t *mark;       /* COLOR: a small square at the matches' centroid */
    lv_obj_t *status;
    lv_obj_t *count_a;    /* "DOWN 3" */
    lv_obj_t *count_b;
    lv_obj_t *btn[VISION_BUTTONS]; /* by role (enum vision_button) */
    lv_obj_t *retry_btn;
    /* The sheet over the picture: made once, filled from the model. */
    lv_obj_t *sheet;
    lv_obj_t *sheet_title;
    lv_obj_t *sheet_caption[VISION_SHEET_ROWS];
    lv_obj_t *sheet_cell[VISION_SHEET_ROWS][VISION_SHEET_CELLS];
    struct vision_cell_ref sheet_ref[VISION_SHEET_ROWS][VISION_SHEET_CELLS];
    int sheet_code[VISION_SHEET_ROWS][VISION_SHEET_CELLS];

    struct pocketui_layout_guard guard;
    struct vision_layout lay;
    uint32_t lay_frame_w; /* the preview the layout was chosen for */
    uint32_t lay_frame_h;
    enum vision_mode lay_mode;
    bool laid_out;
    lv_timer_t *timer;

    struct vision_model model;
    struct vision_session session;
    struct vision_picture preview;
    bool shown;           /* img shows the preview */
    const char *hint_shown;
    char status_buf[256];
};

static int64_t now_ms(void)
{
    return (int64_t)lv_tick_get();
}

static int display_rotation(void)
{
    return pos_rotation_degrees(pocketui_display_geometry()->rotation);
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

static void place(lv_obj_t *obj, const struct vision_rect *r)
{
    lv_obj_set_pos(obj, r->x, r->y);
    lv_obj_set_size(obj, r->w, r->h);
}

/* ---- pictures ------------------------------------------------------------------ */

static void picture_free(struct vision_picture *p)
{
    if (p->px) {
        lv_image_cache_drop(&p->dsc);
        free(p->px);
    }
    memset(p, 0, sizeof(*p));
}

static bool picture_alloc(struct vision_picture *p, uint32_t w, uint32_t h)
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

static void show(struct vision_app *a, bool on)
{
    if (on == a->shown) {
        return;
    }
    a->shown = on;
    lv_image_set_src(a->img, on && a->preview.px ? &a->preview.dsc : NULL);
}

/* ---- text helpers ----------------------------------------------------------------- */

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = pocketui_button(parent, text, cb, user);

    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_CLIP);
    return b;
}

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

static const char *dir_mark(unsigned dir)
{
    switch (dir) {
    case VISION_DIR_LEFT: return " <";
    case VISION_DIR_RIGHT: return " >";
    case VISION_DIR_UP: return " ^";
    case VISION_DIR_DOWN: return " v";
    default: return "";
    }
}

/* READ: a box on every line read, its text as the tag (the model's copy:
 * it stays while HOLD is on). */
static void draw_text_boxes(struct vision_app *a)
{
    const struct vision_text_report *r = &a->model.text;
    int i;

    for (i = 0; i < VISION_MAX_SHOWN; i++) {
        bool on = a->model.text_valid && i < r->n && i < VISION_TEXT_LINES && a->model.state == VISION_LIVE &&
                  a->model.live;

        set_hidden(a->outline[i], !on);
        set_hidden(a->tag[i], !on);
        if (!on) {
            continue;
        }
        lv_obj_set_pos(a->outline[i], r->line[i].x, r->line[i].y);
        lv_obj_set_size(a->outline[i], r->line[i].w, r->line[i].h);
        {
            char text[VISION_LABEL_MAX];

            vision_model_text_ascii(r->line[i].text, text, sizeof(text));
            label_text(a->tag[i], text);
        }
        lv_obj_set_pos(a->tag[i], r->line[i].x, r->line[i].y > 22 ? r->line[i].y - 22 : r->line[i].y);
    }
}

static void draw_boxes(struct vision_app *a, const struct vision_view_text *s)
{
    int n = 0;
    uint32_t seq;
    const struct vision_shown *t = vision_session_tracks(&a->session, &n, &seq);
    bool traffic = s->traffic;
    int i;

    if (s->read) {
        draw_text_boxes(a);
        return;
    }
    for (i = 0; i < VISION_MAX_SHOWN; i++) {
        bool on = i < n && a->model.state == VISION_LIVE && a->model.live;

        set_hidden(a->outline[i], !on);
        set_hidden(a->tag[i], !on);
        if (!on) {
            continue;
        }
        lv_obj_set_pos(a->outline[i], t[i].x, t[i].y);
        lv_obj_set_size(a->outline[i], t[i].w, t[i].h);
        {
            char text[VISION_LABEL_MAX];
            const char *what = s->faces ? "face" : vision_label(t[i].cls);

            if (traffic && !s->ids && !(s->speeds && t[i].kmh10)) {
                /* LABELS off: the box says nothing. */
                text[0] = '\0';
            } else if (traffic && !s->ids) {
                snprintf(text, sizeof(text), "%u.%u km/h", t[i].kmh10 / 10, t[i].kmh10 % 10);
            } else if (t[i].id && traffic) {
                /* "#7 car > 43 km/h": the id, the class, the way it has
                 * gone, and the speed once the lines have measured it. */
                if (t[i].kmh10 && s->speeds) {
                    snprintf(text, sizeof(text), "#%u %s%s %u.%u km/h", t[i].id, what,
                             dir_mark(t[i].dir), t[i].kmh10 / 10, t[i].kmh10 % 10);
                } else {
                    snprintf(text, sizeof(text), "#%u %s%s %u%%", t[i].id, what,
                             dir_mark(t[i].dir), t[i].conf / 10);
                }
            } else if (t[i].id && s->ids) {
                snprintf(text, sizeof(text), "#%u %s %u%%", t[i].id, what,
                         t[i].conf / 10);
            } else {
                /* DETECT: what it is and how sure, no identity. */
                snprintf(text, sizeof(text), "%s %u%%", what, t[i].conf / 10);
            }
            label_text(a->tag[i], text);
            set_hidden(a->tag[i], text[0] == '\0');
        }
        /* The tag sits on the box's top edge, inside the picture. */
        lv_obj_set_pos(a->tag[i], t[i].x, t[i].y > 22 ? t[i].y - 22 : t[i].y);
    }
}

/* The trails: fed once per det line, drawn as dots behind the boxes. */
static void draw_trails(struct vision_app *a, const struct vision_view_text *s)
{
    int n = 0;
    uint32_t seq = 0;
    const struct vision_shown *t = vision_session_tracks(&a->session, &n, &seq);
    bool on = s->trails && a->model.state == VISION_LIVE && a->model.live;
    int i;
    int k;

    if (!on) {
        vision_trails_clear(&a->trails);
    } else if (seq != a->trails_seq) {
        vision_trails_update(&a->trails, t, n);
    }
    a->trails_seq = seq;
    for (i = 0; i < VISION_TRAIL_TRACKS; i++) {
        int32_t xs[VISION_TRAIL_POINTS];
        int32_t ys[VISION_TRAIL_POINTS];
        int np = on ? vision_trails_points(&a->trails, i, xs, ys, VISION_TRAIL_POINTS) : 0;

        for (k = 0; k < VISION_TRAIL_POINTS; k++) {
            /* The newest point is under the box: a trail is where it was. */
            bool show = np >= 2 && k < np - 1;

            set_hidden(a->dot[i][k], !show);
            if (show) {
                lv_obj_set_pos(a->dot[i][k], xs[k] - 2, ys[k] - 2);
            }
        }
    }
}

/* A 2 px line object on the picture, from its per-mille endpoints (a line
 * is always across or down). */
static void place_line(struct vision_app *a, lv_obj_t *obj, const int32_t pm[4])
{
    if (pm[1] == pm[3]) {
        /* across */
        int32_t y = (int32_t)(((int64_t)pm[1] * (a->preview.h - 1)) / 1000);

        lv_obj_set_pos(obj, 0, y - 1);
        lv_obj_set_size(obj, (int32_t)a->preview.w, 2);
    } else {
        int32_t x = (int32_t)(((int64_t)pm[0] * (a->preview.w - 1)) / 1000);

        lv_obj_set_pos(obj, x - 1, 0);
        lv_obj_set_size(obj, 2, (int32_t)a->preview.h);
    }
}

static void draw_lines(struct vision_app *a)
{
    int32_t pm[8];
    bool live = a->model.state == VISION_LIVE && a->model.live && a->preview.w && a->preview.h &&
                !vision_model_pixel_mode(&a->model);
    bool on = live && vision_model_line_pm(&a->model, pm);
    bool speed;

    set_hidden(a->line, !on);
    if (on) {
        place_line(a, a->line, pm);
    }
    speed = live && a->model.mode == VISION_MODE_TRAFFIC && vision_model_speed_pm(&a->model, pm);
    set_hidden(a->sline[0], !speed);
    set_hidden(a->sline[1], !speed);
    if (speed) {
        place_line(a, a->sline[0], pm);
        place_line(a, a->sline[1], pm + 4);
    }
}

/* The sheet: hidden, or laid out on the picture and filled from the model.
 * A sheet that does not fit the picture stays hidden (and is logged once
 * per shape: it never happens on the reference panel,
 * tests/vision_model_test.c). */
static void draw_sheet(struct vision_app *a)
{
    struct vision_sheet_view v;
    struct vision_sheet_layout sl;
    int cells[VISION_SHEET_ROWS];
    int r;
    int c;

    vision_model_sheet(&a->model, &v);
    for (r = 0; r < v.rows; r++) {
        cells[r] = v.row[r].cells;
    }
    if (v.kind == VISION_SHEET_NONE || v.rows == 0 ||
        vision_layout_sheet(&sl, a->lay.picture.w, a->lay.picture.h, v.rows, cells) != 0) {
        set_hidden(a->sheet, true);
        return;
    }
    lv_obj_set_pos(a->sheet, 0, 0);
    lv_obj_set_size(a->sheet, a->lay.picture.w, a->lay.picture.h);
    place(a->sheet_title, &sl.title);
    label_text(a->sheet_title, v.title);
    for (r = 0; r < VISION_SHEET_ROWS; r++) {
        bool row_on = r < v.rows;

        set_hidden(a->sheet_caption[r], !row_on);
        if (row_on) {
            place(a->sheet_caption[r], &sl.caption[r]);
            label_text(a->sheet_caption[r], v.row[r].caption);
        }
        for (c = 0; c < VISION_SHEET_CELLS; c++) {
            lv_obj_t *b = a->sheet_cell[r][c];
            bool on = row_on && c < v.row[r].cells;

            set_hidden(b, !on);
            if (!on) {
                a->sheet_code[r][c] = -1;
                continue;
            }
            place(b, &sl.cell[r][c]);
            button_text(b, v.row[r].cell[c].text);
            button_style(b, v.row[r].cell[c].selected, v.row[r].cell[c].enabled || v.row[r].cell[c].selected);
            /* A value on show (the distance) looks chosen but is no button.
             * Said every time: button_style skips a role it already has. */
            if (v.row[r].cell[c].enabled) {
                lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
            }
            a->sheet_code[r][c] = v.row[r].cell[c].code;
        }
    }
    set_hidden(a->sheet, false);
}

static void repaint(struct vision_app *a)
{
    struct vision_view_text s;
    enum vision_button order[VISION_BUTTONS];
    int shown_btns;
    int i;

    vision_model_text(&a->model, &s, a->status_buf, sizeof(a->status_buf));
    show(a, s.show_picture);
    set_hidden(a->img, !s.show_picture);
    set_hidden(a->panel, s.title[0] == '\0');
    label_text(a->title, s.title);
    label_text(a->detail, s.detail);
    set_hidden(a->detail, s.detail[0] == '\0');
    label_text(a->status, s.status);
    if (s.status_warn) {
        lv_obj_remove_style(a->status, pos_style(POS_STYLE_TEXT_SECONDARY), 0);
        pos_style_add(a->status, POS_STYLE_STATUS_WARN_TEXT, 0);
    } else {
        lv_obj_remove_style(a->status, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
        pos_style_add(a->status, POS_STYLE_TEXT_SECONDARY, 0);
    }
    label_text(a->count_a, s.count_a);
    label_text(a->count_b, s.count_b);
    button_text(a->btn[VISION_BTN_MODE], s.mode_btn);
    button_text(a->btn[VISION_BTN_LINE], s.line_btn);
    button_text(a->btn[VISION_BTN_TRAILS], s.trails_btn);
    button_text(a->btn[VISION_BTN_HOLD], s.hold_btn);
    button_text(a->btn[VISION_BTN_SETUP], a->model.sheet == VISION_SHEET_SETUP ? "DONE" : "SETUP");
    button_text(a->btn[VISION_BTN_TOL], s.tol_btn);
    button_text(a->btn[VISION_BTN_EDGE], s.edge_btn);
    button_text(a->btn[VISION_BTN_TRACE], s.trace_btn);
    /* The button whose sheet is open is the primary one: it closes it. */
    button_style(a->btn[VISION_BTN_MODE], a->model.sheet == VISION_SHEET_MODES, true);
    button_style(a->btn[VISION_BTN_LINE], false, s.line_enabled);
    button_style(a->btn[VISION_BTN_TRAILS], false, s.line_enabled);
    button_style(a->btn[VISION_BTN_HOLD], s.hold, s.line_enabled);
    button_style(a->btn[VISION_BTN_SETUP], a->model.sheet == VISION_SHEET_SETUP, s.line_enabled);
    button_style(a->btn[VISION_BTN_RESET], false,
                 s.line_enabled && (vision_model_line(&a->model) != VISION_LINE_OFF || s.traffic || s.faces));
    button_style(a->btn[VISION_BTN_SAMPLE], false, s.line_enabled && a->model.live);
    button_style(a->btn[VISION_BTN_TOL], false, s.line_enabled);
    button_style(a->btn[VISION_BTN_EDGE], false, s.line_enabled);
    button_style(a->btn[VISION_BTN_TRACE], false, s.line_enabled);
    /* In COLOR a tap on the picture picks the colour there. */
    if (s.picture_tap) {
        lv_obj_add_flag(a->box, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_clear_flag(a->box, LV_OBJ_FLAG_CLICKABLE);
    }
    set_hidden(a->mark, !(s.show_mark && s.show_picture));
    if (s.show_mark) {
        lv_obj_set_pos(a->mark, s.mark_x - 6, s.mark_y - 6);
        lv_obj_set_size(a->mark, 12, 12);
    }
    /* The mode's buttons, in its order, on the layout's places; the rest
     * hidden. When Vision has stopped, TRY AGAIN is the one button, in
     * the first place. */
    shown_btns = vision_model_buttons(&a->model, order);
    for (i = 0; i < VISION_BUTTONS; i++) {
        set_hidden(a->btn[i], true);
    }
    for (i = 0; i < shown_btns && i < a->lay.buttons; i++) {
        place(a->btn[order[i]], &a->lay.btn[i]);
        set_hidden(a->btn[order[i]], s.show_retry);
    }
    place(a->retry_btn, &a->lay.btn[0]);
    set_hidden(a->retry_btn, !s.show_retry);
    if (s.show_retry) {
        button_text(a->retry_btn, a->model.state == VISION_NO_DEVICE ? "CHECK AGAIN" : "TRY AGAIN");
        button_style(a->retry_btn, true, true);
    }
    draw_trails(a, &s);
    draw_boxes(a, &s);
    draw_lines(a);
    draw_sheet(a);
    if (a->hint_shown != s.hint && (!a->hint_shown || strcmp(a->hint_shown, s.hint) != 0)) {
        pocketos_shell_set_status_hint(s.hint);
    }
    a->hint_shown = s.hint;
}

/* ---- doing what the model asks ---------------------------------------------------------- */

static void do_actions(struct vision_app *a, unsigned acts);
static void layout(struct vision_app *a);

static void start_session(struct vision_app *a)
{
    struct vision_session_config cfg = { 0 };
    char err[VISION_EVENT_TEXT_MAX - 8];

    vision_session_abandon(&a->session, VISION_DESTROY_GRACE_MS);
    if (strcmp(vision_session_backend(), "fake") == 0) {
        cfg.fake = getenv("POCKETOS_CAMERA_FAKE");
        cfg.kpu = getenv("POCKETOS_VISION_KPU_SCRIPT");
    }
    if (vision_session_start(&a->session, &cfg, now_ms(), err, sizeof(err)) != 0) {
        struct vision_event ev;

        LOG_WARN("vision: could not start the helper: %s", err);
        memset(&ev, 0, sizeof(ev));
        ev.kind = VISION_EV_ERROR;
        snprintf(ev.text, sizeof(ev.text), "start %s", err);
        do_actions(a, vision_model_event(&a->model, &ev, &a->session, now_ms()));
    }
}

static void send_line_setting(struct vision_app *a)
{
    int32_t pm[4];

    if (vision_model_line_pm(&a->model, pm)) {
        vision_session_line(&a->session, pm);
    } else {
        vision_session_line(&a->session, NULL);
    }
}

static void send_speed_setting(struct vision_app *a)
{
    int32_t pm[8];

    if (a->model.mode == VISION_MODE_TRAFFIC && vision_model_speed_pm(&a->model, pm)) {
        vision_session_speed_lines(&a->session, pm);
    } else {
        vision_session_speed_lines(&a->session, NULL);
    }
}

static void do_actions(struct vision_app *a, unsigned acts)
{
    int64_t now = now_ms();

    if (acts & VISION_ACT_OPEN) {
        start_session(a);
    }
    if (acts & VISION_ACT_STREAM) {
        vision_session_view(&a->session, a->preview.w, a->preview.h, display_rotation());
        vision_session_stream(&a->session, true, now);
    }
    if (acts & VISION_ACT_MODE) {
        if (vision_session_active(&a->session)) {
            vision_session_mode_word(&a->session, vision_model_mode_word(&a->model));
        }
        /* The mode's own controls: the layout is chosen again. */
        if (a->model.mode != a->lay_mode) {
            pocketui_layout_guard_reset(&a->guard);
            layout(a);
        }
    }
    if (acts & VISION_ACT_LINE) {
        send_line_setting(a);
    }
    if (acts & VISION_ACT_SPEED) {
        send_speed_setting(a);
    }
    if (acts & VISION_ACT_DISTANCE) {
        vision_session_distance(&a->session, vision_model_distance_cm(&a->model));
    }
    if (acts & VISION_ACT_RANGE) {
        vision_session_range(&a->session, vision_model_range_word(&a->model));
    }
    if (acts & VISION_ACT_PIXELS) {
        if (!a->model.have_target) {
            vision_session_color(&a->session, NULL);
        }
        vision_session_tol(&a->session, vision_model_tol(&a->model));
        vision_session_edge(&a->session, vision_model_edge_threshold(&a->model));
        vision_session_trace(&a->session, a->model.set.trace.dark);
    }
    if ((acts & VISION_ACT_SAMPLE) && a->model.sample_x >= 0) {
        vision_session_sample(&a->session, a->model.sample_x, a->model.sample_y);
    }
    if (acts & VISION_ACT_RESET) {
        vision_session_reset(&a->session);
    }
    if ((acts & VISION_ACT_SAVE) && vision_store_save(&a->model.set) != 0) {
        /* Vision carries on with the choice; it is only not kept. */
        LOG_WARN("vision: could not store the settings in %s", vision_store_dir());
    }
    if (acts & VISION_ACT_CLOSE) {
        vision_session_abandon(&a->session, VISION_DESTROY_GRACE_MS);
    }
}

static void on_poll(lv_timer_t *t)
{
    struct vision_app *a = lv_timer_get_user_data(t);
    struct vision_event ev;
    int64_t now = now_ms();
    bool changed = vision_model_tick(&a->model, now);

    while (vision_session_poll(&a->session, &ev, now)) {
        if (ev.kind == VISION_EV_FRAME) {
            if (a->model.state != VISION_LIVE ||
                vision_session_take_frame(&a->session, a->preview.px, a->preview.w,
                                          a->preview.h) != 1) {
                continue;
            }
            lv_image_cache_drop(&a->preview.dsc);
            if (a->shown) {
                lv_obj_invalidate(a->img);
            }
            if (a->model.live && !a->model.stalled) {
                vision_model_event(&a->model, &ev, &a->session, now);
                continue; /* the picture changed, nothing else did */
            }
        } else if (ev.kind == VISION_EV_EXITED && ev.reason != VISION_EXIT_NORMAL) {
            LOG_WARN("vision: helper ended (%d), reason %d", ev.value, ev.reason);
        }
        {
            unsigned acts = vision_model_event(&a->model, &ev, &a->session, now);

            /* A preview of another shape than the one laid out for: the
             * picture box takes the camera's shape before the stream starts. */
            if (ev.kind == VISION_EV_READY &&
                (a->model.preview_w != a->lay_frame_w || a->model.preview_h != a->lay_frame_h)) {
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

static void act(struct vision_app *a, unsigned acts)
{
    do_actions(a, acts);
    repaint(a);
}

static void on_mode(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_mode_button(&a->model));
}

static void on_trails(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_trails_next(&a->model));
}

static void on_hold(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_hold_next(&a->model));
}

static void on_setup(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_setup_button(&a->model));
}

static void on_sheet_cell(lv_event_t *e)
{
    struct vision_cell_ref *ref = lv_event_get_user_data(e);
    struct vision_app *a = ref->a;
    int code = a->sheet_code[ref->row][ref->cell];

    if (code >= 0) {
        act(a, vision_model_sheet_tap(&a->model, code));
    }
}

static void on_line(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_line_next(&a->model));
}

static void on_reset(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_reset(&a->model));
}

static void on_sample(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_sample_middle(&a->model, a->preview.w, a->preview.h));
}

static void on_tol(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_tol_next(&a->model));
}

static void on_edge(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_edge_next(&a->model));
}

static void on_trace(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_trace_next(&a->model));
}

/* A tap on the picture in COLOR: the colour under the finger. The point
 * comes from the input device, in screen pixels; the box's own corner
 * makes it a picture pixel. */
static void on_picture(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;
    lv_area_t box;

    if (!indev) {
        return;
    }
    lv_indev_get_point(indev, &p);
    lv_obj_get_coords(a->box, &box);
    act(a, vision_model_sample_at(&a->model, p.x - box.x1, p.y - box.y1));
}

static void on_retry(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_open(&a->model));
}

/* ---- layout ---------------------------------------------------------------------- */

static void layout(struct vision_app *a)
{
    struct pos_insets in;
    const lv_area_t *area;
    uint32_t fw = a->model.preview_w ? a->model.preview_w : 640;
    uint32_t fh = a->model.preview_h ? a->model.preview_h : 360;
    enum vision_button order[VISION_BUTTONS];
    uint32_t pw;
    uint32_t ph;

    if (!pocketui_layout_begin(&a->guard, a->frame, &in)) {
        return;
    }
    area = &a->guard.area;
    a->lay_frame_w = a->model.preview_w;
    a->lay_frame_h = a->model.preview_h;
    a->lay_mode = a->model.mode;
    /* The picture as the owner holds the unit: the preview turned upright. */
    if (portrait_now()) {
        pw = fh;
        ph = fw;
    } else {
        pw = fw;
        ph = fh;
    }
    if (vision_layout_compute(&a->lay, lv_area_get_width(area), lv_area_get_height(area), in.left,
                              in.top, in.right, in.bottom, pw, ph, vision_model_buttons(&a->model, order),
                              vision_model_status_lines(&a->model)) != 0) {
        LOG_WARN("vision: body %dx%d is too small for the vision screen",
                 (int)lv_area_get_width(area), (int)lv_area_get_height(area));
        return;
    }
    a->laid_out = true;
    place(a->box, &a->lay.picture);
    place(a->status, &a->lay.status);
    place(a->count_a, &a->lay.count_a);
    place(a->count_b, &a->lay.count_b);
    lv_obj_set_width(a->detail, a->lay.picture.w - 2 * POCKETUI_PAD);
    if ((uint32_t)a->lay.picture.w != a->preview.w || (uint32_t)a->lay.picture.h != a->preview.h) {
        uint32_t w = (uint32_t)a->lay.picture.w;
        uint32_t h = (uint32_t)a->lay.picture.h;

        show(a, false);
        if (!picture_alloc(&a->preview, w, h)) {
            LOG_ERROR("vision: no memory for a %ux%u picture", w, h);
            picture_free(&a->preview);
        }
        a->model.live = false;
        if (vision_session_active(&a->session) && a->model.state == VISION_LIVE) {
            vision_session_view(&a->session, w, h, display_rotation());
        }
    }
    repaint(a);
}

static void on_frame_size(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    layout(a);
}

/* ---- building ---------------------------------------------------------------------- */

static lv_obj_t *line_object(lv_obj_t *parent, enum pos_style_role fill)
{
    lv_obj_t *l = lv_obj_create(parent);

    lv_obj_remove_style_all(l);
    pos_style_add(l, fill, 0);
    /* A chip role carries its colour but not its opacity (the chip
     * geometry does); a bare object needs the fill said. */
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 0, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

static void build(struct vision_app *a, lv_obj_t *root)
{
    int i;
    int r;

    a->frame = lv_obj_create(root);
    lv_obj_remove_style_all(a->frame);
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(a->frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    a->box = lv_obj_create(a->frame);
    lv_obj_remove_style_all(a->box);
    pos_style_add(a->box, POS_STYLE_SLAB, 0);
    lv_obj_clear_flag(a->box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    a->img = lv_image_create(a->box);
    lv_obj_set_pos(a->img, 0, 0);
    lv_obj_add_flag(a->img, LV_OBJ_FLAG_HIDDEN);

    /* The lines and the boxes, over the picture. The counting line is a
     * 2 px object in the accent colour, the speed lines in the TX chip's;
     * a box is an outline (the DS focus ring, 2 px accent) with no fill
     * and a caption on a slab at its top. */
    a->line = line_object(a->box, POS_STYLE_BUTTON_PRIMARY);
    a->sline[0] = line_object(a->box, POS_STYLE_CHIP_TX);
    a->sline[1] = line_object(a->box, POS_STYLE_CHIP_TX);
    a->mark = line_object(a->box, POS_STYLE_BUTTON_PRIMARY);
    /* The picture takes a tap only in COLOR (repaint sets the flag); the
     * image itself never does, so the tap reaches the box. */
    lv_obj_clear_flag(a->img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->box, on_picture, LV_EVENT_CLICKED, a);
    /* The trails' dots, behind the boxes: 4 px squares in the counting
     * line's accent, half seen. */
    for (i = 0; i < VISION_TRAIL_TRACKS; i++) {
        for (r = 0; r < VISION_TRAIL_POINTS; r++) {
            a->dot[i][r] = line_object(a->box, POS_STYLE_BUTTON_PRIMARY);
            lv_obj_set_size(a->dot[i][r], 4, 4);
            lv_obj_set_style_bg_opa(a->dot[i][r], LV_OPA_60, 0);
        }
    }
    for (i = 0; i < VISION_MAX_SHOWN; i++) {
        a->outline[i] = lv_obj_create(a->box);
        lv_obj_remove_style_all(a->outline[i]);
        pos_style_add(a->outline[i], POS_STYLE_SELECTED, 0);
        lv_obj_clear_flag(a->outline[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(a->outline[i], LV_OBJ_FLAG_HIDDEN);
        a->tag[i] = pocketui_label(a->box, "", POS_STYLE_CAPTION);
        pos_style_add(a->tag[i], POS_STYLE_SLAB, 0);
        lv_obj_set_style_pad_hor(a->tag[i], 4, 0);
        lv_obj_set_style_pad_ver(a->tag[i], 2, 0);
        lv_obj_add_flag(a->tag[i], LV_OBJ_FLAG_HIDDEN);
    }

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
    lv_label_set_long_mode(a->status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(a->status, LV_TEXT_ALIGN_CENTER, 0);

    a->count_a = pocketui_label(a->frame, "-", POS_STYLE_VALUE);
    pos_style_add(a->count_a, POS_STYLE_SLAB, 0);
    lv_obj_set_style_text_align(a->count_a, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(a->count_a, 12, 0);
    a->count_b = pocketui_label(a->frame, "-", POS_STYLE_VALUE);
    pos_style_add(a->count_b, POS_STYLE_SLAB, 0);
    lv_obj_set_style_text_align(a->count_b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(a->count_b, 12, 0);

    /* The sheet, over everything on the picture: a slab that takes the taps
     * it covers, a title, and rows of a caption and choices. */
    a->sheet = lv_obj_create(a->box);
    lv_obj_remove_style_all(a->sheet);
    pos_style_add(a->sheet, POS_STYLE_SLAB, 0);
    lv_obj_set_style_bg_opa(a->sheet, LV_OPA_COVER, 0);
    /* Square: the picture it covers is. */
    lv_obj_set_style_radius(a->sheet, 0, 0);
    lv_obj_clear_flag(a->sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(a->sheet, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    a->sheet_title = pocketui_label(a->sheet, "", POS_STYLE_TITLE);
    for (r = 0; r < VISION_SHEET_ROWS; r++) {
        a->sheet_caption[r] = pocketui_label(a->sheet, "", POS_STYLE_CAPTION);
        lv_label_set_long_mode(a->sheet_caption[r], LV_LABEL_LONG_CLIP);
        for (i = 0; i < VISION_SHEET_CELLS; i++) {
            a->sheet_ref[r][i].a = a;
            a->sheet_ref[r][i].row = r;
            a->sheet_ref[r][i].cell = i;
            a->sheet_code[r][i] = -1;
            a->sheet_cell[r][i] = button(a->sheet, "", on_sheet_cell, &a->sheet_ref[r][i]);
            lv_obj_add_flag(a->sheet_cell[r][i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    a->btn[VISION_BTN_MODE] = button(a->frame, "DETECT", on_mode, a);
    a->btn[VISION_BTN_LINE] = button(a->frame, "LINE: ACROSS", on_line, a);
    a->btn[VISION_BTN_TRAILS] = button(a->frame, "TRAILS: ON", on_trails, a);
    a->btn[VISION_BTN_HOLD] = button(a->frame, "HOLD", on_hold, a);
    a->btn[VISION_BTN_SETUP] = button(a->frame, "SETUP", on_setup, a);
    a->btn[VISION_BTN_RESET] = button(a->frame, "RESET", on_reset, a);
    a->btn[VISION_BTN_SAMPLE] = button(a->frame, "SAMPLE", on_sample, a);
    a->btn[VISION_BTN_TOL] = button(a->frame, "TOL: MED", on_tol, a);
    a->btn[VISION_BTN_EDGE] = button(a->frame, "EDGE: SOFT", on_edge, a);
    a->btn[VISION_BTN_TRACE] = button(a->frame, "LINE: DARK", on_trace, a);
    for (i = 0; i < VISION_BUTTONS; i++) {
        lv_obj_add_flag(a->btn[i], LV_OBJ_FLAG_HIDDEN);
    }
    a->retry_btn = button(a->frame, "TRY AGAIN", on_retry, a);
    lv_obj_add_flag(a->retry_btn, LV_OBJ_FLAG_HIDDEN);
}

/* ---- the app ------------------------------------------------------------------------ */

static void *vision_create(lv_obj_t *root)
{
    struct vision_app *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    vision_session_init(&a->session);
    vision_model_init(&a->model);
    {
        /* The owner's choices from the last time; the defaults when there
         * are none, or the file is damaged. */
        struct vision_settings set;

        if (vision_store_load(&set) < 0) {
            LOG_WARN("vision: the stored settings could not be read; using the defaults");
        }
        vision_model_load(&a->model, &set);
        LOG_INFO("vision: opening in %s", vision_mode_name(a->model.set.mode));
    }
    {
        /* For screenshots and the shell suite: open with a sheet showing,
         * as a tap on MODE or SETUP would (tests/vision_shell_test.sh). */
        const char *sheet = getenv("POCKETOS_VISION_SHEET");

        if (sheet && strcmp(sheet, "modes") == 0) {
            vision_model_mode_button(&a->model);
        } else if (sheet && strcmp(sheet, "setup") == 0) {
            vision_model_setup_button(&a->model);
        }
    }
    build(a, root);
    pocketos_shell_set_status_hint("");
    a->hint_shown = "";
    a->timer = lv_timer_create(on_poll, VISION_POLL_MS, a);
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    layout(a);
    /* The camera and the detector start with the screen and never outlive
     * it. */
    do_actions(a, vision_model_open(&a->model));
    repaint(a);
    return a;
}

static void vision_destroy(void *priv)
{
    struct vision_app *a = priv;

    if (!a) {
        return;
    }
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    lv_obj_remove_event_cb_with_user_data(a->box, on_picture, a);
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    /* The helper first: once it is gone nothing writes the shared memory
     * the session is about to unmap. */
    vision_session_abandon(&a->session, VISION_DESTROY_GRACE_MS);
    lv_image_set_src(a->img, NULL);
    picture_free(&a->preview);
    free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_vision);

const struct pocketos_app app_vision = {
    .id = "vision",
    .name = "Vision",
    .icon = LV_SYMBOL_EYE_OPEN,
    .icon_mask = &pos_app_icon_vision,
    .create = vision_create,
    .tick = NULL,
    .destroy = vision_destroy,
    .chrome = POCKETOS_CHROME_NONE,
};
