/*
 * RX LOG: what a reader does - PAUSE, CLEAR, FILTER, a tap, a drag, the keys
 * and the detail. See rift_rxlog_view.h; the drawing is rift_rxlog_view.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_rxlog_view_int.h"

#include <stdio.h>
#include <string.h>

/* ---- what a reader does -------------------------------------------------- */

static void toggle_pause(struct rift_app *a)
{
    struct rift_rxlog *log = &a->rxlog;

    log->paused = !log->paused;
    if (!log->paused) {
        /* RESUME is back to the newest, live. */
        log->top_uid = 0;
    }
    rxv_repaint(a->rxlog_view, 1);
}

static void next_filter(struct rift_app *a)
{
    struct rift_rxlog *log = &a->rxlog;

    log->filter = (log->filter + 1) % RIFT_RXF_COUNT;
    /* A held row the new filter does not show lets go (window_top). */
    rxv_repaint(a->rxlog_view, 1);
}

static void clear_log(struct rift_app *a)
{
    /* The log and only the log: no node, message or mesh state is touched. */
    rift_rxlog_clear(&a->rxlog);
    lv_obj_add_flag(a->rxlog_view->detail, LV_OBJ_FLAG_HIDDEN);
    rxv_repaint(a->rxlog_view, 1);
}

static void open_detail(struct rift_app *a, uint32_t uid)
{
    struct rift_rxlog_view *v = a->rxlog_view;

    if (!uid || !rift_rxlog_find(&a->rxlog, uid)) {
        return;
    }
    a->rxlog.selected_uid = uid;
    lv_obj_remove_flag(v->detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(v->detail);
    rxv_repaint(v, 1);
    rxv_paint_detail(v);
}

void rxv_on_pause(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    if (a && a->rxlog_view) {
        toggle_pause(a);
    }
}

void rxv_on_clear(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    if (a && a->rxlog_view) {
        clear_log(a);
    }
}

void rxv_on_filter(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    if (a && a->rxlog_view) {
        next_filter(a);
    }
}

void rxv_on_detail_close(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    rift_rxlog_view_back(a);
}

void rift_rxlog_view_scroll(struct rift_app *app, int rows)
{
    struct rift_rxlog_view *v = rxv_of(app);

    if (!v || rows == 0) {
        return;
    }
    rxv_set_top(&app->rxlog, rxv_window_top(&app->rxlog) + rows);
    rxv_repaint(v, 1);
}

/* A drag moves the window a row per RXV_DRAG_ROW_PX, the finger going up showing
 * older entries; a tap that did not drag opens the row it landed on. */
void rxv_on_list(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    struct rift_rxlog_view *v = rxv_of(a);
    lv_event_code_t code = lv_event_get_code(e);

    if (!v) {
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        v->drag_acc = 0;
        v->dragged = 0;
    } else if (code == LV_EVENT_PRESSING) {
        lv_point_t vect;
        lv_indev_t *indev = lv_indev_active();

        if (!indev) {
            return;
        }
        lv_indev_get_vect(indev, &vect);
        v->drag_acc += vect.y;
        if (v->drag_acc >= RXV_DRAG_ROW_PX || v->drag_acc <= -RXV_DRAG_ROW_PX) {
            int rows = -v->drag_acc / RXV_DRAG_ROW_PX;

            v->drag_acc += rows * RXV_DRAG_ROW_PX;
            v->dragged = 1;
            rift_rxlog_view_scroll(a, rows);
        }
    } else if (code == LV_EVENT_CLICKED && !v->dragged) {
        lv_obj_t *target = lv_event_get_target(e);
        int i;

        for (i = 0; i < v->shown; i++) {
            if (v->rows[i].row == target) {
                open_detail(a, v->rows[i].uid);
                return;
            }
        }
    }
}

/* Move the selection by step entries, keeping it in the window. */
static void move_selection(struct rift_app *a, int step)
{
    struct rift_rxlog_view *v = a->rxlog_view;
    struct rift_rxlog *log = &a->rxlog;
    int count = rift_rxlog_count(log, log->filter);
    int top = rxv_window_top(log);
    int sel = rift_rxlog_position(log, log->filter, log->selected_uid);
    int page = v->visible > 0 ? v->visible : 1;

    if (count == 0) {
        return;
    }
    sel = sel < 0 ? top : sel + step;
    if (sel < 0) {
        sel = 0;
    }
    if (sel > count - 1) {
        sel = count - 1;
    }
    log->selected_uid = rxv_uid_at(log, sel);
    if (sel < top) {
        rxv_set_top(log, sel);
    } else if (sel >= top + page) {
        rxv_set_top(log, sel - page + 1);
    }
    rxv_repaint(v, 1);
}

int rift_rxlog_view_key(struct rift_app *app, uint32_t key)
{
    struct rift_rxlog_view *v = rxv_of(app);
    int page;

    if (!app || !v) {
        return 0;
    }
    if (app->section == RIFT_SEC_ACTIVITY && (key == 'r' || key == 'R')) {
        rift_app_show_section(app, RIFT_SEC_RXLOG);
        return 1;
    }
    if (app->section != RIFT_SEC_RXLOG) {
        return 0;
    }
    page = v->visible > 1 ? v->visible - 1 : 1;
    if (!lv_obj_has_flag(v->detail, LV_OBJ_FLAG_HIDDEN)) {
        if (key == LV_KEY_ESC || key == LV_KEY_ENTER || key == LV_KEY_BACKSPACE) {
            return rift_rxlog_view_back(app);
        }
    }
    switch (key) {
    case LV_KEY_UP:
        move_selection(app, -1);
        return 1;
    case LV_KEY_DOWN:
        move_selection(app, 1);
        return 1;
    case LV_KEY_LEFT:
        move_selection(app, -page);
        return 1;
    case LV_KEY_RIGHT:
        move_selection(app, page);
        return 1;
    case LV_KEY_HOME:
        app->rxlog.paused = 0;
        app->rxlog.top_uid = 0;
        app->rxlog.selected_uid = 0;
        rxv_repaint(v, 1);
        return 1;
    case LV_KEY_ENTER:
        if (!rift_rxlog_find(&app->rxlog, app->rxlog.selected_uid)) {
            move_selection(app, 0);
        }
        open_detail(app, app->rxlog.selected_uid);
        return 1;
    case 'p':
    case 'P':
    case ' ':
        toggle_pause(app);
        return 1;
    case 'f':
    case 'F':
        next_filter(app);
        return 1;
    case 'c':
    case 'C':
        clear_log(app);
        return 1;
    default:
        return 0;
    }
}

int rift_rxlog_view_back(struct rift_app *app)
{
    struct rift_rxlog_view *v = rxv_of(app);

    if (!v || lv_obj_has_flag(v->detail, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    lv_obj_add_flag(v->detail, LV_OBJ_FLAG_HIDDEN);
    return 1;
}

/* ---- for the tests -------------------------------------------------------- */

lv_obj_t *rift_rxlog_view_button(const struct rift_app *app, int which)
{
    const struct rift_rxlog_view *v = rxv_of(app);

    if (!v) {
        return NULL;
    }
    return which == 0 ? v->pause : which == 1 ? v->clear : which == 2 ? v->filter : NULL;
}

lv_obj_t *rift_rxlog_view_caption(const struct rift_app *app)
{
    return rxv_of(app) ? rxv_of(app)->caption : NULL;
}

int rift_rxlog_view_rows_shown(const struct rift_app *app)
{
    return rxv_of(app) ? rxv_of(app)->shown : 0;
}

lv_obj_t *rift_rxlog_view_row(const struct rift_app *app, int i)
{
    const struct rift_rxlog_view *v = rxv_of(app);

    return (v && i >= 0 && i < v->shown) ? v->rows[i].row : NULL;
}

lv_obj_t *rift_rxlog_view_detail(const struct rift_app *app)
{
    const struct rift_rxlog_view *v = rxv_of(app);

    return (v && !lv_obj_has_flag(v->detail, LV_OBJ_FLAG_HIDDEN)) ? v->detail_text : NULL;
}

int rift_rxlog_view_row_text(const struct rift_app *app, int i, int line, char *out, size_t out_len)
{
    const struct rift_rxlog_view *v = rxv_of(app);
    const struct row *r;

    if (!v || i < 0 || i >= v->shown || !out || out_len == 0) {
        return -1;
    }
    r = &v->rows[i];
    if (line == 0) {
        snprintf(out, out_len, "%s %s %s %s %s %s %s %s", lv_label_get_text(r->time.label),
                 lv_label_get_text(r->state.label), lv_label_get_text(r->type.label),
                 lv_label_get_text(r->ch.label), lv_label_get_text(r->hash.label),
                 lv_label_get_text(r->size.label), lv_label_get_text(r->rssi.label),
                 lv_label_get_text(r->snr.label));
    } else if (line == 1) {
        snprintf(out, out_len, "%s",
                 lv_obj_has_flag(r->path.label, LV_OBJ_FLAG_HIDDEN) ? "" : lv_label_get_text(r->path.label));
    } else {
        snprintf(out, out_len, "%s%s%s",
                 lv_obj_has_flag(r->line3, LV_OBJ_FLAG_HIDDEN) ? "" : lv_label_get_text(r->who.label),
                 lv_obj_has_flag(r->line3, LV_OBJ_FLAG_HIDDEN) || !lv_label_get_text(r->who.label)[0]
                     ? ""
                     : " ",
                 lv_obj_has_flag(r->line3, LV_OBJ_FLAG_HIDDEN) ? "" : lv_label_get_text(r->what.label));
    }
    return 0;
}
