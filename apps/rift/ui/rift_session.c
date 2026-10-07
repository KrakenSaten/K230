/*
 * SESSION: the way to end RIFT. See rift_session.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_session.h"

#include "pos_styles.h"
#include "rift_form.h"

#include <stdlib.h>

#define CLOSE_W 168

struct rift_session_view {
    struct rift_app *app;
    lv_obj_t *close;
    lv_obj_t *note;
    lv_obj_t *confirm;
    lv_obj_t *cancel_btn;
    lv_obj_t *confirm_btn;
    int confirming;
    int drawn; /* the confirmation as last drawn; -1 not yet */
};

static struct rift_session_view *of(const struct rift_app *app)
{
    return app ? app->session : NULL;
}

static void on_close(lv_event_t *e)
{
    struct rift_session_view *v = of(lv_event_get_user_data(e));

    if (v) {
        v->confirming = 1;
        v->app->refresh_pending = 1;
    }
}

static void on_cancel(lv_event_t *e)
{
    struct rift_session_view *v = of(lv_event_get_user_data(e));

    if (v) {
        v->confirming = 0;
        v->app->refresh_pending = 1;
    }
}

/* The one way to the end of the session. The shell takes this screen away
 * inside this press, as it does for the back slab's. */
static void on_confirm(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    struct rift_session_view *v = of(a);

    if (v && v->confirming) {
        v->confirming = 0;
        rift_app_end(a);
    }
}

void rift_session_build(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_session_view *v = calloc(1, sizeof(*v));
    lv_obj_t *panel;
    lv_obj_t *row;
    lv_obj_t *title;
    lv_obj_t *bar;

    if (!v) {
        return;
    }
    v->app = app;
    v->drawn = -1;
    app->session = v;

    panel = rift_panel(parent, "SESSION");
    row = rift_form_row(panel, RIFT_TOUCH_H);
    title = rift_cell(row, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_text(title, "Runs when left");
    v->close = rift_action(row, "CLOSE RIFT", 0, 1, on_close, app);
    lv_obj_set_flex_grow(v->close, 0);
    lv_obj_set_width(v->close, CLOSE_W);
    v->note = rift_form_text(panel, POS_STYLE_CAPTION);
    lv_label_set_text(v->note, "Back and Home leave RIFT running: it keeps taking in the mesh, "
                               "and the status bar shows RIFT. CLOSE RIFT ends it. meshcored and "
                               "the radio keep running either way.");

    /* The confirmation (DS §17.5): what it costs, Cancel first and accented. */
    v->confirm = rift_form_column(panel);
    lv_label_set_text(rift_form_text(v->confirm, POS_STYLE_TITLE), "Close RIFT?");
    lv_label_set_text(rift_form_text(v->confirm, POS_STYLE_TEXT_SECONDARY),
                      "RIFT stops listening and lets go of meshcored. What it gathered since it "
                      "opened - activity, the traffic graph, messages the radio service no "
                      "longer holds - goes with it. meshcored and the radio keep running.");
    bar = rift_form_row(v->confirm, RIFT_TOUCH_H);
    v->cancel_btn = rift_action(bar, "CANCEL", 1, 1, on_cancel, app);
    v->confirm_btn = rift_action(bar, "CLOSE RIFT", 0, 1, on_confirm, app);
    lv_obj_add_flag(v->confirm, LV_OBJ_FLAG_HIDDEN);
}

void rift_session_refresh(struct rift_app *app)
{
    struct rift_session_view *v = of(app);

    if (!v || v->confirming == v->drawn) {
        return;
    }
    /* Only on a change: re-enabling restyles the button. */
    rift_action_set_enabled(v->close, 0, !v->confirming);
    rift_form_show(v->confirm, v->confirming);
    v->drawn = v->confirming;
}

void rift_session_cancel(struct rift_app *app)
{
    struct rift_session_view *v = of(app);

    if (v && v->confirming) {
        v->confirming = 0;
        app->refresh_pending = 1;
    }
}

void rift_session_destroy(struct rift_app *app)
{
    struct rift_session_view *v = of(app);

    if (v) {
        free(v);
        app->session = NULL;
    }
}

lv_obj_t *rift_session_close_button(const struct rift_app *app)
{
    return of(app) ? of(app)->close : NULL;
}

lv_obj_t *rift_session_confirm_button(const struct rift_app *app, int close)
{
    struct rift_session_view *v = of(app);

    return v ? (close ? v->confirm_btn : v->cancel_btn) : NULL;
}

int rift_session_confirming(const struct rift_app *app)
{
    return of(app) ? of(app)->confirming : 0;
}
