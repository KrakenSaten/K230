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
 * created per frame. A box's tag stays inside the picture, and says less
 * when there are many (VISION_TAGS_FULL).
 *
 * THE PICTURE COMES FIRST. What a mode counts lies on the picture's top
 * corners as two small chips, there only while there is something to say;
 * SEGMENT's key to its colours lies along its foot. Under the picture (or
 * beside it, held wide) are the status, the mode switcher - a button that
 * says MODE over the mode's name and opens the picker - and the mode's own
 * buttons. SEGMENT's and INSTANCES' view is a selector: ORIGINAL, OVERLAY
 * and MASK side by side, the one in force the primary button.
 *
 * FULLSCREEN (DS §30.8): the app declares NONE; the shell's header carries
 * the back button and the hint (SIMULATED under the fake backend).
 *
 * KEYS (DS §38.10): every control is a button, so the keyboard needs no
 * other way in. The arrows move a focus ring over the buttons on show - the
 * sheet's choices while a sheet is open (not the picker's dimmed modes),
 * else the mode switcher and the mode's buttons - Enter presses
 * the one with the ring, and Esc or Back closes an open sheet. The ring
 * appears with the first key and stays out of a touch-only screen; the moves
 * are vision_keys.c's. What it is for: an external display, where the board
 * has no touch (docs/hardware/HDMI_OUTPUT.md).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"
#include "hold_follow.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pocketvision/vision_labels.h"
#include "pocketvision/vision_seg.h"
#include "pos_input.h"
#include "vision_keys.h"
#include "vision_layout.h"
#include "vision_model.h"
#include "vision_session.h"
#include "vision_store.h"
#include "vision_trails.h"

#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: not in lvgl.h */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VISION_POLL_MS 33
/* destroy() and Try again: time for the helper to close the camera and the
 * detector before it is killed. Camera's helper needs well under 300 ms;
 * Vision's took ~420 ms after SIGTERM on unit B (it unloads the model and
 * returns the KPU's shared pool), and a SIGKILL half way through closing the
 * camera and the KPU is exactly the kind of exit this helper must not get.
 * Bounded like every wait here; it only runs when the user leaves. */
/* The helper's own time to close the camera and give the KPU back
 * (vision_session.h); longer while it is still opening a model. */
#define VISION_DESTROY_GRACE_MS VISION_LEAVE_GRACE_MS
#define VISION_LABEL_MAX 48
/* A tag: its height above its box, and the padding either side of its text. */
#define VISION_TAG_H 22
#define VISION_TAG_PAD 4
/* With more boxes than this a tag says what the box is (and which, by its
 * id) and no longer how sure: a crowded picture is read by its boxes. */
#define VISION_TAGS_FULL 6
/* The chips and the key on the picture: their distance from its edge. */
#define VISION_HUD_MARGIN 8
/* The mode switcher's text from its left edge, and the room its chevron
 * takes at the right. */
#define VISION_MODE_PAD 14
#define VISION_MODE_CHEVRON_W 34
/* The detector files are vision_store.h's (vision_store_detector_path). */
#define VISION_DET_STATS_LOG_MS 10000

struct vision_picture {
    lv_image_dsc_t dsc;
    uint16_t *px;
    uint32_t w;
    uint32_t h;
};

_Static_assert(VISION_SHEET_ROWS == VISION_SHEET_ROWS_MAX && VISION_SHEET_CELLS == VISION_SHEET_CELLS_MAX,
               "the model's sheet and the layout's are the same size");
_Static_assert(VISION_BTN_VIEW_OVERLAY == VISION_BTN_VIEW_ORIGINAL + VISION_SEG_OVERLAY &&
                   VISION_BTN_VIEW_MASK == VISION_BTN_VIEW_ORIGINAL + VISION_SEG_MASK,
               "the view selector's buttons are in the views' order");

struct vision_app;

/* Which sheet cell a button is, for its tap. */
struct vision_cell_ref {
    struct vision_app *a;
    int row;
    int cell;
};

struct vision_app {
    int det_logged;           /* the A/B detector last said in force in the log; -1 none */
    int64_t det_stats_ms;     /* when its numbers were last logged */
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
    lv_obj_t *probe;      /* COLOR: the square the colour is read in, an outline */
    lv_obj_t *swatch;     /* COLOR: the colour read, beside the square */
    /* TRACE: the line followed, as dots along it, made once and moved. */
#define VISION_TRACE_DOTS 24
    lv_obj_t *trace_dot[VISION_TRACE_DOTS];
    lv_obj_t *status;
    lv_obj_t *count_a;    /* "DOWN 3": a chip on the picture's top left corner */
    lv_obj_t *count_b;    /* and its top right */
    /* SEGMENT's key: a strip along the picture's foot, a swatch and a name
     * per class. */
    lv_obj_t *legend;
    lv_obj_t *legend_sw[VISION_LEGEND_MAX];
    lv_obj_t *legend_lb[VISION_LEGEND_MAX];
    lv_obj_t *mode_name;  /* the mode switcher's name line */
    lv_obj_t *btn[VISION_BUTTONS]; /* by role (enum vision_button) */
    lv_obj_t *retry_btn;
    /* The sheet over the picture: made once, filled from the model. */
    lv_obj_t *sheet;
    lv_obj_t *sheet_title;
    lv_obj_t *sheet_body;  /* CLASSES: the page's text */
    lv_obj_t *sheet_caption[VISION_SHEET_ROWS];
    lv_obj_t *sheet_cell[VISION_SHEET_ROWS][VISION_SHEET_CELLS];
    struct vision_cell_ref sheet_ref[VISION_SHEET_ROWS][VISION_SHEET_CELLS];
    int sheet_code[VISION_SHEET_ROWS][VISION_SHEET_CELLS];
    struct vision_sheet_layout sheet_lay; /* where the open sheet's choices are */
    bool sheet_on;

    /* The keyboard's targets (KEYS above): the buttons on show, in their
     * order, with their places as laid out - never measured, so a key in
     * the same tick as a repaint sees what the repaint placed. */
#define VISION_KEY_TARGETS (VISION_SHEET_ROWS * VISION_SHEET_CELLS + VISION_BUTTONS + 1)
#define VISION_KEY_RING_PAD 3 /* px between a button and its focus ring */
    lv_obj_t *key_obj[VISION_KEY_TARGETS];
    struct vision_rect key_rect[VISION_KEY_TARGETS];
    int key_n;
    int key_focus;        /* index into key_obj; -1 before the first key */
    lv_obj_t *key_ringed; /* the button that carries the ring now */

    struct pocketui_layout_guard guard;
    struct vision_layout lay;
    uint32_t lay_frame_w; /* the preview the layout was chosen for */
    uint32_t lay_frame_h;
    enum vision_mode lay_mode;
    int lay_hold;         /* the hold (degrees) the layout was made for */
    int hold_tried;       /* the last hold the poll laid out for; -1 none */
    int view_rot;         /* the rotation the helper was last told; -1 none */
    bool laid_out;
    lv_timer_t *timer;

    struct vision_model model;
    struct vision_session session;
    struct vision_picture preview;
    bool shown;           /* img shows the preview */
    const char *hint_shown;
    char status_buf[384];
};

static int64_t now_ms(void)
{
    return (int64_t)lv_tick_get();
}

/* How the unit is held: the camera is fixed to the unit, so its picture is
 * turned upright by the hold, which is the display's rotation on the panel
 * and the mode's and keyboard's on an external display (app.h). */
static struct pocketos_orientation hold_now(void)
{
    struct pocketos_orientation o;

    pocketos_shell_orientation(&o);
    return o;
}

static int display_rotation(void)
{
    return hold_now().hold_rotation;
}

/* Whether the upright picture is tall: the unit held in portrait. */
static bool portrait_now(void)
{
    return !hold_now().hold_landscape;
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

/* A box's tag: on the box's top edge, above it where there is room, and
 * moved left where it would run off the picture - the picture clips what
 * lies outside it, and a tag cut short says the wrong thing. Empty text
 * hides it. */
static void place_tag(struct vision_app *a, int i, int32_t x, int32_t y, const char *text)
{
    lv_obj_t *tag = a->tag[i];
    int32_t w;

    label_text(tag, text);
    set_hidden(tag, text[0] == '\0');
    if (text[0] == '\0') {
        return;
    }
    w = pocketui_text_width(tag, text) + 2 * VISION_TAG_PAD;
    if (x + w > (int32_t)a->preview.w) {
        x = (int32_t)a->preview.w - w;
    }
    lv_obj_set_pos(tag, x < 0 ? 0 : x, y > VISION_TAG_H ? y - VISION_TAG_H : y);
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
            place_tag(a, i, r->line[i].x, r->line[i].y, text);
        }
    }
}

static void draw_boxes(struct vision_app *a, const struct vision_view_text *s)
{
    int n = 0;
    uint32_t seq;
    const struct vision_shown *t = vision_session_tracks(&a->session, &n, &seq);
    bool traffic = s->traffic;
    bool crowded = n > VISION_TAGS_FULL;
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
            char who[24];
            const char *what = s->faces ? "face" : vision_label(t[i].cls);

            if (s->recog && t[i].id) {
                /* RECOGNIZE: "#3 OWNER 91%" - the score, not the detector's
                 * confidence, is the figure worth showing. */
                vision_model_who_label(&a->model, t[i].id, who, sizeof(who));
                snprintf(text, sizeof(text), "#%u %s", t[i].id, who);
                place_tag(a, i, t[i].x, t[i].y, text);
                continue;
            }

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
                } else if (crowded) {
                    snprintf(text, sizeof(text), "#%u %s%s", t[i].id, what, dir_mark(t[i].dir));
                } else {
                    snprintf(text, sizeof(text), "#%u %s%s %u%%", t[i].id, what,
                             dir_mark(t[i].dir), t[i].conf / 10);
                }
            } else if (t[i].id && s->ids) {
                if (crowded) {
                    snprintf(text, sizeof(text), "#%u %s", t[i].id, what);
                } else {
                    snprintf(text, sizeof(text), "#%u %s %u%%", t[i].id, what, t[i].conf / 10);
                }
            } else if (crowded) {
                snprintf(text, sizeof(text), "%s", what);
            } else {
                /* DETECT: what it is and how sure, no identity. */
                snprintf(text, sizeof(text), "%s %u%%", what, t[i].conf / 10);
            }
            place_tag(a, i, t[i].x, t[i].y, text);
        }
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

/* COLOR: the square read, outlined, and its colour beside it - on the side
 * with room, inside the picture. */
static void draw_probe(struct vision_app *a, const struct vision_view_text *s)
{
    bool on = s->show_probe && s->show_picture && a->preview.w && a->preview.h;
    int32_t side;
    int32_t x;
    int32_t y;

    set_hidden(a->probe, !on);
    set_hidden(a->swatch, !on);
    if (!on) {
        return;
    }
    lv_obj_set_pos(a->probe, s->probe_x - s->probe_half, s->probe_y - s->probe_half);
    lv_obj_set_size(a->probe, 2 * s->probe_half + 1, 2 * s->probe_half + 1);
    side = 2 * s->probe_half + 1 < 32 ? 32 : 2 * s->probe_half + 1;
    x = s->probe_x + s->probe_half + 8;
    if (x + side > (int32_t)a->preview.w) {
        x = s->probe_x - s->probe_half - 8 - side;
    }
    y = s->probe_y - side / 2;
    y = y + side > (int32_t)a->preview.h ? (int32_t)a->preview.h - side : y;
    /* With no room either side (a tiny picture) it stays inside the
     * picture, over the square rather than off the picture. */
    lv_obj_set_pos(a->swatch, x < 0 ? 0 : x, y < 0 ? 0 : y);
    lv_obj_set_size(a->swatch, side, side);
    pos_style_set_sample_fill(a->swatch, s->swatch[0], s->swatch[1], s->swatch[2]);
}

/* TRACE: dots along the line followed, only while it is in the picture. */
static void draw_trace(struct vision_app *a, const struct vision_view_text *s)
{
    bool on = s->show_trace && s->show_picture;
    int i;

    for (i = 0; i < VISION_TRACE_DOTS; i++) {
        set_hidden(a->trace_dot[i], !on);
        if (on) {
            int32_t x = s->trace[0] + ((s->trace[2] - s->trace[0]) * i) / (VISION_TRACE_DOTS - 1);
            int32_t y = s->trace[1] + ((s->trace[3] - s->trace[1]) * i) / (VISION_TRACE_DOTS - 1);

            lv_obj_set_pos(a->trace_dot[i], x - 4, y - 4);
        }
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

/* A counter on the picture: there while the picture is, and only with
 * something to say. */
static void draw_chip(lv_obj_t *chip, const char *text, bool picture)
{
    bool on = picture && text[0] != '\0' && strcmp(text, "-") != 0;

    set_hidden(chip, !on);
    if (on) {
        label_text(chip, text);
    }
}

/* SEGMENT's key: the classes the helper says most of the picture is, each
 * with the colour it is painted in, as many as the picture's width holds. A
 * class this build has no colour for ends the key. */
static void draw_legend(struct vision_app *a, const struct vision_view_text *s)
{
    int32_t room = (int32_t)a->preview.w - 2 * VISION_HUD_MARGIN - 2 * VISION_HUD_MARGIN;
    int shown = 0;
    int i;

    for (i = 0; i < VISION_LEGEND_MAX; i++) {
        bool on = s->show_picture && i < s->legend_n && i == shown;
        uint32_t cls = 0;
        int32_t w = 0;

        if (on) {
            while (cls < VISION_SEG_CLASSES && strcmp(vision_seg_label(cls), s->legend[i].name) != 0) {
                cls++;
            }
            w = 14 + 6 + pocketui_text_width(a->legend_lb[i], s->legend[i].text) + (i ? 12 : 0);
            on = cls < VISION_SEG_CLASSES && w <= room;
        }
        set_hidden(a->legend_sw[i], !on);
        set_hidden(a->legend_lb[i], !on);
        if (!on) {
            continue;
        }
        room -= w;
        shown++;
        if (strcmp(lv_label_get_text(a->legend_lb[i]), s->legend[i].text) != 0) {
            lv_label_set_text(a->legend_lb[i], s->legend[i].text);
        }
        /* The colour is said only when the class changes: the helper speaks
         * with every frame. */
        if ((uintptr_t)lv_obj_get_user_data(a->legend_sw[i]) != cls + 1) {
            uint16_t c = vision_seg_color(cls);

            lv_obj_set_user_data(a->legend_sw[i], (void *)(uintptr_t)(cls + 1));
            pos_style_set_sample_fill(a->legend_sw[i], (uint8_t)(((c >> 11) & 0x1f) * 255 / 31),
                                      (uint8_t)(((c >> 5) & 0x3f) * 255 / 63), (uint8_t)((c & 0x1f) * 255 / 31));
        }
    }
    set_hidden(a->legend, shown == 0);
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
    bool body_on;
    int r;
    int c;

    vision_model_sheet(&a->model, &v);
    for (r = 0; r < v.rows; r++) {
        cells[r] = v.row[r].cells;
    }
    if (v.kind == VISION_SHEET_NONE || v.rows == 0 ||
        vision_layout_sheet(&sl, a->lay.picture.w, a->lay.picture.h, v.rows, cells) != 0) {
        set_hidden(a->sheet, true);
        a->sheet_on = false;
        return;
    }
    a->sheet_lay = sl;
    a->sheet_on = true;
    lv_obj_set_pos(a->sheet, 0, 0);
    lv_obj_set_size(a->sheet, a->lay.picture.w, a->lay.picture.h);
    {
        /* The title's band is the layout's, in pixels; a larger text size
         * takes its extra height from the padding above, never the row
         * under it. */
        struct vision_rect title = sl.title;
        int32_t lh = lv_font_get_line_height(lv_obj_get_style_text_font(a->sheet_title, 0));

        if (lh > title.h) {
            title.y -= lh - title.h < title.y ? lh - title.h : title.y;
            title.h = lh;
        }
        place(a->sheet_title, &title);
    }
    label_text(a->sheet_title, v.title);
    /* Text under the rows only where a line of it has room (the picker's
     * note has none on the wide picture: the dimmed buttons say it there). */
    body_on = v.body[0] != '\0' && sl.body.h >= lv_font_get_line_height(lv_obj_get_style_text_font(a->sheet_body, 0));
    set_hidden(a->sheet_body, !body_on);
    if (body_on) {
        place(a->sheet_body, &sl.body);
        label_text(a->sheet_body, v.body);
    }
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

/* ---- the keyboard (KEYS above) ------------------------------------------------------- */

static void key_target(struct vision_app *a, lv_obj_t *obj, const struct vision_rect *r)
{
    if (a->key_n < VISION_KEY_TARGETS) {
        a->key_obj[a->key_n] = obj;
        a->key_rect[a->key_n] = *r;
        a->key_n++;
    }
}

/* The ring on the focused target, and on nothing else; none before the
 * first key. Kept on the same button when the targets change under it,
 * else on the nearest index that is still there. */
static void key_ring(struct vision_app *a)
{
    lv_obj_t *now = NULL;

    if (a->key_focus >= a->key_n) {
        a->key_focus = a->key_n - 1;
    }
    if (a->key_focus >= 0) {
        now = a->key_obj[a->key_focus];
    }
    if (now == a->key_ringed) {
        return;
    }
    if (a->key_ringed) {
        lv_obj_remove_style(a->key_ringed, pos_style(POS_STYLE_SELECTED), 0);
        lv_obj_remove_local_style_prop(a->key_ringed, LV_STYLE_OUTLINE_PAD, 0);
    }
    if (now) {
        pos_style_add(now, POS_STYLE_SELECTED, 0);
        /* Clear of the button: on a primary (filled) button an outline on its
         * edge is lost against the fill. */
        lv_obj_set_style_outline_pad(now, VISION_KEY_RING_PAD, 0);
    }
    a->key_ringed = now;
}

/* What the keyboard can reach now: an open sheet's choices (the sheet sits
 * over the picture, so its places are the picture's plus the box's corner;
 * a cell with no code, a dimmed mode, is passed by), else the mode switcher
 * and the mode's buttons on show, or TRY AGAIN alone. */
static void key_targets(struct vision_app *a, const enum vision_button *order, int shown, bool retry)
{
    lv_obj_t *was = a->key_focus >= 0 && a->key_focus < a->key_n ? a->key_obj[a->key_focus] : NULL;
    int r;
    int c;
    int i;

    a->key_n = 0;
    if (a->sheet_on) {
        for (r = 0; r < VISION_SHEET_ROWS; r++) {
            for (c = 0; c < VISION_SHEET_CELLS; c++) {
                struct vision_rect cell = a->sheet_lay.cell[r][c];

                if (a->sheet_code[r][c] < 0) {
                    continue;
                }
                cell.x += a->lay.picture.x;
                cell.y += a->lay.picture.y;
                key_target(a, a->sheet_cell[r][c], &cell);
            }
        }
    } else if (retry) {
        key_target(a, a->retry_btn, &a->lay.btn[0]);
    } else {
        /* The mode switcher, then the mode's own buttons. */
        key_target(a, a->btn[VISION_BTN_MODE], &a->lay.mode);
        for (i = 1; i < shown && i - 1 < a->lay.buttons; i++) {
            key_target(a, a->btn[order[i]], &a->lay.btn[i - 1]);
        }
    }
    if (was && a->key_focus >= 0) {
        for (i = 0; i < a->key_n; i++) {
            if (a->key_obj[i] == was) {
                a->key_focus = i;
                break;
            }
        }
        if (i == a->key_n) {
            /* The sheet opened or closed: start again at its first choice. */
            a->key_focus = 0;
        }
    }
    key_ring(a);
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
    draw_chip(a->count_a, s.count_a, s.show_picture);
    draw_chip(a->count_b, s.count_b, s.show_picture);
    button_text(a->btn[VISION_BTN_MODE], s.mode_btn);
    button_text(a->btn[VISION_BTN_LINE], s.line_btn);
    button_text(a->btn[VISION_BTN_TRAILS], s.trails_btn);
    button_text(a->btn[VISION_BTN_HOLD], s.hold_btn);
    button_text(a->btn[VISION_BTN_SETUP], a->model.sheet == VISION_SHEET_SETUP ? "DONE" : "SETUP");
    button_text(a->btn[VISION_BTN_TOL], s.tol_btn);
    button_text(a->btn[VISION_BTN_EDGE], s.edge_btn);
    button_text(a->btn[VISION_BTN_TRACE], s.trace_btn);
    button_text(a->btn[VISION_BTN_ENROL], s.enrol_btn);
    button_text(a->btn[VISION_BTN_FORGET], s.forget_btn);
    button_text(a->btn[VISION_BTN_MODEL], s.model_btn);
    button_text(a->btn[VISION_BTN_SAMPLE], s.match_btn);
    button_style(a->btn[VISION_BTN_CLASSES], a->model.sheet == VISION_SHEET_CLASSES, s.line_enabled);
    button_style(a->btn[VISION_BTN_MODEL], false, s.model_enabled);
    button_style(a->btn[VISION_BTN_ENROL], a->model.enrolling, s.line_enabled);
    button_style(a->btn[VISION_BTN_FORGET], a->model.forget_armed, s.line_enabled && s.forget_enabled);
    /* The button whose sheet is open is the primary one: it closes it. */
    button_style(a->btn[VISION_BTN_MODE], a->model.sheet == VISION_SHEET_MODES, true);
    button_style(a->btn[VISION_BTN_LINE], false, s.line_enabled);
    button_style(a->btn[VISION_BTN_TRAILS], false, s.line_enabled);
    button_style(a->btn[VISION_BTN_HOLD], s.hold, s.line_enabled);
    button_style(a->btn[VISION_BTN_SETUP], a->model.sheet == VISION_SHEET_SETUP, s.line_enabled);
    button_style(a->btn[VISION_BTN_RESET], false,
                 s.line_enabled && (vision_model_line(&a->model) != VISION_LINE_OFF || s.traffic || s.faces));
    button_style(a->btn[VISION_BTN_SAMPLE], a->model.have_target, s.line_enabled && s.match_enabled);
    button_style(a->btn[VISION_BTN_TOL], false, s.line_enabled);
    button_style(a->btn[VISION_BTN_EDGE], false, s.line_enabled);
    for (i = 0; i < VISION_SEG_VIEWS; i++) {
        button_style(a->btn[VISION_BTN_VIEW_ORIGINAL + i], s.seg_view == (enum vision_seg_view)i, s.line_enabled);
    }
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
    /* The mode switcher in its place and the mode's own buttons, in its
     * order, on the layout's; the rest hidden. When Vision has stopped,
     * TRY AGAIN is the one button, in the first place. */
    shown_btns = vision_model_buttons(&a->model, order);
    for (i = 0; i < VISION_BUTTONS; i++) {
        set_hidden(a->btn[i], true);
    }
    place(a->btn[VISION_BTN_MODE], &a->lay.mode);
    set_hidden(a->btn[VISION_BTN_MODE], s.show_retry);
    for (i = 1; i < shown_btns && i - 1 < a->lay.buttons; i++) {
        place(a->btn[order[i]], &a->lay.btn[i - 1]);
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
    draw_probe(a, &s);
    draw_trace(a, &s);
    draw_legend(a, &s);
    draw_sheet(a);
    key_targets(a, order, shown_btns, s.show_retry);
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
    int d = vision_model_detector_to_load(&a->model);
    int64_t t0 = now_ms();
    enum vision_leave left = vision_session_abandon(&a->session, VISION_DESTROY_GRACE_MS);

    if (d >= 0) {
        /* The old helper, and the detector it held, are gone before the
         * next one opens: only one detector is ever loaded. */
        cfg.model = vision_store_detector_path((enum vision_detector)d);
        LOG_INFO("vision: detector %s: starting the helper on %s (the last one left: %d, %lld ms)",
                 vision_detector_name((enum vision_detector)d), cfg.model, (int)left, (long long)(now_ms() - t0));
        a->det_logged = -1;
        a->det_stats_ms = 0;
    }
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
        a->view_rot = display_rotation();
        vision_session_view(&a->session, a->preview.w, a->preview.h, a->view_rot);
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
        /* A new helper (TRY AGAIN, MODEL) has no target of its own: the one
         * on show is sent again, or none. */
        vision_session_color(&a->session, a->model.have_target ? a->model.target : NULL);
        vision_session_tol(&a->session, vision_model_tol(&a->model));
        vision_session_edge(&a->session, vision_model_edge_threshold(&a->model));
        vision_session_seg_view(&a->session, vision_seg_view_word(a->model.set.seg.view));
        vision_session_trace(&a->session, a->model.set.trace.dark);
        vision_session_probe(&a->session, a->model.probe_x, a->model.probe_y);
    } else if (acts & VISION_ACT_PROBE) {
        vision_session_probe(&a->session, a->model.probe_x, a->model.probe_y);
    }
    if (acts & VISION_ACT_TARGET) {
        vision_session_color(&a->session, a->model.have_target ? a->model.target : NULL);
    }
    if ((acts & VISION_ACT_SAMPLE) && a->model.sample_x >= 0) {
        vision_session_sample(&a->session, a->model.sample_x, a->model.sample_y);
    }
    if (acts & VISION_ACT_RESET) {
        vision_session_reset(&a->session);
    }
    if (acts & VISION_ACT_ENROL) {
        vision_session_enrol(&a->session, a->model.enrolling);
    }
    if (acts & VISION_ACT_FORGET) {
        vision_session_forget(&a->session);
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

    /* The unit turned while the screen is open: on an external display the
     * shell does not restart for it (app.h hold_rotation), so the picture
     * takes the new shape and the helper the new turn here. */
    if (a->laid_out) {
        int hold = display_rotation();

        /* Tried once per new hold (hold_follow.h); the size change that gives
         * a frame with no area its area lays it out with the hold as it is. */
        if (hold_follow(hold, a->lay_hold, &a->hold_tried)) {
            LOG_INFO("vision: the unit is now held at %d, was %d", hold, a->lay_hold);
            pocketui_layout_guard_reset(&a->guard);
            layout(a);
        }
    }
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
            char det_fail_was[sizeof(a->model.det_fail)];
            unsigned acts;

            memcpy(det_fail_was, a->model.det_fail, sizeof(det_fail_was));
            acts = vision_model_event(&a->model, &ev, &a->session, now);
            if (a->model.det_fail[0] && strcmp(a->model.det_fail, det_fail_was) != 0) {
                LOG_WARN("vision: detector: %s", a->model.det_fail);
            }
            if (a->model.det_active >= 0 && a->model.det_active != a->det_logged) {
                a->det_logged = a->model.det_active;
                LOG_INFO("vision: detector %s in force: %s, %u classes",
                         vision_detector_name((enum vision_detector)a->model.det_active), a->model.model_name,
                         a->model.classes);
            }
            if (ev.kind == VISION_EV_STATS && a->model.det_active >= 0 &&
                (a->det_stats_ms == 0 || now - a->det_stats_ms >= VISION_DET_STATS_LOG_MS)) {
                a->det_stats_ms = now;
                /* The numbers are the mode's: KPU 0 ms in a mode without the detector. */
                LOG_INFO("vision: %s, detector %s loaded: %u.%u fps, KPU %d ms, pre %d ms, post %d ms, CPU %d%%, "
                         "RSS %ld KB, bad %u",
                         vision_mode_name(a->model.mode), vision_detector_name((enum vision_detector)a->model.det_active),
                         a->model.stats.fps_x10 / 10, a->model.stats.fps_x10 % 10, a->model.stats.infer_ms,
                         a->model.stats.pre_ms, a->model.stats.post_ms, a->model.stats.cpu_pct,
                         a->model.stats.rss_kb, a->model.stats.bad);
            }

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

static void on_enrol(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_enrol_button(&a->model));
}

static void on_forget(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_forget_button(&a->model));
}

static void on_model(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);
    int was = a->model.det_active;
    unsigned acts = vision_model_model_button(&a->model, vision_store_detector_files());

    if (acts) {
        LOG_INFO("vision: detector %s asked for (was %s)",
                 vision_detector_name((enum vision_detector)a->model.det_loading),
                 vision_detector_name((enum vision_detector)was));
    } else if (a->model.det_fail[0]) {
        LOG_WARN("vision: detector: %s", a->model.det_fail);
    }
    act(a, acts);
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

    act(a, vision_model_match_button(&a->model));
}

static void on_classes(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_classes_button(&a->model));
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

/* The view selector: the part tapped is the view. */
static void on_view(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);
    lv_obj_t *part = lv_event_get_current_target(e);
    int i;

    for (i = 0; i < VISION_SEG_VIEWS; i++) {
        if (a->btn[VISION_BTN_VIEW_ORIGINAL + i] == part) {
            act(a, vision_model_set_view(&a->model, (enum vision_seg_view)i));
            return;
        }
    }
}

static void on_trace(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_trace_next(&a->model));
}

/* A tap on the picture in COLOR: the square moves under the finger. The
 * point comes from the input device, in screen pixels; the box's own corner
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
    act(a, vision_model_probe_at(&a->model, p.x - box.x1, p.y - box.y1));
}

static void on_retry(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);

    act(a, vision_model_open(&a->model));
}

/* The keyboard (KEYS above). The first key only shows the ring; after that
 * the arrows move it and Enter presses what it is on, exactly as a tap
 * would - a button that takes no tap takes no Enter either. */
static void on_key(lv_event_t *e)
{
    struct vision_app *a = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    enum vision_key_move m = VISION_KEY_NONE;
    lv_obj_t *target;

    switch (key) {
    case LV_KEY_LEFT: m = VISION_KEY_LEFT; break;
    case LV_KEY_RIGHT: m = VISION_KEY_RIGHT; break;
    case LV_KEY_UP: m = VISION_KEY_UP; break;
    case LV_KEY_DOWN: m = VISION_KEY_DOWN; break;
    case LV_KEY_NEXT: m = VISION_KEY_NEXT; break;
    case LV_KEY_PREV: m = VISION_KEY_PREV; break;
    case LV_KEY_ESC:
        if (a->model.sheet != VISION_SHEET_NONE) {
            act(a, vision_model_sheet_close(&a->model));
        }
        return;
    case LV_KEY_ENTER:
        break;
    default:
        return;
    }
    if (a->key_focus < 0 || a->key_focus >= a->key_n) {
        a->key_focus = a->key_n > 0 ? 0 : -1;
        key_ring(a);
        return;
    }
    if (key != LV_KEY_ENTER) {
        a->key_focus = vision_keys_move(a->key_rect, a->key_n, a->key_focus, m);
        key_ring(a);
        return;
    }
    target = a->key_obj[a->key_focus];
    if (lv_obj_has_flag(target, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_has_flag(target, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_send_event(target, LV_EVENT_CLICKED, NULL);
    }
}

/* ---- layout ---------------------------------------------------------------------- */

/* How wide `text` is in `label`'s own font and letter spacing. */
static int32_t text_w(lv_obj_t *label, const char *text)
{
    lv_point_t size;

    lv_text_get_size(&size, text, lv_obj_get_style_text_font(label, 0),
                     lv_obj_get_style_text_letter_space(label, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* Whether the mode's name and a selector's parts are readable where the
 * layout put them. The layout's widths are for the default type; with a
 * larger text size (Settings) they may not be. */
static bool labels_fit(struct vision_app *a, const enum vision_button order[], int own)
{
    struct vision_view_text s;
    int i;

    vision_model_text(&a->model, &s, a->status_buf, sizeof(a->status_buf));
    if (s.mode_btn && text_w(a->mode_name, s.mode_btn) > a->lay.mode.w - VISION_MODE_PAD - VISION_MODE_CHEVRON_W) {
        return false;
    }
    for (i = 1; i <= own && i - 1 < a->lay.buttons; i++) {
        lv_obj_t *b = a->btn[order[i]];
        lv_obj_t *lb = lv_obj_get_child(b, 0);

        if (text_w(lb, lv_label_get_text(lb)) >
            a->lay.btn[i - 1].w - lv_obj_get_style_pad_left(b, 0) - lv_obj_get_style_pad_right(b, 0)) {
            return false;
        }
    }
    return true;
}

static void layout(struct vision_app *a)
{
    struct pos_insets in;
    const lv_area_t *area;
    uint32_t fw = a->model.preview_w ? a->model.preview_w : 640;
    uint32_t fh = a->model.preview_h ? a->model.preview_h : 360;
    enum vision_button order[VISION_BUTTONS];
    int own;
    bool one_row;
    uint32_t pw;
    uint32_t ph;

    if (!pocketui_layout_begin(&a->guard, a->frame, &in)) {
        return;
    }
    area = &a->guard.area;
    a->lay_frame_w = a->model.preview_w;
    a->lay_frame_h = a->model.preview_h;
    a->lay_mode = a->model.mode;
    a->lay_hold = display_rotation();
    /* The picture as the owner holds the unit: the preview turned upright. */
    if (portrait_now()) {
        pw = fh;
        ph = fw;
    } else {
        pw = fw;
        ph = fh;
    }
    /* The mode's own buttons: all but the mode switcher (TRY AGAIN's place
     * is the first of them, so there is always one). */
    own = vision_model_buttons(&a->model, order) - 1;
    one_row = vision_model_buttons_one_row(&a->model);
    if (vision_layout_compute(&a->lay, lv_area_get_width(area), lv_area_get_height(area), in.left,
                              in.top, in.right, in.bottom, pw, ph, own < 1 ? 1 : own, one_row,
                              vision_model_status_lines(&a->model)) != 0) {
        LOG_WARN("vision: body %dx%d is too small for the vision screen",
                 (int)lv_area_get_width(area), (int)lv_area_get_height(area));
        return;
    }
    /* A selector whose parts or mode name do not fit beside the switcher
     * (large text) is laid out as buttons of their own: the switcher gets
     * its own row upright, and the parts the full width under it. */
    if (one_row && !labels_fit(a, order, own)) {
        struct vision_layout alt;

        if (vision_layout_compute(&alt, lv_area_get_width(area), lv_area_get_height(area), in.left, in.top,
                                  in.right, in.bottom, pw, ph, own < 1 ? 1 : own, false,
                                  vision_model_status_lines(&a->model)) == 0) {
            a->lay = alt;
        }
    }
    a->laid_out = true;
    place(a->box, &a->lay.picture);
    place(a->status, &a->lay.status);
    /* The switcher's name stops short of its chevron, however wide it is. */
    lv_obj_set_width(a->mode_name, a->lay.mode.w - VISION_MODE_PAD - VISION_MODE_CHEVRON_W);
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
            vision_session_view(&a->session, w, h, a->lay_hold);
            a->view_rot = a->lay_hold;
        }
        /* COLOR's square was in the old picture's pixels: back to the
         * centre of the new one. */
        do_actions(a, vision_model_probe_centre(&a->model));
    } else if (a->view_rot >= 0 && a->view_rot != a->lay_hold && vision_session_active(&a->session) &&
               a->model.state == VISION_LIVE) {
        /* The same size, turned the other way: the helper turns the frame
         * the detector sees, so it is told even when nothing is resized. */
        vision_session_view(&a->session, a->preview.w, a->preview.h, a->lay_hold);
        a->view_rot = a->lay_hold;
        /* The square was a point of the picture as it was turned. */
        do_actions(a, vision_model_probe_centre(&a->model));
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

/* A counter's chip on the picture: a value on a slab, in a top corner. */
static lv_obj_t *hud_chip(lv_obj_t *parent, lv_align_t corner, int32_t dx)
{
    lv_obj_t *c = pocketui_label(parent, "-", POS_STYLE_VALUE);

    pos_style_add(c, POS_STYLE_SLAB, 0);
    lv_obj_add_style(c, pos_style_fixed_size(POS_STYLE_VALUE), 0);
    lv_obj_set_style_pad_hor(c, 10, 0);
    lv_obj_set_style_pad_ver(c, 6, 0);
    lv_obj_align(c, corner, dx, VISION_HUD_MARGIN);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    return c;
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
    /* COLOR's square: an outline as the boxes have; its swatch a fill of the
     * colour read inside the same accent ring, so a dark, a white or the
     * picture's own colour still stands out. */
    a->probe = lv_obj_create(a->box);
    lv_obj_remove_style_all(a->probe);
    pos_style_add(a->probe, POS_STYLE_SELECTED, 0);
    lv_obj_clear_flag(a->probe, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->probe, LV_OBJ_FLAG_HIDDEN);
    a->swatch = lv_obj_create(a->box);
    lv_obj_remove_style_all(a->swatch);
    pos_style_add(a->swatch, POS_STYLE_SELECTED, 0);
    lv_obj_set_style_bg_opa(a->swatch, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_pad(a->swatch, 2, 0);
    lv_obj_clear_flag(a->swatch, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->swatch, LV_OBJ_FLAG_HIDDEN);
    /* TRACE's line: 8 px squares in the counting line's accent. */
    for (i = 0; i < VISION_TRACE_DOTS; i++) {
        a->trace_dot[i] = line_object(a->box, POS_STYLE_BUTTON_PRIMARY);
        lv_obj_set_size(a->trace_dot[i], 8, 8);
    }
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
        /* Compact at every text size: a tag lies on the picture (DS §46.4). */
        lv_obj_add_style(a->tag[i], pos_style_fixed_size(POS_STYLE_CAPTION), 0);
        lv_obj_set_style_pad_hor(a->tag[i], VISION_TAG_PAD, 0);
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

    /* The counters: chips on the picture's top corners, over the boxes and
     * under the sheet. As the tags, compact at every text size. */
    a->count_a = hud_chip(a->box, LV_ALIGN_TOP_LEFT, VISION_HUD_MARGIN);
    a->count_b = hud_chip(a->box, LV_ALIGN_TOP_RIGHT, -VISION_HUD_MARGIN);

    /* SEGMENT's key along the picture's foot: one strip, a swatch and a
     * name per class. */
    a->legend = lv_obj_create(a->box);
    lv_obj_remove_style_all(a->legend);
    pos_style_add(a->legend, POS_STYLE_SLAB, 0);
    lv_obj_set_style_bg_opa(a->legend, LV_OPA_COVER, 0);
    lv_obj_set_size(a->legend, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(a->legend, VISION_HUD_MARGIN, 0);
    lv_obj_set_style_pad_ver(a->legend, 6, 0);
    lv_obj_set_style_pad_column(a->legend, 6, 0);
    lv_obj_set_flex_flow(a->legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(a->legend, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(a->legend, LV_ALIGN_BOTTOM_LEFT, VISION_HUD_MARGIN, -VISION_HUD_MARGIN);
    lv_obj_clear_flag(a->legend, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->legend, LV_OBJ_FLAG_HIDDEN);
    for (i = 0; i < VISION_LEGEND_MAX; i++) {
        a->legend_sw[i] = lv_obj_create(a->legend);
        lv_obj_remove_style_all(a->legend_sw[i]);
        lv_obj_set_size(a->legend_sw[i], 14, 14);
        lv_obj_set_style_bg_opa(a->legend_sw[i], LV_OPA_COVER, 0);
        /* Clear of the name before it. */
        lv_obj_set_style_margin_left(a->legend_sw[i], i ? 6 : 0, 0);
        lv_obj_clear_flag(a->legend_sw[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        a->legend_lb[i] = pocketui_label(a->legend, "", POS_STYLE_CAPTION);
        lv_obj_add_style(a->legend_lb[i], pos_style_fixed_size(POS_STYLE_CAPTION), 0);
    }

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
    a->sheet_body = pocketui_label(a->sheet, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(a->sheet_body, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(a->sheet_body, LV_OBJ_FLAG_HIDDEN);
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

    /* The mode switcher: MODE, small, over the mode's name, and a chevron -
     * a button that says what it changes, apart from the mode's own. The
     * small text takes the button's colour, so it reads on the accent fill
     * the open picker gives it. */
    a->btn[VISION_BTN_MODE] = button(a->frame, "DETECT", on_mode, a);
    a->mode_name = lv_obj_get_child(a->btn[VISION_BTN_MODE], 0);
    lv_obj_align(a->mode_name, LV_ALIGN_LEFT_MID, VISION_MODE_PAD, 10);
    {
        lv_obj_t *cap = lv_label_create(a->btn[VISION_BTN_MODE]);
        lv_obj_t *chev = lv_label_create(a->btn[VISION_BTN_MODE]);

        lv_label_set_text(cap, "MODE");
        lv_obj_add_style(cap, pos_style_fixed_size(POS_STYLE_CAPTION), 0);
        lv_obj_set_style_text_opa(cap, LV_OPA_70, 0);
        lv_obj_align(cap, LV_ALIGN_TOP_LEFT, VISION_MODE_PAD, 9);
        lv_label_set_text(chev, LV_SYMBOL_DOWN);
        pos_style_add(chev, POS_STYLE_SYMBOL, 0);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -VISION_MODE_PAD, 0);
    }
    a->btn[VISION_BTN_LINE] = button(a->frame, "LINE: ACROSS", on_line, a);
    a->btn[VISION_BTN_TRAILS] = button(a->frame, "TRAILS: ON", on_trails, a);
    a->btn[VISION_BTN_HOLD] = button(a->frame, "HOLD", on_hold, a);
    a->btn[VISION_BTN_SETUP] = button(a->frame, "SETUP", on_setup, a);
    a->btn[VISION_BTN_RESET] = button(a->frame, "RESET", on_reset, a);
    a->btn[VISION_BTN_SAMPLE] = button(a->frame, "MATCH", on_sample, a);
    a->btn[VISION_BTN_CLASSES] = button(a->frame, "CLASSES", on_classes, a);
    a->btn[VISION_BTN_TOL] = button(a->frame, "TOL: MED", on_tol, a);
    a->btn[VISION_BTN_EDGE] = button(a->frame, "EDGE: SOFT", on_edge, a);
    a->btn[VISION_BTN_VIEW_ORIGINAL] = button(a->frame, "ORIGINAL", on_view, a);
    a->btn[VISION_BTN_VIEW_OVERLAY] = button(a->frame, "OVERLAY", on_view, a);
    a->btn[VISION_BTN_VIEW_MASK] = button(a->frame, "MASK", on_view, a);
    a->btn[VISION_BTN_TRACE] = button(a->frame, "LINE: DARK", on_trace, a);
    a->btn[VISION_BTN_ENROL] = button(a->frame, "ENROL", on_enrol, a);
    a->btn[VISION_BTN_FORGET] = button(a->frame, "FORGET", on_forget, a);
    a->btn[VISION_BTN_MODEL] = button(a->frame, "GENERAL 80", on_model, a);
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
        /* The detector files: R0 on every 0.3.6 card, GENERAL 80 by hand. */
        uint32_t present = vision_store_detector_files();

        a->det_logged = -1;
        vision_model_set_detector_files(&a->model, present);
        if (present) {
            LOG_INFO("vision: detector files: R0 %s, GENERAL 80 (upstream) %s; last confirmed %s",
                     (present & (1u << VISION_DET_R0)) ? "present" : "missing",
                     (present & (1u << VISION_DET_UPSTREAM)) ? "present" : "missing",
                     vision_detector_name(a->model.set.detector));
        }
    }
    {
        /* For screenshots and the shell suite: open with a sheet showing,
         * as a tap on MODE or SETUP would (tests/vision_shell_test.sh). */
        const char *sheet = getenv("POCKETOS_VISION_SHEET");

        if (sheet && strcmp(sheet, "modes") == 0) {
            vision_model_mode_button(&a->model);
        } else if (sheet && strcmp(sheet, "setup") == 0) {
            vision_model_setup_button(&a->model);
        } else if (sheet && strcmp(sheet, "classes") == 0) {
            vision_model_classes_button(&a->model);
        }
    }
    a->key_focus = -1;
    a->view_rot = -1;
    a->hold_tried = -1;
    build(a, root);
    pocketos_shell_set_status_hint("");
    a->hint_shown = "";
    a->timer = lv_timer_create(on_poll, VISION_POLL_MS, a);
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    /* The screen takes the keys (KEYS above), as 2048's and DeskBuddy's do. */
    lv_obj_add_event_cb(a->frame, on_key, LV_EVENT_KEY, a);
    pos_input_add_obj(a->frame);
    pos_input_focus(a->frame);
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
    lv_obj_remove_event_cb_with_user_data(a->frame, on_key, a);
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

/* Back (the BOOT key, shell.action back): an open sheet closes, as its own
 * button would close it; at the top Back is the shell's (app.h). */
static int vision_back(void *priv)
{
    struct vision_app *a = priv;

    if (!a || a->model.sheet == VISION_SHEET_NONE) {
        return 0;
    }
    act(a, vision_model_sheet_close(&a->model));
    return 1;
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
    .back = vision_back,
};
