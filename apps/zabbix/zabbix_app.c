/*
 * Zabbix: a monitoring terminal for an existing Zabbix server - OVERVIEW,
 * PROBLEMS, HOSTS with a host's detail, and STATUS. Read-only: nothing here
 * acknowledges, configures or changes anything on the server.
 *
 * This file is the screen. What the helper said and the words each screen
 * shows are zabbix_view.c; the server is not here at all. It lives in a
 * pos-zabbix helper process (zabbix_session.c, ADR-007 accepted), polled
 * from an LVGL timer that only ever makes non-blocking calls. The only wait
 * on the LVGL thread is destroy() (and a demo switch) giving the helper
 * ZABBIX_DESTROY_GRACE_MS to leave before it is killed.
 *
 * LISTS are pools: a row is created the first time a set needs it and
 * reused for every set after, never more than the model holds
 * (ZBX_PROBLEM_MAX, ZBX_HOST_MAX), and only the labels whose text changed
 * are written. A refresh of 100 problems on a 30 s cycle therefore creates
 * nothing and redraws only what moved.
 *
 * FULLSCREEN (DS section 30.8): the app declares NONE; the shell's header
 * carries the back button and the hint, which says SIMULATED whenever the
 * data comes from the fake server, so a made-up outage is never mistaken for
 * a real one.
 *
 * LAYOUT is chosen from the body in a timer, never inside an LVGL event
 * (docs: lvgl-layout gotchas; the size handler only sets a flag).
 *
 * CONNECTION (DS §50), opened from STATUS or the set-up panel, changes the
 * server and the sign-in on the unit: the fields zabbix.conf and the secret
 * already had, TEST CONNECTION and SAVE. This screen only collects them;
 * the helper checks, tries and stores (core/zabbix/zbx_settings.h). The
 * token or password field is masked and never filled from the store - the
 * helper says only whether one is stored - and what was typed is wiped
 * when it has been sent on SAVE, when the screen closes and when the app
 * does.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE /* explicit_bzero */
#include "app.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "zabbix_session.h"
#include "zabbix_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZABBIX_POLL_MS 100
#define ZABBIX_PAINT_MS 1000
/* destroy() and a demo switch: time for the helper to leave. */
#define ZABBIX_DESTROY_GRACE_MS 300
#define TAB_H 64
#define TAB_GAP 0
#define UNDERLINE_H 2
#define GAP 12
#define ROW_PAD 12
#define SEV_W 96
#define KEY_W 150
#define BANNER_PAD 12
/* The most severe open problems OVERVIEW shows under its cards. */
#define OVERVIEW_TOP 3
/* CONNECTION: how long a test or save may take before the screen gives up
 * on the helper. A round is at most a few requests of timeout_s (30 s at
 * most) each; this is past any of them. */
#define ZABBIX_SETTINGS_WAIT_MS 150000
/* What the fields take. The url keeps room for "api_jsonrpc.php"
 * (zbx_config_url); the others are the configuration's sizes less one. */
#define CN_URL_MAX 200
#define CN_USER_MAX (ZBX_PROTO_USER_MAX - 1)
#define CN_SECRET_MAX (ZBX_PROTO_SECRET_MAX - 1)

enum cn_pending {
    CN_IDLE = 0,
    CN_TESTING,
    CN_SAVING,
};

enum zabbix_tab {
    TAB_OVERVIEW = 0,
    TAB_PROBLEMS,
    TAB_HOSTS,
    TAB_STATUS,
    TAB_COUNT
};

static const char *const tab_name[TAB_COUNT] = { "OVERVIEW", "PROBLEMS", "HOSTS", "STATUS" };

/* The fake scenarios the STATUS button steps through: the ones that show
 * something on the screen, not every one the tests use. */
static const char *const demo_cycle[] = { "demo", "healthy", "large", "flap", "drop",
                                          "auth", "timeout", "malformed", "empty" };
#define DEMO_CYCLE ((int)(sizeof(demo_cycle) / sizeof(demo_cycle[0])))

struct zabbix_app;

struct prow {
    lv_obj_t *row;
    lv_obj_t *sev;
    lv_obj_t *host;
    lv_obj_t *meta;
    lv_obj_t *name;
    char hostid[ZBX_ID_MAX];
    char hostname[ZBX_HOST_NAME_MAX]; /* the label may hold a cut copy */
    struct zabbix_app *app;
};

struct hrow {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *avail;
    lv_obj_t *problems;
    lv_obj_t *maint;
    char hostid[ZBX_ID_MAX];
    char hostname[ZBX_HOST_NAME_MAX];
    struct zabbix_app *app;
};

struct irow {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *value;
    lv_obj_t *age;
};

struct srow {
    lv_obj_t *row;
    lv_obj_t *key;
    lv_obj_t *value;
};

struct zabbix_app {
    lv_obj_t *frame;
    lv_obj_t *strip;
    lv_obj_t *tab[TAB_COUNT];
    lv_obj_t *tab_label[TAB_COUNT];
    lv_obj_t *tab_rule[TAB_COUNT];
    lv_obj_t *pr_age;           /* LIVE age, UPDATING, OFFLINE: beside each list title */
    lv_obj_t *ho_age;
    lv_obj_t *banner;
    lv_obj_t *banner_label;
    lv_obj_t *content;
    lv_obj_t *page[TAB_COUNT];

    /* OVERVIEW */
    lv_obj_t *ov_setup;
    lv_obj_t *ov_setup_text;
    lv_obj_t *ov_demo;
    lv_obj_t *ov_cards;
    lv_obj_t *ov_head;
    lv_obj_t *ov_headline;
    lv_obj_t *ov_head_note;
    lv_obj_t *ov_sev;
    lv_obj_t *ov_sev_cell[ZBX_SEV_COUNT];
    lv_obj_t *ov_sev_value[ZBX_SEV_COUNT];
    lv_obj_t *ov_sev_word[ZBX_SEV_COUNT];
    lv_obj_t *ov_counts;
    lv_obj_t *ov_problems;
    lv_obj_t *ov_hosts;
    lv_obj_t *ov_hosts_note;
    lv_obj_t *ov_server;
    lv_obj_t *ov_updated;
    lv_obj_t *ov_top_cap;       /* the most severe problems, a tap from their host */
    struct prow ov_top[OVERVIEW_TOP];
    int ov_top_made;

    /* PROBLEMS */
    lv_obj_t *pr_title;
    lv_obj_t *pr_empty;
    struct prow *prows;         /* ZBX_PROBLEM_MAX slots, rows made on demand */
    int prow_made;

    /* HOSTS */
    lv_obj_t *ho_title;
    lv_obj_t *ho_empty;
    struct hrow *hrows;         /* ZBX_HOST_MAX slots */
    int hrow_made;

    /* one host, over PROBLEMS or HOSTS */
    lv_obj_t *detail;
    lv_obj_t *dt_back;
    lv_obj_t *dt_name;
    lv_obj_t *dt_state;
    lv_obj_t *dt_note;
    lv_obj_t *dt_pcap;
    lv_obj_t *dt_icap;
    struct prow dprows[ZBX_DETAIL_PROBLEM_MAX];
    int dprow_made;
    struct irow irows[ZBX_ITEM_MAX];
    int irow_made;
    bool detail_open;
    char detail_hostid[ZBX_ID_MAX];
    char detail_name[ZBX_HOST_NAME_MAX];

    /* STATUS */
    struct srow srows[ZABBIX_STATUS_LINES];
    lv_obj_t *st_refresh;
    lv_obj_t *st_conn;
    lv_obj_t *st_scenario;
    lv_obj_t *st_demo;

    /* CONNECTION, over everything (the tabs and the banner hide) */
    lv_obj_t *conn;
    lv_obj_t *cn_back;
    lv_obj_t *cn_url;
    lv_obj_t *cn_auth[2];       /* API TOKEN, PASSWORD */
    lv_obj_t *cn_user_cap;
    lv_obj_t *cn_user;
    lv_obj_t *cn_secret_cap;
    lv_obj_t *cn_secret;        /* masked; never holds a stored secret */
    lv_obj_t *cn_secret_note;
    lv_obj_t *cn_file_note;
    lv_obj_t *cn_result;
    lv_obj_t *cn_result_text;
    lv_obj_t *cn_test;
    lv_obj_t *cn_save;
    lv_obj_t *cn_focus;         /* the field last tapped, to bring into view */
    bool conn_open;
    bool cn_password;           /* the sign-in chosen on the screen */
    int cn_choice_shown;        /* the auth buttons as last styled; -1: not yet */
    bool cn_filled;             /* the fields hold the files' values (or the user's) */
    bool cn_filling;            /* writing them: not the user's edit */
    bool cn_edited;
    enum cn_pending cn_pending;
    int64_t cn_deadline;
    unsigned cn_seen;           /* the model's cresult_seq already shown */
    bool cn_reveal;
    bool cn_restart;            /* a save stored new settings: start a helper on them */

    enum zabbix_tab cur;        /* the tab in front */
    bool wide;
    bool relayout;
    bool dirty_problems;
    bool dirty_hosts;
    bool dirty_detail;
    struct pocketui_layout_guard guard;
    lv_timer_t *timer;
    int64_t painted_ms;

    bool demo;                  /* the user asked for the demo */
    int demo_index;
    struct zabbix_session *session;
    struct zabbix_model *model;
    const char *hint_shown;
};

static int64_t now_ms(void)
{
    return (int64_t)lv_tick_get();
}

/* ---- small helpers ------------------------------------------------------------------ */

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj && hidden != lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        if (hidden) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void label_text(lv_obj_t *lb, const char *text)
{
    if (lb && strcmp(lv_label_get_text(lb), text) != 0) {
        lv_label_set_text(lb, text);
    }
}

/* A tone is a colour-only role over the label's own font role (pos_styles.c:
 * the status text roles set nothing but the colour). PLAIN and QUIET leave
 * the base role's colour. Only a change is applied; the last is kept in the
 * user data, offset by one so 0 means "not yet". */
static void tone(lv_obj_t *lb, enum zabbix_tone t)
{
    intptr_t want = (intptr_t)t + 1;

    if (!lb || (intptr_t)lv_obj_get_user_data(lb) == want) {
        return;
    }
    lv_obj_set_user_data(lb, (void *)want);
    lv_obj_remove_style(lb, pos_style(POS_STYLE_STATUS_OK_TEXT), 0);
    lv_obj_remove_style(lb, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
    lv_obj_remove_style(lb, pos_style(POS_STYLE_STATUS_ERROR_TEXT), 0);
    switch (t) {
    case ZABBIX_TONE_OK:
        pos_style_add(lb, POS_STYLE_STATUS_OK_TEXT, 0);
        break;
    case ZABBIX_TONE_WARN:
        pos_style_add(lb, POS_STYLE_STATUS_WARN_TEXT, 0);
        break;
    case ZABBIX_TONE_ERROR:
        pos_style_add(lb, POS_STYLE_STATUS_ERROR_TEXT, 0);
        break;
    case ZABBIX_TONE_PLAIN:
    case ZABBIX_TONE_QUIET:
    default:
        break;
    }
    lv_obj_invalidate(lb);
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *column(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *o = box(parent);

    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

static lv_obj_t *row_box(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *o = box(parent);

    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}

/* A label that never wraps: one line tall, cut with dots (a dotted label of
 * automatic height wraps instead, which is what the Files gate found). */
static lv_obj_t *one_line(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, text, role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_height(lb, lv_font_get_line_height(lv_obj_get_style_text_font(lb, LV_PART_MAIN)));
    return lb;
}

static lv_obj_t *wrapping(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, text, role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_PCT(100));
    return lb;
}

static lv_obj_t *grow(lv_obj_t *obj)
{
    lv_obj_set_width(obj, 1);
    lv_obj_set_flex_grow(obj, 1);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, bool primary, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = pocketui_button(parent, text, cb, user);

    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_CLIP);
    if (!primary) {
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
        pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(b, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
    return b;
}

static void button_text(lv_obj_t *b, const char *text)
{
    label_text(lv_obj_get_child(b, 0), text);
}

/* A tappable list row: a slab-pressed box with the divider under it. */
static lv_obj_t *list_row(lv_obj_t *parent, lv_event_cb_t cb, void *user)
{
    lv_obj_t *r = column(parent, 4);

    pos_style_add(r, POS_STYLE_DIVIDER, 0);
    lv_obj_set_style_pad_all(r, ROW_PAD, 0);
    lv_obj_set_style_min_height(r, POCKETUI_TOUCH_MIN, 0);
    if (cb) {
        pos_style_add(r, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, user);
    }
    return r;
}

/* ---- the session ------------------------------------------------------------------ */

static void start_helper(struct zabbix_app *a)
{
    struct zabbix_session_config cfg = { 0 };
    char err[ZBX_TEXT_MAX];

    cfg.fake = a->demo ? demo_cycle[a->demo_index] : zabbix_session_default_fake();
    if (zabbix_session_start(a->session, &cfg, now_ms(), err, sizeof(err)) != 0) {
        LOG_WARN("zabbix: could not start the helper: %s", err);
        zabbix_model_helper_started(a->model, now_ms());
        zabbix_model_helper_stopped(a->model, ZABBIX_EXIT_START, 0, now_ms());
        return;
    }
    zabbix_model_helper_started(a->model, now_ms());
    if (a->detail_open && a->detail_hostid[0]) {
        zabbix_session_detail(a->session, a->detail_hostid);
    }
}

static void restart_helper(struct zabbix_app *a)
{
    zabbix_session_abandon(a->session, ZABBIX_DESTROY_GRACE_MS);
    zabbix_model_forget(a->model);
    a->model->helper_running = false;
    a->dirty_problems = a->dirty_hosts = a->dirty_detail = true;
    start_helper(a);
}

/* ---- rows ------------------------------------------------------------------------ */

static void open_detail(struct zabbix_app *a, const char *hostid, const char *name);

static void on_problem_row(lv_event_t *e)
{
    struct prow *r = lv_event_get_user_data(e);

    if (r->hostid[0]) {
        open_detail(r->app, r->hostid, r->hostname);
    }
}

static void on_host_row(lv_event_t *e)
{
    struct hrow *r = lv_event_get_user_data(e);

    if (r->hostid[0]) {
        open_detail(r->app, r->hostid, r->hostname);
    }
}

static void make_prow(struct zabbix_app *a, struct prow *r, lv_obj_t *parent, bool tappable)
{
    lv_obj_t *line;

    r->app = a;
    r->row = list_row(parent, tappable ? on_problem_row : NULL, r);
    line = row_box(r->row, GAP);
    r->sev = one_line(line, "", POS_STYLE_CAPTION);
    lv_obj_set_width(r->sev, SEV_W);
    r->host = grow(one_line(line, "", POS_STYLE_TEXT_PRIMARY));
    r->meta = one_line(line, "", POS_STYLE_CAPTION);
    lv_obj_set_width(r->meta, LV_SIZE_CONTENT);
    lv_label_set_long_mode(r->meta, LV_LABEL_LONG_CLIP);
    r->name = one_line(r->row, "", POS_STYLE_TEXT_SECONDARY);
    lv_obj_set_width(r->name, LV_PCT(100));
}

static void fill_prow(struct prow *r, const struct zbx_problem *p, int64_t server_now)
{
    struct zabbix_problem_row v;

    zabbix_view_problem(p, server_now, &v);
    label_text(r->sev, v.severity);
    tone(r->sev, v.tone);
    label_text(r->host, v.host);
    label_text(r->meta, v.meta);
    tone(r->meta, v.acknowledged ? ZABBIX_TONE_QUIET : ZABBIX_TONE_PLAIN);
    label_text(r->name, v.name);
    snprintf(r->hostid, sizeof(r->hostid), "%s", p->hostid);
    snprintf(r->hostname, sizeof(r->hostname), "%s", p->host);
    set_hidden(r->row, false);
}

static void make_hrow(struct zabbix_app *a, struct hrow *r, lv_obj_t *parent)
{
    lv_obj_t *l1;
    lv_obj_t *l2;

    r->app = a;
    r->row = list_row(parent, on_host_row, r);
    l1 = row_box(r->row, GAP);
    r->name = grow(one_line(l1, "", POS_STYLE_TEXT_PRIMARY));
    r->avail = one_line(l1, "", POS_STYLE_CAPTION);
    lv_obj_set_width(r->avail, LV_SIZE_CONTENT);
    lv_label_set_long_mode(r->avail, LV_LABEL_LONG_CLIP);
    l2 = row_box(r->row, GAP);
    r->problems = grow(one_line(l2, "", POS_STYLE_CAPTION));
    r->maint = one_line(l2, "MAINTENANCE", POS_STYLE_CAPTION);
    lv_obj_set_width(r->maint, LV_SIZE_CONTENT);
    lv_label_set_long_mode(r->maint, LV_LABEL_LONG_CLIP);
    tone(r->maint, ZABBIX_TONE_WARN);
}

static void fill_hrow(struct hrow *r, const struct zbx_host *h)
{
    struct zabbix_host_row v;

    zabbix_view_host(h, &v);
    label_text(r->name, v.name);
    label_text(r->avail, v.avail);
    tone(r->avail, v.avail_tone);
    label_text(r->problems, v.problems);
    tone(r->problems, v.problems_tone);
    set_hidden(r->maint, !v.maintenance);
    snprintf(r->hostid, sizeof(r->hostid), "%s", h->hostid);
    snprintf(r->hostname, sizeof(r->hostname), "%s", h->name);
    set_hidden(r->row, false);
}

/* ---- painting ------------------------------------------------------------------------ */

static void paint_tabs(struct zabbix_app *a)
{
    char name[48];
    int i;

    for (i = 0; i < TAB_COUNT; i++) {
        bool active = i == (int)a->cur;

        if (i == TAB_PROBLEMS && a->model->problems_ms > 0) {
            char n[24];

            zabbix_format_count(n, sizeof(n), a->model->problems.total);
            snprintf(name, sizeof(name), "%s %s", tab_name[i], n);
        } else {
            snprintf(name, sizeof(name), "%s", tab_name[i]);
        }
        label_text(a->tab_label[i], name);
        if (active) {
            pos_style_add(a->tab_label[i], POS_STYLE_ACCENT_TEXT, 0);
        } else {
            lv_obj_remove_style(a->tab_label[i], pos_style(POS_STYLE_ACCENT_TEXT), 0);
        }
        /* Shown by opacity, not hidden: a hidden rule would leave the flex
         * and move its label by two pixels. */
        lv_obj_set_style_opa(a->tab_rule[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void paint_banner(struct zabbix_app *a, int64_t now)
{
    struct zabbix_banner b;
    char cap[64];

    zabbix_view_banner(a->model, now, &b);
    /* CONNECTION has the screen to itself: with the keyboard up in
     * landscape the body is 100 px, room for a field and nothing else. */
    set_hidden(a->banner, !b.show || a->conn_open);
    if (b.show) {
        label_text(a->banner_label, b.text);
        tone(a->banner_label, b.tone);
    }
    zabbix_view_caption(a->model, now, cap, sizeof(cap));
    label_text(a->pr_age, cap);
    label_text(a->ho_age, cap);
}

static void paint_overview(struct zabbix_app *a, int64_t now)
{
    struct zabbix_overview_view v;
    int s;

    zabbix_view_overview(a->model, now, &v);
    set_hidden(a->ov_setup, !v.setup);
    set_hidden(a->ov_cards, v.setup);
    {
        const struct zabbix_model *m = a->model;
        int64_t server_now = zabbix_server_now(m->problems.ref_clock, m->problems_ms, now);
        int n = v.setup || m->problems_ms <= 0 ? 0 : m->problems.count;
        int i;

        /* The most severe open problems, under the cards: the first thing
         * to look at, a tap away from their host. */
        set_hidden(a->ov_top_cap, n == 0);
        for (i = 0; i < n && i < OVERVIEW_TOP; i++) {
            if (i >= a->ov_top_made) {
                make_prow(a, &a->ov_top[i], a->page[TAB_OVERVIEW], true);
                a->ov_top_made = i + 1;
            }
            fill_prow(&a->ov_top[i], &m->problems.p[i], server_now);
        }
        for (; i < a->ov_top_made; i++) {
            set_hidden(a->ov_top[i].row, true);
            a->ov_top[i].hostid[0] = '\0';
        }
    }
    if (v.setup) {
        char text[ZABBIX_LINE_TEXT * 3];
        /* Why it is not set up, when the helper said more than "it is not". */
        const char *why = a->model->err_text;
        bool reason = why[0] && strcmp(why, "No Zabbix server is set up") != 0;

        snprintf(text, sizeof(text),
                 "This unit has no Zabbix server set up%s%s.\n\n"
                 "SET UP THE CONNECTION takes the server's address and an API token "
                 "(or a user and password), tests them and stores them. It can also "
                 "be done over SSH (docs/apps/ZABBIX.md).\n\n"
                 "An API token of a read-only user is best.",
                 reason ? ": " : "", reason ? why : "");
        label_text(a->ov_setup_text, text);
        return;
    }
    label_text(a->ov_headline, v.headline);
    tone(a->ov_headline, v.headline_tone);
    label_text(a->ov_head_note, v.headline_note);
    for (s = 0; s < ZBX_SEV_COUNT; s++) {
        char n[24];
        int sev = ZBX_SEV_COUNT - 1 - s; /* disaster on the left */

        if (v.loading) {
            snprintf(n, sizeof(n), "--");
        } else {
            zabbix_format_count(n, sizeof(n), v.sev_count[sev]);
        }
        label_text(a->ov_sev_value[s], n);
        tone(a->ov_sev_value[s], !v.loading && v.sev_count[sev] > 0 ? zabbix_severity_tone(sev)
                                                                      : ZABBIX_TONE_QUIET);
    }
    label_text(a->ov_problems, v.loading ? "Waiting for the first refresh" : v.problems);
    label_text(a->ov_hosts, v.hosts[0] ? v.hosts : "Hosts: not read yet");
    tone(a->ov_hosts, v.hosts_tone);
    label_text(a->ov_hosts_note, v.hosts_note);
    label_text(a->ov_server, v.server);
    label_text(a->ov_updated, v.updated);
}

static void paint_problems(struct zabbix_app *a, int64_t now)
{
    const struct zabbix_model *m = a->model;
    int64_t server_now = zabbix_server_now(m->problems.ref_clock, m->problems_ms, now);
    char title[96];
    int n = m->problems_ms > 0 ? m->problems.count : 0;
    int i;

    zabbix_view_problems_title(m, title, sizeof(title));
    label_text(a->pr_title, title);
    set_hidden(a->pr_empty, n > 0);
    label_text(a->pr_empty, m->problems_ms <= 0 ? "Waiting for the first refresh"
                                                : "No open problems. All clear.");
    tone(a->pr_empty, m->problems_ms <= 0 ? ZABBIX_TONE_QUIET : ZABBIX_TONE_OK);
    for (i = 0; i < n && i < ZBX_PROBLEM_MAX; i++) {
        if (i >= a->prow_made) {
            make_prow(a, &a->prows[i], a->page[TAB_PROBLEMS], true);
            a->prow_made = i + 1;
        }
        fill_prow(&a->prows[i], &m->problems.p[i], server_now);
    }
    for (; i < a->prow_made; i++) {
        set_hidden(a->prows[i].row, true);
        a->prows[i].hostid[0] = '\0';
    }
}

static void paint_hosts(struct zabbix_app *a)
{
    const struct zabbix_model *m = a->model;
    char title[96];
    int n = m->hosts_ms > 0 ? m->hosts.count : 0;
    int i;

    zabbix_view_hosts_title(m, title, sizeof(title));
    label_text(a->ho_title, title);
    set_hidden(a->ho_empty, n > 0);
    label_text(a->ho_empty, m->hosts_ms <= 0 ? "Waiting for the first refresh"
                                             : "No monitored hosts visible to this user.");
    for (i = 0; i < n && i < ZBX_HOST_MAX; i++) {
        if (i >= a->hrow_made) {
            make_hrow(a, &a->hrows[i], a->page[TAB_HOSTS]);
            a->hrow_made = i + 1;
        }
        fill_hrow(&a->hrows[i], &m->hosts.h[i]);
    }
    for (; i < a->hrow_made; i++) {
        set_hidden(a->hrows[i].row, true);
        a->hrows[i].hostid[0] = '\0';
    }
}

static void paint_detail(struct zabbix_app *a, int64_t now)
{
    const struct zabbix_model *m = a->model;
    const struct zbx_detail *d = &m->detail;
    bool have = m->detail_ms > 0 && strcmp(d->host.hostid, a->detail_hostid) == 0;
    bool missing = strcmp(m->detail_missing, a->detail_hostid) == 0;
    int64_t server_now = zabbix_server_now(d->ref_clock, m->detail_ms, now);
    char line[ZABBIX_LINE_TEXT];
    int i;

    label_text(a->dt_name, have ? d->host.name : a->detail_name);
    if (missing) {
        label_text(a->dt_state, m->detail_missing_text);
        tone(a->dt_state, ZABBIX_TONE_ERROR);
    } else if (!have) {
        label_text(a->dt_state, "Reading this host...");
        tone(a->dt_state, ZABBIX_TONE_QUIET);
    } else {
        struct zabbix_host_row v;

        zabbix_view_host(&d->host, &v);
        snprintf(line, sizeof(line), "%s · %s%s", v.avail, v.problems,
                 v.maintenance ? " · MAINTENANCE" : "");
        label_text(a->dt_state, line);
        tone(a->dt_state, d->host.avail == ZBX_AVAIL_DOWN || d->host.max_severity >= ZBX_SEV_HIGH
                              ? ZABBIX_TONE_ERROR
                              : d->host.problems > 0 ? ZABBIX_TONE_WARN : ZABBIX_TONE_OK);
    }
    set_hidden(a->dt_note, !(have && d->problem_count == 0));
    set_hidden(a->dt_pcap, !(have && d->problem_count > 0));
    set_hidden(a->dt_icap, !(have && d->item_count > 0));
    for (i = 0; have && i < d->problem_count && i < ZBX_DETAIL_PROBLEM_MAX; i++) {
        if (i >= a->dprow_made) {
            make_prow(a, &a->dprows[i], a->detail, false);
            /* Problems sit between their caption and the values. */
            lv_obj_move_to_index(a->dprows[i].row, (int32_t)lv_obj_get_index(a->dt_icap));
            a->dprow_made = i + 1;
        }
        fill_prow(&a->dprows[i], &d->p[i], server_now);
    }
    for (; i < a->dprow_made; i++) {
        set_hidden(a->dprows[i].row, true);
    }
    for (i = 0; have && i < d->item_count && i < ZBX_ITEM_MAX; i++) {
        struct zabbix_item_row v;
        struct irow *r = &a->irows[i];

        if (i >= a->irow_made) {
            r->row = list_row(a->detail, NULL, NULL);
            {
                lv_obj_t *l = row_box(r->row, GAP);

                r->name = grow(one_line(l, "", POS_STYLE_TEXT_SECONDARY));
                r->value = one_line(l, "", POS_STYLE_VALUE);
                lv_obj_set_width(r->value, LV_SIZE_CONTENT);
                lv_label_set_long_mode(r->value, LV_LABEL_LONG_CLIP);
                r->age = one_line(l, "", POS_STYLE_CAPTION);
                lv_obj_set_width(r->age, 64);
                lv_obj_set_style_text_align(r->age, LV_TEXT_ALIGN_RIGHT, 0);
            }
            a->irow_made = i + 1;
        }
        zabbix_view_item(&d->item[i], server_now, &v);
        label_text(r->name, v.name);
        label_text(r->value, v.value);
        label_text(r->age, v.age);
        set_hidden(r->row, false);
    }
    for (; i < a->irow_made; i++) {
        set_hidden(a->irows[i].row, true);
    }
}

static void paint_status(struct zabbix_app *a, int64_t now)
{
    struct zabbix_status_view v;
    int i;

    zabbix_view_status(a->model, now, &v);
    for (i = 0; i < ZABBIX_STATUS_LINES; i++) {
        set_hidden(a->srows[i].row, i >= v.count);
        if (i < v.count) {
            label_text(a->srows[i].key, v.key[i]);
            label_text(a->srows[i].value, v.value[i]);
            tone(a->srows[i].value, v.tone[i]);
        }
    }
    set_hidden(a->st_scenario, !a->model->fake);
    if (a->model->fake) {
        char t[64];

        snprintf(t, sizeof(t), "SCENARIO: %.40s", a->model->scenario[0] ? a->model->scenario : "demo");
        button_text(a->st_scenario, t);
    }
    set_hidden(a->st_demo, !a->demo);
}

/* CONNECTION. Only what changed is written; the fields themselves are
 * filled once (fill_conn), and after that they are the user's. */
static void choice_style(lv_obj_t *b, bool on)
{
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    lv_obj_remove_style(b, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
    if (on) {
        pos_style_add(b, POS_STYLE_BUTTON_PRIMARY, 0);
        pos_style_add(b, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    } else {
        pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(b, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
}

static void paint_conn(struct zabbix_app *a)
{
    const struct zabbix_model *m = a->model;
    /* A stored secret counts for the sign-in it was stored for. */
    bool stored = m->have_settings && m->set_stored && m->set_password == a->cn_password;
    size_t typed = strlen(lv_textarea_get_text(a->cn_secret));
    char t[ZABBIX_LINE_TEXT];
    const char *ph;

    if (a->cn_choice_shown != (int)a->cn_password) {
        /* The choice is in the words as well as the fill (DS §2). */
        choice_style(a->cn_auth[0], !a->cn_password);
        choice_style(a->cn_auth[1], a->cn_password);
        button_text(a->cn_auth[0], a->cn_password ? "API TOKEN" : "\xe2\x80\xa2 API TOKEN");
        button_text(a->cn_auth[1], a->cn_password ? "\xe2\x80\xa2 PASSWORD" : "PASSWORD");
        a->cn_choice_shown = (int)a->cn_password;
    }
    set_hidden(a->cn_user_cap, !a->cn_password);
    set_hidden(lv_obj_get_parent(a->cn_user), !a->cn_password);
    zabbix_view_secret_caption(a->cn_password, t, sizeof(t));
    label_text(a->cn_secret_cap, t);
    zabbix_view_secret_note(a->cn_password, stored, typed, t, sizeof(t));
    label_text(a->cn_secret_note, t);
    ph = stored ? "Stored: leave empty to keep it" : a->cn_password ? "Password" : "API token";
    if (strcmp(lv_textarea_get_placeholder_text(a->cn_secret), ph) != 0) {
        lv_textarea_set_placeholder_text(a->cn_secret, ph);
    }
    set_hidden(a->cn_file_note, !m->set_note[0]);
    if (m->set_note[0]) {
        snprintf(t, sizeof(t), "zabbix.conf now: %s", m->set_note);
        label_text(a->cn_file_note, t);
    }
    button_text(a->cn_test, a->cn_pending == CN_TESTING ? "TESTING..." : "TEST CONNECTION");
    button_text(a->cn_save, a->cn_pending == CN_SAVING ? "SAVING..." : "SAVE");
    if (a->cn_pending != CN_IDLE) {
        label_text(a->cn_result, a->cn_pending == CN_SAVING ? "SAVING" : "TESTING");
        tone(a->cn_result, ZABBIX_TONE_QUIET);
        label_text(a->cn_result_text, "Asking the server. This takes up to a few seconds.");
        tone(a->cn_result_text, ZABBIX_TONE_QUIET);
        set_hidden(a->cn_result, false);
        set_hidden(a->cn_result_text, false);
    }
}

static void repaint(struct zabbix_app *a, bool force_lists)
{
    int64_t now = now_ms();
    const char *hint = a->model->fake ? "SIMULATED" : "";

    paint_tabs(a);
    paint_banner(a, now);
    if (a->conn_open) {
        paint_conn(a);
    } else if (a->detail_open) {
        paint_detail(a, now);
        a->dirty_detail = false;
    } else {
        switch (a->cur) {
        case TAB_OVERVIEW:
            paint_overview(a, now);
            break;
        case TAB_PROBLEMS:
            /* Every paint: the ages move each second. fill_prow writes only
             * the labels whose text differs, so an unchanged set costs a
             * string compare per label. */
            paint_problems(a, now);
            a->dirty_problems = false;
            break;
        case TAB_HOSTS:
            if (a->dirty_hosts || force_lists) {
                paint_hosts(a);
                a->dirty_hosts = false;
            }
            break;
        case TAB_STATUS:
        default:
            paint_status(a, now);
            break;
        }
    }
    /* Only a change: writing the hint repaints the header. */
    if (a->hint_shown != hint && (!a->hint_shown || strcmp(a->hint_shown, hint) != 0)) {
        pocketos_shell_set_status_hint(hint);
    }
    a->hint_shown = hint;
    a->painted_ms = now;
}

/* ---- navigation ----------------------------------------------------------------------- */

static void show_page(struct zabbix_app *a)
{
    int i;

    for (i = 0; i < TAB_COUNT; i++) {
        set_hidden(a->page[i], a->conn_open || a->detail_open || i != (int)a->cur);
    }
    set_hidden(a->detail, a->conn_open || !a->detail_open);
    set_hidden(a->conn, !a->conn_open);
    set_hidden(a->strip, a->conn_open);
}

static void select_tab(struct zabbix_app *a, enum zabbix_tab t)
{
    if (a->detail_open) {
        a->detail_open = false;
        zabbix_session_detail(a->session, NULL);
    }
    a->cur = t;
    show_page(a);
    repaint(a, true);
    lv_obj_scroll_to_y(a->page[t], 0, LV_ANIM_OFF);
}

static void open_detail(struct zabbix_app *a, const char *hostid, const char *name)
{
    snprintf(a->detail_hostid, sizeof(a->detail_hostid), "%s", hostid);
    snprintf(a->detail_name, sizeof(a->detail_name), "%s", name ? name : "");
    a->detail_open = true;
    zabbix_session_detail(a->session, hostid);
    show_page(a);
    repaint(a, true);
    lv_obj_scroll_to_y(a->detail, 0, LV_ANIM_OFF);
}

static void on_tab(lv_event_t *e)
{
    struct zabbix_app *a = lv_event_get_user_data(e);
    lv_obj_t *t = lv_event_get_target_obj(e);
    int i;

    for (i = 0; i < TAB_COUNT; i++) {
        if (a->tab[i] == t) {
            select_tab(a, (enum zabbix_tab)i);
            return;
        }
    }
}

static void on_back(lv_event_t *e)
{
    struct zabbix_app *a = lv_event_get_user_data(e);

    select_tab(a, a->cur);
}

static void on_refresh(lv_event_t *e)
{
    struct zabbix_app *a = lv_event_get_user_data(e);

    if (!zabbix_session_active(a->session)) {
        start_helper(a);
    } else {
        zabbix_session_refresh(a->session);
    }
    repaint(a, false);
}

static void on_try_demo(lv_event_t *e)
{
    struct zabbix_app *a = lv_event_get_user_data(e);

    a->demo = true;
    a->demo_index = 0;
    restart_helper(a);
    repaint(a, true);
}

static void on_leave_demo(lv_event_t *e)
{
    struct zabbix_app *a = lv_event_get_user_data(e);

    a->demo = false;
    restart_helper(a);
    select_tab(a, TAB_OVERVIEW);
}

static void on_scenario(lv_event_t *e)
{
    struct zabbix_app *a = lv_event_get_user_data(e);
    int i;

    /* The next one after the scenario the helper says it runs. */
    for (i = 0; i < DEMO_CYCLE; i++) {
        if (strcmp(demo_cycle[i], a->model->scenario) == 0) {
            break;
        }
    }
    a->demo_index = (i + 1) % DEMO_CYCLE;
    zabbix_session_scenario(a->session, demo_cycle[a->demo_index]);
}

/* ---- CONNECTION ------------------------------------------------------------------------ */

/* Empty a field that may hold a typed secret. LVGL keeps a password
 * field's text in one buffer of its own, which is overwritten here before
 * it is released; the buffers earlier edits left behind were freed by
 * LVGL unwiped (docs/apps/ZABBIX.md, Security). */
static void wipe_field(struct zabbix_app *a, lv_obj_t *ta)
{
    char *t = (char *)lv_textarea_get_text(ta);

    if (t && *t) {
        explicit_bzero(t, strlen(t));
    }
    a->cn_filling = true;
    lv_textarea_set_text(ta, "");
    a->cn_filling = false;
}

/* The fields from what the helper read in the files. */
static void fill_conn(struct zabbix_app *a)
{
    const struct zabbix_model *m = a->model;

    a->cn_filling = true;
    lv_textarea_set_text(a->cn_url, m->set_url);
    lv_textarea_set_text(a->cn_user, m->set_user);
    a->cn_filling = false;
    a->cn_password = m->set_password;
    a->cn_filled = true;
    a->cn_edited = false;
}

static void show_result(struct zabbix_app *a, const char *word, enum zabbix_tone t, const char *text)
{
    label_text(a->cn_result, word);
    tone(a->cn_result, t);
    label_text(a->cn_result_text, text);
    tone(a->cn_result_text, ZABBIX_TONE_PLAIN);
    set_hidden(a->cn_result, !word[0]);
    set_hidden(a->cn_result_text, !text[0]);
}

static void open_conn(struct zabbix_app *a)
{
    a->conn_open = true;
    a->cn_focus = NULL;
    a->cn_filled = false;
    a->cn_edited = false;
    wipe_field(a, a->cn_secret);
    if (a->model->have_settings) {
        fill_conn(a);
    }
    if (a->cn_pending == CN_IDLE) {
        show_result(a, "", ZABBIX_TONE_QUIET, "");
    }
    show_page(a);
    repaint(a, true);
    lv_obj_scroll_to_y(a->conn, 0, LV_ANIM_OFF);
}

static void close_conn(struct zabbix_app *a)
{
    pocketos_shell_keyboard_hide();
    wipe_field(a, a->cn_secret);
    a->conn_open = false;
    a->cn_focus = NULL;
    show_page(a);
    repaint(a, true);
}

static void on_open_conn(lv_event_t *e)
{
    open_conn(lv_event_get_user_data(e));
}

static void on_close_conn(lv_event_t *e)
{
    close_conn(lv_event_get_user_data(e));
}

static void on_auth(lv_event_t *e)
{
    struct zabbix_app *a = lv_event_get_user_data(e);
    bool password = lv_event_get_current_target_obj(e) == a->cn_auth[1];

    if (password != a->cn_password) {
        a->cn_password = password;
        a->cn_edited = true;
        repaint(a, false);
    }
}

/* A finger on a field brings the keyboard. Only a finger: the keyboard's
 * own Done reaches the field as a click too, after on_field_ready put the
 * keyboard away (the Wave lesson). */
static void on_field_clicked(lv_event_t *e)
{
    struct zabbix_app *a = lv_event_get_user_data(e);
    lv_indev_t *src = lv_indev_active();

    if (!src || lv_indev_get_type(src) != LV_INDEV_TYPE_POINTER) {
        return;
    }
    a->cn_focus = lv_event_get_current_target_obj(e);
    pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, NULL);
    a->cn_reveal = true; /* in the timer, once the body has its new height */
}

static void on_field_ready(lv_event_t *e)
{
    (void)e;
    pocketos_shell_keyboard_hide();
}

static void on_field_changed(lv_event_t *e)
{
    struct zabbix_app *a = lv_event_get_user_data(e);

    if (!a->cn_filling) {
        a->cn_edited = true;
        if (a->conn_open) {
            paint_conn(a);
        }
    }
}

static void conn_send(struct zabbix_app *a, bool save)
{
    const char *url = lv_textarea_get_text(a->cn_url);
    const char *user = lv_textarea_get_text(a->cn_user);
    const char *secret = lv_textarea_get_text(a->cn_secret);

    if (a->cn_pending != CN_IDLE) {
        return;
    }
    pocketos_shell_keyboard_hide();
    if (zabbix_session_settings(a->session, save, url, a->cn_password ? "password" : "token",
                                a->cn_password ? user : "", secret) != 0) {
        show_result(a, "--", ZABBIX_TONE_QUIET,
                    zabbix_session_active(a->session)
                        ? "This could not be sent to the Zabbix helper. Nothing was changed."
                        : "The Zabbix helper is not running. Try again in a moment.");
        return;
    }
    a->cn_pending = save ? CN_SAVING : CN_TESTING;
    a->cn_deadline = now_ms() + ZABBIX_SETTINGS_WAIT_MS;
    repaint(a, false);
}

static void on_conn_test(lv_event_t *e)
{
    conn_send(lv_event_get_user_data(e), false);
}

static void on_conn_save(lv_event_t *e)
{
    conn_send(lv_event_get_user_data(e), true);
}

/* From the timer: an answer, a helper that went away, the fields once the
 * files are known. */
static void conn_poll(struct zabbix_app *a, int64_t now)
{
    const struct zabbix_model *m = a->model;

    if (m->cresult_seq != a->cn_seen) {
        struct zabbix_cresult_view v;

        a->cn_seen = m->cresult_seq;
        a->cn_pending = CN_IDLE;
        zabbix_view_cresult(m->cresult, m->cresult_save, m->cresult_saved, m->cresult_text, &v);
        show_result(a, v.word, v.tone, v.text);
        if (m->cresult_saved) {
            /* Stored: the secret typed is not needed on this side any more,
             * and the connection starts again on the new settings. */
            wipe_field(a, a->cn_secret);
            a->cn_restart = true;
        }
    } else if (a->cn_pending != CN_IDLE && (now >= a->cn_deadline || !m->helper_running)) {
        a->cn_pending = CN_IDLE;
        show_result(a, "--", ZABBIX_TONE_ERROR,
                    "No answer from the Zabbix helper. Open this screen again to see what is stored.");
        if (zabbix_session_active(a->session)) {
            a->cn_restart = true;
        }
    }
    if (a->cn_restart) {
        a->cn_restart = false;
        a->demo = false; /* settings for a real server are meant to be used */
        restart_helper(a);
        a->cn_seen = 0;
        a->cn_filled = false;
        a->cn_edited = false;
    }
    if (a->conn_open && !a->cn_filled && !a->cn_edited && m->have_settings) {
        fill_conn(a);
    }
}

/* ---- the timer ------------------------------------------------------------------------ */

static void layout(struct zabbix_app *a);

static void on_poll(lv_timer_t *t)
{
    struct zabbix_app *a = lv_timer_get_user_data(t);
    int64_t now = now_ms();
    unsigned changed;

    if (a->relayout) {
        a->relayout = false;
        layout(a);
        /* The keyboard came or went: the field being typed in stays in
         * view. */
        a->cn_reveal = a->cn_reveal || (a->conn_open && a->cn_focus);
    }
    if (a->cn_reveal) {
        a->cn_reveal = false;
        if (a->conn_open && a->cn_focus) {
            lv_obj_update_layout(a->frame);
            lv_obj_scroll_to_view_recursive(lv_obj_get_parent(a->cn_focus), LV_ANIM_OFF);
        }
    }
    changed = zabbix_session_poll(a->session, a->model, now);
    if (changed & (ZABBIX_CHANGED_SETTINGS | ZABBIX_CHANGED_EXITED)) {
        conn_poll(a, now);
    } else if (a->cn_pending != CN_IDLE || a->cn_restart) {
        conn_poll(a, now); /* the deadline */
    }
    if (changed & ZABBIX_CHANGED_EXITED) {
        LOG_WARN("zabbix: helper ended (%d), reason %d", a->model->exit_value, a->model->exit_reason);
    }
    if (!zabbix_session_active(a->session) && a->model->restart_at_ms &&
        now >= a->model->restart_at_ms) {
        start_helper(a);
        changed |= ZABBIX_CHANGED_STATE;
    }
    if (changed & ZABBIX_CHANGED_PROBLEMS) {
        a->dirty_problems = true;
    }
    if (changed & ZABBIX_CHANGED_HOSTS) {
        a->dirty_hosts = true;
    }
    if (changed & ZABBIX_CHANGED_DETAIL) {
        a->dirty_detail = true;
    }
    if (changed || now - a->painted_ms >= ZABBIX_PAINT_MS) {
        repaint(a, false);
    }
}

/* ---- layout ----------------------------------------------------------------------------- */

static void layout(struct zabbix_app *a)
{
    struct pos_insets in;
    const lv_area_t *area;
    int32_t w;
    int32_t h;

    if (!pocketui_layout_begin(&a->guard, a->frame, &in)) {
        return;
    }
    area = &a->guard.area;
    w = lv_area_get_width(area);
    h = lv_area_get_height(area);
    a->wide = w > h;
    /* The corner rule of DS section 21.3: nothing inside the unsafe corners.
     * The strip and every page start inside the insets. */
    lv_obj_set_style_pad_left(a->frame, in.left, 0);
    lv_obj_set_style_pad_right(a->frame, in.right, 0);
    lv_obj_set_style_pad_top(a->frame, in.top, 0);
    lv_obj_set_style_pad_bottom(a->frame, in.bottom, 0);
    /* OVERVIEW: in the wide shape the headline and the severities share a
     * line; in the tall one everything is a column. */
    lv_obj_set_width(a->ov_head, a->wide ? LV_PCT(48) : LV_PCT(100));
    lv_obj_set_width(a->ov_sev, a->wide ? LV_PCT(48) : LV_PCT(100));
    /* Six severities in a line when there is room for "DISASTER" in each
     * (wide), three by two otherwise: a 74 px cell cut it to "DISAS...". */
    {
        int s;
        bool six = a->wide;

        for (s = 0; s < ZBX_SEV_COUNT; s++) {
            lv_obj_set_width(a->ov_sev_cell[s], six ? LV_PCT(15) : LV_PCT(31));
        }
        /* Whether the words fit six in a line is measured, not assumed from
         * the shape: at a larger text size (DS §46) they may not, and three
         * by two is the shape that has room for them. Outside a layout pass
         * (the timer runs this), so laying out to measure is allowed. */
        if (six) {
            lv_obj_update_layout(a->frame);
            for (s = 0; s < ZBX_SEV_COUNT && six; s++) {
                /* The word itself: a label in the dots mode has already
                 * shortened its own text to fit. */
                six = pocketui_text_width(a->ov_sev_word[s], zbx_severity_word(ZBX_SEV_COUNT - 1 - s)) <=
                      lv_obj_get_content_width(a->ov_sev_word[s]);
            }
            for (s = 0; s < ZBX_SEV_COUNT && !six; s++) {
                lv_obj_set_width(a->ov_sev_cell[s], LV_PCT(31));
            }
        }
    }
    lv_obj_invalidate(a->frame);
}

static void on_frame_size(lv_event_t *e)
{
    struct zabbix_app *a = lv_event_get_user_data(e);

    /* Never lay out inside a layout pass: the timer does it. */
    a->relayout = true;
}

/* ---- building --------------------------------------------------------------------------- */

static lv_obj_t *page(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);

    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 0, 0);
    lv_obj_set_style_pad_top(p, GAP, 0);
    lv_obj_set_style_pad_bottom(p, POCKETUI_PAD, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    /* Clickable, with nothing to click: LVGL finds only clickable objects
     * under a finger, so a drag that starts on text or a card (nothing
     * clickable) must find the page, or it scrolls nothing - on unit A the
     * buttons at the foot of STATUS could not be reached in landscape. */
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return p;
}

static void build_strip(struct zabbix_app *a)
{
    int i;

    a->strip = box(a->frame);
    pos_style_add(a->strip, POS_STYLE_DIVIDER, 0);
    lv_obj_set_width(a->strip, LV_PCT(100));
    lv_obj_set_height(a->strip, TAB_H);
    lv_obj_set_flex_flow(a->strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(a->strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(a->strip, TAB_GAP, 0);
    for (i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *t = box(a->strip);

        /* Four equal tabs across the body: each is a full 64 px target (a
         * tab only as wide as "HOSTS" would be narrower than DS §7 allows). */
        lv_obj_set_height(t, TAB_H);
        grow(t);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, on_tab, LV_EVENT_CLICKED, a);
        a->tab[i] = t;
        a->tab_label[i] = lv_label_create(t);
        lv_obj_remove_style_all(a->tab_label[i]);
        pos_style_add(a->tab_label[i], POS_STYLE_CAPTION, 0);
        lv_label_set_text(a->tab_label[i], tab_name[i]);
        lv_obj_set_style_pad_bottom(a->tab_label[i], 16, 0);
        a->tab_rule[i] = box(t);
        /* The 2 px underline in the accent: a fill role, not a colour. */
        pos_style_add(a->tab_rule[i], POS_STYLE_BUTTON_PRIMARY, 0);
        lv_obj_set_style_radius(a->tab_rule[i], 0, 0);
        lv_obj_set_width(a->tab_rule[i], LV_PCT(100));
        lv_obj_set_height(a->tab_rule[i], UNDERLINE_H);
        lv_obj_set_style_opa(a->tab_rule[i], LV_OPA_TRANSP, 0);
    }
}

/* A list's heading: what the list is on the left, how fresh on the right. */
static lv_obj_t *list_title(lv_obj_t *parent, const char *text, lv_obj_t **age)
{
    lv_obj_t *l = row_box(parent, GAP);
    lv_obj_t *t;

    lv_obj_set_style_pad_hor(l, ROW_PAD, 0);
    lv_obj_set_style_pad_bottom(l, 6, 0);
    t = grow(one_line(l, text, POS_STYLE_CAPTION));
    /* Two lines before the dots: at a larger text size "100 of 1 200 open ·
     * most severe first" lost its point to them on one (DS §46.5). One line
     * where one line holds it, as it always was. */
    lv_obj_set_height(t, LV_SIZE_CONTENT);
    pocketui_label_fit(t, 2);
    *age = one_line(l, "", POS_STYLE_CAPTION);
    lv_obj_set_width(*age, LV_SIZE_CONTENT);
    lv_label_set_long_mode(*age, LV_LABEL_LONG_CLIP);
    return t;
}

static void build_overview(struct zabbix_app *a)
{
    lv_obj_t *p = a->page[TAB_OVERVIEW];
    lv_obj_t *l;
    int s;

    lv_obj_set_style_pad_hor(p, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_row(p, GAP, 0);

    a->ov_setup = column(p, GAP);
    pocketui_label(a->ov_setup, "Not set up", POS_STYLE_TITLE);
    a->ov_setup_text = wrapping(a->ov_setup, "", POS_STYLE_TEXT_SECONDARY);
    {
        lv_obj_t *b = button(a->ov_setup, "SET UP THE CONNECTION", true, on_open_conn, a);

        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_set_style_max_width(b, 420, 0);
    }
    a->ov_demo = button(a->ov_setup, "TRY THE DEMO", false, on_try_demo, a);
    lv_obj_set_width(a->ov_demo, 280);
    lv_obj_add_flag(a->ov_setup, LV_OBJ_FLAG_HIDDEN);

    a->ov_cards = box(p);
    lv_obj_set_size(a->ov_cards, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(a->ov_cards, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(a->ov_cards, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(a->ov_cards, GAP, 0);

    /* The headline: the worst open severity, in its own words and tone. */
    a->ov_head = pocketui_card(a->ov_cards);
    lv_obj_set_width(a->ov_head, LV_PCT(100));
    lv_obj_set_style_pad_row(a->ov_head, 4, 0);
    pocketui_label(a->ov_head, "HIGHEST OPEN SEVERITY", POS_STYLE_CAPTION);
    a->ov_headline = one_line(a->ov_head, "--", POS_STYLE_HERO_40);
    lv_obj_set_width(a->ov_headline, LV_PCT(100));
    a->ov_head_note = wrapping(a->ov_head, "", POS_STYLE_TEXT_SECONDARY);

    /* Every severity's count, disaster first. */
    a->ov_sev = pocketui_card(a->ov_cards);
    lv_obj_set_width(a->ov_sev, LV_PCT(100));
    pocketui_label(a->ov_sev, "OPEN PROBLEMS BY SEVERITY", POS_STYLE_CAPTION);
    l = row_box(a->ov_sev, 8);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(l, GAP, 0);
    for (s = 0; s < ZBX_SEV_COUNT; s++) {
        int sev = ZBX_SEV_COUNT - 1 - s;

        a->ov_sev_cell[s] = column(l, 2);
        lv_obj_set_width(a->ov_sev_cell[s], LV_PCT(31));
        a->ov_sev_value[s] = one_line(a->ov_sev_cell[s], "--", POS_STYLE_VALUE);
        lv_obj_set_width(a->ov_sev_value[s], LV_PCT(100));
        a->ov_sev_word[s] = one_line(a->ov_sev_cell[s], zbx_severity_word(sev), POS_STYLE_CAPTION);
        lv_obj_set_width(a->ov_sev_word[s], LV_PCT(100));
    }

    /* The counts and where they come from. */
    a->ov_counts = pocketui_card(a->ov_cards);
    lv_obj_set_width(a->ov_counts, LV_PCT(100));
    lv_obj_set_style_pad_row(a->ov_counts, 6, 0);
    a->ov_problems = wrapping(a->ov_counts, "", POS_STYLE_ROW_TITLE);
    a->ov_hosts = wrapping(a->ov_counts, "", POS_STYLE_ROW_TITLE);
    a->ov_hosts_note = wrapping(a->ov_counts, "", POS_STYLE_TEXT_SECONDARY);
    a->ov_server = wrapping(a->ov_counts, "", POS_STYLE_TEXT_SECONDARY);
    a->ov_updated = one_line(a->ov_counts, "", POS_STYLE_CAPTION);
    lv_obj_set_width(a->ov_updated, LV_PCT(100));

    /* The heading of the most severe problems; their rows follow it, made
     * when there are problems to show. */
    a->ov_top_cap = one_line(p, "MOST SEVERE OPEN PROBLEMS", POS_STYLE_CAPTION);
    lv_obj_set_width(a->ov_top_cap, LV_PCT(100));
    lv_obj_set_style_margin_top(a->ov_top_cap, GAP, 0);
    lv_obj_add_flag(a->ov_top_cap, LV_OBJ_FLAG_HIDDEN);
}

static void build_lists(struct zabbix_app *a)
{
    lv_obj_t *p;

    p = a->page[TAB_PROBLEMS];
    a->pr_title = list_title(p, "PROBLEMS", &a->pr_age);
    a->pr_empty = wrapping(p, "", POS_STYLE_TEXT_SECONDARY);
    lv_obj_set_style_pad_all(a->pr_empty, ROW_PAD, 0);

    p = a->page[TAB_HOSTS];
    a->ho_title = list_title(p, "HOSTS", &a->ho_age);
    a->ho_empty = wrapping(p, "", POS_STYLE_TEXT_SECONDARY);
    lv_obj_set_style_pad_all(a->ho_empty, ROW_PAD, 0);
}

static void build_detail(struct zabbix_app *a)
{
    lv_obj_t *d;

    d = a->detail = page(a->content);
    lv_obj_set_style_pad_hor(d, 0, 0);
    lv_obj_set_style_pad_row(d, 4, 0);
    a->dt_back = button(d, "\xe2\x80\xb9 BACK", false, on_back, a);
    lv_obj_set_width(a->dt_back, 180);
    lv_obj_set_style_margin_left(a->dt_back, ROW_PAD, 0);
    a->dt_name = wrapping(d, "", POS_STYLE_TITLE);
    lv_obj_set_style_pad_hor(a->dt_name, ROW_PAD, 0);
    lv_obj_set_style_pad_top(a->dt_name, GAP, 0);
    a->dt_state = wrapping(d, "", POS_STYLE_TEXT_PRIMARY);
    lv_obj_set_style_pad_hor(a->dt_state, ROW_PAD, 0);
    a->dt_note = wrapping(d, "No open problems on this host.", POS_STYLE_TEXT_SECONDARY);
    lv_obj_set_style_pad_all(a->dt_note, ROW_PAD, 0);
    a->dt_pcap = one_line(d, "OPEN PROBLEMS", POS_STYLE_CAPTION);
    lv_obj_set_width(a->dt_pcap, LV_PCT(100));
    lv_obj_set_style_pad_hor(a->dt_pcap, ROW_PAD, 0);
    lv_obj_set_style_margin_top(a->dt_pcap, GAP, 0);
    a->dt_icap = one_line(d, "LATEST VALUES", POS_STYLE_CAPTION);
    lv_obj_set_width(a->dt_icap, LV_PCT(100));
    lv_obj_set_style_pad_hor(a->dt_icap, ROW_PAD, 0);
    lv_obj_set_style_margin_top(a->dt_icap, GAP, 0);
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
}

static void build_status(struct zabbix_app *a)
{
    lv_obj_t *p = a->page[TAB_STATUS];
    lv_obj_t *buttons;
    int i;

    for (i = 0; i < ZABBIX_STATUS_LINES; i++) {
        struct srow *r = &a->srows[i];

        r->row = list_row(p, NULL, NULL);
        {
            lv_obj_t *l = row_box(r->row, GAP);

            lv_obj_set_flex_align(l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
            r->key = one_line(l, "", POS_STYLE_CAPTION);
            lv_obj_set_width(r->key, KEY_W);
            r->value = grow(pocketui_label(l, "", POS_STYLE_TEXT_PRIMARY));
            lv_label_set_long_mode(r->value, LV_LABEL_LONG_WRAP);
        }
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    }
    buttons = column(p, GAP);
    lv_obj_set_style_pad_all(buttons, ROW_PAD, 0);
    a->st_refresh = button(buttons, "REFRESH NOW", true, on_refresh, a);
    a->st_conn = button(buttons, "CONNECTION SETTINGS", false, on_open_conn, a);
    a->st_scenario = button(buttons, "SCENARIO", false, on_scenario, a);
    a->st_demo = button(buttons, "LEAVE THE DEMO", false, on_leave_demo, a);
    lv_obj_add_flag(a->st_scenario, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a->st_demo, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *cn_caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *c = one_line(parent, text, POS_STYLE_CAPTION);

    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_style_margin_top(c, GAP, 0);
    return c;
}

static lv_obj_t *cn_field(struct zabbix_app *a, lv_obj_t *parent, const char *placeholder,
                          uint32_t max)
{
    lv_obj_t *f = pocketui_text_field(parent, placeholder, true);

    lv_textarea_set_max_length(f, max);
    lv_obj_add_event_cb(f, on_field_clicked, LV_EVENT_CLICKED, a);
    lv_obj_add_event_cb(f, on_field_ready, LV_EVENT_READY, a);
    lv_obj_add_event_cb(f, on_field_changed, LV_EVENT_VALUE_CHANGED, a);
    return f;
}

/* CONNECTION: one column that scrolls, in both shapes. With the keyboard up
 * the field being typed in is brought into view (on_poll). */
static void build_conn(struct zabbix_app *a)
{
    lv_obj_t *p;
    lv_obj_t *l;
    lv_obj_t *card;
    lv_obj_t *buttons;

    p = a->conn = page(a->content);
    lv_obj_set_style_pad_hor(p, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_row(p, 8, 0);
    a->cn_back = button(p, "\xe2\x80\xb9 BACK", false, on_close_conn, a);
    lv_obj_set_width(a->cn_back, 180);
    pocketui_label(p, "Connection", POS_STYLE_TITLE);
    wrapping(p, "The Zabbix server this unit shows, and how it signs in.", POS_STYLE_TEXT_SECONDARY);

    cn_caption(p, "SERVER");
    a->cn_url = cn_field(a, p, "https://zabbix.example.com/", CN_URL_MAX);

    cn_caption(p, "SIGN IN WITH");
    l = row_box(p, GAP);
    a->cn_auth[0] = grow(button(l, "API TOKEN", false, on_auth, a));
    a->cn_auth[1] = grow(button(l, "PASSWORD", false, on_auth, a));
    a->cn_choice_shown = -1;

    a->cn_user_cap = cn_caption(p, "USER");
    a->cn_user = cn_field(a, p, "User name", CN_USER_MAX);

    a->cn_secret_cap = cn_caption(p, "API TOKEN");
    a->cn_secret = cn_field(a, p, "API token", CN_SECRET_MAX);
    /* Masked, and never a character shown, not even the last one typed. */
    lv_textarea_set_password_mode(a->cn_secret, true);
    lv_textarea_set_password_show_time(a->cn_secret, 0);
    a->cn_secret_note = wrapping(p, "", POS_STYLE_CAPTION);

    card = pocketui_card(p);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_style_margin_top(card, GAP, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    pocketui_label(card, "RESULT", POS_STYLE_CAPTION);
    a->cn_result = one_line(card, "", POS_STYLE_TITLE);
    lv_obj_set_width(a->cn_result, LV_PCT(100));
    a->cn_result_text = wrapping(card, "Nothing tested yet.", POS_STYLE_TEXT_PRIMARY);
    set_hidden(a->cn_result, true);

    buttons = column(p, GAP);
    lv_obj_set_style_margin_top(buttons, GAP, 0);
    a->cn_test = button(buttons, "TEST CONNECTION", false, on_conn_test, a);
    lv_obj_set_width(a->cn_test, LV_PCT(100));
    a->cn_save = button(buttons, "SAVE", true, on_conn_save, a);
    lv_obj_set_width(a->cn_save, LV_PCT(100));
    wrapping(p, "SAVE tests first and stores only settings that connect; until then the "
                "previous ones stay in use. Everything else in zabbix.conf is kept.",
             POS_STYLE_CAPTION);
    a->cn_file_note = wrapping(p, "", POS_STYLE_CAPTION);
    tone(a->cn_file_note, ZABBIX_TONE_WARN);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
}

static void build(struct zabbix_app *a, lv_obj_t *root)
{
    int i;

    a->frame = lv_obj_create(root);
    lv_obj_remove_style_all(a->frame);
    /* Exactly the body's content box, so the shape is chosen from the room
     * the shell gives. */
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(a->frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    build_strip(a);

    /* The banner: shown whenever the data is not simply fresh. */
    a->banner = box(a->frame);
    pos_style_add(a->banner, POS_STYLE_SLAB, 0);
    lv_obj_set_width(a->banner, LV_PCT(100));
    lv_obj_set_height(a->banner, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(a->banner, BANNER_PAD, 0);
    lv_obj_set_style_margin_top(a->banner, 8, 0);
    a->banner_label = wrapping(a->banner, "", POS_STYLE_TEXT_PRIMARY);
    lv_obj_add_flag(a->banner, LV_OBJ_FLAG_HIDDEN);

    a->content = box(a->frame);
    lv_obj_set_width(a->content, LV_PCT(100));
    lv_obj_set_flex_grow(a->content, 1);
    for (i = 0; i < TAB_COUNT; i++) {
        a->page[i] = page(a->content);
    }
    build_overview(a);
    build_lists(a);
    build_detail(a);
    build_status(a);
    build_conn(a);
}

/* ---- the app ---------------------------------------------------------------------------- */

static void *zabbix_create(lv_obj_t *root)
{
    struct zabbix_app *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    a->session = malloc(sizeof(*a->session));
    a->model = malloc(sizeof(*a->model));
    a->prows = calloc(ZBX_PROBLEM_MAX, sizeof(*a->prows));
    a->hrows = calloc(ZBX_HOST_MAX, sizeof(*a->hrows));
    if (!a->session || !a->model || !a->prows || !a->hrows) {
        free(a->session);
        free(a->model);
        free(a->prows);
        free(a->hrows);
        free(a);
        return NULL;
    }
    zabbix_session_init(a->session);
    zabbix_model_init(a->model);
    build(a, root);
    pocketos_shell_set_status_hint("");
    a->hint_shown = "";
    a->cur = TAB_OVERVIEW;
    show_page(a);
    a->timer = lv_timer_create(on_poll, ZABBIX_POLL_MS, a);
    /* Only now: building lays objects out as it goes. */
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    layout(a);
    /* The connection starts with the screen and never outlives it. */
    start_helper(a);
    repaint(a, true);
    return a;
}

static void zabbix_destroy(void *priv)
{
    struct zabbix_app *a = priv;

    if (!a) {
        return;
    }
    /* The frame outlives this by a moment, until the shell deletes the app's
     * objects; nothing may call back into a freed app in between. */
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    /* A token or password typed and not saved goes with the screen. */
    if (a->cn_secret) {
        pocketos_shell_keyboard_hide();
        wipe_field(a, a->cn_secret);
    }
    /* Leaving the app ends the connection. */
    zabbix_session_abandon(a->session, ZABBIX_DESTROY_GRACE_MS);
    free(a->session);
    free(a->model);
    free(a->prows);
    free(a->hrows);
    free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_zabbix);

/* The Back action (app.h `back`, hw_actions.h): CONNECTION and a host's
 * detail close as their "‹ BACK" buttons close them; the tabs themselves are
 * one level. */
static int zabbix_back(void *priv)
{
    struct zabbix_app *a = priv;

    if (a && a->conn_open) {
        close_conn(a);
        return 1;
    }
    if (!a || !a->detail_open) {
        return 0;
    }
    select_tab(a, a->cur);
    return 1;
}

const struct pocketos_app app_zabbix = {
    .id = "zabbix",
    .name = "Zabbix",
    .icon = LV_SYMBOL_EYE_OPEN,
    /* A first-party icon in the extension's line language: a screen with a
     * pulse (docs/design/doors-app-icons/README.md). */
    .icon_mask = &pos_app_icon_zabbix,
    .create = zabbix_create,
    .tick = NULL,
    .destroy = zabbix_destroy,
    /* Fullscreen (DS section 30.8): the lists take the height. The hint
     * shows in the header. */
    .chrome = POCKETOS_CHROME_NONE,
    .back = zabbix_back,
};
