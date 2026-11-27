/*
 * Notices. See shell_notice.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "shell_notice.h"

#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pos_styles.h"
#include "shell_lock.h"

#include <stdio.h>
#include <string.h>

#define NOTICE_MAX_W 600
#define NOTICE_SIDE 16   /* the least it keeps from each side of the screen */
#define NOTICE_TOP 10    /* and from the top edge */
#define NOTICE_RULE_W 4
#define NOTICE_IN_MS 240
#define NOTICE_OUT_MS 180

struct notice {
    char app_id[32];
    char source[NOTICE_SOURCE_MAX];
    char title[NOTICE_TITLE_MAX];
    char body[NOTICE_BODY_MAX];
    int accent;
    lv_style_t *title_style;
    lv_style_t *body_style;
    void (*on_open)(void *user);
    void *user;
    int hold_ms;        /* alone on screen */
    int hold_queued_ms; /* with another waiting behind it */
    bool has_arg;       /* on_open is given arg, the notice's own copy */
    char arg[NOTICE_ARG_MAX];
};

static struct {
    lv_obj_t *box;
    lv_obj_t *rule;
    lv_obj_t *source;
    lv_obj_t *title;
    lv_obj_t *body;
    lv_timer_t *timer;
    struct notice now;     /* the one on screen, or the last one shown */
    struct notice queue[NOTICE_QUEUE];
    int queued;
    bool visible;
    bool leaving;          /* sliding out: the next one follows it */
    unsigned posted;
    unsigned dropped;
    /* A tap's action, taken outside the event that saw it. */
    struct notice opening;
    bool open_pending;
} nt;

static void show_next(void);

static void copy(char *out, size_t out_len, const char *in)
{
    size_t n;

    snprintf(out, out_len, "%s", in ? in : "");
    /* Never half a UTF-8 character at the cut. */
    n = strlen(out);
    while (n > 0 && n == out_len - 1 && ((unsigned char)out[n - 1] & 0xC0) == 0x80) {
        out[--n] = '\0';
    }
    if (n > 0 && n == out_len - 1 && ((unsigned char)out[n - 1] & 0xC0) == 0xC0) {
        out[--n] = '\0';
    }
}

static void rule_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_rect_dsc_t dsc;
    lv_area_t a;

    if (!layer) {
        return;
    }
    lv_obj_get_coords(obj, &a);
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 2;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = nt.now.accent >= 0 ? pos_identity_hue((unsigned)nt.now.accent)
                                      : pos_theme_color(POS_COLOR_ACCENT_PRIMARY);
    lv_draw_rect(layer, &dsc, &a);
}

static void open_async(void *unused)
{
    (void)unused;
    if (!nt.open_pending) {
        return;
    }
    nt.open_pending = false;
    if (nt.opening.on_open) {
        /* The tapped notice's own copy (nt.opening is one), never a pointer
         * into the app that a later notice may have reused. */
        nt.opening.on_open(nt.opening.has_arg ? (void *)nt.opening.arg : nt.opening.user);
    } else if (nt.opening.app_id[0]) {
        (void)pocketos_shell_open_app(nt.opening.app_id);
    }
}

static void set_y(void *obj, int32_t y)
{
    lv_obj_set_y(obj, y);
}

static void hidden_done(lv_anim_t *a)
{
    (void)a;
    nt.leaving = false;
    nt.visible = false;
    if (nt.box) {
        lv_obj_add_flag(nt.box, LV_OBJ_FLAG_HIDDEN);
    }
    show_next();
}

static void leave(void)
{
    lv_anim_t a;

    if (!nt.box || !nt.visible || nt.leaving) {
        return;
    }
    if (nt.timer) {
        lv_timer_pause(nt.timer);
    }
    lv_anim_delete(nt.box, set_y);
    if (pocketos_shell_reduced_motion()) {
        hidden_done(NULL);
        return;
    }
    nt.leaving = true;
    lv_anim_init(&a);
    lv_anim_set_var(&a, nt.box);
    lv_anim_set_exec_cb(&a, set_y);
    lv_anim_set_values(&a, lv_obj_get_y(nt.box), -lv_obj_get_height(nt.box) - 4);
    lv_anim_set_duration(&a, NOTICE_OUT_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, hidden_done);
    lv_anim_start(&a);
}

static void on_hold(lv_timer_t *t)
{
    (void)t;
    leave();
}

static void on_tap(lv_event_t *e)
{
    (void)e;
    /* Read, not acted on, while the lock is engaged. */
    if (shell_lock_is_locked()) {
        return;
    }
    nt.opening = nt.now;
    nt.open_pending = true;
    lv_async_call(open_async, NULL);
    leave();
}

static lv_obj_t *line(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_remove_style_all(label);
    pos_style_add(label, role, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text(label, "");
    return label;
}

static void build(void)
{
    lv_obj_t *words;

    nt.box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(nt.box);
    /* A raised face with a hairline, as a secondary button has: it is one
     * thing that can be pressed. */
    pos_style_add(nt.box, POS_STYLE_BUTTON_SECONDARY, 0);
    pos_style_add(nt.box, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_height(nt.box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nt.box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(nt.box, 12, 0);
    lv_obj_set_style_pad_column(nt.box, 12, 0);
    lv_obj_remove_flag(nt.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(nt.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(nt.box, on_tap, LV_EVENT_CLICKED, NULL);

    nt.rule = lv_obj_create(nt.box);
    lv_obj_remove_style_all(nt.rule);
    lv_obj_set_width(nt.rule, NOTICE_RULE_W);
    lv_obj_remove_flag(nt.rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(nt.rule, rule_draw, LV_EVENT_DRAW_MAIN, NULL);

    words = lv_obj_create(nt.box);
    lv_obj_remove_style_all(words);
    lv_obj_set_flex_grow(words, 1);
    lv_obj_set_height(words, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(words, 2, 0);
    lv_obj_remove_flag(words, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(words, LV_OBJ_FLAG_CLICKABLE);
    nt.source = line(words, POS_STYLE_CAPTION);
    nt.title = line(words, POS_STYLE_ROW_TITLE);
    nt.body = line(words, POS_STYLE_TEXT_SECONDARY);
    nt.timer = lv_timer_create(on_hold, NOTICE_HOLD_MS, NULL);
    lv_timer_pause(nt.timer);
}

/* Put `n` on screen. The labels' own extra styles (an app's emoji font) are
 * taken off and put on per notice, so one app's do not dress another's. */
static void present(const struct notice *n)
{
    lv_display_t *disp = lv_display_get_default();
    int32_t screen_w = disp ? lv_display_get_horizontal_resolution(disp) : 568;
    int32_t w = screen_w - 2 * NOTICE_SIDE;
    lv_anim_t a;

    if (!nt.box) {
        build();
    }
    if (nt.now.title_style) {
        lv_obj_remove_style(nt.title, nt.now.title_style, 0);
    }
    if (nt.now.body_style) {
        lv_obj_remove_style(nt.body, nt.now.body_style, 0);
    }
    nt.now = *n;
    if (n->title_style) {
        lv_obj_add_style(nt.title, n->title_style, 0);
    }
    if (n->body_style) {
        lv_obj_add_style(nt.body, n->body_style, 0);
    }
    if (w > NOTICE_MAX_W) {
        w = NOTICE_MAX_W;
    }
    lv_obj_set_width(nt.box, w);
    lv_label_set_text(nt.source, n->source);
    lv_label_set_text(nt.title, n->title);
    lv_label_set_text(nt.body, n->body);
    if (n->body[0]) {
        lv_obj_remove_flag(nt.body, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(nt.body, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(nt.box, LV_OBJ_FLAG_HIDDEN);
    /* The rule is as tall as the words, which are not measured yet: out of
     * their way first, so the last notice's height does not hold this one. */
    lv_obj_set_height(nt.rule, 1);
    lv_obj_update_layout(nt.box);
    /* One line each for who says it and who it is about, two for the words;
     * a longer text ends in "...". Fitted once the width is settled. */
    pocketui_label_fit(nt.source, 1);
    pocketui_label_fit(nt.title, 1);
    pocketui_label_fit(nt.body, 2);
    lv_obj_update_layout(nt.box);
    lv_obj_set_height(nt.rule, lv_obj_get_content_height(nt.box));
    lv_obj_set_x(nt.box, (screen_w - w) / 2);
    lv_obj_move_foreground(nt.box);
    lv_obj_invalidate(nt.box);
    nt.visible = true;
    nt.leaving = false;
    lv_anim_delete(nt.box, set_y);
    if (pocketos_shell_reduced_motion()) {
        lv_obj_set_y(nt.box, NOTICE_TOP);
    } else {
        lv_obj_set_y(nt.box, -lv_obj_get_height(nt.box) - 4);
        lv_anim_init(&a);
        lv_anim_set_var(&a, nt.box);
        lv_anim_set_exec_cb(&a, set_y);
        lv_anim_set_values(&a, -lv_obj_get_height(nt.box) - 4, NOTICE_TOP);
        lv_anim_set_duration(&a, NOTICE_IN_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    lv_timer_set_period(nt.timer, (uint32_t)(nt.queued > 0 ? n->hold_queued_ms : n->hold_ms));
    lv_timer_reset(nt.timer);
    lv_timer_resume(nt.timer);
}

static void show_next(void)
{
    struct notice n;

    if (nt.queued <= 0 || nt.visible) {
        return;
    }
    n = nt.queue[0];
    memmove(&nt.queue[0], &nt.queue[1], sizeof(nt.queue[0]) * (size_t)(nt.queued - 1));
    nt.queued--;
    present(&n);
}

void shell_notice_post(const struct pocketos_notice *in)
{
    struct notice n;

    if (!in || !in->title || !in->title[0]) {
        return;
    }
    memset(&n, 0, sizeof(n));
    copy(n.app_id, sizeof(n.app_id), in->app_id);
    copy(n.source, sizeof(n.source), in->source);
    copy(n.title, sizeof(n.title), in->title);
    copy(n.body, sizeof(n.body), in->body);
    n.accent = in->accent;
    n.title_style = in->title_style;
    n.body_style = in->body_style;
    n.on_open = in->on_open;
    n.user = in->user;
    if (in->arg) {
        n.has_arg = true;
        copy(n.arg, sizeof(n.arg), in->arg);
    }
    shell_notice_holds(in->hold_ms, in->hold_queued_ms, &n.hold_ms, &n.hold_queued_ms);
    nt.posted++;
    LOG_INFO("notice: %s (%s)", n.source[0] ? n.source : "-", n.app_id[0] ? n.app_id : "-");
    if (!nt.visible) {
        present(&n);
        return;
    }
    if (nt.queued == NOTICE_QUEUE) {
        /* The oldest waiting one makes room: newer news is the news. */
        memmove(&nt.queue[0], &nt.queue[1], sizeof(nt.queue[0]) * (NOTICE_QUEUE - 1));
        nt.queued--;
        nt.dropped++;
    }
    nt.queue[nt.queued++] = n;
    /* The one on screen gives way sooner, by its own measure. */
    if (!nt.leaving && nt.timer) {
        lv_timer_set_period(nt.timer, (uint32_t)nt.now.hold_queued_ms);
    }
}

void shell_notice_dismiss_all(void)
{
    nt.queued = 0;
    leave();
}

bool shell_notice_visible(void)
{
    return nt.visible && !nt.leaving;
}

unsigned shell_notice_posted(void)
{
    return nt.posted;
}

unsigned shell_notice_dropped(void)
{
    return nt.dropped;
}

const char *shell_notice_title(void)
{
    return nt.now.title;
}

const char *shell_notice_body(void)
{
    return nt.now.body;
}

void shell_notice_shutdown(void)
{
    nt.queued = 0;
    nt.open_pending = false;
    lv_async_call_cancel(open_async, NULL);
    if (nt.box) {
        lv_anim_delete(nt.box, set_y);
    }
    if (nt.timer) {
        lv_timer_delete(nt.timer);
        nt.timer = NULL;
    }
}
