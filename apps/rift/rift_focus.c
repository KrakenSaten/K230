/*
 * RIFT's composers and the keys: which of the two fields a conversation is
 * written in has the focus, what the keys do while it has it, and how it
 * comes by it when a conversation opens. Kept apart from rift_app.c, which
 * owns the chrome and the lifecycle, so neither file is everything.
 *
 * The landscape composer is the command line's field (rift_app.c); the
 * portrait one is the thread's (ui/rift_thread.c). Both are followed here
 * the same way.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_app.h"

#include "pos_input.h"
#include "rift_comms.h"
#include "rift_msgact.h"

#include <string.h>

/* Which of the two has the focus is LVGL's business, not this app's: the
 * command line's field and the key sink are both in the one Doors focus
 * group, and TAB moves between them because that is what a focus group
 * does (DS §17.2). This only *follows* that, so the hint line and the
 * arrow keys know where the keys are going. */
static void on_composer_focus(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    a->composer_focused = (lv_event_get_code(e) == LV_EVENT_FOCUSED);
    if (!a->composer_focused) {
        a->composer_auto = 0;
    }
    /* Asked for, not done here. This runs inside lv_group_focus_obj, which
     * sends DEFOCUSED to the old object and *abandons the focus change* if
     * that event does not come back clean - and a refresh rebuilds rows,
     * which is exactly the kind of thing that does not. Leaving the group's
     * bookkeeping alone and repainting on the next timer pass keeps Esc's
     * way out of the composer working. */
    a->refresh_pending = 1;
}

/* Keys while a composer holds focus - the landscape command line's field or
 * the portrait thread's. Enter is the field's own READY event. Up and down
 * step through the conversations: a one-line field has nothing above or
 * below its caret, and the composer has the focus as soon as a conversation
 * opens, so without this the list could not be walked by key any more. Esc
 * clears what was typed and, when there is nothing left to clear, hands the
 * list its focus back - or, when the focus came with opening the
 * conversation rather than by TAB, does what Esc did from the list: back to
 * ACTIVITY.
 *
 * TAB is deliberately not handled. It reaches the field as character 9 and
 * the text area inserts it, which is a tab in the message - legal text, one
 * of the two control characters mesh.send takes. Taking it back off the
 * field to move focus would mean undoing an edit the widget has already
 * made; Esc is the way out, and the hint line says so. */
static void on_composer_key(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);

    lv_obj_t *field = lv_event_get_target_obj(e);
    const char *text;
    int typed = 0;
    int i;

    /* The actions on a message have the keys while they are open, even the
     * few that reach the composer before the focus has followed them (it
     * moves from the timer). The Esc the field took in is taken out again. */
    if (rift_msgact_id(rift_comms_actions(a)) && field) {
        if (key == LV_KEY_ESC) {
            lv_textarea_delete_char(field);
        }
        (void)rift_comms_key(a, key);
        return;
    }
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        (void)rift_comms_step(a, key == LV_KEY_UP ? -1 : 1);
        return;
    }
    /* LEFT with nothing typed has no caret to move: it goes up into the
     * thread, onto the newest message and its actions (ui/rift_msgact.h),
     * and the keys go with it - from the timer, as Esc's do. */
    if (key == LV_KEY_LEFT && field && lv_textarea_get_text(field)[0] == '\0') {
        if (rift_comms_select_message(a)) {
            a->focus_list_pending = 1;
            a->refresh_pending = 1;
        }
        return;
    }
    if (key != LV_KEY_ESC || !field) {
        return;
    }
    /* The field already holds this Esc.
     *
     * A text area's own class handler runs before any callback added to it
     * and puts the key in the buffer, so by the time this is reached the
     * field contains character 27 whether or not anything was typed before
     * it. Asking whether the field is empty would therefore always answer
     * no, and Esc would never do anything but clear itself. What counts as
     * typed is a character somebody could have meant: a control character
     * is not one, and mesh.send would refuse it anyway. */
    text = lv_textarea_get_text(field);
    for (i = 0; text && text[i]; i++) {
        if ((unsigned char)text[i] >= 0x20 && (unsigned char)text[i] != 0x7F) {
            typed = 1;
            break;
        }
    }
    lv_textarea_set_text(field, "");
    if (typed) {
        /* There was something to clear, and now it is cleared. */
        rift_model_send_clear(&a->model);
        rift_app_refresh(a);
        return;
    }
    if (a->composer_auto) {
        /* Nobody went to this field: it took the focus when the
         * conversation opened. Esc is the list's, as it was. */
        rift_app_show_section(a, RIFT_SEC_ACTIVITY);
        return;
    }
    /* Nothing to clear, so Esc means "give the list its focus back". Asked
     * for rather than done here: changing the group's focus from inside the
     * event LVGL is dispatching does not stick. */
    a->focus_list_pending = 1;
    a->refresh_pending = 1;
}

/* The open conversation's composer takes the keys (focus_composer_pending):
 * the command line's field in landscape, the thread's in portrait. Nothing
 * is shown or raised by it - the touch keyboard still comes up on a tap, as
 * it did - and it takes the focus only from where a reader left it by
 * opening a conversation: the key sink, a composer, or nothing. A dialog
 * that has the keys (the shell redirects the group to it) or a form's field
 * keeps them, and the request then waits for neither: it is dropped. One
 * that cannot be met yet - the field is disabled while a message is on its
 * way, or the service is not answering - stays asked. */
void rift_focus_composer(struct rift_app *a)
{
    lv_obj_t *field = a->wide ? a->composer : rift_comms_field(a);
    lv_obj_t *has;

    if (a->section != RIFT_SEC_COMMS || !rift_comms_open_peer(a)) {
        a->focus_composer_pending = 0;
        return;
    }
    if (pos_input_group_redirected()) {
        a->focus_composer_pending = 0;
        return;
    }
    has = pos_input_focused();
    if (has == field) {
        a->focus_composer_pending = 0;
        a->composer_auto = 1;
        return;
    }
    if (has && has != a->keysink && has != a->composer && has != rift_comms_field(a) &&
        lv_obj_is_visible(has)) {
        a->focus_composer_pending = 0;
        return;
    }
    if (!field || lv_obj_get_group(field) == NULL || !lv_obj_is_visible(field) ||
        lv_obj_has_state(field, LV_STATE_DISABLED)) {
        return; /* not yet: asked again on the next pass */
    }
    pos_input_focus(field);
    if (pos_input_focused() == field) {
        a->focus_composer_pending = 0;
        a->composer_auto = 1;
    }
}

void rift_focus_attach(struct rift_app *a)
{
    lv_obj_t *field[2];
    int i;

    if (!a) {
        return;
    }
    field[0] = a->composer;
    field[1] = rift_comms_field(a);
    for (i = 0; i < 2; i++) {
        if (field[i]) {
            lv_obj_add_event_cb(field[i], on_composer_key, LV_EVENT_KEY, a);
            lv_obj_add_event_cb(field[i], on_composer_focus, LV_EVENT_FOCUSED, a);
            lv_obj_add_event_cb(field[i], on_composer_focus, LV_EVENT_DEFOCUSED, a);
        }
    }
    /* A screen built over a session that already has a conversation open in
     * COMMS (DS §51) is that conversation opened again. */
    a->composer_focused = 0;
    a->composer_auto = 0;
    a->focus_composer_pending = (a->section == RIFT_SEC_COMMS && a->have_conv);
}
