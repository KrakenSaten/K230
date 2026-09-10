/*
 * PocketNotes in the running app, driven by a real LVGL pointer device and a
 * real touch keyboard, against a real (temporary) store.
 *
 * Nothing here sets the field's text to make a note: every character is
 * tapped on the keyboard, travels the logical stream, and lands in the
 * focused field, the way it will on the panel. What is then read back off
 * the disk is what the store actually kept.
 *
 * The app is hosted the way the shell hosts it, but the shell itself is not
 * here, so the three app.h keyboard entry points are implemented below
 * against the real pos_keyboard. That is exactly what the shell does, and it
 * keeps the app honest: it can only ask, and it never sees a keyboard.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/notes_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "notes_store.h"
#include "notes_view.h"
#include "pocketui.h"
#include "pos_keyboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
#define STATUS_H POCKETUI_STATUS_BAR_H

extern const struct pocketos_app app_notes;

static int failed;
static int checks;
static char root[128];

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void check_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want);
    }
}

/* ---- the shell's side of app.h, as the shell implements it ------------- */

static lv_obj_t *g_keyboard;
static lv_obj_t *g_content;
static char g_hint[64];

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(g_hint, sizeof(g_hint), "%s", text ? text : "");
}

void pocketos_shell_go_home(void) { }
int pocketos_shell_reduced_motion(void) { return 0; }
const char *pocketos_shell_radio_state(void) { return NULL; }

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret,
                                  void (*on_done)(void *user), void *user)
{
    (void)on_done;
    (void)user;
    pos_keyboard_set_return(g_keyboard, ret == POCKETOS_KB_NEWLINE ? POS_KB_RETURN_NEWLINE
                                                                   : POS_KB_RETURN_DONE);
    lv_obj_set_height(g_content, PANEL_H - STATUS_H - POS_KB_H);
    pos_keyboard_show(g_keyboard);
}

void pocketos_shell_keyboard_hide(void)
{
    pos_keyboard_hide(g_keyboard);
    lv_obj_set_height(g_content, PANEL_H - STATUS_H);
}

int pocketos_shell_keyboard_visible(void)
{
    return pos_keyboard_is_shown(g_keyboard);
}

/* ---- display and finger ------------------------------------------------ */

static uint8_t draw_buf[PANEL_W * 40 * 2];
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = finger_state;
    data->point = finger_point;
}

static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static void drain(void)
{
    int t;

    for (t = 0; t < 300 && pos_input_queued() > 0; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
    pump(40);
}

static void tap_obj(lv_obj_t *obj)
{
    lv_area_t a;

    if (!obj) {
        printf("FAIL tap on a missing object\n");
        failed++;
        checks++;
        return;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
    drain();
}

static void tap_key(const char *label)
{
    tap_obj(pos_keyboard_key(g_keyboard, label));
}

/* Type it the way a person would: the alpha layer shows lower case, so a
 * capital needs Shift first. One-shot Shift clears itself after the letter,
 * which is why no capital here ever leaks into the next one. */
static void type_text(const char *s)
{
    char one[2] = { 0, 0 };

    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            tap_key("SHIFT");
        }
        one[0] = *s;
        tap_key(one);
    }
}

/* ---- finding things in the app's tree ---------------------------------- */

/* Depth-first search for a label with this text; returns its clickable
 * ancestor, which is what a finger would press. */
static lv_obj_t *find_labelled(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    /* A hidden screen is still in the tree, and both the editor and the
     * confirmation carry a button labelled Delete. A finger cannot press
     * what is not shown, so neither may this. */
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && strcmp(t, text) == 0) {
            lv_obj_t *p = obj;

            while (p && !lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE)) {
                p = lv_obj_get_parent(p);
            }
            return p ? p : obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_labelled(lv_obj_get_child(obj, i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static lv_obj_t *find_field(lv_obj_t *obj)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        return obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_field(lv_obj_get_child(obj, i));

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static int label_present(lv_obj_t *obj, const char *text)
{
    return find_labelled(obj, text) != NULL;
}

/* ---- the app, hosted the way the shell hosts it ------------------------ */

static lv_obj_t *app_body;
static void *app_priv;

static void app_start(void)
{
    app_body = lv_obj_create(g_content);
    lv_obj_remove_style_all(app_body);
    lv_obj_set_size(app_body, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(app_body, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_row(app_body, POCKETUI_PAD, 0);
    lv_obj_add_flag(app_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(app_body, LV_DIR_VER);
    app_priv = app_notes.create(app_body);
    pump(60);
}

/* The shell's app_close(): hide the keyboard, destroy, delete the root. */
static void app_stop(void)
{
    pocketos_shell_keyboard_hide();
    app_notes.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_body);
    app_body = NULL;
    pump(60);
}

static void wipe(void)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        /* nothing there yet */
    }
}

int main(void)
{
    lv_display_t *disp;
    lv_indev_t *finger;
    struct notes_entry list[NOTES_MAX_NOTES];
    char text[NOTES_MAX_BYTES + 1];
    lv_obj_t *field;
    int n;

    snprintf(root, sizeof(root), "/tmp/pocketnotes-app-%u", (unsigned)getpid());
    setenv("POCKETOS_STATE_DIR", root, 1);
    wipe();

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    finger = lv_indev_create();
    lv_indev_set_type(finger, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(finger, read_cb);

    pos_input_init();
    pocketui_init();
    pocketui_style_screen(lv_screen_active());

    g_content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_content);
    lv_obj_set_size(g_content, PANEL_W, PANEL_H - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    g_keyboard = pos_keyboard_create(lv_screen_active());
    pump(60);

    /* ---- 1. the empty state -------------------------------------------- */

    app_start();
    check("an empty store shows the empty state", label_present(app_body, "No notes yet"));
    check("and offers a new note", label_present(app_body, "New note"));
    check("the keyboard is not up on the list", !pocketos_shell_keyboard_visible());

    /* ---- 2. create a note and type into it ----------------------------- */

    tap_obj(find_labelled(app_body, "New note"));
    check("the editor opens", label_present(app_body, "Done"));
    check("the keyboard comes up with it", pocketos_shell_keyboard_visible());
    field = find_field(app_body);
    check("the editor has a field", field != NULL);
    check("and it is focused", pos_input_focused() == field);

    type_text("Shopping");
    check_str("typed characters reach the note", lv_textarea_get_text(field), "Shopping");
    check("focus stayed on the field while typing", pos_input_focused() == field);

    /* ---- 3. a newline, and the second line ----------------------------- */

    tap_key("ENTER");
    type_text("milk");
    check_str("Enter breaks the line", lv_textarea_get_text(field), "Shopping\nmilk");

    /* ---- 4. æ ø å through the symbol layer ----------------------------- */

    tap_key("ENTER");
    tap_key("?123");
    tap_key("\xC3\xA6");
    tap_key("\xC3\xB8");
    tap_key("\xC3\xA5");
    tap_key("ABC");
    check_str("Norwegian letters type like any other",
              lv_textarea_get_text(field), "Shopping\nmilk\n\xC3\xA6\xC3\xB8\xC3\xA5");

    /* ---- 5. Done saves and returns to the list ------------------------- */

    tap_obj(find_labelled(app_body, "Done"));
    check("the keyboard goes away with the editor",
          !pocketos_shell_keyboard_visible());
    check("the list is back", label_present(app_body, "New note"));
    check("titled by the note's first line", label_present(app_body, "Shopping"));

    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("one note was stored", n == 1);
    check("with the text that was typed",
          notes_store_read(list[0].id, text, sizeof(text)) ==
              (int)strlen("Shopping\nmilk\n\xC3\xA6\xC3\xB8\xC3\xA5"));
    check_str("byte for byte", text, "Shopping\nmilk\n\xC3\xA6\xC3\xB8\xC3\xA5");

    /* ---- 6. reopen it -------------------------------------------------- */

    tap_obj(find_labelled(app_body, "Shopping"));
    field = find_field(app_body);
    check_str("reopening shows what was written",
              lv_textarea_get_text(field), "Shopping\nmilk\n\xC3\xA6\xC3\xB8\xC3\xA5");
    check("the keyboard is up again", pocketos_shell_keyboard_visible());

    /* Punctuation lives on the symbol layer, and the layer the keyboard is
     * left on is its own business, not the app's. */
    tap_key("?123");
    tap_key("!");
    tap_key("ABC");
    tap_obj(find_labelled(app_body, "Done"));
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("editing does not make a second note", n == 1);
    notes_store_read(list[0].id, text, sizeof(text));
    check("the edit was saved",
          text[strlen(text) - 1] == '!');

    /* ---- 7. a blank note is not worth a file --------------------------- */

    tap_obj(find_labelled(app_body, "New note"));
    tap_key("SPACE");
    tap_key("ENTER");
    tap_obj(find_labelled(app_body, "Done"));
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("a note with only whitespace is not stored", n == 1);

    /* ---- 8. leaving the app saves what was being written ---------------- */

    tap_obj(find_labelled(app_body, "New note"));
    type_text("draft");
    app_stop();                 /* the shell closing the app */
    check("the keyboard is down after the app closes",
          !pocketos_shell_keyboard_visible());
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("an unsaved edit survives the app closing", n == 2);
    app_start();
    check("and is there on the next launch", label_present(app_body, "draft"));

    /* ---- 9. delete: cancel changes nothing ----------------------------- */

    tap_obj(find_labelled(app_body, "draft"));
    check("the keyboard is up in the editor", pocketos_shell_keyboard_visible());
    tap_obj(find_labelled(app_body, "Delete"));
    check("the confirmation asks first", label_present(app_body, "Delete this note?"));
    check("the keyboard is dismissed for the dialog (DS §17.5)",
          !pocketos_shell_keyboard_visible());
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("nothing is deleted when the dialog opens", n == 2);

    tap_obj(find_labelled(app_body, "Cancel"));
    check("Cancel returns to the editor", label_present(app_body, "Done"));
    check("and brings the keyboard back", pocketos_shell_keyboard_visible());
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("and deleted nothing", n == 2);

    /* ---- 10. delete: confirm removes it -------------------------------- */

    tap_obj(find_labelled(app_body, "Delete"));   /* editor: open the dialog */
    tap_obj(find_labelled(app_body, "Delete"));   /* dialog: confirm */
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("confirming deletes the note", n == 1);
    check("and returns to the list", label_present(app_body, "New note"));
    check("the deleted note is gone from the list", !label_present(app_body, "draft"));
    check("the other note is untouched", label_present(app_body, "Shopping"));

    /* ---- 11. a note that is not text is shown, not edited over ---------- */

    app_stop();
    {
        char path[256];
        FILE *f;

        snprintf(path, sizeof(path), "%s/notes/note-00000042.txt", root);
        f = fopen(path, "wb");
        if (f) {
            (void)!fwrite("bad \xFF byte", 1, 10, f);
            fclose(f);
        }
    }
    app_start();
    check("an unreadable note is listed", label_present(app_body, "Unreadable note"));
    tap_obj(find_labelled(app_body, "Unreadable note"));
    field = find_field(app_body);
    check("opening it does not offer an editable field",
          lv_obj_has_state(field, LV_STATE_DISABLED));
    check("and says why", label_present(app_body, "This note could not be read. "
                                                  "It is left exactly as it is."));
    check("the keyboard stays down for it", !pocketos_shell_keyboard_visible());
    tap_obj(find_labelled(app_body, "Done"));
    {
        char path[256];
        FILE *f;
        char raw[32];
        size_t got = 0;

        snprintf(path, sizeof(path), "%s/notes/note-00000042.txt", root);
        f = fopen(path, "rb");
        if (f) {
            got = fread(raw, 1, sizeof(raw), f);
            fclose(f);
        }
        check("leaving it did not overwrite it", got == 10);
    }

    /* ---- 12. a long note scrolls rather than overflowing ---------------- */

    tap_obj(find_labelled(app_body, "New note"));
    field = find_field(app_body);
    {
        int i;

        for (i = 0; i < 40; i++) {
            type_text("line");
            tap_key("ENTER");
        }
    }
    check("a long note is all there",
          strlen(lv_textarea_get_text(field)) == 40 * 5);
    lv_obj_update_layout(field);
    check("the editor scrolls rather than growing past the body",
          lv_obj_get_height(field) <= PANEL_H - STATUS_H - POS_KB_H);
    check("and the caret is still in view: the field scrolled",
          lv_obj_get_scroll_y(field) > 0);
    check("the body itself did not become scrollable",
          lv_obj_get_scroll_bottom(app_body) <= 0);
    tap_obj(find_labelled(app_body, "Done"));

    app_stop();
    wipe();
    printf("notes_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
