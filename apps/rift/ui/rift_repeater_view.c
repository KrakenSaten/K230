/*
 * One repeater's page. See rift_repeater_view.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_repeater_view.h"

#include "pocketui.h"
#include "pos_input.h"
#include "pos_styles.h"
#include "rift_form.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BACK_W 168
#define LOGIN_W 160

static struct rift_repeater_view *of(const struct rift_app *app)
{
    return app ? app->repeater : NULL;
}

static void focus_cursor_later(void *user);

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

static lv_obj_t *dense(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);

    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, RIFT_ROW_H);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/* The key the page is about, or NULL. */
static const char *target(const struct rift_app *a)
{
    const struct rift_repeater *r = &a->model.repeater;

    return r->have_target ? r->target : NULL;
}

/* ---- handlers: each a reader's press ----------------------------------------- */

static void on_back(lv_event_t *e)
{
    rift_app_show_section(lv_event_get_user_data(e), RIFT_SEC_ACTIVITY);
}

/* Overwrite a field's text where LVGL keeps it, then empty it. In password
 * mode lv_textarea_get_text is the real text, LVGL's own buffer. */
static void scrub_field(lv_obj_t *field)
{
    volatile char *t = field ? (volatile char *)lv_textarea_get_text(field) : NULL;

    while (t && *t) {
        *t++ = '\0';
    }
    if (field) {
        lv_textarea_set_text(field, "");
    }
}

/* The one place a login is asked for: the field's text goes into one
 * request and is wiped there (rift_ipc_repeater_login), and the field is
 * emptied whether or not it went. */
static void on_login(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    struct rift_repeater_view *v = of(a);
    char pw[RIFT_REP_PASSWORD_MAX + 1];
    const char *key = target(a);

    if (!v || !key || rift_rep_busy(&a->model.repeater)) {
        return;
    }
    rift_form_typed(v->part[RIFT_REPV_PASSWORD], pw, sizeof(pw));
    scrub_field(v->part[RIFT_REPV_PASSWORD]);
    rift_ipc_repeater_login(&a->ipc, key, pw, sizeof(pw));
    a->refresh_pending = 1;
}

static void on_logout(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    rift_ipc_repeater_logout(&a->ipc);
    a->refresh_pending = 1;
}

static void on_ask(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    struct rift_repeater_view *v = of(a);
    lv_obj_t *button = lv_event_get_current_target_obj(e);
    enum rift_rep_kind kind = RIFT_REP_NONE;

    if (!v || !target(a)) {
        return;
    }
    if (button == v->part[RIFT_REPV_STATUS]) {
        kind = RIFT_REP_STATUS;
    } else if (button == v->part[RIFT_REPV_NEIGHBOURS]) {
        kind = RIFT_REP_NEIGHBOURS;
    } else if (button == v->part[RIFT_REPV_VERSION]) {
        kind = RIFT_REP_OWNER;
    }
    rift_ipc_repeater_ask(&a->ipc, target(a), kind);
    a->refresh_pending = 1;
}

/* Esc in either field gives the keys back to the page (from the timer:
 * focus moved inside an LVGL event does not stick). */
static void on_field_key(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    if (lv_event_get_key(e) == LV_KEY_ESC) {
        a->focus_list_pending = 1;
    }
}

/* ---- build ------------------------------------------------------------------- */

static void build_identity(struct rift_repeater_view *v, lv_obj_t *parent)
{
    struct rift_app *a = v->app;
    lv_obj_t *top = rift_form_row(parent, RIFT_TOUCH_H);
    lv_obj_t *panel;

    v->part[RIFT_REPV_BACK] = rift_action(top, "\xE2\x80\xB9 ACTIVITY", 0, 1, on_back, a);
    lv_obj_set_flex_grow(v->part[RIFT_REPV_BACK], 0);
    lv_obj_set_width(v->part[RIFT_REPV_BACK], BACK_W);
    v->part[RIFT_REPV_TITLE] = rift_cell(top, POS_STYLE_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(v->part[RIFT_REPV_TITLE], 1);

    panel = rift_panel(parent, "REPEATER");
    v->key_value = pocketui_kv_row(panel, "Key", RIFT_UNKNOWN);
    v->heard_value = pocketui_kv_row(panel, "Heard", RIFT_UNKNOWN);
    v->signal_value = pocketui_kv_row(panel, "Signal", RIFT_UNKNOWN);
    v->contact_value = pocketui_kv_row(panel, "Node list", RIFT_UNKNOWN);
}

static void build_login(struct rift_repeater_view *v, lv_obj_t *parent)
{
    struct rift_app *a = v->app;
    lv_obj_t *panel = rift_panel(parent, "LOGIN");
    lv_obj_t *field;

    v->part[RIFT_REPV_LOGIN_STATE] = rift_form_text(panel, POS_STYLE_ROW_TITLE);
    v->login_row = rift_form_row(panel, 64);
    field = rift_form_field(a, v->login_row, "Repeater password", RIFT_REP_PASSWORD_MAX);
    if (field) {
        lv_textarea_set_password_mode(field, true);
        /* Not even the last character is shown: somebody may be watching. */
        lv_textarea_set_password_show_time(field, 0);
        /* The field sits in PocketUI's 100 %-wide wrapper; the wrapper takes
         * the row's room left over, so the button beside it fits. */
        lv_obj_set_width(lv_obj_get_parent(field), 1);
        lv_obj_set_flex_grow(lv_obj_get_parent(field), 1);
        lv_obj_add_event_cb(field, on_login, LV_EVENT_READY, a);
    }
    v->part[RIFT_REPV_PASSWORD] = field;
    v->part[RIFT_REPV_LOGIN] = rift_action(v->login_row, "LOGIN", 1, 1, on_login, a);
    lv_obj_set_flex_grow(v->part[RIFT_REPV_LOGIN], 0);
    lv_obj_set_width(v->part[RIFT_REPV_LOGIN], LOGIN_W);
    v->part[RIFT_REPV_LOGOUT] = rift_action(panel, "LOGOUT", 0, 1, on_logout, a);
    lv_obj_set_flex_grow(v->part[RIFT_REPV_LOGOUT], 0);
    lv_obj_set_width(v->part[RIFT_REPV_LOGOUT], LOGIN_W);
    v->clock_line = rift_form_text(panel, POS_STYLE_CAPTION);
    v->login_hint = rift_form_text(panel, POS_STYLE_CAPTION);
}

static void build_read(struct rift_repeater_view *v, lv_obj_t *parent)
{
    static const char *const keys[RIFT_REPV_STATUS_ROWS] = {
        "Battery", "Uptime", "Airtime", "Noise floor", "Last heard", "Packets",
        "Sent", "Received", "Errors", "Duplicates", "Receive time", "Queue"
    };
    struct rift_app *a = v->app;
    lv_obj_t *panel = rift_panel(parent, "READ");
    lv_obj_t *bar = rift_form_row(panel, RIFT_TOUCH_H);
    int i;

    v->part[RIFT_REPV_STATUS] = rift_action(bar, "STATUS", 0, 1, on_ask, a);
    v->part[RIFT_REPV_NEIGHBOURS] = rift_action(bar, "NEIGHBOURS", 0, 1, on_ask, a);
    v->part[RIFT_REPV_VERSION] = rift_action(bar, "VERSION", 0, 1, on_ask, a);
    v->part[RIFT_REPV_NOTE] = rift_form_text(panel, POS_STYLE_CAPTION);
    for (i = 0; i < RIFT_REPV_STATUS_ROWS; i++) {
        v->status_value[i] = pocketui_kv_row(panel, keys[i], "");
        v->status_row[i] = lv_obj_get_parent(v->status_value[i]);
        lv_obj_add_flag(v->status_row[i], LV_OBJ_FLAG_HIDDEN);
    }
    v->neigh_caption = rift_group_label(panel, "");
    lv_obj_add_flag(v->neigh_caption, LV_OBJ_FLAG_HIDDEN);
    for (i = 0; i < RIFT_REP_NEIGHBOURS_MAX; i++) {
        v->neigh_row[i] = dense(panel);
        v->neigh_name[i] = rift_cell(v->neigh_row[i], POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_flex_grow(v->neigh_name[i], 1);
        v->neigh_snr[i] = rift_cell(v->neigh_row[i], POS_STYLE_CAPTION, 72, LV_TEXT_ALIGN_RIGHT);
        v->neigh_age[i] = rift_cell(v->neigh_row[i], POS_STYLE_CAPTION, 56, LV_TEXT_ALIGN_RIGHT);
        lv_obj_add_flag(v->neigh_row[i], LV_OBJ_FLAG_HIDDEN);
    }
    v->owner_value[0] = pocketui_kv_row(panel, "Firmware", "");
    v->owner_value[1] = pocketui_kv_row(panel, "Its name", "");
    v->owner_value[2] = pocketui_kv_row(panel, "Owner", "");
    for (i = 0; i < 3; i++) {
        v->owner_row[i] = lv_obj_get_parent(v->owner_value[i]);
        lv_obj_add_flag(v->owner_row[i], LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *rift_repeater_view_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_repeater_view *v = calloc(1, sizeof(*v));

    if (!v) {
        return NULL;
    }
    v->app = app;
    v->fields_live = -1;
    v->cmd_live = -1;
    app->repeater = v;

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
    /* Who it is and the login on the left; what it answers and the console
     * on the right. Stacked in portrait in the same order. */
    build_identity(v, v->col[0]);
    build_login(v, v->col[0]);
    build_read(v, v->col[1]);
    rift_repeater_cmd_build(v, v->col[1]);
    {
        int i;

        /* The DS §9 focus outline on whatever the keys are on. */
        for (i = 0; i < RIFT_REPV_LOGIN_STATE; i++) {
            if (v->part[i]) {
                pos_style_add(v->part[i], POS_STYLE_FIELD_FOCUSED, LV_STATE_USER_1);
            }
        }
        if (v->part[RIFT_REPV_PASSWORD]) {
            lv_obj_add_event_cb(v->part[RIFT_REPV_PASSWORD], on_field_key, LV_EVENT_KEY, app);
        }
        if (v->part[RIFT_REPV_COMMAND]) {
            lv_obj_add_event_cb(v->part[RIFT_REPV_COMMAND], on_field_key, LV_EVENT_KEY, app);
        }
    }
    lv_obj_add_flag(v->root, LV_OBJ_FLAG_HIDDEN);
    return v->root;
}

void rift_repeater_view_destroy(struct rift_app *app)
{
    struct rift_repeater_view *v = of(app);

    if (!v) {
        return;
    }
    scrub_field(v->part[RIFT_REPV_PASSWORD]);
    lv_async_call_cancel(focus_cursor_later, app);
    free(v);
    app->repeater = NULL;
}

void rift_repeater_view_shape(struct rift_app *app)
{
    struct rift_repeater_view *v = of(app);
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

void rift_repeater_view_cancel(struct rift_app *app)
{
    struct rift_repeater_view *v = of(app);

    if (!v) {
        return;
    }
    rift_repeater_cmd_cancel(v);
    rift_repeater_cursor_move(&v->cursor, NULL);
    scrub_field(v->part[RIFT_REPV_PASSWORD]);
    rift_form_field_live(v->part[RIFT_REPV_PASSWORD], 0);
    rift_form_field_live(v->part[RIFT_REPV_COMMAND], 0);
    v->fields_live = 0;
    v->cmd_live = 0;
}

void rift_repeater_open(struct rift_app *app, const char *key)
{
    if (!app || !key) {
        return;
    }
    rift_rep_set_target(&app->model.repeater, key);
    /* Ask about the node too: its name and route, as NODES' detail does. */
    rift_ipc_request_node(&app->ipc, key);
    rift_app_show_section(app, RIFT_SEC_REPEATER);
    if (app->repeater) {
        lv_obj_scroll_to_y(app->repeater->root, 0, LV_ANIM_OFF);
    }
}

/* ---- refresh ----------------------------------------------------------------- */

static void fmt_duration(double secs, char *out, size_t len)
{
    unsigned long s = secs > 0 ? (unsigned long)secs : 0;
    unsigned long m = s / 60;
    unsigned long h = m / 60;
    unsigned long d = h / 24;

    if (d > 0) {
        snprintf(out, len, "%lud %luh", d, h % 24);
    } else if (h > 0) {
        snprintf(out, len, "%luh %lum", h, m % 60);
    } else if (m > 0) {
        snprintf(out, len, "%lum %lus", m, s % 60);
    } else {
        snprintf(out, len, "%lus", s);
    }
}

static void show_status(struct rift_repeater_view *v, const struct rift_rep_status *st)
{
    char text[RIFT_TEXT_MAX];
    char a[24];
    int i;

    for (i = 0; i < RIFT_REPV_STATUS_ROWS; i++) {
        rift_form_show(v->status_row[i], st->have);
    }
    if (!st->have) {
        return;
    }
    snprintf(text, sizeof(text), "%.2f V", st->battery_mv / 1000.0);
    rift_label_set(v->status_value[0], text);
    fmt_duration(st->uptime_s, text, sizeof(text));
    rift_label_set(v->status_value[1], text);
    fmt_duration(st->air_time_s, a, sizeof(a));
    if (st->uptime_s > 0 && st->air_time_s <= st->uptime_s) {
        snprintf(text, sizeof(text), "%s" RIFT_SEP "%.1f %% OF UPTIME", a,
                 st->air_time_s * 100.0 / st->uptime_s);
    } else {
        snprintf(text, sizeof(text), "%s", a);
    }
    rift_label_set(v->status_value[2], text);
    snprintf(text, sizeof(text), "%.0f dBm", st->noise_floor_dbm);
    rift_label_set(v->status_value[3], text);
    snprintf(text, sizeof(text), "%.0f dBm" RIFT_SEP "SNR %.1f dB", st->last_rssi_dbm,
             st->last_snr_db);
    rift_label_set(v->status_value[4], text);
    snprintf(text, sizeof(text), "RX %.0f" RIFT_SEP "TX %.0f", st->packets_recv, st->packets_sent);
    rift_label_set(v->status_value[5], text);
    snprintf(text, sizeof(text), "FLOOD %.0f" RIFT_SEP "DIRECT %.0f", st->sent_flood,
             st->sent_direct);
    rift_label_set(v->status_value[6], text);
    snprintf(text, sizeof(text), "FLOOD %.0f" RIFT_SEP "DIRECT %.0f", st->recv_flood,
             st->recv_direct);
    rift_label_set(v->status_value[7], text);
    if (st->have_rx_air) {
        snprintf(text, sizeof(text), "EVENTS %.0f" RIFT_SEP "RX ERRORS %.0f", st->err_events,
                 st->recv_errors);
    } else {
        snprintf(text, sizeof(text), "EVENTS %.0f", st->err_events);
    }
    rift_label_set(v->status_value[8], text);
    /* The later tiers only when this repeater sent them: an older firmware
     * does not, and a 0 here would be a count nobody made. */
    rift_form_show(v->status_row[9], st->have_dups);
    if (st->have_dups) {
        snprintf(text, sizeof(text), "DIRECT %.0f" RIFT_SEP "FLOOD %.0f", st->direct_dups,
                 st->flood_dups);
        rift_label_set(v->status_value[9], text);
    }
    rift_form_show(v->status_row[10], st->have_rx_air);
    if (st->have_rx_air) {
        fmt_duration(st->rx_air_time_s, text, sizeof(text));
        rift_label_set(v->status_value[10], text);
    }
    snprintf(text, sizeof(text), "%.0f TO SEND", st->tx_queue);
    rift_label_set(v->status_value[11], text);
}

static void show_neighbours(struct rift_repeater_view *v, const struct rift_rep_neighbours *nb)
{
    char text[RIFT_NAME_MAX + 16];
    int i;

    rift_form_show(v->neigh_caption, nb->have);
    if (nb->have) {
        snprintf(text, sizeof(text), "ITS NEIGHBOURS" RIFT_SEP "%d OF %d", nb->count, nb->total);
        rift_label_set(v->neigh_caption, text);
    }
    for (i = 0; i < RIFT_REP_NEIGHBOURS_MAX; i++) {
        const struct rift_rep_neighbour *e = &nb->e[i];

        if (!nb->have || i >= nb->count) {
            lv_obj_add_flag(v->neigh_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(v->neigh_row[i], LV_OBJ_FLAG_HIDDEN);
        rift_fmt_snr(e->snr_db, 1, text, sizeof(text));
        rift_label_set(v->neigh_snr[i], text);
        rift_fmt_age((int64_t)(e->heard_s_ago * 1000.0), 1, text, sizeof(text));
        rift_label_set(v->neigh_age[i], text);
        /* Named only when the prefix names one node this node holds; the
         * repeater sent six bytes of a key, not a name. */
        rift_cell_set_text_fit(v->neigh_name[i], e->name[0] ? e->name : e->prefix);
    }
}

static void show_owner(struct rift_repeater_view *v, const struct rift_rep_owner *ow)
{
    const char *text[3];
    int i;

    text[0] = ow->firmware;
    text[1] = ow->name;
    text[2] = ow->owner;
    for (i = 0; i < 3; i++) {
        rift_form_show(v->owner_row[i], ow->have && text[i][0]);
        if (ow->have && text[i][0]) {
            rift_label_set(v->owner_value[i], text[i]);
        }
    }
}

static const char *login_words(const struct rift_repeater *r, const char *key, int other)
{
    if (other) {
        return "Logged in to another repeater";
    }
    if (!r->active || strcmp(r->key, key) != 0) {
        return r->asking == RIFT_REP_LOGIN ? "Logging in" : "Not logged in";
    }
    switch (r->login) {
    case RIFT_REP_LOGIN_WAITING:
        return "Logging in";
    case RIFT_REP_LOGIN_OK:
        return r->legacy ? "Logged in" : (r->admin ? "Logged in as admin" : "Logged in as guest");
    case RIFT_REP_LOGIN_REFUSED:
        return "Login refused";
    case RIFT_REP_LOGIN_TIMEOUT:
        return "No answer to the login";
    case RIFT_REP_LOGIN_NONE:
    default:
        return "Not logged in";
    }
}

static void refresh_identity(struct rift_repeater_view *v, const char *key, int64_t now)
{
    struct rift_app *a = v->app;
    const struct rift_node *node = rift_model_find(&a->model, key);
    const struct rift_rep_found *f = rift_rep_found_by_key(&a->model.repeater, key);
    char text[RIFT_TEXT_MAX];
    char rssi[RIFT_SIGNAL_MAX];
    char snr[RIFT_SIGNAL_MAX];
    char age[RIFT_AGE_MAX];

    if (node && node->name[0]) {
        rift_cell_set_text_fit(v->part[RIFT_REPV_TITLE], node->name);
    } else if (f && f->name[0]) {
        rift_cell_set_text_fit(v->part[RIFT_REPV_TITLE], f->name);
    } else {
        rift_cell_set_text_fit(v->part[RIFT_REPV_TITLE], "Unnamed repeater");
    }
    snprintf(text, sizeof(text), "%.12s\xE2\x80\xA6", key);
    rift_label_set(v->key_value, text);
    if (f) {
        rift_fmt_age(now - f->mono_ms, f->mono_ms > 0, age, sizeof(age));
        snprintf(text, sizeof(text), "%s" RIFT_SEP "%s AGO", f->current ? "LATEST SCAN"
                                                                          : "EARLIER SCAN",
                 age);
        rift_label_set(v->heard_value, text);
        rift_fmt_rssi(f->rssi_dbm, f->have_rssi, rssi, sizeof(rssi));
        rift_fmt_snr(f->snr_db, f->have_snr, snr, sizeof(snr));
        /* Both halves of the link: how this node heard the answer, and how
         * the repeater heard the request. They often differ. */
        snprintf(text, sizeof(text), "US %s %s" RIFT_SEP "IT %.1f dB", rssi, snr, f->their_snr_db);
        rift_label_set(v->signal_value, text);
    } else {
        rift_label_set(v->heard_value, "NOT IN THE SCAN LIST");
        rift_label_set(v->signal_value, RIFT_UNKNOWN);
    }
    rift_label_set(v->contact_value,
                   node ? "HELD" : "NOT HELD");
}

static void refresh_login(struct rift_repeater_view *v, const char *key, int ready)
{
    struct rift_app *a = v->app;
    const struct rift_repeater *r = &a->model.repeater;
    const struct rift_node *node = rift_model_find(&a->model, key);
    int other = r->active && strcmp(r->key, key) != 0;
    int logged = rift_rep_logged_in(r, key);
    int busy = rift_rep_busy(r);
    int can_login = ready && node && !busy && !r->unsupported;
    int live;

    rift_label_set(v->part[RIFT_REPV_LOGIN_STATE], login_words(r, key, other));
    rift_action_set_enabled(v->part[RIFT_REPV_LOGIN], 1, can_login);
    rift_form_show(v->part[RIFT_REPV_LOGOUT], r->active && !other);
    rift_action_set_enabled(v->part[RIFT_REPV_LOGOUT], 0, r->active && !other);
    rift_form_show(v->login_row, !logged);
    /* The password field takes keys only while it can be used, and only on
     * a change (lvgl-layout gotcha 4: re-adding moves the focus). */
    live = can_login && !logged;
    if (live != v->fields_live) {
        rift_form_field_live(v->part[RIFT_REPV_PASSWORD], live);
        v->fields_live = live;
    }
    if (r->active && !other && r->have_clock) {
        struct tm tm;
        time_t t = (time_t)r->repeater_clock;
        char when[40];

        if (gmtime_r(&t, &tm)) {
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", &tm);
            lv_label_set_text_fmt(v->clock_line, "REPEATER CLOCK AT LOGIN %s", when);
            rift_form_show(v->clock_line, 1);
        }
    } else {
        rift_form_show(v->clock_line, 0);
    }
    if (r->unsupported) {
        lv_label_set_text(v->login_hint, "This meshcored has no repeater login.");
    } else if (!node) {
        lv_label_set_text(v->login_hint, "Not in the node list yet: a login needs the repeater's "
                                         "advert to have been heard.");
    } else if (other) {
        lv_label_set_text(v->login_hint, "Logging in here ends the session with the other "
                                         "repeater.");
    } else if (logged) {
        lv_label_set_text(v->login_hint, "LOGOUT forgets the session here; MeshCore sends "
                                         "nothing for it. Leaving RIFT logs out too.");
    } else {
        lv_label_set_text(v->login_hint, "A wrong password is not answered at all: the login "
                                         "then ends as no answer. Nothing typed is kept.");
    }
}

void rift_repeater_view_refresh(struct rift_app *app)
{
    struct rift_repeater_view *v = of(app);
    const struct rift_repeater *r;
    const char *key;
    int ready;
    int logged;
    int can_ask;

    if (!v) {
        return;
    }
    r = &app->model.repeater;
    key = target(app);
    if (!key) {
        rift_cell_set_text_fit(v->part[RIFT_REPV_TITLE], "No repeater chosen");
        return;
    }
    ready = rift_form_service_ready(app) && rift_ipc_connected(&app->ipc);
    logged = rift_rep_logged_in(r, key);
    can_ask = ready && logged && !rift_rep_busy(r);
    refresh_identity(v, key, rift_app_now(app));
    refresh_login(v, key, ready);

    rift_action_set_enabled(v->part[RIFT_REPV_STATUS], 0, can_ask);
    rift_action_set_enabled(v->part[RIFT_REPV_NEIGHBOURS], 0, can_ask);
    rift_action_set_enabled(v->part[RIFT_REPV_VERSION], 0, can_ask);
    if (r->note[0]) {
        rift_label_set(v->part[RIFT_REPV_NOTE], r->note);
    } else {
        rift_label_set(v->part[RIFT_REPV_NOTE], logged ? "Ask the repeater what it knows."
                                                       : "Log in to read the repeater.");
    }
    /* Only what this repeater answered in this session is shown. */
    if (logged || (r->active && strcmp(r->key, key) == 0)) {
        show_status(v, &r->status);
        show_neighbours(v, &r->neighbours);
        show_owner(v, &r->owner);
    } else {
        struct rift_rep_status none_s;
        struct rift_rep_neighbours none_n;
        struct rift_rep_owner none_o;

        memset(&none_s, 0, sizeof(none_s));
        memset(&none_n, 0, sizeof(none_n));
        memset(&none_o, 0, sizeof(none_o));
        show_status(v, &none_s);
        show_neighbours(v, &none_n);
        show_owner(v, &none_o);
    }
    rift_repeater_cmd_refresh(v, ready && logged);
}

lv_obj_t *rift_repeater_view_part(const struct rift_app *app, enum rift_repv_part part)
{
    return (of(app) && part >= 0 && part < RIFT_REPV_PART_COUNT) ? of(app)->part[part] : NULL;
}

/* ---- keys -------------------------------------------------------------------- */

void rift_repeater_cursor_move(lv_obj_t **cursor, lv_obj_t *to)
{
    if (*cursor == to) {
        return;
    }
    if (*cursor) {
        lv_obj_remove_state(*cursor, LV_STATE_USER_1);
    }
    *cursor = to;
    if (to) {
        lv_obj_add_state(to, LV_STATE_USER_1);
        lv_obj_scroll_to_view_recursive(to, LV_ANIM_OFF);
    }
}

/* The deferred half of Enter on a field: whatever field the keys are on
 * by then, if the page is still there. */
static void focus_cursor_later(void *user)
{
    struct rift_app *a = user;
    struct rift_repeater_view *v = of(a);

    if (v && v->cursor &&
        (v->cursor == v->part[RIFT_REPV_PASSWORD] || v->cursor == v->part[RIFT_REPV_COMMAND])) {
        pos_input_focus(v->cursor);
    }
}

/* Shown, and not disabled, up to the page's root. */
static int reachable(lv_obj_t *o, lv_obj_t *root)
{
    lv_obj_t *p;

    if (!o || lv_obj_has_state(o, LV_STATE_DISABLED)) {
        return 0;
    }
    for (p = o; p && p != root; p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN)) {
            return 0;
        }
    }
    return 1;
}

int rift_repeater_key(struct rift_app *app, uint32_t key)
{
    static const enum rift_repv_part order[] = {
        RIFT_REPV_BACK,       RIFT_REPV_PASSWORD,   RIFT_REPV_LOGIN,
        RIFT_REPV_LOGOUT,     RIFT_REPV_STATUS,     RIFT_REPV_NEIGHBOURS,
        RIFT_REPV_VERSION,    RIFT_REPV_COMMAND,    RIFT_REPV_SEND,
        RIFT_REPV_QUICK_VER,  RIFT_REPV_QUICK_CLOCK, RIFT_REPV_QUICK_NEIGHBORS,
        RIFT_REPV_CANCEL,     RIFT_REPV_CONFIRM,
    };
    struct rift_repeater_view *v = of(app);
    lv_obj_t *items[sizeof(order) / sizeof(order[0])];
    int n = 0;
    int at = -1;
    size_t i;

    if (!app) {
        return 0;
    }
    if (app->section == RIFT_SEC_ACTIVITY) {
        return rift_scan_key(app, key);
    }
    if (app->section != RIFT_SEC_REPEATER || !v ||
        (key != LV_KEY_UP && key != LV_KEY_DOWN && key != LV_KEY_ENTER)) {
        return 0;
    }
    for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        lv_obj_t *o = v->part[order[i]];

        /* A field counts while it can take keys at all. */
        if ((order[i] == RIFT_REPV_PASSWORD && v->fields_live != 1) ||
            (order[i] == RIFT_REPV_COMMAND && v->cmd_live != 1) || !reachable(o, v->root)) {
            continue;
        }
        if (o == v->cursor) {
            at = n;
        }
        items[n++] = o;
    }
    if (n == 0) {
        return 1;
    }
    if (key == LV_KEY_ENTER) {
        if (at < 0) {
            return 0;
        }
        if (v->cursor == v->part[RIFT_REPV_PASSWORD] || v->cursor == v->part[RIFT_REPV_COMMAND]) {
            /* Into the field: typing goes there, Enter in it sends, Esc
             * comes back (on_field_key). Not from inside this key event -
             * group focus moved there does not stick (lvgl-layout gotcha
             * 3) - but on LVGL's next pass. */
            lv_async_call(focus_cursor_later, app);
        } else {
            lv_obj_send_event(v->cursor, LV_EVENT_CLICKED, NULL);
        }
        return 1;
    }
    if (at < 0) {
        at = key == LV_KEY_DOWN ? 0 : n - 1;
    } else if (key == LV_KEY_DOWN && at + 1 < n) {
        at++;
    } else if (key == LV_KEY_UP && at > 0) {
        at--;
    }
    rift_repeater_cursor_move(&v->cursor, items[at]);
    return 1;
}
