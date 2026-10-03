/*
 * SYSTEM. See rift_system.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_system.h"

#include "pos_styles.h"
#include "rift_device.h"
#include "rift_manage.h"
#include "rift_session.h"
#include "rift_sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A sound's ON/OFF, as Settings draws a switch: a 56 px action beside its
 * title, primary while on. */
#define SOUND_TOGGLE_W 112

struct sound_switch {
    lv_obj_t *toggle;
    lv_obj_t *label;
    int drawn; /* the state it was last drawn in; -1 not yet */
};

struct rift_system_view {
    struct rift_app *app;
    lv_obj_t *root;
    lv_obj_t *split;
    lv_obj_t *col[2];

    lv_obj_t *id_glyph;
    lv_obj_t *id_name;
    lv_obj_t *id_hash;
    lv_obj_t *id_key;
    lv_obj_t *advert_near;
    lv_obj_t *advert_mesh;
    lv_obj_t *advert_line;
    int advert_warn;

    struct sound_switch dm;
    lv_obj_t *sound_note;
};

static struct rift_system_view *of(const struct rift_app *app)
{
    return app ? app->system : NULL;
}

static lv_obj_t *dense(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *r = lv_obj_create(parent);

    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, RIFT_ROW_H);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

static lv_obj_t *wrapping(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_remove_style_all(label);
    pos_style_add(label, role, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, "");
    return label;
}

/* As ACTIVITY's columns: room for the first panel's caption, which rises
 * above the panel (rift_panel). */
static lv_obj_t *column(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);

    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 20, 0);
    lv_obj_set_style_pad_top(c, rift_caption_overhang(), 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* ---- DEVICE ----------------------------------------------------------------- */

/* An advert, on a reader's press, and only then: the one handler for both
 * buttons, and the only caller of the advert in this app. Zero-hop is heard
 * in direct range and repeated by nobody; the mesh one is flooded. */
static void on_advert(lv_event_t *e)
{
    struct rift_system_view *v = lv_event_get_user_data(e);
    int zero_hop = lv_event_get_target_obj(e) == v->advert_near;

    rift_ipc_send_advert(&v->app->ipc, zero_hop);
    rift_app_refresh(v->app);
}

static void build_device(struct rift_system_view *v, lv_obj_t *parent)
{
    lv_obj_t *panel = rift_panel(parent, "DEVICE");
    lv_obj_t *path = rift_panel(parent, "ADDRESSING");
    lv_obj_t *line = dense(panel, 10);
    lv_obj_t *bar;

    v->id_glyph = rift_glyph_create(line);
    rift_glyph_set(v->id_glyph, RIFT_GLYPH_SELF);
    v->id_name = rift_cell(line, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(v->id_name, 1);
    v->id_hash = rift_cell(line, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_RIGHT);
    v->id_key = pocketui_kv_row(panel, "Key", RIFT_UNKNOWN);

    /* Telling the mesh this node is here. Nothing in RIFT does it on its
     * own - there is no periodic advert, by decision (docs/services/
     * MESHCORED.md) - so a peer that has lost this node's key, or one that
     * has never heard it, waits for a reader to press one of these. */
    bar = dense(panel, 12);
    lv_obj_set_height(bar, RIFT_TOUCH_H);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    v->advert_near = rift_action(bar, "ADVERT NEAR", 0, 0, on_advert, v);
    v->advert_mesh = rift_action(bar, "ADVERT MESH", 0, 0, on_advert, v);
    v->advert_line = wrapping(panel, POS_STYLE_CAPTION);
    /* The name in DEVICE, the path hash size in ADDRESSING. */
    rift_device_build(v->app, panel, path);
}

/* The ADVERT buttons: pressable only when the service is answering, its
 * radio can send, and no advert is already on its way. The line under them
 * says what became of the last one, or - before any - what the two do. */
static void refresh_advert(struct rift_system_view *v)
{
    const struct rift_model *m = &v->app->model;
    /* The model's word for it, as the composer's (rift_thread_refusal): the
     * service said its radio can send. A press while the socket is between
     * connections is refused by the client, with a reason, here. */
    int ready = !m->stale && m->state != RIFT_SVC_ABSENT && m->have_status &&
                m->radio_online && !rift_model_action_busy(m, RIFT_ACTION_ADVERT_MESH);
    char text[RIFT_ACTION_TEXT_MAX];
    int warn = 0;

    rift_action_set_enabled(v->advert_near, 0, ready);
    rift_action_set_enabled(v->advert_mesh, 0, ready);
    rift_fmt_action(&m->advert, rift_app_now(v->app), text, sizeof(text));
    if (text[0]) {
        warn = m->advert.failed;
        lv_label_set_text(v->advert_line, text);
    } else if (!ready && (m->stale || m->state == RIFT_SVC_ABSENT)) {
        lv_label_set_text(v->advert_line, "meshcored is not answering.");
    } else if (!ready && !m->advert.active) {
        lv_label_set_text(v->advert_line, "The radio is not ready to send.");
    } else {
        lv_label_set_text(v->advert_line, "NEAR: heard in direct range, repeated by nobody. "
                                          "MESH: flooded through every repeater.");
    }
    if (warn != v->advert_warn) {
        if (warn) {
            pos_style_add(v->advert_line, POS_STYLE_STATUS_WARN_TEXT, 0);
        } else {
            lv_obj_remove_style(v->advert_line, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
        }
        v->advert_warn = warn;
    }
}

static void refresh_identity(struct rift_system_view *v)
{
    const struct rift_model *m = &v->app->model;
    char text[RIFT_KEY_SHORT_MAX];

    refresh_advert(v);
    if (!m->have_identity) {
        lv_label_set_text(v->id_name, RIFT_UNKNOWN);
        lv_label_set_text(v->id_hash, "");
        lv_label_set_text(v->id_key, RIFT_UNKNOWN);
        return;
    }
    lv_label_set_text(v->id_name, m->self_name[0] ? m->self_name : RIFT_UNKNOWN);
    lv_label_set_text(v->id_hash, m->self_hash);
    rift_fmt_key_short(m->self_key, text, sizeof(text));
    lv_label_set_text(v->id_key, text);
}

/* ---- SOUND ------------------------------------------------------------------ */

/* A sound's setting: a press turns it over, and that is all it does. It
 * sends nothing and plays nothing. */
static void on_dm_toggle(lv_event_t *e)
{
    struct rift_system_view *v = lv_event_get_user_data(e);

    rift_app_set_dm_sound(v->app, !v->app->prefs.dm_sound);
}

static void build_switch(struct sound_switch *s, lv_obj_t *panel, const char *title_text,
                         lv_event_cb_t cb, void *user)
{
    lv_obj_t *row = dense(panel, 12);
    lv_obj_t *title;

    lv_obj_set_height(row, RIFT_TOUCH_H);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    title = rift_cell(row, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_text(title, title_text);
    s->toggle = rift_action(row, "ON", 1, 1, cb, user);
    lv_obj_set_flex_grow(s->toggle, 0);
    lv_obj_set_width(s->toggle, SOUND_TOGGLE_W);
    s->label = lv_obj_get_child(s->toggle, 0);
    s->drawn = -1;
}

static void paint_switch(struct sound_switch *s, int on)
{
    if (on == s->drawn) {
        return;
    }
    lv_label_set_text(s->label, on ? "ON" : "OFF");
    lv_obj_remove_style(s->toggle, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(s->toggle, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(s->toggle, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    lv_obj_remove_style(s->toggle, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
    if (on) {
        pos_style_add(s->toggle, POS_STYLE_BUTTON_PRIMARY, 0);
        pos_style_add(s->toggle, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    } else {
        pos_style_add(s->toggle, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(s->toggle, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
    s->drawn = on;
}

static void build_sound(struct rift_system_view *v, lv_obj_t *parent)
{
    lv_obj_t *panel = rift_panel(parent, "SOUND");

    build_switch(&v->dm, panel, "Sound for a new DM", on_dm_toggle, v);
    v->sound_note = wrapping(panel, POS_STYLE_CAPTION);
}

/* The switch, and one line saying what it will actually do on this device -
 * which, with no platform sound to ask for, is nothing, and a switch that
 * says ON must not leave that unsaid. */
static void refresh_sound(struct rift_system_view *v)
{
    const struct rift_app *a = v->app;
    int on = a->prefs.dm_sound ? 1 : 0;
    const char *what;

    paint_switch(&v->dm, on);
    if (!on) {
        what = "Off: a new direct message is shown, not heard.";
    } else if (!rift_sound_available()) {
        what = rift_sound_why();
    } else if (!rift_app_can_sound(a)) {
        what = "Doors is muted: nothing is heard until the volume is up.";
    } else {
        what = "One short sound for a new direct message, at most one every 10 s. "
               "Not for channels, history or your own.";
    }
    if (!a->prefs_saved) {
        lv_label_set_text_fmt(v->sound_note, "%s Not saved: this lasts until RIFT closes.",
                              what);
    } else {
        lv_label_set_text(v->sound_note, what);
    }
}

/* ---- the screen ------------------------------------------------------------- */

lv_obj_t *rift_system_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_system_view *v = calloc(1, sizeof(*v));

    if (!v) {
        return NULL;
    }
    app->system = v;
    v->app = app;

    v->root = lv_obj_create(parent);
    lv_obj_remove_style_all(v->root);
    lv_obj_set_size(v->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(v->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(v->root, RIFT_PAD, 0);
    lv_obj_set_style_pad_top(v->root, RIFT_PAD - rift_caption_overhang(), 0);
    lv_obj_set_scroll_dir(v->root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->root, LV_SCROLLBAR_MODE_AUTO);

    v->split = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->split);
    lv_obj_set_width(v->split, LV_PCT(100));
    lv_obj_set_height(v->split, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v->split, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(v->split, 20 - rift_caption_overhang(), 0);
    lv_obj_set_style_pad_column(v->split, 20, 0);
    lv_obj_remove_flag(v->split, LV_OBJ_FLAG_SCROLLABLE);

    v->col[0] = column(v->split);
    v->col[1] = column(v->split);
    /* This node on the left - who it is, then how its floods are addressed -
     * and CLOSE RIFT last, under what it governs; what it plays and the
     * channels it holds on the right. Stacked in portrait in that order. */
    build_device(v, v->col[0]);
    rift_session_build(app, v->col[0]);
    build_sound(v, v->col[1]);
    rift_manage_build_channels(app, v->col[1]);
    return v->root;
}

void rift_system_destroy(struct rift_app *app)
{
    if (!of(app)) {
        return;
    }
    rift_manage_destroy(app);
    rift_device_destroy(app);
    rift_session_destroy(app);
    free(app->system);
    app->system = NULL;
}

void rift_system_shape(struct rift_app *app)
{
    struct rift_system_view *v = of(app);
    int i;

    if (!v) {
        return;
    }
    lv_obj_set_flex_flow(v->split, app->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    for (i = 0; i < 2; i++) {
        lv_obj_set_flex_grow(v->col[i], app->wide ? 1 : 0);
        lv_obj_set_width(v->col[i], app->wide ? LV_SIZE_CONTENT : LV_PCT(100));
    }
}

void rift_system_cancel(struct rift_app *app)
{
    rift_manage_cancel(app);
    rift_device_cancel(app);
    rift_session_cancel(app);
}

void rift_system_refresh(struct rift_app *app)
{
    struct rift_system_view *v = of(app);

    if (!v) {
        return;
    }
    refresh_identity(v);
    refresh_sound(v);
    rift_manage_refresh(app);
    rift_device_refresh(app);
    rift_session_refresh(app);
}
