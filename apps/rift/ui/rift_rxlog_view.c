/*
 * RX LOG. See rift_rxlog_view.h; the ring and its words are rift_rxlog.[ch].
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_rxlog_view_int.h"

#include "rift_emoji.h"
#include "rift_emoji_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAP 6
/* Live traffic repaints the list at most this often; a reader's own action
 * repaints at once, and the app's once-a-second repaint catches the rest. */
#define LIVE_REPAINT_MS 250

struct rift_rxlog_view *rxv_of(const struct rift_app *a)
{
    return a ? a->rxlog_view : NULL;
}

/* ---- the type ----------------------------------------------------------- *
 *
 * Small and fixed: the log is read for its fields, and at Large the first
 * line would not fit a portrait row (DS §54). The fixed-size Small font of
 * the caption role is pos_styles.c's (DS §46.4); the message text gets the
 * same face with the colour emoji behind it, as a thread's does. */
static lv_style_t *fixed_font(void)
{
    return pos_style_fixed_size(POS_STYLE_CAPTION);
}

static void fill_text_font(struct rift_rxlog_view *v)
{
    lv_style_value_t base;

    lv_style_init(&v->text_font);
    if (lv_style_get_prop(fixed_font(), LV_STYLE_TEXT_FONT, &base) == LV_STYLE_RES_FOUND && base.ptr) {
        lv_style_set_text_font(&v->text_font, rift_emoji_font(base.ptr));
    }
}

static void field_init(struct field *f, lv_obj_t *parent, int32_t chars, lv_style_t *font)
{
    f->label = rift_cell(parent, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_add_style(f->label, font, 0);
    /* No tracking: the caption role's extra pixel a character is what made
     * a portrait first line one field too wide. The face is monospaced, so
     * the columns stay even without it. */
    lv_obj_set_style_text_letter_space(f->label, 0, 0);
    if (chars > 0) {
        /* A column of a width every row shares, in the face's own advance,
         * so the fields line up from row to row. */
        char sample[24];

        memset(sample, '0', (size_t)chars);
        sample[chars] = '\0';
        lv_obj_set_width(f->label, rift_cell_text_width(f->label, sample));
    }
    f->tone = NULL;
}

/* Put want on the field instead of whatever colour it had. A role style
 * brings its own font with its colour, so the fixed one goes back on top. */
static void tone(struct field *f, lv_style_t *want, lv_style_t *font)
{
    if (f->tone == want) {
        return;
    }
    if (f->tone) {
        lv_obj_remove_style(f->label, f->tone, 0);
    }
    if (want) {
        lv_obj_add_style(f->label, want, 0);
        lv_obj_remove_style(f->label, font, 0);
        lv_obj_add_style(f->label, font, 0);
    }
    f->tone = want;
}

static lv_style_t *hue(unsigned index)
{
    return pos_style_identity(index);
}

static lv_style_t *grade_tone(int grade)
{
    switch (grade) {
    case 2:
        return pos_style(POS_STYLE_STATUS_OK_TEXT);
    case 1:
        return pos_style(POS_STYLE_STATUS_WARN_TEXT);
    case 0:
        return pos_style(POS_STYLE_STATUS_ERROR_TEXT);
    default:
        return pos_style(POS_STYLE_TEXT_MUTED);
    }
}

static lv_style_t *state_tone(const struct rift_rx_entry *e)
{
    if (e->kind == RIFT_RX_MARK) {
        return pos_style(POS_STYLE_STATUS_WARN_TEXT);
    }
    switch (e->state) {
    case RIFT_RXS_DUP:
        return hue(HUE_VIOLET);
    case RIFT_RXS_ECHO:
        return hue(HUE_PINK);
    case RIFT_RXS_REJECTED:
    case RIFT_RXS_UNRESOLVED:
        return pos_style(POS_STYLE_STATUS_ERROR_TEXT);
    case RIFT_RXS_RX:
    default:
        return hue(HUE_TEAL);
    }
}

static lv_style_t *type_tone(const struct rift_rx_entry *e)
{
    int t = (e->flags & RIFT_RXF_HAVE_HEADER) ? e->type_code : -1;

    if (t == 2 || t == 5 || t == 6) {
        return pos_style(POS_STYLE_TEXT_PRIMARY);
    }
    if (t == 4) {
        return hue(HUE_GOLD);
    }
    if (t < 0 || t >= 12) {
        return pos_style(POS_STYLE_TEXT_MUTED);
    }
    return hue(HUE_SKY);
}

/* The route's colour, on the bar and the path line alike: no hop between
 * green, relayed or routed through hops orange, a TRACE's SNRs blue, a
 * rejected frame coral. */
static unsigned route_hue(const struct rift_rx_entry *e, int *none)
{
    *none = 0;
    if (e->kind == RIFT_RX_MARK) {
        *none = 1;
        return 0;
    }
    if (e->state == RIFT_RXS_REJECTED || !(e->flags & RIFT_RXF_PARSED)) {
        return HUE_CORAL;
    }
    if (e->flags & RIFT_RXF_PATH_SNR) {
        return HUE_SKY;
    }
    return e->path_hops == 0 ? HUE_GREEN : HUE_ORANGE;
}

/* ---- building ----------------------------------------------------------- */

static lv_obj_t *flex(lv_obj_t *parent, lv_flex_flow_t flow, int32_t gap)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(o, gap, 0);
    lv_obj_set_style_pad_row(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void build_row(struct rift_rxlog_view *v, struct row *r)
{
    lv_style_t *font = fixed_font();
    int32_t indent;

    r->row = flex(v->list, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_ver(r->row, 3, 0);
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    pos_style_add(r->row, POS_STYLE_DIVIDER, 0);

    /* Line one wraps rather than clips: at a narrow width the signal goes
     * to a line of its own instead of off the edge. */
    r->line1 = flex(r->row, LV_FLEX_FLOW_ROW_WRAP, GAP);
    r->bar = rift_vrule(r->line1, RIFT_IDENT_W);
    lv_obj_set_height(r->bar, rift_caption_h());
    field_init(&r->time, r->line1, 12, font);
    field_init(&r->state, r->line1, 7, font);
    field_init(&r->type, r->line1, 5, font);
    field_init(&r->ch, r->line1, 5, font);
    field_init(&r->hash, r->line1, 6, font);
    field_init(&r->size, r->line1, 4, font);
    field_init(&r->rssi, r->line1, 9, font);
    field_init(&r->snr, r->line1, 9, font);

    /* Lines two and three start under the time, past the bar. */
    indent = RIFT_IDENT_W + GAP;
    field_init(&r->path, r->row, 0, font);
    lv_obj_set_width(r->path.label, LV_PCT(100));
    lv_obj_set_style_pad_left(r->path.label, indent, 0);
    lv_label_set_long_mode(r->path.label, LV_LABEL_LONG_WRAP);

    r->line3 = flex(r->row, LV_FLEX_FLOW_ROW, GAP);
    lv_obj_set_style_pad_left(r->line3, indent, 0);
    field_init(&r->who, r->line3, 0, font);
    field_init(&r->what, r->line3, 0, &v->text_font);
    lv_obj_set_flex_grow(r->what.label, 1);
    lv_obj_set_width(r->what.label, 1);
    lv_label_set_long_mode(r->what.label, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    r->uid = 0;
}


static void build_detail(struct rift_rxlog_view *v)
{
    lv_obj_t *box;
    lv_obj_t *actions;

    /* Over the list, opaque: PANEL first and SCREEN after it, because
     * PANEL's own background is transparent (rift_emoji_picker.c). */
    v->detail = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->detail);
    pos_style_add(v->detail, POS_STYLE_PANEL, 0);
    pos_style_add(v->detail, POS_STYLE_SCREEN, 0);
    lv_obj_add_flag(v->detail, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(v->detail, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(v->detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(v->detail, RIFT_PANE_PAD, 0);
    lv_obj_set_style_pad_row(v->detail, 12, 0);
    lv_obj_remove_flag(v->detail, LV_OBJ_FLAG_SCROLLABLE);
    rift_group_label(v->detail, "PACKET");

    box = lv_obj_create(v->detail);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    v->detail_text = lv_label_create(box);
    lv_obj_remove_style_all(v->detail_text);
    pos_style_add(v->detail_text, POS_STYLE_TEXT_PRIMARY, 0);
    lv_obj_add_style(v->detail_text, &v->text_font, 0);
    lv_obj_set_width(v->detail_text, LV_PCT(100));
    lv_label_set_long_mode(v->detail_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(v->detail_text, "");

    actions = flex(v->detail, LV_FLEX_FLOW_ROW, 12);
    rift_action(actions, "CLOSE", 1, 1, rxv_on_detail_close, v->app);
}

lv_obj_t *rift_rxlog_view_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_rxlog_view *v = calloc(1, sizeof(*v));
    lv_obj_t *controls;
    int i;

    if (!v) {
        return NULL;
    }
    app->rxlog_view = v;
    v->app = app;
    fill_text_font(v);

    v->root = lv_obj_create(parent);
    lv_obj_remove_style_all(v->root);
    lv_obj_set_size(v->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(v->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(v->root, 12, 0);
    lv_obj_set_style_pad_ver(v->root, 8, 0);
    lv_obj_set_style_pad_row(v->root, 6, 0);
    lv_obj_remove_flag(v->root, LV_OBJ_FLAG_SCROLLABLE);

    controls = flex(v->root, LV_FLEX_FLOW_ROW, 12);
    v->pause = rift_action(controls, "PAUSE", 0, 1, rxv_on_pause, app);
    v->clear = rift_action(controls, "CLEAR", 0, 1, rxv_on_clear, app);
    v->filter = rift_action(controls, "FILTER ALL", 0, 1, rxv_on_filter, app);

    v->caption = lv_label_create(v->root);
    lv_obj_remove_style_all(v->caption);
    pos_style_add(v->caption, POS_STYLE_CAPTION, 0);
    lv_obj_add_style(v->caption, fixed_font(), 0);
    lv_obj_set_width(v->caption, LV_PCT(100));
    lv_label_set_long_mode(v->caption, LV_LABEL_LONG_WRAP);
    lv_label_set_text(v->caption, "");

    /* The list is not scrollable: the rows are a window onto the ring, and
     * a drag moves the window (rxv_on_list), not the rows. */
    v->list = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->list);
    lv_obj_set_width(v->list, LV_PCT(100));
    lv_obj_set_flex_grow(v->list, 1);
    lv_obj_set_flex_flow(v->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(v->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(v->list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v->list, rxv_on_list, LV_EVENT_PRESSED, app);
    lv_obj_add_event_cb(v->list, rxv_on_list, LV_EVENT_PRESSING, app);
    lv_obj_add_event_cb(v->list, rxv_on_list, LV_EVENT_CLICKED, app);
    v->note = lv_label_create(v->list);
    lv_obj_remove_style_all(v->note);
    pos_style_add(v->note, POS_STYLE_TEXT_MUTED, 0);
    lv_obj_set_width(v->note, LV_PCT(100));
    lv_label_set_long_mode(v->note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(v->note, "");
    for (i = 0; i < RXV_POOL; i++) {
        build_row(v, &v->rows[i]);
    }
    build_detail(v);
    return v->root;
}

void rift_rxlog_view_destroy(struct rift_app *app)
{
    struct rift_rxlog_view *v = rxv_of(app);

    if (!v) {
        return;
    }
    /* The objects go with the shell's body; the style is this block's, and
     * nothing that uses it outlives it. */
    lv_style_reset(&v->text_font);
    free(v);
    app->rxlog_view = NULL;
}

void rift_rxlog_view_shape(struct rift_app *app)
{
    struct rift_rxlog_view *v = rxv_of(app);

    if (v) {
        /* The same rows either way; only the width differs, and the lines
         * wrap to it. A new shape is a new window, so draw it again. */
        v->drawn_once = 0;
    }
}

/* ---- filling a row ------------------------------------------------------- */

static void fill_row(struct rift_rxlog_view *v, struct row *r, const struct rift_rx_entry *e)
{
    lv_style_t *font = fixed_font();
    char text[RIFT_RXLOG_LINE_MAX];
    int none = 0;
    unsigned h;

    r->uid = e->uid;
    lv_obj_remove_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    if (e->kind == RIFT_RX_MARK) {
        rift_rxlog_fmt_time(e, text, sizeof(text));
        rift_label_set(r->time.label, text);
        rift_label_set(r->state.label, "--");
        tone(&r->state, state_tone(e), font);
        rift_label_set(r->type.label, "");
        rift_label_set(r->ch.label, "");
        rift_label_set(r->hash.label, "");
        rift_label_set(r->size.label, "");
        rift_label_set(r->rssi.label, "");
        rift_label_set(r->snr.label, "");
        rift_vrule_set(r->bar, RIFT_TONE_NONE);
        lv_obj_add_flag(r->path.label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(r->line3, LV_OBJ_FLAG_HIDDEN);
        rift_label_set(r->who.label, "");
        rift_label_set(r->what.label, e->text);
        tone(&r->what, pos_style(POS_STYLE_STATUS_WARN_TEXT), &v->text_font);
        return;
    }
    rift_rxlog_fmt_time(e, text, sizeof(text));
    rift_label_set(r->time.label, text);
    rift_rxlog_state_word(e, text, sizeof(text));
    rift_label_set(r->state.label, text);
    tone(&r->state, state_tone(e), font);
    rift_label_set(r->type.label, rift_rxlog_type_word(e));
    tone(&r->type, type_tone(e), font);
    rift_rxlog_fmt_channel(e, text, sizeof(text));
    rift_label_set(r->ch.label, text);
    rift_rxlog_fmt_hash(e, text, sizeof(text));
    rift_label_set(r->hash.label, text);
    rift_rxlog_fmt_size(e, text, sizeof(text));
    rift_label_set(r->size.label, text);
    rift_rxlog_fmt_rssi(e, text, sizeof(text));
    rift_label_set(r->rssi.label, text);
    tone(&r->rssi, grade_tone(rift_rxlog_rssi_grade(e)), font);
    rift_rxlog_fmt_snr(e, text, sizeof(text));
    rift_label_set(r->snr.label, text);
    tone(&r->snr, grade_tone(rift_rxlog_snr_grade(e)), font);

    h = route_hue(e, &none);
    if (none) {
        rift_vrule_set(r->bar, RIFT_TONE_NONE);
    } else {
        rift_vrule_set_identity(r->bar, h);
    }
    /* The path whole, wrapping as far as it has to (rift_rxlog_fmt_path). */
    rift_rxlog_fmt_path(e, text, sizeof(text));
    lv_obj_remove_flag(r->path.label, LV_OBJ_FLAG_HIDDEN);
    rift_label_set(r->path.label, text);
    tone(&r->path, (e->flags & RIFT_RXF_PARSED) ? hue(h) : pos_style(POS_STYLE_TEXT_MUTED), font);

    {
        char who[RIFT_RXLOG_NAME_MAX * 2 + 16];
        char what[RIFT_RXLOG_TEXT_MAX + 64];
        char shown[RIFT_RXLOG_TEXT_MAX + 64];
        lv_style_t *who_tone = NULL;

        rift_rxlog_fmt_who(e, who, sizeof(who));
        rift_rxlog_fmt_what(e, what, sizeof(what));
        if (!who[0] && !what[0]) {
            /* Nothing to read, and no third line for it. */
            lv_obj_add_flag(r->line3, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_remove_flag(r->line3, LV_OBJ_FLAG_HIDDEN);
        /* The sender in the accent a thread gives them (DS §37.3): a channel
         * sender by the name it claims, a contact by its key. */
        if (e->decode == RIFT_RXD_CHANNEL && e->sender[0]) {
            who_tone = hue(rift_ident_hash(e->sender));
        } else if (e->decode == RIFT_RXD_DIRECT && (e->flags & RIFT_RXF_HAVE_KEY)) {
            who_tone = hue(e->sender_ident);
        } else if (e->decode == RIFT_RXD_ADVERT) {
            who_tone = hue(HUE_GOLD);
        }
        rift_label_set(r->who.label, who);
        if (who[0]) {
            lv_obj_remove_flag(r->who.label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(r->who.label, LV_OBJ_FLAG_HIDDEN);
        }
        tone(&r->who, who_tone, font);
        /* Colour emoji as the thread draws them: the stored text untouched,
         * the shown one folded for the emoji font. */
        rift_emoji_fold(what, shown, sizeof(shown));
        rift_label_set(r->what.label, shown);
        tone(&r->what,
             (e->decode == RIFT_RXD_DIRECT || e->decode == RIFT_RXD_CHANNEL)
                 ? pos_style(POS_STYLE_TEXT_PRIMARY)
             : e->state == RIFT_RXS_REJECTED || e->state == RIFT_RXS_UNRESOLVED
                 ? pos_style(POS_STYLE_STATUS_ERROR_TEXT)
                 : pos_style(POS_STYLE_TEXT_MUTED),
             &v->text_font);
    }
}

static void select_mark(struct row *r, int on)
{
    if (r->selected == on) {
        return;
    }
    if (on) {
        pos_style_add(r->row, POS_STYLE_SELECTED, 0);
    } else {
        lv_obj_remove_style(r->row, pos_style(POS_STYLE_SELECTED), 0);
    }
    r->selected = on;
}

/* ---- where the window is ------------------------------------------------- */

/* The position of the window's first row among the filter's entries, and
 * the hold put right where it no longer can be. */
int rxv_window_top(struct rift_rxlog *log)
{
    int pos = 0;

    if (log->top_uid) {
        pos = rift_rxlog_position(log, log->filter, log->top_uid);
        if (pos < 0) {
            /* Gone from the ring, or not shown by this filter. */
            log->top_uid = 0;
            pos = 0;
        }
    }
    if (log->paused && !log->top_uid) {
        const struct rift_rx_entry *first = NULL;

        if (rift_rxlog_window(log, log->filter, 0, &first, 1) == 1) {
            log->top_uid = first->uid;
        }
    }
    return pos;
}

uint32_t rxv_uid_at(const struct rift_rxlog *log, int pos)
{
    const struct rift_rx_entry *e = NULL;

    if (pos < 0 || rift_rxlog_window(log, log->filter, pos, &e, 1) != 1) {
        return 0;
    }
    return e->uid;
}

/* Put the window's top at pos: following the newest again when that is the
 * top and nothing is paused. */
void rxv_set_top(struct rift_rxlog *log, int pos)
{
    int count = rift_rxlog_count(log, log->filter);

    if (pos > count - 1) {
        pos = count - 1;
    }
    if (pos <= 0 && !log->paused) {
        log->top_uid = 0;
        return;
    }
    log->top_uid = rxv_uid_at(log, pos < 0 ? 0 : pos);
}

static void caption(struct rift_rxlog_view *v, int pos)
{
    const struct rift_rxlog *log = &v->app->rxlog;
    const struct rift_rx_entry *newest = rift_rxlog_at(log, 0);
    char text[256];
    size_t at = 0;
    int count = rift_rxlog_count(log, log->filter);

    at += (size_t)snprintf(text, sizeof(text), "%d SHOWN" RIFT_SEP "%d OF %d HELD" RIFT_SEP "%s", count,
                           log->count, RIFT_RXLOG_MAX,
                           log->paused ? "PAUSED" : pos > 0 ? "HELD" : "LIVE");
    if (pos > 0 && at < sizeof(text)) {
        at += (size_t)snprintf(text + at, sizeof(text) - at, RIFT_SEP "%d NEWER ABOVE", pos);
    }
    if (newest && !(newest->flags & RIFT_RXF_HAVE_WALL) && at < sizeof(text)) {
        at += (size_t)snprintf(text + at, sizeof(text) - at, RIFT_SEP "TIMES SINCE BOOT");
    }
    if (log->malformed && at < sizeof(text)) {
        snprintf(text + at, sizeof(text) - at, RIFT_SEP "%u REFUSED", log->malformed);
    }
    rift_label_set(v->caption, text);
}

static void note(struct rift_rxlog_view *v, int count)
{
    const struct rift_rxlog *log = &v->app->rxlog;
    const char *say = NULL;

    if (log->supported == 0) {
        say = "This meshcored keeps no receive log. RX LOG needs a meshcored that reports "
              "receptions (docs/api/mesh.md).";
    } else if (count == 0 && !rift_ipc_connected(&v->app->ipc)) {
        say = "Waiting for meshcored.";
    } else if (count == 0 && log->count > 0) {
        say = "Nothing heard matches this filter.";
    } else if (count == 0) {
        say = "Nothing received yet. Every frame the radio takes off the air is listed "
              "here, repeats included.";
    }
    if (say) {
        rift_label_set(v->note, say);
        lv_obj_remove_flag(v->note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v->note, LV_OBJ_FLAG_HIDDEN);
    }
}

void rxv_paint_detail(struct rift_rxlog_view *v)
{
    const struct rift_rx_entry *e = rift_rxlog_find(&v->app->rxlog, v->app->rxlog.selected_uid);
    char text[1600];
    char shown[1600];

    if (lv_obj_has_flag(v->detail, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (!e) {
        /* Its entry left the ring, or CLEAR took it. */
        lv_obj_add_flag(v->detail, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    rift_rxlog_fmt_detail(e, text, sizeof(text));
    rift_emoji_fold(text, shown, sizeof(shown));
    rift_label_set(v->detail_text, shown);
}

static void draw(struct rift_rxlog_view *v)
{
    struct rift_rxlog *log = &v->app->rxlog;
    const struct rift_rx_entry *win[RXV_POOL];
    int pos = rxv_window_top(log);
    int n = rift_rxlog_window(log, log->filter, pos, win, RXV_POOL);
    int32_t limit;
    int i;

    for (i = 0; i < RXV_POOL; i++) {
        struct row *r = &v->rows[i];

        if (i < n) {
            fill_row(v, r, win[i]);
            select_mark(r, win[i]->uid == log->selected_uid);
        } else if (!lv_obj_has_flag(r->row, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
            r->uid = 0;
            select_mark(r, 0);
        }
    }
    v->shown = n;
    caption(v, pos);
    note(v, rift_rxlog_count(log, log->filter));
    rift_label_set(lv_obj_get_child(v->pause, 0), log->paused ? "RESUME" : "PAUSE");
    {
        char word[24];

        snprintf(word, sizeof(word), "FILTER %s", rift_rxlog_filter_word(log->filter));
        rift_label_set(lv_obj_get_child(v->filter, 0), word);
    }
    rxv_paint_detail(v);

    /* How many rows are wholly in view, for the keys' paging. Measured here,
     * from the timer or a key - never from inside a layout pass. */
    lv_obj_update_layout(v->list);
    limit = lv_obj_get_content_height(v->list);
    v->visible = 0;
    for (i = 0; i < n; i++) {
        if (lv_obj_get_y(v->rows[i].row) + lv_obj_get_height(v->rows[i].row) > limit) {
            break;
        }
        v->visible++;
    }
    if (v->visible == 0 && n > 0) {
        v->visible = 1;
    }
}

void rxv_repaint(struct rift_rxlog_view *v, int now_too)
{
    struct rift_rxlog *log = &v->app->rxlog;
    int connected = rift_ipc_connected(&v->app->ipc);
    int64_t now = rift_app_now(v->app);
    int same_view = v->drawn_once && v->drawn_top == log->top_uid && v->drawn_sel == log->selected_uid &&
                    v->drawn_filter == log->filter && v->drawn_paused == log->paused &&
                    v->drawn_supported == log->supported && v->drawn_connected == connected;

    if (same_view && v->drawn_rev == log->revision) {
        return;
    }
    /* New traffic only: at most every LIVE_REPAINT_MS. A paused or held view
     * shows the same rows whatever arrives, so only the caption moves. */
    if (same_view && !now_too && now - v->drawn_ms < LIVE_REPAINT_MS) {
        /* Owed, not dropped: the app's timer comes back for it (rift_app.h
         * refresh_pending), so the last frame of a burst is not left
         * undrawn until the next once-a-second repaint. */
        v->app->refresh_pending = 1;
        return;
    }
    draw(v);
    v->drawn_once = 1;
    v->drawn_rev = log->revision;
    v->drawn_top = log->top_uid;
    v->drawn_sel = log->selected_uid;
    v->drawn_filter = log->filter;
    v->drawn_paused = log->paused;
    v->drawn_supported = log->supported;
    v->drawn_connected = connected;
    v->drawn_ms = now;
}

void rift_rxlog_view_refresh(struct rift_app *app)
{
    struct rift_rxlog_view *v = rxv_of(app);

    if (v) {
        rxv_repaint(v, 0);
    }
}

