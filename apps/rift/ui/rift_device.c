/*
 * THIS DEVICE's name and path hash size. See rift_device.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_device.h"

#include "app.h"
#include "pocketui.h"
#include "pos_styles.h"
#include "rift_form.h"
#include "rift_keys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RENAME_W 144
#define BYTES_W 76

struct rift_device {
    struct rift_app *app;
    lv_obj_t *rename;
    lv_obj_t *rename_note;
    /* The warning under it, for what the last rename came to when that was
     * not "renamed and saved": refused, unanswered, or in use and not
     * written. Beside the name, because the panel's status line is at its
     * foot - below the fold on unit B, where a refused rename read as one
     * that had been taken (2026-10-03). */
    lv_obj_t *rename_unsaved;
    lv_obj_t *rename_form;
    lv_obj_t *rename_field;
    lv_obj_t *rename_status;
    int rename_open;
    char rename_error[RIFT_TEXT_MAX];
    lv_obj_t *bytes_btn[3];
    int bytes_drawn; /* -1 not yet */
    lv_obj_t *bytes_note;
    lv_obj_t *bytes_confirm;
    lv_obj_t *bytes_confirm_title;
    int bytes_pending; /* the size a confirmation is up for; 0 none */
    lv_obj_t *status;
};

static struct rift_device *of(const struct rift_app *app)
{
    return app ? app->device : NULL;
}

static int busy(const struct rift_app *a)
{
    return a->model.manage_op.active;
}

/* ---- the name ------------------------------------------------------------- */

static void on_rename_open(lv_event_t *e)
{
    struct rift_device *v = of(lv_event_get_user_data(e));

    if (v) {
        v->rename_open = 1;
        v->rename_error[0] = '\0';
        lv_textarea_set_text(v->rename_field, v->app->model.self_name);
        v->app->refresh_pending = 1;
    }
}

static void on_rename_cancel(lv_event_t *e)
{
    struct rift_device *v = of(lv_event_get_user_data(e));

    if (v) {
        v->rename_open = 0;
        if (pocketos_shell_keyboard_visible()) {
            pocketos_shell_keyboard_hide();
        }
        v->app->refresh_pending = 1;
    }
}

/* SAVE: the name checked here first by the service's own rule - one line,
 * no control characters, not only spaces, at most the bytes it reports - so
 * the reader hears what is wrong before anything is asked. */
static void on_rename_save(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    struct rift_device *v = of(a);
    char typed[4 * RIFT_NAME_MAX];
    char why[RIFT_TEXT_MAX];
    int max;

    if (!v) {
        return;
    }
    rift_form_typed(v->rename_field, typed, sizeof(typed));
    max = a->model.self_name_max > 0 ? a->model.self_name_max : RIFT_CHANNEL_NAME_BYTES;
    v->rename_error[0] = '\0';
    if (strlen(typed) > (size_t)max) {
        snprintf(v->rename_error, sizeof(v->rename_error),
                 "That name is longer than the %d bytes MeshCore keeps.", max);
    } else if (rift_channel_name_check(typed, why, sizeof(why)) != 0) {
        snprintf(v->rename_error, sizeof(v->rename_error), "%s",
                 typed[0] ? "A name is one line, with no control characters and not only spaces."
                          : "A node needs a name.");
    } else if (strcmp(typed, a->model.self_name) == 0 &&
               !(a->model.manage_op.kind == RIFT_ACTION_RENAME && a->model.manage_op.unsaved)) {
        v->rename_open = 0; /* the same name, and it is saved: nothing to ask */
    } else if (rift_ipc_set_name(&a->ipc, typed) == 0) {
        v->rename_open = 0;
    } else {
        snprintf(v->rename_error, sizeof(v->rename_error), "%s",
                 a->model.manage_op.failed ? a->model.manage_op.error : "Nothing was asked.");
    }
    if (!v->rename_open && pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_hide();
    }
    a->refresh_pending = 1;
}

/* ---- the path hash size --------------------------------------------------- */

static void on_bytes(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    struct rift_device *v = of(a);
    lv_obj_t *btn = lv_event_get_target_obj(e);
    int b;

    for (b = 1; v && b <= 3; b++) {
        if (v->bytes_btn[b - 1] != btn || b == a->model.path_hash_bytes) {
            continue;
        }
        if (b == 1) {
            /* Back to what every MeshCore node reads: nothing to warn of. */
            rift_ipc_set_path_hash(&a->ipc, 1);
            v->bytes_pending = 0;
        } else {
            v->bytes_pending = b; /* asks first */
        }
        a->refresh_pending = 1;
    }
}

static void on_bytes_cancel(lv_event_t *e)
{
    struct rift_device *v = of(lv_event_get_user_data(e));

    if (v) {
        v->bytes_pending = 0;
        v->app->refresh_pending = 1;
    }
}

/* The one place a size other than 1 is asked for: the confirmation. */
static void on_bytes_confirm(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    struct rift_device *v = of(a);

    if (v && v->bytes_pending) {
        rift_ipc_set_path_hash(&a->ipc, v->bytes_pending);
        v->bytes_pending = 0;
        a->refresh_pending = 1;
    }
}

/* ---- the entry points ------------------------------------------------------- */

void rift_device_build(struct rift_app *app, lv_obj_t *panel)
{
    static const char *const words[3] = { "1 B", "2 B", "3 B" };
    struct rift_device *v = calloc(1, sizeof(*v));
    lv_obj_t *row;
    lv_obj_t *title;
    lv_obj_t *bar;
    int b;

    if (!v) {
        return;
    }
    v->app = app;
    v->bytes_drawn = -1;
    app->device = v;

    row = rift_form_row(panel, RIFT_TOUCH_H);
    title = rift_cell(row, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_text(title, "Name");
    v->rename = rift_action(row, "RENAME", 0, 1, on_rename_open, app);
    lv_obj_set_flex_grow(v->rename, 0);
    lv_obj_set_width(v->rename, RENAME_W);
    v->rename_note = rift_form_text(panel, POS_STYLE_CAPTION);
    v->rename_unsaved = rift_form_text(panel, POS_STYLE_STATUS_WARN_TEXT);
    lv_obj_add_flag(v->rename_unsaved, LV_OBJ_FLAG_HIDDEN);

    v->rename_form = rift_form_column(panel);
    v->rename_field = rift_form_field(app, v->rename_form, "Node name", RIFT_NAME_MAX - 1);
    v->rename_status = rift_form_text(v->rename_form, POS_STYLE_STATUS_WARN_TEXT);
    bar = rift_form_row(v->rename_form, RIFT_TOUCH_H);
    rift_action(bar, "CANCEL", 0, 1, on_rename_cancel, app);
    rift_action(bar, "SAVE", 1, 1, on_rename_save, app);
    lv_obj_add_flag(v->rename_form, LV_OBJ_FLAG_HIDDEN);

    row = rift_form_row(panel, RIFT_TOUCH_H);
    title = rift_cell(row, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_text(title, "Path hash");
    for (b = 0; b < 3; b++) {
        v->bytes_btn[b] = rift_action(row, words[b], 0, 1, on_bytes, app);
        lv_obj_set_flex_grow(v->bytes_btn[b], 0);
        lv_obj_set_width(v->bytes_btn[b], BYTES_W);
    }
    v->bytes_note = rift_form_text(panel, POS_STYLE_CAPTION);
    /* The confirmation (DS §17.5): what it costs, Cancel first and accented. */
    v->bytes_confirm = rift_form_column(panel);
    v->bytes_confirm_title = rift_form_text(v->bytes_confirm, POS_STYLE_TITLE);
    lv_label_set_text(rift_form_text(v->bytes_confirm, POS_STYLE_TEXT_SECONDARY),
                      "Each repeater writes this many bytes of its key into the path of every "
                      "flood this node starts, so more relays can be told apart. Repeaters "
                      "whose firmware does not read multi-byte paths drop such floods: a "
                      "message or advert may not get through where 1 byte would.");
    bar = rift_form_row(v->bytes_confirm, RIFT_TOUCH_H);
    rift_action(bar, "CANCEL", 1, 1, on_bytes_cancel, app);
    rift_action(bar, "USE IT", 0, 1, on_bytes_confirm, app);
    lv_obj_add_flag(v->bytes_confirm, LV_OBJ_FLAG_HIDDEN);
    v->status = rift_form_text(panel, POS_STYLE_CAPTION);
}

void rift_device_refresh(struct rift_app *app)
{
    struct rift_device *v = of(app);
    const struct rift_model *m;
    const struct rift_action_state *op;
    int can;
    char text[RIFT_ACTION_TEXT_MAX];
    int b;

    if (!v) {
        return;
    }
    m = &app->model;
    op = &m->manage_op;
    can = rift_form_service_ready(app) && !busy(app);

    /* The name is this node's to change wherever it came from: one given in
     * the service's configuration is renamed too, and the service keeps the
     * rename over it (docs/api/mesh.md, mesh.set_name). */
    rift_action_set_enabled(v->rename, 0, can && !v->rename_open);
    rift_label_set(v->rename_note, "The name goes out in this node's adverts and in front of "
                                   "every channel message.");
    /* What the last rename came to, when it was not a name taken and
     * saved - said here, under the name, for as long as it is the last
     * thing a rename did. A refusal is the service's own words. */
    text[0] = '\0';
    if (op->kind == RIFT_ACTION_RENAME && !op->active) {
        if (op->failed && op->unknown) {
            snprintf(text, sizeof(text),
                     "The radio service did not answer: the name may or may not have "
                     "changed. It is %s now.",
                     m->self_name[0] ? m->self_name : RIFT_UNKNOWN);
        } else if (op->failed) {
            snprintf(text, sizeof(text), "Not renamed - the name is still %s. %s",
                     m->self_name[0] ? m->self_name : RIFT_UNKNOWN, op->error);
        } else if (op->done && op->unsaved) {
            snprintf(text, sizeof(text),
                     "The new name is in use, but the radio service could not save it: "
                     "the old name returns when that service restarts. RENAME again to "
                     "retry.");
        }
    }
    rift_label_set(v->rename_unsaved, text);
    rift_form_show(v->rename_unsaved, text[0] != '\0');
    rift_form_show(v->rename_form, v->rename_open);
    rift_form_field_live(v->rename_field, v->rename_open);
    rift_label_set(v->rename_status, v->rename_error);
    rift_form_show(v->rename_status, v->rename_error[0] != '\0');

    for (b = 1; b <= 3; b++) {
        int allowed = m->have_path_hash && (m->path_hash_allowed & (1u << b));
        int chosen = m->have_path_hash && b == m->path_hash_bytes;

        /* Enabling restyles the button; the chosen size comes back accented,
         * or any channel operation (which disables these while it runs)
         * left no size looking chosen (unit B, 2026-10-02). */
        rift_action_set_enabled(v->bytes_btn[b - 1], chosen,
                                can && allowed && !v->bytes_pending);
    }
    if (m->have_path_hash && m->path_hash_bytes != v->bytes_drawn) {
        for (b = 1; b <= 3; b++) {
            rift_form_chosen(v->bytes_btn[b - 1], b == m->path_hash_bytes);
        }
        v->bytes_drawn = m->path_hash_bytes;
    }
    if (m->path_hash_unsupported) {
        rift_label_set(v->bytes_note, "This radio service has no path hash setting: its floods "
                                      "carry 1-byte hashes.");
    } else if (!m->have_path_hash) {
        rift_label_set(v->bytes_note, "PATH HASH " RIFT_UNKNOWN);
    } else {
        snprintf(text, sizeof(text),
                 "%d BYTE%s PER RELAY IN THIS NODE'S FLOODS" RIFT_SEP
                 "1 IS WHAT EVERY MESHCORE NODE READS",
                 m->path_hash_bytes, m->path_hash_bytes == 1 ? "" : "S");
        rift_label_set(v->bytes_note, text);
    }
    rift_form_show(v->bytes_confirm, v->bytes_pending != 0);
    if (v->bytes_pending) {
        snprintf(text, sizeof(text), "Use %d-byte path hashes?", v->bytes_pending);
        rift_label_set(v->bytes_confirm_title, text);
    }

    text[0] = '\0';
    if (op->kind == RIFT_ACTION_RENAME || op->kind == RIFT_ACTION_PATH_HASH) {
        rift_fmt_action(op, rift_app_now(app), text, sizeof(text));
    }
    rift_label_set(v->status, text);
    rift_form_show(v->status, text[0] != '\0');
}

void rift_device_cancel(struct rift_app *app)
{
    struct rift_device *v = of(app);

    if (v) {
        v->rename_open = 0;
        v->rename_error[0] = '\0';
        v->bytes_pending = 0;
        app->refresh_pending = 1;
    }
}

void rift_device_destroy(struct rift_app *app)
{
    struct rift_device *v = of(app);

    if (v) {
        free(v);
        app->device = NULL;
    }
}

lv_obj_t *rift_device_rename_field(const struct rift_app *app)
{
    return of(app) ? of(app)->rename_field : NULL;
}
