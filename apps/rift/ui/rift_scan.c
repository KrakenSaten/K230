/*
 * ACTIVITY's REPEATERS 0-HOP panel. See rift_scan.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_scan.h"

#include "pos_styles.h"
#include "rift_form.h"
#include "rift_repeater_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCAN_W 200

struct scan_row {
    lv_obj_t *row;
    lv_obj_t *glyph;
    lv_obj_t *name;
    lv_obj_t *rssi;
    lv_obj_t *snr;
    lv_obj_t *age;
    char key[RIFT_REP_KEY];
};

struct rift_scan_view {
    struct rift_app *app;
    lv_obj_t *button;
    lv_obj_t *button_label;
    lv_obj_t *caption;
    lv_obj_t *note;
    lv_obj_t *earlier;
    struct scan_row rows[RIFT_SCAN_ROWS];
    lv_obj_t *more;
    /* The keyboard's place: SCAN 0-HOP or a row, or NULL before an arrow
     * key was pressed (rift_scan_key). */
    lv_obj_t *cursor;
};

static struct rift_scan_view *of(const struct rift_app *app)
{
    return app ? app->scan : NULL;
}

/* The one handler that asks repeaters to answer, on a reader's press. The
 * client refuses it, unwritten, while a round is open. */
static void on_scan(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    rift_ipc_scan_repeaters(&a->ipc);
    a->refresh_pending = 1;
}

static void on_row(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    struct rift_scan_view *v = of(a);
    lv_obj_t *target = lv_event_get_current_target_obj(e);
    int i;

    if (!v) {
        return;
    }
    for (i = 0; i < RIFT_SCAN_ROWS; i++) {
        if (v->rows[i].row == target && v->rows[i].key[0]) {
            rift_repeater_open(a, v->rows[i].key);
            return;
        }
    }
}

void rift_scan_build(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_scan_view *v = calloc(1, sizeof(*v));
    lv_obj_t *panel;
    lv_obj_t *line;
    int i;

    if (!v) {
        return;
    }
    v->app = app;
    app->scan = v;
    panel = rift_panel(parent, "REPEATERS 0-HOP");
    line = rift_form_row(panel, RIFT_TOUCH_H);
    v->button = rift_action(line, "SCAN 0-HOP", 0, 1, on_scan, app);
    lv_obj_set_flex_grow(v->button, 0);
    lv_obj_set_width(v->button, SCAN_W);
    v->button_label = lv_obj_get_child(v->button, 0);
    pos_style_add(v->button, POS_STYLE_FIELD_FOCUSED, LV_STATE_USER_1);
    v->caption = rift_cell(line, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(v->caption, 1);
    v->note = rift_form_text(panel, POS_STYLE_TEXT_MUTED);

    for (i = 0; i < RIFT_SCAN_ROWS; i++) {
        struct scan_row *r = &v->rows[i];

        /* A row opens a page, so it is a 56 px target, not a 36 px one that
         * only selects (handoff §4). A button, so the keys reach it too. */
        r->row = lv_button_create(panel);
        lv_obj_remove_style_all(r->row);
        pos_style_add(r->row, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(r->row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_width(r->row, LV_PCT(100));
        lv_obj_set_height(r->row, RIFT_TOUCH_H);
        lv_obj_set_flex_flow(r->row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(r->row, 12, 0);
        lv_obj_set_style_pad_column(r->row, 8, 0);
        lv_obj_remove_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(r->row, on_row, LV_EVENT_CLICKED, app);
        pos_style_add(r->row, POS_STYLE_FIELD_FOCUSED, LV_STATE_USER_1);
        r->glyph = rift_glyph_create(r->row);
        r->name = rift_cell(r->row, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_flex_grow(r->name, 1);
        r->rssi = rift_cell(r->row, POS_STYLE_CAPTION, 72, LV_TEXT_ALIGN_RIGHT);
        r->snr = rift_cell(r->row, POS_STYLE_CAPTION, 64, LV_TEXT_ALIGN_RIGHT);
        r->age = rift_cell(r->row, POS_STYLE_CAPTION, 48, LV_TEXT_ALIGN_RIGHT);
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        if (i == 0) {
            /* Placed before the first row of the earlier group when there is
             * one (refresh moves it). */
            v->earlier = rift_group_label(panel, "EARLIER SCANS");
            lv_obj_add_flag(v->earlier, LV_OBJ_FLAG_HIDDEN);
        }
    }
    v->more = rift_form_text(panel, POS_STYLE_CAPTION);
}

void rift_scan_destroy(struct rift_app *app)
{
    struct rift_scan_view *v = of(app);

    if (v) {
        free(v);
        app->scan = NULL;
    }
}

/* Newest first, the latest round's answers before the earlier ones. */
static int order(const struct rift_repeater *r, int *idx)
{
    int n = 0;
    int pass;
    int i;
    int j;

    for (pass = 0; pass < 2; pass++) {
        int start = n;

        for (i = 0; i < r->scan.count; i++) {
            if ((pass == 0) == (r->scan.found[i].current != 0)) {
                idx[n++] = i;
            }
        }
        for (i = start + 1; i < n; i++) {
            int k = idx[i];

            for (j = i; j > start && r->scan.found[idx[j - 1]].mono_ms < r->scan.found[k].mono_ms;
                 j--) {
                idx[j] = idx[j - 1];
            }
            idx[j] = k;
        }
    }
    return n;
}

static void refresh_head(struct rift_scan_view *v, const struct rift_repeater *r, int64_t now,
                         int current)
{
    int scanning = rift_rep_scanning(r, now);
    int can = rift_ipc_connected(&v->app->ipc) && !scanning && !r->scan.unsupported;
    char text[RIFT_TEXT_MAX];
    char age[RIFT_AGE_MAX];

    rift_action_set_enabled(v->button, 0, can);
    rift_label_set(v->button_label, scanning ? "SCANNING" : "SCAN 0-HOP");
    if (scanning && r->scan.open) {
        int64_t left = r->scan.until_ms - now;

        snprintf(text, sizeof(text), "LISTENING" RIFT_SEP "%d S LEFT" RIFT_SEP "%d HEARD",
                 (int)((left > 0 ? left : 0) + 999) / 1000, current);
    } else if (scanning) {
        snprintf(text, sizeof(text), "ASKING");
    } else if (r->scan.have_round) {
        rift_fmt_age(now - r->scan.started_ms, r->scan.started_ms > 0, age, sizeof(age));
        snprintf(text, sizeof(text), "LAST SCAN %s AGO" RIFT_SEP "%d HEARD", age, current);
    } else {
        snprintf(text, sizeof(text), "NOT SCANNED YET");
    }
    rift_cell_set_text_fit(v->caption, text);

    if (r->scan.unsupported) {
        lv_label_set_text(v->note, "This meshcored has no repeater discovery.");
    } else if (r->scan.error[0]) {
        lv_label_set_text_fmt(v->note, "Not scanned: %s.", r->scan.error);
    } else if (!r->scan.have_round && r->scan.count == 0) {
        lv_label_set_text(v->note, "Asks every repeater this node can hear directly to "
                                   "answer: one packet, sent only when pressed. A row opens "
                                   "the repeater.");
    } else if (r->scan.count == 0 && !scanning) {
        lv_label_set_text(v->note, "No repeater answered directly.");
    } else {
        lv_label_set_text(v->note, "");
    }
    rift_form_show(v->note, lv_label_get_text(v->note)[0] != '\0');
}

void rift_scan_refresh(struct rift_app *app)
{
    struct rift_scan_view *v = of(app);
    const struct rift_repeater *r;
    int idx[RIFT_REP_FOUND_MAX];
    int n;
    int current = 0;
    int earlier_at = -1;
    int64_t now;
    int i;

    if (!v) {
        return;
    }
    r = &app->model.repeater;
    now = rift_app_now(app);
    n = order(r, idx);
    for (i = 0; i < n; i++) {
        current += r->scan.found[idx[i]].current ? 1 : 0;
    }
    refresh_head(v, r, now, current);

    for (i = 0; i < RIFT_SCAN_ROWS; i++) {
        struct scan_row *row = &v->rows[i];
        const struct rift_rep_found *f;
        char text[RIFT_NAME_MAX + 24];

        if (i >= n) {
            row->key[0] = '\0';
            lv_obj_add_flag(row->row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        f = &r->scan.found[idx[i]];
        if (!f->current && earlier_at < 0) {
            earlier_at = i;
        }
        snprintf(row->key, sizeof(row->key), "%s", f->key);
        lv_obj_remove_flag(row->row, LV_OBJ_FLAG_HIDDEN);
        /* Heard directly in the latest round, or in an earlier one: the
         * glyph and the group say which, and the age says when. */
        rift_glyph_set(row->glyph, f->current ? RIFT_GLYPH_DIRECT : RIFT_GLYPH_STALE);
        rift_fmt_rssi(f->rssi_dbm, f->have_rssi, text, sizeof(text));
        rift_label_set(row->rssi, text);
        rift_fmt_snr(f->snr_db, f->have_snr, text, sizeof(text));
        rift_label_set(row->snr, text);
        rift_fmt_age(now - f->mono_ms, f->mono_ms > 0, text, sizeof(text));
        rift_label_set(row->age, text);
        if (f->name[0]) {
            snprintf(text, sizeof(text), "%s", f->name);
        } else {
            /* Not in the node list: a key and nothing more. */
            snprintf(text, sizeof(text), "%.8s" RIFT_SEP "NO ADVERT YET", f->key);
        }
        rift_cell_set_text_fit_room(row->name, text, 0);
    }
    /* The group label sits right above the first earlier row. */
    if (earlier_at >= 0) {
        int32_t at = (int32_t)lv_obj_get_index(v->rows[earlier_at].row);
        int32_t label = (int32_t)lv_obj_get_index(v->earlier);

        /* Taking the label out first moves every later child up by one. */
        lv_obj_move_to_index(v->earlier, label < at ? at - 1 : at);
        lv_obj_remove_flag(v->earlier, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v->earlier, LV_OBJ_FLAG_HIDDEN);
    }
    if (n > RIFT_SCAN_ROWS) {
        lv_label_set_text_fmt(v->more, "%d more not shown.", n - RIFT_SCAN_ROWS);
        rift_form_show(v->more, 1);
    } else {
        rift_form_show(v->more, 0);
    }
}

lv_obj_t *rift_scan_button(const struct rift_app *app)
{
    return of(app) ? of(app)->button : NULL;
}

lv_obj_t *rift_scan_row(const struct rift_app *app, int i)
{
    return (of(app) && i >= 0 && i < RIFT_SCAN_ROWS) ? of(app)->rows[i].row : NULL;
}

lv_obj_t *rift_scan_caption(const struct rift_app *app)
{
    return of(app) ? of(app)->caption : NULL;
}

int rift_scan_key(struct rift_app *app, uint32_t key)
{
    struct rift_scan_view *v = of(app);
    lv_obj_t *items[1 + RIFT_SCAN_ROWS];
    int n = 0;
    int at = -1;
    int i;

    if (!v || (key != LV_KEY_UP && key != LV_KEY_DOWN && key != LV_KEY_ENTER)) {
        return 0;
    }
    if (!lv_obj_has_state(v->button, LV_STATE_DISABLED)) {
        items[n++] = v->button;
    }
    for (i = 0; i < RIFT_SCAN_ROWS; i++) {
        if (!lv_obj_has_flag(v->rows[i].row, LV_OBJ_FLAG_HIDDEN)) {
            items[n++] = v->rows[i].row;
        }
    }
    for (i = 0; i < n; i++) {
        if (items[i] == v->cursor) {
            at = i;
        }
    }
    if (key == LV_KEY_ENTER) {
        if (at < 0) {
            return 0;
        }
        lv_obj_send_event(v->cursor, LV_EVENT_CLICKED, NULL);
        return 1;
    }
    if (n == 0) {
        return 0;
    }
    if (at < 0) {
        at = 0;
    } else if (key == LV_KEY_DOWN && at + 1 < n) {
        at++;
    } else if (key == LV_KEY_UP && at > 0) {
        at--;
    }
    rift_repeater_cursor_move(&v->cursor, items[at]);
    return 1;
}
