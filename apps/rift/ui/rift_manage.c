/*
 * ACTIVITY's CHANNELS panel. See rift_manage.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_manage.h"

#include "app.h"
#include "pocketui.h"
#include "pos_styles.h"
#include "rift_form.h"
#include "rift_keys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEAVE_W 120

struct chan_row {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *meta;
    lv_obj_t *leave;
    int slot;
    char conv[RIFT_KEY_HEX]; /* the channel this row is, as COMMS keys it */
    char label[RIFT_CHANNEL_NAME_MAX];
};

struct rift_manage {
    struct rift_app *app;
    lv_obj_t *caption;
    lv_obj_t *empty;
    struct chan_row row[RIFT_MAX_CHANNELS];

    /* LEAVE's confirmation, for one channel at a time. */
    lv_obj_t *confirm;
    lv_obj_t *confirm_title;
    int confirming;
    int confirm_slot;
    char confirm_conv[RIFT_KEY_HEX];
    char confirm_label[RIFT_CHANNEL_NAME_MAX];

    /* ADD CHANNEL, and the form it opens in place. */
    lv_obj_t *add_bar;
    lv_obj_t *add;
    lv_obj_t *form;
    lv_obj_t *kind_btn[3];
    int kind;
    int kind_drawn; /* -1 not yet */
    lv_obj_t *kind_note;
    lv_obj_t *name_field;
    lv_obj_t *key_field;
    lv_obj_t *form_status;
    int form_open;
    char form_error[RIFT_TEXT_MAX + 32];
    /* The fields to be emptied on the next refresh: rift_manage_cancel may
     * run inside a layout pass, where no widget is to be changed. */
    int clear_fields;
    /* A join written and not yet answered, and whether its key was made here. */
    int add_pending;
    int add_private;

    /* A private channel's key, made here and joined: shown once so it can be
     * shared, and forgotten when the reader is done with it or goes. */
    lv_obj_t *share;
    lv_obj_t *share_title;
    lv_obj_t *share_key;
    char shared_key[RIFT_KEY_B64_MAX];

    lv_obj_t *status;
};

static struct rift_manage *of(const struct rift_app *app)
{
    return app ? app->manage : NULL;
}

static int busy(const struct rift_app *a)
{
    return a->model.manage_op.active;
}

/* ---- LEAVE ------------------------------------------------------------------ */

static void on_leave(lv_event_t *e)
{
    struct rift_manage *v = of(lv_event_get_user_data(e));
    lv_obj_t *btn = lv_event_get_target_obj(e);
    int i;

    /* LEAVE only asks. Nothing is sent until the confirmation is pressed. */
    for (i = 0; v && i < RIFT_MAX_CHANNELS; i++) {
        if (v->row[i].leave == btn && v->row[i].slot >= 0) {
            v->confirming = 1;
            v->confirm_slot = v->row[i].slot;
            snprintf(v->confirm_conv, sizeof(v->confirm_conv), "%s", v->row[i].conv);
            snprintf(v->confirm_label, sizeof(v->confirm_label), "%s", v->row[i].label);
            v->app->refresh_pending = 1;
            return;
        }
    }
}

static void on_leave_cancel(lv_event_t *e)
{
    struct rift_manage *v = of(lv_event_get_user_data(e));

    if (v) {
        v->confirming = 0;
        v->app->refresh_pending = 1;
    }
}

/* The one place a channel is left from: the confirmation, for the channel it
 * was asked about, and only while that channel is still the one in its slot
 * - a slot taken by another channel since would be the wrong key forgotten. */
static void on_leave_confirm(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    struct rift_manage *v = of(a);
    const struct rift_channel *ch;

    if (!v || !v->confirming) {
        return;
    }
    ch = rift_model_key_channel(&a->model, v->confirm_conv);
    if (ch && ch->slot == v->confirm_slot) {
        rift_ipc_channel_remove(&a->ipc, v->confirm_slot, v->confirm_label);
    }
    v->confirming = 0;
    a->refresh_pending = 1;
}

/* ---- ADD CHANNEL ------------------------------------------------------------ */

static void on_share_done(lv_event_t *e)
{
    struct rift_manage *v = of(lv_event_get_user_data(e));

    if (v) {
        memset(v->shared_key, 0, sizeof(v->shared_key));
        v->app->refresh_pending = 1;
    }
}

static void open_form(struct rift_manage *v, int open)
{
    v->form_open = open;
    v->form_error[0] = '\0';
    if (open) {
        v->kind = RIFT_CHANNEL_HASHTAG;
    }
    /* Emptied either way: a key typed into a closed form is a key left
     * lying about. */
    v->clear_fields = 1;
}

static void on_add_open(lv_event_t *e)
{
    struct rift_manage *v = of(lv_event_get_user_data(e));

    if (v) {
        memset(v->shared_key, 0, sizeof(v->shared_key));
        open_form(v, 1);
        v->app->refresh_pending = 1;
    }
}

static void on_add_cancel(lv_event_t *e)
{
    struct rift_manage *v = of(lv_event_get_user_data(e));

    if (v) {
        open_form(v, 0);
        if (pocketos_shell_keyboard_visible()) {
            pocketos_shell_keyboard_hide();
        }
        v->app->refresh_pending = 1;
    }
}

static void on_kind(lv_event_t *e)
{
    struct rift_manage *v = of(lv_event_get_user_data(e));
    lv_obj_t *btn = lv_event_get_target_obj(e);
    int k;

    for (k = 0; v && k < 3; k++) {
        if (v->kind_btn[k] == btn) {
            v->kind = k;
            v->form_error[0] = '\0';
            v->app->refresh_pending = 1;
        }
    }
}

/* The name and the key, checked here before anything is asked. Returns 0 with
 * both written, or -1 with the reason in the form. */
static int name_and_key(struct rift_manage *v, char *name, size_t name_len, char *key,
                        size_t key_len)
{
    char typed[4 * RIFT_CHANNEL_NAME_MAX];
    char why[RIFT_TEXT_MAX];
    size_t start = 0;
    size_t len;

    rift_form_typed(v->name_field, typed, sizeof(typed));
    if (v->kind == RIFT_CHANNEL_HASHTAG) {
        if (rift_hashtag_name(typed, name, name_len, why, sizeof(why)) != 0 ||
            rift_hashtag_key(name, key, key_len) != 0) {
            snprintf(v->form_error, sizeof(v->form_error), "%s", why);
            return -1;
        }
        return 0;
    }
    while (typed[start] == ' ') {
        start++;
    }
    len = strlen(typed + start);
    while (len > 0 && typed[start + len - 1] == ' ') {
        len--;
    }
    typed[start + len] = '\0';
    if (len >= name_len) {
        snprintf(v->form_error, sizeof(v->form_error), "That name is longer than 31 bytes.");
        return -1;
    }
    if (rift_channel_name_check(typed + start, why, sizeof(why)) != 0) {
        snprintf(v->form_error, sizeof(v->form_error), "%s", why);
        return -1;
    }
    memcpy(name, typed + start, len + 1);
    if (v->kind == RIFT_CHANNEL_PRIVATE) {
        if (rift_random_key(key, key_len) != 0) {
            snprintf(v->form_error, sizeof(v->form_error),
                     "No random source to make a key with; nothing was joined.");
            return -1;
        }
        return 0;
    }
    {
        char pasted[4 * RIFT_KEY_B64_MAX];
        int rc;

        rift_form_typed(v->key_field, pasted, sizeof(pasted));
        rc = rift_key_check(pasted, key, key_len, why, sizeof(why));
        memset(pasted, 0, sizeof(pasted));
        if (rc != 0) {
            snprintf(v->form_error, sizeof(v->form_error), "%s", why);
            return -1;
        }
    }
    return 0;
}

/* JOIN: one request, and the key is wiped from this function's buffer as soon
 * as it is written. The service checks again and is the one that keeps it. */
static void on_join(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    struct rift_manage *v = of(a);
    const struct rift_model *m = &a->model;
    char name[RIFT_CHANNEL_NAME_MAX];
    char key[RIFT_KEY_B64_MAX];
    int i;

    if (!v) {
        return;
    }
    v->form_error[0] = '\0';
    memset(key, 0, sizeof(key));
    a->refresh_pending = 1;
    if (name_and_key(v, name, sizeof(name), key, sizeof(key)) != 0) {
        memset(key, 0, sizeof(key));
        return;
    }
    /* A name already joined: the service would take a different key under
     * it, and COMMS would then list two channels nobody could tell apart. */
    for (i = 0; i < m->channel_count; i++) {
        if (m->channels[i].have_name && strcmp(m->channels[i].name, name) == 0) {
            snprintf(v->form_error, sizeof(v->form_error),
                     "A channel called %s is joined already (slot %d).", name,
                     m->channels[i].slot);
            memset(key, 0, sizeof(key));
            return;
        }
    }
    if (m->have_channels_reported && m->channels_max > 0 &&
        m->channels_reported >= m->channels_max) {
        snprintf(v->form_error, sizeof(v->form_error),
                 "All %d channel slots are taken: leave one first.", m->channels_max);
        memset(key, 0, sizeof(key));
        return;
    }
    if (rift_ipc_channel_add(&a->ipc, name, key) == 0) {
        v->add_pending = 1;
        v->add_private = v->kind == RIFT_CHANNEL_PRIVATE;
        /* Kept only to be shown once it is joined, and only when it was made
         * here: a key the reader pasted is one they already have. */
        if (v->add_private) {
            snprintf(v->shared_key, sizeof(v->shared_key), "%s", key);
        }
    } else {
        snprintf(v->form_error, sizeof(v->form_error), "%s",
                 m->manage_op.failed ? m->manage_op.error : "Nothing was asked.");
    }
    memset(key, 0, sizeof(key));
    if (v->key_field) {
        lv_textarea_set_text(v->key_field, "");
    }
}

/* ---- building ------------------------------------------------------------- */

void rift_manage_build_channels(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_manage *v = calloc(1, sizeof(*v));
    lv_obj_t *panel;
    lv_obj_t *bar;
    int i;

    if (!v) {
        return;
    }
    v->app = app;
    v->kind_drawn = -1;
    app->manage = v;

    panel = rift_panel(parent, "CHANNELS");
    v->caption = rift_form_text(panel, POS_STYLE_CAPTION);
    v->empty = rift_form_text(panel, POS_STYLE_TEXT_MUTED);
    for (i = 0; i < RIFT_MAX_CHANNELS; i++) {
        struct chan_row *r = &v->row[i];
        lv_obj_t *words;

        r->slot = -1;
        r->row = rift_form_row(panel, RIFT_TOUCH_H);
        words = rift_form_column(r->row);
        lv_obj_set_style_pad_row(words, 0, 0);
        lv_obj_set_flex_grow(words, 1);
        lv_obj_set_width(words, 1);
        r->name = rift_cell(words, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(r->name, LV_PCT(100));
        r->meta = rift_cell(words, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(r->meta, LV_PCT(100));
        r->leave = rift_action(r->row, "LEAVE", 0, 1, on_leave, app);
        lv_obj_set_flex_grow(r->leave, 0);
        lv_obj_set_width(r->leave, LEAVE_W);
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    }

    /* The confirmation (DS §17.5): what it costs, Cancel first and accented. */
    v->confirm = rift_form_column(panel);
    v->confirm_title = rift_form_text(v->confirm, POS_STYLE_TITLE);
    lv_label_set_text(rift_form_text(v->confirm, POS_STYLE_TEXT_SECONDARY),
                      "This device forgets the key. Nothing on the air gives it back: to join "
                      "again you need the key, or the name of a hashtag channel. Messages "
                      "already received stay in COMMS.");
    bar = rift_form_row(v->confirm, RIFT_TOUCH_H);
    rift_action(bar, "CANCEL", 1, 1, on_leave_cancel, app);
    rift_action(bar, "LEAVE", 0, 1, on_leave_confirm, app);
    lv_obj_add_flag(v->confirm, LV_OBJ_FLAG_HIDDEN);

    /* A private channel's key, once, to be shared. */
    v->share = rift_form_column(panel);
    v->share_title = rift_form_text(v->share, POS_STYLE_CAPTION);
    v->share_key = rift_form_text(v->share, POS_STYLE_VALUE);
    lv_label_set_text(rift_form_text(v->share, POS_STYLE_TEXT_SECONDARY),
                      "Give this key to whoever should read the channel. It is shown once: "
                      "this device keeps it only inside the radio service.");
    bar = rift_form_row(v->share, RIFT_TOUCH_H);
    rift_action(bar, "DONE", 1, 1, on_share_done, app);
    lv_obj_add_flag(v->share, LV_OBJ_FLAG_HIDDEN);

    v->add_bar = rift_form_row(panel, RIFT_TOUCH_H);
    v->add = rift_action(v->add_bar, "ADD CHANNEL", 0, 1, on_add_open, app);

    /* The form, in place: no new screen and no modal. */
    v->form = rift_form_column(panel);
    bar = rift_form_row(v->form, RIFT_TOUCH_H);
    v->kind_btn[RIFT_CHANNEL_HASHTAG] = rift_action(bar, "HASHTAG", 0, 1, on_kind, app);
    v->kind_btn[RIFT_CHANNEL_PRIVATE] = rift_action(bar, "PRIVATE", 0, 1, on_kind, app);
    v->kind_btn[RIFT_CHANNEL_KEY] = rift_action(bar, "KEY", 0, 1, on_kind, app);
    v->kind_note = rift_form_text(v->form, POS_STYLE_CAPTION);
    v->name_field = rift_form_field(app, v->form, "Channel name", RIFT_CHANNEL_NAME_BYTES);
    v->key_field = rift_form_field(app, v->form, "Key: base64 of 16 or 32 bytes", 60);
    v->form_status = rift_form_text(v->form, POS_STYLE_STATUS_WARN_TEXT);
    bar = rift_form_row(v->form, RIFT_TOUCH_H);
    rift_action(bar, "CANCEL", 0, 1, on_add_cancel, app);
    rift_action(bar, "JOIN", 1, 1, on_join, app);
    lv_obj_add_flag(v->form, LV_OBJ_FLAG_HIDDEN);

    v->status = rift_form_text(panel, POS_STYLE_CAPTION);
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_rows(struct rift_manage *v, int can)
{
    const struct rift_model *m = &v->app->model;
    char text[RIFT_ACTION_TEXT_MAX];
    int i;

    for (i = 0; i < RIFT_MAX_CHANNELS; i++) {
        struct chan_row *r = &v->row[i];
        const struct rift_channel *ch = i < m->channel_count ? &m->channels[i] : NULL;

        if (!ch) {
            r->slot = -1;
            r->conv[0] = '\0';
            rift_form_show(r->row, 0);
            continue;
        }
        r->slot = ch->slot;
        rift_channel_conv_key(ch, r->conv, sizeof(r->conv));
        snprintf(r->label, sizeof(r->label), "%s", ch->have_name ? ch->name : "");
        if (ch->have_name && ch->name[0]) {
            rift_cell_set_text_fit(r->name, ch->name);
        } else {
            snprintf(text, sizeof(text), "CHANNEL %d", ch->slot);
            rift_cell_set_text_fit(r->name, text);
        }
        snprintf(text, sizeof(text), "SLOT %d" RIFT_SEP "HASH %s", ch->slot,
                 ch->have_hash ? ch->hash : RIFT_UNKNOWN);
        if (ch->have_key_bits) {
            size_t at = strlen(text);

            snprintf(text + at, sizeof(text) - at, RIFT_SEP "%d-BIT", ch->key_bits);
        }
        rift_label_set(r->meta, text);
        rift_action_set_enabled(r->leave, 0, can && !v->confirming);
        rift_form_show(r->row, 1);
    }
}

static void refresh_form(struct rift_manage *v, int ready)
{
    static const char *const notes[3] = {
        "A public topic. The key is made from the name, so anyone who knows the name can "
        "read it. A # is added if you leave it out.",
        "A new random key, made on this device. Nobody else can read the channel until you "
        "give them the key, which is shown once it is joined.",
        "Join a channel somebody shared: its name here is yours to choose, the key is theirs.",
    };
    int k;

    rift_form_show(v->form, v->form_open);
    if (!v->form_open) {
        return;
    }
    if (v->kind != v->kind_drawn) {
        for (k = 0; k < 3; k++) {
            rift_form_chosen(v->kind_btn[k], k == v->kind);
        }
        v->kind_drawn = v->kind;
    }
    rift_label_set(v->kind_note, notes[v->kind]);
    if (v->key_field) {
        rift_form_show(lv_obj_get_parent(v->key_field), v->kind == RIFT_CHANNEL_KEY);
    }
    if (v->form_error[0]) {
        rift_label_set(v->form_status, v->form_error);
    } else if (v->add_pending) {
        rift_label_set(v->form_status, "Joining\xE2\x80\xA6");
    } else if (!ready) {
        rift_label_set(v->form_status, "meshcored is not answering; nothing can be joined.");
    } else {
        rift_label_set(v->form_status, "");
    }
    rift_form_show(v->form_status, lv_label_get_text(v->form_status)[0] != '\0');
}

void rift_manage_refresh(struct rift_app *app)
{
    struct rift_manage *v = of(app);
    const struct rift_model *m;
    const struct rift_action_state *op;
    char text[RIFT_ACTION_TEXT_MAX];
    int ready;
    int max;

    if (!v) {
        return;
    }
    m = &app->model;
    op = &m->manage_op;
    ready = rift_form_service_ready(app) && m->channels_valid;
    max = m->channels_max > 0 ? m->channels_max : RIFT_MAX_CHANNELS;

    if (v->clear_fields) {
        v->clear_fields = 0;
        if (v->name_field) {
            lv_textarea_set_text(v->name_field, "");
        }
        if (v->key_field) {
            lv_textarea_set_text(v->key_field, "");
        }
    }
    /* A join that has been answered: closed and, for a key made here, the
     * key shown to be shared; refused, and the form says why and stays. */
    if (v->add_pending && !op->active) {
        v->add_pending = 0;
        if (op->kind == RIFT_ACTION_CHANNEL_ADD && op->done) {
            open_form(v, 0);
            if (!v->add_private) {
                memset(v->shared_key, 0, sizeof(v->shared_key));
            }
        } else {
            memset(v->shared_key, 0, sizeof(v->shared_key));
            snprintf(v->form_error, sizeof(v->form_error), "%s: %s",
                     op->unknown ? "No answer" : "Not joined", op->error);
        }
    }

    if (!m->channels_valid) {
        rift_label_set(v->caption, "");
        rift_label_set(v->empty, m->stale ? "meshcored is not answering."
                                          : "Waiting for meshcored.");
    } else {
        /* True of every channel, so said once: this service sends a channel
         * message as an unscoped flood, and has no flood scopes to choose. */
        snprintf(text, sizeof(text), "%d OF %d SLOTS" RIFT_SEP "UNSCOPED FLOOD",
                 m->channel_count, max);
        rift_label_set(v->caption, text);
        rift_label_set(v->empty, m->channel_count == 0 ? "No channel joined." : "");
    }
    rift_form_show(v->empty, lv_label_get_text(v->empty)[0] != '\0');
    rift_form_show(v->caption, lv_label_get_text(v->caption)[0] != '\0');
    refresh_rows(v, ready && !busy(app));

    /* A confirmation belongs to the channel it was asked about. */
    if (v->confirming) {
        const struct rift_channel *ch = rift_model_key_channel(m, v->confirm_conv);

        if (!ch || ch->slot != v->confirm_slot) {
            v->confirming = 0;
        }
    }
    rift_form_show(v->confirm, v->confirming);
    if (v->confirming) {
        snprintf(text, sizeof(text), "Leave %s?",
                 v->confirm_label[0] ? v->confirm_label : "this channel");
        rift_label_set(v->confirm_title, text);
    }

    if (v->shared_key[0] && !v->add_pending) {
        snprintf(text, sizeof(text), "KEY TO SHARE" RIFT_SEP "%s", op->label);
        rift_label_set(v->share_title, text);
        rift_label_set(v->share_key, v->shared_key);
        rift_form_show(v->share, 1);
    } else {
        /* Not left in the label of a hidden panel either. */
        if (lv_label_get_text(v->share_key)[0]) {
            lv_label_set_text(v->share_key, "");
        }
        rift_form_show(v->share, 0);
    }

    rift_form_show(v->add_bar, !v->form_open && !v->confirming);
    rift_action_set_enabled(v->add, 0, ready && !busy(app) && m->channel_count < max);
    refresh_form(v, ready);

    /* What became of the last join or leave, when the form is not saying it. */
    text[0] = '\0';
    if (!v->form_open &&
        (op->kind == RIFT_ACTION_CHANNEL_ADD || op->kind == RIFT_ACTION_CHANNEL_REMOVE)) {
        rift_fmt_action(op, rift_app_now(app), text, sizeof(text));
    }
    rift_label_set(v->status, text);
    rift_form_show(v->status, text[0] != '\0');
}

void rift_manage_cancel(struct rift_app *app)
{
    struct rift_manage *v = of(app);

    if (!v) {
        return;
    }
    /* Flags and memory only: this can run inside LVGL's layout pass (a turn
     * of the panel), where no widget is to be changed. The next refresh
     * empties the fields and the shown key's label. */
    v->confirming = 0;
    v->form_open = 0;
    v->form_error[0] = '\0';
    v->clear_fields = 1;
    memset(v->shared_key, 0, sizeof(v->shared_key));
    app->refresh_pending = 1;
}

void rift_manage_destroy(struct rift_app *app)
{
    struct rift_manage *v = of(app);

    if (!v) {
        return;
    }
    /* The key shown for sharing goes with the app, wiped rather than freed
     * with it still in the block. The objects are the section's. */
    memset(v->shared_key, 0, sizeof(v->shared_key));
    free(v);
    app->manage = NULL;
}

lv_obj_t *rift_manage_name_field(const struct rift_app *app)
{
    return of(app) ? of(app)->name_field : NULL;
}

lv_obj_t *rift_manage_key_field(const struct rift_app *app)
{
    return of(app) ? of(app)->key_field : NULL;
}
