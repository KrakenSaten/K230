/*
 * PocketNotes: a list of notes and a place to write one.
 *
 * Three screens in one body, one visible at a time: the list, the editor and
 * the delete confirmation. There is no search, no folders, no formatting and
 * no undo; a note is text, and the app's whole job is to keep it.
 *
 * The app never creates a keyboard and never names one. It asks the shell to
 * show the one keyboard (app.h, DS §17.4) and then forgets about it: what it
 * types with is a focused pocketui_text_field, and characters arrive there
 * whether they were tapped, typed on the host keyboard, or - later - on a
 * physical one.
 *
 * Saving is on the way out, in both directions: leaving the editor for the
 * list saves, and so does leaving the app, because the v0.1 lifecycle has no
 * pause and destroy() is the last moment an app gets (ADR-002).
 *
 * A save that fails does not let go of what it was given. Done keeps the
 * editor, the text and the note it belongs to, and can be pressed again;
 * only the app being torn down has nowhere to keep it, and that says so in
 * the log rather than closing quietly over it.
 *
 * LAYOUT. Each screen in two shapes, chosen from the body the app is given
 * and chosen again whenever that body changes size - the keyboard coming up
 * or going down is the everyday case (see "the layout" below): its content
 * above its actions when the body is tall, beside them when it is wide. The
 * orientation is the system's (DS section 21.2); nothing here asks what it
 * is, only how much room there is.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "notes_store.h"
#include "notes_view.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The wide shape's action rail: two actions of 140 px and the 8 px pair gap,
 * more than twice the 64 px touch minimum across, leaving the content about
 * three quarters of the landscape body (884 of 1192 px). */
#define NOTES_RAIL_W (2 * 140 + 8)

/* The portrait body's width on the reference panel (568 less the 20 px side
 * padding of DS section 7). The wide shape is only chosen when the content
 * beside the rail keeps at least this much, so a note is never narrower
 * there than in portrait; and the delete confirmation keeps this width in
 * the wide shape, where the whole body would stretch it into a banner. */
#define NOTES_COLUMN_W 528

enum notes_screen {
    NOTES_SCREEN_LIST = 0,
    NOTES_SCREEN_EDITOR,
    NOTES_SCREEN_CONFIRM
};

struct notes_app {
    lv_obj_t *frame;      /* the app's own box in the body: the three screens */
    lv_obj_t *screen[3];
    lv_obj_t *field;
    lv_obj_t *list_card;  /* the rows or the empty state, rebuilt with the list */
    lv_obj_t *new_note;
    lv_obj_t *actions;    /* the editor's Done and Delete */
    lv_obj_t *dialog;     /* the delete confirmation's panel */
    /* What the layout in force was chosen from, and the one place that
     * decides whether a pass is needed at all (pocketui.h). */
    struct pocketui_layout_guard layout_guard;
    int32_t field_min_h;  /* the field's own floor, as pocketui set it */
    bool wide;
    bool list_rows;       /* list_card holds rows, not the empty state */
    uint8_t screen_id;
    uint32_t open_id;    /* the note being edited, 0 when none */
    int open_existing;   /* 0 for a note that has never been stored */
    int open_readable;   /* 0 when the note is not editable - it could not be
                          * read, or is longer than the editor holds: do not
                          * save over it */
    char text[NOTES_MAX_BYTES + 1]; /* the note as it was read when opened */
};

static void show_screen(struct notes_app *a, enum notes_screen which);
static void build_list(struct notes_app *a);
static void shape_list(struct notes_app *a);

/* ---- saving ------------------------------------------------------------ */

/* Returns 0 when the note is safely stored, deliberately gone, or did not
 * need writing. A blank note is not worth a file, so it is removed rather
 * than kept as "Untitled"; a note that is not editable is never written over;
 * and a note nobody changed is not written at all. */
static int save_open_note(struct notes_app *a)
{
    const char *text;

    if (!a->open_id || !a->field) {
        return 0;
    }
    if (!a->open_readable) {
        return 0;
    }
    text = lv_textarea_get_text(a->field);
    /* Opening a note and leaving it is reading it. The same bytes written
     * again would only move its time and its place in the list. */
    if (a->open_existing && strcmp(text, a->text) == 0) {
        return 0;
    }
    if (notes_text_is_blank(text)) {
        /* A note emptied on purpose is removed, and a removal that did not
         * happen is a failure like any other: the note is still on disk, and
         * reporting success would send the owner back to a list that still
         * has it. */
        if (a->open_existing) {
            return notes_store_delete(a->open_id);
        }
        return 0;
    }
    return notes_store_write(a->open_id, text);
}

/* ---- the editor -------------------------------------------------------- */

/* An error caption under the field (DS §17.1), which takes its room from the
 * field (pocketui_text_field). The first caption of an app instance is
 * created here, and for one layout pass its size is not yet its own: the
 * field is squeezed past its real size - in the wide shape, where it has no
 * floor, to nothing - and starts scrolling its caret into view for that
 * moment. LVGL finishes that scroll after the field has its size back, so a
 * short note on unit A was left scrolled out of sight above its caption. The
 * editor is therefore laid out at once, the scroll begun for that moment is
 * dropped with the field's other animations, the field is brought back inside
 * its text, and the caret is placed again - a step aside and back, since
 * LVGL ignores placing it where it already is - which scrolls it into view
 * for the size the field really has and restarts its blink. */
static void show_field_error(struct notes_app *a, const char *message)
{
    int32_t pos;

    pocketui_text_field_set_error(a->field, message);
    lv_obj_update_layout(a->frame);
    lv_anim_delete(a->field, NULL);
    lv_obj_readjust_scroll(a->field, LV_ANIM_OFF);
    pos = lv_textarea_get_cursor_pos(a->field);
    lv_textarea_set_cursor_pos(a->field, pos > 0 ? pos - 1 : pos + 1);
    lv_textarea_set_cursor_pos(a->field, pos);
}

/* The way out of the editor, and the only place a note is written on the way
 * to the list. A save that failed does not take the editor with it: the text
 * is the only copy there is - open_id names the note it belongs to, and
 * opening another note over it would write this one's words into that one's
 * file. It stays here, typed as it was, and Done tries again. */
static void editor_leave(struct notes_app *a)
{
    if (save_open_note(a) != 0) {
        pocketos_shell_set_status_hint("Note not saved");
        show_field_error(a, "This note could not be saved. It is still here; Done tries again.");
        return;
    }
    pocketos_shell_set_status_hint("");
    pocketui_text_field_set_error(a->field, NULL);
    pocketos_shell_keyboard_hide();
    a->open_id = 0;
    build_list(a);
    show_screen(a, NOTES_SCREEN_LIST);
}

static void on_editor_done(lv_event_t *e)
{
    editor_leave(lv_event_get_user_data(e));
}

static void on_field_clicked(lv_event_t *e)
{
    struct notes_app *a = lv_event_get_user_data(e);

    /* Tapping back into the field after the keyboard was dismissed brings it
     * back. Focus is already the field's: pocketui_text_field handles that,
     * and showing the keyboard does not move it (DS §17.2). */
    if (!pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_show(POCKETOS_KB_NEWLINE, NULL, a);
    }
}

/* The note as read into the field. */
static void put_note_text(struct notes_app *a, int too_long)
{
    if (too_long) {
        /* With a cap set, LVGL takes text in a character at a time and stops
         * at the cap. It is lifted for as long as that takes, so what is
         * shown is the whole note; the field is disabled, so nothing can be
         * typed while it is. */
        lv_textarea_set_max_length(a->field, 0);
        lv_textarea_set_text(a->field, a->text);
        lv_textarea_set_max_length(a->field, NOTES_MAX_CHARS);
    } else {
        lv_textarea_set_text(a->field, a->text);
    }
}

static void open_editor(struct notes_app *a, uint32_t id, int existing)
{
    const char *refusal = NULL;
    int too_long = 0;
    int len;

    a->open_id = id;
    a->open_existing = existing;
    a->text[0] = '\0';
    if (existing) {
        len = notes_store_read(id, a->text, sizeof(a->text));
        if (len < 0) {
            /* Refuse to edit what we could not read, so a save cannot
             * replace a damaged note with an empty one. */
            a->text[0] = '\0';
            refusal = "This note could not be read. It is left exactly as it is.";
        } else if (len == 1 && a->text[0] == '\0') {
            a->text[0] = '\0';
        } else if (notes_text_chars(a->text) > NOTES_MAX_CHARS) {
            /* More than the editor holds, which only a note written
             * somewhere else can be. The field would keep the first
             * NOTES_MAX_CHARS characters and the way out would save those
             * over the rest, so it is shown and left alone instead. */
            too_long = 1;
            refusal = "This note is too long to edit here. It is left exactly as it is.";
        }
    }
    a->open_readable = refusal == NULL;
    /* The editor is shown as it will be seen - the keyboard up or down, the
     * caption there or not - before the note goes in. Setting the text lays
     * the screen out and scrolls the field to its caret for the size it has
     * then, and LVGL does not scroll it again when the field takes another
     * size: a note set into a hidden editor, whose field was still at its
     * three-line floor, opened scrolled past its end. */
    pocketui_text_field_set_enabled(a->field, a->open_readable);
    if (a->open_readable) {
        pocketui_text_field_set_error(a->field, NULL);
        pocketos_shell_keyboard_show(POCKETOS_KB_NEWLINE, NULL, a);
    } else {
        pocketui_text_field_set_error(a->field, refusal);
        pocketos_shell_keyboard_hide();
    }
    show_screen(a, NOTES_SCREEN_EDITOR);
    put_note_text(a, too_long);
    if (a->open_readable) {
        pos_input_focus(a->field);
    }
}

/* ---- the delete confirmation (DS §17.5) -------------------------------- */

static void on_confirm_cancel(lv_event_t *e)
{
    struct notes_app *a = lv_event_get_user_data(e);

    /* Back to the editor exactly as it was, keyboard and all. */
    show_screen(a, NOTES_SCREEN_EDITOR);
    if (a->open_readable) {
        pos_input_focus(a->field);
        pocketos_shell_keyboard_show(POCKETOS_KB_NEWLINE, NULL, a);
    }
}

static void on_confirm_delete(lv_event_t *e)
{
    struct notes_app *a = lv_event_get_user_data(e);

    /* Only here, and only on this press: nothing was removed when the dialog
     * opened (DS §17.5). A delete that failed leaves the note where it was,
     * so the editor does too, with its text: emptying the field and going to
     * the list would be this app agreeing that a note it can still see is
     * gone. */
    if (a->open_existing && notes_store_delete(a->open_id) != 0) {
        pocketos_shell_set_status_hint("Note not deleted");
        show_screen(a, NOTES_SCREEN_EDITOR);
        show_field_error(a, "This note could not be deleted. It is still here.");
        return;
    }
    pocketos_shell_set_status_hint("");
    pocketui_text_field_set_error(a->field, NULL);
    a->open_id = 0;
    lv_textarea_set_text(a->field, "");
    build_list(a);
    show_screen(a, NOTES_SCREEN_LIST);
}

static void on_editor_delete(lv_event_t *e)
{
    struct notes_app *a = lv_event_get_user_data(e);

    /* A dialog over a field dismisses the keyboard and takes the focus
     * (DS §17.5); Cancel gives both back. */
    pocketos_shell_keyboard_hide();
    show_screen(a, NOTES_SCREEN_CONFIRM);
}

/* ---- the list ---------------------------------------------------------- */

static void on_note_row(lv_event_t *e)
{
    struct notes_app *a = lv_event_get_user_data(e);
    uint32_t id = (uint32_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));

    open_editor(a, id, 1);
}

static void on_new_note(lv_event_t *e)
{
    struct notes_app *a = lv_event_get_user_data(e);
    uint32_t id = notes_store_next_id();

    if (id == 0) {
        pocketos_shell_set_status_hint("No room for another note");
        return;
    }
    pocketos_shell_set_status_hint("");
    open_editor(a, id, 0);
}

static void when_text(int64_t modified, char *out, size_t out_len)
{
    time_t t = (time_t)modified;
    struct tm tm;

    if (modified <= 0 || !localtime_r(&t, &tm)) {
        out[0] = '\0';
        return;
    }
    strftime(out, out_len, "%Y-%m-%d %H:%M", &tm);
}

static void build_list(struct notes_app *a)
{
    struct notes_entry entries[NOTES_MAX_NOTES];
    lv_obj_t *panel;
    int n;
    int i;

    /* Only the rows are rebuilt; New note stays where it was built. */
    if (a->list_card) {
        lv_obj_delete(a->list_card);
        a->list_card = NULL;
    }
    n = notes_store_list(entries, NOTES_MAX_NOTES);
    if (n < 0) {
        n = 0;
        pocketos_shell_set_status_hint("Notes storage unreadable");
    }

    if (n == 0) {
        /* The empty state of DS §9: a dashed panel, a disc, a caption. */
        lv_obj_t *empty = pocketui_card(a->screen[NOTES_SCREEN_LIST]);
        lv_obj_t *disc;
        lv_obj_t *glyph;

        lv_obj_set_style_border_side(empty, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_flex_align(empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(empty, 16, 0);
        disc = lv_obj_create(empty);
        lv_obj_remove_style_all(disc);
        pos_style_add(disc, POS_STYLE_SLAB, 0);
        lv_obj_set_size(disc, 80, 80);
        lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
        glyph = pocketui_label(disc, LV_SYMBOL_FILE, POS_STYLE_SYMBOL);
        pos_style_add(glyph, POS_STYLE_TEXT_SECONDARY, 0);
        lv_obj_center(glyph);
        pocketui_label(empty, "No notes yet", POS_STYLE_CAPTION);
        a->list_card = empty;
        a->list_rows = false;
    } else {
        panel = pocketui_card(a->screen[NOTES_SCREEN_LIST]);
        lv_obj_set_style_pad_hor(panel, POCKETUI_PAD, 0);
        lv_obj_set_style_pad_ver(panel, 0, 0);
        /* The rows scroll inside the list and nowhere else; how tall the
         * card may grow is the layout's (shape_list). */
        lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(panel, LV_DIR_VER);

        for (i = 0; i < n; i++) {
            lv_obj_t *row = lv_obj_create(panel);
            lv_obj_t *title;
            char when[24];

            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, LV_PCT(100), POCKETUI_ROW_H);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            /* The whole row is the hit area (DS §9). */
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
            pos_style_add(row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
            if (i + 1 < n) {
                pos_style_add(row, POS_STYLE_DIVIDER, 0);
            }
            lv_obj_set_user_data(row, (void *)(uintptr_t)entries[i].id);
            lv_obj_add_event_cb(row, on_note_row, LV_EVENT_CLICKED, a);

            title = pocketui_label(row, entries[i].title, POS_STYLE_ROW_TITLE);
            lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
            lv_obj_set_flex_grow(title, 1);
            if (!entries[i].readable) {
                pos_style_add(title, POS_STYLE_STATUS_WARN_TEXT, 0);
            }

            when_text(entries[i].modified, when, sizeof(when));
            pocketui_label(row, when, POS_STYLE_CAPTION);
        }
        a->list_card = panel;
        a->list_rows = true;
    }

    /* Before New note, which was built once and stays. */
    lv_obj_move_to_index(a->list_card, 0);
    shape_list(a);
}

/* ---- screens ----------------------------------------------------------- */

static void show_screen(struct notes_app *a, enum notes_screen which)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (i == (int)which) {
            lv_obj_clear_flag(a->screen[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(a->screen[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    a->screen_id = (uint8_t)which;
}

static lv_obj_t *make_screen(lv_obj_t *parent)
{
    lv_obj_t *s = lv_obj_create(parent);

    lv_obj_remove_style_all(s);
    lv_obj_set_width(s, LV_PCT(100));
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    /* The DS 7 gutter between content and actions, whichever way they lie. */
    lv_obj_set_style_pad_row(s, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_column(s, POCKETUI_PAD, 0);
    lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static void build_editor(struct notes_app *a)
{
    lv_obj_t *actions = lv_obj_create(a->screen[NOTES_SCREEN_EDITOR]);
    lv_obj_t *done;
    lv_obj_t *del;

    lv_obj_remove_style_all(actions);
    lv_obj_set_width(actions, LV_PCT(100));
    lv_obj_set_height(actions, 56); /* paired buttons, DS §7 */
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actions, 8, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    a->actions = actions;

    done = pocketui_button(actions, "Done", on_editor_done, a);
    lv_obj_set_height(done, 56);
    lv_obj_set_flex_grow(done, 1);
    lv_obj_clear_flag(done, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    del = pocketui_button(actions, "Delete", on_editor_delete, a);
    lv_obj_set_height(del, 56);
    lv_obj_set_flex_grow(del, 1);
    lv_obj_clear_flag(del, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    /* Destructive, so it does not carry the accent (DS §17.5). */
    lv_obj_remove_style(del, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(del, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
    pos_style_add(del, POS_STYLE_BUTTON_SECONDARY, 0);

    a->field = pocketui_text_field(a->screen[NOTES_SCREEN_EDITOR], "Write a note", false);
    lv_textarea_set_max_length(a->field, NOTES_MAX_CHARS);
    lv_obj_set_flex_grow(lv_obj_get_parent(a->field), 1);
    lv_obj_add_event_cb(a->field, on_field_clicked, LV_EVENT_CLICKED, a);
    a->field_min_h = lv_obj_get_style_min_height(a->field, LV_PART_MAIN);
}

static void build_confirm(struct notes_app *a)
{
    lv_obj_t *panel = pocketui_card(a->screen[NOTES_SCREEN_CONFIRM]);
    lv_obj_t *body;
    lv_obj_t *buttons;
    lv_obj_t *cancel;
    lv_obj_t *confirm;

    a->dialog = panel;
    pocketui_label(panel, "Delete this note?", POS_STYLE_TITLE);
    body = pocketui_label(panel, "The note is removed from this device. "
                                 "There is no undo.",
                          POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_pad_top(body, 12, 0);
    lv_obj_set_style_pad_bottom(body, 20, 0);

    buttons = lv_obj_create(panel);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, 56);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);

    /* Cancel first and accented: deleting a note cannot be undone, so the
     * bright treatment goes to the safe choice (DS §17.5, the power-off
     * precedent). */
    cancel = pocketui_button(buttons, "Cancel", on_confirm_cancel, a);
    lv_obj_set_height(cancel, 56);
    lv_obj_set_flex_grow(cancel, 1);

    confirm = pocketui_button(buttons, "Delete", on_confirm_delete, a);
    lv_obj_set_height(confirm, 56);
    lv_obj_set_flex_grow(confirm, 1);
    lv_obj_remove_style(confirm, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(confirm, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED),
                        LV_STATE_PRESSED);
    pos_style_add(confirm, POS_STYLE_BUTTON_SECONDARY, 0);

    pos_input_add_obj(cancel);
    pos_input_add_obj(confirm);
}

/* ---- the layout -------------------------------------------------------- *
 *
 * The screens sit in one frame that is exactly the body's content box - the
 * whole of the room the shell gives the app - and each is shaped from the
 * size of that box alone:
 *
 *   TALL (portrait: 528 x 1060 on the reference panel, 528 x 764 with the
 *   keyboard up). Content above, actions below it or above it as they always
 *   were: the list's rows over New note, Done and Delete over the field, the
 *   confirmation across the body's top. The v0.0.10 layout, except that the
 *   field now reaches the foot of the body (build_frame says why it did not)
 *   and that nothing reaches into the corners there (below).
 *
 *   WIDE (landscape: 1192 x 396, and 1192 x 100 with the keyboard up). Height
 *   is what landscape is short of, and above a keyboard a field under a row
 *   of buttons had 4 px left to show a note in. So the actions move beside
 *   the content instead, into a rail of NOTES_RAIL_W on the right: New note
 *   beside the rows, Done and Delete beside the field, both at the top of the
 *   rail and at their usual heights. The content takes the rest of the width
 *   and the full height. The confirmation keeps its portrait width, centred.
 *   Chosen only when the content beside the rail keeps NOTES_COLUMN_W, so no
 *   note is narrower than in portrait. No control's size is shared out of
 *   the height, so no body a panel gives can bring one under 64 px.
 *
 * Neither shape needs the body to scroll, and it never does (DS 17.1).
 *
 * The objects are built once and only shaped here - the flow, sizes and the
 * rail - so the note, its caret, the focus and the keyboard are never touched
 * by a change of shape. The list's rows are the one exception, rebuilt from
 * the store as they always were, and they are shaped when they are built.
 *
 * Whatever the shape, the content clears the panel's unsafe area (DS 21.1,
 * 22.2): anything that reaches the foot of the body - a long list, the field
 * with the keyboard down - would reach 10 px into the 30 px corner squares
 * of the reference panel, so the frame pads its foot by however far a corner
 * square reaches into the body, from the platform's description
 * (pos_display_rect_insets, the rule Calculator uses too). With the keyboard
 * up the foot is far from the corners and the pad is 0; on a panel with
 * square corners it is always 0. */

/* The rows, then New note. Tall: the card grows into what New note leaves it
 * but never past its own rows, so a short list is exactly as tall as it
 * always was, a long one scrolls under the finger, and New note stays on
 * screen below it however many notes there are. A card that simply grew put
 * New note, and then the oldest notes, past the bottom of a screen that does
 * not scroll, above a body that must not (DS §17.1). Wide: the card takes
 * the width and is as tall as its rows up to the full height, and New note
 * waits at the top of the rail. */
static void shape_list(struct notes_app *a)
{
    lv_obj_t *s = a->screen[NOTES_SCREEN_LIST];

    lv_obj_set_flex_flow(s, a->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(a->new_note, a->wide ? NOTES_RAIL_W : LV_PCT(100));
    if (!a->list_card) {
        return;
    }
    if (a->wide) {
        lv_obj_set_flex_grow(a->list_card, 1);
        lv_obj_set_style_max_height(a->list_card, LV_PCT(100), 0);
    } else if (a->list_rows) {
        lv_obj_set_flex_grow(a->list_card, 1);
        lv_obj_set_style_max_height(a->list_card, LV_SIZE_CONTENT, 0);
    } else {
        lv_obj_set_flex_grow(a->list_card, 0);
        lv_obj_remove_local_style_prop(a->list_card, LV_STYLE_MAX_HEIGHT, 0);
    }
}

/* Done and Delete, then the field. Wide: the flow runs right to left, so the
 * actions - first in the tree - take the rail on the right and the field's
 * wrapper the rest of the row, at the full height.
 *
 * The field grows into its wrapper and an error caption takes its room from
 * the field (pocketui_text_field). Its floor is DS 17.1's three body lines,
 * which pocketui counts as the font's line height times the 1.5 line-height
 * of DS 3: 94 px in Normal, 117 px in Outdoor. Above the keyboard in
 * landscape the whole wrapper is 100 px: that holds three lines as the field
 * draws them (21 px apart, 26 in Outdoor) but not the Outdoor floor, and not
 * the Normal floor and a caption. A floor the wrapper cannot hold keeps the
 * field taller than its box, which pushes the caption out of sight (a failed
 * save keeps the keyboard up) and scrolls the caret into the part that is cut
 * off. So in the wide shape the field has no floor of its own: it is the
 * whole wrapper, or all of it but a caption's room while an error is shown.
 * Tall keeps pocketui's floor. */
static void shape_editor(struct notes_app *a)
{
    lv_obj_t *wrap = lv_obj_get_parent(a->field);

    lv_obj_set_flex_flow(a->screen[NOTES_SCREEN_EDITOR],
                         a->wide ? LV_FLEX_FLOW_ROW_REVERSE : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(a->actions, a->wide ? NOTES_RAIL_W : LV_PCT(100));
    lv_obj_set_height(wrap, a->wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(a->field, a->wide ? 0 : a->field_min_h, 0);
}

/* The panel across the top of the body; in the wide shape at its portrait
 * width, centred. */
static void shape_confirm(struct notes_app *a)
{
    lv_flex_align_t across = a->wide ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START;

    lv_obj_set_flex_align(a->screen[NOTES_SCREEN_CONFIRM], LV_FLEX_ALIGN_START, across, across);
    lv_obj_set_width(a->dialog, a->wide ? NOTES_COLUMN_W : LV_PCT(100));
}

static void layout(struct notes_app *a)
{
    struct pos_insets in;
    const lv_area_t *box;
    int32_t w;
    int32_t h;

    /* Nothing to lay out in, or nothing the layout is chosen from has
     * changed: PocketUI owns that decision for every responsive app, and
     * hands back the corner clearance the platform rule gives this box. */
    if (!pocketui_layout_begin(&a->layout_guard, a->frame, &in)) {
        return;
    }
    box = &a->layout_guard.area;
    lv_obj_set_style_pad_left(a->frame, in.left, 0);
    lv_obj_set_style_pad_top(a->frame, in.top, 0);
    lv_obj_set_style_pad_right(a->frame, in.right, 0);
    lv_obj_set_style_pad_bottom(a->frame, in.bottom, 0);
    w = lv_area_get_width(box) - in.left - in.right;
    h = lv_area_get_height(box) - in.top - in.bottom;
    a->wide = w > h && w >= NOTES_COLUMN_W + POCKETUI_PAD + NOTES_RAIL_W;
    shape_list(a);
    shape_editor(a);
    shape_confirm(a);
}

/* The frame is the body's content box, so this is the body changing size:
 * the keyboard came up or went down, or this is the first layout pass after
 * the app was built. */
static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

static void build_frame(struct notes_app *a, lv_obj_t *root)
{
    lv_obj_t *frame = lv_obj_create(root);

    lv_obj_remove_style_all(frame);
    /* Exactly the body's content box, whatever is in it, so the shape is
     * always chosen from the room the shell gives and never from the size of
     * what the shape itself put there. */
    lv_obj_set_size(frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(frame, LV_FLEX_FLOW_COLUMN);
    /* No gap between the screens, only one of which is ever shown. In the
     * body's own flow the gap cost the editor 20 px: LVGL 9.5 takes a gap
     * from a growing item for every sibling before it, hidden ones included,
     * which above a landscape keyboard is a fifth of the editor. */
    lv_obj_set_style_pad_row(frame, 0, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    a->frame = frame;
}

/* ---- the app ----------------------------------------------------------- */

static void *notes_create(lv_obj_t *root)
{
    struct notes_app *a = lv_malloc_zeroed(sizeof(*a));
    int i;

    if (!a) {
        return NULL;
    }
    build_frame(a, root);
    for (i = 0; i < 3; i++) {
        a->screen[i] = make_screen(a->frame);
    }
    build_editor(a);
    build_confirm(a);
    a->new_note = pocketui_button(a->screen[NOTES_SCREEN_LIST], "New note", on_new_note, a);
    build_list(a);
    show_screen(a, NOTES_SCREEN_LIST);
    /* Only now: building lays objects out as it goes, and the layout step
     * shapes objects that must all exist. */
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    layout(a);
    return a;
}

static void notes_destroy(void *priv)
{
    struct notes_app *a = priv;

    if (!a) {
        return;
    }
    /* The frame outlives this by a moment, until the shell deletes the app's
     * objects; nothing may call back into a freed app in between. */
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    /* The last moment this app gets: v0.1 has no pause, so whatever is in
     * the editor is saved here or lost (ADR-002). There is nowhere left to
     * put it and no one left to ask, so a failure here is text the owner
     * typed and will not get back. It is at least said out loud: silence
     * would make it look like an ordinary close.
     *
     * The log is the only place it can be said. The shell clears the status
     * hint immediately after destroy() returns (shell.c, app_close), so a
     * hint set here is overwritten before a frame is drawn and no one ever
     * sees it. */
    if (save_open_note(a) != 0) {
        LOG_ERROR("notes: note %u could not be saved on the way out; "
                  "its edits are lost", (unsigned)a->open_id);
    }
    pocketos_shell_keyboard_hide();
    lv_free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_notes);

const struct pocketos_app app_notes = {
    .id = "notes",
    .name = "Notes",
    .icon = LV_SYMBOL_FILE,
    .icon_mask = &pos_app_icon_notes,
    .create = notes_create,
    .tick = NULL,
    .destroy = notes_destroy,
};
