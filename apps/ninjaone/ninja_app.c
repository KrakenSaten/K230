/* Unofficial NinjaOne read-only dashboard. The helper owns HTTP and JSON.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "app.h"
#include "ninja_session.h"
#include "pocketui.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
extern const lv_image_dsc_t pos_app_icon_ninjaone;
#define PAGE_ROWS 20
#define FAVORITES 12
struct ninja_app {
    lv_obj_t *frame, *nav, *status, *freshness, *search, *severity, *severity_cap, *body;
    lv_timer_t *timer;
    struct pocketui_layout_guard guard;
    struct ninja_session session;
    struct ninja_snapshot data;
    int tab, org, device, page, favorites[FAVORITES], nfavorites;
    int error, failures;
    int64_t next, last_ms;
    bool loading, renewing, forgetting, dirty, refresh, only_favorites, stale;
    int64_t expires;
    char profile[25], favorite_key[32];
    const char *fake;
    char query[80], severity_filter[64], notice[80];
};
static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static bool favorite(struct ninja_app *a, int id)
{
    for (int i = 0; i < a->nfavorites; i++) if (a->favorites[i] == id) return true;
    return false;
}
static void save_favorites(struct ninja_app *a, int id)
{
    if (!a->favorite_key[0]) { snprintf(a->notice, sizeof(a->notice), "Sign in with a tenant identifier to save favorites"); return; }
    int items[FAVORITES], count = 0;
    bool removing = favorite(a, id);
    char text[128] = "";
    size_t used = 0;
    for (int i = 0; i < a->nfavorites; i++) if (a->favorites[i] != id) items[count++] = a->favorites[i];
    if (!removing) {
        if (count == FAVORITES) { snprintf(a->notice, sizeof(a->notice), "Favorite limit: 12 organizations"); return; }
        items[count++] = id;
    }
    for (int i = 0; i < count; i++) {
        int n = snprintf(text + used, sizeof(text) - used, "%s%d", i ? "," : "", items[i]);
        if (n < 0 || (size_t)n >= sizeof(text) - used) return;
        used += (size_t)n;
    }
    if (settings_set(a->favorite_key, text)) {
        snprintf(a->notice, sizeof(a->notice), "Favorites could not be saved"); return;
    }
    memcpy(a->favorites, items, (size_t)count * sizeof(int)); a->nfavorites = count;
    a->notice[0] = 0;
}
static void select_profile(struct ninja_app *a, const char *profile)
{
    if (!strcmp(a->profile, profile)) return;
    /* A tenant switch discards old data and selection, even on failure. */
    memset(&a->data, 0, sizeof(a->data)); a->last_ms = 0;
    a->org = a->device = a->page = a->nfavorites = 0; a->only_favorites = false;
    strcpy(a->profile, profile); a->favorite_key[0] = 0;
    if (a->profile[0]) {
        snprintf(a->favorite_key, sizeof(a->favorite_key), "n1_%c_%s", a->fake ? 'd' : 'l', a->profile);
        const char *f = settings_get(a->favorite_key, "");
        while (*f && a->nfavorites < FAVORITES) {
            char *end; long id = strtol(f, &end, 10);
            if (end == f || id <= 0 || id > INT32_MAX || (*end && *end != ',')) break;
            a->favorites[a->nfavorites++] = (int)id; f = *end ? end + 1 : end;
        }
    }
}
static void action(lv_event_t *e)
{
    struct ninja_app *a = lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_target(e);
    int code = (int)(intptr_t)lv_obj_get_user_data(o);
    if (code < 0) { a->device = -code; a->dirty = true; return; }
    switch (code) {
    case 0: case 1: case 2: a->tab = code; a->device = 0; a->page = 0; break;
    case 3: a->refresh = true; break;
    case 4: a->device = 0; break;
    case 5: a->org = 0; a->page = 0; a->device = 0; break;
    case 6: save_favorites(a, a->org); break;
    case 7: a->only_favorites = !a->only_favorites; a->page = 0; break;
    case 8: if (a->page) a->page--; break;
    case 9: a->page++; break;
    case 10:
        a->refresh = false; a->renewing = false; a->stale = a->data.updated != 0;
        a->forgetting = true; a->loading = true; a->error = NINJA_DISCONNECTED;
        if (ninja_session_start_action(&a->session, now_ms(), a->fake, "forget")) {
            a->loading = false; a->renewing = false; a->error = NINJA_HELPER;
        }
        break;
    }
    a->dirty = true;
}
static void organization(lv_event_t *e)
{
    struct ninja_app *a = lv_event_get_user_data(e);
    a->org = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    a->tab = 2; a->device = 0; a->page = 0; a->dirty = true;
}
static lv_obj_t *label(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *o = pocketui_label(parent, text, role);
    lv_obj_set_width(o, LV_PCT(100));
    lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
    return o;
}
static lv_obj_t *button(struct ninja_app *a, lv_obj_t *parent, const char *text, int code)
{
    lv_obj_t *b = pocketui_button(parent, text, action, a);
    lv_obj_set_user_data(b, (void *)(intptr_t)code);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(b, POCKETUI_TOUCH_MIN, 0);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    lv_obj_set_width(l, LV_PCT(100)); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    pos_input_add_obj(b); return b;
}
static void search_event(lv_event_t *e)
{
    struct ninja_app *a = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        if (lv_event_get_target(e) == a->search)
            snprintf(a->query, sizeof(a->query), "%s", lv_textarea_get_text(a->search));
        else snprintf(a->severity_filter, sizeof(a->severity_filter), "%s", lv_textarea_get_text(a->severity));
        a->page = 0; a->dirty = true;
    } else if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        pos_input_focus(lv_event_get_target(e));
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, NULL);
    }
}
static void freshness(struct ninja_app *a)
{
    char text[250], when[64];
    if (!a->data.updated) snprintf(text, sizeof(text), "No successful update");
    else {
        time_t t = (time_t)a->data.updated;
        struct tm tm;
        localtime_r(&t, &tm); strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
        snprintf(text, sizeof(text), "%sLast success: %s%s", a->stale ? "STALE / " : "", when,
                 a->data.partial ? " / Limited snapshot" : "");
    }
    lv_label_set_text(a->freshness, text);
    snprintf(text, sizeof(text), "%s%s%s%s", a->fake ? "DEMO / synthetic / " : "",
             a->forgetting && a->loading ? "Forgetting credentials..." :
             a->forgetting && a->error != NINJA_DISCONNECTED ? "Credentials could not be forgotten: try Disconnect again" :
             a->renewing ? "Renewing sign-in... / monitoring data is stale" :
             a->loading ? "Refreshing monitoring data..." : ninja_error_text(a->error),
             a->notice[0] ? " / " : "", a->notice);
    lv_label_set_text(a->status, text);
}
static void draw(struct ninja_app *a)
{
    char text[500];
    unsigned shown = 0, matching = 0;
    lv_obj_clean(a->body);
    lv_obj_set_flag(a->search, LV_OBJ_FLAG_HIDDEN, a->tab != 1 || a->device != 0);
    lv_obj_set_flag(a->severity, LV_OBJ_FLAG_HIDDEN, a->tab != 2 || a->device != 0);
    lv_obj_set_flag(a->severity_cap, LV_OBJ_FLAG_HIDDEN, a->tab != 2 || a->device != 0);
    freshness(a);
    if (!a->data.updated) {
        label(a->body, a->loading ? "Waiting for the first snapshot" : "Set up read-only access on your trusted workstation, then refresh.", POS_STYLE_TEXT_SECONDARY);
        return;
    }
    if (a->device) {
        const struct ninja_device *d = ninja_device_find(&a->data, a->device);
        button(a, a->body, "Back to list", 4);
        if (d) {
            label(a->body, d->name, POS_STYLE_TITLE);
            label(a->body, ninja_org_name(&a->data, d->org), POS_STYLE_TEXT_SECONDARY);
            snprintf(text, sizeof(text), "%s%s", a->stale ? "Last observed: " : "Status: ",
                     d->offline == -1 ? "Unknown" : d->offline ? "Offline" : "Online");
            label(a->body, text, POS_STYLE_VALUE);
            if (d->last_contact) {
                snprintf(text, sizeof(text), "Last contact (API timestamp): %.17g", d->last_contact);
            } else snprintf(text, sizeof(text), "Last contact: Unknown");
            label(a->body, text, POS_STYLE_TEXT_SECONDARY);
        } else label(a->body, "Device details unavailable in this bounded snapshot", POS_STYLE_TEXT_SECONDARY);
        label(a->body, "Active alerts", POS_STYLE_CAPTION);
        for (unsigned i = 0; i < a->data.nalerts; i++) if (a->data.alerts[i].device == a->device) {
            if (shown++ < PAGE_ROWS) {
                snprintf(text, sizeof(text), "%s / %s", a->data.alerts[i].severity, a->data.alerts[i].message);
                label(a->body, text, POS_STYLE_TEXT_PRIMARY);
            }
        }
        if (!shown) label(a->body, "No active alerts in this snapshot", POS_STYLE_TEXT_SECONDARY);
        if (shown > PAGE_ROWS) label(a->body, "First 20 alerts shown; use Alerts for the paged list", POS_STYLE_CAPTION);
        return;
    }
    if (a->tab == 0) {
        snprintf(text, sizeof(text), "%u active alerts / %u affected devices in snapshot", a->data.nalerts, ninja_affected(&a->data, 0));
        label(a->body, text, POS_STYLE_TITLE);
        snprintf(text, sizeof(text), "%u organizations / %u devices in snapshot", a->data.norgs, a->data.ndevices);
        label(a->body, text, POS_STYLE_TEXT_SECONDARY);
        label(a->body, "Read-only monitoring. Select an affected device to inspect its alerts.", POS_STYLE_TEXT_SECONDARY);

        /* One row per affected device, including an alert whose device is outside
         * the device cap. Such a row opens unknown details, never a healthy fake. */
        for (unsigned i = 0; i < a->data.nalerts; i++) {
            int id = a->data.alerts[i].device;
            bool seen = false;
            for (unsigned j = 0; j < i; j++) if (a->data.alerts[j].device == id) seen = true;
            if (seen) continue;
            if (matching++ < (unsigned)a->page * PAGE_ROWS || shown == PAGE_ROWS) continue;
            const struct ninja_device *d = ninja_device_find(&a->data, id);
            snprintf(text, sizeof(text), "%s / %s", d ? d->name : "Unknown device", d ? ninja_org_name(&a->data, d->org) : "Unknown organization");
            button(a, a->body, text, -id); shown++;
        }
    } else if (a->tab == 1) {
        button(a, a->body, a->only_favorites ? "Show all organizations" : "Show favorites only", 7);
        for (unsigned i = 0; i < a->data.norgs; i++) {
            const struct ninja_org *o = &a->data.orgs[i];
            if ((a->only_favorites && !favorite(a, o->id)) ||
                (a->query[0] && !strcasestr(o->name, a->query))) continue;
            if (matching++ < (unsigned)a->page * PAGE_ROWS || shown == PAGE_ROWS) continue;
            snprintf(text, sizeof(text), "%s%s", favorite(a, o->id) ? "Favorite / " : "", o->name);
            lv_obj_t *b = button(a, a->body, text, -1);
            lv_obj_remove_event_cb(b, action);
            lv_obj_add_event_cb(b, organization, LV_EVENT_CLICKED, a);
            lv_obj_set_user_data(b, (void *)(intptr_t)o->id); shown++;
        }
    } else {
        if (a->org) {
            label(a->body, ninja_org_name(&a->data, a->org), POS_STYLE_TITLE);
            button(a, a->body, "All organizations", 5);
            button(a, a->body, favorite(a, a->org) ? "Remove local favorite" : "Save local favorite", 6);
            label(a->body, "Devices", POS_STYLE_CAPTION);
            for (unsigned i = 0; i < a->data.ndevices; i++) {
                const struct ninja_device *d = &a->data.devices[i];
                if (d->org != a->org) continue;
                if (matching++ < (unsigned)a->page * PAGE_ROWS || shown == PAGE_ROWS) continue;
                button(a, a->body, d->name, -d->id); shown++;
            }
        }
        label(a->body, "Active alerts", POS_STYLE_CAPTION);
        for (unsigned i = 0; i < a->data.nalerts; i++) {
            const struct ninja_alert *alert = &a->data.alerts[i];
            const struct ninja_device *d = ninja_device_find(&a->data, alert->device);
            if (a->org && (!d || d->org != a->org)) continue;
            if (a->severity_filter[0] && strcasecmp(a->severity_filter, alert->severity)) continue;
            if (matching++ < (unsigned)a->page * PAGE_ROWS || shown == PAGE_ROWS) continue;
            snprintf(text, sizeof(text), "%s / %s / %s", d ? d->name : "Unknown device", alert->severity, alert->message);
            button(a, a->body, text, -alert->device); shown++;
        }
        if (a->data.partial && a->org) label(a->body, "Organization filtering covers loaded devices only", POS_STYLE_CAPTION);
    }
    if (!matching) label(a->body, "No matching results in this snapshot", POS_STYLE_TEXT_SECONDARY);
    if (a->page) button(a, a->body, "Previous page", 8);
    if (matching > (unsigned)(a->page + 1) * PAGE_ROWS) button(a, a->body, "Next page", 9);
    if (a->page && !shown) { a->page = 0; a->dirty = true; }
}
static void poll_timer(lv_timer_t *timer)
{
    struct ninja_app *a = lv_timer_get_user_data(timer);
    int64_t now = now_ms();
    struct pos_insets insets;
    if (pocketui_layout_begin(&a->guard, a->frame, &insets)) {
        bool wide = lv_obj_get_width(a->frame) > lv_obj_get_height(a->frame);
        lv_obj_set_style_pad_left(a->frame, POCKETUI_PAD + insets.left, 0);
        lv_obj_set_style_pad_right(a->frame, POCKETUI_PAD + insets.right, 0);
        lv_obj_set_style_pad_bottom(a->frame, POCKETUI_PAD + insets.bottom, 0);
        for (unsigned i = 0; i < lv_obj_get_child_count(a->nav); i++)
            lv_obj_set_width(lv_obj_get_child(a->nav, i), LV_PCT(wide ? 23 : 48));
    }
    if (a->refresh && now >= a->next) {
        a->refresh = false; a->loading = true; a->renewing = false; a->forgetting = false;
        if (ninja_session_start(&a->session, now, a->fake)) { a->loading = false; a->error = NINJA_HELPER; a->stale = a->data.updated != 0; }
        a->dirty = true;
    }
    if (a->loading) {
        struct ninja_snapshot *reply = malloc(sizeof(*reply));
        int rc = reply ? ninja_session_poll(&a->session, now, reply) : -1;
        if (!reply) ninja_session_close(&a->session);
        if (rc == 2) {
            if (reply->profile[0]) select_profile(a, reply->profile);
            a->renewing = true; a->stale = a->data.updated != 0; a->dirty = true;
        } else if (rc) {
            a->loading = false;
            a->renewing = false;
            a->error = rc > 0 ? reply->error : NINJA_HELPER;
            if (rc > 0 && !a->forgetting && (reply->profile[0] || !reply->error))
                select_profile(a, reply->profile);
            if (rc > 0) a->expires = reply->expires;
            if (!a->error) { memcpy(&a->data, reply, sizeof(*reply)); a->last_ms = now; a->failures = 0; a->stale = false; }
            else { a->stale = a->data.updated != 0; if (a->failures < 7) a->failures++; }
            int retry = a->error ? 30 * (1 << a->failures) : NINJA_REFRESH_S;
            if (retry > 3600) retry = 3600;
            if (rc > 0 && reply->retry_s > retry) retry = reply->retry_s;
            if (!a->error && a->expires > 0) {
                int64_t remaining = a->expires - (int64_t)time(NULL);
                if (remaining < retry) retry = remaining > 0 ? (int)remaining : 1;
            }
            a->next = now + (int64_t)retry * 1000;
            a->dirty = true;
        }
        free(reply);
    }
    if (!a->loading && now >= a->next && a->error != NINJA_SETUP && a->error != NINJA_AUTH && a->error != NINJA_DENIED && a->error != NINJA_DISCONNECTED && !a->forgetting)
        a->refresh = true;
    if (a->last_ms && now - a->last_ms > 2 * NINJA_REFRESH_S * 1000 && !a->stale) { a->stale = true; a->dirty = true; }
    /* Manual refresh can retry auth/setup immediately; rate backoff is always
     * respected. Other deadlines are shortened by the click handler below. */
    if (a->dirty) { a->dirty = false; draw(a); }
}
static void manual_refresh(lv_event_t *e)
{
    struct ninja_app *a = lv_event_get_user_data(e);
    if (a->error != NINJA_RATE && a->error != NINJA_RENEW_FAILED) a->next = 0;
}
static void *create(lv_obj_t *root)
{
    struct ninja_app *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    ninja_session_init(&a->session); a->error = NINJA_SETUP;
    const char *backend = getenv("POCKETOS_NINJAONE_BACKEND");
    if (backend && !strcmp(backend, "fake")) {
        a->fake = getenv("POCKETOS_NINJAONE_FAKE");
        if (!a->fake || !*a->fake) a->fake = "demo";
    }
    a->frame = pocketui_card(root); lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(a->frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->frame, LV_DIR_VER);
    label(a->frame, "Unofficial NinjaOne client / read-only", POS_STYLE_CAPTION);
    a->nav = lv_obj_create(a->frame); lv_obj_remove_style_all(a->nav);
    lv_obj_set_size(a->nav, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(a->nav, LV_FLEX_FLOW_ROW_WRAP); lv_obj_set_style_pad_gap(a->nav, 8, 0);
    button(a, a->nav, "Overview", 0); button(a, a->nav, "Organizations", 1); button(a, a->nav, "Alerts", 2);
    lv_obj_t *refresh = button(a, a->nav, "Refresh data", 3);
    lv_obj_add_event_cb(refresh, manual_refresh, LV_EVENT_CLICKED, a);
    button(a, a->frame, "Disconnect / Forget credentials", 10);
    a->status = label(a->frame, "Loading...", POS_STYLE_TEXT_SECONDARY);
    a->freshness = label(a->frame, "No successful update", POS_STYLE_CAPTION);
    a->search = lv_textarea_create(a->frame);
    lv_obj_remove_style_all(a->search);
    lv_obj_add_style(a->search, pos_style(POS_STYLE_FIELD), 0);
    lv_obj_add_style(a->search, pos_style(POS_STYLE_FIELD_FOCUSED), LV_STATE_FOCUSED);
    lv_obj_add_style(a->search, pos_style(POS_STYLE_FIELD_CURSOR), LV_PART_CURSOR);
    lv_obj_add_style(a->search, pos_style(POS_STYLE_FIELD_PLACEHOLDER), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_width(a->search, LV_PCT(100)); lv_obj_set_style_min_height(a->search, POCKETUI_TOUCH_MIN, 0);
    lv_textarea_set_one_line(a->search, true); lv_textarea_set_max_length(a->search, sizeof(a->query)-1);
    lv_textarea_set_placeholder_text(a->search, "Search organizations");
    pos_input_add_obj(a->search); lv_obj_add_event_cb(a->search, search_event, LV_EVENT_VALUE_CHANGED, a);
    lv_obj_add_event_cb(a->search, search_event, LV_EVENT_CLICKED, a);
    a->severity_cap = label(a->frame, "Severity: exact API value; blank shows all", POS_STYLE_CAPTION);
    a->severity = lv_textarea_create(a->frame);
    lv_obj_remove_style_all(a->severity);
    lv_obj_add_style(a->severity, pos_style(POS_STYLE_FIELD), 0);
    lv_obj_add_style(a->severity, pos_style(POS_STYLE_FIELD_FOCUSED), LV_STATE_FOCUSED);
    lv_obj_add_style(a->severity, pos_style(POS_STYLE_FIELD_CURSOR), LV_PART_CURSOR);
    lv_obj_add_style(a->severity, pos_style(POS_STYLE_FIELD_PLACEHOLDER), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_width(a->severity, LV_PCT(100)); lv_obj_set_style_min_height(a->severity, POCKETUI_TOUCH_MIN, 0);
    lv_textarea_set_one_line(a->severity, true); lv_textarea_set_max_length(a->severity, sizeof(a->severity_filter)-1);
    lv_textarea_set_placeholder_text(a->severity, "All severities (Unknown for missing)");
    pos_input_add_obj(a->severity);
    lv_obj_add_event_cb(a->severity, search_event, LV_EVENT_VALUE_CHANGED, a);
    lv_obj_add_event_cb(a->severity, search_event, LV_EVENT_CLICKED, a);
    a->body = pocketui_card(a->frame); lv_obj_set_style_pad_row(a->body, 8, 0); lv_obj_set_width(a->body, LV_PCT(100)); lv_obj_set_height(a->body, LV_SIZE_CONTENT);
    a->dirty = a->refresh = true;
    a->timer = lv_timer_create(poll_timer, 50, a);
    return a;
}
static void destroy(void *priv)
{
    struct ninja_app *a = priv;
    if (!a) return;
    pocketos_shell_keyboard_hide();
    lv_timer_delete(a->timer); ninja_session_close(&a->session);
    free(a);
}
static int back(void *priv)
{
    struct ninja_app *a = priv;
    if (a->device) a->device = 0;
    else if (a->org) { a->org = 0; a->page = 0; }
    else if (a->tab) { a->tab = 0; a->page = 0; }
    else return 0;
    a->dirty = true; return 1;
}
const struct pocketos_app app_ninjaone = {
    .id = "ninjaone",
    .name = "NinjaOne",
    .icon = LV_SYMBOL_LIST,
    .icon_mask = &pos_app_icon_ninjaone,
    .create = create,
    .destroy = destroy,
    .back = back,
    .back_slab_in_app = true,
};
