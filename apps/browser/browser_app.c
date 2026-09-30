/*
 * Browser: simple web pages on the unit - read, follow links, go back and
 * forward (docs/apps/BROWSER.md, ADR-009 accepted).
 *
 * This file is the screen and nothing else. What it shows and allows is
 * browser_view.c; the network, TLS, the HTML reader and the picture
 * decoders are not in the shell at all but in a pos-browser helper process
 * (browser_session.c), polled from an LVGL timer that only ever makes
 * non-blocking calls. The only waits on the LVGL thread are destroy() giving
 * the helper BROWSER_DESTROY_GRACE_MS to leave, and reading a finished
 * picture file from the runtime tmpfs.
 *
 * THE SCREEN, top to bottom:
 *
 *   portrait   the address field and GO; a status line; the page, which
 *              scrolls; BACK, FORWARD, RELOAD/STOP, HOME and BOOKMARK at the
 *              foot, by the thumb.
 *   landscape  the five buttons, the address field, GO and KEYS in one row;
 *              the status line; the page. With the touch keyboard up only
 *              the row is left (the body is 156 px then).
 *
 * THE PAGE is a column of LVGL objects, one per block of the document: a
 * label for plain text, a span group where there are links (a tap finds its
 * span, with a finger's tolerance), an image for a picture that arrived.
 * Blocks are made a few milliseconds' worth at a time from the timer, so a
 * long page never holds a frame; the column scrolls vertically only, and
 * everything wraps, so nothing needs scrolling sideways.
 *
 * THE KEYBOARD (DS 17.3, as Wave): in portrait a tap on the address field
 * brings the touch keyboard up; going somewhere, tapping the page or a link
 * puts it away. In landscape - where the physical keyboard is - a tap only
 * focuses the field, and KEYS brings the touch keyboard up when there is
 * none. Enter, from any keyboard, goes.
 *
 * FULLSCREEN (DS 30.4): the app declares NONE; the shell's header carries
 * the back button and the hint (LOADING, SECURE, NOT SECURE, ERROR).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "browser_session.h"
#include "browser_view.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
/* Not in lvgl.h: dropping a picture's cache entry before its pixels go. The
 * device's LVGL has no image cache (LV_CACHE_DEF_SIZE 0), but a build that
 * has one must not show the last page's pixels under the same descriptor. */
#include "src/misc/cache/instance/lv_image_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BROWSER_POLL_MS 40
/* How long one tick may spend making page objects. */
#define RENDER_BUDGET_MS 8
#define SAVE_EVERY_MS 3000
#define BROWSER_DESTROY_GRACE_MS 300
#define BTN_H 64
#define ICON_W 76
#define GO_W 88
#define KEYS_W 104
#define GAP 8
#define BLOCK_GAP 14
#define INDENT_PX 22
#define PAGE_PAD 16
#define STRIP_MAX_H 240
/* Blocks per chunk: the page is a column of chunks, each a column of
 * blocks, so adding a block lays out its chunk and the chunks, never every
 * block made so far (which made an 800-block page take 0.9 s on a PC). */
#define CHUNK_BLOCKS 24
/* A tap this close to a link still opens it (a fingertip is not a pixel). */
#define LINK_SLOP 12

enum nav_button {
    NAV_BACK = 0,
    NAV_FORWARD,
    NAV_RELOAD,
    NAV_HOME,
    NAV_MARK,
    NAV_COUNT
};

struct browser_app {
    struct browser_view *v;
    struct browser_session *s;
    lv_timer_t *timer;
    struct pocketui_layout_guard guard;
    bool relayout;
    bool landscape;
    bool strip;
    bool editing;               /* the person is typing an address */
    bool setting_field;
    int64_t saved_ms;
    const char *hint_shown;
    lv_obj_t *screen;           /* where the theme event is heard */

    lv_obj_t *frame;
    lv_obj_t *bar;
    lv_obj_t *field;
    lv_obj_t *field_wrap;
    lv_obj_t *go_btn;
    lv_obj_t *keys_btn;
    lv_obj_t *status;
    lv_obj_t *viewport;
    lv_obj_t *navbar;
    lv_obj_t *nav[NAV_COUNT];
    lv_obj_t *nav_label[NAV_COUNT];

    lv_obj_t *start;
    lv_obj_t *st_bookmarks;
    lv_obj_t *st_recent;
    lv_obj_t *st_recent_cap;
    lv_obj_t *st_clear;

    lv_obj_t *error;
    lv_obj_t *er_title;
    lv_obj_t *er_text;
    lv_obj_t *er_detail;
    lv_obj_t *er_retry;
    lv_obj_t *er_insecure;

    lv_obj_t *page;
    lv_obj_t *chunk;            /* where the next block goes */
    size_t rendered;            /* blocks made so far */
    bool rendering;
    uint32_t render_started;    /* lv_tick when the page began to be made */
    unsigned render_ticks;      /* timer ticks it took */
    lv_obj_t *img_box[WEB_DOC_IMAGE_MAX];
    lv_obj_t *img_note[WEB_DOC_IMAGE_MAX];
    lv_obj_t *img_obj[WEB_DOC_IMAGE_MAX];
    lv_image_dsc_t img_dsc[WEB_DOC_IMAGE_MAX];
    lv_obj_t *cut_note;
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

static void set_enabled(lv_obj_t *obj, bool on)
{
    if (on) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
}

static void label_text(lv_obj_t *lb, const char *text)
{
    if (lb && strcmp(lv_label_get_text(lb), text) != 0) {
        lv_label_set_text(lb, text);
    }
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

static lv_obj_t *wrapping(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, text, role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_PCT(100));
    return lb;
}

static lv_obj_t *one_line(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, text, role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lb, LV_PCT(100));
    lv_obj_set_height(lb, lv_font_get_line_height(lv_obj_get_style_text_font(lb, LV_PART_MAIN)));
    return lb;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, bool primary, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = pocketui_button(parent, text, cb, user);

    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_height(b, BTN_H);
    pos_style_add(b, POS_STYLE_BUTTON_DISABLED, LV_STATE_DISABLED);
    if (!primary) {
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
        pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(b, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
    return b;
}

/* A square button with an LVGL symbol: the navigation row. */
static lv_obj_t *icon_button(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb, void *user, lv_obj_t **label)
{
    lv_obj_t *b = button(parent, "", false, cb, user);
    lv_obj_t *lb = lv_obj_get_child(b, 0);

    lv_obj_set_width(b, ICON_W);
    lv_obj_remove_style(lb, pos_style(POS_STYLE_BUTTON_LABEL), 0);
    pos_style_add(lb, POS_STYLE_SYMBOL, 0);
    lv_label_set_text(lb, symbol);
    lv_obj_center(lb);
    *label = lb;
    return b;
}

static void hide_keyboard_in_portrait(struct browser_app *a)
{
    if (!a->landscape) {
        pocketos_shell_keyboard_hide();
    }
}

/* ---- the helper --------------------------------------------------------------------- */

static unsigned start_helper(struct browser_app *a)
{
    struct browser_session_config cfg;
    const char *backend = getenv("POCKETOS_BROWSER_BACKEND");
    char err[160];

    memset(&cfg, 0, sizeof(cfg));
    cfg.fake = backend && strcmp(backend, "fake") == 0;
#ifdef POCKETOS_SHELL_TEST_HOOKS
    /* The simulator only: a CA file for a test server or a proxy. The
     * device always uses the system store. */
    cfg.ca_file = getenv("POCKETOS_BROWSER_CA_FILE");
#endif
    if (!browser_view_may_start_helper(a->v, now_ms())) {
        return browser_view_helper_stopped(a->v, BROWSER_EXIT_CRASHED, now_ms());
    }
    if (browser_session_start(a->s, &cfg, now_ms(), err, sizeof(err)) != 0) {
        LOG_WARN("browser: could not start the helper: %s", err);
        return browser_view_helper_stopped(a->v, BROWSER_EXIT_START, now_ms());
    }
    return 0;
}

static void repaint(struct browser_app *a, unsigned changed);

/* Carry out what the view asked for, then show what changed. */
static void act(struct browser_app *a, unsigned changed, const struct browser_cmd *cmd)
{
    if (cmd->kind == BROWSER_CMD_OPEN) {
        if (!browser_session_active(a->s)) {
            changed |= start_helper(a);
        }
        if (browser_session_active(a->s) && browser_session_send(a->s, cmd, now_ms()) != 0) {
            /* The helper went between two polls; the next poll reaps it and
             * the view shows why. */
            LOG_WARN("browser: the helper did not take a request");
        }
    } else if (cmd->kind == BROWSER_CMD_STOP && browser_session_active(a->s)) {
        browser_session_send(a->s, cmd, now_ms());
    }
    a->editing = false;
    repaint(a, changed);
}

/* ---- the page ------------------------------------------------------------------------ */

static void drop_image_objects(struct browser_app *a)
{
    int i;

    for (i = 0; i < WEB_DOC_IMAGE_MAX; i++) {
        if (a->img_obj[i]) {
            lv_image_cache_drop(&a->img_dsc[i]);
        }
        a->img_box[i] = NULL;
        a->img_note[i] = NULL;
        a->img_obj[i] = NULL;
    }
    memset(a->img_dsc, 0, sizeof(a->img_dsc));
}

static void clear_page(struct browser_app *a)
{
    drop_image_objects(a);
    lv_obj_clean(a->page);
    a->chunk = NULL;
    a->cut_note = NULL;
    a->rendered = 0;
    a->rendering = false;
}

static enum pos_style_role block_role(const struct web_block *b)
{
    switch (b->type) {
    case WEB_BLOCK_HEADING:
        return POS_STYLE_TITLE;
    case WEB_BLOCK_PRE:
        return POS_STYLE_VALUE;
    case WEB_BLOCK_QUOTE:
        return POS_STYLE_TEXT_SECONDARY;
    case WEB_BLOCK_NOTE:
        return POS_STYLE_TEXT_MUTED;
    default:
        return POS_STYLE_TEXT_PRIMARY;
    }
}

static void follow(struct browser_app *a, int link)
{
    struct browser_cmd cmd;
    unsigned changed;

    hide_keyboard_in_portrait(a);
    changed = browser_view_follow(a->v, link, now_ms(), &cmd);
    act(a, changed, &cmd);
}

/* The link under point in a span group, looking a finger's width around it. */
static int link_at(struct browser_app *a, lv_obj_t *sg, const struct web_block *b, const lv_point_t *pt)
{
    static const int8_t dx[] = { 0, 0, 0, -1, 1, -1, 1 };
    static const int8_t dy[] = { 0, -1, 1, 0, 0, -1, 1 };
    uint32_t count = lv_spangroup_get_span_count(sg);
    size_t k;

    for (k = 0; k < sizeof(dx); k++) {
        lv_point_t p = { pt->x + dx[k] * LINK_SLOP, pt->y + dy[k] * LINK_SLOP };
        lv_span_t *sp = lv_spangroup_get_span_by_point(sg, &p);
        uint32_t i;

        if (!sp) {
            continue;
        }
        for (i = 0; i < count && i < b->run_count; i++) {
            if (lv_spangroup_get_child(sg, (int32_t)i) == sp) {
                int link = a->v->doc.runs[b->run_first + i].link;

                if (link >= 0) {
                    return link;
                }
                break;
            }
        }
    }
    return -1;
}

static void on_text_click(lv_event_t *e)
{
    struct browser_app *a = lv_event_get_user_data(e);
    lv_obj_t *sg = lv_event_get_current_target(e);
    size_t bi = (size_t)(intptr_t)lv_obj_get_user_data(sg);
    lv_indev_t *in = lv_indev_active();
    lv_point_t pt;
    int link;

    if (!in || bi == 0 || bi > a->v->doc.nblocks) {
        return;
    }
    lv_indev_get_point(in, &pt);
    link = link_at(a, sg, &a->v->doc.blocks[bi - 1], &pt);
    if (link >= 0) {
        follow(a, link);
    } else {
        hide_keyboard_in_portrait(a);
    }
}

static void on_image_click(lv_event_t *e)
{
    struct browser_app *a = lv_event_get_user_data(e);
    int im = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e)) - 1;

    if (im >= 0 && (size_t)im < a->v->doc.nimages && a->v->doc.images[im].link >= 0) {
        follow(a, a->v->doc.images[im].link);
    }
}

static void image_note_text(struct browser_app *a, int i)
{
    const struct web_image *im = &a->v->doc.images[i];
    const struct browser_image *st = &a->v->img[i];
    char text[WEB_ALT_MAX + 96];

    switch (st->state) {
    case BROWSER_IMAGE_WAIT:
        snprintf(text, sizeof(text), "[picture: %s \xe2\x80\x94 loading]", im->alt[0] ? im->alt : "no description");
        break;
    case BROWSER_IMAGE_FAILED:
        snprintf(text, sizeof(text), "[picture: %s \xe2\x80\x94 %s]", im->alt[0] ? im->alt : "no description",
                 st->why[0] ? st->why : "not shown");
        break;
    default:
        snprintf(text, sizeof(text), "[picture: %s]", im->alt[0] ? im->alt : "no description");
        break;
    }
    label_text(a->img_note[i], text);
}

static void show_image(struct browser_app *a, int i)
{
    const struct browser_image *st = &a->v->img[i];
    lv_image_dsc_t *d = &a->img_dsc[i];

    if (!a->img_box[i]) {
        return; /* its block is not made yet; it is shown when it is */
    }
    if (st->state == BROWSER_IMAGE_READY && st->px && !a->img_obj[i]) {
        memset(d, 0, sizeof(*d));
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = (uint32_t)st->w;
        d->header.h = (uint32_t)st->h;
        d->header.stride = (uint32_t)st->w * 2;
        d->data = (const uint8_t *)st->px;
        d->data_size = (uint32_t)st->w * (uint32_t)st->h * 2;
        a->img_obj[i] = lv_image_create(a->img_box[i]);
        lv_image_set_src(a->img_obj[i], d);
        set_hidden(a->img_note[i], true);
    } else if (st->state != BROWSER_IMAGE_READY) {
        image_note_text(a, i);
    }
}

static lv_obj_t *make_block(struct browser_app *a, size_t bi)
{
    const struct web_doc *d = &a->v->doc;
    const struct web_block *b = &d->blocks[bi];
    enum pos_style_role role = block_role(b);
    bool spans = false;
    lv_obj_t *o;
    size_t r;
    int indent = b->type == WEB_BLOCK_HEADING ? 0 : b->level * INDENT_PX;
    lv_obj_t *parent;

    if (!a->chunk || bi % CHUNK_BLOCKS == 0) {
        a->chunk = column(a->page, BLOCK_GAP);
    }
    parent = a->chunk;
    if (b->type == WEB_BLOCK_RULE) {
        o = box(parent);
        lv_obj_set_size(o, LV_PCT(100), 1);
        pos_style_add(o, POS_STYLE_DIVIDER, 0);
        return o;
    }
    if (b->type == WEB_BLOCK_IMAGE) {
        int im = b->image;

        o = column(parent, 4);
        lv_obj_set_user_data(o, (void *)(intptr_t)(im + 1));
        if (d->images[im].link >= 0) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_add_event_cb(o, on_image_click, LV_EVENT_CLICKED, a);
        }
        a->img_box[im] = o;
        a->img_note[im] = wrapping(o, "", POS_STYLE_CAPTION);
        image_note_text(a, im);
        show_image(a, im);
        return o;
    }
    for (r = b->run_first; r < b->run_first + b->run_count; r++) {
        if (d->runs[r].link >= 0 || (d->runs[r].flags & WEB_RUN_CODE)) {
            spans = true;
            break;
        }
    }
    if (!spans) {
        size_t total = 0;
        char *text;

        for (r = b->run_first; r < b->run_first + b->run_count; r++) {
            total += d->runs[r].len;
        }
        text = malloc(total + 1);
        if (!text) {
            return NULL;
        }
        total = 0;
        for (r = b->run_first; r < b->run_first + b->run_count; r++) {
            memcpy(text + total, d->text + d->runs[r].off, d->runs[r].len);
            total += d->runs[r].len;
        }
        text[total] = '\0';
        o = pocketui_label(parent, "", role);
        lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(o, LV_PCT(100));
        lv_label_set_text(o, text);
        free(text);
    } else {
        o = lv_spangroup_create(parent);
        lv_obj_set_width(o, LV_PCT(100));
        lv_obj_set_height(o, LV_SIZE_CONTENT);
        pos_style_add(o, role, 0);
        for (r = b->run_first; r < b->run_first + b->run_count; r++) {
            const struct web_run *run = &d->runs[r];
            lv_span_t *sp = lv_spangroup_add_span(o);
            char *t = malloc(run->len + 1);

            if (!sp || !t) {
                free(t);
                break;
            }
            memcpy(t, d->text + run->off, run->len);
            t[run->len] = '\0';
            lv_span_set_text(sp, t);
            free(t);
            if (run->link >= 0) {
                lv_spangroup_set_span_style(o, sp, pos_style(d->links[run->link].supported ?
                                                             POS_STYLE_ACCENT_TEXT : POS_STYLE_TEXT_MUTED));
                lv_style_set_text_decor(lv_span_get_style(sp), LV_TEXT_DECOR_UNDERLINE);
            } else if (run->flags & WEB_RUN_CODE) {
                lv_spangroup_set_span_style(o, sp, pos_style(POS_STYLE_VALUE));
            }
        }
        lv_spangroup_refresh(o);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_event_cb(o, on_text_click, LV_EVENT_CLICKED, a);
        lv_obj_set_user_data(o, (void *)(intptr_t)(bi + 1));
    }
    if (indent) {
        lv_obj_set_style_pad_left(o, indent, 0);
    }
    if (b->type == WEB_BLOCK_HEADING && bi > 0) {
        lv_obj_set_style_margin_top(o, 10, 0);
    }
    if (b->type == WEB_BLOCK_NOTE) {
        pos_style_add(o, POS_STYLE_SLAB, 0);
        lv_obj_set_style_pad_all(o, 12, 0);
    }
    return o;
}

/* Make blocks for at most RENDER_BUDGET_MS; the rest on the next tick. */
static void render_some(struct browser_app *a)
{
    uint32_t start = lv_tick_get();
    const struct web_doc *d = &a->v->doc;

    a->render_ticks++;
    while (a->rendering && a->rendered < d->nblocks) {
        make_block(a, a->rendered++);
        if (lv_tick_elaps(start) >= RENDER_BUDGET_MS) {
            return;
        }
    }
    if (a->rendering && a->rendered >= d->nblocks) {
        a->rendering = false;
        /* For the bench and the hardware gate: how long a page takes to make. */
        LOG_INFO("browser: page made: %zu blocks, %zu links, %zu pictures, %zu KB text, in %u ms over %u tick(s)",
                 d->nblocks, d->nlinks, d->nimages, d->text_len / 1024, lv_tick_elaps(a->render_started),
                 a->render_ticks);
        if (d->truncated) {
            a->cut_note = wrapping(a->page, "This page is longer than Browser shows: it was cut here.",
                                   POS_STYLE_TEXT_MUTED);
            pos_style_add(a->cut_note, POS_STYLE_SLAB, 0);
            lv_obj_set_style_pad_all(a->cut_note, 12, 0);
        }
        if (d->nblocks == 0 && !d->truncated) {
            wrapping(a->page, "This page has no text Browser can show.", POS_STYLE_TEXT_MUTED);
        }
    }
}

/* ---- the start page -------------------------------------------------------------------- */

static void on_place(lv_event_t *e)
{
    struct browser_app *a = lv_event_get_user_data(e);
    intptr_t tag = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    const struct web_store *st = &a->v->store;
    const char *url = NULL;
    struct browser_cmd cmd;
    unsigned changed;

    /* 1..: bookmarks, -1..: recent pages */
    if (tag > 0 && tag <= st->nbookmark) {
        url = st->bookmark[tag - 1].url;
    } else if (tag < 0 && -tag <= st->nrecent) {
        url = st->recent[-tag - 1].url;
    }
    if (!url) {
        return;
    }
    hide_keyboard_in_portrait(a);
    changed = browser_view_open(a->v, url, now_ms(), &cmd);
    act(a, changed, &cmd);
}

static void place_row(struct browser_app *a, lv_obj_t *list, const struct web_place *p, intptr_t tag)
{
    lv_obj_t *r = column(list, 2);

    lv_obj_set_style_pad_all(r, 12, 0);
    lv_obj_set_style_min_height(r, POCKETUI_TOUCH_MIN, 0);
    pos_style_add(r, POS_STYLE_DIVIDER, 0);
    pos_style_add(r, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_user_data(r, (void *)tag);
    lv_obj_add_event_cb(r, on_place, LV_EVENT_CLICKED, a);
    one_line(r, p->title[0] ? p->title : p->url, POS_STYLE_TEXT_PRIMARY);
    one_line(r, p->url, POS_STYLE_CAPTION);
}

static void fill_start(struct browser_app *a)
{
    const struct web_store *st = &a->v->store;
    int i;

    lv_obj_clean(a->st_bookmarks);
    lv_obj_clean(a->st_recent);
    for (i = 0; i < st->nbookmark; i++) {
        place_row(a, a->st_bookmarks, &st->bookmark[i], i + 1);
    }
    if (st->nbookmark == 0) {
        wrapping(a->st_bookmarks, "No bookmarks yet. Open a page and tap + to keep it here.", POS_STYLE_TEXT_MUTED);
    }
    for (i = 0; i < st->nrecent; i++) {
        place_row(a, a->st_recent, &st->recent[i], -(i + 1));
    }
    set_hidden(a->st_recent_cap, st->nrecent == 0);
    set_hidden(a->st_clear, st->nrecent == 0);
}

static void on_clear_recent(lv_event_t *e)
{
    struct browser_app *a = lv_event_get_user_data(e);

    repaint(a, browser_view_clear_recent(a->v));
}

/* ---- the buttons ------------------------------------------------------------------------- */

static void on_nav(lv_event_t *e)
{
    struct browser_app *a = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_current_target(e);
    struct browser_cmd cmd;
    unsigned changed = 0;

    memset(&cmd, 0, sizeof(cmd));
    hide_keyboard_in_portrait(a);
    if (b == a->nav[NAV_BACK]) {
        changed = browser_view_back(a->v, now_ms(), &cmd);
    } else if (b == a->nav[NAV_FORWARD]) {
        changed = browser_view_forward(a->v, now_ms(), &cmd);
    } else if (b == a->nav[NAV_RELOAD]) {
        changed = a->v->loading || a->v->images_loading ? browser_view_stop(a->v, &cmd)
                                                        : browser_view_reload(a->v, now_ms(), &cmd);
    } else if (b == a->nav[NAV_HOME]) {
        changed = browser_view_home(a->v, now_ms(), &cmd);
    } else if (b == a->nav[NAV_MARK]) {
        changed = browser_view_toggle_bookmark(a->v);
    }
    act(a, changed, &cmd);
}

static void go(struct browser_app *a)
{
    struct browser_cmd cmd;
    unsigned changed = browser_view_go(a->v, lv_textarea_get_text(a->field), now_ms(), &cmd);

    pocketos_shell_keyboard_hide();
    act(a, changed | BROWSER_CHANGED_FIELD, &cmd);
}

static void on_go(lv_event_t *e)
{
    go(lv_event_get_user_data(e));
}

static void on_retry(lv_event_t *e)
{
    struct browser_app *a = lv_event_get_user_data(e);
    struct browser_cmd cmd;
    unsigned changed = browser_view_reload(a->v, now_ms(), &cmd);

    act(a, changed, &cmd);
}

static void on_insecure(lv_event_t *e)
{
    struct browser_app *a = lv_event_get_user_data(e);
    struct browser_cmd cmd;
    unsigned changed = browser_view_try_insecure(a->v, now_ms(), &cmd);

    act(a, changed, &cmd);
}

static void on_keys(lv_event_t *e)
{
    struct browser_app *a = lv_event_get_user_data(e);

    if (pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_hide();
    } else {
        pos_input_focus(a->field);
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, NULL);
    }
    a->relayout = true;
}

static void on_field_clicked(lv_event_t *e)
{
    struct browser_app *a = lv_event_get_user_data(e);
    lv_indev_t *src = lv_indev_active();

    /* A finger only: Enter reaches the focused field as a click too, after
     * go() put the keyboard away, and must not bring it back (Wave). */
    if (!src || lv_indev_get_type(src) != LV_INDEV_TYPE_POINTER) {
        return;
    }
    if (!a->landscape) {
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, NULL);
        a->relayout = true;
    }
}

static void on_field_changed(lv_event_t *e)
{
    struct browser_app *a = lv_event_get_user_data(e);

    if (!a->setting_field) {
        a->editing = true;
    }
}

static void on_field_ready(lv_event_t *e)
{
    go(lv_event_get_user_data(e));
}

static void on_viewport_pressed(lv_event_t *e)
{
    hide_keyboard_in_portrait(lv_event_get_user_data(e));
}

/* ---- painting ---------------------------------------------------------------------------- */

static void paint_buttons(struct browser_app *a)
{
    const struct browser_view *v = a->v;
    bool busy = v->loading || v->images_loading;

    set_enabled(a->nav[NAV_BACK], browser_view_can_back(v));
    set_enabled(a->nav[NAV_FORWARD], browser_view_can_forward(v));
    label_text(a->nav_label[NAV_RELOAD], busy ? LV_SYMBOL_CLOSE : LV_SYMBOL_REFRESH);
    set_enabled(a->nav[NAV_RELOAD], busy || v->state == BROWSER_PAGE ||
                                        (v->state == BROWSER_ERROR && v->err.fail != WEB_FAIL_URL));
    set_enabled(a->nav[NAV_MARK], v->state == BROWSER_PAGE);
    label_text(a->nav_label[NAV_MARK], browser_view_is_bookmark(v) ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
    label_text(lv_obj_get_child(a->keys_btn, 0), pocketos_shell_keyboard_visible() ? "HIDE" : "KEYS");
}

static void paint_error(struct browser_app *a)
{
    const struct browser_error *er = &a->v->err;

    label_text(a->er_title, er->title);
    label_text(a->er_text, er->text);
    label_text(a->er_detail, er->detail[0] ? er->detail : "");
    set_hidden(a->er_detail, !er->detail[0]);
    set_hidden(a->er_retry, er->fail == WEB_FAIL_URL);
    set_hidden(a->er_insecure, !er->alt_url[0]);
}

static void repaint(struct browser_app *a, unsigned changed)
{
    struct browser_view *v = a->v;

    if (changed & (BROWSER_CHANGED_STATE | BROWSER_CHANGED_DOC)) {
        set_hidden(a->start, v->state != BROWSER_START);
        set_hidden(a->page, v->state != BROWSER_PAGE);
        set_hidden(a->error, v->state != BROWSER_ERROR);
        if (changed & BROWSER_CHANGED_DOC) {
            clear_page(a);
            lv_obj_scroll_to_y(a->viewport, 0, LV_ANIM_OFF);
            a->rendering = v->state == BROWSER_PAGE;
            a->render_started = lv_tick_get();
            a->render_ticks = 0;
            render_some(a);
        }
        if (v->state == BROWSER_ERROR) {
            paint_error(a);
            lv_obj_scroll_to_y(a->viewport, 0, LV_ANIM_OFF);
        }
        if (v->state == BROWSER_START) {
            lv_obj_scroll_to_y(a->viewport, 0, LV_ANIM_OFF);
        }
    }
    if ((changed & BROWSER_CHANGED_IMAGES) && v->state == BROWSER_PAGE) {
        size_t i;

        for (i = 0; i < v->doc.nimages; i++) {
            show_image(a, (int)i);
        }
    }
    if (changed & BROWSER_CHANGED_START) {
        fill_start(a);
    }
    if ((changed & BROWSER_CHANGED_FIELD) && !(a->editing && pos_input_focused() == a->field)) {
        a->setting_field = true;
        lv_textarea_set_text(a->field, v->field);
        a->setting_field = false;
        a->editing = false;
    }
    label_text(a->status, v->status);
    if (v->hint != a->hint_shown) {
        pocketos_shell_set_status_hint(v->hint ? v->hint : "");
        a->hint_shown = v->hint;
    }
    paint_buttons(a);
}

/* ---- layout ------------------------------------------------------------------------------ */

static void layout(struct browser_app *a)
{
    struct pos_insets in;
    int32_t w;
    int32_t h;
    bool kb = pocketos_shell_keyboard_visible() != 0;
    int i;

    lv_obj_update_layout(a->frame);
    w = lv_obj_get_content_width(a->frame);
    h = lv_obj_get_content_height(a->frame);
    if (!pocketui_layout_begin(&a->guard, a->frame, &in) && !a->relayout) {
        return;
    }
    a->relayout = false;
    a->landscape = w > h || (a->landscape && kb && h < STRIP_MAX_H);
    a->strip = a->landscape && h < STRIP_MAX_H;
    /* The navigation buttons: in the foot bar in portrait, before the
     * address field in landscape, gone while typing in the strip. */
    for (i = 0; i < NAV_COUNT; i++) {
        lv_obj_t *want = a->landscape ? a->bar : a->navbar;

        if (lv_obj_get_parent(a->nav[i]) != want) {
            lv_obj_set_parent(a->nav[i], want);
        }
        if (a->landscape) {
            lv_obj_move_to_index(a->nav[i], i);
        }
        lv_obj_set_width(a->nav[i], a->landscape ? ICON_W : 1);
        lv_obj_set_flex_grow(a->nav[i], a->landscape ? 0 : 1);
        set_hidden(a->nav[i], a->strip);
    }
    set_hidden(a->navbar, a->landscape);
    set_hidden(a->keys_btn, !a->landscape);
    set_hidden(a->status, a->strip);
    set_hidden(a->viewport, a->strip);
    lv_obj_update_layout(a->frame);
    a->v->max_width = lv_obj_get_content_width(a->viewport);
    if (a->v->max_width < 120 || a->v->max_width > WEB_IMAGE_SIDE_MAX) {
        a->v->max_width = a->landscape ? 800 : 528;
    }
    paint_buttons(a);
}

static void on_frame_size(lv_event_t *e)
{
    struct browser_app *a = lv_event_get_user_data(e);

    a->relayout = true; /* laid out from the timer, never inside an event */
}

/* A theme or mode change: the span groups hold copies of the role styles,
 * so the page is made again. */
static void on_theme(lv_event_t *e)
{
    struct browser_app *a = lv_event_get_user_data(e);

    if (a->v->state == BROWSER_PAGE) {
        clear_page(a);
        a->rendering = true;
        a->render_started = lv_tick_get();
        a->render_ticks = 0;
    }
}

/* ---- the timer -------------------------------------------------------------------------- */

static void on_poll(lv_timer_t *t)
{
    struct browser_app *a = lv_timer_get_user_data(t);
    unsigned changed = 0;

    if (browser_session_active(a->s)) {
        changed = browser_session_poll(a->s, a->v, now_ms());
        if (changed & BROWSER_CHANGED_RESTART) {
            struct browser_cmd cmd;

            browser_view_resend(a->v, &cmd);
            changed &= ~(unsigned)BROWSER_CHANGED_RESTART;
            act(a, changed, &cmd);
            changed = 0;
        }
    }
    if (changed) {
        repaint(a, changed);
    }
    if (a->rendering) {
        render_some(a);
    }
    if (a->relayout) {
        layout(a);
    }
    if (a->v->store_dirty && now_ms() - a->saved_ms >= SAVE_EVERY_MS) {
        a->saved_ms = now_ms();
        if (browser_view_save(a->v) != 0) {
            LOG_WARN("browser: could not save its state");
        }
    }
}

/* ---- building ------------------------------------------------------------------------------ */

static void build_start(struct browser_app *a)
{
    lv_obj_t *s = column(a->viewport, 10);
    lv_obj_t *about;

    a->start = s;
    pocketui_label(s, "Browser", POS_STYLE_TITLE);
    wrapping(s, "Type an address above - example.com becomes https://example.com. There is no built-in search.",
             POS_STYLE_TEXT_SECONDARY);
    pocketui_label(s, "BOOKMARKS", POS_STYLE_CAPTION);
    a->st_bookmarks = column(s, 0);
    a->st_recent_cap = pocketui_label(s, "RECENT", POS_STYLE_CAPTION);
    a->st_recent = column(s, 0);
    a->st_clear = button(s, "CLEAR RECENT", false, on_clear_recent, a);
    lv_obj_set_width(a->st_clear, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(a->st_clear, 20, 0);
    about = wrapping(s, "A text-first reader: pages, links and JPEG/PNG pictures. No JavaScript, no page "
                        "styles, no forms. Secure certificates are always checked.",
                     POS_STYLE_TEXT_MUTED);
    lv_obj_set_style_margin_top(about, 12, 0);
}

static void build_error(struct browser_app *a)
{
    lv_obj_t *e = column(a->viewport, 12);
    lv_obj_t *row;

    a->error = e;
    a->er_title = wrapping(e, "", POS_STYLE_TITLE);
    a->er_text = wrapping(e, "", POS_STYLE_TEXT_PRIMARY);
    a->er_detail = wrapping(e, "", POS_STYLE_CAPTION);
    row = column(e, GAP);
    lv_obj_set_style_margin_top(row, 8, 0);
    a->er_retry = button(row, "TRY AGAIN", true, on_retry, a);
    a->er_insecure = button(row, "OPEN WITH HTTP:// (NOT SECURE)", false, on_insecure, a);
    set_hidden(e, true);
}

static void build(struct browser_app *a, lv_obj_t *root)
{
    static const char *const symbols[NAV_COUNT] = { LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_REFRESH,
                                                    LV_SYMBOL_HOME, LV_SYMBOL_PLUS };
    int i;

    a->frame = box(root);
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(a->frame, GAP, 0);

    a->bar = box(a->frame);
    lv_obj_set_size(a->bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(a->bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(a->bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(a->bar, GAP, 0);
    a->field = pocketui_text_field(a->bar, "Address, e.g. example.com", true);
    a->field_wrap = lv_obj_get_parent(a->field);
    lv_obj_set_width(a->field_wrap, 1);
    lv_obj_set_flex_grow(a->field_wrap, 1);
    lv_textarea_set_max_length(a->field, WEB_URL_MAX - 1);
    lv_obj_add_event_cb(a->field, on_field_clicked, LV_EVENT_CLICKED, a);
    lv_obj_add_event_cb(a->field, on_field_changed, LV_EVENT_VALUE_CHANGED, a);
    lv_obj_add_event_cb(a->field, on_field_ready, LV_EVENT_READY, a);
    a->go_btn = button(a->bar, "GO", true, on_go, a);
    lv_obj_set_width(a->go_btn, GO_W);
    a->keys_btn = button(a->bar, "KEYS", false, on_keys, a);
    lv_obj_set_width(a->keys_btn, KEYS_W);

    a->status = one_line(a->frame, "", POS_STYLE_CAPTION);

    a->viewport = box(a->frame);
    lv_obj_set_width(a->viewport, LV_PCT(100));
    lv_obj_set_height(a->viewport, 1);
    lv_obj_set_flex_grow(a->viewport, 1);
    lv_obj_add_flag(a->viewport, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(a->viewport, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_scroll_dir(a->viewport, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(a->viewport, LV_SCROLLBAR_MODE_ACTIVE);
    pos_style_add(a->viewport, POS_STYLE_PANEL, 0);
    lv_obj_set_style_pad_all(a->viewport, PAGE_PAD, 0);
    lv_obj_add_event_cb(a->viewport, on_viewport_pressed, LV_EVENT_PRESSED, a);
    build_start(a);
    a->page = column(a->viewport, BLOCK_GAP);
    set_hidden(a->page, true);
    build_error(a);

    a->navbar = box(a->frame);
    lv_obj_set_size(a->navbar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(a->navbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(a->navbar, GAP, 0);
    for (i = 0; i < NAV_COUNT; i++) {
        a->nav[i] = icon_button(a->navbar, symbols[i], on_nav, a, &a->nav_label[i]);
    }
}

/* ---- the app ------------------------------------------------------------------------------ */

static void *browser_create(lv_obj_t *root)
{
    struct browser_app *a = calloc(1, sizeof(*a));
    uint32_t t0 = lv_tick_get();
    char why[128];

    if (!a) {
        return NULL;
    }
    a->v = malloc(sizeof(*a->v));
    a->s = malloc(sizeof(*a->s));
    if (!a->v || !a->s) {
        free(a->v);
        free(a->s);
        free(a);
        return NULL;
    }
    browser_view_init(a->v, NULL, why, sizeof(why));
    if (why[0]) {
        LOG_WARN("browser: remembered state %s (%s)",
                 a->v->store_unreadable ? "cannot be read: nothing is saved over it this time"
                 : a->v->store_aside    ? "not used: kept as state.bad at the next save"
                                        : "partly used: bad lines skipped",
                 why);
    }
    browser_session_init(a->s);
    build(a, root);
    a->hint_shown = NULL;
    a->saved_ms = now_ms();
    a->timer = lv_timer_create(on_poll, BROWSER_POLL_MS, a);
    /* Only now: building lays objects out as it goes. */
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    a->screen = lv_screen_active();
    lv_obj_add_event_cb(a->screen, on_theme, (lv_event_code_t)pos_event_theme_changed(), a);
    a->relayout = true;
    layout(a);
    /* No helper yet: it starts with the first page, so the start page costs
     * nothing but itself. */
    repaint(a, BROWSER_CHANGED_STATE | BROWSER_CHANGED_START | BROWSER_CHANGED_FIELD | BROWSER_CHANGED_STATUS);
    LOG_INFO("browser: start page in %u ms, %d bookmark(s), %d recent", lv_tick_elaps(t0), a->v->store.nbookmark,
             a->v->store.nrecent);
#ifdef POCKETOS_SHELL_TEST_HOOKS
    {
        /* The simulator only (tests/browser_shell_test.sh): open a page at
         * once, as if typed, so a headless run can show one. */
        const char *first = getenv("POCKETOS_BROWSER_OPEN");

        if (first && *first) {
            struct browser_cmd cmd;
            unsigned changed = browser_view_go(a->v, first, now_ms(), &cmd);

            act(a, changed, &cmd);
        }
    }
#endif
    return a;
}

static void browser_destroy(void *priv)
{
    struct browser_app *a = priv;

    if (!a) {
        return;
    }
    /* The frame outlives this by a moment, until the shell deletes the app's
     * objects; nothing may call back into a freed app in between. */
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    lv_obj_remove_event_cb_with_user_data(a->screen, on_theme, a);
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    /* The page's pictures point into the view's memory: their objects go
     * first. */
    clear_page(a);
    /* Leaving the app ends every connection and the helper with it. */
    browser_session_abandon(a->s, BROWSER_DESTROY_GRACE_MS);
    if (browser_view_save(a->v) != 0) {
        LOG_WARN("browser: could not save its state");
    }
    browser_view_free(a->v);
    pocketos_shell_keyboard_hide();
    free(a->s);
    free(a->v);
    free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_browser);

const struct pocketos_app app_browser = {
    .id = "browser",
    .name = "Browser",
    /* The launcher draws the Doors icon (DS 20); the glyph is the text icon. */
    .icon = LV_SYMBOL_DIRECTORY,
    /* A first-party icon in the extension's line language: a globe
     * (docs/design/doors-app-icons/README.md). */
    .icon_mask = &pos_app_icon_browser,
    .create = browser_create,
    .tick = NULL,
    .destroy = browser_destroy,
    /* Fullscreen (DS 30.4): the page takes the height. The hint shows in
     * the header. */
    .chrome = POCKETOS_CHROME_NONE,
};
