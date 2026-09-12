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
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "notes_store.h"
#include "notes_view.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

enum notes_screen {
    NOTES_SCREEN_LIST = 0,
    NOTES_SCREEN_EDITOR,
    NOTES_SCREEN_CONFIRM
};

struct notes_app {
    lv_obj_t *body;
    lv_obj_t *screen[3];
    lv_obj_t *field;
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

/* The way out of the editor, and the only place a note is written on the way
 * to the list. A save that failed does not take the editor with it: the text
 * is the only copy there is - open_id names the note it belongs to, and
 * opening another note over it would write this one's words into that one's
 * file. It stays here, typed as it was, and Done tries again. */
static void editor_leave(struct notes_app *a)
{
    if (save_open_note(a) != 0) {
        pocketos_shell_set_status_hint("Note not saved");
        pocketui_text_field_set_error(a->field,
                                      "This note could not be saved. It is still "
                                      "here; Done tries again.");
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
    if (too_long) {
        /* With a cap set, LVGL takes text in a character at a time and stops
         * at the cap. It is lifted for as long as that takes, so what is
         * shown is the whole note; the field is disabled below, so nothing
         * can be typed while it is. */
        lv_textarea_set_max_length(a->field, 0);
        lv_textarea_set_text(a->field, a->text);
        lv_textarea_set_max_length(a->field, NOTES_MAX_CHARS);
    } else {
        lv_textarea_set_text(a->field, a->text);
    }
    pocketui_text_field_set_enabled(a->field, a->open_readable);
    if (a->open_readable) {
        pocketui_text_field_set_error(a->field, NULL);
        pos_input_focus(a->field);
        pocketos_shell_keyboard_show(POCKETOS_KB_NEWLINE, NULL, a);
    } else {
        pocketui_text_field_set_error(a->field, refusal);
        pocketos_shell_keyboard_hide();
    }
    show_screen(a, NOTES_SCREEN_EDITOR);
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
        pocketui_text_field_set_error(a->field,
                                      "This note could not be deleted. It is "
                                      "still here.");
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

    lv_obj_clean(a->screen[NOTES_SCREEN_LIST]);
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
    } else {
        panel = pocketui_card(a->screen[NOTES_SCREEN_LIST]);
        lv_obj_set_style_pad_hor(panel, POCKETUI_PAD, 0);
        lv_obj_set_style_pad_ver(panel, 0, 0);
        /* The rows scroll inside the list and nowhere else. The card grows
         * into what New note leaves it but never past its own rows, so a
         * short list is exactly as tall as it always was, a long one
         * scrolls under the finger, and New note stays on screen below it
         * however many notes there are. A card that simply grew put New
         * note, and then the oldest notes, past the bottom of a screen that
         * does not scroll, above a body that must not (DS §17.1). */
        lv_obj_set_flex_grow(panel, 1);
        lv_obj_set_style_max_height(panel, LV_SIZE_CONTENT, 0);
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
    }

    pocketui_button(a->screen[NOTES_SCREEN_LIST], "New note", on_new_note, a);
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
    lv_obj_set_style_pad_row(s, POCKETUI_PAD, 0);
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
}

static void build_confirm(struct notes_app *a)
{
    lv_obj_t *panel = pocketui_card(a->screen[NOTES_SCREEN_CONFIRM]);
    lv_obj_t *body;
    lv_obj_t *buttons;
    lv_obj_t *cancel;
    lv_obj_t *confirm;

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

/* ---- the app ----------------------------------------------------------- */

static void *notes_create(lv_obj_t *root)
{
    struct notes_app *a = lv_malloc_zeroed(sizeof(*a));
    int i;

    if (!a) {
        return NULL;
    }
    a->body = root;
    for (i = 0; i < 3; i++) {
        a->screen[i] = make_screen(root);
    }
    build_editor(a);
    build_confirm(a);
    build_list(a);
    show_screen(a, NOTES_SCREEN_LIST);
    return a;
}

static void notes_destroy(void *priv)
{
    struct notes_app *a = priv;

    if (!a) {
        return;
    }
    /* The last moment this app gets: v0.1 has no pause, so whatever is in
     * the editor is saved here or lost (ADR-002). There is nowhere left to
     * put it and no one left to ask, so a failure here is text the owner
     * typed and will not get back. It is at least said out loud: silence
     * would make it look like an ordinary close. */
    if (save_open_note(a) != 0) {
        LOG_ERROR("notes: note %u could not be saved on the way out; "
                  "its edits are lost", (unsigned)a->open_id);
        pocketos_shell_set_status_hint("Note not saved");
    }
    pocketos_shell_keyboard_hide();
    lv_free(a);
}

const struct pocketos_app app_notes = {
    .id = "notes",
    .name = "Notes",
    .icon = LV_SYMBOL_FILE,
    .create = notes_create,
    .tick = NULL,
    .destroy = notes_destroy,
};
